#ifndef STELOG_H
#define STELOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STELOG_AGENT_CODEX = 0,
    STELOG_AGENT_AGY   = 1,
    STELOG_AGENT_PI    = 2,
    STELOG_AGENT_DROID = 3,
    STELOG_AGENT_COUNT = 4
} StelogAgentKind;

typedef enum {
    STELOG_ROLE_USER        = 0,
    STELOG_ROLE_ASSISTANT   = 1,
    STELOG_ROLE_INTER_AGENT = 2,
    STELOG_ROLE_TOOL_CALL   = 3,
    STELOG_ROLE_TOOL_RESULT = 4,
    STELOG_ROLE_THINKING    = 5,
    STELOG_ROLE_SYSTEM      = 6,
    STELOG_ROLE_COUNT       = 7
} StelogRoleKind;

/* Default decision-focused role mask: USER | ASSISTANT | INTER_AGENT | TOOL_CALL */
#define STELOG_DEFAULT_ROLE_MASK \
    ((1u << STELOG_ROLE_USER) | (1u << STELOG_ROLE_ASSISTANT) | \
     (1u << STELOG_ROLE_INTER_AGENT) | (1u << STELOG_ROLE_TOOL_CALL))

/* Bitmask for StelogTag flags (3 bits available in col_tag) */
enum {
    STELOG_FLAG_NONE        = 0,
    STELOG_FLAG_IS_SUBAGENT = 1u << 0,
    STELOG_FLAG_TRUNCATED   = 1u << 1,
    STELOG_FLAG_ARCHIVED    = 1u << 2
};

/* Packed 1-byte hot filter tag: [agent:2][role:3][flags:3] */
typedef uint8_t StelogTag;

static inline StelogTag stelog_tag_pack(StelogAgentKind agent, StelogRoleKind role, uint8_t flags) {
    return (StelogTag)((((uint8_t)agent & 0x3u) << 6) |
                       (((uint8_t)role  & 0x7u) << 3) |
                       ((uint8_t)flags  & 0x7u));
}

static inline StelogAgentKind stelog_tag_agent(StelogTag tag) {
    return (StelogAgentKind)((tag >> 6) & 0x3u);
}

static inline StelogRoleKind stelog_tag_role(StelogTag tag) {
    return (StelogRoleKind)((tag >> 3) & 0x7u);
}

static inline uint8_t stelog_tag_flags(StelogTag tag) {
    return (uint8_t)(tag & 0x7u);
}

/* Internal position of a document: Base (bit 32 = 0) or Delta (bit 32 = 1) plus the local row.
   It changes whenever a segment is rebuilt or compacted, so it never leaves the query engine; every
   doc_id handed to callers is the stable per-document key below. */
static inline uint64_t stelog_make_doc_id(bool is_delta, uint32_t local_id) {
    return (is_delta ? (1ull << 32) : 0ull) | (uint64_t)local_id;
}
static inline bool stelog_doc_is_delta(uint64_t global_id) {
    return (global_id >> 32) != 0;
}
static inline uint32_t stelog_doc_local_id(uint64_t global_id) {
    return (uint32_t)(global_id & 0xFFFFFFFFull);
}

/* Public doc_ids are 48-bit keys hashed from (session id, transcript file name, line byte offset, role,
   ordinal of that role within the line), so they survive rebuilds and Base/Delta compaction and stay
   far below 2^53 (IEEE-754 MAX_SAFE_INTEGER) for JSON/JS MCP hosts. */
#define STELOG_DOC_KEY_BITS 48u
#define STELOG_DOC_KEY_MASK ((1ull << STELOG_DOC_KEY_BITS) - 1ull)

/* Non-owning UTF-8 byte slice */
typedef struct {
    const char *ptr;
    uint32_t len;
} StelogStr;

/* Unified parsed event emitted by any adapter */
typedef struct {
    int64_t ts_ms;
    uint64_t raw_offset;
    uint32_t raw_len;
    uint32_t file_id;
    uint32_t seq_in_chunk;
    StelogTag tag;
    uint8_t _pad[3];

    StelogStr session_id;
    StelogStr project;
    StelogStr actor;
    StelogStr title;
    StelogStr msg_id;
    StelogStr parent_id;
    StelogStr text;
} StelogEvent;

/* Session-level metadata loaded from SQLite or session headers */
typedef struct {
    const char *session_id;
    const char *title;
    const char *cwd;
    const char *git_branch;
    const char *agent_name;
    const char *agent_role;
    const char *model;
    const char *parent_id;
    bool is_archived;
    bool is_subagent;
} StelogSessionMeta;

/* ============================================================================
 * Phase 1: Scan / Ingest Benchmark API
 * ============================================================================ */

typedef struct {
    uint64_t files_scanned;
    uint64_t compressed_files;
    uint64_t chunks_processed;
    uint64_t bytes_read;
    uint64_t bytes_skipped_prefilter;
    uint64_t lines_total;
    uint64_t lines_skipped_prefilter;
    uint64_t lines_parsed_simdjson;
    uint64_t parse_errors;
    uint64_t events_emitted;
    uint64_t text_bytes_emitted;
    uint64_t role_counts[STELOG_ROLE_COUNT];
} StelogAgentStats;

typedef struct {
    StelogAgentStats per_agent[STELOG_AGENT_COUNT];
    uint32_t worker_threads;
    double elapsed_ms;
    double discover_ms;
    double metadata_ms;
    double scan_ms;
} StelogScanReport;

typedef struct {
    const char *codex_home;
    const char *agy_home;
    const char *pi_home;
    const char *droid_home;
    uint32_t agent_mask;
    uint32_t threads;
    uint32_t chunk_bytes;
    uint32_t tool_call_max_bytes;
    bool include_tool_results;
    bool include_thinking;
    bool include_system;
    uint32_t sample_limit;
} StelogScanConfig;

typedef struct {
    StelogEvent *items;
    uint32_t count;
    uint32_t capacity;
    void *_arena;
} StelogSampleList;

const char *stelog_agent_name(StelogAgentKind kind);
const char *stelog_role_name(StelogRoleKind kind);

int stelog_run_scan(const StelogScanConfig *cfg,
                    StelogScanReport *out_report,
                    StelogSampleList *out_samples);

void stelog_sample_list_free(StelogSampleList *list);

/* ============================================================================
 * Phase 2: DOD Index Builder, mmap Reader & Zero-Alloc Query Engine API
 * ============================================================================ */

typedef struct {
    const char *index_dir;          /* default: $STELOG_INDEX_DIR or ~/.cache/stelog/index.v2 */
    const char *codex_home;
    const char *agy_home;
    const char *pi_home;
    const char *droid_home;
    uint32_t agent_mask;            /* 0 = all */
    uint32_t threads;               /* 0 = auto P-cores */
    uint32_t chunk_bytes;           /* 0 = 8 MiB */
    uint32_t tool_call_max_bytes;   /* 0 = 512 bytes */
    uint32_t delta_compact_threshold; /* 0 = default 16384 docs */
    bool include_tool_results;
    bool include_thinking;
    bool include_system;
    bool force_rebuild;
    bool force_compact;
} StelogIndexConfig;

typedef struct {
    uint32_t base_gen;
    uint32_t delta_gen;
    uint32_t base_docs;
    uint32_t delta_docs;
    uint32_t new_docs_added;
    uint32_t files_total;
    uint32_t files_changed;
    uint32_t unique_terms_base;
    uint32_t unique_terms_delta;
    uint64_t bytes_on_disk;
    bool was_rebuild;
    bool did_compact;
    bool noop_up_to_date;
    double total_ms;
    double discover_ms;
    double ingest_ms;
    double build_ms;
} StelogIndexReport;

/* Resolve default index directory into `out` */
void stelog_resolve_index_dir(const char *override_dir, char *out, size_t cap);

/* Build or incrementally refresh the DOD Base+Delta index. */
int stelog_index_sync(const StelogIndexConfig *cfg, StelogIndexReport *out_report);

/* Opaque zero-copy mmap index handle (holds Base + Delta segments + QueryScratchArena) */
typedef struct StelogIndex StelogIndex;

StelogIndex *stelog_index_open(const char *index_dir);
void stelog_index_close(StelogIndex *idx);
/* Reloads mmap views if current.meta changed (< 0.1 ms check). Returns 1 if reloaded, 0 if unchanged, -1 on error. */
int stelog_index_reload_if_changed(StelogIndex *idx);

/* Optional content an index was built with (sticky across incremental syncs) */
enum {
    STELOG_INDEX_HAS_TOOL_RESULTS = 1u << 0,
    STELOG_INDEX_HAS_THINKING     = 1u << 1,
    STELOG_INDEX_HAS_SYSTEM       = 1u << 2
};

typedef struct {
    uint32_t base_gen;
    uint32_t delta_gen;
    uint32_t base_docs;
    uint32_t delta_docs;
    uint32_t content_flags;   /* STELOG_INDEX_HAS_* */
    uint32_t base_sessions;
    uint32_t delta_sessions;
    uint32_t base_terms;
    uint32_t delta_terms;
    uint64_t base_text_bytes;
    uint64_t delta_text_bytes;
    uint64_t total_disk_bytes;
    int64_t  updated_at_ms;
} StelogIndexStats;

void stelog_index_get_stats(const StelogIndex *idx, StelogIndexStats *out);

/* Diagnostic: total bytes allocated by the index's reusable QueryScratchArena (used to verify 0-malloc hot queries). */
size_t stelog_index_scratch_allocated_bytes(const StelogIndex *idx);

typedef enum {
    STELOG_CASE_SMART       = 0, /* Default: case-sensitive if query contains any ASCII uppercase */
    STELOG_CASE_INSENSITIVE = 1, /* -i / --ignore-case */
    STELOG_CASE_SENSITIVE   = 2  /* -s / --case-sensitive */
} StelogCaseMode;

typedef enum {
    STELOG_SORT_RELEVANCE = 0,
    STELOG_SORT_OLDEST    = 1,
    STELOG_SORT_NEWEST    = 2
} StelogSortMode;

typedef enum {
    STELOG_COUNT_BY_NONE    = 0,
    STELOG_COUNT_BY_AGENT   = 1,
    STELOG_COUNT_BY_ROLE    = 2,
    STELOG_COUNT_BY_PROJECT = 3,
    STELOG_COUNT_BY_ACTOR   = 4,
    STELOG_COUNT_BY_SESSION = 5
} StelogCountByAxis;

typedef struct {
    StelogStr key;
    uint32_t  count;
    /* Optional metadata populated for STELOG_COUNT_BY_SESSION */
    StelogStr meta_title;
    StelogStr meta_project;
    int64_t   meta_last_ts_ms;
    StelogAgentKind meta_agent;
} StelogCountBucket;

typedef struct {
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogAgentKind agent;
    int64_t  first_hit_ts_ms;
    int64_t  last_hit_ts_ms;
    uint32_t hit_count;
    uint64_t best_doc_id;
    StelogStr best_snippet;
    float    best_score;
    float    rank_score;  /* best_score boosted by log2(1 + hit_count); used for relevance ordering */
} StelogSessionHit;

typedef struct {
    const char *query;          /* NULL or empty = filter-only scan; supports `|` or ` OR ` branches */
    uint32_t agent_mask;        /* 0 = all */
    uint32_t role_mask;         /* 0 = STELOG_DEFAULT_ROLE_MASK */
    uint32_t exclude_role_mask; /* -T: exclude these roles */
    const char *project_substr; /* case-insensitive substring match on project/cwd */
    const char *actor_substr;   /* case-insensitive substring match on actor */
    const char *session_id;     /* exact or prefix match on session_id (@prefix) */
    int64_t since_ms;           /* 0 = unbounded */
    int64_t until_ms;           /* 0 = unbounded */
    uint32_t limit;             /* 0 = default 20 */
    uint32_t offset;            /* pagination offset */
    uint32_t snippet_max_bytes; /* 0 = default 600 */
    bool sort_by_time;          /* shorthand for STELOG_SORT_OLDEST */
    StelogSortMode sort_mode;   /* relevance / oldest / newest */
    bool no_subagents;          /* --no-subagents: exclude FLAG_IS_SUBAGENT */
    bool no_dedup;              /* --no-dedup: keep exact duplicate texts across forks/retries */
    bool group_by_session;      /* --group-by session: collapse hits per session with title + best snippet */
    bool sessions_only;         /* -l: populate unique matched_sessions */
    StelogCountByAxis count_by; /* --count-by: populate histogram buckets */
    StelogCaseMode case_mode;   /* -S (smart, default) / -i (ignore-case) / -s (case-sensitive) */
    bool fixed_strings;         /* -F: literal fixed-string search (no syntax/wildcard parsing) */
    bool regex_mode;            /* -E / -P: PCRE2 JIT regular expression search */
    bool existence_only;        /* Return total_matches as 0 or 1, without materializing hits. */
} StelogQuery;

typedef struct {
    uint64_t doc_id;
    int64_t  ts_ms;
    float    score;
    StelogTag tag;
    bool     truncated;
    uint32_t full_text_len;
    uint32_t dup_count;              /* folded exact-duplicate count (Codex forks / retries) */
    uint32_t collapsed_tools_before; /* intermediate tool_calls collapsed in context/digest */
    uint64_t raw_offset;
    uint32_t raw_len;
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogStr actor;
    StelogStr file_path;
    StelogStr snippet;
} StelogHit;

/* Note: slices inside StelogSearchResult are backed by StelogIndex's QueryScratchArena
   (valid until the next query on the same StelogIndex handle; zero malloc!). */
typedef struct {
    StelogHit *hits;
    uint32_t count;
    uint32_t total_matches;
    uint32_t duplicates_folded;
    uint32_t docs_in_scope;
    bool truncated;
    double elapsed_ms;

    /* Definitive Absence diagnostics (populated when total_matches == 0 & query non-empty) */
    uint32_t unfiltered_total_matches;
    uint32_t other_role_counts[STELOG_ROLE_COUNT];

    /* Optional --count-by histogram */
    StelogCountBucket *buckets;
    uint32_t bucket_count;

    /* Optional --group-by session results */
    StelogSessionHit *session_groups;
    uint32_t session_group_count;   /* groups on this page */
    uint32_t session_group_total;   /* groups before offset/limit */

    /* Optional -l unique matching sessions */
    StelogStr *matched_sessions;
    uint32_t matched_session_count;

    /* Populated on query syntax error (e.g. invalid -E PCRE2 regex pattern) */
    StelogStr error_msg;
} StelogSearchResult;

int stelog_index_search(StelogIndex *idx, const StelogQuery *q, StelogSearchResult *out);

typedef struct {
    uint64_t doc_id;
    int64_t  ts_ms;
    StelogTag tag;
    StelogStr file_touched;
    StelogStr tool_name;
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogStr actor;
} StelogFileTouch;

typedef struct {
    StelogFileTouch *items;
    uint32_t count;
    uint32_t total_matches;
    bool truncated;
    double elapsed_ms;
} StelogFileHistoryResult;

/* Find which sessions, when, and who modified/wrote files matching `file_substr` (NULL = all file writes). */
int stelog_index_files(StelogIndex *idx,
                       const char *file_substr,
                       uint32_t agent_mask,
                       const char *project_substr,
                       const char *session_prefix,
                       int64_t since_ms,
                       int64_t until_ms,
                       bool no_subagents,
                       uint32_t limit,
                       uint32_t offset,
                       StelogFileHistoryResult *out);

/* MCP stdio server entry point (implemented in src/mcp/mcp_server.c) */
int stelog_mcp_serve(const char *index_dir);

typedef struct {
    StelogHit *items;
    uint32_t count;
    uint32_t anchor_index;
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    double elapsed_ms;
} StelogContextResult;

int stelog_index_context(StelogIndex *idx,
                         uint64_t anchor_doc_id,
                         uint32_t window,
                         uint32_t max_bytes_per_item,
                         bool include_tools,
                         StelogContextResult *out);

typedef struct {
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogStr git_branch;
    StelogStr parent_session_id;
    int64_t  first_ts_ms;
    int64_t  last_ts_ms;
    uint32_t doc_count;
    StelogAgentKind agent;
    uint8_t flags;
} StelogSessionSummary;

#define STELOG_MAX_SESSION_CANDIDATES 8u

typedef struct {
    StelogHit *items;
    uint32_t count;
    uint32_t total_dialogue_turns;
    uint32_t total_tools_collapsed;
    uint32_t duplicates_skipped;
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogAgentKind agent;
    bool truncated;
    double elapsed_ms;

    /* Coverage: items[0] is dialogue turn #turn_offset. When omitted_count > 0, turns
       [omitted_begin, omitted_begin + omitted_count) are not shown; items[gap_item_index]
       (if < count) is the first item after that gap. next_offset is the turn_offset that
       continues reading unread turns (0 when nothing was omitted). */
    uint32_t turn_offset;
    uint32_t gap_item_index;
    uint32_t omitted_begin;
    uint32_t omitted_count;
    uint32_t next_offset;
    int64_t  first_ts_ms;
    int64_t  last_ts_ms;

    /* Session resolution: match_count distinct sessions matched the prefix; when > 1 the call
       returns -2 and the most recent candidates are listed here. */
    uint32_t match_count;
    uint32_t candidate_count;
    StelogSessionSummary candidates[STELOG_MAX_SESSION_CANDIDATES];
} StelogDigestResult;

typedef struct {
    const char *session;       /* full session id or (@)prefix */
    uint32_t max_total_bytes;  /* 0 = 7200 */
    uint32_t turn_offset;      /* first dialogue turn to show; 0 enables head+tail sampling when over budget */
    bool include_tools;
} StelogDigestOptions;

/* Condensed chronological user<->assistant decision stream for a session (@prefix or full id).
   Collapses intermediate tool_calls to counts, deduplicates consecutive retries, and uses
   tail-biased truncation on long assistant replies so ending options/conclusions stay intact.
   When the session exceeds the budget, the opening and closing turns are kept and the middle
   is reported as an omitted range that can be paged with turn_offset.
   Returns 0 on success, -1 when no session matches, -2 when the prefix is ambiguous. */
int stelog_index_digest_ex(StelogIndex *idx, const StelogDigestOptions *opts, StelogDigestResult *out);

int stelog_index_digest(StelogIndex *idx,
                        const char *session_prefix,
                        uint32_t max_total_bytes,
                        bool include_tools,
                        StelogDigestResult *out);

/* Resolve a full session id or (@)prefix, case-insensitively. An exact full-id match wins over
   longer ids that share it as a prefix. Returns the number of distinct matching sessions and writes
   up to cap of them (most recent first) to out. Slices stay valid until the index reloads. */
uint32_t stelog_index_resolve_session(const StelogIndex *idx,
                                      const char *id_or_prefix,
                                      StelogSessionSummary *out,
                                      uint32_t cap);

/* Shortest prefix length (at least min_len) that identifies session_id uniquely in the index. */
uint32_t stelog_index_unique_prefix_len(const StelogIndex *idx, StelogStr session_id, uint32_t min_len);

typedef struct {
    uint64_t doc_id;
    int64_t  ts_ms;
    StelogTag tag;
    StelogStr session_id;
    StelogStr title;
    StelogStr project;
    StelogStr actor;
    StelogStr file_path;
    uint64_t raw_offset;
    uint32_t raw_len;
    StelogStr content;      /* slice backed by scratch arena */
    uint32_t total_bytes;
    uint32_t returned_offset;
    bool truncated;
    double elapsed_ms;
} StelogDocResult;

int stelog_index_get_document(StelogIndex *idx,
                              uint64_t doc_id,
                              bool raw_jsonl,
                              uint32_t byte_offset,
                              uint32_t max_bytes,
                              bool tail_mode,
                              StelogDocResult *out);

/* Maps a public doc_id to its internal Base/Delta position (stelog_make_doc_id form). */
bool stelog_index_resolve_doc_id(const StelogIndex *idx, uint64_t doc_id, uint64_t *out_internal_id);

typedef struct {
    StelogSessionSummary *items;
    uint32_t count;
    uint32_t total_matches;
    double elapsed_ms;
} StelogSessionListResult;

int stelog_index_list_sessions(StelogIndex *idx,
                               uint32_t agent_mask,
                               const char *project_substr,
                               int64_t since_ms,
                               int64_t until_ms,
                               uint32_t limit,
                               uint32_t offset,
                               StelogSessionListResult *out);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_H */
