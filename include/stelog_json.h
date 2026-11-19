#ifndef STELOG_JSON_H
#define STELOG_JSON_H

#include "stelog.h"
#include "base/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque thread-local wrapper around simdjson::ondemand::parser */
typedef struct StelogJsonParser StelogJsonParser;

StelogJsonParser *stelog_json_parser_create(void);
void stelog_json_parser_destroy(StelogJsonParser *parser);

/* Mutable per-chunk context updated by turn_context / session_meta / model_change */
typedef struct {
    StelogStr session_id;
    StelogStr cwd;
    StelogStr actor;
    StelogStr parent_session_id;
    StelogStr title;          /* session title when the transcript carries one (Droid session_start) */
    uint8_t session_flags;
} StelogStreamContext;

typedef struct {
    uint32_t tool_call_max_bytes;
    bool include_tool_results;
    bool include_thinking;
    bool include_system;
} StelogParseOptions;

/* Up to 3 logical events may be produced from a single JSONL line (e.g. AGY/Pi assistant with text + tool_calls + thinking) */
#define STELOG_MAX_LINE_EVENTS 4

typedef struct {
    StelogEvent events[STELOG_MAX_LINE_EVENTS];
    uint32_t count;
    bool updated_context;
} StelogLineResult;

/*
 * Parse a single JSONL line using simdjson On-Demand API.
 * `padded_cap` is the total readable capacity starting at `line` (>= line_len).
 * If padded_cap >= line_len + 64 (SIMDJSON_PADDING), simdjson parses in-place with ZERO copy.
 * Otherwise the bridge copies into its internal reusable padded scratch buffer.
 * Strings inside `out->events` are allocated in `arena`.
 * Returns 0 on success (even if 0 events emitted), non-zero on JSON parse error.
 */
int stelog_json_parse_codex_line(StelogJsonParser *parser,
                                 const char *line,
                                 size_t line_len,
                                 size_t padded_cap,
                                 uint32_t file_id,
                                 uint64_t raw_offset,
                                 const StelogParseOptions *opts,
                                 StelogStreamContext *ctx,
                                 StelogArena *arena,
                                 StelogLineResult *out);

int stelog_json_parse_agy_line(StelogJsonParser *parser,
                               const char *line,
                               size_t line_len,
                               size_t padded_cap,
                               uint32_t file_id,
                               uint64_t raw_offset,
                               const StelogParseOptions *opts,
                               StelogStreamContext *ctx,
                               StelogArena *arena,
                               StelogLineResult *out);

int stelog_json_parse_pi_line(StelogJsonParser *parser,
                              const char *line,
                              size_t line_len,
                              size_t padded_cap,
                              uint32_t file_id,
                              uint64_t raw_offset,
                              const StelogParseOptions *opts,
                              StelogStreamContext *ctx,
                              StelogArena *arena,
                              StelogLineResult *out);

int stelog_json_parse_droid_line(StelogJsonParser *parser,
                                 const char *line,
                                 size_t line_len,
                                 size_t padded_cap,
                                 uint32_t file_id,
                                 uint64_t raw_offset,
                                 const StelogParseOptions *opts,
                                 StelogStreamContext *ctx,
                                 StelogArena *arena,
                                 StelogLineResult *out);

/* ============================================================================
 * MCP JSON-RPC 2.0 Request Parser (powered by simdjson On-Demand)
 * ============================================================================ */

typedef enum {
    STELOG_RPC_ID_NONE = 0,
    STELOG_RPC_ID_INT  = 1,
    STELOG_RPC_ID_STR  = 2,
    STELOG_RPC_ID_NULL = 3
} StelogRpcIdKind;

#define STELOG_MCP_MAX_BATCH_DOCS 16

typedef struct {
    StelogRpcIdKind id_kind;
    int64_t id_int;
    StelogStr id_str;
    StelogStr method;
    StelogStr tool_name;

    /* Tool call arguments (all optional, allocated in arena) */
    StelogStr arg_query;
    StelogStr arg_agent;
    StelogStr arg_project;
    StelogStr arg_role;
    StelogStr arg_exclude_role;
    StelogStr arg_actor;
    StelogStr arg_session;
    StelogStr arg_since;
    StelogStr arg_until;
    StelogStr arg_sort;
    StelogStr arg_count_by;
    StelogStr arg_group_by;
    StelogStr arg_file;
    StelogStr arg_case;
    StelogStr protocol_version;   /* initialize: params.protocolVersion */
    StelogStr client_name;        /* initialize: params.clientInfo.name */

    uint64_t arg_doc_id;
    bool     has_doc_id;
    uint64_t arg_doc_ids[STELOG_MCP_MAX_BATCH_DOCS];
    uint32_t arg_doc_id_count;
    uint32_t arg_limit;
    uint32_t arg_offset;
    uint32_t arg_window;
    uint32_t arg_max_bytes;
    bool     arg_include_tools;
    bool     arg_include_thinking;
    bool     arg_no_subagents;
    bool     arg_no_dedup;
    bool     arg_fixed;
    bool     arg_regex;
    bool     arg_raw;
    bool     arg_tail;
} StelogMcpRequest;

int stelog_json_parse_mcp_request(StelogJsonParser *parser,
                                  const char *line,
                                  size_t line_len,
                                  size_t padded_cap,
                                  StelogArena *arena,
                                  StelogMcpRequest *out);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_JSON_H */
