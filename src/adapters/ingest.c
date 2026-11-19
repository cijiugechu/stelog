#define _POSIX_C_SOURCE 200809L
#include "adapters/adapter.h"
#include "base/simd.h"
#include "base/thread_pool.h"
#include "base/time_iso8601.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#define STELOG_SIMD_PAD 64u
#define STELOG_DEFAULT_CHUNK_BYTES (8u * 1024u * 1024u)
#define STELOG_MAX_VALID_LINE_BYTES (64u * 1024u * 1024u)

const char *stelog_agent_name(StelogAgentKind kind) {
    switch (kind) {
        case STELOG_AGENT_CODEX: return "codex";
        case STELOG_AGENT_AGY:   return "agy";
        case STELOG_AGENT_PI:    return "pi";
        default:                 return "unknown";
    }
}

const char *stelog_role_name(StelogRoleKind kind) {
    switch (kind) {
        case STELOG_ROLE_USER:        return "user";
        case STELOG_ROLE_ASSISTANT:   return "assistant";
        case STELOG_ROLE_INTER_AGENT: return "inter_agent";
        case STELOG_ROLE_TOOL_CALL:   return "tool_call";
        case STELOG_ROLE_TOOL_RESULT: return "tool_result";
        case STELOG_ROLE_THINKING:    return "thinking";
        case STELOG_ROLE_SYSTEM:      return "system";
        default:                      return "unknown";
    }
}

/* ============================================================================
 * File List & Directory Walker
 * ============================================================================ */

void stelog_filelist_init(StelogFileList *list) {
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
    stelog_arena_init(&list->arena, 128u * 1024u);
}

void stelog_filelist_destroy(StelogFileList *list) {
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
    stelog_arena_destroy(&list->arena);
}

uint32_t stelog_filelist_add(StelogFileList *list,
                             StelogAgentKind agent,
                             const char *path,
                             const char *session_id_hint,
                             const char *cwd_hint,
                             uint64_t file_size,
                             bool is_zstd,
                             bool is_archived) {
    if (list->count == list->cap) {
        uint32_t ncap = list->cap ? list->cap * 2 : 128;
        StelogFileEntry *nitems = (StelogFileEntry *)realloc(list->items, ncap * sizeof(StelogFileEntry));
        if (!nitems) return UINT32_MAX;
        list->items = nitems;
        list->cap = ncap;
    }
    uint32_t id = list->count++;
    StelogFileEntry *e = &list->items[id];
    e->file_id = id;
    e->agent = agent;
    e->path = stelog_arena_strdup(&list->arena, path);
    e->session_id_hint = session_id_hint ? stelog_arena_strdup(&list->arena, session_id_hint) : NULL;
    e->cwd_hint = cwd_hint ? stelog_arena_strdup(&list->arena, cwd_hint) : NULL;
    e->file_size = file_size;
    e->is_zstd = is_zstd;
    e->is_archived = is_archived;
    return id;
}

void stelog_walk_dir(const char *dir_path, StelogWalkCb cb, void *ctx) {
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *ent;
    char child[2048];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        int n = snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;

        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            stelog_walk_dir(child, cb, ctx);
        } else if (S_ISREG(st.st_mode)) {
            cb(child, ent->d_name, (uint64_t)st.st_size, ctx);
        }
    }
    closedir(d);
}

/* ============================================================================
 * Chunk Task Queue & Per-Worker State
 * ============================================================================ */

typedef struct {
    uint32_t file_idx;
    uint64_t chunk_start;
    uint64_t chunk_end;
    bool is_zstd;
} ChunkTask;

typedef struct {
    _Alignas(64) StelogAgentStats stats[STELOG_AGENT_COUNT];
} AlignedWorkerStats;

typedef struct {
    const StelogFileList *files;
    const StelogMetaMap *meta;
    const ChunkTask *tasks;
    uint32_t task_count;
    uint32_t chunk_bytes;
    StelogParseOptions parse_opts;
    _Atomic uint32_t next_task;

    AlignedWorkerStats *worker_stats;

    /* Optional sample capture */
    pthread_mutex_t sample_mu;
    StelogSampleList *samples;
    uint32_t sample_limit;
} SharedScanState;

typedef struct {
    StelogJsonParser *json;
    StelogArena line_arena;
    char *read_buf;
    size_t read_buf_cap;
    char *overflow_buf;
    size_t overflow_cap;
    ZSTD_DStream *zds;
    int cached_fd;
    uint32_t cached_file_idx;
} WorkerLocal;

static bool ensure_buf(char **buf, size_t *cap, size_t needed) {
    if (*cap >= needed) return true;
    size_t ncap = *cap ? *cap : (64u * 1024u);
    while (ncap < needed) ncap *= 2;
    char *nb = (char *)realloc(*buf, ncap);
    if (!nb) return false;
    *buf = nb;
    *cap = ncap;
    return true;
}

static ssize_t pread_full(int fd, void *buf, size_t count, uint64_t offset) {
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

static void init_stream_context(const StelogFileEntry *f,
                                const StelogMetaMap *meta,
                                StelogArena *arena,
                                StelogStreamContext *ctx) {
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

    if (meta && f->session_id_hint) {
        const StelogSessionMeta *sm = stelog_metamap_get(meta, f->session_id_hint, strlen(f->session_id_hint));
        if (sm) {
            if (sm->cwd) {
                ctx->cwd.ptr = sm->cwd;
                ctx->cwd.len = (uint32_t)strlen(sm->cwd);
            }
            if (sm->parent_id) {
                ctx->parent_session_id.ptr = sm->parent_id;
                ctx->parent_session_id.len = (uint32_t)strlen(sm->parent_id);
            }
            if (sm->is_subagent) ctx->session_flags |= STELOG_FLAG_IS_SUBAGENT;
            if (sm->is_archived) ctx->session_flags |= STELOG_FLAG_ARCHIVED;

            if (f->agent == STELOG_AGENT_CODEX) {
                char abuf[256];
                if (sm->agent_name && sm->agent_name[0]) {
                    int n = (sm->agent_role && sm->agent_role[0])
                                ? snprintf(abuf, sizeof(abuf), "codex:%s(%s)", sm->agent_name, sm->agent_role)
                                : snprintf(abuf, sizeof(abuf), "codex:%s", sm->agent_name);
                    if (n > 0) {
                        ctx->actor.ptr = stelog_arena_strndup(arena, abuf, (size_t)n);
                        ctx->actor.len = (uint32_t)n;
                    }
                } else if (sm->model && sm->model[0]) {
                    int n = snprintf(abuf, sizeof(abuf), "codex:%s", sm->model);
                    if (n > 0) {
                        ctx->actor.ptr = stelog_arena_strndup(arena, abuf, (size_t)n);
                        ctx->actor.len = (uint32_t)n;
                    }
                }
            } else if (f->agent == STELOG_AGENT_AGY && sm->agent_name && sm->agent_name[0]) {
                char abuf[256];
                int n = snprintf(abuf, sizeof(abuf), "agy:%s", sm->agent_name);
                if (n > 0) {
                    ctx->actor.ptr = stelog_arena_strndup(arena, abuf, (size_t)n);
                    ctx->actor.len = (uint32_t)n;
                }
            }
        }
    }
}

static bool should_skip_line(StelogAgentKind agent,
                             const char *line,
                             size_t len,
                             const StelogParseOptions *opts) {
    switch (agent) {
        case STELOG_AGENT_CODEX:
            return stelog_codex_should_skip_line(line, len, opts->include_tool_results, opts->include_thinking);
        case STELOG_AGENT_AGY:
            return stelog_agy_should_skip_line(line, len, opts->include_tool_results, opts->include_system);
        case STELOG_AGENT_PI:
            return stelog_pi_should_skip_line(line, len, opts->include_tool_results);
        default:
            return true;
    }
}

static void capture_samples(SharedScanState *S,
                            const StelogFileEntry *f,
                            const StelogMetaMap *meta,
                            const StelogLineResult *res) {
    if (!S->samples || S->sample_limit == 0 || res->count == 0) return;
    /* Fast lock-free early exit once full */
    if (S->samples->count >= S->sample_limit) return;

    pthread_mutex_lock(&S->sample_mu);
    StelogArena *sa = (StelogArena *)S->samples->_arena;
    const StelogSessionMeta *sm = (meta && f->session_id_hint)
                                      ? stelog_metamap_get(meta, f->session_id_hint, strlen(f->session_id_hint))
                                      : NULL;
    for (uint32_t i = 0; i < res->count && S->samples->count < S->sample_limit; ++i) {
        const StelogEvent *src = &res->events[i];
        StelogEvent dst = *src;
        if (src->session_id.len) dst.session_id.ptr = stelog_arena_strndup(sa, src->session_id.ptr, src->session_id.len);
        if (src->project.len) dst.project.ptr = stelog_arena_strndup(sa, src->project.ptr, src->project.len);
        if (src->actor.len) dst.actor.ptr = stelog_arena_strndup(sa, src->actor.ptr, src->actor.len);
        if (sm && sm->title && sm->title[0]) {
            dst.title.ptr = stelog_arena_strdup(sa, sm->title);
            dst.title.len = (uint32_t)strlen(sm->title);
        }
        if (src->msg_id.len) dst.msg_id.ptr = stelog_arena_strndup(sa, src->msg_id.ptr, src->msg_id.len);
        if (src->parent_id.len) dst.parent_id.ptr = stelog_arena_strndup(sa, src->parent_id.ptr, src->parent_id.len);
        if (src->text.len) {
            bool tr = false;
            StelogStr preview = stelog_truncate_middle(sa, src->text.ptr, src->text.len, 240, &tr);
            if (!tr) preview.ptr = stelog_arena_strndup(sa, src->text.ptr, src->text.len);
            dst.text = preview;
        }
        S->samples->items[S->samples->count++] = dst;
    }
    pthread_mutex_unlock(&S->sample_mu);
}

static void parse_and_record_line(SharedScanState *S,
                                  WorkerLocal *W,
                                  const StelogFileEntry *f,
                                  const char *line,
                                  size_t line_len,
                                  size_t padded_cap,
                                  uint64_t raw_offset,
                                  StelogStreamContext *ctx,
                                  StelogAgentStats *st) {
    /* Strip trailing CR/whitespace */
    while (line_len > 0 && (line[line_len - 1] == '\r' || line[line_len - 1] == ' ')) {
        line_len--;
    }
    if (line_len == 0) return;

    st->lines_total++;
    if (should_skip_line(f->agent, line, line_len, &S->parse_opts)) {
        st->lines_skipped_prefilter++;
        st->bytes_skipped_prefilter += line_len + 1;
        return;
    }

    st->lines_parsed_simdjson++;
    StelogLineResult res;
    int rc = 0;
    switch (f->agent) {
        case STELOG_AGENT_CODEX:
            rc = stelog_json_parse_codex_line(W->json, line, line_len, padded_cap,
                                              f->file_id, raw_offset, &S->parse_opts,
                                              ctx, &W->line_arena, &res);
            break;
        case STELOG_AGENT_AGY:
            rc = stelog_json_parse_agy_line(W->json, line, line_len, padded_cap,
                                            f->file_id, raw_offset, &S->parse_opts,
                                            ctx, &W->line_arena, &res);
            break;
        case STELOG_AGENT_PI:
            rc = stelog_json_parse_pi_line(W->json, line, line_len, padded_cap,
                                           f->file_id, raw_offset, &S->parse_opts,
                                           ctx, &W->line_arena, &res);
            break;
        default:
            return;
    }

    if (rc != 0) {
        st->parse_errors++;
        return;
    }

    for (uint32_t i = 0; i < res.count; ++i) {
        StelogRoleKind r = stelog_tag_role(res.events[i].tag);
        st->events_emitted++;
        st->text_bytes_emitted += res.events[i].text.len;
        if ((unsigned)r < STELOG_ROLE_COUNT) {
            st->role_counts[r]++;
        }
    }

    if (S->samples && S->sample_limit > 0 && res.count > 0) {
        capture_samples(S, f, S->meta, &res);
    }
}

/*
 * Process one byte-range chunk [task->chunk_start, task->chunk_end) of an uncompressed .jsonl file.
 * Exact ownership invariant: a line is processed iff its first byte lies in [chunk_start, chunk_end).
 */
static void process_jsonl_chunk(SharedScanState *S,
                                WorkerLocal *W,
                                const ChunkTask *task,
                                StelogAgentStats *st) {
    const StelogFileEntry *f = &S->files->items[task->file_idx];

    if (W->cached_file_idx != task->file_idx || W->cached_fd < 0) {
        if (W->cached_fd >= 0) close(W->cached_fd);
        W->cached_fd = open(f->path, O_RDONLY);
        W->cached_file_idx = task->file_idx;
        if (W->cached_fd < 0) return;
    }
    int fd = W->cached_fd;

    uint64_t read_start = (task->chunk_start > 0) ? (task->chunk_start - 1) : 0;
    size_t read_len = (size_t)(task->chunk_end - read_start);
    if (!ensure_buf(&W->read_buf, &W->read_buf_cap, read_len + STELOG_SIMD_PAD)) return;

    ssize_t got = pread_full(fd, W->read_buf, read_len, read_start);
    if (got <= 0) return;
    read_len = (size_t)got;
    memset(W->read_buf + read_len, ' ', STELOG_SIMD_PAD);

    st->chunks_processed++;
    st->bytes_read += (uint64_t)(task->chunk_end - task->chunk_start);

    stelog_arena_reset(&W->line_arena);
    StelogStreamContext ctx;
    init_stream_context(f, S->meta, &W->line_arena, &ctx);

    size_t pos = 0;
    if (task->chunk_start > 0) {
        /* 1-byte overlap trick: find first '\n' starting at chunk_start - 1. */
        const char *nl = (const char *)memchr(W->read_buf, '\n', read_len);
        if (!nl) {
            /* Entire chunk is strictly inside the middle of a giant earlier line (e.g. 11MB compacted). */
            st->bytes_skipped_prefilter += (uint64_t)(task->chunk_end - task->chunk_start);
            return;
        }
        pos = (size_t)(nl - W->read_buf) + 1;
        if (pos > 1) {
            /* Leading tail bytes belonged to a line started in an earlier chunk */
            st->bytes_skipped_prefilter += (uint64_t)(pos - 1);
        }
    }

    while (pos < read_len) {
        uint64_t line_file_off = read_start + pos;
        const char *nl = (const char *)memchr(W->read_buf + pos, '\n', read_len - pos);
        if (nl) {
            size_t line_len = (size_t)(nl - (W->read_buf + pos));
            size_t padded_cap = (read_len + STELOG_SIMD_PAD) - pos;
            parse_and_record_line(S, W, f, W->read_buf + pos, line_len, padded_cap, line_file_off, &ctx, st);
            pos = (size_t)(nl - W->read_buf) + 1;
        } else {
            /* Line starts in this chunk and crosses chunk_end (or hits EOF) */
            size_t partial_len = read_len - pos;
            if (task->chunk_end >= f->file_size) {
                /* EOF without trailing newline */
                size_t padded_cap = (read_len + STELOG_SIMD_PAD) - pos;
                parse_and_record_line(S, W, f, W->read_buf + pos, partial_len, padded_cap, line_file_off, &ctx, st);
                break;
            }

            /* Ensure we have at least 256 bytes to run the pre-filter accurately */
            char peek_buf[320];
            const char *probe = W->read_buf + pos;
            size_t probe_len = partial_len;
            if (partial_len < 256) {
                memcpy(peek_buf, W->read_buf + pos, partial_len);
                ssize_t extra = pread_full(fd, peek_buf + partial_len, 256 - partial_len, task->chunk_end);
                if (extra > 0) probe_len = partial_len + (size_t)extra;
                probe = peek_buf;
            }

            if (should_skip_line(f->agent, probe, probe_len, &S->parse_opts)) {
                /* Noise line crossing chunk_end (e.g. 11MB compacted): stop immediately!
                   Subsequent chunk(s) will skip its remainder in parallel via memchr('\n'). */
                st->lines_total++;
                st->lines_skipped_prefilter++;
                st->bytes_skipped_prefilter += partial_len;
                break;
            }

            /* Valid line crossing chunk_end: assemble full line into overflow_buf */
            if (!ensure_buf(&W->overflow_buf, &W->overflow_cap, partial_len + 65536u + STELOG_SIMD_PAD)) break;
            memcpy(W->overflow_buf, W->read_buf + pos, partial_len);
            size_t acc_len = partial_len;
            uint64_t next_off = task->chunk_end;
            bool found_nl = false;

            while (next_off < f->file_size && acc_len < STELOG_MAX_VALID_LINE_BYTES) {
                if (!ensure_buf(&W->overflow_buf, &W->overflow_cap, acc_len + 65536u + STELOG_SIMD_PAD)) break;
                ssize_t n = pread(fd, W->overflow_buf + acc_len, 65536u, (off_t)next_off);
                if (n <= 0) break;
                const char *onl = (const char *)memchr(W->overflow_buf + acc_len, '\n', (size_t)n);
                if (onl) {
                    acc_len = (size_t)(onl - W->overflow_buf);
                    found_nl = true;
                    break;
                }
                acc_len += (size_t)n;
                next_off += (uint64_t)n;
            }
            (void)found_nl;
            memset(W->overflow_buf + acc_len, ' ', STELOG_SIMD_PAD);
            parse_and_record_line(S, W, f, W->overflow_buf, acc_len, acc_len + STELOG_SIMD_PAD, line_file_off, &ctx, st);
            break;
        }
    }
}

/* Stream-decompress and parse a Codex .jsonl.zst cold rollout file */
static void process_zstd_file(SharedScanState *S,
                              WorkerLocal *W,
                              const ChunkTask *task,
                              StelogAgentStats *st) {
    const StelogFileEntry *f = &S->files->items[task->file_idx];
    int fd = open(f->path, O_RDONLY);
    if (fd < 0) return;

    if (!W->zds) W->zds = ZSTD_createDStream();
    if (!W->zds) {
        close(fd);
        return;
    }
    ZSTD_initDStream(W->zds);

    const size_t in_cap = ZSTD_DStreamInSize();
    const size_t out_cap = ZSTD_DStreamOutSize();
    uint8_t *in_buf = (uint8_t *)malloc(in_cap);
    uint8_t *out_buf = (uint8_t *)malloc(out_cap);
    if (!in_buf || !out_buf) {
        free(in_buf);
        free(out_buf);
        close(fd);
        return;
    }

    stelog_arena_reset(&W->line_arena);
    StelogStreamContext ctx;
    init_stream_context(f, S->meta, &W->line_arena, &ctx);

    size_t acc_len = 0;
    uint64_t decompressed_off = 0;
    bool skipping_giant_line = false;

    st->chunks_processed++;
    for (;;) {
        ssize_t n = read(fd, in_buf, in_cap);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;

        ZSTD_inBuffer zin = { in_buf, (size_t)n, 0 };
        while (zin.pos < zin.size) {
            ZSTD_outBuffer zout = { out_buf, out_cap, 0 };
            size_t ret = ZSTD_decompressStream(W->zds, &zout, &zin);
            if (ZSTD_isError(ret)) {
                st->parse_errors++;
                goto done;
            }
            st->bytes_read += zout.pos;

            size_t p = 0;
            while (p < zout.pos) {
                const char *nl = (const char *)memchr(out_buf + p, '\n', zout.pos - p);
                if (skipping_giant_line) {
                    if (nl) {
                        size_t seg = (size_t)(nl - (const char *)(out_buf + p)) + 1;
                        st->bytes_skipped_prefilter += seg;
                        decompressed_off += seg;
                        p += seg;
                        skipping_giant_line = false;
                    } else {
                        st->bytes_skipped_prefilter += (zout.pos - p);
                        decompressed_off += (zout.pos - p);
                        p = zout.pos;
                    }
                    continue;
                }

                if (nl && acc_len == 0) {
                    /* Fast path: complete line inside out_buf */
                    size_t llen = (size_t)(nl - (const char *)(out_buf + p));
                    parse_and_record_line(S, W, f, (const char *)(out_buf + p), llen,
                                          zout.pos - p, decompressed_off, &ctx, st);
                    decompressed_off += llen + 1;
                    p += llen + 1;
                } else {
                    size_t seg = nl ? (size_t)(nl - (const char *)(out_buf + p)) : (zout.pos - p);
                    if (acc_len + seg > STELOG_MAX_VALID_LINE_BYTES) {
                        st->lines_total++;
                        st->lines_skipped_prefilter++;
                        st->bytes_skipped_prefilter += acc_len + seg;
                        acc_len = 0;
                        skipping_giant_line = (nl == NULL);
                        decompressed_off += seg + (nl ? 1u : 0u);
                        p += seg + (nl ? 1u : 0u);
                        continue;
                    }
                    if (!ensure_buf(&W->overflow_buf, &W->overflow_cap, acc_len + seg + STELOG_SIMD_PAD)) {
                        goto done;
                    }
                    memcpy(W->overflow_buf + acc_len, out_buf + p, seg);
                    acc_len += seg;

                    /* Early noise skip for giant streaming compacted lines */
                    if (!nl && acc_len >= 256 &&
                        should_skip_line(f->agent, W->overflow_buf, acc_len, &S->parse_opts)) {
                        st->lines_total++;
                        st->lines_skipped_prefilter++;
                        st->bytes_skipped_prefilter += acc_len;
                        acc_len = 0;
                        skipping_giant_line = true;
                        decompressed_off += seg;
                        p += seg;
                        continue;
                    }

                    if (nl) {
                        memset(W->overflow_buf + acc_len, ' ', STELOG_SIMD_PAD);
                        parse_and_record_line(S, W, f, W->overflow_buf, acc_len,
                                              acc_len + STELOG_SIMD_PAD,
                                              decompressed_off - (acc_len - seg), &ctx, st);
                        acc_len = 0;
                        decompressed_off += seg + 1;
                        p += seg + 1;
                    } else {
                        decompressed_off += seg;
                        p += seg;
                    }
                }
            }
        }
    }

    if (acc_len > 0 && !skipping_giant_line) {
        memset(W->overflow_buf + acc_len, ' ', STELOG_SIMD_PAD);
        parse_and_record_line(S, W, f, W->overflow_buf, acc_len,
                              acc_len + STELOG_SIMD_PAD, decompressed_off - acc_len, &ctx, st);
    }

done:
    free(in_buf);
    free(out_buf);
    close(fd);
}

static void scan_worker_fn(uint32_t worker_id, uint32_t num_workers, void *user_ctx) {
    (void)num_workers;
    SharedScanState *S = (SharedScanState *)user_ctx;
    WorkerLocal W;
    memset(&W, 0, sizeof(W));
    W.json = stelog_json_parser_create();
    stelog_arena_init(&W.line_arena, 2u * 1024u * 1024u);
    W.cached_fd = -1;
    W.cached_file_idx = UINT32_MAX;

    StelogAgentStats *my_stats = S->worker_stats[worker_id].stats;

    for (;;) {
        uint32_t tidx = atomic_fetch_add_explicit(&S->next_task, 1, memory_order_relaxed);
        if (tidx >= S->task_count) break;
        const ChunkTask *t = &S->tasks[tidx];
        StelogAgentKind kind = S->files->items[t->file_idx].agent;
        if (t->is_zstd) {
            process_zstd_file(S, &W, t, &my_stats[kind]);
        } else {
            process_jsonl_chunk(S, &W, t, &my_stats[kind]);
        }
    }

    if (W.cached_fd >= 0) close(W.cached_fd);
    if (W.zds) ZSTD_freeDStream(W.zds);
    free(W.read_buf);
    free(W.overflow_buf);
    stelog_arena_destroy(&W.line_arena);
    stelog_json_parser_destroy(W.json);
}

/* Sort chunks largest-first (LPT scheduling) to eliminate tail stragglers */
static int cmp_task_desc(const void *a, const void *b) {
    const ChunkTask *ta = (const ChunkTask *)a;
    const ChunkTask *tb = (const ChunkTask *)b;
    uint64_t sa = ta->chunk_end - ta->chunk_start;
    uint64_t sb = tb->chunk_end - tb->chunk_start;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    if (ta->file_idx < tb->file_idx) return -1;
    if (ta->file_idx > tb->file_idx) return 1;
    return (ta->chunk_start < tb->chunk_start) ? -1 : 1;
}

static void resolve_default_path(const char *override, const char *home, const char *rel, char *out, size_t cap) {
    if (override && override[0]) {
        snprintf(out, cap, "%s", override);
    } else if (home && home[0]) {
        snprintf(out, cap, "%s/%s", home, rel);
    } else {
        out[0] = '\0';
    }
}

int stelog_run_scan(const StelogScanConfig *cfg,
                    StelogScanReport *out_report,
                    StelogSampleList *out_samples) {
    if (!cfg || !out_report) return -1;
    memset(out_report, 0, sizeof(*out_report));

    double t_start = stelog_now_ms();
    const char *home = getenv("HOME");
    char codex_dir[1024], agy_dir[1024], pi_dir[1024];
    resolve_default_path(cfg->codex_home, home, ".codex", codex_dir, sizeof(codex_dir));
    resolve_default_path(cfg->agy_home, home, ".gemini/antigravity-cli", agy_dir, sizeof(agy_dir));
    resolve_default_path(cfg->pi_home, home, ".pi/agent", pi_dir, sizeof(pi_dir));

    uint32_t mask = cfg->agent_mask ? cfg->agent_mask : ((1u << STELOG_AGENT_COUNT) - 1u);

    /* 1. Discover files */
    StelogFileList files;
    stelog_filelist_init(&files);
    if (mask & (1u << STELOG_AGENT_CODEX)) stelog_codex_discover(codex_dir, &files);
    if (mask & (1u << STELOG_AGENT_AGY))   stelog_agy_discover(agy_dir, &files);
    if (mask & (1u << STELOG_AGENT_PI))    stelog_pi_discover(pi_dir, &files);
    double t_disc = stelog_now_ms();

    /* 2. Load SQLite session metadata */
    StelogMetaMap meta;
    stelog_metamap_init(&meta, 256);
    if (mask & (1u << STELOG_AGENT_CODEX)) stelog_codex_load_metadata(codex_dir, &meta);
    if (mask & (1u << STELOG_AGENT_AGY))   stelog_agy_load_metadata(agy_dir, &meta);
    double t_meta = stelog_now_ms();

    /* 3. Build Intra-file Chunk Task Queue */
    uint32_t chunk_bytes = cfg->chunk_bytes ? cfg->chunk_bytes : STELOG_DEFAULT_CHUNK_BYTES;
    size_t task_cap = files.count * 2 + 64;
    ChunkTask *tasks = (ChunkTask *)malloc(task_cap * sizeof(ChunkTask));
    uint32_t task_count = 0;

    for (uint32_t i = 0; i < files.count; ++i) {
        const StelogFileEntry *f = &files.items[i];
        out_report->per_agent[f->agent].files_scanned++;
        if (f->is_zstd) out_report->per_agent[f->agent].compressed_files++;
        if (f->file_size == 0) continue;

        if (f->is_zstd || f->file_size <= chunk_bytes) {
            if (task_count == task_cap) {
                task_cap *= 2;
                tasks = (ChunkTask *)realloc(tasks, task_cap * sizeof(ChunkTask));
            }
            tasks[task_count++] = (ChunkTask){ i, 0, f->file_size, f->is_zstd };
        } else {
            for (uint64_t off = 0; off < f->file_size; off += chunk_bytes) {
                if (task_count == task_cap) {
                    task_cap *= 2;
                    tasks = (ChunkTask *)realloc(tasks, task_cap * sizeof(ChunkTask));
                }
                uint64_t end = off + chunk_bytes;
                if (end > f->file_size) end = f->file_size;
                tasks[task_count++] = (ChunkTask){ i, off, end, false };
            }
        }
    }

    if (task_count > 1) {
        qsort(tasks, task_count, sizeof(ChunkTask), cmp_task_desc);
    }

    uint32_t nworkers = cfg->threads ? cfg->threads : stelog_detect_pcore_count();
    if (nworkers == 0) nworkers = 1;
    if (task_count > 0 && nworkers > task_count) nworkers = task_count;

    /* 4. Optional sample collector */
    if (out_samples) {
        memset(out_samples, 0, sizeof(*out_samples));
        if (cfg->sample_limit > 0) {
            out_samples->items = (StelogEvent *)calloc(cfg->sample_limit, sizeof(StelogEvent));
            out_samples->capacity = cfg->sample_limit;
            StelogArena *sa = (StelogArena *)malloc(sizeof(StelogArena));
            stelog_arena_init(sa, 64u * 1024u);
            out_samples->_arena = sa;
        }
    }

    /* 5. Parallel scan on P-core workers */
    AlignedWorkerStats *wstats = (AlignedWorkerStats *)calloc(nworkers, sizeof(AlignedWorkerStats));
    SharedScanState state;
    memset(&state, 0, sizeof(state));
    state.files = &files;
    state.meta = &meta;
    state.tasks = tasks;
    state.task_count = task_count;
    state.chunk_bytes = chunk_bytes;
    state.parse_opts.tool_call_max_bytes = cfg->tool_call_max_bytes ? cfg->tool_call_max_bytes : 512u;
    state.parse_opts.include_tool_results = cfg->include_tool_results;
    state.parse_opts.include_thinking = cfg->include_thinking;
    state.parse_opts.include_system = cfg->include_system;
    atomic_init(&state.next_task, 0);
    state.worker_stats = wstats;
    pthread_mutex_init(&state.sample_mu, NULL);
    state.samples = out_samples;
    state.sample_limit = out_samples ? cfg->sample_limit : 0;

    if (task_count > 0) {
        stelog_parallel_run(nworkers, scan_worker_fn, &state);
    }

    double t_end = stelog_now_ms();

    /* 6. Aggregate lock-free per-worker stats */
    for (uint32_t w = 0; w < nworkers; ++w) {
        for (int a = 0; a < STELOG_AGENT_COUNT; ++a) {
            StelogAgentStats *dst = &out_report->per_agent[a];
            const StelogAgentStats *src = &wstats[w].stats[a];
            dst->chunks_processed += src->chunks_processed;
            dst->bytes_read += src->bytes_read;
            dst->bytes_skipped_prefilter += src->bytes_skipped_prefilter;
            dst->lines_total += src->lines_total;
            dst->lines_skipped_prefilter += src->lines_skipped_prefilter;
            dst->lines_parsed_simdjson += src->lines_parsed_simdjson;
            dst->parse_errors += src->parse_errors;
            dst->events_emitted += src->events_emitted;
            dst->text_bytes_emitted += src->text_bytes_emitted;
            for (int r = 0; r < STELOG_ROLE_COUNT; ++r) {
                dst->role_counts[r] += src->role_counts[r];
            }
        }
    }

    out_report->worker_threads = nworkers;
    out_report->discover_ms = t_disc - t_start;
    out_report->metadata_ms = t_meta - t_disc;
    out_report->scan_ms = t_end - t_meta;
    out_report->elapsed_ms = t_end - t_start;

    pthread_mutex_destroy(&state.sample_mu);
    free(wstats);
    free(tasks);
    stelog_metamap_destroy(&meta);
    stelog_filelist_destroy(&files);
    return 0;
}

void stelog_sample_list_free(StelogSampleList *list) {
    if (!list) return;
    free(list->items);
    if (list->_arena) {
        stelog_arena_destroy((StelogArena *)list->_arena);
        free(list->_arena);
    }
    memset(list, 0, sizeof(*list));
}

/* Unit test helper: run exact chunking pipeline on a single file with custom chunk_bytes */
int stelog_scan_single_file_for_test(const char *path,
                                     StelogAgentKind agent,
                                     uint32_t chunk_bytes,
                                     const StelogParseOptions *opts,
                                     StelogAgentStats *out_stats) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    memset(out_stats, 0, sizeof(*out_stats));

    StelogFileList files;
    stelog_filelist_init(&files);
    stelog_filelist_add(&files, agent, path, "test-session", "/tmp/test", (uint64_t)st.st_size, false, false);

    uint32_t cb = chunk_bytes ? chunk_bytes : STELOG_DEFAULT_CHUNK_BYTES;
    uint32_t max_tasks = (uint32_t)((st.st_size + cb - 1) / cb);
    if (max_tasks == 0) max_tasks = 1;
    ChunkTask *tasks = (ChunkTask *)calloc(max_tasks, sizeof(ChunkTask));
    uint32_t tcount = 0;
    for (uint64_t off = 0; off < (uint64_t)st.st_size; off += cb) {
        uint64_t end = off + cb;
        if (end > (uint64_t)st.st_size) end = (uint64_t)st.st_size;
        tasks[tcount++] = (ChunkTask){ 0, off, end, false };
    }

    AlignedWorkerStats wstats;
    memset(&wstats, 0, sizeof(wstats));
    SharedScanState state;
    memset(&state, 0, sizeof(state));
    state.files = &files;
    state.tasks = tasks;
    state.task_count = tcount;
    state.chunk_bytes = cb;
    state.parse_opts = *opts;
    atomic_init(&state.next_task, 0);
    state.worker_stats = &wstats;
    pthread_mutex_init(&state.sample_mu, NULL);

    scan_worker_fn(0, 1, &state);
    *out_stats = wstats.stats[agent];

    pthread_mutex_destroy(&state.sample_mu);
    free(tasks);
    stelog_filelist_destroy(&files);
    return 0;
}
