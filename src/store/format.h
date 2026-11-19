#ifndef STELOG_STORE_FORMAT_H
#define STELOG_STORE_FORMAT_H

#include "stelog.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STELOG_INDEX_MAGIC      0x474C5453u /* 'STLG' */
#define STELOG_MANIFEST_MAGIC   0x4E4D5453u /* 'STMN' */
#define STELOG_CURRENT_MAGIC    0x52435453u /* 'STCR' */
#define STELOG_FORMAT_VERSION   4u

/* Column indices in meta.col */
enum {
    STELOG_COL_TS_MS       = 0,  /* int64_t[doc_count]  */
    STELOG_COL_TAG         = 1,  /* uint8_t[doc_count]  */
    STELOG_COL_PROJECT_ID  = 2,  /* uint32_t[doc_count] */
    STELOG_COL_ACTOR_ID    = 3,  /* uint32_t[doc_count] */
    STELOG_COL_SESSION_ID  = 4,  /* uint32_t[doc_count] */
    STELOG_COL_TEXT_OFF    = 5,  /* uint64_t[doc_count] */
    STELOG_COL_TEXT_LEN    = 6,  /* uint32_t[doc_count] */
    STELOG_COL_PARENT_DOC  = 7,  /* uint32_t[doc_count] */
    STELOG_COL_FILE_ID     = 8,  /* uint32_t[doc_count] */
    STELOG_COL_RAW_OFF     = 9,  /* uint64_t[doc_count] */
    STELOG_COL_RAW_LEN     = 10, /* uint32_t[doc_count] */
    STELOG_COL_DOC_KEY     = 11, /* uint64_t[doc_count]: public doc_id (see stelog_doc_key_*) */
    STELOG_COL_KEY_ORDER   = 12, /* uint32_t[doc_count]: local doc ids sorted by DOC_KEY */
    STELOG_COL_COUNT       = 13
};

/* Fixed 192-byte, 64-byte aligned header at start of meta.col */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t flags;
    uint32_t doc_count;
    uint32_t session_count;
    uint32_t actor_count;
    uint32_t project_count;
    uint32_t file_count;
    uint32_t avg_text_len;
    uint64_t col_offsets[STELOG_COL_COUNT]; /* each 64-byte aligned */
    uint8_t  _reserved[56];
} StelogMetaHeader;

_Static_assert(sizeof(StelogMetaHeader) == 192, "StelogMetaHeader must be 192 bytes");

/* String slice stored inside dict.bin string pool */
typedef struct {
    uint32_t off;
    uint32_t len;
} StelogStrRef;

typedef struct {
    StelogStrRef session_id;
    StelogStrRef title;
    StelogStrRef cwd;
    StelogStrRef git_branch;
    StelogStrRef parent_session_id;
    int64_t  first_ts_ms;
    int64_t  last_ts_ms;
    uint32_t project_id;
    uint32_t doc_count;
    uint8_t  agent_kind;
    uint8_t  flags;
    uint8_t  _pad[6];
} StelogSessionRecord;

typedef struct {
    StelogStrRef path;
    uint8_t  agent_kind;
    uint8_t  is_zstd;
    uint8_t  is_archived;
    uint8_t  _pad[5];
} StelogFileRecord;

/* Header of dict.bin */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t _pad;
    uint32_t session_count;
    uint32_t actor_count;
    uint32_t project_count;
    uint32_t file_count;
    uint64_t sessions_off;
    uint64_t actors_off;    /* StelogStrRef[actor_count] */
    uint64_t projects_off;  /* StelogStrRef[project_count] */
    uint64_t files_off;     /* StelogFileRecord[file_count] */
    uint64_t strpool_off;
    uint64_t strpool_len;
} StelogDictHeader;

/* Header of session_idx.bin (CSR mapping session_id -> sorted doc_ids) */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t _pad;
    uint32_t session_count;
    uint32_t doc_count;
    uint64_t offsets_off;   /* uint32_t[session_count + 1] */
    uint64_t docs_off;      /* uint32_t[doc_count] */
} StelogSessionIdxHeader;

/* ============================================================================
 * Static SwissTable (terms.ht)
 * ============================================================================ */

#define STELOG_SWISS_EMPTY 0x80u

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t _pad;
    uint32_t term_count;
    uint32_t num_groups;    /* power of 2; total slots = num_groups * 16 */
    uint64_t ctrl_off;      /* 64B-aligned uint8_t[num_groups * 16 + 16] */
    uint64_t slots_off;     /* 64B-aligned StelogTermSlot[num_groups * 16] */
} StelogSwissHeader;

/* 16 bytes per slot -> 4 slots fit in one 64-byte cache line */
typedef struct {
    uint64_t term_hash;
    uint32_t doc_freq;
    uint32_t payload_or_off; /* DF == 1: inline doc_id directly!
                                DF >= 2: byte offset / 4 inside postings.bin */
} StelogTermSlot;

_Static_assert(sizeof(StelogTermSlot) == 16, "StelogTermSlot must be 16 bytes");

/* ============================================================================
 * Adaptive 3-Tier Postings & Roaring 64K-Chunk Containers
 * ============================================================================ */

#define STELOG_POSTING_COMPACT_MAX 63u /* DF in [2..63] stored as raw sorted uint32_t[] */

typedef enum {
    STELOG_ROARING_ARRAY16  = 0, /* uint16_t[card] sorted */
    STELOG_ROARING_BITSET8K = 1, /* uint64_t[1024] = 8192 bytes */
    STELOG_ROARING_RUN16    = 2  /* uint16_t[num_runs * 2]: (start, len_minus_1) */
} StelogRoaringKind;

typedef struct {
    uint16_t key;           /* doc_id >> 16 */
    uint8_t  kind;          /* StelogRoaringKind */
    uint8_t  _pad;
    uint16_t card_minus_1;  /* cardinality - 1 (or num_runs for RUN16, stored in separate u16) */
    uint16_t run_count;     /* used only for RUN16 */
    uint32_t data_off;      /* byte offset relative to container payload start */
} StelogRoaringChunkMeta;

_Static_assert(sizeof(StelogRoaringChunkMeta) == 12, "StelogRoaringChunkMeta must be 12 bytes");

/* Header of a serialized Roaring bitmap blob (used in postings.bin for DF>=64 and bitmaps.bin) */
typedef struct {
    uint32_t total_card;
    uint32_t num_chunks;
    /* Followed by StelogRoaringChunkMeta[num_chunks], padded to 8B, then chunk payloads */
} StelogRoaringHeader;

/* Header of bitmaps.bin (dimension filter bitmaps) */
typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t _pad;
    uint32_t agent_count;
    uint32_t role_count;
    uint32_t project_count;
    uint32_t _pad2;
    uint64_t agent_offs_off;    /* uint64_t[agent_count] (0 = empty) */
    uint64_t role_offs_off;     /* uint64_t[role_count]  (0 = empty) */
    uint64_t project_offs_off;  /* uint64_t[project_count] (0 = empty) */
} StelogBitmapsHeader;

/* ============================================================================
 * Top-Level Index Directory State (current.meta & manifest.bin)
 * ============================================================================ */

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t flags;         /* STELOG_INDEX_HAS_* content flags the index was built with */
    uint32_t base_gen;      /* 0 if none */
    uint32_t delta_gen;     /* 0 if none */
    uint32_t base_docs;
    uint32_t delta_docs;
    int64_t  updated_at_ms;
    uint32_t tombstone_count; /* replaced/deleted file paths in base */
    uint32_t _pad;
    /* Followed by tombstone uint32_t file_ids (indices in base dict files table) */
} StelogCurrentMeta;

typedef struct {
    uint64_t path_hash;
    uint64_t inode;
    int64_t  mtime_ns;
    uint64_t file_size;
    uint64_t indexed_offset;
    uint32_t path_off;
    uint32_t path_len;
    uint32_t sid_off;
    uint32_t sid_len;
    uint8_t  agent_kind;
    uint8_t  is_zstd;
    uint8_t  in_base;       /* 1 if indexed in base, 0 if only in delta */
    uint8_t  _pad[5];
} StelogManifestEntry;

typedef struct {
    uint32_t magic;
    uint16_t format_version;
    uint16_t _pad;
    uint32_t entry_count;
    uint32_t _pad2;
    uint64_t entries_off;
    uint64_t strpool_off;
    uint64_t strpool_len;
} StelogManifestHeader;

static inline uint64_t stelog_align64(uint64_t v) {
    return (v + 63ull) & ~63ull;
}

static inline uint64_t stelog_align8(uint64_t v) {
    return (v + 7ull) & ~7ull;
}

#ifdef __cplusplus
}
#endif

#endif /* STELOG_STORE_FORMAT_H */
