#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "store/segment_builder.h"

#include "stelog_json.h"
#include "adapters/adapter.h"
#include "base/arena.h"
#include "base/hash.h"
#include "base/simd.h"
#include "base/thread_pool.h"
#include "base/time_iso8601.h"
#include "store/format.h"
#include "store/roaring.h"
#include "store/segment_reader.h"
#include "store/swisstable.h"
#include "store/tokenizer.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zstd.h>

#define STELOG_SIMD_PAD 64u
#define STELOG_DEFAULT_CHUNK_BYTES (8u * 1024u * 1024u)
#define STELOG_MAX_VALID_LINE_BYTES (64u * 1024u * 1024u)
#define STELOG_NUM_PARTITIONS 64u
#define STELOG_DEFAULT_DELTA_THRESHOLD 16384u

/* ============================================================================
 * Filesystem Utilities
 * ============================================================================ */

static int mkdir_p(const char *path) {
    char tmp[1536];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

void stelog_rmdir_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *ent;
    char child[2048];
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            stelog_rmdir_recursive(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(path);
}

static int write_all_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += (size_t)n;
    }
    return 0;
}

static int write_file_atomic(const char *path, const void *buf, size_t len) {
    char tmp[1600];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = write_all_fd(fd, buf, len);
    close(fd);
    if (rc != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int64_t get_stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000ll + (int64_t)st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000ll + (int64_t)st->st_mtim.tv_nsec;
#endif
}

static int64_t wall_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000ll + (int64_t)(ts.tv_nsec / 1000000ll);
}

/* ============================================================================
 * Byte Buffer & String Interner
 * ============================================================================ */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} ByteBuf;

static void bbuf_free(ByteBuf *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static bool bbuf_reserve(ByteBuf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 4096u;
    while (ncap < b->len + extra) ncap *= 2u;
    uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
    if (!nd) return false;
    b->data = nd;
    b->cap = ncap;
    return true;
}

static bool bbuf_append(ByteBuf *b, const void *src, size_t n) {
    if (n == 0) return true;
    if (!bbuf_reserve(b, n)) return false;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}

static bool bbuf_pad_to(ByteBuf *b, size_t align) {
    size_t rem = b->len & (align - 1u);
    if (rem == 0) return true;
    size_t pad = align - rem;
    if (!bbuf_reserve(b, pad)) return false;
    memset(b->data + b->len, 0, pad);
    b->len += pad;
    return true;
}

/* Generic string -> uint32_t ID interner backed by a contiguous strpool ByteBuf */
typedef struct {
    uint64_t hash;
    uint32_t id;
    uint32_t occupied;
} InternSlot;

typedef struct {
    InternSlot *slots;
    uint32_t cap;
    uint32_t count;
    StelogStrRef *refs;
    uint32_t refs_cap;
    ByteBuf *pool;
} StrInterner;

static void interner_init(StrInterner *in, ByteBuf *pool, uint32_t initial_cap) {
    uint32_t cap = 64;
    while (cap < initial_cap) cap <<= 1;
    in->slots = (InternSlot *)calloc(cap, sizeof(InternSlot));
    in->cap = cap;
    in->count = 0;
    in->refs_cap = 64;
    in->refs = (StelogStrRef *)malloc(in->refs_cap * sizeof(StelogStrRef));
    in->pool = pool;
}

static void interner_destroy(StrInterner *in) {
    free(in->slots);
    free(in->refs);
    memset(in, 0, sizeof(*in));
}

static void interner_grow(StrInterner *in) {
    uint32_t ncap = in->cap * 2;
    InternSlot *ns = (InternSlot *)calloc(ncap, sizeof(InternSlot));
    uint32_t mask = ncap - 1;
    for (uint32_t i = 0; i < in->cap; ++i) {
        if (!in->slots[i].occupied) continue;
        uint32_t idx = (uint32_t)in->slots[i].hash & mask;
        while (ns[idx].occupied) idx = (idx + 1) & mask;
        ns[idx] = in->slots[i];
    }
    free(in->slots);
    in->slots = ns;
    in->cap = ncap;
}

static StelogStrRef pool_push_str(ByteBuf *pool, const char *s, size_t len) {
    if (!s || len == 0) return (StelogStrRef){ 0, 0 };
    uint32_t off = (uint32_t)pool->len;
    bbuf_append(pool, s, len);
    uint8_t nul = 0;
    bbuf_append(pool, &nul, 1);
    return (StelogStrRef){ off, (uint32_t)len };
}

static uint32_t interner_get_or_add(StrInterner *in, const char *s, size_t len) {
    if (!s) { s = ""; len = 0; }
    if ((in->count + 1) * 10 >= in->cap * 7) interner_grow(in);
    uint64_t h = stelog_wyhash(s, len, 0x243f6a8885a308d3ull);
    uint32_t mask = in->cap - 1;
    uint32_t idx = (uint32_t)h & mask;
    while (in->slots[idx].occupied) {
        if (in->slots[idx].hash == h) {
            StelogStrRef r = in->refs[in->slots[idx].id];
            if (r.len == len && (len == 0 || memcmp(in->pool->data + r.off, s, len) == 0)) {
                return in->slots[idx].id;
            }
        }
        idx = (idx + 1) & mask;
    }
    uint32_t id = in->count++;
    if (id >= in->refs_cap) {
        in->refs_cap *= 2;
        in->refs = (StelogStrRef *)realloc(in->refs, in->refs_cap * sizeof(StelogStrRef));
    }
    in->refs[id] = pool_push_str(in->pool, s, len);
    in->slots[idx].hash = h;
    in->slots[idx].id = id;
    in->slots[idx].occupied = 1;
    return id;
}

/* ============================================================================
 * Raw Event Representation Collected Across Workers & Existing Segments
 * ============================================================================ */

typedef struct {
    int64_t ts_ms;
    uint64_t raw_offset;
    uint64_t doc_key;      /* public doc_id; collisions are resolved when the segment is written */
    uint32_t raw_len;
    uint32_t src_file_idx; /* index into build file table */
    uint32_t seq;          /* arrival order, keeps records of one JSONL line in emission order */
    StelogTag tag;
    uint8_t _pad[3];

    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogStr actor;
    StelogStr git_branch;
    StelogStr msg_id;
    StelogStr parent_id;
    StelogStr text;
} BuildDoc;

typedef struct {
    BuildDoc *items;
    uint32_t count;
    uint32_t cap;
} BuildDocVec;

static void docvec_push(BuildDocVec *v, const BuildDoc *d) {
    if (v->count == v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2 : 1024;
        BuildDoc *ni = (BuildDoc *)realloc(v->items, ncap * sizeof(BuildDoc));
        if (!ni) return;
        v->items = ni;
        v->cap = ncap;
    }
    v->items[v->count++] = *d;
}

/* ============================================================================
 * Manifest Load / Save
 * ============================================================================ */

typedef struct {
    char *path;
    char *session_id;
    uint64_t path_hash;
    uint64_t inode;
    int64_t  mtime_ns;
    uint64_t file_size;
    uint64_t indexed_offset;
    uint8_t  agent_kind;
    uint8_t  is_zstd;
    uint8_t  in_base;
} ManifestItem;

typedef struct {
    ManifestItem *items;
    uint32_t count;
    uint32_t cap;
    StelogArena arena;
} ManifestTable;

static void manifest_init(ManifestTable *mt) {
    memset(mt, 0, sizeof(*mt));
    stelog_arena_init(&mt->arena, 64u * 1024u);
}

static void manifest_destroy(ManifestTable *mt) {
    free(mt->items);
    stelog_arena_destroy(&mt->arena);
    memset(mt, 0, sizeof(*mt));
}

static void manifest_push(ManifestTable *mt, const ManifestItem *it) {
    if (mt->count == mt->cap) {
        uint32_t ncap = mt->cap ? mt->cap * 2 : 128;
        mt->items = (ManifestItem *)realloc(mt->items, ncap * sizeof(ManifestItem));
        mt->cap = ncap;
    }
    ManifestItem copy = *it;
    copy.path = stelog_arena_strdup(&mt->arena, it->path ? it->path : "");
    copy.session_id = stelog_arena_strdup(&mt->arena, it->session_id ? it->session_id : "");
    mt->items[mt->count++] = copy;
}

static const ManifestItem *manifest_find_path(const ManifestTable *mt, const char *path) {
    size_t len = strlen(path);
    uint64_t h = stelog_wyhash(path, len, 0x123456789abcdef0ull);
    for (uint32_t i = 0; i < mt->count; ++i) {
        if (mt->items[i].path_hash == h && strcmp(mt->items[i].path, path) == 0) {
            return &mt->items[i];
        }
    }
    return NULL;
}

static const ManifestItem *manifest_find_sid(const ManifestTable *mt, uint8_t agent, const char *sid) {
    if (!sid || !sid[0]) return NULL;
    for (uint32_t i = 0; i < mt->count; ++i) {
        if (mt->items[i].agent_kind == agent && mt->items[i].session_id &&
            strcmp(mt->items[i].session_id, sid) == 0) {
            return &mt->items[i];
        }
    }
    return NULL;
}

static int manifest_load(const char *path, ManifestTable *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(StelogManifestHeader)) {
        close(fd);
        return -1;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) { close(fd); return -1; }
    ssize_t got = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (got < (ssize_t)sizeof(StelogManifestHeader)) { free(buf); return -1; }

    const StelogManifestHeader *h = (const StelogManifestHeader *)buf;
    if (h->magic != STELOG_MANIFEST_MAGIC || h->format_version != STELOG_FORMAT_VERSION) {
        free(buf);
        return -1;
    }
    if (h->entries_off + (uint64_t)h->entry_count * sizeof(StelogManifestEntry) > (uint64_t)got ||
        h->strpool_off + h->strpool_len > (uint64_t)got) {
        free(buf);
        return -1;
    }

    const StelogManifestEntry *entries = (const StelogManifestEntry *)(buf + h->entries_off);
    const char *pool = (const char *)(buf + h->strpool_off);
    for (uint32_t i = 0; i < h->entry_count; ++i) {
        ManifestItem it;
        memset(&it, 0, sizeof(it));
        it.path_hash      = entries[i].path_hash;
        it.inode          = entries[i].inode;
        it.mtime_ns       = entries[i].mtime_ns;
        it.file_size      = entries[i].file_size;
        it.indexed_offset = entries[i].indexed_offset;
        it.agent_kind     = entries[i].agent_kind;
        it.is_zstd        = entries[i].is_zstd;
        it.in_base        = entries[i].in_base;
        it.path           = (char *)(pool + entries[i].path_off);
        it.session_id     = (char *)(pool + entries[i].sid_off);
        manifest_push(out, &it);
    }
    free(buf);
    return 0;
}

static int manifest_save(const char *path, const ManifestTable *mt) {
    ByteBuf pool = { 0 };
    StelogManifestEntry *entries = (StelogManifestEntry *)calloc(mt->count ? mt->count : 1u, sizeof(StelogManifestEntry));
    for (uint32_t i = 0; i < mt->count; ++i) {
        StelogStrRef pr = pool_push_str(&pool, mt->items[i].path, strlen(mt->items[i].path));
        StelogStrRef sr = pool_push_str(&pool, mt->items[i].session_id, strlen(mt->items[i].session_id));
        entries[i].path_hash      = mt->items[i].path_hash;
        entries[i].inode          = mt->items[i].inode;
        entries[i].mtime_ns       = mt->items[i].mtime_ns;
        entries[i].file_size      = mt->items[i].file_size;
        entries[i].indexed_offset = mt->items[i].indexed_offset;
        entries[i].path_off       = pr.off;
        entries[i].path_len       = pr.len;
        entries[i].sid_off        = sr.off;
        entries[i].sid_len        = sr.len;
        entries[i].agent_kind     = mt->items[i].agent_kind;
        entries[i].is_zstd        = mt->items[i].is_zstd;
        entries[i].in_base        = mt->items[i].in_base;
    }

    ByteBuf out = { 0 };
    StelogManifestHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = STELOG_MANIFEST_MAGIC;
    h.format_version = STELOG_FORMAT_VERSION;
    h.entry_count = mt->count;
    h.entries_off = stelog_align8(sizeof(StelogManifestHeader));
    h.strpool_off = stelog_align8(h.entries_off + (uint64_t)mt->count * sizeof(StelogManifestEntry));
    h.strpool_len = pool.len;

    bbuf_append(&out, &h, sizeof(h));
    bbuf_pad_to(&out, 8);
    if (mt->count > 0) bbuf_append(&out, entries, (size_t)mt->count * sizeof(StelogManifestEntry));
    bbuf_pad_to(&out, 8);
    if (pool.len > 0) bbuf_append(&out, pool.data, pool.len);

    int rc = write_file_atomic(path, out.data, out.len);
    bbuf_free(&out);
    bbuf_free(&pool);
    free(entries);
    return rc;
}

/* ============================================================================
 * Stage 2: Parallel Chunked Ingest Collecting BuildDocs (Lock-Free per Worker)
 * ============================================================================ */

typedef struct {
    uint32_t file_idx;
    uint64_t chunk_start;
    uint64_t chunk_end;
    bool is_zstd;
} BuildChunkTask;

typedef struct {
    _Alignas(64) BuildDocVec docs;
    StelogArena arena;
} WorkerDocOut;

typedef struct {
    const StelogFileList *files;
    const StelogMetaMap *meta;
    const BuildChunkTask *tasks;
    uint32_t task_count;
    StelogParseOptions opts;
    _Atomic uint32_t next_task;
    WorkerDocOut *outs;
} ParallelIngestCtx;

static bool ensure_c_buf(char **buf, size_t *cap, size_t needed) {
    if (*cap >= needed) return true;
    size_t ncap = *cap ? *cap : (64u * 1024u);
    while (ncap < needed) ncap *= 2;
    char *nb = (char *)realloc(*buf, ncap);
    if (!nb) return false;
    *buf = nb;
    *cap = ncap;
    return true;
}

static ssize_t pread_exact(int fd, void *buf, size_t count, uint64_t offset) {
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        ssize_t n = pread(fd, p + total, count - total, (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static bool skip_line_prefilter(StelogAgentKind agent, const char *line, size_t len, const StelogParseOptions *opts) {
    switch (agent) {
        case STELOG_AGENT_CODEX: return stelog_codex_should_skip_line(line, len, opts->include_tool_results, opts->include_thinking);
        case STELOG_AGENT_AGY:   return stelog_agy_should_skip_line(line, len, opts->include_tool_results, opts->include_system);
        case STELOG_AGENT_PI:    return stelog_pi_should_skip_line(line, len, opts->include_tool_results);
        case STELOG_AGENT_DROID: return stelog_droid_should_skip_line(line, len, opts->include_tool_results);
        default:                 return true;
    }
}

static void init_chunk_ctx(const StelogFileEntry *f, const StelogMetaMap *meta, StelogArena *arena,
                           StelogStreamContext *ctx, const StelogSessionMeta **out_sm) {
    memset(ctx, 0, sizeof(*ctx));
    if (f->is_archived) ctx->session_flags |= STELOG_FLAG_ARCHIVED;
    if (f->session_id_hint) {
        ctx->session_id.ptr = f->session_id_hint;
        ctx->session_id.len = (uint32_t)strlen(f->session_id_hint);
    }
    if (f->cwd_hint) {
        ctx->cwd.ptr = f->cwd_hint;
        ctx->cwd.len = (uint32_t)strlen(f->cwd_hint);
    }
    const StelogSessionMeta *sm = (meta && f->session_id_hint)
                                      ? stelog_metamap_get(meta, f->session_id_hint, strlen(f->session_id_hint))
                                      : NULL;
    if (out_sm) *out_sm = sm;
    if (!sm) return;

    if (sm->cwd) { ctx->cwd.ptr = sm->cwd; ctx->cwd.len = (uint32_t)strlen(sm->cwd); }
    if (sm->parent_id) { ctx->parent_session_id.ptr = sm->parent_id; ctx->parent_session_id.len = (uint32_t)strlen(sm->parent_id); }
    if (sm->is_subagent) ctx->session_flags |= STELOG_FLAG_IS_SUBAGENT;
    if (sm->is_archived) ctx->session_flags |= STELOG_FLAG_ARCHIVED;

    char abuf[256];
    if (f->agent == STELOG_AGENT_CODEX) {
        if (sm->agent_name && sm->agent_name[0]) {
            int n = (sm->agent_role && sm->agent_role[0])
                        ? snprintf(abuf, sizeof(abuf), "codex:%s(%s)", sm->agent_name, sm->agent_role)
                        : snprintf(abuf, sizeof(abuf), "codex:%s", sm->agent_name);
            if (n > 0) { ctx->actor.ptr = stelog_arena_strndup(arena, abuf, (size_t)n); ctx->actor.len = (uint32_t)n; }
        } else if (sm->model && sm->model[0]) {
            int n = snprintf(abuf, sizeof(abuf), "codex:%s", sm->model);
            if (n > 0) { ctx->actor.ptr = stelog_arena_strndup(arena, abuf, (size_t)n); ctx->actor.len = (uint32_t)n; }
        }
    } else if (f->agent == STELOG_AGENT_AGY && sm->agent_name && sm->agent_name[0]) {
        int n = snprintf(abuf, sizeof(abuf), "agy:%s", sm->agent_name);
        if (n > 0) { ctx->actor.ptr = stelog_arena_strndup(arena, abuf, (size_t)n); ctx->actor.len = (uint32_t)n; }
    }
}

static uint64_t compute_doc_key(StelogStr session_id, const char *path, uint64_t raw_offset,
                                StelogRoleKind role, uint32_t role_ordinal) {
    /* The file name (not the full path) keeps keys stable when transcripts are archived or moved, and
       a ".zst" suffix is dropped because raw offsets always refer to the decompressed stream. */
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : (path ? path : "");
    size_t blen = strlen(base);
    if (blen > 4 && memcmp(base + blen - 4, ".zst", 4) == 0) blen -= 4;
    uint64_t h = stelog_wyhash(session_id.ptr ? session_id.ptr : "", session_id.len, 0x5354454C4B455931ull);
    h = stelog_wyhash(base, blen, h);
    uint64_t tail[2] = { raw_offset, ((uint64_t)role << 32) | role_ordinal };
    h = stelog_wyhash(tail, sizeof(tail), h) & STELOG_DOC_KEY_MASK;
    return h ? h : 1u;
}

static void emit_parsed_line(const ParallelIngestCtx *P,
                             StelogJsonParser *jp,
                             WorkerDocOut *Wout,
                             const StelogFileEntry *f,
                             const StelogSessionMeta *sm,
                             const char *line,
                             size_t line_len,
                             size_t padded_cap,
                             uint64_t raw_offset,
                             StelogStreamContext *ctx) {
    while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) line_len--;
    if (line_len == 0) return;
    if (skip_line_prefilter(f->agent, line, line_len, &P->opts)) return;

    StelogLineResult res;
    int rc = 0;
    if (f->agent == STELOG_AGENT_CODEX) {
        rc = stelog_json_parse_codex_line(jp, line, line_len, padded_cap, f->file_id, raw_offset, &P->opts, ctx, &Wout->arena, &res);
    } else if (f->agent == STELOG_AGENT_AGY) {
        rc = stelog_json_parse_agy_line(jp, line, line_len, padded_cap, f->file_id, raw_offset, &P->opts, ctx, &Wout->arena, &res);
    } else if (f->agent == STELOG_AGENT_DROID) {
        rc = stelog_json_parse_droid_line(jp, line, line_len, padded_cap, f->file_id, raw_offset, &P->opts, ctx, &Wout->arena, &res);
    } else {
        rc = stelog_json_parse_pi_line(jp, line, line_len, padded_cap, f->file_id, raw_offset, &P->opts, ctx, &Wout->arena, &res);
    }
    if (rc != 0 || res.count == 0) return;

    StelogStr title = { "", 0 };
    StelogStr branch = { "", 0 };
    if (sm && sm->title) { title.ptr = sm->title; title.len = (uint32_t)strlen(sm->title); }
    else if (ctx->title.len) title = ctx->title;
    if (sm && sm->git_branch) { branch.ptr = sm->git_branch; branch.len = (uint32_t)strlen(sm->git_branch); }

    for (uint32_t i = 0; i < res.count; ++i) {
        const StelogEvent *ev = &res.events[i];
        /* Ordinal among same-role records of this line, so enabling tool results/thinking for a rebuild
           does not shift the keys of the other records. */
        StelogRoleKind role = stelog_tag_role(ev->tag);
        uint32_t role_ord = 0;
        for (uint32_t j = 0; j < i; ++j) {
            if (stelog_tag_role(res.events[j].tag) == role) role_ord++;
        }
        BuildDoc bd;
        memset(&bd, 0, sizeof(bd));
        bd.doc_key      = compute_doc_key(ev->session_id, f->path, ev->raw_offset, role, role_ord);
        bd.ts_ms        = ev->ts_ms;
        bd.raw_offset   = ev->raw_offset;
        bd.raw_len      = ev->raw_len;
        bd.src_file_idx = f->file_id;
        bd.tag          = ev->tag;
        bd.session_id   = ev->session_id;
        bd.title        = title;
        bd.project      = ev->project;
        bd.actor        = ev->actor;
        bd.git_branch   = branch;
        bd.msg_id       = ev->msg_id;
        bd.parent_id    = ev->parent_id;
        bd.text         = ev->text;
        docvec_push(&Wout->docs, &bd);
    }
}

static void ingest_worker_fn(uint32_t wid, uint32_t nworkers, void *ud) {
    (void)nworkers;
    ParallelIngestCtx *P = (ParallelIngestCtx *)ud;
    WorkerDocOut *Wout = &P->outs[wid];
    StelogJsonParser *jp = stelog_json_parser_create();

    char *read_buf = NULL; size_t read_cap = 0;
    char *over_buf = NULL; size_t over_cap = 0;
    ZSTD_DStream *zds = NULL;
    int cached_fd = -1;
    uint32_t cached_fidx = UINT32_MAX;

    for (;;) {
        uint32_t tidx = atomic_fetch_add_explicit(&P->next_task, 1, memory_order_relaxed);
        if (tidx >= P->task_count) break;
        const BuildChunkTask *t = &P->tasks[tidx];
        const StelogFileEntry *f = &P->files->items[t->file_idx];

        StelogStreamContext ctx;
        const StelogSessionMeta *sm = NULL;
        init_chunk_ctx(f, P->meta, &Wout->arena, &ctx, &sm);

        if (t->is_zstd) {
            int fd = open(f->path, O_RDONLY);
            if (fd < 0) continue;
            if (!zds) zds = ZSTD_createDStream();
            ZSTD_initDStream(zds);
            size_t in_cap = ZSTD_DStreamInSize();
            size_t out_cap = ZSTD_DStreamOutSize();
            uint8_t *ib = (uint8_t *)malloc(in_cap);
            uint8_t *ob = (uint8_t *)malloc(out_cap);
            size_t acc = 0;
            uint64_t doff = 0;
            bool skipping = false;

            for (;;) {
                ssize_t n = read(fd, ib, in_cap);
                if (n <= 0) break;
                ZSTD_inBuffer zin = { ib, (size_t)n, 0 };
                while (zin.pos < zin.size) {
                    ZSTD_outBuffer zout = { ob, out_cap, 0 };
                    if (ZSTD_isError(ZSTD_decompressStream(zds, &zout, &zin))) goto zstd_done;
                    size_t p = 0;
                    while (p < zout.pos) {
                        const char *nl = (const char *)memchr(ob + p, '\n', zout.pos - p);
                        if (skipping) {
                            size_t seg = nl ? (size_t)(nl - (const char *)(ob + p)) + 1 : (zout.pos - p);
                            doff += seg; p += seg;
                            if (nl) skipping = false;
                            continue;
                        }
                        if (nl && acc == 0) {
                            size_t ll = (size_t)(nl - (const char *)(ob + p));
                            emit_parsed_line(P, jp, Wout, f, sm, (const char *)(ob + p), ll, zout.pos - p, doff, &ctx);
                            doff += ll + 1; p += ll + 1;
                        } else {
                            size_t seg = nl ? (size_t)(nl - (const char *)(ob + p)) : (zout.pos - p);
                            if (acc + seg > STELOG_MAX_VALID_LINE_BYTES) {
                                acc = 0; skipping = (nl == NULL);
                                doff += seg + (nl ? 1u : 0u); p += seg + (nl ? 1u : 0u);
                                continue;
                            }
                            if (!ensure_c_buf(&over_buf, &over_cap, acc + seg + STELOG_SIMD_PAD)) goto zstd_done;
                            memcpy(over_buf + acc, ob + p, seg);
                            acc += seg;
                            if (!nl && acc >= 256 && skip_line_prefilter(f->agent, over_buf, acc, &P->opts)) {
                                acc = 0; skipping = true; doff += seg; p += seg; continue;
                            }
                            if (nl) {
                                memset(over_buf + acc, ' ', STELOG_SIMD_PAD);
                                emit_parsed_line(P, jp, Wout, f, sm, over_buf, acc, acc + STELOG_SIMD_PAD, doff - (acc - seg), &ctx);
                                acc = 0; doff += seg + 1; p += seg + 1;
                            } else {
                                doff += seg; p += seg;
                            }
                        }
                    }
                }
            }
            if (acc > 0 && !skipping) {
                memset(over_buf + acc, ' ', STELOG_SIMD_PAD);
                emit_parsed_line(P, jp, Wout, f, sm, over_buf, acc, acc + STELOG_SIMD_PAD, doff - acc, &ctx);
            }
zstd_done:
            free(ib); free(ob); close(fd);
            continue;
        }

        /* Uncompressed JSONL byte-range chunk via pread() */
        if (cached_fidx != t->file_idx || cached_fd < 0) {
            if (cached_fd >= 0) close(cached_fd);
            cached_fd = open(f->path, O_RDONLY);
            cached_fidx = t->file_idx;
            if (cached_fd < 0) continue;
        }

        uint64_t rstart = (t->chunk_start > 0) ? (t->chunk_start - 1) : 0;
        size_t rlen = (size_t)(t->chunk_end - rstart);
        if (!ensure_c_buf(&read_buf, &read_cap, rlen + STELOG_SIMD_PAD)) continue;
        ssize_t got = pread_exact(cached_fd, read_buf, rlen, rstart);
        if (got <= 0) continue;
        rlen = (size_t)got;
        memset(read_buf + rlen, ' ', STELOG_SIMD_PAD);

        size_t pos = 0;
        if (t->chunk_start > 0) {
            const char *nl = (const char *)memchr(read_buf, '\n', rlen);
            if (!nl) continue;
            pos = (size_t)(nl - read_buf) + 1;
        }

        while (pos < rlen) {
            uint64_t loff = rstart + pos;
            const char *nl = (const char *)memchr(read_buf + pos, '\n', rlen - pos);
            if (nl) {
                size_t ll = (size_t)(nl - (read_buf + pos));
                emit_parsed_line(P, jp, Wout, f, sm, read_buf + pos, ll, (rlen + STELOG_SIMD_PAD) - pos, loff, &ctx);
                pos = (size_t)(nl - read_buf) + 1;
            } else {
                size_t plen = rlen - pos;
                if (t->chunk_end >= f->file_size) {
                    emit_parsed_line(P, jp, Wout, f, sm, read_buf + pos, plen, (rlen + STELOG_SIMD_PAD) - pos, loff, &ctx);
                    break;
                }
                char peek[320];
                const char *probe = read_buf + pos;
                size_t probe_len = plen;
                if (plen < 256) {
                    memcpy(peek, read_buf + pos, plen);
                    ssize_t ex = pread_exact(cached_fd, peek + plen, 256 - plen, t->chunk_end);
                    if (ex > 0) probe_len = plen + (size_t)ex;
                    probe = peek;
                }
                if (skip_line_prefilter(f->agent, probe, probe_len, &P->opts)) break;

                if (!ensure_c_buf(&over_buf, &over_cap, plen + 65536u + STELOG_SIMD_PAD)) break;
                memcpy(over_buf, read_buf + pos, plen);
                size_t acc = plen;
                uint64_t next_off = t->chunk_end;
                while (next_off < f->file_size && acc < STELOG_MAX_VALID_LINE_BYTES) {
                    if (!ensure_c_buf(&over_buf, &over_cap, acc + 65536u + STELOG_SIMD_PAD)) break;
                    ssize_t n = pread(cached_fd, over_buf + acc, 65536u, (off_t)next_off);
                    if (n <= 0) break;
                    const char *onl = (const char *)memchr(over_buf + acc, '\n', (size_t)n);
                    if (onl) { acc = (size_t)(onl - over_buf); break; }
                    acc += (size_t)n;
                    next_off += (uint64_t)n;
                }
                memset(over_buf + acc, ' ', STELOG_SIMD_PAD);
                emit_parsed_line(P, jp, Wout, f, sm, over_buf, acc, acc + STELOG_SIMD_PAD, loff, &ctx);
                break;
            }
        }
    }

    if (cached_fd >= 0) close(cached_fd);
    if (zds) ZSTD_freeDStream(zds);
    free(read_buf);
    free(over_buf);
    stelog_json_parser_destroy(jp);
}

/* ============================================================================
 * Stage 4: 64-Partition Parallel Inverted Index Build
 * ============================================================================ */

typedef struct {
    uint64_t term_hash;
    uint32_t doc_id;
} PostingPair;

typedef struct {
    PostingPair *items;
    uint32_t count;
    uint32_t cap;
} PairVec;

static inline void pairvec_push(PairVec *v, uint64_t h, uint32_t doc_id) {
    if (v->count == v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2 : 256;
        v->items = (PostingPair *)realloc(v->items, ncap * sizeof(PostingPair));
        v->cap = ncap;
    }
    v->items[v->count].term_hash = h;
    v->items[v->count].doc_id = doc_id;
    v->count++;
}

typedef struct {
    _Alignas(64) PairVec buckets[STELOG_NUM_PARTITIONS];
} WorkerBuckets;

typedef struct {
    const char *text_base;
    const uint64_t *col_text_off;
    const uint32_t *col_text_len;
    uint32_t doc_count;
    WorkerBuckets *wb;
} TokenizeCtx;

typedef struct {
    WorkerBuckets *wb;
    uint32_t doc_id;
} TokEmitArg;

static void scatter_token_cb(uint64_t h, void *ctx) {
    TokEmitArg *arg = (TokEmitArg *)ctx;
    uint32_t p = (uint32_t)(h >> 58); /* top 6 bits -> [0..63] */
    pairvec_push(&arg->wb->buckets[p], h, arg->doc_id);
}

static void tokenize_worker_fn(uint32_t wid, uint32_t nworkers, void *ud) {
    TokenizeCtx *T = (TokenizeCtx *)ud;
    uint32_t per = (T->doc_count + nworkers - 1u) / nworkers;
    uint32_t start = wid * per;
    uint32_t end = start + per;
    if (end > T->doc_count) end = T->doc_count;

    TokEmitArg arg = { &T->wb[wid], 0 };
    for (uint32_t d = start; d < end; ++d) {
        arg.doc_id = d;
        uint32_t len = T->col_text_len[d];
        if (len > 0) {
            stelog_tokenize_text(T->text_base + T->col_text_off[d], len, scatter_token_cb, &arg);
        }
    }
}

static int cmp_posting_pair(const void *a, const void *b) {
    const PostingPair *pa = (const PostingPair *)a;
    const PostingPair *pb = (const PostingPair *)b;
    if (pa->term_hash < pb->term_hash) return -1;
    if (pa->term_hash > pb->term_hash) return 1;
    if (pa->doc_id < pb->doc_id) return -1;
    if (pa->doc_id > pb->doc_id) return 1;
    return 0;
}

typedef struct {
    StelogTermSlot *terms;
    uint32_t term_count;
    ByteBuf postings;
} PartitionOut;

typedef struct {
    WorkerBuckets *wb;
    uint32_t nworkers;
    PartitionOut parts[STELOG_NUM_PARTITIONS];
    _Atomic uint32_t next_part;
} PartitionBuildCtx;

static void partition_worker_fn(uint32_t wid, uint32_t nworkers, void *ud) {
    (void)wid; (void)nworkers;
    PartitionBuildCtx *B = (PartitionBuildCtx *)ud;
    uint32_t *doc_tmp = NULL;
    size_t doc_tmp_cap = 0;

    for (;;) {
        uint32_t p = atomic_fetch_add_explicit(&B->next_part, 1, memory_order_relaxed);
        if (p >= STELOG_NUM_PARTITIONS) break;

        size_t total_pairs = 0;
        for (uint32_t w = 0; w < B->nworkers; ++w) {
            total_pairs += B->wb[w].buckets[p].count;
        }
        if (total_pairs == 0) continue;

        PostingPair *merged = (PostingPair *)malloc(total_pairs * sizeof(PostingPair));
        size_t off = 0;
        for (uint32_t w = 0; w < B->nworkers; ++w) {
            uint32_t c = B->wb[w].buckets[p].count;
            if (c > 0) {
                memcpy(merged + off, B->wb[w].buckets[p].items, c * sizeof(PostingPair));
                off += c;
            }
        }
        qsort(merged, total_pairs, sizeof(PostingPair), cmp_posting_pair);

        PartitionOut *po = &B->parts[p];
        uint32_t terms_cap = 256;
        po->terms = (StelogTermSlot *)malloc(terms_cap * sizeof(StelogTermSlot));
        po->term_count = 0;

        size_t i = 0;
        while (i < total_pairs) {
            uint64_t th = merged[i].term_hash;
            uint32_t df = 0;
            uint32_t last_doc = UINT32_MAX;
            while (i < total_pairs && merged[i].term_hash == th) {
                uint32_t d = merged[i].doc_id;
                if (d != last_doc) {
                    if (df == doc_tmp_cap) {
                        doc_tmp_cap = doc_tmp_cap ? doc_tmp_cap * 2 : 1024;
                        doc_tmp = (uint32_t *)realloc(doc_tmp, doc_tmp_cap * sizeof(uint32_t));
                    }
                    doc_tmp[df++] = d;
                    last_doc = d;
                }
                i++;
            }

            if (po->term_count == terms_cap) {
                terms_cap *= 2;
                po->terms = (StelogTermSlot *)realloc(po->terms, terms_cap * sizeof(StelogTermSlot));
            }
            StelogTermSlot slot;
            slot.term_hash = th;
            slot.doc_freq = df;

            if (df == 1u) {
                /* Inline directly in SwissTable slot: 0 extra bytes, 0 extra IO! */
                slot.payload_or_off = doc_tmp[0];
            } else if (df <= STELOG_POSTING_COMPACT_MAX) {
                bbuf_pad_to(&po->postings, 4);
                slot.payload_or_off = (uint32_t)(po->postings.len / 4u);
                bbuf_append(&po->postings, doc_tmp, df * sizeof(uint32_t));
            } else {
                bbuf_pad_to(&po->postings, 8);
                slot.payload_or_off = (uint32_t)(po->postings.len / 4u);
                uint8_t *rb = NULL;
                size_t rlen = 0;
                if (stelog_roaring_encode(doc_tmp, df, &rb, &rlen) == 0) {
                    bbuf_append(&po->postings, rb, rlen);
                    free(rb);
                }
            }
            po->terms[po->term_count++] = slot;
        }

        free(merged);
    }

    free(doc_tmp);
}

/* ============================================================================
 * Segment Writer (Writes one complete Base or Delta segment atomically)
 * ============================================================================ */

static int cmp_build_doc_ptr(const void *a, const void *b) {
    const BuildDoc *da = *(const BuildDoc *const *)a;
    const BuildDoc *db = *(const BuildDoc *const *)b;
    if (da->ts_ms < db->ts_ms) return -1;
    if (da->ts_ms > db->ts_ms) return 1;
    if (da->src_file_idx < db->src_file_idx) return -1;
    if (da->src_file_idx > db->src_file_idx) return 1;
    if (da->raw_offset < db->raw_offset) return -1;
    if (da->raw_offset > db->raw_offset) return 1;
    if (da->seq < db->seq) return -1;
    if (da->seq > db->seq) return 1;
    return 0;
}

typedef struct {
    const char *path;
    uint8_t agent_kind;
    uint8_t is_zstd;
    uint8_t is_archived;
} SegSrcFile;

typedef struct {
    uint64_t key;
    uint32_t doc;
} DocKeyPair;

static int cmp_doc_key_pair(const void *a, const void *b) {
    const DocKeyPair *pa = (const DocKeyPair *)a;
    const DocKeyPair *pb = (const DocKeyPair *)b;
    if (pa->key != pb->key) return pa->key < pb->key ? -1 : 1;
    return pa->doc < pb->doc ? -1 : (pa->doc > pb->doc ? 1 : 0);
}

/* Makes keys unique within the segment and fills order[] with doc rows sorted by key. On a hash
   collision the earlier row keeps its key and later rows are re-hashed, so the outcome depends only
   on the segment contents. */
static void assign_unique_doc_keys(uint64_t *keys, uint32_t *order, uint32_t doc_count) {
    if (doc_count == 0) return;
    DocKeyPair *pairs = (DocKeyPair *)malloc((size_t)doc_count * sizeof(DocKeyPair));
    if (!pairs) {
        for (uint32_t d = 0; d < doc_count; ++d) order[d] = d;
        return;
    }
    for (uint32_t d = 0; d < doc_count; ++d) { pairs[d].key = keys[d]; pairs[d].doc = d; }
    for (uint64_t salt = 1;; ++salt) {
        qsort(pairs, doc_count, sizeof(DocKeyPair), cmp_doc_key_pair);
        bool dup = false;
        for (uint32_t i = 1; i < doc_count; ++i) {
            if (pairs[i].key != pairs[i - 1].key) continue;
            uint64_t buf[2] = { pairs[i].key, salt };
            uint64_t k = stelog_wyhash(buf, sizeof(buf), 0x444F434B45595245ull) & STELOG_DOC_KEY_MASK;
            pairs[i].key = k ? k : 1u;
            dup = true;
        }
        if (!dup) break;
    }
    for (uint32_t i = 0; i < doc_count; ++i) {
        keys[pairs[i].doc] = pairs[i].key;
        order[i] = pairs[i].doc;
    }
    free(pairs);
}

static int write_segment_dir(const char *final_seg_dir,
                             const BuildDoc **sorted_docs,
                             uint32_t doc_count,
                             const SegSrcFile *src_files,
                             uint32_t src_file_count,
                             uint32_t nworkers,
                             uint32_t *out_unique_terms,
                             uint64_t *out_disk_bytes) {
    char tmp_dir[1600];
    snprintf(tmp_dir, sizeof(tmp_dir), "%s.staging.%d", final_seg_dir, (int)getpid());
    stelog_rmdir_recursive(tmp_dir);
    if (mkdir_p(tmp_dir) != 0) return -1;

    /* 1. Intern sessions, actors, projects, files */
    ByteBuf strpool = { 0 };
    StrInterner sess_in, actor_in, proj_in;
    interner_init(&sess_in, &strpool, 256);
    interner_init(&actor_in, &strpool, 64);
    interner_init(&proj_in, &strpool, 64);

    /* Ensure empty string / default entries exist */
    StelogSessionRecord *sess_recs = NULL;
    uint32_t sess_recs_cap = 0;

    /* Allocate SoA columns */
    int64_t  *col_ts_ms      = (int64_t  *)calloc(doc_count ? doc_count : 1u, sizeof(int64_t));
    uint8_t  *col_tag        = (uint8_t  *)calloc(doc_count ? doc_count : 1u, sizeof(uint8_t));
    uint32_t *col_project_id = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    uint32_t *col_actor_id   = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    uint32_t *col_session_id = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    uint64_t *col_text_off   = (uint64_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint64_t));
    uint32_t *col_text_len   = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    uint32_t *col_parent_doc = (uint32_t *)malloc((doc_count ? doc_count : 1u) * sizeof(uint32_t));
    uint32_t *col_file_id    = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    uint64_t *col_raw_off    = (uint64_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint64_t));
    uint32_t *col_raw_len    = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    uint64_t *col_doc_key    = (uint64_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint64_t));
    uint32_t *col_key_order  = (uint32_t *)calloc(doc_count ? doc_count : 1u, sizeof(uint32_t));
    memset(col_parent_doc, 0xFF, (doc_count ? doc_count : 1u) * sizeof(uint32_t));

    /* Build text.bin in RAM (contiguous UTF-8 clean text pool) */
    ByteBuf text_pool = { 0 };
    uint64_t total_text_len = 0;

    for (uint32_t d = 0; d < doc_count; ++d) {
        const BuildDoc *bd = sorted_docs[d];
        uint32_t sid = interner_get_or_add(&sess_in, bd->session_id.ptr, bd->session_id.len);
        uint32_t aid = interner_get_or_add(&actor_in, bd->actor.ptr, bd->actor.len);
        uint32_t pid = interner_get_or_add(&proj_in, bd->project.ptr, bd->project.len);

        if (sid >= sess_recs_cap) {
            uint32_t ncap = sess_recs_cap ? sess_recs_cap * 2 : 256;
            while (ncap <= sid) ncap *= 2;
            sess_recs = (StelogSessionRecord *)realloc(sess_recs, ncap * sizeof(StelogSessionRecord));
            memset(sess_recs + sess_recs_cap, 0, (ncap - sess_recs_cap) * sizeof(StelogSessionRecord));
            sess_recs_cap = ncap;
        }
        StelogSessionRecord *sr = &sess_recs[sid];
        if (sr->doc_count == 0) {
            sr->session_id = sess_in.refs[sid];
            sr->first_ts_ms = bd->ts_ms;
            sr->last_ts_ms  = bd->ts_ms;
            sr->agent_kind  = (uint8_t)stelog_tag_agent(bd->tag);
            sr->flags       = stelog_tag_flags(bd->tag);
            sr->project_id  = pid;
            sr->cwd         = proj_in.refs[pid];
        } else {
            if (bd->ts_ms < sr->first_ts_ms) sr->first_ts_ms = bd->ts_ms;
            if (bd->ts_ms > sr->last_ts_ms) sr->last_ts_ms = bd->ts_ms;
            if (sr->cwd.len == 0 && bd->project.len > 0) {
                sr->project_id = pid;
                sr->cwd = proj_in.refs[pid];
            }
        }
        if (sr->title.len == 0 && bd->title.len > 0) {
            sr->title = pool_push_str(&strpool, bd->title.ptr, bd->title.len);
        } else if (sr->title.len == 0 && stelog_tag_role(bd->tag) == STELOG_ROLE_USER && bd->text.len > 0) {
            /* Auto-fallback session title from first real human message (csift list pattern) */
            char tbuf[128];
            size_t ti = 0;
            for (uint32_t k = 0; k < bd->text.len && ti < 110; ++k) {
                unsigned char c = (unsigned char)bd->text.ptr[k];
                if (c == '\n' || c == '\r' || c == '\t') {
                    if (ti > 0 && tbuf[ti - 1] != ' ') tbuf[ti++] = ' ';
                } else {
                    tbuf[ti++] = (char)c;
                }
            }
            /* Trim back to valid UTF-8 codepoint boundary */
            while (ti > 0 && (((uint8_t)tbuf[ti - 1]) & 0xC0u) == 0x80u) ti--;
            if (ti > 0 && (((uint8_t)tbuf[ti - 1]) & 0x80u) != 0) {
                /* Drop potentially incomplete leading multi-byte char at exact cut */
                uint8_t lead = (uint8_t)tbuf[ti - 1];
                if ((lead & 0xE0u) == 0xC0u || (lead & 0xF0u) == 0xE0u || (lead & 0xF8u) == 0xF0u) ti--;
            }
            if (ti > 0) {
                sr->title = pool_push_str(&strpool, tbuf, ti);
            }
        }
        if (sr->git_branch.len == 0 && bd->git_branch.len > 0) {
            sr->git_branch = pool_push_str(&strpool, bd->git_branch.ptr, bd->git_branch.len);
        }
        sr->doc_count++;

        col_ts_ms[d]      = bd->ts_ms;
        col_tag[d]        = bd->tag;
        col_project_id[d] = pid;
        col_actor_id[d]   = aid;
        col_session_id[d] = sid;
        col_text_off[d]   = text_pool.len;
        col_text_len[d]   = bd->text.len;
        col_file_id[d]    = bd->src_file_idx;
        col_raw_off[d]    = bd->raw_offset;
        col_raw_len[d]    = bd->raw_len;
        col_doc_key[d]    = bd->doc_key ? bd->doc_key : 1u;

        if (bd->text.len > 0) {
            bbuf_append(&text_pool, bd->text.ptr, bd->text.len);
            total_text_len += bd->text.len;
        }
        uint8_t nul = 0;
        bbuf_append(&text_pool, &nul, 1);
    }

    assign_unique_doc_keys(col_doc_key, col_key_order, doc_count);

    uint32_t session_count = sess_in.count;
    uint32_t actor_count   = actor_in.count;
    uint32_t project_count = proj_in.count;

    /* 2. Build CSR session_idx.bin (session_id -> sorted doc_ids) */
    uint32_t *sess_offs = (uint32_t *)calloc(session_count + 1u, sizeof(uint32_t));
    uint32_t *sess_cur  = (uint32_t *)calloc(session_count ? session_count : 1u, sizeof(uint32_t));
    uint32_t *sess_docs = (uint32_t *)malloc((doc_count ? doc_count : 1u) * sizeof(uint32_t));
    for (uint32_t s = 0; s < session_count; ++s) {
        sess_offs[s + 1] = sess_offs[s] + sess_recs[s].doc_count;
        sess_cur[s] = sess_offs[s];
    }
    for (uint32_t d = 0; d < doc_count; ++d) {
        uint32_t s = col_session_id[d];
        sess_docs[sess_cur[s]++] = d;
    }
    free(sess_cur);

    /* 3. Write text.bin */
    char fpath[1800];
    uint64_t disk_total = 0;
    snprintf(fpath, sizeof(fpath), "%s/text.bin", tmp_dir);
    write_file_atomic(fpath, text_pool.data ? text_pool.data : (const uint8_t *)"", text_pool.len);
    disk_total += text_pool.len;

    /* 4. Write meta.col (64B-aligned SoA columns) */
    ByteBuf meta_buf = { 0 };
    StelogMetaHeader mh;
    memset(&mh, 0, sizeof(mh));
    mh.magic = STELOG_INDEX_MAGIC;
    mh.format_version = STELOG_FORMAT_VERSION;
    mh.doc_count = doc_count;
    mh.session_count = session_count;
    mh.actor_count = actor_count;
    mh.project_count = project_count;
    mh.file_count = src_file_count;
    mh.avg_text_len = doc_count ? (uint32_t)(total_text_len / doc_count) : 1u;

    bbuf_append(&meta_buf, &mh, sizeof(mh));
    bbuf_pad_to(&meta_buf, 64);

    const void *col_ptrs[STELOG_COL_COUNT] = {
        col_ts_ms, col_tag, col_project_id, col_actor_id, col_session_id,
        col_text_off, col_text_len, col_parent_doc, col_file_id, col_raw_off, col_raw_len,
        col_doc_key, col_key_order
    };
    const size_t col_elem_sz[STELOG_COL_COUNT] = {
        sizeof(int64_t), sizeof(uint8_t), sizeof(uint32_t), sizeof(uint32_t), sizeof(uint32_t),
        sizeof(uint64_t), sizeof(uint32_t), sizeof(uint32_t), sizeof(uint32_t), sizeof(uint64_t), sizeof(uint32_t),
        sizeof(uint64_t), sizeof(uint32_t)
    };
    for (int c = 0; c < STELOG_COL_COUNT; ++c) {
        bbuf_pad_to(&meta_buf, 64);
        mh.col_offsets[c] = meta_buf.len;
        if (doc_count > 0) bbuf_append(&meta_buf, col_ptrs[c], (size_t)doc_count * col_elem_sz[c]);
    }
    memcpy(meta_buf.data, &mh, sizeof(mh));
    snprintf(fpath, sizeof(fpath), "%s/meta.col", tmp_dir);
    write_file_atomic(fpath, meta_buf.data, meta_buf.len);
    disk_total += meta_buf.len;
    bbuf_free(&meta_buf);

    /* 5. Write dict.bin */
    StelogFileRecord *file_recs = (StelogFileRecord *)calloc(src_file_count ? src_file_count : 1u, sizeof(StelogFileRecord));
    for (uint32_t i = 0; i < src_file_count; ++i) {
        file_recs[i].path        = pool_push_str(&strpool, src_files[i].path, strlen(src_files[i].path));
        file_recs[i].agent_kind  = src_files[i].agent_kind;
        file_recs[i].is_zstd     = src_files[i].is_zstd;
        file_recs[i].is_archived = src_files[i].is_archived;
    }

    ByteBuf dict_buf = { 0 };
    StelogDictHeader dh;
    memset(&dh, 0, sizeof(dh));
    dh.magic = STELOG_INDEX_MAGIC;
    dh.format_version = STELOG_FORMAT_VERSION;
    dh.session_count = session_count;
    dh.actor_count   = actor_count;
    dh.project_count = project_count;
    dh.file_count    = src_file_count;
    bbuf_append(&dict_buf, &dh, sizeof(dh));

    bbuf_pad_to(&dict_buf, 8);
    dh.sessions_off = dict_buf.len;
    if (session_count > 0) bbuf_append(&dict_buf, sess_recs, session_count * sizeof(StelogSessionRecord));

    bbuf_pad_to(&dict_buf, 8);
    dh.actors_off = dict_buf.len;
    if (actor_count > 0) bbuf_append(&dict_buf, actor_in.refs, actor_count * sizeof(StelogStrRef));

    bbuf_pad_to(&dict_buf, 8);
    dh.projects_off = dict_buf.len;
    if (project_count > 0) bbuf_append(&dict_buf, proj_in.refs, project_count * sizeof(StelogStrRef));

    bbuf_pad_to(&dict_buf, 8);
    dh.files_off = dict_buf.len;
    if (src_file_count > 0) bbuf_append(&dict_buf, file_recs, src_file_count * sizeof(StelogFileRecord));

    bbuf_pad_to(&dict_buf, 8);
    dh.strpool_off = dict_buf.len;
    dh.strpool_len = strpool.len;
    if (strpool.len > 0) bbuf_append(&dict_buf, strpool.data, strpool.len);

    memcpy(dict_buf.data, &dh, sizeof(dh));
    snprintf(fpath, sizeof(fpath), "%s/dict.bin", tmp_dir);
    write_file_atomic(fpath, dict_buf.data, dict_buf.len);
    disk_total += dict_buf.len;
    bbuf_free(&dict_buf);
    free(file_recs);

    /* 6. Write session_idx.bin */
    ByteBuf sidx_buf = { 0 };
    StelogSessionIdxHeader sh;
    memset(&sh, 0, sizeof(sh));
    sh.magic = STELOG_INDEX_MAGIC;
    sh.format_version = STELOG_FORMAT_VERSION;
    sh.session_count = session_count;
    sh.doc_count = doc_count;
    bbuf_append(&sidx_buf, &sh, sizeof(sh));
    bbuf_pad_to(&sidx_buf, 8);
    sh.offsets_off = sidx_buf.len;
    bbuf_append(&sidx_buf, sess_offs, (session_count + 1u) * sizeof(uint32_t));
    bbuf_pad_to(&sidx_buf, 8);
    sh.docs_off = sidx_buf.len;
    if (doc_count > 0) bbuf_append(&sidx_buf, sess_docs, doc_count * sizeof(uint32_t));
    memcpy(sidx_buf.data, &sh, sizeof(sh));
    snprintf(fpath, sizeof(fpath), "%s/session_idx.bin", tmp_dir);
    write_file_atomic(fpath, sidx_buf.data, sidx_buf.len);
    disk_total += sidx_buf.len;
    bbuf_free(&sidx_buf);
    free(sess_offs);
    free(sess_docs);

    /* 7. Build dimension bitmaps (bitmaps.bin): agent, role, project */
    uint32_t *agent_cnt = (uint32_t *)calloc(STELOG_AGENT_COUNT, sizeof(uint32_t));
    uint32_t *role_cnt  = (uint32_t *)calloc(STELOG_ROLE_COUNT, sizeof(uint32_t));
    uint32_t *proj_cnt  = (uint32_t *)calloc(project_count ? project_count : 1u, sizeof(uint32_t));
    for (uint32_t d = 0; d < doc_count; ++d) {
        agent_cnt[stelog_tag_agent(col_tag[d])]++;
        role_cnt[stelog_tag_role(col_tag[d])]++;
        proj_cnt[col_project_id[d]]++;
    }
    uint32_t **agent_docs = (uint32_t **)calloc(STELOG_AGENT_COUNT, sizeof(uint32_t *));
    uint32_t **role_docs  = (uint32_t **)calloc(STELOG_ROLE_COUNT, sizeof(uint32_t *));
    uint32_t **proj_docs  = (uint32_t **)calloc(project_count ? project_count : 1u, sizeof(uint32_t *));
    for (uint32_t a = 0; a < STELOG_AGENT_COUNT; ++a) if (agent_cnt[a]) agent_docs[a] = (uint32_t *)malloc(agent_cnt[a] * sizeof(uint32_t));
    for (uint32_t r = 0; r < STELOG_ROLE_COUNT; ++r)  if (role_cnt[r])  role_docs[r]  = (uint32_t *)malloc(role_cnt[r]  * sizeof(uint32_t));
    for (uint32_t p = 0; p < project_count; ++p)      if (proj_cnt[p])  proj_docs[p]  = (uint32_t *)malloc(proj_cnt[p]  * sizeof(uint32_t));

    memset(agent_cnt, 0, STELOG_AGENT_COUNT * sizeof(uint32_t));
    memset(role_cnt, 0, STELOG_ROLE_COUNT * sizeof(uint32_t));
    memset(proj_cnt, 0, (project_count ? project_count : 1u) * sizeof(uint32_t));
    for (uint32_t d = 0; d < doc_count; ++d) {
        uint32_t a = (uint32_t)stelog_tag_agent(col_tag[d]);
        uint32_t r = (uint32_t)stelog_tag_role(col_tag[d]);
        uint32_t p = col_project_id[d];
        agent_docs[a][agent_cnt[a]++] = d;
        role_docs[r][role_cnt[r]++]   = d;
        proj_docs[p][proj_cnt[p]++]   = d;
    }

    ByteBuf bm_buf = { 0 };
    StelogBitmapsHeader bh;
    memset(&bh, 0, sizeof(bh));
    bh.magic = STELOG_INDEX_MAGIC;
    bh.format_version = STELOG_FORMAT_VERSION;
    bh.agent_count   = STELOG_AGENT_COUNT;
    bh.role_count    = STELOG_ROLE_COUNT;
    bh.project_count = project_count;
    bbuf_append(&bm_buf, &bh, sizeof(bh));

    bbuf_pad_to(&bm_buf, 8);
    bh.agent_offs_off = bm_buf.len;
    uint64_t zero = 0;
    for (uint32_t a = 0; a < STELOG_AGENT_COUNT; ++a) bbuf_append(&bm_buf, &zero, sizeof(uint64_t));

    bbuf_pad_to(&bm_buf, 8);
    bh.role_offs_off = bm_buf.len;
    for (uint32_t r = 0; r < STELOG_ROLE_COUNT; ++r) bbuf_append(&bm_buf, &zero, sizeof(uint64_t));

    bbuf_pad_to(&bm_buf, 8);
    bh.project_offs_off = bm_buf.len;
    for (uint32_t p = 0; p < project_count; ++p) bbuf_append(&bm_buf, &zero, sizeof(uint64_t));

    uint64_t *a_offs = (uint64_t *)calloc(STELOG_AGENT_COUNT, sizeof(uint64_t));
    uint64_t *r_offs = (uint64_t *)calloc(STELOG_ROLE_COUNT, sizeof(uint64_t));
    uint64_t *p_offs = (uint64_t *)calloc(project_count ? project_count : 1u, sizeof(uint64_t));

    for (uint32_t a = 0; a < STELOG_AGENT_COUNT; ++a) {
        if (!agent_cnt[a]) continue;
        bbuf_pad_to(&bm_buf, 8);
        a_offs[a] = bm_buf.len;
        uint8_t *rb = NULL; size_t rl = 0;
        if (stelog_roaring_encode(agent_docs[a], agent_cnt[a], &rb, &rl) == 0) { bbuf_append(&bm_buf, rb, rl); free(rb); }
        free(agent_docs[a]);
    }
    for (uint32_t r = 0; r < STELOG_ROLE_COUNT; ++r) {
        if (!role_cnt[r]) continue;
        bbuf_pad_to(&bm_buf, 8);
        r_offs[r] = bm_buf.len;
        uint8_t *rb = NULL; size_t rl = 0;
        if (stelog_roaring_encode(role_docs[r], role_cnt[r], &rb, &rl) == 0) { bbuf_append(&bm_buf, rb, rl); free(rb); }
        free(role_docs[r]);
    }
    for (uint32_t p = 0; p < project_count; ++p) {
        if (!proj_cnt[p]) continue;
        bbuf_pad_to(&bm_buf, 8);
        p_offs[p] = bm_buf.len;
        uint8_t *rb = NULL; size_t rl = 0;
        if (stelog_roaring_encode(proj_docs[p], proj_cnt[p], &rb, &rl) == 0) { bbuf_append(&bm_buf, rb, rl); free(rb); }
        free(proj_docs[p]);
    }
    memcpy(bm_buf.data, &bh, sizeof(bh));
    memcpy(bm_buf.data + bh.agent_offs_off, a_offs, STELOG_AGENT_COUNT * sizeof(uint64_t));
    memcpy(bm_buf.data + bh.role_offs_off, r_offs, STELOG_ROLE_COUNT * sizeof(uint64_t));
    if (project_count > 0) memcpy(bm_buf.data + bh.project_offs_off, p_offs, project_count * sizeof(uint64_t));

    snprintf(fpath, sizeof(fpath), "%s/bitmaps.bin", tmp_dir);
    write_file_atomic(fpath, bm_buf.data, bm_buf.len);
    disk_total += bm_buf.len;
    bbuf_free(&bm_buf);
    free(a_offs); free(r_offs); free(p_offs);
    free(agent_docs); free(role_docs); free(proj_docs);
    free(agent_cnt); free(role_cnt); free(proj_cnt);

    /* 8. 64-Partition Parallel Tokenization + Adaptive Roaring Inverted Index + Static SwissTable */
    uint32_t tok_workers = nworkers;
    if (doc_count < 256) tok_workers = 1;
    WorkerBuckets *wb = (WorkerBuckets *)calloc(tok_workers, sizeof(WorkerBuckets));

    if (doc_count > 0) {
        TokenizeCtx tctx = {
            .text_base = (const char *)text_pool.data,
            .col_text_off = col_text_off,
            .col_text_len = col_text_len,
            .doc_count = doc_count,
            .wb = wb
        };
        stelog_parallel_run(tok_workers, tokenize_worker_fn, &tctx);
    }

    PartitionBuildCtx pbctx;
    memset(&pbctx, 0, sizeof(pbctx));
    pbctx.wb = wb;
    pbctx.nworkers = tok_workers;
    atomic_init(&pbctx.next_part, 0);
    if (doc_count > 0) {
        stelog_parallel_run(tok_workers, partition_worker_fn, &pbctx);
    }

    /* Free worker scatter buckets immediately to keep peak RSS low */
    for (uint32_t w = 0; w < tok_workers; ++w) {
        for (uint32_t p = 0; p < STELOG_NUM_PARTITIONS; ++p) {
            free(wb[w].buckets[p].items);
        }
    }
    free(wb);

    /* Concatenate 64 partition posting buffers + gather all StelogTermSlots */
    uint32_t total_terms = 0;
    size_t total_post_bytes = 8; /* reserve offset 0..7 so valid offset > 0 */
    for (uint32_t p = 0; p < STELOG_NUM_PARTITIONS; ++p) {
        total_terms += pbctx.parts[p].term_count;
        total_post_bytes = (size_t)stelog_align8(total_post_bytes) + pbctx.parts[p].postings.len;
    }

    StelogTermSlot *all_terms = (StelogTermSlot *)malloc((total_terms ? total_terms : 1u) * sizeof(StelogTermSlot));
    ByteBuf all_postings = { 0 };
    bbuf_reserve(&all_postings, total_post_bytes + 64);
    uint64_t zpad = 0;
    bbuf_append(&all_postings, &zpad, 8);

    uint32_t ti = 0;
    for (uint32_t p = 0; p < STELOG_NUM_PARTITIONS; ++p) {
        PartitionOut *po = &pbctx.parts[p];
        if (po->term_count == 0) {
            bbuf_free(&po->postings);
            free(po->terms);
            continue;
        }
        bbuf_pad_to(&all_postings, 8);
        uint32_t base_off_div4 = (uint32_t)(all_postings.len / 4u);
        if (po->postings.len > 0) {
            bbuf_append(&all_postings, po->postings.data, po->postings.len);
        }
        for (uint32_t k = 0; k < po->term_count; ++k) {
            StelogTermSlot s = po->terms[k];
            if (s.doc_freq >= 2u) {
                s.payload_or_off += base_off_div4;
            }
            all_terms[ti++] = s;
        }
        bbuf_free(&po->postings);
        free(po->terms);
    }

    snprintf(fpath, sizeof(fpath), "%s/postings.bin", tmp_dir);
    write_file_atomic(fpath, all_postings.data, all_postings.len);
    disk_total += all_postings.len;
    bbuf_free(&all_postings);

    uint8_t *swiss_buf = NULL;
    size_t swiss_len = 0;
    if (stelog_swiss_build(all_terms, total_terms, &swiss_buf, &swiss_len) == 0) {
        snprintf(fpath, sizeof(fpath), "%s/terms.ht", tmp_dir);
        write_file_atomic(fpath, swiss_buf, swiss_len);
        disk_total += swiss_len;
        free(swiss_buf);
    }
    free(all_terms);

    /* Free temporary SoA & dictionaries */
    free(col_ts_ms); free(col_tag); free(col_project_id); free(col_actor_id);
    free(col_session_id); free(col_text_off); free(col_text_len); free(col_parent_doc);
    free(col_file_id); free(col_raw_off); free(col_raw_len);
    free(col_doc_key); free(col_key_order);
    free(sess_recs);
    bbuf_free(&text_pool);
    bbuf_free(&strpool);
    interner_destroy(&sess_in);
    interner_destroy(&actor_in);
    interner_destroy(&proj_in);

    /* Atomic publish segment directory */
    stelog_rmdir_recursive(final_seg_dir);
    if (rename(tmp_dir, final_seg_dir) != 0) {
        stelog_rmdir_recursive(tmp_dir);
        return -1;
    }

    if (out_unique_terms) *out_unique_terms = total_terms;
    if (out_disk_bytes) *out_disk_bytes = disk_total;
    return 0;
}

/* Import docs from an already-mmapped segment into BuildDocVec (zero JSON re-parsing!) */
static void import_segment_docs(const StelogSegment *seg,
                                const uint8_t *tombstone_bits,
                                uint32_t tombstone_len,
                                SegSrcFile **io_files,
                                uint32_t *io_file_count,
                                uint32_t *io_file_cap,
                                BuildDocVec *out_docs) {
    if (!seg || !seg->loaded || seg->doc_count == 0) return;

    /* Map old segment file_ids -> new combined file table indices */
    uint32_t *fmap = (uint32_t *)malloc((seg->file_count ? seg->file_count : 1u) * sizeof(uint32_t));
    for (uint32_t fi = 0; fi < seg->file_count; ++fi) {
        if (tombstone_bits && fi < tombstone_len && (tombstone_bits[fi >> 3] & (1u << (fi & 7u)))) {
            fmap[fi] = UINT32_MAX;
            continue;
        }
        if (*io_file_count == *io_file_cap) {
            *io_file_cap = *io_file_cap ? *io_file_cap * 2 : 128;
            *io_files = (SegSrcFile *)realloc(*io_files, (*io_file_cap) * sizeof(SegSrcFile));
        }
        uint32_t nf = (*io_file_count)++;
        StelogStr fp = stelog_seg_str(seg, seg->files[fi].path);
        (*io_files)[nf].path = fp.ptr;
        (*io_files)[nf].agent_kind = seg->files[fi].agent_kind;
        (*io_files)[nf].is_zstd = seg->files[fi].is_zstd;
        (*io_files)[nf].is_archived = seg->files[fi].is_archived;
        fmap[fi] = nf;
    }

    for (uint32_t d = 0; d < seg->doc_count; ++d) {
        uint32_t old_fi = seg->col_file_id[d];
        if (old_fi >= seg->file_count || fmap[old_fi] == UINT32_MAX) continue;
        uint32_t sid = seg->col_session_id[d];
        const StelogSessionRecord *sr = &seg->sessions[sid];
        BuildDoc bd;
        memset(&bd, 0, sizeof(bd));
        bd.ts_ms        = seg->col_ts_ms[d];
        bd.raw_offset   = seg->col_raw_off[d];
        bd.raw_len      = seg->col_raw_len[d];
        bd.doc_key      = seg->col_doc_key[d];
        bd.src_file_idx = fmap[old_fi];
        bd.tag          = seg->col_tag[d];
        bd.session_id   = stelog_seg_str(seg, sr->session_id);
        bd.title        = stelog_seg_str(seg, sr->title);
        bd.git_branch   = stelog_seg_str(seg, sr->git_branch);
        bd.project      = stelog_seg_str(seg, seg->projects[seg->col_project_id[d]]);
        bd.actor        = stelog_seg_str(seg, seg->actors[seg->col_actor_id[d]]);
        bd.text.ptr     = seg->text_base + seg->col_text_off[d];
        bd.text.len     = seg->col_text_len[d];
        docvec_push(out_docs, &bd);
    }

    free(fmap);
}

static int cmp_build_task_desc(const void *a, const void *b) {
    const BuildChunkTask *ta = (const BuildChunkTask *)a;
    const BuildChunkTask *tb = (const BuildChunkTask *)b;
    uint64_t sa = ta->chunk_end - ta->chunk_start;
    uint64_t sb = tb->chunk_end - tb->chunk_start;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return 0;
}

static void resolve_agent_home(const char *override, const char *home, const char *rel, char *out, size_t cap) {
    if (override && override[0]) snprintf(out, cap, "%s", override);
    else if (home && home[0]) snprintf(out, cap, "%s/%s", home, rel);
    else out[0] = '\0';
}

/* ============================================================================
 * Public Entry: stelog_index_sync (Base + Hot Delta + Incremental Manifest)
 * ============================================================================ */

static void peek_current_generations(const char *cur_path, uint32_t *base_gen, uint32_t *delta_gen) {
    int fd = open(cur_path, O_RDONLY);
    if (fd < 0) return;
    StelogCurrentMeta cm;
    ssize_t got = read(fd, &cm, sizeof(cm));
    close(fd);
    if (got != (ssize_t)sizeof(cm) || cm.magic != STELOG_CURRENT_MAGIC) return;
    *base_gen = cm.base_gen;
    *delta_gen = cm.delta_gen;
}

int stelog_index_sync(const StelogIndexConfig *cfg, StelogIndexReport *out_report) {
    if (!cfg) return -1;
    StelogIndexReport rep;
    memset(&rep, 0, sizeof(rep));
    double t0 = stelog_now_ms();

    char index_dir[1024];
    stelog_resolve_index_dir(cfg->index_dir, index_dir, sizeof(index_dir));
    if (mkdir_p(index_dir) != 0) return -1;

    /* Acquire exclusive flock for writer */
    char lock_path[1280];
    snprintf(lock_path, sizeof(lock_path), "%s/lock", index_dir);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) return -1;
    if (flock(lock_fd, LOCK_EX) != 0) {
        close(lock_fd);
        return -1;
    }

    /* 1. Discover current source files on disk */
    const char *home = getenv("HOME");
    char codex_dir[1024], agy_dir[1024], pi_dir[1024], droid_dir[1024];
    resolve_agent_home(cfg->codex_home, home, ".codex", codex_dir, sizeof(codex_dir));
    resolve_agent_home(cfg->agy_home, home, ".gemini/antigravity-cli", agy_dir, sizeof(agy_dir));
    resolve_agent_home(cfg->pi_home, home, ".pi/agent", pi_dir, sizeof(pi_dir));
    resolve_agent_home(cfg->droid_home, home, ".factory", droid_dir, sizeof(droid_dir));
    uint32_t mask = cfg->agent_mask ? cfg->agent_mask : ((1u << STELOG_AGENT_COUNT) - 1u);

    StelogFileList discovered;
    stelog_filelist_init(&discovered);
    if (mask & (1u << STELOG_AGENT_CODEX)) stelog_codex_discover(codex_dir, &discovered);
    if (mask & (1u << STELOG_AGENT_AGY))   stelog_agy_discover(agy_dir, &discovered);
    if (mask & (1u << STELOG_AGENT_PI))    stelog_pi_discover(pi_dir, &discovered);
    if (mask & (1u << STELOG_AGENT_DROID)) stelog_droid_discover(droid_dir, &discovered);
    double t_disc = stelog_now_ms();

    /* 2. Load existing current.meta + manifest.bin (unless force_rebuild) */
    char cur_path[1280], man_path[1280];
    snprintf(cur_path, sizeof(cur_path), "%s/current.meta", index_dir);
    snprintf(man_path, sizeof(man_path), "%s/manifest.bin", index_dir);

    StelogIndex *existing = cfg->force_rebuild ? NULL : stelog_index_open(index_dir);
    ManifestTable old_man;
    manifest_init(&old_man);
    if (existing) {
        manifest_load(man_path, &old_man);
    }

    bool do_full_rebuild = (existing == NULL) || cfg->force_rebuild || (!existing->base.loaded);

    /* Content flags are sticky so incremental syncs from callers with default options (e.g. the MCP
       server) keep ingesting what the index was built with; newly requested content needs a full re-ingest. */
    uint16_t content_flags = (uint16_t)((cfg->include_tool_results ? STELOG_INDEX_HAS_TOOL_RESULTS : 0u) |
                                        (cfg->include_thinking ? STELOG_INDEX_HAS_THINKING : 0u) |
                                        (cfg->include_system ? STELOG_INDEX_HAS_SYSTEM : 0u));
    if (existing) {
        uint16_t have = (uint16_t)existing->content_flags;
        if (content_flags & ~have) do_full_rebuild = true;
        content_flags |= have;
    }
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : STELOG_DEFAULT_CHUNK_BYTES;
    uint32_t nworkers = cfg->threads ? cfg->threads : stelog_detect_pcore_count();
    if (nworkers == 0) nworkers = 1;

    /* 3. Diff discovered files against manifest */
    ManifestTable new_man;
    manifest_init(&new_man);

    BuildChunkTask *tasks = NULL;
    size_t task_cap = 64;
    uint32_t task_count = 0;
    tasks = (BuildChunkTask *)malloc(task_cap * sizeof(BuildChunkTask));

    /* Track base file_ids that need tombstoning (file replaced/shrunk/re-indexed) */
    uint32_t *tomb_ids = NULL;
    uint32_t tomb_count = 0;
    uint32_t tomb_cap = 0;
    if (existing && existing->base.loaded && existing->base_file_tombstones) {
        for (uint32_t fi = 0; fi < existing->base.file_count; ++fi) {
            if (stelog_index_base_file_tombstoned(existing, fi)) {
                if (tomb_count == tomb_cap) {
                    tomb_cap = tomb_cap ? tomb_cap * 2 : 32;
                    tomb_ids = (uint32_t *)realloc(tomb_ids, tomb_cap * sizeof(uint32_t));
                }
                tomb_ids[tomb_count++] = fi;
            }
        }
    }

    uint32_t files_changed = 0;
    for (uint32_t i = 0; i < discovered.count; ++i) {
        const StelogFileEntry *f = &discovered.items[i];
        struct stat st;
        if (stat(f->path, &st) != 0) continue;

        uint64_t ino = (uint64_t)st.st_ino;
        int64_t mtime_ns = get_stat_mtime_ns(&st);
        uint64_t fsize = (uint64_t)st.st_size;
        uint64_t phash = stelog_wyhash(f->path, strlen(f->path), 0x123456789abcdef0ull);

        uint64_t start_off = 0;
        bool need_ingest = true;
        uint8_t in_base = do_full_rebuild ? 1 : 0;

        if (!do_full_rebuild) {
            const ManifestItem *prev = manifest_find_path(&old_man, f->path);
            if (prev) {
                if (prev->inode == ino && prev->mtime_ns == mtime_ns && prev->file_size == fsize) {
                    /* Unchanged -> 0ms skip! */
                    need_ingest = false;
                    in_base = prev->in_base;
                } else if (prev->inode == ino && !f->is_zstd && fsize > prev->indexed_offset) {
                    /* Append-only hot tail! Only ingest [prev->indexed_offset, fsize) */
                    start_off = prev->indexed_offset;
                    need_ingest = true;
                    in_base = prev->in_base;
                } else {
                    /* Shrunk or inode replaced -> tombstone old base file if present & re-ingest */
                    start_off = 0;
                    need_ingest = true;
                    in_base = 0;
                    if (existing && existing->base.loaded) {
                        for (uint32_t bfi = 0; bfi < existing->base.file_count; ++bfi) {
                            StelogStr bfp = stelog_seg_str(&existing->base, existing->base.files[bfi].path);
                            if (bfp.len == strlen(f->path) && memcmp(bfp.ptr, f->path, bfp.len) == 0) {
                                if (tomb_count == tomb_cap) {
                                    tomb_cap = tomb_cap ? tomb_cap * 2 : 32;
                                    tomb_ids = (uint32_t *)realloc(tomb_ids, tomb_cap * sizeof(uint32_t));
                                }
                                tomb_ids[tomb_count++] = bfi;
                                break;
                            }
                        }
                    }
                }
            } else if (f->is_zstd && f->session_id_hint) {
                /* Codex cold compression: .jsonl -> .jsonl.zst for an already-indexed session */
                const ManifestItem *prev_sid = manifest_find_sid(&old_man, (uint8_t)f->agent, f->session_id_hint);
                if (prev_sid && !prev_sid->is_zstd) {
                    need_ingest = false;
                    in_base = prev_sid->in_base;
                }
            }
        }

        ManifestItem mi;
        memset(&mi, 0, sizeof(mi));
        mi.path = (char *)f->path;
        mi.session_id = (char *)(f->session_id_hint ? f->session_id_hint : "");
        mi.path_hash = phash;
        mi.inode = ino;
        mi.mtime_ns = mtime_ns;
        mi.file_size = fsize;
        mi.indexed_offset = fsize;
        mi.agent_kind = (uint8_t)f->agent;
        mi.is_zstd = f->is_zstd ? 1 : 0;
        mi.in_base = in_base;
        manifest_push(&new_man, &mi);

        if (!need_ingest || fsize == 0 || start_off >= fsize) continue;
        files_changed++;

        if (f->is_zstd || (fsize - start_off) <= chunk_bytes) {
            if (task_count == task_cap) {
                task_cap *= 2;
                tasks = (BuildChunkTask *)realloc(tasks, task_cap * sizeof(BuildChunkTask));
            }
            tasks[task_count++] = (BuildChunkTask){ i, start_off, fsize, f->is_zstd };
        } else {
            for (uint64_t off = start_off; off < fsize; off += chunk_bytes) {
                if (task_count == task_cap) {
                    task_cap *= 2;
                    tasks = (BuildChunkTask *)realloc(tasks, task_cap * sizeof(BuildChunkTask));
                }
                uint64_t end = off + chunk_bytes;
                if (end > fsize) end = fsize;
                tasks[task_count++] = (BuildChunkTask){ i, off, end, false };
            }
        }
    }

    /* Fast path: nothing changed and no compact requested -> < 1 ms return! */
    if (!do_full_rebuild && files_changed == 0 && !cfg->force_compact) {
        rep.base_gen = existing->base.generation;
        rep.delta_gen = existing->delta.generation;
        rep.base_docs = existing->base.doc_count;
        rep.delta_docs = existing->delta.doc_count;
        rep.files_total = discovered.count;
        rep.unique_terms_base = existing->base.terms.term_count;
        rep.unique_terms_delta = existing->delta.terms.term_count;
        rep.bytes_on_disk = existing->base.disk_bytes + existing->delta.disk_bytes;
        rep.noop_up_to_date = true;
        rep.discover_ms = t_disc - t0;
        rep.total_ms = stelog_now_ms() - t0;

        free(tomb_ids);
        free(tasks);
        manifest_destroy(&new_man);
        manifest_destroy(&old_man);
        stelog_index_close(existing);
        stelog_filelist_destroy(&discovered);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        if (out_report) *out_report = rep;
        return 0;
    }

    /* 4. Load SQLite metadata & run parallel ingest for changed chunks */
    StelogMetaMap meta;
    stelog_metamap_init(&meta, 256);
    if (files_changed > 0) {
        if (mask & (1u << STELOG_AGENT_CODEX)) stelog_codex_load_metadata(codex_dir, &meta);
        if (mask & (1u << STELOG_AGENT_AGY))   stelog_agy_load_metadata(agy_dir, &meta);
    }

    if (task_count > 1) qsort(tasks, task_count, sizeof(BuildChunkTask), cmp_build_task_desc);
    uint32_t run_workers = (task_count > 0 && nworkers > task_count) ? task_count : nworkers;
    if (run_workers == 0) run_workers = 1;

    WorkerDocOut *wouts = (WorkerDocOut *)calloc(run_workers, sizeof(WorkerDocOut));
    for (uint32_t w = 0; w < run_workers; ++w) {
        stelog_arena_init(&wouts[w].arena, 2u * 1024u * 1024u);
    }

    if (task_count > 0) {
        ParallelIngestCtx pctx;
        memset(&pctx, 0, sizeof(pctx));
        pctx.files = &discovered;
        pctx.meta = &meta;
        pctx.tasks = tasks;
        pctx.task_count = task_count;
        pctx.opts.tool_call_max_bytes = cfg->tool_call_max_bytes ? cfg->tool_call_max_bytes : 512u;
        pctx.opts.include_tool_results = (content_flags & STELOG_INDEX_HAS_TOOL_RESULTS) != 0;
        pctx.opts.include_thinking = (content_flags & STELOG_INDEX_HAS_THINKING) != 0;
        pctx.opts.include_system = (content_flags & STELOG_INDEX_HAS_SYSTEM) != 0;
        atomic_init(&pctx.next_task, 0);
        pctx.outs = wouts;
        stelog_parallel_run(run_workers, ingest_worker_fn, &pctx);
    }
    double t_ingest = stelog_now_ms();

    uint32_t new_docs_count = 0;
    for (uint32_t w = 0; w < run_workers; ++w) new_docs_count += wouts[w].docs.count;

    uint32_t delta_thresh = cfg->delta_compact_threshold ? cfg->delta_compact_threshold : STELOG_DEFAULT_DELTA_THRESHOLD;
    uint32_t prev_delta_docs = (existing && existing->delta.loaded) ? existing->delta.doc_count : 0;
    bool need_compact = do_full_rebuild || cfg->force_compact ||
                        (prev_delta_docs + new_docs_count >= delta_thresh) ||
                        (existing && existing->base.loaded && existing->base.file_count > 0 &&
                         tomb_count * 100 >= existing->base.file_count * 15);

    /* 5. Assemble combined docs & file table for the target segment (Base or Delta) */
    SegSrcFile *seg_files = (SegSrcFile *)calloc(discovered.count ? discovered.count : 1u, sizeof(SegSrcFile));
    uint32_t seg_file_count = discovered.count;
    uint32_t seg_file_cap = discovered.count ? discovered.count : 1u;
    for (uint32_t i = 0; i < discovered.count; ++i) {
        seg_files[i].path        = discovered.items[i].path;
        seg_files[i].agent_kind  = (uint8_t)discovered.items[i].agent;
        seg_files[i].is_zstd     = discovered.items[i].is_zstd ? 1 : 0;
        seg_files[i].is_archived = discovered.items[i].is_archived ? 1 : 0;
    }

    BuildDocVec imported_docs = { 0 };
    if (!do_full_rebuild && existing) {
        if (need_compact && existing->base.loaded) {
            uint8_t *tb = NULL;
            uint32_t tblen = existing->base.file_count;
            if (tomb_count > 0 && tblen > 0) {
                tb = (uint8_t *)calloc((tblen + 7u) / 8u, 1);
                for (uint32_t k = 0; k < tomb_count; ++k) {
                    if (tomb_ids[k] < tblen) tb[tomb_ids[k] >> 3] |= (uint8_t)(1u << (tomb_ids[k] & 7u));
                }
            }
            import_segment_docs(&existing->base, tb, tblen, &seg_files, &seg_file_count, &seg_file_cap, &imported_docs);
            free(tb);
        }
        if (existing->delta.loaded) {
            import_segment_docs(&existing->delta, NULL, 0, &seg_files, &seg_file_count, &seg_file_cap, &imported_docs);
        }
    }

    uint32_t total_seg_docs = imported_docs.count + new_docs_count;
    const BuildDoc **ptrs = (const BuildDoc **)malloc((total_seg_docs ? total_seg_docs : 1u) * sizeof(const BuildDoc *));
    uint32_t pi = 0;
    for (uint32_t i = 0; i < imported_docs.count; ++i) {
        imported_docs.items[i].seq = pi;
        ptrs[pi++] = &imported_docs.items[i];
    }
    for (uint32_t w = 0; w < run_workers; ++w) {
        for (uint32_t i = 0; i < wouts[w].docs.count; ++i) {
            wouts[w].docs.items[i].seq = pi;
            ptrs[pi++] = &wouts[w].docs.items[i];
        }
    }
    if (total_seg_docs > 1) {
        qsort(ptrs, total_seg_docs, sizeof(const BuildDoc *), cmp_build_doc_ptr);
    }

    /* 6. Write new Base (if full rebuild / compact) or Tiny Hot Delta */
    uint32_t old_base_gen  = (existing && existing->base.loaded) ? existing->base.generation : 0;
    uint32_t old_delta_gen = (existing && existing->delta.loaded) ? existing->delta.generation : 0;
    if (!existing) {
        /* Forced or format-upgrade rebuild: still advance past the previous generations so readers that
           have the old segments mmapped never see files rewritten in place, and the old dirs get cleaned. */
        peek_current_generations(cur_path, &old_base_gen, &old_delta_gen);
    }

    uint32_t new_base_gen = old_base_gen;
    uint32_t new_delta_gen = old_delta_gen;
    uint32_t new_base_docs = (existing && existing->base.loaded) ? existing->base.doc_count : 0;
    uint32_t new_delta_docs = (existing && existing->delta.loaded) ? existing->delta.doc_count : 0;
    uint32_t uniq_terms = 0;
    uint64_t seg_disk = 0;

    char seg_dir[1280];
    int write_rc = 0;
    if (need_compact) {
        new_base_gen = old_base_gen + 1u;
        new_delta_gen = 0;
        new_base_docs = total_seg_docs;
        new_delta_docs = 0;
        tomb_count = 0;
        for (uint32_t i = 0; i < new_man.count; ++i) new_man.items[i].in_base = 1;

        snprintf(seg_dir, sizeof(seg_dir), "%s/base_%06u", index_dir, new_base_gen);
        write_rc = write_segment_dir(seg_dir, ptrs, total_seg_docs, seg_files, seg_file_count, nworkers, &uniq_terms, &seg_disk);
        rep.unique_terms_base = uniq_terms;
        rep.bytes_on_disk = seg_disk;
    } else {
        new_delta_gen = old_delta_gen + 1u;
        new_delta_docs = total_seg_docs;
        snprintf(seg_dir, sizeof(seg_dir), "%s/delta_%06u", index_dir, new_delta_gen);
        write_rc = write_segment_dir(seg_dir, ptrs, total_seg_docs, seg_files, seg_file_count, nworkers, &uniq_terms, &seg_disk);
        rep.unique_terms_base = existing ? existing->base.terms.term_count : 0;
        rep.unique_terms_delta = uniq_terms;
        rep.bytes_on_disk = (existing ? existing->base.disk_bytes : 0) + seg_disk;
    }

    if (write_rc == 0) {
        /* 7. Save manifest.bin & atomically publish current.meta */
        manifest_save(man_path, &new_man);

        size_t cm_sz = sizeof(StelogCurrentMeta) + (size_t)tomb_count * sizeof(uint32_t);
        uint8_t *cm_buf = (uint8_t *)calloc(1, cm_sz);
        StelogCurrentMeta *cm = (StelogCurrentMeta *)cm_buf;
        cm->magic = STELOG_CURRENT_MAGIC;
        cm->format_version = STELOG_FORMAT_VERSION;
        cm->flags = content_flags;
        cm->base_gen = new_base_gen;
        cm->delta_gen = new_delta_gen;
        cm->base_docs = new_base_docs;
        cm->delta_docs = new_delta_docs;
        cm->updated_at_ms = wall_time_ms();
        cm->tombstone_count = tomb_count;
        if (tomb_count > 0) {
            memcpy(cm_buf + sizeof(StelogCurrentMeta), tomb_ids, tomb_count * sizeof(uint32_t));
        }
        write_rc = write_file_atomic(cur_path, cm_buf, cm_sz);
        free(cm_buf);
    }

    /* 8. Close old mmap handles before cleaning up superseded generation directories */
    if (existing) stelog_index_close(existing);
    if (write_rc == 0) {
        char old_dir[1280];
        if (old_base_gen > 0 && old_base_gen != new_base_gen) {
            snprintf(old_dir, sizeof(old_dir), "%s/base_%06u", index_dir, old_base_gen);
            stelog_rmdir_recursive(old_dir);
        }
        if (old_delta_gen > 0 && old_delta_gen != new_delta_gen) {
            snprintf(old_dir, sizeof(old_dir), "%s/delta_%06u", index_dir, old_delta_gen);
            stelog_rmdir_recursive(old_dir);
        }
    }

    double t_end = stelog_now_ms();
    rep.base_gen = new_base_gen;
    rep.delta_gen = new_delta_gen;
    rep.base_docs = new_base_docs;
    rep.delta_docs = new_delta_docs;
    rep.new_docs_added = new_docs_count;
    rep.files_total = discovered.count;
    rep.files_changed = files_changed;
    rep.was_rebuild = do_full_rebuild;
    rep.did_compact = need_compact && !do_full_rebuild;
    rep.discover_ms = t_disc - t0;
    rep.ingest_ms = t_ingest - t_disc;
    rep.build_ms = t_end - t_ingest;
    rep.total_ms = t_end - t0;

    free(ptrs);
    free(imported_docs.items);
    free(seg_files);
    for (uint32_t w = 0; w < run_workers; ++w) {
        free(wouts[w].docs.items);
        stelog_arena_destroy(&wouts[w].arena);
    }
    free(wouts);
    free(tomb_ids);
    free(tasks);
    stelog_metamap_destroy(&meta);
    manifest_destroy(&new_man);
    manifest_destroy(&old_man);
    stelog_filelist_destroy(&discovered);

    flock(lock_fd, LOCK_UN);
    close(lock_fd);

    if (out_report) *out_report = rep;
    return write_rc;
}
