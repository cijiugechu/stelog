#define _POSIX_C_SOURCE 200809L
#include "stelog.h"
#include "stelog_json.h"
#include "base/arena.h"
#include "base/simd.h"
#include "base/time_iso8601.h"
#include "json/json_writer.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MCP_MAX_TEXT_BYTES    32768u
#define MCP_INLINE_SOFT_BYTES 7600u
#define MCP_MAX_TEXT_LINES    500u
#define MCP_SYNC_COOLDOWN_MS  2000.0

/* ============================================================================
 * Bounded Text Builder with Codex & Pi Dual-Budget Guard (32 KB / 500 lines)
 * ============================================================================ */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    uint32_t lines;
    uint32_t max_bytes;
    uint32_t max_lines;
    bool truncated;
    const char *truncated_by;
} BoundedText;

static void bt_init(BoundedText *bt, uint32_t max_bytes, uint32_t max_lines) {
    bt->max_bytes = max_bytes ? max_bytes : MCP_MAX_TEXT_BYTES;
    bt->max_lines = max_lines ? max_lines : MCP_MAX_TEXT_LINES;
    bt->cap = bt->max_bytes + 1024u;
    bt->buf = (char *)malloc(bt->cap);
    bt->len = 0;
    bt->lines = 0;
    bt->truncated = false;
    bt->truncated_by = NULL;
    if (bt->buf) bt->buf[0] = '\0';
}

static void bt_destroy(BoundedText *bt) {
    free(bt->buf);
    memset(bt, 0, sizeof(*bt));
}

static bool bt_can_append(BoundedText *bt, size_t extra_bytes, uint32_t extra_lines) {
    if (bt->truncated) return false;
    if (bt->len + extra_bytes > bt->max_bytes) {
        bt->truncated = true;
        bt->truncated_by = "bytes";
        return false;
    }
    if (bt->lines + extra_lines > bt->max_lines) {
        bt->truncated = true;
        bt->truncated_by = "lines";
        return false;
    }
    return true;
}

static void bt_append_raw(BoundedText *bt, const char *s, size_t n) {
    if (!bt->buf || n == 0) return;
    if (bt->len + n + 1 > bt->cap) {
        size_t ncap = bt->cap * 2 + n;
        char *nb = (char *)realloc(bt->buf, ncap);
        if (!nb) return;
        bt->buf = nb;
        bt->cap = ncap;
    }
    memcpy(bt->buf + bt->len, s, n);
    bt->len += n;
    bt->buf[bt->len] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == '\n') bt->lines++;
    }
}

static void bt_printf_force(BoundedText *bt, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t use = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
        bt_append_raw(bt, tmp, use);
    }
}

static void bt_append_sanitized(BoundedText *bt, const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        bt_append_raw(bt, &c, 1);
    }
}

/* ============================================================================
 * Filter Parsing Helpers (shared semantics with CLI)
 * ============================================================================ */

/* Local time with an explicit UTC offset ("2026-11-19 21:22:11+08:00") so agents can compare it against
   other clocks, and so it can be pasted back into since/until unchanged. */
static void fmt_ts_iso(int64_t ts_ms, char *out, size_t cap) {
    if (ts_ms <= 0) { snprintf(out, cap, "unknown"); return; }
    time_t sec = (time_t)(ts_ms / 1000ll);
    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);
    char base[24], zone[8];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm_buf);
    if (strftime(zone, sizeof(zone), "%z", &tm_buf) == 5) {
        snprintf(out, cap, "%s%.3s:%.2s", base, zone, zone + 3);
    } else {
        snprintf(out, cap, "%s", base);
    }
}

/* RFC 3339 form of fmt_ts_iso for structuredContent ("2026-11-19T21:22:11+08:00"). */
static void fmt_ts_rfc3339(int64_t ts_ms, char *out, size_t cap) {
    fmt_ts_iso(ts_ms, out, cap);
    if (strlen(out) > 10 && out[10] == ' ') out[10] = 'T';
}

/* Writes a user-facing message into err and returns false when since/until cannot be parsed. */
static bool parse_time_range(const StelogMcpRequest *req, int64_t *since_ms, int64_t *until_ms, char *err, size_t err_cap) {
    *since_ms = 0;
    *until_ms = 0;
    const StelogStr *vals[2] = { &req->arg_since, &req->arg_until };
    int64_t *outs[2] = { since_ms, until_ms };
    static const char *names[2] = { "since", "until" };
    for (int i = 0; i < 2; ++i) {
        const StelogStr *v = vals[i];
        if (!v->ptr || v->len == 0) continue;
        if (!stelog_parse_time_arg(v->ptr, v->len, i == 1, outs[i])) {
            snprintf(err, err_cap,
                     "Invalid %s=\"%.*s\". Use a relative offset (30m, 24h, 7d, 2w), today, yesterday, "
                     "YYYY-MM-DD, or YYYY-MM-DDTHH:MM[:SS] (local time unless Z or +HH:MM is given).",
                     names[i], (int)(v->len > 64 ? 64 : v->len), v->ptr);
            return false;
        }
    }
    if (*since_ms && *until_ms && *since_ms > *until_ms) {
        snprintf(err, err_cap, "Invalid time range: since (%.*s) is after until (%.*s).",
                 (int)(req->arg_since.len > 64 ? 64 : req->arg_since.len), req->arg_since.ptr,
                 (int)(req->arg_until.len > 64 ? 64 : req->arg_until.len), req->arg_until.ptr);
        return false;
    }
    return true;
}

static uint32_t parse_agent_csv(StelogStr s) {
    if (!s.ptr || s.len == 0) return 0;
    uint32_t mask = 0;
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        const char *c = (const char *)memchr(p, ',', (size_t)(end - p));
        size_t l = c ? (size_t)(c - p) : (size_t)(end - p);
        while (l > 0 && *p == ' ') { p++; l--; }
        if (l == 5 && memcmp(p, "codex", 5) == 0) mask |= (1u << STELOG_AGENT_CODEX);
        else if (l == 3 && memcmp(p, "agy", 3) == 0) mask |= (1u << STELOG_AGENT_AGY);
        else if (l == 2 && memcmp(p, "pi", 2) == 0) mask |= (1u << STELOG_AGENT_PI);
        else if (l == 5 && memcmp(p, "droid", 5) == 0) mask |= (1u << STELOG_AGENT_DROID);
        if (!c) break;
        p = c + 1;
    }
    return mask;
}

static uint32_t parse_role_csv(StelogStr s) {
    if (!s.ptr || s.len == 0) return 0;
    uint32_t mask = 0;
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        const char *c = (const char *)memchr(p, ',', (size_t)(end - p));
        size_t l = c ? (size_t)(c - p) : (size_t)(end - p);
        while (l > 0 && *p == ' ') { p++; l--; }
        if (l == 4 && memcmp(p, "user", 4) == 0) mask |= (1u << STELOG_ROLE_USER);
        else if (l == 9 && memcmp(p, "assistant", 9) == 0) mask |= (1u << STELOG_ROLE_ASSISTANT);
        else if (l == 11 && memcmp(p, "inter_agent", 11) == 0) mask |= (1u << STELOG_ROLE_INTER_AGENT);
        else if (l == 9 && memcmp(p, "tool_call", 9) == 0) mask |= (1u << STELOG_ROLE_TOOL_CALL);
        else if (l == 11 && memcmp(p, "tool_result", 11) == 0) mask |= (1u << STELOG_ROLE_TOOL_RESULT);
        else if (l == 8 && memcmp(p, "thinking", 8) == 0) mask |= (1u << STELOG_ROLE_THINKING);
        else if (l == 6 && memcmp(p, "system", 6) == 0) mask |= (1u << STELOG_ROLE_SYSTEM);
        if (!c) break;
        p = c + 1;
    }
    return mask;
}

static StelogCaseMode parse_case_mode(StelogStr s) {
    if (!s.ptr || s.len == 0) return STELOG_CASE_SMART;
    if ((s.len == 11 && memcmp(s.ptr, "insensitive", 11) == 0) ||
        (s.len == 6  && memcmp(s.ptr, "ignore", 6) == 0) ||
        (s.len == 1  && s.ptr[0] == 'i')) {
        return STELOG_CASE_INSENSITIVE;
    }
    if ((s.len == 9 && memcmp(s.ptr, "sensitive", 9) == 0) ||
        (s.len == 1 && s.ptr[0] == 's')) {
        return STELOG_CASE_SENSITIVE;
    }
    return STELOG_CASE_SMART;
}

static StelogCountByAxis parse_count_by(StelogStr s) {
    if (!s.ptr || s.len == 0) return STELOG_COUNT_BY_NONE;
    if (s.len == 5 && memcmp(s.ptr, "agent", 5) == 0) return STELOG_COUNT_BY_AGENT;
    if (s.len == 4 && memcmp(s.ptr, "role", 4) == 0) return STELOG_COUNT_BY_ROLE;
    if (s.len == 7 && memcmp(s.ptr, "project", 7) == 0) return STELOG_COUNT_BY_PROJECT;
    if (s.len == 5 && memcmp(s.ptr, "actor", 5) == 0) return STELOG_COUNT_BY_ACTOR;
    if (s.len == 7 && memcmp(s.ptr, "session", 7) == 0) return STELOG_COUNT_BY_SESSION;
    return STELOG_COUNT_BY_NONE;
}

static uint32_t short_sid_len(const StelogIndex *idx, StelogStr sid) {
    return stelog_index_unique_prefix_len(idx, sid, 8u);
}

/* ============================================================================
 * structuredContent Builder (shapes declared as outputSchema in tools_list.json)
 * ============================================================================ */

/* Off for clients that would show structuredContent to the model in place of the text result (Codex). */
static bool g_structured_output = true;
static StelogJsonWriter g_sc;    /* structuredContent object of the current tool result */
static StelogJsonWriter g_notes; /* items of its "notes" array */

static StelogJsonWriter *sc_begin(void) {
    stelog_jw_reset(&g_sc);
    stelog_jw_reset(&g_notes);
    stelog_jw_obj_begin(&g_sc);
    return &g_sc;
}

static void sc_kv_s(StelogJsonWriter *sc, const char *key, StelogStr s) {
    stelog_jw_kv_str(sc, key, s.ptr ? s.ptr : "", s.ptr ? s.len : 0);
}

static void sc_kv_ts(StelogJsonWriter *sc, const char *key, int64_t ts_ms) {
    char b[48];
    fmt_ts_rfc3339(ts_ms, b, sizeof(b));
    stelog_jw_kv_cstr(sc, key, b);
}

static void sc_kv_session(StelogJsonWriter *sc, const char *key, const StelogIndex *idx, StelogStr sid) {
    char b[80];
    uint32_t n = stelog_index_unique_prefix_len(idx, sid, 8u);
    if (n > sid.len) n = sid.len;
    if (n > sizeof(b) - 2) n = (uint32_t)sizeof(b) - 2;
    b[0] = '@';
    if (n > 0) memcpy(b + 1, sid.ptr, n);
    stelog_jw_kv_str(sc, key, b, (size_t)n + 1);
}

/* Appends a "Note:"/"Hint:" line to the text result and mirrors it into structuredContent.notes. */
static void bt_note(BoundedText *bt, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    size_t use = (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1;
    bt_append_raw(bt, tmp, use);
    while (use > 0 && tmp[use - 1] == '\n') use--;
    stelog_jw_str(&g_notes, tmp, use);
}

static void sc_finish(StelogJsonWriter *sc) {
    if (g_notes.len > 0) {
        stelog_jw_key(sc, "notes");
        stelog_jw_arr_begin(sc);
        stelog_jw_raw(sc, g_notes.buf, g_notes.len);
        stelog_jw_arr_end(sc);
    }
    stelog_jw_obj_end(sc);
}

static bool query_has_upper(StelogStr q) {
    for (uint32_t i = 0; i < q.len; ++i) {
        if (q.ptr[i] >= 'A' && q.ptr[i] <= 'Z') return true;
    }
    return false;
}
/* ============================================================================
 * JSON-RPC 2.0 Envelope Helpers
 * ============================================================================ */

static void write_rpc_id(StelogJsonWriter *w, const StelogMcpRequest *req) {
    stelog_jw_key(w, "id");
    switch (req->id_kind) {
        case STELOG_RPC_ID_INT:  stelog_jw_int(w, req->id_int); break;
        case STELOG_RPC_ID_STR:  stelog_jw_str(w, req->id_str.ptr, req->id_str.len); break;
        default:                 stelog_jw_null(w); break;
    }
}

/* sc (already finished with sc_finish) is attached as structuredContent when the client wants it. */
static void send_rpc_tool_result(StelogJsonWriter *w, const StelogMcpRequest *req, const char *text, size_t len,
                                 bool is_error, const StelogJsonWriter *sc) {
    stelog_jw_reset(w);
    stelog_jw_obj_begin(w);
    stelog_jw_kv_cstr(w, "jsonrpc", "2.0");
    write_rpc_id(w, req);
    stelog_jw_key(w, "result");
    stelog_jw_obj_begin(w);
    stelog_jw_key(w, "content");
    stelog_jw_arr_begin(w);
    stelog_jw_obj_begin(w);
    stelog_jw_kv_cstr(w, "type", "text");
    stelog_jw_kv_str(w, "text", text, len);
    stelog_jw_obj_end(w);
    stelog_jw_arr_end(w);
    if (sc && g_structured_output) {
        stelog_jw_key(w, "structuredContent");
        stelog_jw_raw(w, sc->buf, sc->len);
    }
    if (is_error) stelog_jw_kv_bool(w, "isError", true);
    stelog_jw_obj_end(w);
    stelog_jw_obj_end(w);
    fwrite(w->buf, 1, w->len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_rpc_tool_text(StelogJsonWriter *w, const StelogMcpRequest *req, const char *text, size_t len, bool is_error) {
    send_rpc_tool_result(w, req, text, len, is_error, NULL);
}

static void send_rpc_tool_sc(StelogJsonWriter *w, const StelogMcpRequest *req, const BoundedText *bt, StelogJsonWriter *sc) {
    sc_finish(sc);
    send_rpc_tool_result(w, req, bt->buf, bt->len, false, sc);
}

static void send_rpc_error(StelogJsonWriter *w, const StelogMcpRequest *req, int code, const char *msg) {
    stelog_jw_reset(w);
    stelog_jw_obj_begin(w);
    stelog_jw_kv_cstr(w, "jsonrpc", "2.0");
    write_rpc_id(w, req);
    stelog_jw_key(w, "error");
    stelog_jw_obj_begin(w);
    stelog_jw_kv_int(w, "code", code);
    stelog_jw_kv_cstr(w, "message", msg);
    stelog_jw_obj_end(w);
    stelog_jw_obj_end(w);
    fwrite(w->buf, 1, w->len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/* ============================================================================
 * Embedded MCP Static JSON Payloads (src/mcp/{initialize,tools_list}.json)
 * Minified & validated at compile time via src/mcp/mcp_schemas.zig
 * ============================================================================ */

extern const char stelog_mcp_initialize_json[];
extern const size_t stelog_mcp_initialize_json_len;
extern const char stelog_mcp_tools_list_json[];
extern const size_t stelog_mcp_tools_list_json_len;
extern const char stelog_mcp_tools_list_plain_json[];
extern const size_t stelog_mcp_tools_list_plain_json_len;

/* Codex hands structuredContent to the model instead of the text content, which would hide the
   formatted results and next-step hints, so it gets plain text results. */
static bool client_wants_structured(StelogStr client_name) {
    return !(client_name.len > 0 && stelog_memcasemem(client_name.ptr, client_name.len, "codex", 5) != NULL);
}

static const char *const kSupportedProtocolVersions[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

/* Echo the client's protocolVersion when supported, otherwise offer the newest one we speak. */
static void write_initialize_result(StelogJsonWriter *w, StelogStr requested) {
    const char *ver = kSupportedProtocolVersions[0];
    for (size_t i = 0; i < sizeof(kSupportedProtocolVersions) / sizeof(kSupportedProtocolVersions[0]); ++i) {
        size_t l = strlen(kSupportedProtocolVersions[i]);
        if (requested.len == l && memcmp(requested.ptr, kSupportedProtocolVersions[i], l) == 0) {
            ver = kSupportedProtocolVersions[i];
            break;
        }
    }
    /* initialize.json is a minified object without protocolVersion; splice it in as the first key */
    const char *body = stelog_mcp_initialize_json;
    size_t body_len = stelog_mcp_initialize_json_len;
    char *tmp = (char *)malloc(body_len + 64u);
    if (!tmp) {
        stelog_jw_raw(w, body, body_len);
        return;
    }
    int n = snprintf(tmp, 64u, "{\"protocolVersion\":\"%s\",", ver);
    memcpy(tmp + n, body + 1, body_len - 1);
    stelog_jw_raw(w, tmp, (size_t)n + body_len - 1);
    free(tmp);
}
/* ============================================================================
 * MCP Tool Call Handlers
 * ============================================================================ */

static void handle_search_tool(StelogIndex *idx, const StelogMcpRequest *req, bool is_timeline, StelogJsonWriter *w) {
    StelogQuery q;
    memset(&q, 0, sizeof(q));
    q.query             = req->arg_query.len ? req->arg_query.ptr : NULL;
    q.agent_mask        = parse_agent_csv(req->arg_agent);
    q.role_mask         = parse_role_csv(req->arg_role);
    q.exclude_role_mask = parse_role_csv(req->arg_exclude_role);
    q.project_substr    = req->arg_project.len ? req->arg_project.ptr : NULL;
    q.actor_substr      = req->arg_actor.len ? req->arg_actor.ptr : NULL;
    q.session_id        = req->arg_session.len ? req->arg_session.ptr : NULL;
    char time_err[320];
    if (!parse_time_range(req, &q.since_ms, &q.until_ms, time_err, sizeof(time_err))) {
        send_rpc_tool_text(w, req, time_err, strlen(time_err), true);
        return;
    }
    q.limit             = req->arg_limit ? req->arg_limit : (is_timeline ? 10u : 15u);
    q.offset            = req->arg_offset;
    q.snippet_max_bytes = is_timeline ? 320u : 420u;
    q.no_subagents      = req->arg_no_subagents;
    q.no_dedup          = req->arg_no_dedup;
    q.group_by_session  = (req->arg_group_by.len == 7 && memcmp(req->arg_group_by.ptr, "session", 7) == 0);
    q.count_by          = parse_count_by(req->arg_count_by);
    q.case_mode         = parse_case_mode(req->arg_case);
    q.fixed_strings     = req->arg_fixed;
    q.regex_mode        = req->arg_regex;

    if (req->arg_include_tools)    q.role_mask = (q.role_mask ? q.role_mask : STELOG_DEFAULT_ROLE_MASK) | (1u << STELOG_ROLE_TOOL_RESULT);
    if (req->arg_include_thinking) q.role_mask = (q.role_mask ? q.role_mask : STELOG_DEFAULT_ROLE_MASK) | (1u << STELOG_ROLE_THINKING);

    if (is_timeline) {
        q.sort_mode = STELOG_SORT_OLDEST;
    } else if (req->arg_sort.len == 6 && memcmp(req->arg_sort.ptr, "oldest", 6) == 0) {
        q.sort_mode = STELOG_SORT_OLDEST;
    } else if (req->arg_sort.len == 6 && memcmp(req->arg_sort.ptr, "newest", 6) == 0) {
        q.sort_mode = STELOG_SORT_NEWEST;
    }

    StelogSearchResult sr;
    if (stelog_index_search(idx, &q, &sr) != 0) {
        if (sr.error_msg.len > 0) {
            send_rpc_tool_text(w, req, sr.error_msg.ptr, sr.error_msg.len, true);
        } else {
            send_rpc_tool_text(w, req, "Search failed.", 14, true);
        }
        return;
    }

    StelogIndexStats ist;
    stelog_index_get_stats(idx, &ist);
    uint32_t content_flags = ist.content_flags;

    BoundedText bt;
    uint32_t text_budget = (req->arg_limit > 20u) ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    StelogJsonWriter *sc = sc_begin();
    const char *mode = q.group_by_session ? "sessions" : (q.count_by != STELOG_COUNT_BY_NONE ? "counts" : "hits");
    stelog_jw_kv_cstr(sc, "mode", mode);
    stelog_jw_kv_u64(sc, "total_matches", sr.total_matches);
    stelog_jw_kv_double(sc, "elapsed_ms", sr.elapsed_ms);
    if (req->arg_include_tools && !(content_flags & STELOG_INDEX_HAS_TOOL_RESULTS)) {
        bt_note(&bt, "Note: include_tools has no effect because this index was built without tool outputs (rebuild with `stelog index --rebuild --include-tools`).\n");
    }
    if (req->arg_include_thinking && !(content_flags & STELOG_INDEX_HAS_THINKING)) {
        bt_note(&bt, "Note: include_thinking has no effect because this index was built without thinking blocks (rebuild with `stelog index --rebuild --include-thinking`).\n");
    }

    if (sr.total_matches == 0) {
        stelog_jw_key(sc, mode);
        stelog_jw_arr_begin(sc);
        stelog_jw_arr_end(sc);
        stelog_jw_kv_u64(sc, "shown", 0);
        bt_printf_force(&bt, "0 matches across %u indexed docs in scope (%.3f ms).\n", sr.docs_in_scope, sr.elapsed_ms);
        if (sr.unfiltered_total_matches > 0) {
            char roles[256];
            size_t rl = 0;
            roles[0] = '\0';
            for (int r = 0; r < STELOG_ROLE_COUNT && rl < sizeof(roles); ++r) {
                if (sr.other_role_counts[r] > 0) {
                    int n = snprintf(roles + rl, sizeof(roles) - rl, "%s%s=%u", rl ? ", " : "",
                                     stelog_role_name((StelogRoleKind)r), sr.other_role_counts[r]);
                    if (n > 0) rl += (size_t)n;
                }
            }
            bt_note(&bt, "Note: %u matches exist outside current filters (%s). Retry with broader agent/project/role/time filters%s.\n",
                    sr.unfiltered_total_matches, roles,
                    (content_flags & STELOG_INDEX_HAS_TOOL_RESULTS) ? " or include_tools=true" : "");
        } else {
            bt_note(&bt, "No match in the indexed layers (user/assistant/inter_agent/tool_call%s%s%s).\n",
                    (content_flags & STELOG_INDEX_HAS_TOOL_RESULTS) ? "/tool_result" : "",
                    (content_flags & STELOG_INDEX_HAS_THINKING) ? "/thinking" : "",
                    (content_flags & STELOG_INDEX_HAS_SYSTEM) ? "/system" : "");
        }
        if (q.case_mode == STELOG_CASE_SMART && !q.regex_mode && query_has_upper(req->arg_query)) {
            bt_note(&bt, "Hint: the query contains uppercase, so smart case matched case-sensitively; retry with case=\"insensitive\".\n");
        }
        if (!(content_flags & STELOG_INDEX_HAS_TOOL_RESULTS)) {
            bt_note(&bt, "Hint: tool outputs are not indexed; run `stelog index --rebuild --include-tools` to make them searchable.\n");
        }
        if (q.since_ms || q.until_ms || q.project_substr || q.agent_mask || q.session_id || q.actor_substr) {
            bt_note(&bt, "Hint: filters are active (agent/project/session/actor/since/until); drop them to widen the search.\n");
        }
        send_rpc_tool_sc(w, req, &bt, sc);
        bt_destroy(&bt);
        return;
    }
    if (q.group_by_session) {
        if (sr.session_group_count < sr.session_group_total) {
            bt_printf_force(&bt, "Matched %u sessions, showing %u from offset %u (%u unique hits, %u duplicates folded, %.3f ms):\n\n",
                            sr.session_group_total, sr.session_group_count, q.offset,
                            sr.total_matches, sr.duplicates_folded, sr.elapsed_ms);
        } else {
            bt_printf_force(&bt, "Matched %u sessions (%u unique hits, %u duplicates folded, %.3f ms):\n\n",
                            sr.session_group_total, sr.total_matches, sr.duplicates_folded, sr.elapsed_ms);
        }
        stelog_jw_kv_u64(sc, "total_sessions", sr.session_group_total);
        stelog_jw_key(sc, "sessions");
        stelog_jw_arr_begin(sc);
        char tbuf[32];
        uint32_t shown = 0;
        for (uint32_t i = 0; i < sr.session_group_count; ++i) {
            const StelogSessionHit *g = &sr.session_groups[i];
            if (!bt_can_append(&bt, 160 + g->title.len + g->best_snippet.len, 3)) break;
            fmt_ts_iso(g->last_hit_ts_ms, tbuf, sizeof(tbuf));
            uint32_t sid_s = short_sid_len(idx, g->session_id);
            bt_printf_force(&bt, "@%.*s [%-5s %s] (%u hits) | proj=%.*s | %.*s\n  best doc_id=%" PRIu64 ": ",
                            (int)sid_s, g->session_id.ptr,
                            stelog_agent_name(g->agent), tbuf, g->hit_count,
                            (int)g->project.len, g->project.ptr,
                            (int)g->title.len, g->title.ptr,
                            g->best_doc_id);
            bt_append_sanitized(&bt, g->best_snippet.ptr, g->best_snippet.len);
            bt_append_raw(&bt, "\n\n", 2);
            shown++;

            stelog_jw_obj_begin(sc);
            sc_kv_session(sc, "session", idx, g->session_id);
            stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(g->agent));
            sc_kv_ts(sc, "last_hit_ts", g->last_hit_ts_ms);
            stelog_jw_kv_u64(sc, "hit_count", g->hit_count);
            sc_kv_s(sc, "project", g->project);
            sc_kv_s(sc, "title", g->title);
            stelog_jw_kv_u64(sc, "best_doc_id", g->best_doc_id);
            sc_kv_s(sc, "best_snippet", g->best_snippet);
            stelog_jw_obj_end(sc);
        }
        stelog_jw_arr_end(sc);
        stelog_jw_kv_u64(sc, "shown", shown);
        if (q.offset + shown < sr.session_group_total) {
            stelog_jw_kv_u64(sc, "next_offset", q.offset + shown);
            bt_printf_force(&bt, "[more sessions: call search_history(..., group_by=\"session\", offset=%u)]\n", q.offset + shown);
        }
        bt_printf_force(&bt, "Next: call get_session_digest(session=\"@<id>\") to read the full decision stream of any session.\n");
        send_rpc_tool_sc(w, req, &bt, sc);
        bt_destroy(&bt);
        return;
    }

    if (q.count_by != STELOG_COUNT_BY_NONE) {
        bt_printf_force(&bt, "Grouped counts (%u total matches, %u buckets, %.3f ms):\n", sr.total_matches, sr.bucket_count, sr.elapsed_ms);
        stelog_jw_key(sc, "counts");
        stelog_jw_arr_begin(sc);
        char tbuf[32];
        uint32_t shown = 0;
        for (uint32_t i = 0; i < sr.bucket_count; ++i) {
            const StelogCountBucket *bk = &sr.buckets[i];
            if (!bt_can_append(&bt, bk->key.len + bk->meta_title.len + 64, 1)) break;
            shown++;
            stelog_jw_obj_begin(sc);
            stelog_jw_kv_u64(sc, "count", bk->count);
            if (q.count_by == STELOG_COUNT_BY_SESSION) {
                fmt_ts_iso(bk->meta_last_ts_ms, tbuf, sizeof(tbuf));
                uint32_t sid_s = short_sid_len(idx, bk->key);
                bt_printf_force(&bt, "%6u  @%.*s [%-5s %s] %.*s\n",
                                bk->count,
                                (int)sid_s, bk->key.ptr,
                                stelog_agent_name(bk->meta_agent), tbuf,
                                (int)bk->meta_title.len, bk->meta_title.ptr);
                sc_kv_session(sc, "key", idx, bk->key);
                stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(bk->meta_agent));
                sc_kv_ts(sc, "last_ts", bk->meta_last_ts_ms);
                sc_kv_s(sc, "title", bk->meta_title);
            } else {
                bt_printf_force(&bt, "%6u  %.*s\n", bk->count, (int)bk->key.len, bk->key.ptr);
                sc_kv_s(sc, "key", bk->key);
            }
            stelog_jw_obj_end(sc);
        }
        stelog_jw_arr_end(sc);
        stelog_jw_kv_u64(sc, "shown", shown);
        send_rpc_tool_sc(w, req, &bt, sc);
        bt_destroy(&bt);
        return;
    }

    bt_printf_force(&bt, "Found %u matches (%.3f ms)%s:\n\n",
                    sr.total_matches, sr.elapsed_ms, is_timeline ? " [oldest-first timeline]" : "");

    stelog_jw_key(sc, "hits");
    stelog_jw_arr_begin(sc);
    uint32_t shown = 0;
    char tbuf[32];
    for (uint32_t i = 0; i < sr.count; ++i) {
        const StelogHit *h = &sr.hits[i];
        size_t est = 160 + h->actor.len + h->project.len + h->snippet.len;
        if (!bt_can_append(&bt, est, 3)) break;
        fmt_ts_iso(h->ts_ms, tbuf, sizeof(tbuf));
        uint32_t sid_short = short_sid_len(idx, h->session_id);
        bt_printf_force(&bt, "[%s] doc_id=%" PRIu64 " @%.*s | %s/%s | actor=%.*s | proj=%.*s\n  ",
                        tbuf, h->doc_id,
                        (int)sid_short, h->session_id.ptr,
                        stelog_agent_name(stelog_tag_agent(h->tag)),
                        stelog_role_name(stelog_tag_role(h->tag)),
                        (int)h->actor.len, h->actor.ptr,
                        (int)h->project.len, h->project.ptr);
        bt_append_sanitized(&bt, h->snippet.ptr, h->snippet.len);
        bt_append_raw(&bt, "\n\n", 2);
        shown++;

        stelog_jw_obj_begin(sc);
        stelog_jw_kv_u64(sc, "doc_id", h->doc_id);
        sc_kv_ts(sc, "ts", h->ts_ms);
        sc_kv_session(sc, "session", idx, h->session_id);
        stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(stelog_tag_agent(h->tag)));
        stelog_jw_kv_cstr(sc, "role", stelog_role_name(stelog_tag_role(h->tag)));
        sc_kv_s(sc, "actor", h->actor);
        sc_kv_s(sc, "project", h->project);
        sc_kv_s(sc, "snippet", h->snippet);
        stelog_jw_obj_end(sc);
    }
    stelog_jw_arr_end(sc);
    stelog_jw_kv_u64(sc, "shown", shown);

    if (bt.truncated || sr.truncated) {
        stelog_jw_kv_u64(sc, "next_offset", q.offset + shown);
        bt_printf_force(&bt, "[truncated by %s: showing %u/%u matches. Next: call %s(..., offset=%u), get_session_context(doc_id=...), or get_document(doc_id=...)]\n",
                        bt.truncated ? bt.truncated_by : "limit",
                        shown, sr.total_matches,
                        is_timeline ? "get_decision_timeline" : "search_history",
                        q.offset + shown);
    }

    send_rpc_tool_sc(w, req, &bt, sc);
    bt_destroy(&bt);
}

static void handle_context_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    if (!req->has_doc_id) {
        send_rpc_tool_text(w, req, "Missing required argument: doc_id", 33, true);
        return;
    }
    StelogContextResult cr;
    if (stelog_index_context(idx, req->arg_doc_id, req->arg_window ? req->arg_window : 5u,
                             req->arg_max_bytes ? req->arg_max_bytes : 1200u,
                             req->arg_include_tools, &cr) != 0) {
        static const char kMsg[] = "doc_id not found in index. doc_ids are stable across rebuilds; use one returned by "
                                   "search_history, get_session_digest, or search_file_history.";
        send_rpc_tool_text(w, req, kMsg, sizeof(kMsg) - 1, true);
        return;
    }

    BoundedText bt;
    uint32_t text_budget = req->arg_max_bytes ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    StelogJsonWriter *sc = sc_begin();
    bt_printf_force(&bt, "Session %.*s | proj=%.*s | title=%.*s | %u turns (%.3f ms)\n\n",
                    (int)cr.session_id.len, cr.session_id.ptr,
                    (int)cr.project.len, cr.project.ptr,
                    (int)cr.title.len, cr.title.ptr, cr.count, cr.elapsed_ms);
    sc_kv_session(sc, "session", idx, cr.session_id);
    sc_kv_s(sc, "session_id", cr.session_id);
    sc_kv_s(sc, "project", cr.project);
    sc_kv_s(sc, "title", cr.title);
    stelog_jw_kv_u64(sc, "anchor_doc_id", req->arg_doc_id);
    stelog_jw_key(sc, "turns");
    stelog_jw_arr_begin(sc);

    char tbuf[32];
    for (uint32_t i = 0; i < cr.count; ++i) {
        const StelogHit *h = &cr.items[i];
        if (!bt_can_append(&bt, 120 + h->snippet.len, 3)) break;
        if (h->collapsed_tools_before > 0) {
            bt_printf_force(&bt, "    [...%u tool calls collapsed]\n", h->collapsed_tools_before);
        }
        fmt_ts_iso(h->ts_ms, tbuf, sizeof(tbuf));
        bt_printf_force(&bt, "%s[%s] doc_id=%" PRIu64 " %s (%.*s): ",
                        (i == cr.anchor_index) ? ">>> " : "    ",
                        tbuf, h->doc_id,
                        stelog_role_name(stelog_tag_role(h->tag)),
                        (int)h->actor.len, h->actor.ptr);
        bt_append_sanitized(&bt, h->snippet.ptr, h->snippet.len);
        bt_append_raw(&bt, "\n", 1);

        stelog_jw_obj_begin(sc);
        stelog_jw_kv_u64(sc, "doc_id", h->doc_id);
        sc_kv_ts(sc, "ts", h->ts_ms);
        stelog_jw_kv_cstr(sc, "role", stelog_role_name(stelog_tag_role(h->tag)));
        sc_kv_s(sc, "actor", h->actor);
        sc_kv_s(sc, "text", h->snippet);
        if (h->truncated) stelog_jw_kv_bool(sc, "trimmed", true);
        if (i == cr.anchor_index) stelog_jw_kv_bool(sc, "anchor", true);
        if (h->collapsed_tools_before > 0) stelog_jw_kv_u64(sc, "tools_before", h->collapsed_tools_before);
        stelog_jw_obj_end(sc);
    }
    stelog_jw_arr_end(sc);
    if (bt.truncated) {
        bt_note(&bt, "[truncated by %s: use get_document(doc_id=...) to read any specific turn in full]\n", bt.truncated_by);
    }
    send_rpc_tool_sc(w, req, &bt, sc);
    bt_destroy(&bt);
}

static void handle_digest_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    if (req->arg_session.len == 0) {
        send_rpc_tool_text(w, req, "Missing required argument: session (e.g. \"@30c3cab1\")", 54, true);
        return;
    }
    uint32_t budget = req->arg_max_bytes ? req->arg_max_bytes : 7200u;
    if (budget > MCP_MAX_TEXT_BYTES - 1024u) budget = MCP_MAX_TEXT_BYTES - 1024u;

    StelogDigestOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.session = req->arg_session.ptr;
    opts.max_total_bytes = budget;
    opts.turn_offset = req->arg_offset;
    opts.include_tools = req->arg_include_tools;

    StelogDigestResult dr;
    int rc = stelog_index_digest_ex(idx, &opts, &dr);
    BoundedText bt;
    char tbuf[32], tbuf2[32];
    if (rc == -1) {
        char msg[256];
        int n = snprintf(msg, sizeof(msg),
                         "No session matches \"%.*s\". Use list_sessions or search_history(group_by=\"session\") to find a session id.",
                         (int)(req->arg_session.len > 80 ? 80 : req->arg_session.len), req->arg_session.ptr);
        send_rpc_tool_text(w, req, msg, (size_t)(n > 0 ? n : 0), true);
        return;
    }
    if (rc == -2) {
        bt_init(&bt, MCP_INLINE_SOFT_BYTES, MCP_MAX_TEXT_LINES);
        bt_printf_force(&bt, "Ambiguous session prefix \"%.*s\": %u sessions match. Retry with a longer prefix:\n",
                        (int)(req->arg_session.len > 80 ? 80 : req->arg_session.len), req->arg_session.ptr, dr.match_count);
        for (uint32_t i = 0; i < dr.candidate_count; ++i) {
            const StelogSessionSummary *c = &dr.candidates[i];
            fmt_ts_iso(c->first_ts_ms, tbuf, sizeof(tbuf));
            fmt_ts_iso(c->last_ts_ms, tbuf2, sizeof(tbuf2));
            uint32_t sl = short_sid_len(idx, c->session_id);
            bt_printf_force(&bt, "  @%.*s [%s %s .. %s] %u docs | proj=%.*s | %.*s\n",
                            (int)sl, c->session_id.ptr, stelog_agent_name(c->agent), tbuf, tbuf2, c->doc_count,
                            (int)c->project.len, c->project.ptr, (int)(c->title.len > 120 ? 120 : c->title.len), c->title.ptr);
        }
        if (dr.match_count > dr.candidate_count) {
            bt_printf_force(&bt, "  ... and %u more\n", dr.match_count - dr.candidate_count);
        }
        send_rpc_tool_text(w, req, bt.buf, bt.len, true);
        bt_destroy(&bt);
        return;
    }
    if (rc != 0) {
        send_rpc_tool_text(w, req, "Digest failed.", 14, true);
        return;
    }

    uint32_t text_budget = req->arg_max_bytes ? MCP_MAX_TEXT_BYTES : (MCP_INLINE_SOFT_BYTES + 400u);
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    StelogJsonWriter *sc = sc_begin();
    fmt_ts_iso(dr.first_ts_ms, tbuf, sizeof(tbuf));
    fmt_ts_iso(dr.last_ts_ms, tbuf2, sizeof(tbuf2));
    uint32_t sl = short_sid_len(idx, dr.session_id);
    bt_printf_force(&bt, "Digest @%.*s [%s] %s .. %s | proj=%.*s | title=%.*s\n",
                    (int)sl, dr.session_id.ptr, stelog_agent_name(dr.agent), tbuf, tbuf2,
                    (int)dr.project.len, dr.project.ptr,
                    (int)(dr.title.len > 160 ? 160 : dr.title.len), dr.title.ptr);
    sc_kv_session(sc, "session", idx, dr.session_id);
    sc_kv_s(sc, "session_id", dr.session_id);
    stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(dr.agent));
    sc_kv_ts(sc, "first_ts", dr.first_ts_ms);
    sc_kv_ts(sc, "last_ts", dr.last_ts_ms);
    sc_kv_s(sc, "project", dr.project);
    sc_kv_s(sc, "title", dr.title);
    stelog_jw_kv_u64(sc, "total_turns", dr.total_dialogue_turns);
    stelog_jw_kv_u64(sc, "tools_collapsed", dr.total_tools_collapsed);
    stelog_jw_kv_u64(sc, "duplicates_skipped", dr.duplicates_skipped);
    stelog_jw_key(sc, "turns");
    stelog_jw_arr_begin(sc);
    if (dr.count == 0) {
        stelog_jw_arr_end(sc);
        bt_printf_force(&bt, "No dialogue turns at offset %u (session has %u turns, %u tool records collapsed).\n",
                        dr.turn_offset, dr.total_dialogue_turns, dr.total_tools_collapsed);
        send_rpc_tool_sc(w, req, &bt, sc);
        bt_destroy(&bt);
        return;
    }
    bt_printf_force(&bt, "Showing %u of %u dialogue turns%s (%u tool records collapsed, %u retries deduped, %.3f ms)\n\n",
                    dr.count, dr.total_dialogue_turns,
                    dr.omitted_count ? "" : (dr.turn_offset ? " (through the end)" : " (complete)"),
                    dr.total_tools_collapsed, dr.duplicates_skipped, dr.elapsed_ms);

    uint32_t shown = 0;
    for (uint32_t i = 0; i < dr.count; ++i) {
        const StelogHit *h = &dr.items[i];
        bool after_gap = dr.omitted_count > 0 && i >= dr.gap_item_index;
        if (dr.omitted_count > 0 && i == dr.gap_item_index) {
            bt_printf_force(&bt, "  [... %u turns omitted (#%u-#%u); call get_session_digest(session=\"@%.*s\", offset=%u) to read them]\n",
                            dr.omitted_count, dr.omitted_begin, dr.omitted_begin + dr.omitted_count - 1u,
                            (int)sl, dr.session_id.ptr, dr.next_offset);
        }
        if (!bt_can_append(&bt, 140 + h->snippet.len, 3)) break;
        uint32_t turn_no = after_gap ? dr.omitted_begin + dr.omitted_count + (i - dr.gap_item_index) : dr.turn_offset + i;
        if (h->collapsed_tools_before > 0) {
            bt_printf_force(&bt, "  [...%u tool calls]\n", h->collapsed_tools_before);
        }
        fmt_ts_iso(h->ts_ms, tbuf, sizeof(tbuf));
        bt_printf_force(&bt, "#%u [%s] doc_id=%" PRIu64 " %s (%.*s): ",
                        turn_no, tbuf, h->doc_id,
                        stelog_role_name(stelog_tag_role(h->tag)),
                        (int)h->actor.len, h->actor.ptr);
        bt_append_sanitized(&bt, h->snippet.ptr, h->snippet.len);
        bt_append_raw(&bt, "\n", 1);
        shown++;

        stelog_jw_obj_begin(sc);
        stelog_jw_kv_u64(sc, "turn", turn_no);
        stelog_jw_kv_u64(sc, "doc_id", h->doc_id);
        sc_kv_ts(sc, "ts", h->ts_ms);
        stelog_jw_kv_cstr(sc, "role", stelog_role_name(stelog_tag_role(h->tag)));
        sc_kv_s(sc, "actor", h->actor);
        sc_kv_s(sc, "text", h->snippet);
        if (h->truncated) stelog_jw_kv_bool(sc, "trimmed", true);
        if (h->collapsed_tools_before > 0) stelog_jw_kv_u64(sc, "tools_before", h->collapsed_tools_before);
        stelog_jw_obj_end(sc);
    }
    stelog_jw_arr_end(sc);
    if (dr.omitted_count > 0) {
        stelog_jw_key(sc, "omitted");
        stelog_jw_obj_begin(sc);
        stelog_jw_kv_u64(sc, "from_turn", dr.omitted_begin);
        stelog_jw_kv_u64(sc, "to_turn", dr.omitted_begin + dr.omitted_count - 1u);
        stelog_jw_kv_u64(sc, "next_offset", dr.next_offset);
        stelog_jw_obj_end(sc);
    }
    if (dr.omitted_count > 0 && dr.gap_item_index >= dr.count && shown == dr.count) {
        bt_printf_force(&bt, "\n[... %u more turns (#%u-#%u); call get_session_digest(session=\"@%.*s\", offset=%u) to continue]\n",
                        dr.omitted_count, dr.omitted_begin, dr.omitted_begin + dr.omitted_count - 1u,
                        (int)sl, dr.session_id.ptr, dr.next_offset);
    }
    if (bt.truncated) {
        bool in_head = dr.omitted_count == 0 || shown < dr.gap_item_index;
        uint32_t next = in_head ? dr.turn_offset + shown
                                : dr.omitted_begin + dr.omitted_count + (shown - dr.gap_item_index);
        /* structuredContent.next_offset is the earliest unread turn, so paging from it reads everything */
        stelog_jw_kv_u64(sc, "next_offset", (dr.omitted_count > 0 && dr.next_offset < next) ? dr.next_offset : next);
        bt_printf_force(&bt, "\n[output budget reached after %u items; call get_session_digest(session=\"@%.*s\", offset=%u) or get_document(doc_id=...)]\n",
                        shown, (int)sl, dr.session_id.ptr, next);
    } else if (dr.omitted_count > 0) {
        stelog_jw_kv_u64(sc, "next_offset", dr.next_offset);
    }
    bt_printf_force(&bt, "\n[long turns are trimmed; call get_document(doc_ids=[...]) to read any turn in full]\n");
    send_rpc_tool_sc(w, req, &bt, sc);
    bt_destroy(&bt);
}

static void handle_get_doc_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    uint64_t ids[STELOG_MCP_MAX_BATCH_DOCS];
    uint32_t id_count = 0;
    if (req->arg_doc_id_count > 0) {
        id_count = req->arg_doc_id_count;
        memcpy(ids, req->arg_doc_ids, id_count * sizeof(uint64_t));
    } else if (req->has_doc_id) {
        ids[0] = req->arg_doc_id;
        id_count = 1;
    } else {
        send_rpc_tool_text(w, req, "Missing required argument: doc_id or doc_ids", 44, true);
        return;
    }

    uint32_t per_doc_lim = req->arg_limit;
    if (per_doc_lim == 0) {
        if (id_count == 1) per_doc_lim = 7200u;
        else {
            per_doc_lim = 24000u / id_count;
            if (per_doc_lim < 2000u) per_doc_lim = 2000u;
            if (per_doc_lim > 6000u) per_doc_lim = 6000u;
        }
    }
    if (per_doc_lim > MCP_MAX_TEXT_BYTES - 512u) per_doc_lim = MCP_MAX_TEXT_BYTES - 512u;

    BoundedText bt;
    bt_init(&bt, MCP_MAX_TEXT_BYTES, MCP_MAX_TEXT_LINES * 4u);
    StelogJsonWriter *sc = sc_begin();
    stelog_jw_key(sc, "documents");
    stelog_jw_arr_begin(sc);
    char tbuf[32];
    uint32_t done = id_count;

    for (uint32_t i = 0; i < id_count; ++i) {
        if (i > 0) bt_append_raw(&bt, "\n", 1);
        size_t rem_bt = (bt.max_bytes > bt.len + 256u) ? (bt.max_bytes - bt.len - 256u) : 0u;
        if (rem_bt < 256u) {
            bt_printf_force(&bt, "[batch budget reached after %u/%u docs; fetch remaining doc_ids in another call]\n",
                            i, id_count);
            done = i;
            break;
        }
        uint32_t this_lim = per_doc_lim < (uint32_t)rem_bt ? per_doc_lim : (uint32_t)rem_bt;
        uint32_t this_off = (id_count == 1) ? req->arg_offset : 0u;

        StelogDocResult dr;
        if (stelog_index_get_document(idx, ids[i], req->arg_raw, this_off, this_lim, req->arg_tail, &dr) != 0) {
            if (id_count == 1) {
                bt_destroy(&bt);
                static const char kMsg[] = "Failed to read document (unknown doc_id or compressed raw file). doc_ids are stable "
                                           "across rebuilds; use one returned by search_history, get_session_digest, or search_file_history.";
                send_rpc_tool_text(w, req, kMsg, sizeof(kMsg) - 1, true);
                return;
            }
            bt_printf_force(&bt, "doc_id=%" PRIu64 " | ERROR: not found or compressed raw file\n", ids[i]);
            stelog_jw_obj_begin(sc);
            stelog_jw_kv_u64(sc, "doc_id", ids[i]);
            stelog_jw_kv_cstr(sc, "error", "not found or compressed raw file");
            stelog_jw_obj_end(sc);
            continue;
        }

        fmt_ts_iso(dr.ts_ms, tbuf, sizeof(tbuf));
        if (!dr.truncated) {
            bt_printf_force(&bt, "doc_id=%" PRIu64 " | %s | %s/%s | actor=%.*s | proj=%.*s | complete (%uB, %.3f ms)\n---\n",
                            dr.doc_id, tbuf,
                            stelog_agent_name(stelog_tag_agent(dr.tag)),
                            stelog_role_name(stelog_tag_role(dr.tag)),
                            (int)dr.actor.len, dr.actor.ptr,
                            (int)dr.project.len, dr.project.ptr,
                            dr.total_bytes, dr.elapsed_ms);
        } else {
            bt_printf_force(&bt, "doc_id=%" PRIu64 " | %s | %s/%s | actor=%.*s | proj=%.*s | truncated (%u..%u/%uB, %.3f ms)\n---\n",
                            dr.doc_id, tbuf,
                            stelog_agent_name(stelog_tag_agent(dr.tag)),
                            stelog_role_name(stelog_tag_role(dr.tag)),
                            (int)dr.actor.len, dr.actor.ptr,
                            (int)dr.project.len, dr.project.ptr,
                            dr.returned_offset, dr.returned_offset + dr.content.len, dr.total_bytes, dr.elapsed_ms);
        }
        bt_append_raw(&bt, dr.content.ptr, dr.content.len);
        bt_append_raw(&bt, "\n", 1);

        stelog_jw_obj_begin(sc);
        stelog_jw_kv_u64(sc, "doc_id", dr.doc_id);
        sc_kv_ts(sc, "ts", dr.ts_ms);
        sc_kv_session(sc, "session", idx, dr.session_id);
        stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(stelog_tag_agent(dr.tag)));
        stelog_jw_kv_cstr(sc, "role", stelog_role_name(stelog_tag_role(dr.tag)));
        sc_kv_s(sc, "actor", dr.actor);
        sc_kv_s(sc, "project", dr.project);
        stelog_jw_kv_u64(sc, "offset", dr.returned_offset);
        stelog_jw_kv_u64(sc, "total_bytes", dr.total_bytes);
        stelog_jw_kv_str(sc, "content", dr.content.ptr ? dr.content.ptr : "", dr.content.len);
        if (dr.truncated) {
            uint32_t next_off = dr.returned_offset + dr.content.len;
            uint32_t rem_bytes = dr.total_bytes > next_off ? (dr.total_bytes - next_off) : 0u;
            bt_printf_force(&bt, "---\n[truncated: next call get_document(doc_id=%" PRIu64 ", offset=%u) for remaining %uB]\n",
                            dr.doc_id, next_off, rem_bytes);
            if (rem_bytes > 0) stelog_jw_kv_u64(sc, "next_offset", next_off);
        }
        stelog_jw_obj_end(sc);
    }
    stelog_jw_arr_end(sc);
    if (done < id_count) {
        stelog_jw_key(sc, "remaining_doc_ids");
        stelog_jw_arr_begin(sc);
        for (uint32_t i = done; i < id_count; ++i) stelog_jw_u64(sc, ids[i]);
        stelog_jw_arr_end(sc);
    }
    send_rpc_tool_sc(w, req, &bt, sc);
    bt_destroy(&bt);
}

static void handle_sessions_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    int64_t since_ms, until_ms;
    char time_err[320];
    if (!parse_time_range(req, &since_ms, &until_ms, time_err, sizeof(time_err))) {
        send_rpc_tool_text(w, req, time_err, strlen(time_err), true);
        return;
    }
    StelogSessionListResult sl;
    stelog_index_list_sessions(idx,
                               parse_agent_csv(req->arg_agent),
                               req->arg_project.len ? req->arg_project.ptr : NULL,
                               since_ms,
                               until_ms,
                               req->arg_limit ? req->arg_limit : 20u,
                               req->arg_offset,
                               &sl);

    BoundedText bt;
    uint32_t text_budget = (req->arg_limit > 20u) ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    StelogJsonWriter *sc = sc_begin();
    stelog_jw_kv_u64(sc, "total", sl.total_matches);
    stelog_jw_key(sc, "sessions");
    stelog_jw_arr_begin(sc);
    bt_printf_force(&bt, "Sessions: %u total (showing %u from offset %u, %.3f ms; times are first .. last activity)\n\n",
                    sl.total_matches, sl.count, req->arg_offset, sl.elapsed_ms);
    char tbuf[32], tbuf2[32];
    uint32_t shown = 0;
    for (uint32_t i = 0; i < sl.count; ++i) {
        const StelogSessionSummary *s = &sl.items[i];
        if (!bt_can_append(&bt, 160 + s->project.len + s->title.len, 1)) break;
        shown++;
        fmt_ts_iso(s->first_ts_ms, tbuf, sizeof(tbuf));
        fmt_ts_iso(s->last_ts_ms, tbuf2, sizeof(tbuf2));
        uint32_t sl_len = short_sid_len(idx, s->session_id);
        bool sub = (s->flags & STELOG_FLAG_IS_SUBAGENT) != 0;
        bt_printf_force(&bt, "[%s .. %s] %-5s %4u docs | @%.*s%s | proj=%.*s | %.*s\n",
                        tbuf, tbuf2, stelog_agent_name(s->agent), s->doc_count,
                        (int)sl_len, s->session_id.ptr,
                        sub ? " (subagent)" : "",
                        (int)s->project.len, s->project.ptr,
                        (int)s->title.len, s->title.ptr);

        stelog_jw_obj_begin(sc);
        sc_kv_session(sc, "session", idx, s->session_id);
        sc_kv_s(sc, "session_id", s->session_id);
        stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(s->agent));
        sc_kv_ts(sc, "first_ts", s->first_ts_ms);
        sc_kv_ts(sc, "last_ts", s->last_ts_ms);
        stelog_jw_kv_u64(sc, "doc_count", s->doc_count);
        if (sub) stelog_jw_kv_bool(sc, "subagent", true);
        sc_kv_s(sc, "project", s->project);
        sc_kv_s(sc, "title", s->title);
        stelog_jw_obj_end(sc);
    }
    stelog_jw_arr_end(sc);
    if (req->arg_offset + shown < sl.total_matches) {
        stelog_jw_kv_u64(sc, "next_offset", req->arg_offset + shown);
        bt_printf_force(&bt, "\n[more: call list_sessions(..., offset=%u)]\n", req->arg_offset + shown);
    }
    send_rpc_tool_sc(w, req, &bt, sc);
    bt_destroy(&bt);
}

static void handle_files_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    int64_t since_ms, until_ms;
    char time_err[320];
    if (!parse_time_range(req, &since_ms, &until_ms, time_err, sizeof(time_err))) {
        send_rpc_tool_text(w, req, time_err, strlen(time_err), true);
        return;
    }
    StelogFileHistoryResult fh;
    stelog_index_files(idx,
                       req->arg_file.len ? req->arg_file.ptr : NULL,
                       parse_agent_csv(req->arg_agent),
                       req->arg_project.len ? req->arg_project.ptr : NULL,
                       req->arg_session.len ? req->arg_session.ptr : NULL,
                       since_ms,
                       until_ms,
                       req->arg_no_subagents,
                       req->arg_limit ? req->arg_limit : 30u,
                       req->arg_offset,
                       &fh);

    BoundedText bt;
    uint32_t text_budget = (req->arg_limit > 20u) ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    StelogJsonWriter *sc = sc_begin();
    stelog_jw_kv_u64(sc, "total_matches", fh.total_matches);
    stelog_jw_key(sc, "touches");
    stelog_jw_arr_begin(sc);
    bt_printf_force(&bt, "File modifications: %u matches (showing %u, %.3f ms)\n\n", fh.total_matches, fh.count, fh.elapsed_ms);
    char tbuf[32];
    uint32_t shown = 0;
    for (uint32_t i = 0; i < fh.count; ++i) {
        const StelogFileTouch *ft = &fh.items[i];
        if (!bt_can_append(&bt, 140 + ft->file_touched.len + ft->project.len, 1)) break;
        shown++;
        fmt_ts_iso(ft->ts_ms, tbuf, sizeof(tbuf));
        uint32_t sid_s = short_sid_len(idx, ft->session_id);
        bt_printf_force(&bt, "[%s] doc_id=%" PRIu64 " @%.*s | %-5s | %-10.*s | actor=%.*s | %.*s\n",
                        tbuf, ft->doc_id, (int)sid_s, ft->session_id.ptr,
                        stelog_agent_name(stelog_tag_agent(ft->tag)),
                        (int)ft->tool_name.len, ft->tool_name.ptr,
                        (int)ft->actor.len, ft->actor.ptr,
                        (int)ft->file_touched.len, ft->file_touched.ptr);

        stelog_jw_obj_begin(sc);
        stelog_jw_kv_u64(sc, "doc_id", ft->doc_id);
        sc_kv_ts(sc, "ts", ft->ts_ms);
        sc_kv_session(sc, "session", idx, ft->session_id);
        stelog_jw_kv_cstr(sc, "agent", stelog_agent_name(stelog_tag_agent(ft->tag)));
        sc_kv_s(sc, "tool", ft->tool_name);
        sc_kv_s(sc, "actor", ft->actor);
        sc_kv_s(sc, "file", ft->file_touched);
        stelog_jw_obj_end(sc);
    }
    stelog_jw_arr_end(sc);
    stelog_jw_kv_u64(sc, "shown", shown);
    if (fh.truncated || bt.truncated) {
        stelog_jw_kv_u64(sc, "next_offset", req->arg_offset + shown);
        bt_printf_force(&bt, "\n[truncated by %s: showing %u/%u matches. Next: call search_file_history(..., offset=%u) for more, "
                             "or get_session_context(doc_id=...) to inspect the decision behind an edit]\n",
                        bt.truncated ? bt.truncated_by : "limit", shown, fh.total_matches, req->arg_offset + shown);
    } else if (fh.count > 0) {
        bt_printf_force(&bt, "\n[use get_session_context(doc_id=...) to inspect the decision behind an edit]\n");
    }
    send_rpc_tool_sc(w, req, &bt, sc);
    bt_destroy(&bt);
}
/* ============================================================================
 * Main stdio JSON-RPC 2.0 Loop
 * ============================================================================ */

int stelog_mcp_serve(const char *index_dir) {
    /* Ensure index exists on startup */
    StelogIndexConfig icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.index_dir = index_dir;
    stelog_index_sync(&icfg, NULL);

    StelogIndex *idx = stelog_index_open(index_dir);
    if (!idx) {
        fprintf(stderr, "[stelog mcp] failed to open index\n");
        return 1;
    }

    StelogJsonParser *jp = stelog_json_parser_create();
    StelogArena req_arena;
    stelog_arena_init(&req_arena, 64u * 1024u);
    StelogJsonWriter jw;
    stelog_jw_init(&jw, 65536u);
    stelog_jw_init(&g_sc, 65536u);
    stelog_jw_init(&g_notes, 1024u);

    double last_sync_ms = stelog_now_ms();
    char *line = NULL;
    size_t line_cap = 0;

    StelogIndexStats st;
    stelog_index_get_stats(idx, &st);
    fprintf(stderr, "[stelog mcp] ready on stdio (base=%u docs, delta=%u docs)\n",
            st.base_docs, st.delta_docs);

    for (;;) {
        ssize_t n = getline(&line, &line_cap, stdin);
        if (n < 0) break;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        if (n == 0) continue;

        stelog_arena_reset(&req_arena);
        StelogMcpRequest req;
        if (stelog_json_parse_mcp_request(jp, line, (size_t)n, (size_t)n, &req_arena, &req) != 0) {
            StelogMcpRequest err_req;
            memset(&err_req, 0, sizeof(err_req));
            err_req.id_kind = STELOG_RPC_ID_NULL;
            send_rpc_error(&jw, &err_req, -32700, "Parse error");
            continue;
        }

        /* Notifications have no id -> no response */
        if (req.id_kind == STELOG_RPC_ID_NONE) {
            continue;
        }

        if (req.method.len == 10 && memcmp(req.method.ptr, "initialize", 10) == 0) {
            stelog_jw_reset(&jw);
            stelog_jw_obj_begin(&jw);
            stelog_jw_kv_cstr(&jw, "jsonrpc", "2.0");
            g_structured_output = client_wants_structured(req.client_name);
            write_rpc_id(&jw, &req);
            stelog_jw_key(&jw, "result");
            write_initialize_result(&jw, req.protocol_version);
            stelog_jw_obj_end(&jw);
            fwrite(jw.buf, 1, jw.len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        } else if (req.method.len == 4 && memcmp(req.method.ptr, "ping", 4) == 0) {
            stelog_jw_reset(&jw);
            stelog_jw_obj_begin(&jw);
            stelog_jw_kv_cstr(&jw, "jsonrpc", "2.0");
            write_rpc_id(&jw, &req);
            stelog_jw_key(&jw, "result");
            stelog_jw_obj_begin(&jw);
            stelog_jw_obj_end(&jw);
            stelog_jw_obj_end(&jw);
            fwrite(jw.buf, 1, jw.len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        } else if (req.method.len == 10 && memcmp(req.method.ptr, "tools/list", 10) == 0) {
            stelog_jw_reset(&jw);
            stelog_jw_obj_begin(&jw);
            stelog_jw_kv_cstr(&jw, "jsonrpc", "2.0");
            write_rpc_id(&jw, &req);
            stelog_jw_key(&jw, "result");
            if (g_structured_output) stelog_jw_raw(&jw, stelog_mcp_tools_list_json, stelog_mcp_tools_list_json_len);
            else stelog_jw_raw(&jw, stelog_mcp_tools_list_plain_json, stelog_mcp_tools_list_plain_json_len);
            stelog_jw_obj_end(&jw);
            fwrite(jw.buf, 1, jw.len, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        } else if (req.method.len == 10 && memcmp(req.method.ptr, "tools/call", 10) == 0) {
            /* Auto Hot-Delta refresh with cooldown */
            double now = stelog_now_ms();
            if (now - last_sync_ms >= MCP_SYNC_COOLDOWN_MS) {
                stelog_index_sync(&icfg, NULL);
                stelog_index_reload_if_changed(idx);
                last_sync_ms = now;
            }

            if (req.tool_name.len == 14 && memcmp(req.tool_name.ptr, "search_history", 14) == 0) {
                handle_search_tool(idx, &req, false, &jw);
            } else if (req.tool_name.len == 18 && memcmp(req.tool_name.ptr, "get_session_digest", 18) == 0) {
                handle_digest_tool(idx, &req, &jw);
            } else if (req.tool_name.len == 21 && memcmp(req.tool_name.ptr, "get_decision_timeline", 21) == 0) {
                handle_search_tool(idx, &req, true, &jw);
            } else if (req.tool_name.len == 19 && memcmp(req.tool_name.ptr, "get_session_context", 19) == 0) {
                handle_context_tool(idx, &req, &jw);
            } else if (req.tool_name.len == 12 && memcmp(req.tool_name.ptr, "get_document", 12) == 0) {
                handle_get_doc_tool(idx, &req, &jw);
            } else if (req.tool_name.len == 13 && memcmp(req.tool_name.ptr, "list_sessions", 13) == 0) {
                handle_sessions_tool(idx, &req, &jw);
            } else if (req.tool_name.len == 19 && memcmp(req.tool_name.ptr, "search_file_history", 19) == 0) {
                handle_files_tool(idx, &req, &jw);
            } else {
                send_rpc_error(&jw, &req, -32602, "Unknown tool name");
            }
        } else {
            send_rpc_error(&jw, &req, -32601, "Method not found");
        }
    }

    free(line);
    stelog_jw_destroy(&g_notes);
    stelog_jw_destroy(&g_sc);
    stelog_jw_destroy(&jw);
    stelog_arena_destroy(&req_arena);
    stelog_json_parser_destroy(jp);
    stelog_index_close(idx);
    return 0;
}
