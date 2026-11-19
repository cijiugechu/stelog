#ifndef STELOG_STORE_SEGMENT_READER_H
#define STELOG_STORE_SEGMENT_READER_H

#include "stelog.h"
#include "base/arena.h"
#include "store/format.h"
#include "store/roaring.h"
#include "store/swisstable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *base;
    size_t len;
} StelogMmapFile;

typedef struct {
    bool loaded;
    uint32_t generation;

    StelogMmapFile f_meta;
    StelogMmapFile f_dict;
    StelogMmapFile f_sess_idx;
    StelogMmapFile f_terms;
    StelogMmapFile f_postings;
    StelogMmapFile f_bitmaps;
    StelogMmapFile f_text;

    /* Zero-copy pointers into meta.col */
    uint32_t doc_count;
    uint32_t session_count;
    uint32_t actor_count;
    uint32_t project_count;
    uint32_t file_count;
    uint32_t avg_text_len;

    const int64_t  *col_ts_ms;
    const uint8_t  *col_tag;
    const uint32_t *col_project_id;
    const uint32_t *col_actor_id;
    const uint32_t *col_session_id;
    const uint64_t *col_text_off;
    const uint32_t *col_text_len;
    const uint32_t *col_parent_doc;
    const uint32_t *col_file_id;
    const uint64_t *col_raw_off;
    const uint32_t *col_raw_len;
    const uint64_t *col_doc_key;
    const uint32_t *col_key_order;

    /* Zero-copy pointers into dict.bin */
    const StelogSessionRecord *sessions;
    const StelogStrRef *actors;
    const StelogStrRef *projects;
    const StelogFileRecord *files;
    const char *strpool;
    size_t strpool_len;

    /* Zero-copy pointers into session_idx.bin */
    const uint32_t *session_offsets;
    const uint32_t *session_docs;

    /* Zero-copy SwissTable, Postings, Bitmaps, Text */
    StelogSwissView terms;
    const uint8_t *postings_base;
    size_t postings_len;

    const uint64_t *agent_bm_offs;
    const uint64_t *role_bm_offs;
    const uint64_t *project_bm_offs;
    uint32_t bm_agent_count;
    uint32_t bm_role_count;
    uint32_t bm_project_count;
    const uint8_t *bitmaps_base;
    size_t bitmaps_len;

    const char *text_base;
    size_t text_len;

    uint64_t disk_bytes;
} StelogSegment;

struct StelogIndex {
    char index_dir[1024];
    uint64_t current_inode;
    int64_t  current_mtime_ns;
    int64_t  updated_at_ms;
    uint32_t content_flags;   /* STELOG_INDEX_HAS_* from current.meta */

    StelogSegment base;
    StelogSegment delta;

    /* Tombstoned base file_ids (bitset of length base.file_count) */
    uint8_t *base_file_tombstones;
    uint32_t base_file_tombstones_len;

    /* Pre-allocated 4 MiB scratch arena for 0-malloc hot queries */
    StelogArena scratch;
};

int stelog_segment_open(const char *seg_dir, uint32_t generation, StelogSegment *out);
void stelog_segment_close(StelogSegment *seg);

static inline StelogStr stelog_seg_str(const StelogSegment *seg, StelogStrRef ref) {
    if (ref.len == 0 || (uint64_t)ref.off + ref.len > seg->strpool_len) {
        return (StelogStr){ "", 0 };
    }
    return (StelogStr){ seg->strpool + ref.off, ref.len };
}

static inline StelogDocSet stelog_seg_agent_bitmap(const StelogSegment *seg, uint32_t agent) {
    if (agent >= seg->bm_agent_count || seg->agent_bm_offs[agent] == 0) {
        StelogDocSet e = { STELOG_DOCSET_EMPTY, 0, 0, NULL, NULL };
        return e;
    }
    return stelog_docset_from_roaring(seg->bitmaps_base + seg->agent_bm_offs[agent]);
}

static inline StelogDocSet stelog_seg_role_bitmap(const StelogSegment *seg, uint32_t role) {
    if (role >= seg->bm_role_count || seg->role_bm_offs[role] == 0) {
        StelogDocSet e = { STELOG_DOCSET_EMPTY, 0, 0, NULL, NULL };
        return e;
    }
    return stelog_docset_from_roaring(seg->bitmaps_base + seg->role_bm_offs[role]);
}

static inline StelogDocSet stelog_seg_project_bitmap(const StelogSegment *seg, uint32_t proj) {
    if (proj >= seg->bm_project_count || seg->project_bm_offs[proj] == 0) {
        StelogDocSet e = { STELOG_DOCSET_EMPTY, 0, 0, NULL, NULL };
        return e;
    }
    return stelog_docset_from_roaring(seg->bitmaps_base + seg->project_bm_offs[proj]);
}

static inline bool stelog_index_base_file_tombstoned(const StelogIndex *idx, uint32_t file_id) {
    if (!idx->base_file_tombstones || file_id >= idx->base_file_tombstones_len) return false;
    return (idx->base_file_tombstones[file_id >> 3] & (1u << (file_id & 7u))) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* STELOG_STORE_SEGMENT_READER_H */
