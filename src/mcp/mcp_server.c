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

static void fmt_ts_iso(int64_t ts_ms, char *out, size_t cap) {
    if (ts_ms <= 0) { snprintf(out, cap, "unknown"); return; }
    time_t sec = (time_t)(ts_ms / 1000ll);
    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

static int64_t parse_time_arg(StelogStr s) {
    if (!s.ptr || s.len == 0) return 0;
    if (s.len >= 2 && (s.ptr[s.len - 1] == 'd' || s.ptr[s.len - 1] == 'h')) {
        char *end = NULL;
        long val = strtol(s.ptr, &end, 10);
        if (end == s.ptr + s.len - 1 && val > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t now_ms = (int64_t)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
            int64_t d = (s.ptr[s.len - 1] == 'd') ? (val * 86400ll * 1000ll) : (val * 3600ll * 1000ll);
            return now_ms - d;
        }
    }
    return stelog_parse_iso8601_ms(s.ptr, s.len);
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

static void send_rpc_tool_text(StelogJsonWriter *w, const StelogMcpRequest *req, const char *text, size_t len, bool is_error) {
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
    if (is_error) stelog_jw_kv_bool(w, "isError", true);
    stelog_jw_obj_end(w);
    stelog_jw_obj_end(w);
    fwrite(w->buf, 1, w->len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
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
    q.since_ms          = parse_time_arg(req->arg_since);
    q.until_ms          = parse_time_arg(req->arg_until);
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

    BoundedText bt;
    uint32_t text_budget = (req->arg_limit > 20u) ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);

    if (sr.total_matches == 0) {
        bt_printf_force(&bt, "0 matches across %u indexed docs in scope (%.3f ms).\n", sr.docs_in_scope, sr.elapsed_ms);
        if (sr.unfiltered_total_matches > 0) {
            bt_printf_force(&bt, "Note: %u matches exist outside current filters (", sr.unfiltered_total_matches);
            bool first = true;
            for (int r = 0; r < STELOG_ROLE_COUNT; ++r) {
                if (sr.other_role_counts[r] > 0) {
                    bt_printf_force(&bt, "%s%s=%u", first ? "" : ", ", stelog_role_name((StelogRoleKind)r), sr.other_role_counts[r]);
                    first = false;
                }
            }
            bt_printf_force(&bt, "). Retry with broader agent/project/role filters or include_tools=true.\n");
        } else {
            bt_printf_force(&bt, "Definitive absence in indexed decision layers (user/assistant/inter_agent/tool_call). Note: large tool_result outputs and thinking blocks are excluded unless indexed.\n");
        }
        send_rpc_tool_text(w, req, bt.buf, bt.len, false);
        bt_destroy(&bt);
        return;
    }

    if (q.group_by_session) {
        bt_printf_force(&bt, "Matched %u sessions (%u unique hits, %u duplicates folded, %.3f ms):\n\n",
                        sr.session_group_count, sr.total_matches, sr.duplicates_folded, sr.elapsed_ms);
        char tbuf[32];
        for (uint32_t i = 0; i < sr.session_group_count; ++i) {
            const StelogSessionHit *g = &sr.session_groups[i];
            if (!bt_can_append(&bt, 160 + g->title.len + g->best_snippet.len, 3)) break;
            fmt_ts_iso(g->last_hit_ts_ms, tbuf, sizeof(tbuf));
            uint32_t sid_s = g->session_id.len > 8 ? 8 : g->session_id.len;
            bt_printf_force(&bt, "@%.*s [%-5s %s] (%u hits) | proj=%.*s | %.*s\n  best doc_id=%" PRIu64 ": ",
                            (int)sid_s, g->session_id.ptr,
                            stelog_agent_name(g->agent), tbuf, g->hit_count,
                            (int)g->project.len, g->project.ptr,
                            (int)g->title.len, g->title.ptr,
                            g->best_doc_id);
            bt_append_sanitized(&bt, g->best_snippet.ptr, g->best_snippet.len);
            bt_append_raw(&bt, "\n\n", 2);
        }
        bt_printf_force(&bt, "Next: call get_session_digest(session=\"@<id>\") to read the full decision stream of any session.\n");
        send_rpc_tool_text(w, req, bt.buf, bt.len, false);
        bt_destroy(&bt);
        return;
    }

    if (q.count_by != STELOG_COUNT_BY_NONE) {
        bt_printf_force(&bt, "Grouped counts (%u total matches, %u buckets, %.3f ms):\n", sr.total_matches, sr.bucket_count, sr.elapsed_ms);
        char tbuf[32];
        for (uint32_t i = 0; i < sr.bucket_count; ++i) {
            if (!bt_can_append(&bt, sr.buckets[i].key.len + sr.buckets[i].meta_title.len + 64, 1)) break;
            if (q.count_by == STELOG_COUNT_BY_SESSION) {
                fmt_ts_iso(sr.buckets[i].meta_last_ts_ms, tbuf, sizeof(tbuf));
                uint32_t sid_s = sr.buckets[i].key.len > 8 ? 8 : sr.buckets[i].key.len;
                bt_printf_force(&bt, "%6u  @%.*s [%-5s %s] %.*s\n",
                                sr.buckets[i].count,
                                (int)sid_s, sr.buckets[i].key.ptr,
                                stelog_agent_name(sr.buckets[i].meta_agent), tbuf,
                                (int)sr.buckets[i].meta_title.len, sr.buckets[i].meta_title.ptr);
            } else {
                bt_printf_force(&bt, "%6u  %.*s\n", sr.buckets[i].count, (int)sr.buckets[i].key.len, sr.buckets[i].key.ptr);
            }
        }
        send_rpc_tool_text(w, req, bt.buf, bt.len, false);
        bt_destroy(&bt);
        return;
    }

    bt_printf_force(&bt, "Found %u matches (%.3f ms)%s:\n\n",
                    sr.total_matches, sr.elapsed_ms, is_timeline ? " [oldest-first timeline]" : "");

    uint32_t shown = 0;
    char tbuf[32];
    for (uint32_t i = 0; i < sr.count; ++i) {
        const StelogHit *h = &sr.hits[i];
        size_t est = 160 + h->actor.len + h->project.len + h->snippet.len;
        if (!bt_can_append(&bt, est, 3)) break;
        fmt_ts_iso(h->ts_ms, tbuf, sizeof(tbuf));
        uint32_t sid_short = h->session_id.len > 8 ? 8 : h->session_id.len;
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
    }

    if (bt.truncated || sr.truncated) {
        bt_printf_force(&bt, "[truncated by %s: showing %u/%u matches. Next: call %s(..., offset=%u), get_session_context(doc_id=...), or get_document(doc_id=...)]\n",
                        bt.truncated ? bt.truncated_by : "limit",
                        shown, sr.total_matches,
                        is_timeline ? "get_decision_timeline" : "search_history",
                        q.offset + shown);
    }

    send_rpc_tool_text(w, req, bt.buf, bt.len, false);
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
        send_rpc_tool_text(w, req, "doc_id not found in index.", 26, true);
        return;
    }

    BoundedText bt;
    uint32_t text_budget = req->arg_max_bytes ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    bt_printf_force(&bt, "Session %.*s | proj=%.*s | title=%.*s | %u turns (%.3f ms)\n\n",
                    (int)cr.session_id.len, cr.session_id.ptr,
                    (int)cr.project.len, cr.project.ptr,
                    (int)cr.title.len, cr.title.ptr, cr.count, cr.elapsed_ms);

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
    }
    if (bt.truncated) {
        bt_printf_force(&bt, "[truncated by %s: use get_document(doc_id=...) to read any specific turn in full]\n", bt.truncated_by);
    }
    send_rpc_tool_text(w, req, bt.buf, bt.len, false);
    bt_destroy(&bt);
}

static void handle_digest_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    if (req->arg_session.len == 0) {
        send_rpc_tool_text(w, req, "Missing required argument: session (e.g. \"@30c3cab1\")", 54, true);
        return;
    }
    uint32_t budget = req->arg_max_bytes ? req->arg_max_bytes : 7200u;
    if (budget > MCP_MAX_TEXT_BYTES - 1024u) budget = MCP_MAX_TEXT_BYTES - 1024u;

    StelogDigestResult dr;
    if (stelog_index_digest(idx, req->arg_session.ptr, budget, req->arg_include_tools, &dr) != 0) {
        send_rpc_tool_text(w, req, "Session not found.", 18, true);
        return;
    }

    BoundedText bt;
    uint32_t text_budget = req->arg_max_bytes ? MCP_MAX_TEXT_BYTES : (MCP_INLINE_SOFT_BYTES + 200u);
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    bt_printf_force(&bt, "Digest @%.*s [%s] | proj=%.*s | %u/%u dialogue turns (%u tools collapsed, %u retries deduped, %.3f ms)\n\n",
                    (int)dr.session_id.len, dr.session_id.ptr,
                    stelog_agent_name(dr.agent),
                    (int)dr.project.len, dr.project.ptr,
                    dr.count, dr.total_dialogue_turns,
                    dr.total_tools_collapsed, dr.duplicates_skipped, dr.elapsed_ms);

    char tbuf[32];
    for (uint32_t i = 0; i < dr.count; ++i) {
        const StelogHit *h = &dr.items[i];
        if (!bt_can_append(&bt, 120 + h->snippet.len, 3)) break;
        if (h->collapsed_tools_before > 0) {
            bt_printf_force(&bt, "  [...%u tool calls]\n", h->collapsed_tools_before);
        }
        fmt_ts_iso(h->ts_ms, tbuf, sizeof(tbuf));
        bt_printf_force(&bt, "[%s] doc_id=%" PRIu64 " %s (%.*s): ",
                        tbuf, h->doc_id,
                        stelog_role_name(stelog_tag_role(h->tag)),
                        (int)h->actor.len, h->actor.ptr);
        bt_append_sanitized(&bt, h->snippet.ptr, h->snippet.len);
        bt_append_raw(&bt, "\n", 1);
    }
    if (dr.truncated || bt.truncated) {
        bt_printf_force(&bt, "\n[truncated: call get_document(doc_id=...) to expand any specific turn]\n");
    }
    send_rpc_tool_text(w, req, bt.buf, bt.len, false);
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
    char tbuf[32];

    for (uint32_t i = 0; i < id_count; ++i) {
        if (i > 0) bt_append_raw(&bt, "\n", 1);
        size_t rem_bt = (bt.max_bytes > bt.len + 256u) ? (bt.max_bytes - bt.len - 256u) : 0u;
        if (rem_bt < 256u) {
            bt_printf_force(&bt, "[batch budget reached after %u/%u docs; fetch remaining doc_ids in another call]\n",
                            i, id_count);
            break;
        }
        uint32_t this_lim = per_doc_lim < (uint32_t)rem_bt ? per_doc_lim : (uint32_t)rem_bt;
        uint32_t this_off = (id_count == 1) ? req->arg_offset : 0u;

        StelogDocResult dr;
        if (stelog_index_get_document(idx, ids[i], req->arg_raw, this_off, this_lim, req->arg_tail, &dr) != 0) {
            if (id_count == 1) {
                bt_destroy(&bt);
                send_rpc_tool_text(w, req, "Failed to read document (invalid doc_id or compressed raw file).", 63, true);
                return;
            }
            bt_printf_force(&bt, "doc_id=%" PRIu64 " | ERROR: not found or compressed raw file\n", ids[i]);
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
        if (dr.truncated) {
            uint32_t next_off = dr.returned_offset + dr.content.len;
            uint32_t rem_bytes = dr.total_bytes > next_off ? (dr.total_bytes - next_off) : 0u;
            bt_printf_force(&bt, "---\n[truncated: next call get_document(doc_id=%" PRIu64 ", offset=%u) for remaining %uB]\n",
                            dr.doc_id, next_off, rem_bytes);
        }
    }
    send_rpc_tool_text(w, req, bt.buf, bt.len, false);
    bt_destroy(&bt);
}

static void handle_sessions_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    StelogSessionListResult sl;
    stelog_index_list_sessions(idx,
                               parse_agent_csv(req->arg_agent),
                               req->arg_project.len ? req->arg_project.ptr : NULL,
                               parse_time_arg(req->arg_since),
                               parse_time_arg(req->arg_until),
                               req->arg_limit ? req->arg_limit : 20u,
                               req->arg_offset,
                               &sl);

    BoundedText bt;
    uint32_t text_budget = (req->arg_limit > 20u) ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    bt_printf_force(&bt, "Sessions: %u total (showing %u, %.3f ms)\n\n", sl.total_matches, sl.count, sl.elapsed_ms);
    char tbuf[32];
    for (uint32_t i = 0; i < sl.count; ++i) {
        const StelogSessionSummary *s = &sl.items[i];
        if (!bt_can_append(&bt, 128 + s->project.len + s->title.len, 1)) break;
        fmt_ts_iso(s->last_ts_ms, tbuf, sizeof(tbuf));
        bt_printf_force(&bt, "[%s] %-5s %4u docs | @%.*s | proj=%.*s | %.*s\n",
                        tbuf, stelog_agent_name(s->agent), s->doc_count,
                        (int)s->session_id.len, s->session_id.ptr,
                        (int)s->project.len, s->project.ptr,
                        (int)s->title.len, s->title.ptr);
    }
    send_rpc_tool_text(w, req, bt.buf, bt.len, false);
    bt_destroy(&bt);
}

static void handle_files_tool(StelogIndex *idx, const StelogMcpRequest *req, StelogJsonWriter *w) {
    StelogFileHistoryResult fh;
    stelog_index_files(idx,
                       req->arg_file.len ? req->arg_file.ptr : NULL,
                       parse_agent_csv(req->arg_agent),
                       req->arg_project.len ? req->arg_project.ptr : NULL,
                       req->arg_session.len ? req->arg_session.ptr : NULL,
                       parse_time_arg(req->arg_since),
                       parse_time_arg(req->arg_until),
                       req->arg_no_subagents,
                       req->arg_limit ? req->arg_limit : 30u,
                       req->arg_offset,
                       &fh);

    BoundedText bt;
    uint32_t text_budget = (req->arg_limit > 20u) ? MCP_MAX_TEXT_BYTES : MCP_INLINE_SOFT_BYTES;
    bt_init(&bt, text_budget, MCP_MAX_TEXT_LINES);
    bt_printf_force(&bt, "File modifications: %u matches (showing %u, %.3f ms)\n\n", fh.total_matches, fh.count, fh.elapsed_ms);
    char tbuf[32];
    for (uint32_t i = 0; i < fh.count; ++i) {
        const StelogFileTouch *ft = &fh.items[i];
        if (!bt_can_append(&bt, 140 + ft->file_touched.len + ft->project.len, 1)) break;
        fmt_ts_iso(ft->ts_ms, tbuf, sizeof(tbuf));
        uint32_t sid_s = ft->session_id.len > 8 ? 8 : ft->session_id.len;
        bt_printf_force(&bt, "[%s] doc_id=%" PRIu64 " @%.*s | %-5s | %-10.*s | actor=%.*s | %.*s\n",
                        tbuf, ft->doc_id, (int)sid_s, ft->session_id.ptr,
                        stelog_agent_name(stelog_tag_agent(ft->tag)),
                        (int)ft->tool_name.len, ft->tool_name.ptr,
                        (int)ft->actor.len, ft->actor.ptr,
                        (int)ft->file_touched.len, ft->file_touched.ptr);
    }
    if (fh.truncated || bt.truncated) {
        bt_printf_force(&bt, "[truncated: use get_session_context(doc_id=...) to inspect the decision behind any file edit]\n");
    }
    send_rpc_tool_text(w, req, bt.buf, bt.len, false);
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
            write_rpc_id(&jw, &req);
            stelog_jw_key(&jw, "result");
            stelog_jw_raw(&jw, stelog_mcp_initialize_json, stelog_mcp_initialize_json_len);
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
            stelog_jw_raw(&jw, stelog_mcp_tools_list_json, stelog_mcp_tools_list_json_len);
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
    stelog_jw_destroy(&jw);
    stelog_arena_destroy(&req_arena);
    stelog_json_parser_destroy(jp);
    stelog_index_close(idx);
    return 0;
}
