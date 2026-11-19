#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "store/segment_reader.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int mmap_ro_file(const char *path, StelogMmapFile *out) {
    out->base = NULL;
    out->len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }
    if (st.st_size == 0) {
        close(fd);
        return 0;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    out->base = (uint8_t *)p;
    out->len = (size_t)st.st_size;
    return 0;
}

static void munmap_file(StelogMmapFile *f) {
    if (f->base && f->len > 0) {
        munmap(f->base, f->len);
    }
    f->base = NULL;
    f->len = 0;
}

int stelog_segment_open(const char *seg_dir, uint32_t generation, StelogSegment *out) {
    memset(out, 0, sizeof(*out));
    out->generation = generation;

    char path[1536];

    snprintf(path, sizeof(path), "%s/meta.col", seg_dir);
    if (mmap_ro_file(path, &out->f_meta) != 0 || out->f_meta.len < sizeof(StelogMetaHeader)) goto fail;

    snprintf(path, sizeof(path), "%s/dict.bin", seg_dir);
    if (mmap_ro_file(path, &out->f_dict) != 0 || out->f_dict.len < sizeof(StelogDictHeader)) goto fail;

    snprintf(path, sizeof(path), "%s/session_idx.bin", seg_dir);
    if (mmap_ro_file(path, &out->f_sess_idx) != 0 || out->f_sess_idx.len < sizeof(StelogSessionIdxHeader)) goto fail;

    snprintf(path, sizeof(path), "%s/terms.ht", seg_dir);
    if (mmap_ro_file(path, &out->f_terms) != 0) goto fail;

    snprintf(path, sizeof(path), "%s/postings.bin", seg_dir);
    if (mmap_ro_file(path, &out->f_postings) != 0) goto fail;

    snprintf(path, sizeof(path), "%s/bitmaps.bin", seg_dir);
    if (mmap_ro_file(path, &out->f_bitmaps) != 0 || out->f_bitmaps.len < sizeof(StelogBitmapsHeader)) goto fail;

    snprintf(path, sizeof(path), "%s/text.bin", seg_dir);
    if (mmap_ro_file(path, &out->f_text) != 0) goto fail;

    /* Wire up meta.col SoA column pointers (0 copies, 0 deserialization!) */
    const StelogMetaHeader *mh = (const StelogMetaHeader *)out->f_meta.base;
    if (mh->magic != STELOG_INDEX_MAGIC || mh->format_version != STELOG_FORMAT_VERSION) goto fail;
    out->doc_count     = mh->doc_count;
    out->session_count = mh->session_count;
    out->actor_count   = mh->actor_count;
    out->project_count = mh->project_count;
    out->file_count    = mh->file_count;
    out->avg_text_len  = mh->avg_text_len ? mh->avg_text_len : 1u;

    const uint8_t *mb = out->f_meta.base;
    out->col_ts_ms      = (const int64_t  *)(mb + mh->col_offsets[STELOG_COL_TS_MS]);
    out->col_tag        = (const uint8_t  *)(mb + mh->col_offsets[STELOG_COL_TAG]);
    out->col_project_id = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_PROJECT_ID]);
    out->col_actor_id   = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_ACTOR_ID]);
    out->col_session_id = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_SESSION_ID]);
    out->col_text_off   = (const uint64_t *)(mb + mh->col_offsets[STELOG_COL_TEXT_OFF]);
    out->col_text_len   = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_TEXT_LEN]);
    out->col_parent_doc = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_PARENT_DOC]);
    out->col_file_id    = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_FILE_ID]);
    out->col_raw_off    = (const uint64_t *)(mb + mh->col_offsets[STELOG_COL_RAW_OFF]);
    out->col_raw_len    = (const uint32_t *)(mb + mh->col_offsets[STELOG_COL_RAW_LEN]);

    /* Hint kernel to keep hot metadata columns resident */
    madvise(out->f_meta.base, out->f_meta.len, MADV_WILLNEED);

    /* Wire up dict.bin */
    const StelogDictHeader *dh = (const StelogDictHeader *)out->f_dict.base;
    if (dh->magic != STELOG_INDEX_MAGIC || dh->format_version != STELOG_FORMAT_VERSION) goto fail;
    out->sessions    = (const StelogSessionRecord *)(out->f_dict.base + dh->sessions_off);
    out->actors      = (const StelogStrRef *)(out->f_dict.base + dh->actors_off);
    out->projects    = (const StelogStrRef *)(out->f_dict.base + dh->projects_off);
    out->files       = (const StelogFileRecord *)(out->f_dict.base + dh->files_off);
    out->strpool     = (const char *)(out->f_dict.base + dh->strpool_off);
    out->strpool_len = (size_t)dh->strpool_len;

    /* Wire up session_idx.bin */
    const StelogSessionIdxHeader *sh = (const StelogSessionIdxHeader *)out->f_sess_idx.base;
    if (sh->magic != STELOG_INDEX_MAGIC || sh->format_version != STELOG_FORMAT_VERSION) goto fail;
    out->session_offsets = (const uint32_t *)(out->f_sess_idx.base + sh->offsets_off);
    out->session_docs    = (const uint32_t *)(out->f_sess_idx.base + sh->docs_off);

    /* Wire up SwissTable */
    if (stelog_swiss_open(out->f_terms.base, out->f_terms.len, &out->terms) != 0) goto fail;

    /* Postings */
    out->postings_base = out->f_postings.base;
    out->postings_len  = out->f_postings.len;

    /* Dimension bitmaps */
    const StelogBitmapsHeader *bh = (const StelogBitmapsHeader *)out->f_bitmaps.base;
    if (bh->magic != STELOG_INDEX_MAGIC || bh->format_version != STELOG_FORMAT_VERSION) goto fail;
    out->bm_agent_count   = bh->agent_count;
    out->bm_role_count    = bh->role_count;
    out->bm_project_count = bh->project_count;
    out->agent_bm_offs    = (const uint64_t *)(out->f_bitmaps.base + bh->agent_offs_off);
    out->role_bm_offs     = (const uint64_t *)(out->f_bitmaps.base + bh->role_offs_off);
    out->project_bm_offs  = (const uint64_t *)(out->f_bitmaps.base + bh->project_offs_off);
    out->bitmaps_base     = out->f_bitmaps.base;
    out->bitmaps_len      = out->f_bitmaps.len;

    /* Text pool (cold, random access on Top-K hits) */
    out->text_base = (const char *)out->f_text.base;
    out->text_len  = out->f_text.len;
    if (out->f_text.base && out->f_text.len > 0) {
        madvise(out->f_text.base, out->f_text.len, MADV_RANDOM);
    }

    out->disk_bytes = out->f_meta.len + out->f_dict.len + out->f_sess_idx.len +
                      out->f_terms.len + out->f_postings.len + out->f_bitmaps.len + out->f_text.len;
    out->loaded = true;
    return 0;

fail:
    stelog_segment_close(out);
    return -1;
}

void stelog_segment_close(StelogSegment *seg) {
    if (!seg) return;
    munmap_file(&seg->f_meta);
    munmap_file(&seg->f_dict);
    munmap_file(&seg->f_sess_idx);
    munmap_file(&seg->f_terms);
    munmap_file(&seg->f_postings);
    munmap_file(&seg->f_bitmaps);
    munmap_file(&seg->f_text);
    memset(seg, 0, sizeof(*seg));
}

void stelog_resolve_index_dir(const char *override_dir, char *out, size_t cap) {
    if (override_dir && override_dir[0]) {
        snprintf(out, cap, "%s", override_dir);
        return;
    }
    const char *env = getenv("STELOG_INDEX_DIR");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        return;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, cap, "%s/.cache/stelog/index.v2", home);
    } else {
        snprintf(out, cap, ".stelog_index");
    }
}

static int64_t stat_mtime_ns(const struct stat *st) {
#ifdef __APPLE__
    return (int64_t)st->st_mtimespec.tv_sec * 1000000000ll + (int64_t)st->st_mtimespec.tv_nsec;
#else
    return (int64_t)st->st_mtim.tv_sec * 1000000000ll + (int64_t)st->st_mtim.tv_nsec;
#endif
}

static int load_current_into(StelogIndex *idx) {
    char meta_path[1280];
    snprintf(meta_path, sizeof(meta_path), "%s/current.meta", idx->index_dir);

    struct stat st;
    if (stat(meta_path, &st) != 0) return -1;

    int fd = open(meta_path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    if (!buf) {
        close(fd);
        return -1;
    }
    ssize_t got = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (got < (ssize_t)sizeof(StelogCurrentMeta)) {
        free(buf);
        return -1;
    }

    const StelogCurrentMeta *cm = (const StelogCurrentMeta *)buf;
    if (cm->magic != STELOG_CURRENT_MAGIC || cm->format_version != STELOG_FORMAT_VERSION) {
        free(buf);
        return -1;
    }

    StelogSegment new_base;
    StelogSegment new_delta;
    memset(&new_base, 0, sizeof(new_base));
    memset(&new_delta, 0, sizeof(new_delta));

    char seg_dir[1280];
    if (cm->base_gen > 0) {
        snprintf(seg_dir, sizeof(seg_dir), "%s/base_%06u", idx->index_dir, cm->base_gen);
        if (stelog_segment_open(seg_dir, cm->base_gen, &new_base) != 0) {
            free(buf);
            return -1;
        }
    }
    if (cm->delta_gen > 0) {
        snprintf(seg_dir, sizeof(seg_dir), "%s/delta_%06u", idx->index_dir, cm->delta_gen);
        if (stelog_segment_open(seg_dir, cm->delta_gen, &new_delta) != 0) {
            stelog_segment_close(&new_base);
            free(buf);
            return -1;
        }
    }

    uint8_t *tomb = NULL;
    uint32_t tomb_len = 0;
    if (cm->tombstone_count > 0 && new_base.loaded && new_base.file_count > 0) {
        size_t need = sizeof(StelogCurrentMeta) + (size_t)cm->tombstone_count * sizeof(uint32_t);
        if ((size_t)got >= need) {
            tomb_len = new_base.file_count;
            tomb = (uint8_t *)calloc((tomb_len + 7u) / 8u, 1);
            const uint32_t *ids = (const uint32_t *)(buf + sizeof(StelogCurrentMeta));
            for (uint32_t i = 0; i < cm->tombstone_count; ++i) {
                if (ids[i] < tomb_len) {
                    tomb[ids[i] >> 3] |= (uint8_t)(1u << (ids[i] & 7u));
                }
            }
        }
    }

    /* Atomic swap in memory */
    stelog_segment_close(&idx->base);
    stelog_segment_close(&idx->delta);
    free(idx->base_file_tombstones);

    idx->base = new_base;
    idx->delta = new_delta;
    idx->base_file_tombstones = tomb;
    idx->base_file_tombstones_len = tomb_len;
    idx->current_inode = (uint64_t)st.st_ino;
    idx->current_mtime_ns = stat_mtime_ns(&st);
    idx->updated_at_ms = cm->updated_at_ms;

    free(buf);
    return 0;
}

StelogIndex *stelog_index_open(const char *index_dir) {
    StelogIndex *idx = (StelogIndex *)calloc(1, sizeof(StelogIndex));
    if (!idx) return NULL;
    stelog_resolve_index_dir(index_dir, idx->index_dir, sizeof(idx->index_dir));
    stelog_arena_init(&idx->scratch, 4u * 1024u * 1024u);

    if (load_current_into(idx) != 0) {
        stelog_arena_destroy(&idx->scratch);
        free(idx);
        return NULL;
    }
    return idx;
}

void stelog_index_close(StelogIndex *idx) {
    if (!idx) return;
    stelog_segment_close(&idx->base);
    stelog_segment_close(&idx->delta);
    free(idx->base_file_tombstones);
    stelog_arena_destroy(&idx->scratch);
    free(idx);
}

int stelog_index_reload_if_changed(StelogIndex *idx) {
    if (!idx) return -1;
    char meta_path[1280];
    snprintf(meta_path, sizeof(meta_path), "%s/current.meta", idx->index_dir);
    struct stat st;
    if (stat(meta_path, &st) != 0) return -1;
    if ((uint64_t)st.st_ino == idx->current_inode && stat_mtime_ns(&st) == idx->current_mtime_ns) {
        return 0;
    }
    return load_current_into(idx) == 0 ? 1 : -1;
}

void stelog_index_get_stats(const StelogIndex *idx, StelogIndexStats *out) {
    memset(out, 0, sizeof(*out));
    if (!idx) return;
    out->updated_at_ms = idx->updated_at_ms;
    if (idx->base.loaded) {
        out->base_gen        = idx->base.generation;
        out->base_docs       = idx->base.doc_count;
        out->base_sessions   = idx->base.session_count;
        out->base_terms      = idx->base.terms.term_count;
        out->base_text_bytes = idx->base.text_len;
        out->total_disk_bytes += idx->base.disk_bytes;
    }
    if (idx->delta.loaded) {
        out->delta_gen        = idx->delta.generation;
        out->delta_docs       = idx->delta.doc_count;
        out->delta_sessions   = idx->delta.session_count;
        out->delta_terms      = idx->delta.terms.term_count;
        out->delta_text_bytes = idx->delta.text_len;
        out->total_disk_bytes += idx->delta.disk_bytes;
    }
}

size_t stelog_index_scratch_allocated_bytes(const StelogIndex *idx) {
    return idx ? idx->scratch.total_allocated : 0;
}
