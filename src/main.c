#define _POSIX_C_SOURCE 200809L
#include "stelog.h"
#include "base/simd.h"
#include "base/time_iso8601.h"
#include "json/json_writer.h"
#include "query/regex_engine.h"
#include "store/segment_reader.h"
#include "store/tokenizer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool g_color = false;

#define C_RESET   (g_color ? "\033[0m"    : "")
#define C_BOLD    (g_color ? "\033[1m"    : "")
#define C_DIM     (g_color ? "\033[2m"    : "")
#define C_CYAN    (g_color ? "\033[36m"   : "")
#define C_GREEN   (g_color ? "\033[32m"   : "")
#define C_YELLOW  (g_color ? "\033[33m"   : "")
#define C_MAGENTA (g_color ? "\033[35m"   : "")
#define C_BLUE    (g_color ? "\033[34m"   : "")
#define C_MATCH   (g_color ? "\033[1;31m" : "")

static void init_color(int argc, char **argv) {
    bool no_color = (getenv("NO_COLOR") != NULL) || !isatty(STDOUT_FILENO);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-color") == 0 || strcmp(argv[i], "--json") == 0) {
            no_color = true;
            break;
        }
    }
    g_color = !no_color;
}

static const char *role_color(StelogRoleKind r) {
    switch (r) {
        case STELOG_ROLE_USER:        return C_GREEN;
        case STELOG_ROLE_ASSISTANT:   return C_CYAN;
        case STELOG_ROLE_INTER_AGENT: return C_MAGENTA;
        case STELOG_ROLE_TOOL_CALL:   return C_YELLOW;
        default:                      return C_DIM;
    }
}

static void print_usage(void) {
    printf("stelog 0.2.0 - Fast DOD coding-agent history search, decision archaeology & MCP server\n\n"
           "Usage:\n"
           "  stelog search <query> [TARGET] [flags]   Search decisions (`A | B`, `-term`, `prefix*`, inline `agent:`)\n"
           "  stelog digest @<session> [--budget N] [--offset T]  Condensed user<->assistant decision stream of a session\n"
           "  stelog timeline <query> [TARGET] [flags] Chronological (oldest-first) decision timeline\n"
           "  stelog files [file_substr] [TARGET]      Find who edited/created a file and when\n"
           "  stelog context <doc_id> [-w N] [--json]  Show surrounding dialogue turns (collapses tools by default)\n"
           "  stelog show <doc_id> [--raw] [--tail]    Show full document or raw JSONL line\n"
           "  stelog sessions|list [TARGET] [flags]    List sessions with auto-extracted titles\n"
           "  stelog mcp [--index-dir DIR]             Run stdio JSON-RPC 2.0 MCP Server\n"
           "  stelog index [--rebuild|--compact]       Build or incrementally refresh DOD index\n"
           "  stelog stats [--json]                    Show Base/Delta index stats\n"
           "  stelog bench [--iters N]                 5-suite query latency & zero-malloc regression benchmark\n"
           "  stelog scan [opts]                       Phase 1 raw parallel ingest benchmark\n\n"
           "Query Syntax (ripgrep-inspired):\n"
           "  A | B / A OR B      Bilingual or alternative OR branches\n"
           "  -term / -\"phrase\"   Exclude documents matching term or exact phrase\n"
           "  prefix*             Word-start prefix wildcard (SIMD contiguous text.bin scan)\n"
           "  agent:agy role:user Inline filters: agent:, role:, -role:, project:, actor:, session:, since:, until:, @sid8\n\n"
           "Positional TARGET shortcuts (inspired by csift):\n"
           "  .                   Scope to current working directory\n"
           "  @<session-prefix>   Scope to session(s) matching UUID prefix\n"
           "  /<path>             Scope to project path substring\n\n"
           "Filters & Output Modes:\n"
           "  -S, --smart-case          Smart case (default: case-sensitive if query has uppercase)\n"
           "  -i, --ignore-case         Force case-insensitive search\n"
           "  -s, --case-sensitive      Force case-sensitive search\n"
           "  -F, --fixed-strings       Literal substring search (disables syntax/wildcard parsing)\n"
           "  -E, --regex, -P           PCRE2 JIT regex search (auto literal/SwissTable prefilter)\n"
           "  -C <N> / -B <N> / -A <N>  Show N surrounding dialogue turns around each search hit\n"
           "  --agent <csv>             codex, agy, pi, droid (repeatable)\n"
           "  -t, --role <csv>          user, assistant, inter_agent, tool_call, tool_result, thinking, system\n"
           "  -T, --exclude-role <csv>  Exclude specified roles\n"
           "  --project <substr>        Match project/cwd substring\n"
           "  --actor <substr>          Match actor/model substring\n"
           "  --no-subagents            Exclude subagent transcripts\n"
           "  --since <ISO8601|Nd|Nh>   Start time (e.g. 7d, 24h, 2026-11-01T00:00:00Z)\n"
           "  --until <ISO8601>         End time\n"
           "  --sort <relevance|oldest|newest>\n"
           "  --first <N> / --last <N> / --max-count <N|-N>\n"
           "  -c, --count               Print total match count only\n"
           "  -l, --sessions-only       Print unique matching session IDs (one per line)\n"
           "  --count-by <axis>         Histogram by: agent, role, project, actor, session\n"
           "  --group-by session        Group hits by session with date, title & best snippet\n"
           "  --no-dedup                Keep exact duplicate copies across forks/retries\n"
           "  --include-tools           Include tool_result records\n"
           "  --include-thinking        Include thinking/reasoning summaries\n"
           "  --limit <N> --offset <N>  Pagination\n"
           "  --json                    Structured JSON output\n"
           "  --no-sync                 Skip automatic pre-query Hot Delta refresh\n"
           "  --no-color                Disable ANSI colors\n"
           "  --index-dir <DIR>         Override index directory\n");
}

static void fmt_ts(int64_t ts_ms, char *out, size_t cap) {
    if (ts_ms <= 0) { snprintf(out, cap, "unknown-time"); return; }
    time_t sec = (time_t)(ts_ms / 1000ll);
    struct tm tm_buf;
    localtime_r(&sec, &tm_buf);
    strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm_buf);
}

static int64_t parse_time_arg_or_exit(const char *flag, const char *s, bool end_of_range) {
    if (!s || !s[0]) return 0;
    int64_t ms = 0;
    if (!stelog_parse_time_arg(s, strlen(s), end_of_range, &ms)) {
        fprintf(stderr,
                "stelog: invalid %s value '%s'. Use 30m/24h/7d/2w, today, yesterday, YYYY-MM-DD, "
                "or YYYY-MM-DDTHH:MM[:SS] (local time unless Z or +HH:MM is given).\n",
                flag, s);
        exit(2);
    }
    return ms;
}

static uint32_t parse_agent_csv(const char *s) {
    uint32_t mask = 0;
    const char *p = s;
    while (p && *p) {
        const char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
        if (l == 5 && memcmp(p, "codex", 5) == 0) mask |= (1u << STELOG_AGENT_CODEX);
        else if (l == 3 && memcmp(p, "agy", 3) == 0) mask |= (1u << STELOG_AGENT_AGY);
        else if (l == 2 && memcmp(p, "pi", 2) == 0) mask |= (1u << STELOG_AGENT_PI);
        else if (l == 5 && memcmp(p, "droid", 5) == 0) mask |= (1u << STELOG_AGENT_DROID);
        if (!c) break;
        p = c + 1;
    }
    return mask;
}

static uint32_t parse_role_csv(const char *s) {
    uint32_t mask = 0;
    const char *p = s;
    while (p && *p) {
        const char *c = strchr(p, ',');
        size_t l = c ? (size_t)(c - p) : strlen(p);
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

static StelogCountByAxis parse_count_by(const char *s) {
    if (!s) return STELOG_COUNT_BY_NONE;
    if (strcmp(s, "agent") == 0) return STELOG_COUNT_BY_AGENT;
    if (strcmp(s, "role") == 0) return STELOG_COUNT_BY_ROLE;
    if (strcmp(s, "project") == 0) return STELOG_COUNT_BY_PROJECT;
    if (strcmp(s, "actor") == 0) return STELOG_COUNT_BY_ACTOR;
    if (strcmp(s, "session") == 0) return STELOG_COUNT_BY_SESSION;
    return STELOG_COUNT_BY_NONE;
}

/* Apply a positional TARGET shortcut (`.`, `@prefix`, or `/path`) */
static bool apply_target_arg(const char *arg,
                             char *cwd_buf,
                             size_t cwd_cap,
                             const char **io_project,
                             const char **io_session) {
    if (strcmp(arg, ".") == 0) {
        if (getcwd(cwd_buf, cwd_cap)) *io_project = cwd_buf;
        return true;
    }
    if (arg[0] == '@' && arg[1] != '\0') {
        *io_session = arg + 1;
        return true;
    }
    if (arg[0] == '/') {
        *io_project = arg;
        return true;
    }
    return false;
}

static void print_sanitized(const char *s, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)s[i];
        putchar((c == '\n' || c == '\r' || c == '\t') ? ' ' : (int)c);
    }
}

typedef struct {
    const char *ptrs[32];
    uint32_t    lens[32];
    uint8_t     cs[32];
    uint32_t    count;
    StelogRegex        *re;
    StelogRegexMatcher *re_matcher;
} HighlightList;

static void highlight_list_destroy(HighlightList *hl) {
    if (!hl) return;
    if (hl->re_matcher) stelog_regex_matcher_destroy(hl->re_matcher);
    if (hl->re) stelog_regex_destroy(hl->re);
    memset(hl, 0, sizeof(*hl));
}

static void collect_highlights(const StelogQueryPlan *plan, HighlightList *hl) {
    memset(hl, 0, sizeof(*hl));
    if (!plan) return;
    for (uint32_t b = 0; b < plan->branch_count; ++b) {
        const StelogQueryTokens *bt = &plan->branches[b];
        for (uint32_t i = 0; i < bt->highlight_count && hl->count < 32; ++i) {
            bool dup = false;
            for (uint32_t k = 0; k < hl->count; ++k) {
                if (hl->lens[k] == bt->highlight_lens[i] &&
                    memcmp(hl->ptrs[k], bt->highlight_ptrs[i], hl->lens[k]) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup && bt->highlight_lens[i] > 0) {
                hl->ptrs[hl->count] = bt->highlight_ptrs[i];
                hl->lens[hl->count] = bt->highlight_lens[i];
                hl->cs[hl->count]   = bt->highlight_cs[i];
                hl->count++;
            }
        }
    }
}

/* Render sanitized snippet with bold-red ANSI highlights on matched query/regex spans (Top-K display only!) */
static void print_sanitized_highlighted(const char *s, uint32_t len, const HighlightList *hl) {
    if (!g_color || !hl || len == 0 || (hl->count == 0 && !hl->re)) {
        print_sanitized(s, len);
        return;
    }
    if (hl->re && hl->re_matcher) {
        size_t pos = 0;
        while (pos < len) {
            size_t ms = 0, me = 0;
            if (!stelog_regex_find(hl->re, hl->re_matcher, s, len, pos, &ms, &me)) {
                print_sanitized(s + pos, len - (uint32_t)pos);
                break;
            }
            if (ms > pos) print_sanitized(s + pos, (uint32_t)(ms - pos));
            if (me > ms) {
                fputs(C_MATCH, stdout);
                print_sanitized(s + ms, (uint32_t)(me - ms));
                fputs(C_RESET, stdout);
                pos = me;
            } else {
                /* Zero-length match guard */
                if (ms < len) {
                    print_sanitized(s + ms, 1);
                    pos = ms + 1;
                } else {
                    break;
                }
            }
        }
        return;
    }
    uint32_t pos = 0;
    while (pos < len) {
        const char *best_hit = NULL;
        uint32_t best_len = 0;
        for (uint32_t k = 0; k < hl->count; ++k) {
            const char *hit = stelog_find_substr(s + pos, len - pos, hl->ptrs[k], hl->lens[k], hl->cs[k] != 0);
            if (hit && (!best_hit || hit < best_hit || (hit == best_hit && hl->lens[k] > best_len))) {
                best_hit = hit;
                best_len = hl->lens[k];
            }
        }
        if (!best_hit) {
            print_sanitized(s + pos, len - pos);
            break;
        }
        uint32_t off = (uint32_t)(best_hit - s);
        if (off > pos) print_sanitized(s + pos, off - pos);
        fputs(C_MATCH, stdout);
        print_sanitized(s + off, best_len);
        fputs(C_RESET, stdout);
        pos = off + best_len;
    }
}

static void write_hit_json(StelogJsonWriter *w, const StelogHit *h) {
    stelog_jw_obj_begin(w);
    stelog_jw_kv_u64(w, "doc_id", h->doc_id);
    stelog_jw_kv_int(w, "ts_ms", h->ts_ms);
    stelog_jw_kv_double(w, "score", (double)h->score);
    stelog_jw_kv_cstr(w, "agent", stelog_agent_name(stelog_tag_agent(h->tag)));
    stelog_jw_kv_cstr(w, "role", stelog_role_name(stelog_tag_role(h->tag)));
    stelog_jw_kv_bool(w, "is_subagent", (stelog_tag_flags(h->tag) & STELOG_FLAG_IS_SUBAGENT) != 0);
    stelog_jw_kv_str(w, "session_id", h->session_id.ptr, h->session_id.len);
    stelog_jw_kv_str(w, "title", h->title.ptr, h->title.len);
    stelog_jw_kv_str(w, "project", h->project.ptr, h->project.len);
    stelog_jw_kv_str(w, "actor", h->actor.ptr, h->actor.len);
    stelog_jw_kv_str(w, "snippet", h->snippet.ptr, h->snippet.len);
    stelog_jw_kv_bool(w, "truncated", h->truncated);
    stelog_jw_kv_u64(w, "full_text_len", h->full_text_len);
    stelog_jw_obj_end(w);
}

static StelogIndex *open_with_optional_sync(const char *index_dir, bool no_sync) {
    if (!no_sync) {
        StelogIndexConfig cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.index_dir = index_dir;
        stelog_index_sync(&cfg, NULL);
    }
    return stelog_index_open(index_dir);
}

/* ============================================================================
 * Commands
 * ============================================================================ */

static int cmd_scan(int argc, char **argv) {
    StelogScanConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) cfg.agent_mask |= parse_agent_csv(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) cfg.threads = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--chunk-mb") == 0 && i + 1 < argc) cfg.chunk_bytes = (uint32_t)strtoul(argv[++i], NULL, 10) * 1024u * 1024u;
        else if (strcmp(argv[i], "--include-tools") == 0) cfg.include_tool_results = true;
        else if (strcmp(argv[i], "--include-thinking") == 0) cfg.include_thinking = true;
        else if (strcmp(argv[i], "--include-system") == 0) cfg.include_system = true;
    }
    StelogScanReport rep;
    if (stelog_run_scan(&cfg, &rep, NULL) != 0) return 1;

    uint64_t tot_bytes = 0, tot_skip = 0, tot_events = 0, tot_text = 0;
    for (int a = 0; a < STELOG_AGENT_COUNT; ++a) {
        tot_bytes += rep.per_agent[a].bytes_read;
        tot_skip  += rep.per_agent[a].bytes_skipped_prefilter;
        tot_events += rep.per_agent[a].events_emitted;
        tot_text  += rep.per_agent[a].text_bytes_emitted;
    }
    double gbps = rep.scan_ms > 0.0 ? ((double)tot_bytes / (1024.0 * 1024.0 * 1024.0)) / (rep.scan_ms / 1000.0) : 0.0;
    printf("Scan: workers=%u total=%.2f ms (scan=%.2f ms) read=%.2f MB skip=%.1f%% events=%" PRIu64 " text=%.2f MB (%.2f GB/s)\n",
           rep.worker_threads, rep.elapsed_ms, rep.scan_ms,
           (double)tot_bytes / 1048576.0, tot_bytes ? 100.0 * (double)tot_skip / (double)tot_bytes : 0.0,
           tot_events, (double)tot_text / 1048576.0, gbps);
    return 0;
}

static int cmd_index(int argc, char **argv) {
    StelogIndexConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    bool json_out = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--rebuild") == 0) cfg.force_rebuild = true;
        else if (strcmp(argv[i], "--compact") == 0) cfg.force_compact = true;
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) cfg.index_dir = argv[++i];
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) cfg.threads = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--include-tools") == 0) cfg.include_tool_results = true;
        else if (strcmp(argv[i], "--include-thinking") == 0) cfg.include_thinking = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
    }
    StelogIndexReport rep;
    if (stelog_index_sync(&cfg, &rep) != 0) {
        fprintf(stderr, "stelog index failed\n");
        return 1;
    }
    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 1024);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_bool(&w, "up_to_date", rep.noop_up_to_date);
        stelog_jw_kv_u64(&w, "base_gen", rep.base_gen);
        stelog_jw_kv_u64(&w, "delta_gen", rep.delta_gen);
        stelog_jw_kv_u64(&w, "base_docs", rep.base_docs);
        stelog_jw_kv_u64(&w, "delta_docs", rep.delta_docs);
        stelog_jw_kv_u64(&w, "new_docs_added", rep.new_docs_added);
        stelog_jw_kv_u64(&w, "disk_bytes", rep.bytes_on_disk);
        stelog_jw_kv_double(&w, "total_ms", rep.total_ms);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        return 0;
    }
    if (rep.noop_up_to_date) {
        printf("Index up-to-date in %.2f ms | base_gen=%u (%u docs) delta_gen=%u (%u docs) disk=%.2f MB\n",
               rep.total_ms, rep.base_gen, rep.base_docs, rep.delta_gen, rep.delta_docs,
               (double)rep.bytes_on_disk / 1048576.0);
    } else {
        printf("Index %s in %.2f ms (discover=%.2f ms, ingest=%.2f ms, build=%.2f ms)\n"
               "  base_gen=%u (%u docs, %u terms) | delta_gen=%u (%u docs, %u terms)\n"
               "  files=%u (changed=%u) | new_docs=%u | disk=%.2f MB\n",
               rep.was_rebuild ? "rebuilt" : (rep.did_compact ? "compacted" : "updated (Hot Delta)"),
               rep.total_ms, rep.discover_ms, rep.ingest_ms, rep.build_ms,
               rep.base_gen, rep.base_docs, rep.unique_terms_base,
               rep.delta_gen, rep.delta_docs, rep.unique_terms_delta,
               rep.files_total, rep.files_changed, rep.new_docs_added,
               (double)rep.bytes_on_disk / 1048576.0);
    }
    return 0;
}

static int cmd_search_or_timeline(int argc, char **argv, bool is_timeline) {
    StelogQuery q;
    memset(&q, 0, sizeof(q));
    q.sort_mode = is_timeline ? STELOG_SORT_OLDEST : STELOG_SORT_RELEVANCE;

    const char *index_dir = NULL;
    bool no_sync = false, json_out = false, count_only = false, end_of_opts = false;
    uint32_t ctx_before = 0, ctx_after = 0;
    char cwd_buf[1024];

    for (int i = 2; i < argc; ++i) {
        if (!end_of_opts && strcmp(argv[i], "--") == 0) { end_of_opts = true; continue; }
        if (!end_of_opts && strcmp(argv[i], "--agent") == 0 && i + 1 < argc) q.agent_mask |= parse_agent_csv(argv[++i]);
        else if (!end_of_opts && (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--role") == 0) && i + 1 < argc) q.role_mask |= parse_role_csv(argv[++i]);
        else if (!end_of_opts && (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "--exclude-role") == 0) && i + 1 < argc) q.exclude_role_mask |= parse_role_csv(argv[++i]);
        else if (!end_of_opts && (strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--smart-case") == 0)) q.case_mode = STELOG_CASE_SMART;
        else if (!end_of_opts && (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--ignore-case") == 0)) q.case_mode = STELOG_CASE_INSENSITIVE;
        else if (!end_of_opts && (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--case-sensitive") == 0)) q.case_mode = STELOG_CASE_SENSITIVE;
        else if (!end_of_opts && (strcmp(argv[i], "-F") == 0 || strcmp(argv[i], "--fixed-strings") == 0)) q.fixed_strings = true;
        else if (!end_of_opts && (strcmp(argv[i], "-E") == 0 || strcmp(argv[i], "--regex") == 0 || strcmp(argv[i], "-P") == 0)) q.regex_mode = true;
        else if (!end_of_opts && (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "--context") == 0) && i + 1 < argc) {
            uint32_t cv = (uint32_t)strtoul(argv[++i], NULL, 10);
            ctx_before = cv; ctx_after = cv;
        } else if (!end_of_opts && strcmp(argv[i], "-B") == 0 && i + 1 < argc) {
            ctx_before = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!end_of_opts && strcmp(argv[i], "-A") == 0 && i + 1 < argc) {
            ctx_after = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
        else if (!end_of_opts && strcmp(argv[i], "--project") == 0 && i + 1 < argc) q.project_substr = argv[++i];
        else if (!end_of_opts && strcmp(argv[i], "--actor") == 0 && i + 1 < argc) q.actor_substr = argv[++i];
        else if (!end_of_opts && strcmp(argv[i], "--session") == 0 && i + 1 < argc) q.session_id = argv[++i];
        else if (!end_of_opts && strcmp(argv[i], "--since") == 0 && i + 1 < argc) q.since_ms = parse_time_arg_or_exit("--since", argv[++i], false);
        else if (!end_of_opts && strcmp(argv[i], "--until") == 0 && i + 1 < argc) q.until_ms = parse_time_arg_or_exit("--until", argv[++i], true);
        else if (!end_of_opts && strcmp(argv[i], "--limit") == 0 && i + 1 < argc) q.limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!end_of_opts && strcmp(argv[i], "--offset") == 0 && i + 1 < argc) q.offset = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!end_of_opts && strcmp(argv[i], "--first") == 0 && i + 1 < argc) {
            q.sort_mode = STELOG_SORT_OLDEST;
            q.limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!end_of_opts && strcmp(argv[i], "--last") == 0 && i + 1 < argc) {
            q.sort_mode = STELOG_SORT_NEWEST;
            q.limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!end_of_opts && strcmp(argv[i], "--max-count") == 0 && i + 1 < argc) {
            long v = strtol(argv[++i], NULL, 10);
            if (v < 0) { q.sort_mode = STELOG_SORT_NEWEST; q.limit = (uint32_t)(-v); }
            else if (v > 0) { q.sort_mode = STELOG_SORT_OLDEST; q.limit = (uint32_t)v; }
        } else if (!end_of_opts && strcmp(argv[i], "--sort") == 0 && i + 1 < argc) {
            const char *sm = argv[++i];
            if (strcmp(sm, "oldest") == 0) q.sort_mode = STELOG_SORT_OLDEST;
            else if (strcmp(sm, "newest") == 0) q.sort_mode = STELOG_SORT_NEWEST;
            else q.sort_mode = STELOG_SORT_RELEVANCE;
        } else if (!end_of_opts && strcmp(argv[i], "--count-by") == 0 && i + 1 < argc) {
            q.count_by = parse_count_by(argv[++i]);
        } else if (!end_of_opts && strcmp(argv[i], "--group-by") == 0 && i + 1 < argc) {
            const char *gb = argv[++i];
            if (strcmp(gb, "session") == 0) q.group_by_session = true;
        } else if (!end_of_opts && strcmp(argv[i], "--no-dedup") == 0) q.no_dedup = true;
        else if (!end_of_opts && (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--count") == 0)) count_only = true;
        else if (!end_of_opts && (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--sessions-only") == 0)) q.sessions_only = true;
        else if (!end_of_opts && strcmp(argv[i], "--no-subagents") == 0) q.no_subagents = true;
        else if (!end_of_opts && strcmp(argv[i], "--include-tools") == 0) q.role_mask = (q.role_mask ? q.role_mask : STELOG_DEFAULT_ROLE_MASK) | (1u << STELOG_ROLE_TOOL_RESULT);
        else if (!end_of_opts && strcmp(argv[i], "--include-thinking") == 0) q.role_mask = (q.role_mask ? q.role_mask : STELOG_DEFAULT_ROLE_MASK) | (1u << STELOG_ROLE_THINKING);
        else if (!end_of_opts && strcmp(argv[i], "--no-sync") == 0) no_sync = true;
        else if (!end_of_opts && strcmp(argv[i], "--json") == 0) json_out = true;
        else if (!end_of_opts && strcmp(argv[i], "--no-color") == 0) { /* handled */ }
        else if (!end_of_opts && strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (end_of_opts || argv[i][0] != '-') {
            if (!q.query) q.query = argv[i];
            else apply_target_arg(argv[i], cwd_buf, sizeof(cwd_buf), &q.project_substr, &q.session_id);
        }
    }

    StelogIndex *idx = open_with_optional_sync(index_dir, no_sync);
    if (!idx) {
        fprintf(stderr, "Failed to open index. Run `stelog index` first.\n");
        return 1;
    }

    StelogSearchResult sr;
    if (stelog_index_search(idx, &q, &sr) != 0) {
        if (sr.error_msg.len > 0) {
            fprintf(stderr, "stelog: %.*s\n", (int)sr.error_msg.len, sr.error_msg.ptr);
        }
        stelog_index_close(idx);
        return 1;
    }

    if (count_only) {
        printf("%u\n", sr.total_matches);
        stelog_index_close(idx);
        return 0;
    }

    if (q.sessions_only) {
        for (uint32_t i = 0; i < sr.matched_session_count; ++i) {
            printf("%.*s\n", (int)sr.matched_sessions[i].len, sr.matched_sessions[i].ptr);
        }
        stelog_index_close(idx);
        return 0;
    }

    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 16384);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_u64(&w, "total_matches", sr.total_matches);
        stelog_jw_kv_u64(&w, "docs_in_scope", sr.docs_in_scope);
        stelog_jw_kv_bool(&w, "truncated", sr.truncated);
        stelog_jw_kv_double(&w, "elapsed_ms", sr.elapsed_ms);
        if (sr.total_matches == 0 && sr.unfiltered_total_matches > 0) {
            stelog_jw_kv_u64(&w, "unfiltered_matches", sr.unfiltered_total_matches);
        }
        if (q.group_by_session) {
            stelog_jw_key(&w, "sessions");
            stelog_jw_arr_begin(&w);
            for (uint32_t i = 0; i < sr.session_group_count; ++i) {
                const StelogSessionHit *g = &sr.session_groups[i];
                stelog_jw_obj_begin(&w);
                stelog_jw_kv_str(&w, "session_id", g->session_id.ptr, g->session_id.len);
                stelog_jw_kv_cstr(&w, "agent", stelog_agent_name(g->agent));
                stelog_jw_kv_u64(&w, "hit_count", g->hit_count);
                stelog_jw_kv_int(&w, "last_hit_ts_ms", g->last_hit_ts_ms);
                stelog_jw_kv_str(&w, "project", g->project.ptr, g->project.len);
                stelog_jw_kv_str(&w, "title", g->title.ptr, g->title.len);
                stelog_jw_kv_u64(&w, "best_doc_id", g->best_doc_id);
                stelog_jw_kv_str(&w, "best_snippet", g->best_snippet.ptr, g->best_snippet.len);
                stelog_jw_obj_end(&w);
            }
            stelog_jw_arr_end(&w);
        } else if (q.count_by != STELOG_COUNT_BY_NONE) {
            stelog_jw_key(&w, "buckets");
            stelog_jw_arr_begin(&w);
            for (uint32_t i = 0; i < sr.bucket_count; ++i) {
                stelog_jw_obj_begin(&w);
                stelog_jw_kv_str(&w, "key", sr.buckets[i].key.ptr, sr.buckets[i].key.len);
                stelog_jw_kv_u64(&w, "count", sr.buckets[i].count);
                stelog_jw_obj_end(&w);
            }
            stelog_jw_arr_end(&w);
        } else {
            stelog_jw_key(&w, "hits");
            stelog_jw_arr_begin(&w);
            for (uint32_t i = 0; i < sr.count; ++i) write_hit_json(&w, &sr.hits[i]);
            stelog_jw_arr_end(&w);
        }
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        stelog_index_close(idx);
        return 0;
    }

    /* Definitive Absence diagnostics (csift Law 2) */
    if (sr.total_matches == 0) {
        fprintf(stderr, "0 matches across %u indexed docs in scope (%.3f ms).\n", sr.docs_in_scope, sr.elapsed_ms);
        if (sr.unfiltered_total_matches > 0) {
            fprintf(stderr, "Hint: %u matches exist outside current filters (", sr.unfiltered_total_matches);
            bool first = true;
            for (int r = 0; r < STELOG_ROLE_COUNT; ++r) {
                if (sr.other_role_counts[r] > 0) {
                    fprintf(stderr, "%s%s=%u", first ? "" : ", ", stelog_role_name((StelogRoleKind)r), sr.other_role_counts[r]);
                    first = false;
                }
            }
            fprintf(stderr, "). Retry with broader filters or --include-tools.\n");
        } else {
            fprintf(stderr, "Definitive absence in indexed decision layers (default excludes tool_result/thinking/system).\n");
        }
        stelog_index_close(idx);
        return 0;
    }

    HighlightList hl;
    memset(&hl, 0, sizeof(hl));
    if (g_color && q.query && q.query[0]) {
        if (q.regex_mode) {
            hl.re = stelog_regex_compile(q.query, strlen(q.query), q.case_mode, NULL, 0);
            if (hl.re) hl.re_matcher = stelog_regex_matcher_create(hl.re);
        } else {
            StelogQueryPlan hplan;
            stelog_parse_query_plan_ex(q.query, strlen(q.query), q.case_mode, q.fixed_strings, &hplan);
            collect_highlights(&hplan, &hl);
        }
    }

    if (q.group_by_session) {
        printf("%sMatched %u sessions (%u unique hits, %u duplicates folded, %.3f ms)%s\n\n",
               C_DIM, sr.session_group_count, sr.total_matches, sr.duplicates_folded, sr.elapsed_ms, C_RESET);
        char tbuf[32];
        for (uint32_t i = 0; i < sr.session_group_count; ++i) {
            const StelogSessionHit *g = &sr.session_groups[i];
            fmt_ts(g->last_hit_ts_ms, tbuf, sizeof(tbuf));
            uint32_t sid_s = stelog_index_unique_prefix_len(idx, g->session_id, 8);
            printf("%s@%.*s%s [%-5s %s%s%s] %s(%u hits)%s | %s%.*s%s | %s%.*s%s\n  %sbest doc=%" PRIu64 ":%s ",
                   C_BLUE, (int)sid_s, g->session_id.ptr, C_RESET,
                   stelog_agent_name(g->agent), C_DIM, tbuf, C_RESET,
                   C_BOLD, g->hit_count, C_RESET,
                   C_DIM, (int)g->project.len, g->project.ptr, C_RESET,
                   C_GREEN, (int)g->title.len, g->title.ptr, C_RESET,
                   C_DIM, g->best_doc_id, C_RESET);
            print_sanitized_highlighted(g->best_snippet.ptr, g->best_snippet.len, &hl);
            printf("\n\n");
        }
        printf("%sNext: `stelog digest @<id>` to read the full decision stream of any session.%s\n", C_DIM, C_RESET);
        highlight_list_destroy(&hl);
        stelog_index_close(idx);
        return 0;
    }

    if (q.count_by != STELOG_COUNT_BY_NONE) {
        printf("%s%u unique matches across %u buckets (%u duplicates folded, %.3f ms)%s\n",
               C_DIM, sr.total_matches, sr.bucket_count, sr.duplicates_folded, sr.elapsed_ms, C_RESET);
        char tbuf[32];
        for (uint32_t i = 0; i < sr.bucket_count; ++i) {
            if (q.count_by == STELOG_COUNT_BY_SESSION) {
                fmt_ts(sr.buckets[i].meta_last_ts_ms, tbuf, sizeof(tbuf));
                uint32_t sid_s = stelog_index_unique_prefix_len(idx, sr.buckets[i].key, 8);
                printf("%6u  %s@%.*s%s [%-5s %s] %.*s\n",
                       sr.buckets[i].count,
                       C_BLUE, (int)sid_s, sr.buckets[i].key.ptr, C_RESET,
                       stelog_agent_name(sr.buckets[i].meta_agent), tbuf,
                       (int)sr.buckets[i].meta_title.len, sr.buckets[i].meta_title.ptr);
            } else {
                printf("%6u  %.*s\n", sr.buckets[i].count, (int)sr.buckets[i].key.len, sr.buckets[i].key.ptr);
            }
        }
        highlight_list_destroy(&hl);
        stelog_index_close(idx);
        return 0;
    }

    printf("%sFound %u unique matches (showing %u, %u duplicates folded, %.3f ms)%s\n\n",
           C_DIM, sr.total_matches, sr.count, sr.duplicates_folded, sr.elapsed_ms, C_RESET);
    char tbuf[32];
    if (ctx_before > 0 || ctx_after > 0) {
        /* Copy doc_ids & dup_counts before calling stelog_index_context (which resets scratch arena) */
        uint32_t show_n = sr.count;
        uint64_t *doc_ids = (uint64_t *)malloc(show_n * sizeof(uint64_t));
        uint32_t *dup_cnts = (uint32_t *)malloc(show_n * sizeof(uint32_t));
        for (uint32_t i = 0; i < show_n; ++i) {
            doc_ids[i] = sr.hits[i].doc_id;
            dup_cnts[i] = sr.hits[i].dup_count;
        }
        uint32_t win = ctx_before > ctx_after ? ctx_before : ctx_after;
        for (uint32_t i = 0; i < show_n; ++i) {
            StelogContextResult cr;
            if (stelog_index_context(idx, doc_ids[i], win, 600, false, &cr) != 0) continue;
            uint32_t lo_i = (cr.anchor_index > ctx_before) ? (cr.anchor_index - ctx_before) : 0u;
            uint32_t hi_i = cr.anchor_index + ctx_after;
            if (hi_i >= cr.count) hi_i = cr.count - 1u;

            uint32_t sid_s = stelog_index_unique_prefix_len(idx, cr.session_id, 8);
            printf("%s@%.*s%s | %s%.*s%s", C_BLUE, (int)sid_s, cr.session_id.ptr, C_RESET,
                   C_DIM, (int)cr.project.len, cr.project.ptr, C_RESET);
            if (dup_cnts[i] > 0) printf(" %s(+%u dup)%s", C_YELLOW, dup_cnts[i], C_RESET);
            putchar('\n');
            for (uint32_t k = lo_i; k <= hi_i; ++k) {
                const StelogHit *h = &cr.items[k];
                StelogRoleKind rk = stelog_tag_role(h->tag);
                if (k > lo_i && h->collapsed_tools_before > 0) {
                    printf("    %s[...%u tool calls]%s\n", C_DIM, h->collapsed_tools_before, C_RESET);
                }
                fmt_ts(h->ts_ms, tbuf, sizeof(tbuf));
                bool is_anchor = (k == cr.anchor_index);
                printf("%s%s[%s]%s doc=%" PRIu64 " %s%s/%s%s: ",
                       is_anchor ? C_BOLD : C_DIM,
                       is_anchor ? ">>> " : "    ",
                       tbuf, C_RESET, h->doc_id,
                       role_color(rk), stelog_agent_name(stelog_tag_agent(h->tag)), stelog_role_name(rk), C_RESET);
                if (is_anchor) print_sanitized_highlighted(h->snippet.ptr, h->snippet.len, &hl);
                else print_sanitized(h->snippet.ptr, h->snippet.len);
                putchar('\n');
            }
            if (i + 1 < show_n) printf("%s--%s\n", C_DIM, C_RESET);
            else putchar('\n');
        }
        free(doc_ids);
        free(dup_cnts);
    } else {
        for (uint32_t i = 0; i < sr.count; ++i) {
            const StelogHit *h = &sr.hits[i];
            StelogRoleKind rk = stelog_tag_role(h->tag);
            fmt_ts(h->ts_ms, tbuf, sizeof(tbuf));
            uint32_t sid_s = stelog_index_unique_prefix_len(idx, h->session_id, 8);
            printf("%s[%s]%s %sdoc=%" PRIu64 "%s %s@%.*s%s | %s%s/%s%s | actor=%.*s | %s%.*s%s",
                   C_DIM, tbuf, C_RESET,
                   C_BOLD, h->doc_id, C_RESET,
                   C_BLUE, (int)sid_s, h->session_id.ptr, C_RESET,
                   role_color(rk), stelog_agent_name(stelog_tag_agent(h->tag)), stelog_role_name(rk), C_RESET,
                   (int)h->actor.len, h->actor.ptr,
                   C_DIM, (int)h->project.len, h->project.ptr, C_RESET);
            if (h->dup_count > 0) printf(" %s(+%u dup)%s", C_YELLOW, h->dup_count, C_RESET);
            printf("\n  ");
            print_sanitized_highlighted(h->snippet.ptr, h->snippet.len, &hl);
            printf("\n\n");
        }
    }
    if (sr.truncated) {
        printf("%s[truncated: showing %u/%u matches. Next: --offset %u | `stelog context <doc_id>` | `stelog show <doc_id>`]%s\n",
               C_DIM, sr.count, sr.total_matches, q.offset + sr.count, C_RESET);
    }

    highlight_list_destroy(&hl);
    stelog_index_close(idx);
    return 0;
}

static int cmd_files(int argc, char **argv) {
    const char *file_substr = NULL, *proj = NULL, *sess = NULL, *index_dir = NULL;
    uint32_t agent_mask = 0, limit = 30, offset = 0;
    int64_t since = 0, until = 0;
    bool no_subagents = false, no_sync = false, json_out = false;
    char cwd_buf[1024];

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) agent_mask |= parse_agent_csv(argv[++i]);
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) proj = argv[++i];
        else if (strcmp(argv[i], "--session") == 0 && i + 1 < argc) sess = argv[++i];
        else if (strcmp(argv[i], "--since") == 0 && i + 1 < argc) since = parse_time_arg_or_exit("--since", argv[++i], false);
        else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) until = parse_time_arg_or_exit("--until", argv[++i], true);
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--no-subagents") == 0) no_subagents = true;
        else if (strcmp(argv[i], "--no-sync") == 0) no_sync = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
        else if (strcmp(argv[i], "--no-color") == 0) { /* handled */ }
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (argv[i][0] != '-') {
            if (!apply_target_arg(argv[i], cwd_buf, sizeof(cwd_buf), &proj, &sess) && !file_substr) {
                file_substr = argv[i];
            }
        }
    }

    StelogIndex *idx = open_with_optional_sync(index_dir, no_sync);
    if (!idx) return 1;
    StelogFileHistoryResult fh;
    stelog_index_files(idx, file_substr, agent_mask, proj, sess, since, until, no_subagents, limit, offset, &fh);

    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 8192);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_u64(&w, "total_matches", fh.total_matches);
        stelog_jw_kv_double(&w, "elapsed_ms", fh.elapsed_ms);
        stelog_jw_key(&w, "items");
        stelog_jw_arr_begin(&w);
        for (uint32_t i = 0; i < fh.count; ++i) {
            const StelogFileTouch *ft = &fh.items[i];
            stelog_jw_obj_begin(&w);
            stelog_jw_kv_u64(&w, "doc_id", ft->doc_id);
            stelog_jw_kv_int(&w, "ts_ms", ft->ts_ms);
            stelog_jw_kv_cstr(&w, "agent", stelog_agent_name(stelog_tag_agent(ft->tag)));
            stelog_jw_kv_str(&w, "tool", ft->tool_name.ptr, ft->tool_name.len);
            stelog_jw_kv_str(&w, "file", ft->file_touched.ptr, ft->file_touched.len);
            stelog_jw_kv_str(&w, "session_id", ft->session_id.ptr, ft->session_id.len);
            stelog_jw_kv_str(&w, "actor", ft->actor.ptr, ft->actor.len);
            stelog_jw_kv_str(&w, "project", ft->project.ptr, ft->project.len);
            stelog_jw_obj_end(&w);
        }
        stelog_jw_arr_end(&w);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        stelog_index_close(idx);
        return 0;
    }

    printf("%sFile modifications: %u matches (showing %u, %.3f ms)%s\n\n",
           C_DIM, fh.total_matches, fh.count, fh.elapsed_ms, C_RESET);
    char tbuf[32];
    for (uint32_t i = 0; i < fh.count; ++i) {
        const StelogFileTouch *ft = &fh.items[i];
        fmt_ts(ft->ts_ms, tbuf, sizeof(tbuf));
        uint32_t sid_s = stelog_index_unique_prefix_len(idx, ft->session_id, 8);
        printf("%s[%s]%s %sdoc=%" PRIu64 "%s %s@%.*s%s | %-5s | %s%-11.*s%s | %.*s | %s%.*s%s\n",
               C_DIM, tbuf, C_RESET,
               C_BOLD, ft->doc_id, C_RESET,
               C_BLUE, (int)sid_s, ft->session_id.ptr, C_RESET,
               stelog_agent_name(stelog_tag_agent(ft->tag)),
               C_YELLOW, (int)ft->tool_name.len, ft->tool_name.ptr, C_RESET,
               (int)ft->actor.len, ft->actor.ptr,
               C_GREEN, (int)ft->file_touched.len, ft->file_touched.ptr, C_RESET);
    }
    stelog_index_close(idx);
    return 0;
}

static int cmd_context(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: stelog context <doc_id> [-w N] [--include-tools] [--json]\n"); return 1; }
    uint64_t doc_id = strtoull(argv[2], NULL, 10);
    uint32_t window = 5;
    const char *index_dir = NULL;
    bool json_out = false, include_tools = false;
    for (int i = 3; i < argc; ++i) {
        if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--window") == 0) && i + 1 < argc)
            window = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (strcmp(argv[i], "--include-tools") == 0) include_tools = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
    }
    StelogIndex *idx = stelog_index_open(index_dir);
    if (!idx) return 1;
    StelogContextResult cr;
    if (stelog_index_context(idx, doc_id, window, 1800, include_tools, &cr) != 0) {
        fprintf(stderr, "Document %" PRIu64 " not found.\n", doc_id);
        stelog_index_close(idx);
        return 1;
    }
    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 16384);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_str(&w, "session_id", cr.session_id.ptr, cr.session_id.len);
        stelog_jw_kv_str(&w, "title", cr.title.ptr, cr.title.len);
        stelog_jw_kv_str(&w, "project", cr.project.ptr, cr.project.len);
        stelog_jw_kv_u64(&w, "anchor_index", cr.anchor_index);
        stelog_jw_kv_double(&w, "elapsed_ms", cr.elapsed_ms);
        stelog_jw_key(&w, "items");
        stelog_jw_arr_begin(&w);
        for (uint32_t i = 0; i < cr.count; ++i) write_hit_json(&w, &cr.items[i]);
        stelog_jw_arr_end(&w);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        stelog_index_close(idx);
        return 0;
    }

    printf("%sSession %.*s | proj=%.*s | %u dialogue turns (%.3f ms)%s\n\n",
           C_DIM, (int)cr.session_id.len, cr.session_id.ptr,
           (int)cr.project.len, cr.project.ptr, cr.count, cr.elapsed_ms, C_RESET);
    char tbuf[32];
    for (uint32_t i = 0; i < cr.count; ++i) {
        const StelogHit *h = &cr.items[i];
        StelogRoleKind rk = stelog_tag_role(h->tag);
        if (h->collapsed_tools_before > 0) {
            printf("    %s[...%u tool calls collapsed]%s\n", C_DIM, h->collapsed_tools_before, C_RESET);
        }
        fmt_ts(h->ts_ms, tbuf, sizeof(tbuf));
        printf("%s%s[%s]%s doc=%" PRIu64 " %s%s%s (%.*s): ",
               (i == cr.anchor_index) ? C_BOLD : "",
               (i == cr.anchor_index) ? ">>> " : "    ",
               tbuf, C_RESET,
               h->doc_id, role_color(rk), stelog_role_name(rk), C_RESET,
               (int)h->actor.len, h->actor.ptr);
        print_sanitized(h->snippet.ptr, h->snippet.len);
        putchar('\n');
    }
    stelog_index_close(idx);
    return 0;
}

static int cmd_digest(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: stelog digest @<session-prefix> [--budget N] [--offset TURN] [--include-tools] [--json]\n"); return 1; }
    const char *sess = NULL, *index_dir = NULL;
    uint32_t budget = 14336, turn_offset = 0;
    bool include_tools = false, no_sync = false, json_out = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--budget") == 0 && i + 1 < argc) budget = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) turn_offset = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--include-tools") == 0) include_tools = true;
        else if (strcmp(argv[i], "--no-sync") == 0) no_sync = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
        else if (strcmp(argv[i], "--no-color") == 0) { /* handled */ }
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (argv[i][0] != '-' && !sess) sess = argv[i];
    }
    if (!sess) { fprintf(stderr, "Missing session id or @prefix.\n"); return 1; }
    StelogIndex *idx = open_with_optional_sync(index_dir, no_sync);
    if (!idx) return 1;
    StelogDigestOptions dopts;
    memset(&dopts, 0, sizeof(dopts));
    dopts.session = sess;
    dopts.max_total_bytes = budget;
    dopts.turn_offset = turn_offset;
    dopts.include_tools = include_tools;
    StelogDigestResult dr;
    int drc = stelog_index_digest_ex(idx, &dopts, &dr);
    if (drc == -2) {
        fprintf(stderr, "Ambiguous session prefix '%s': %u sessions match. Use a longer prefix:\n", sess, dr.match_count);
        char t1[32], t2[32];
        for (uint32_t i = 0; i < dr.candidate_count; ++i) {
            const StelogSessionSummary *c = &dr.candidates[i];
            fmt_ts(c->first_ts_ms, t1, sizeof(t1));
            fmt_ts(c->last_ts_ms, t2, sizeof(t2));
            uint32_t sl = stelog_index_unique_prefix_len(idx, c->session_id, 8);
            fprintf(stderr, "  @%.*s [%s %s .. %s] %u docs | %.*s | %.*s\n",
                    (int)sl, c->session_id.ptr, stelog_agent_name(c->agent), t1, t2, c->doc_count,
                    (int)c->project.len, c->project.ptr, (int)c->title.len, c->title.ptr);
        }
        if (dr.match_count > dr.candidate_count) fprintf(stderr, "  ... and %u more\n", dr.match_count - dr.candidate_count);
        stelog_index_close(idx);
        return 2;
    }
    if (drc != 0) {
        fprintf(stderr, "Session '%s' not found.\n", sess);
        stelog_index_close(idx);
        return 1;
    }

    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 16384);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_str(&w, "session_id", dr.session_id.ptr, dr.session_id.len);
        stelog_jw_kv_cstr(&w, "agent", stelog_agent_name(dr.agent));
        stelog_jw_kv_str(&w, "project", dr.project.ptr, dr.project.len);
        stelog_jw_kv_str(&w, "title", dr.title.ptr, dr.title.len);
        stelog_jw_kv_u64(&w, "total_dialogue_turns", dr.total_dialogue_turns);
        stelog_jw_kv_u64(&w, "total_tools_collapsed", dr.total_tools_collapsed);
        stelog_jw_kv_u64(&w, "duplicates_skipped", dr.duplicates_skipped);
        stelog_jw_kv_bool(&w, "truncated", dr.truncated);
        stelog_jw_kv_int(&w, "first_ts_ms", dr.first_ts_ms);
        stelog_jw_kv_int(&w, "last_ts_ms", dr.last_ts_ms);
        stelog_jw_kv_u64(&w, "turn_offset", dr.turn_offset);
        stelog_jw_kv_u64(&w, "gap_item_index", dr.gap_item_index);
        stelog_jw_kv_u64(&w, "omitted_begin", dr.omitted_begin);
        stelog_jw_kv_u64(&w, "omitted_count", dr.omitted_count);
        stelog_jw_kv_u64(&w, "next_offset", dr.next_offset);
        stelog_jw_kv_double(&w, "elapsed_ms", dr.elapsed_ms);
        stelog_jw_key(&w, "items");
        stelog_jw_arr_begin(&w);
        for (uint32_t i = 0; i < dr.count; ++i) write_hit_json(&w, &dr.items[i]);
        stelog_jw_arr_end(&w);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        stelog_index_close(idx);
        return 0;
    }

    char tbuf[32], tbuf2[32];
    fmt_ts(dr.first_ts_ms, tbuf, sizeof(tbuf));
    fmt_ts(dr.last_ts_ms, tbuf2, sizeof(tbuf2));
    uint32_t dsl = stelog_index_unique_prefix_len(idx, dr.session_id, 8);
    printf("%sDigest @%.*s [%s] %s .. %s | proj=%.*s | %u/%u dialogue turns (%u tools collapsed, %u retries deduped, %.3f ms)%s\n\n",
           C_DIM, (int)dsl, dr.session_id.ptr,
           stelog_agent_name(dr.agent), tbuf, tbuf2,
           (int)dr.project.len, dr.project.ptr,
           dr.count, dr.total_dialogue_turns,
           dr.total_tools_collapsed, dr.duplicates_skipped, dr.elapsed_ms, C_RESET);

    for (uint32_t i = 0; i < dr.count; ++i) {
        const StelogHit *h = &dr.items[i];
        StelogRoleKind rk = stelog_tag_role(h->tag);
        bool after_gap = dr.omitted_count > 0 && i >= dr.gap_item_index;
        if (dr.omitted_count > 0 && i == dr.gap_item_index) {
            printf("  %s[... %u turns omitted (#%u-#%u): `stelog digest @%.*s --offset %u`]%s\n", C_YELLOW,
                   dr.omitted_count, dr.omitted_begin, dr.omitted_begin + dr.omitted_count - 1u,
                   (int)dsl, dr.session_id.ptr, dr.next_offset, C_RESET);
        }
        uint32_t turn_no = after_gap ? dr.omitted_begin + dr.omitted_count + (i - dr.gap_item_index) : dr.turn_offset + i;
        if (h->collapsed_tools_before > 0) {
            printf("  %s[...%u tool calls]%s\n", C_DIM, h->collapsed_tools_before, C_RESET);
        }
        fmt_ts(h->ts_ms, tbuf, sizeof(tbuf));
        printf("%s#%u [%s]%s %sdoc=%" PRIu64 "%s %s%s%s (%.*s): ",
               C_DIM, turn_no, tbuf, C_RESET,
               C_BOLD, h->doc_id, C_RESET,
               role_color(rk), stelog_role_name(rk), C_RESET,
               (int)h->actor.len, h->actor.ptr);
        print_sanitized(h->snippet.ptr, h->snippet.len);
        putchar('\n');
    }
    if (dr.omitted_count > 0 && dr.gap_item_index >= dr.count) {
        printf("\n%s[... %u more turns (#%u-#%u): `stelog digest @%.*s --offset %u`]%s\n", C_YELLOW,
               dr.omitted_count, dr.omitted_begin, dr.omitted_begin + dr.omitted_count - 1u,
               (int)dsl, dr.session_id.ptr, dr.next_offset, C_RESET);
    }
    if (dr.truncated) {
        printf("\n%s[long turns trimmed at budget=%uB: use `stelog show <doc_id>` or `--budget N` for more]%s\n", C_DIM, budget, C_RESET);
    }
    stelog_index_close(idx);
    return 0;
}

static int cmd_show(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: stelog show <doc_id> [--raw] [--tail] [--offset N] [--limit N] [--json]\n"); return 1; }
    uint64_t doc_id = strtoull(argv[2], NULL, 10);
    bool raw = false, tail = false, json_out = false;
    uint32_t off = 0, limit = 32768;
    const char *index_dir = NULL;
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "--raw") == 0) raw = true;
        else if (strcmp(argv[i], "--tail") == 0) tail = true;
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) off = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
    }
    StelogIndex *idx = stelog_index_open(index_dir);
    if (!idx) return 1;
    StelogDocResult dr;
    if (stelog_index_get_document(idx, doc_id, raw, off, limit, tail, &dr) != 0) {
        fprintf(stderr, "Failed to read document %" PRIu64 "\n", doc_id);
        stelog_index_close(idx);
        return 1;
    }
    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, dr.content.len + 1024);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_u64(&w, "doc_id", dr.doc_id);
        stelog_jw_kv_int(&w, "ts_ms", dr.ts_ms);
        stelog_jw_kv_cstr(&w, "agent", stelog_agent_name(stelog_tag_agent(dr.tag)));
        stelog_jw_kv_cstr(&w, "role", stelog_role_name(stelog_tag_role(dr.tag)));
        stelog_jw_kv_str(&w, "session_id", dr.session_id.ptr, dr.session_id.len);
        stelog_jw_kv_str(&w, "project", dr.project.ptr, dr.project.len);
        stelog_jw_kv_str(&w, "actor", dr.actor.ptr, dr.actor.len);
        stelog_jw_kv_str(&w, "content", dr.content.ptr, dr.content.len);
        stelog_jw_kv_u64(&w, "returned_offset", dr.returned_offset);
        stelog_jw_kv_u64(&w, "total_bytes", dr.total_bytes);
        stelog_jw_kv_bool(&w, "truncated", dr.truncated);
        stelog_jw_kv_double(&w, "elapsed_ms", dr.elapsed_ms);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        stelog_index_close(idx);
        return 0;
    }
    char tbuf[32];
    fmt_ts(dr.ts_ms, tbuf, sizeof(tbuf));
    printf("doc=%" PRIu64 " | %s | %s/%s | actor=%.*s | proj=%.*s | %u/%u bytes (%.3f ms)\n---\n%.*s\n",
           dr.doc_id, tbuf,
           stelog_agent_name(stelog_tag_agent(dr.tag)),
           stelog_role_name(stelog_tag_role(dr.tag)),
           (int)dr.actor.len, dr.actor.ptr,
           (int)dr.project.len, dr.project.ptr,
           dr.content.len, dr.total_bytes, dr.elapsed_ms,
           (int)dr.content.len, dr.content.ptr);
    if (dr.truncated) {
        printf("---\n[truncated: offset=%u..%u of %u bytes. Next: `stelog show %" PRIu64 " --offset %u`]\n",
               dr.returned_offset, dr.returned_offset + dr.content.len, dr.total_bytes,
               dr.doc_id, dr.returned_offset + dr.content.len);
    }
    stelog_index_close(idx);
    return 0;
}

static int cmd_sessions(int argc, char **argv) {
    uint32_t agent_mask = 0, limit = 20, offset = 0;
    const char *proj = NULL, *sess = NULL, *index_dir = NULL;
    int64_t since = 0, until = 0;
    bool no_sync = false, json_out = false;
    char cwd_buf[1024];
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc) agent_mask |= parse_agent_csv(argv[++i]);
        else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) proj = argv[++i];
        else if (strcmp(argv[i], "--since") == 0 && i + 1 < argc) since = parse_time_arg_or_exit("--since", argv[++i], false);
        else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) until = parse_time_arg_or_exit("--until", argv[++i], true);
        else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) limit = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--no-sync") == 0) no_sync = true;
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
        else if (strcmp(argv[i], "--no-color") == 0) { /* handled */ }
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (argv[i][0] != '-') apply_target_arg(argv[i], cwd_buf, sizeof(cwd_buf), &proj, &sess);
    }
    StelogIndex *idx = open_with_optional_sync(index_dir, no_sync);
    if (!idx) return 1;
    StelogSessionListResult sl;
    stelog_index_list_sessions(idx, agent_mask, proj, since, until, limit, offset, &sl);

    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 8192);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_u64(&w, "total_matches", sl.total_matches);
        stelog_jw_kv_double(&w, "elapsed_ms", sl.elapsed_ms);
        stelog_jw_key(&w, "sessions");
        stelog_jw_arr_begin(&w);
        for (uint32_t i = 0; i < sl.count; ++i) {
            const StelogSessionSummary *s = &sl.items[i];
            stelog_jw_obj_begin(&w);
            stelog_jw_kv_cstr(&w, "agent", stelog_agent_name(s->agent));
            stelog_jw_kv_str(&w, "session_id", s->session_id.ptr, s->session_id.len);
            stelog_jw_kv_str(&w, "title", s->title.ptr, s->title.len);
            stelog_jw_kv_str(&w, "project", s->project.ptr, s->project.len);
            stelog_jw_kv_int(&w, "first_ts_ms", s->first_ts_ms);
            stelog_jw_kv_int(&w, "last_ts_ms", s->last_ts_ms);
            stelog_jw_kv_u64(&w, "doc_count", s->doc_count);
            stelog_jw_obj_end(&w);
        }
        stelog_jw_arr_end(&w);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
        stelog_index_close(idx);
        return 0;
    }

    printf("%sSessions: %u total (showing %u, %.3f ms; first .. last activity)%s\n\n", C_DIM, sl.total_matches, sl.count, sl.elapsed_ms, C_RESET);
    char tbuf[32], tbuf2[32];
    for (uint32_t i = 0; i < sl.count; ++i) {
        const StelogSessionSummary *s = &sl.items[i];
        fmt_ts(s->first_ts_ms, tbuf, sizeof(tbuf));
        fmt_ts(s->last_ts_ms, tbuf2, sizeof(tbuf2));
        uint32_t sid_s = stelog_index_unique_prefix_len(idx, s->session_id, 8);
        printf("%s[%s .. %s]%s %-5s %4u docs | %s@%.*s%s | %s%.*s%s | %.*s\n",
               C_DIM, tbuf, tbuf2, C_RESET,
               stelog_agent_name(s->agent), s->doc_count,
               C_BLUE, (int)sid_s, s->session_id.ptr, C_RESET,
               C_DIM, (int)s->project.len, s->project.ptr, C_RESET,
               (int)s->title.len, s->title.ptr);
    }
    stelog_index_close(idx);
    return 0;
}

static int cmd_stats(int argc, char **argv) {
    const char *index_dir = NULL;
    bool json_out = false;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
        else if (strcmp(argv[i], "--json") == 0) json_out = true;
    }
    StelogIndex *idx = stelog_index_open(index_dir);
    if (!idx) { fprintf(stderr, "No index found. Run `stelog index`.\n"); return 1; }
    StelogIndexStats st;
    stelog_index_get_stats(idx, &st);
    if (json_out) {
        StelogJsonWriter w;
        stelog_jw_init(&w, 1024);
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_u64(&w, "base_gen", st.base_gen);
        stelog_jw_kv_u64(&w, "base_docs", st.base_docs);
        stelog_jw_kv_u64(&w, "base_sessions", st.base_sessions);
        stelog_jw_kv_u64(&w, "base_terms", st.base_terms);
        stelog_jw_kv_u64(&w, "delta_gen", st.delta_gen);
        stelog_jw_kv_u64(&w, "delta_docs", st.delta_docs);
        stelog_jw_kv_u64(&w, "total_disk_bytes", st.total_disk_bytes);
        stelog_jw_obj_end(&w);
        printf("%s\n", w.buf);
        stelog_jw_destroy(&w);
    } else {
        printf("Base  : gen=%u docs=%u sessions=%u terms=%u text=%.2f MB\n",
               st.base_gen, st.base_docs, st.base_sessions, st.base_terms, (double)st.base_text_bytes / 1048576.0);
        printf("Delta : gen=%u docs=%u sessions=%u terms=%u text=%.2f MB\n",
               st.delta_gen, st.delta_docs, st.delta_sessions, st.delta_terms, (double)st.delta_text_bytes / 1048576.0);
        printf("Disk  : %.2f MB total\n", (double)st.total_disk_bytes / 1048576.0);
    }
    stelog_index_close(idx);
    return 0;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da < db) ? -1 : (da > db ? 1 : 0);
}

typedef struct {
    const char *query;
    StelogCaseMode case_mode;
    bool fixed_strings;
    bool regex_mode;
} BenchCase;

static void run_search_bench_suite(StelogIndex *idx,
                                   const char *suite_name,
                                   const BenchCase *cases,
                                   uint32_t ncases,
                                   uint32_t iters,
                                   double budget_p99_us) {
    double *lat = (double *)malloc(iters * sizeof(double));
    for (uint32_t i = 0; i < ncases; ++i) {
        StelogQuery q;
        memset(&q, 0, sizeof(q));
        q.query = cases[i].query;
        q.case_mode = cases[i].case_mode;
        q.fixed_strings = cases[i].fixed_strings;
        q.regex_mode = cases[i].regex_mode;
        q.limit = 20;
        StelogSearchResult sr;
        stelog_index_search(idx, &q, &sr);
    }
    uint64_t total_hits = 0;
    for (uint32_t i = 0; i < iters; ++i) {
        const BenchCase *bc = &cases[i % ncases];
        StelogQuery q;
        memset(&q, 0, sizeof(q));
        q.query = bc->query;
        q.case_mode = bc->case_mode;
        q.fixed_strings = bc->fixed_strings;
        q.regex_mode = bc->regex_mode;
        q.limit = 20;
        StelogSearchResult sr;
        stelog_index_search(idx, &q, &sr);
        lat[i] = sr.elapsed_ms;
        total_hits += sr.total_matches;
    }
    qsort(lat, iters, sizeof(double), cmp_double);
    double p50 = lat[iters / 2] * 1000.0;
    double p95 = lat[(iters * 95) / 100] * 1000.0;
    double p99 = lat[(iters * 99) / 100] * 1000.0;
    const char *status = (p99 <= budget_p99_us) ? "OK" : "WARN";
    printf("  %-20s | iters=%-3u | avg_hits=%6.1f | P50=%7.1f us | P95=%7.1f us | P99=%7.1f us (budget <= %5.0f us) [%s]\n",
           suite_name, iters, (double)total_hits / (double)iters, p50, p95, p99, budget_p99_us, status);
    free(lat);
}

static int cmd_bench(int argc, char **argv) {
    uint32_t iters = 200;
    const char *index_dir = NULL;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--iters") == 0 && i + 1 < argc) iters = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
    }
    if (iters < 20) iters = 20;
    StelogIndex *idx = stelog_index_open(index_dir);
    if (!idx) { fprintf(stderr, "Run `stelog index` before `stelog bench`.\n"); return 1; }

    StelogIndexStats st;
    stelog_index_get_stats(idx, &st);
    printf("stelog 6-Suite Regression Benchmark (docs=%u+%u, terms=%u, text=%.2f MB)\n",
           st.base_docs, st.delta_docs, st.base_terms,
           (double)(st.base_text_bytes + st.delta_text_bytes) / 1048576.0);

    /* Warm up scratch arena once, then verify ZERO extra heap allocation across all 6 suites! */
    static const BenchCase b1[] = {
        { "gateway",    STELOG_CASE_SMART, false, false },
        { "pnpm",       STELOG_CASE_SMART, false, false },
        { "测试",       STELOG_CASE_SMART, false, false },
        { "决策",       STELOG_CASE_SMART, false, false },
        { "sqlite",     STELOG_CASE_SMART, false, false },
        { "memory",     STELOG_CASE_SMART, false, false },
        { "git status", STELOG_CASE_SMART, false, false },
        { "修复",       STELOG_CASE_SMART, false, false },
        { "schema",     STELOG_CASE_SMART, false, false },
        { "agent",      STELOG_CASE_SMART, false, false }
    };
    static const BenchCase b2[] = {
        { "镜像 | mirror -docker",         STELOG_CASE_SMART, false, false },
        { "gateway | memory -test",        STELOG_CASE_SMART, false, false },
        { "sqlite | roaring -error",       STELOG_CASE_SMART, false, false },
        { "agent:agy role:user 修复 -css", STELOG_CASE_SMART, false, false },
        { "simdjson | simdutf -legacy",    STELOG_CASE_SMART, false, false }
    };
    static const BenchCase b3[] = {
        { "SwissTable",               STELOG_CASE_SMART, false, false },
        { "RoaringBitmap",            STELOG_CASE_SMART, false, false },
        { "AGENTS",                   STELOG_CASE_SMART, false, false },
        { "src/store/roaring.c",      STELOG_CASE_SMART, true,  false },
        { "build.zig",                STELOG_CASE_SMART, false, false }
    };
    static const BenchCase b4[] = {
        { "truncat*",        STELOG_CASE_SMART, false, false },
        { "roaring*",        STELOG_CASE_SMART, false, false },
        { "simd*",           STELOG_CASE_SMART, false, false },
        { "gatew*",          STELOG_CASE_SMART, false, false },
        { "build.zig",       STELOG_CASE_SMART, true,  false },
        { "stelog truncat*", STELOG_CASE_SMART, false, false }
    };
    static const BenchCase b6[] = {
        { "src/store/[a-z_]+\\.c",     STELOG_CASE_SMART, false, true }, /* Tier 1: SwissTable + JIT */
        { "stelog_truncate_[a-z_]+",   STELOG_CASE_SMART, false, true }, /* Tier 2: NEON lit + JIT */
        { "qz[0-9]{4,}[a-z]?",         STELOG_CASE_SMART, false, true }, /* Tier 2: NEON lit + JIT */
        { "\\b2026-11-1[0-9]\\b",      STELOG_CASE_SMART, false, true }, /* Tier 1/2: literal + JIT */
        { "simdjson|simdutf",          STELOG_CASE_SMART, false, true }  /* Tier 3: parallel PCRE2 JIT */
    };

    /* Prime arena before measuring zero-malloc invariant */
    {
        StelogQuery q; memset(&q, 0, sizeof(q)); q.query = "gateway"; q.limit = 20;
        StelogSearchResult sr; stelog_index_search(idx, &q, &sr);
    }
    size_t alloc_before = stelog_index_scratch_allocated_bytes(idx);

    run_search_bench_suite(idx, "B1_exact",           b1, (uint32_t)(sizeof(b1) / sizeof(b1[0])), iters, 150.0);
    run_search_bench_suite(idx, "B2_or_negate",       b2, (uint32_t)(sizeof(b2) / sizeof(b2[0])), iters, 250.0);
    run_search_bench_suite(idx, "B3_smartcase_fixed", b3, (uint32_t)(sizeof(b3) / sizeof(b3[0])), iters, 300.0);
    uint32_t b4_iters = iters > 60 ? 60 : iters;
    run_search_bench_suite(idx, "B4_prefix_simd",     b4, (uint32_t)(sizeof(b4) / sizeof(b4[0])), b4_iters, 3000.0);
    uint32_t b6_iters = iters > 50 ? 50 : iters;
    run_search_bench_suite(idx, "B6_pcre2_regex",     b6, (uint32_t)(sizeof(b6) / sizeof(b6[0])), b6_iters, 5000.0);

    /* B5: context + digest latency on real session & doc */
    StelogSessionListResult sl;
    char sample_sid[128] = { 0 };
    uint64_t sample_doc = 0;
    bool has_sample_doc = false;
    if (stelog_index_list_sessions(idx, 0, NULL, 0, 0, 5, 0, &sl) == 0 && sl.count > 0) {
        size_t copy = sl.items[0].session_id.len < sizeof(sample_sid) - 1 ? sl.items[0].session_id.len : sizeof(sample_sid) - 1;
        memcpy(sample_sid, sl.items[0].session_id.ptr, copy);
        sample_sid[copy] = '\0';
    }
    if (st.base_docs > 0) {
        sample_doc = idx->base.col_doc_key[st.base_docs / 2u];
        has_sample_doc = true;
    }
    if (has_sample_doc && sample_sid[0]) {
        double *lat = (double *)malloc(iters * sizeof(double));
        for (uint32_t i = 0; i < iters; ++i) {
            if ((i & 1u) == 0) {
                StelogContextResult cr;
                stelog_index_context(idx, sample_doc, 5, 1800, false, &cr);
                lat[i] = cr.elapsed_ms;
            } else {
                StelogDigestResult dr;
                stelog_index_digest(idx, sample_sid, 14336, false, &dr);
                lat[i] = dr.elapsed_ms;
            }
        }
        qsort(lat, iters, sizeof(double), cmp_double);
        double p50 = lat[iters / 2] * 1000.0;
        double p95 = lat[(iters * 95) / 100] * 1000.0;
        double p99 = lat[(iters * 99) / 100] * 1000.0;
        printf("  %-20s | iters=%-3u | avg_hits=%6s | P50=%7.1f us | P95=%7.1f us | P99=%7.1f us (budget <=  300 us) [%s]\n",
               "B5_digest_context", iters, "-", p50, p95, p99, (p99 <= 300.0) ? "OK" : "WARN");
        free(lat);
    }

    size_t alloc_after = stelog_index_scratch_allocated_bytes(idx);
    printf("  QueryScratchArena    | before=%zu KB | after=%zu KB | zero-malloc=%s\n",
           alloc_before / 1024u, alloc_after / 1024u,
           (alloc_after == alloc_before) ? "PASS" : "FAIL (grew!)");

    stelog_index_close(idx);
    return 0;
}

static int cmd_mcp(int argc, char **argv) {
    const char *index_dir = NULL;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--index-dir") == 0 && i + 1 < argc) index_dir = argv[++i];
    }
    return stelog_mcp_serve(index_dir);
}

int main(int argc, char **argv) {
    init_color(argc, argv);
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }
    if (strcmp(argv[1], "scan") == 0)     return cmd_scan(argc, argv);
    if (strcmp(argv[1], "index") == 0)    return cmd_index(argc, argv);
    if (strcmp(argv[1], "search") == 0)   return cmd_search_or_timeline(argc, argv, false);
    if (strcmp(argv[1], "digest") == 0)   return cmd_digest(argc, argv);
    if (strcmp(argv[1], "timeline") == 0) return cmd_search_or_timeline(argc, argv, true);
    if (strcmp(argv[1], "files") == 0)    return cmd_files(argc, argv);
    if (strcmp(argv[1], "context") == 0)  return cmd_context(argc, argv);
    if (strcmp(argv[1], "show") == 0)     return cmd_show(argc, argv);
    if (strcmp(argv[1], "sessions") == 0 || strcmp(argv[1], "list") == 0) return cmd_sessions(argc, argv);
    if (strcmp(argv[1], "stats") == 0)    return cmd_stats(argc, argv);
    if (strcmp(argv[1], "bench") == 0)    return cmd_bench(argc, argv);
    if (strcmp(argv[1], "mcp") == 0)      return cmd_mcp(argc, argv);

    fprintf(stderr, "Unknown command '%s'. Run `stelog --help`.\n", argv[1]);
    return 1;
}
