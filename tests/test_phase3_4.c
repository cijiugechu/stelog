#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "stelog.h"
#include "stelog_json.h"
#include "base/arena.h"
#include "base/simd.h"
#include "json/json_writer.h"
#include "query/regex_engine.h"
#include "store/segment_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static void write_str(const char *path, const char *s) {
    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL);
    fputs(s, fp);
    fclose(fp);
}

static void test_json_writer_and_mcp_request_parser(void) {
    StelogJsonWriter w;
    stelog_jw_init(&w, 256);
    stelog_jw_obj_begin(&w);
    stelog_jw_kv_cstr(&w, "jsonrpc", "2.0");
    stelog_jw_kv_int(&w, "id", 42);
    stelog_jw_kv_cstr(&w, "msg", "line1\n\"quoted\"\\slash");
    stelog_jw_obj_end(&w);
    CHECK(strstr(w.buf, "\\n\\\"quoted\\\"\\\\slash") != NULL);
    stelog_jw_destroy(&w);

    StelogJsonParser *jp = stelog_json_parser_create();
    StelogArena arena;
    stelog_arena_init(&arena, 16384);

    const char *rpc =
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"search_history\",\"arguments\":{"
        "\"query\":\"gateway 内存\",\"agent\":\"codex,agy\",\"role\":\"user,assistant\","
        "\"case\":\"sensitive\",\"fixed\":true,"
        "\"no_subagents\":true,\"limit\":15,\"offset\":5,\"doc_id\":25711,\"doc_ids\":[101,202,4294967301]}}}";
    StelogMcpRequest req;
    CHECK(stelog_json_parse_mcp_request(jp, rpc, strlen(rpc), strlen(rpc), &arena, &req) == 0);
    CHECK(req.id_kind == STELOG_RPC_ID_INT && req.id_int == 7);
    CHECK(strcmp(req.method.ptr, "tools/call") == 0);
    CHECK(strcmp(req.tool_name.ptr, "search_history") == 0);
    CHECK(strcmp(req.arg_query.ptr, "gateway 内存") == 0);
    CHECK(strcmp(req.arg_agent.ptr, "codex,agy") == 0);
    CHECK(strcmp(req.arg_case.ptr, "sensitive") == 0);
    CHECK(req.arg_fixed == true);
    CHECK(req.arg_no_subagents == true);
    CHECK(req.arg_limit == 15 && req.arg_offset == 5);
    CHECK(req.has_doc_id == true && req.arg_doc_id == 25711);
    CHECK(req.arg_doc_id_count == 3 && req.arg_doc_ids[0] == 101 && req.arg_doc_ids[2] == 4294967301ull);

    stelog_arena_destroy(&arena);
    stelog_json_parser_destroy(jp);
    printf("[PASS] test_json_writer_and_mcp_request_parser\n");
}

static void test_csift_upgrades_purification_absence_files_countby(void) {
    char root[] = "/tmp/stelog_p34_test_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char codex_home[512], codex_sess[512], agy_home[512], agy_log[512], pi_home[512], pi_sess[512], idx_dir[512];
    snprintf(codex_home, sizeof(codex_home), "%s/codex", root);
    snprintf(codex_sess, sizeof(codex_sess), "%s/codex/sessions/2026/11/19", root);
    snprintf(agy_home, sizeof(agy_home), "%s/agy", root);
    snprintf(agy_log, sizeof(agy_log), "%s/agy/brain/aaaaaaaa-1111-2222-3333-444444444444/.system_generated/logs", root);
    snprintf(pi_home, sizeof(pi_home), "%s/pi", root);
    snprintf(pi_sess, sizeof(pi_sess), "%s/pi/sessions/p", root);
    snprintf(idx_dir, sizeof(idx_dir), "%s/index", root);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s' '%s'", codex_sess, agy_log, pi_sess);
    CHECK(system(cmd) == 0);

    char f_codex[600], f_agy[600], f_pi[600];
    snprintf(f_codex, sizeof(f_codex), "%s/rollout-2026-11-19T01-00-00-deadbeef-1111-2222-3333-444444444444.jsonl", codex_sess);
    snprintf(f_agy, sizeof(f_agy), "%s/transcript.jsonl", agy_log);
    snprintf(f_pi, sizeof(f_pi), "%s/2026-11-19T02-00-00Z_bbbbbbbb-1111-2222-3333-444444444444.jsonl", pi_sess);

    /* Codex: includes harness-injected AGENTS.md & tool exec result in role:"user" (MUST be purified!)
       + real user prompt + apply_patch file edit */
    write_str(f_codex,
        "{\"timestamp\":\"2026-11-19T01:00:00.000Z\",\"type\":\"session_meta\",\"payload\":{\"id\":\"deadbeef-1111-2222-3333-444444444444\",\"cwd\":\"/repos/stelog\"}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:01.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"# AGENTS.md instructions for /repos/stelog\\nSecretInjectedNoiseToken\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:02.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"[42] tool exec result: exit 0 AnotherInjectedNoise\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:03.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"真正的人类决策：采用 simdutf 处理 UTF-8\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:04.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"AssistantOnlyKeywordAlpha 已确认实现方案\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:05.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"custom_tool_call\",\"name\":\"apply_patch\",\"input\":\"*** Begin Patch\\n*** Update File: src/store/tokenizer.c\\n@@ -1 +1 @@\\n*** End Patch\"}}\n");

    /* AGY: edit_file touch + user prompt (no SQLite DB -> must get auto-fallback title!) */
    write_str(f_agy,
        "{\"step_index\":0,\"source\":\"USER_EXPLICIT\",\"type\":\"USER_INPUT\",\"created_at\":\"2026-11-19T02:00:00Z\",\"content\":\"<USER_REQUEST>\\n修一下 gateway 限流配置\\n</USER_REQUEST>\"}\n"
        "{\"step_index\":1,\"source\":\"MODEL\",\"type\":\"PLANNER_RESPONSE\",\"created_at\":\"2026-11-19T02:00:05Z\",\"tool_calls\":[{\"name\":\"edit_file\",\"arguments\":{\"Path\":\"/repos/gateway/rate_limit.rs\"}}]}\n");

    /* Pi: write tool touch */
    write_str(f_pi,
        "{\"type\":\"session\",\"version\":3,\"id\":\"bbbbbbbb-1111-2222-3333-444444444444\",\"timestamp\":\"2026-11-19T03:00:00.000Z\",\"cwd\":\"/repos/stelog\"}\n"
        "{\"type\":\"message\",\"id\":\"m1\",\"timestamp\":\"2026-11-19T03:00:01.000Z\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"toolCall\",\"name\":\"write\",\"arguments\":{\"path\":\"docs/DESIGN.md\"}}]}}\n");

    StelogIndexConfig icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.index_dir = idx_dir;
    icfg.codex_home = codex_home;
    icfg.agy_home = agy_home;
    icfg.pi_home = pi_home;
    icfg.threads = 2;
    StelogIndexReport rep;
    CHECK(stelog_index_sync(&icfg, &rep) == 0);

    StelogIndex *idx = stelog_index_open(idx_dir);
    CHECK(idx != NULL);

    /* 1. Purification: injected AGENTS.md & tool exec result must NOT pollute default search */
    StelogQuery q;
    memset(&q, 0, sizeof(q));
    q.query = "SecretInjectedNoiseToken";
    StelogSearchResult sr;
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 0);

    q.query = "AnotherInjectedNoise";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 0);

    /* 2. Definitive Absence + 20us cross-role fallback diagnostic */
    memset(&q, 0, sizeof(q));
    q.query = "AssistantOnlyKeywordAlpha";
    q.role_mask = (1u << STELOG_ROLE_USER); /* restrict to user -> 0 hits! */
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 0);
    CHECK(sr.unfiltered_total_matches == 1);
    CHECK(sr.other_role_counts[STELOG_ROLE_ASSISTANT] == 1);

    /* 3. Short @prefix session scoping + --count-by role */
    memset(&q, 0, sizeof(q));
    q.session_id = "@deadbeef";
    q.count_by = STELOG_COUNT_BY_ROLE;
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 3); /* 1 real user + 1 assistant + 1 tool_call */
    CHECK(sr.bucket_count == 3);

    /* 4. Auto-fallback session title from first human prompt (no SQLite needed!) */
    StelogSessionListResult sl;
    CHECK(stelog_index_list_sessions(idx, (1u << STELOG_AGENT_AGY), NULL, 0, 0, 10, 0, &sl) == 0);
    CHECK(sl.count == 1);
    CHECK(strstr(sl.items[0].title.ptr, "gateway") != NULL);

    /* 5. File-touch provenance across Codex (apply_patch) + AGY (edit_file) + Pi (write) */
    StelogFileHistoryResult fh;
    CHECK(stelog_index_files(idx, NULL, 0, NULL, NULL, 0, 0, false, 10, 0, &fh) == 0);
    CHECK(fh.total_matches == 3);
    CHECK(stelog_index_files(idx, "rate_limit.rs", 0, NULL, NULL, 0, 0, false, 10, 0, &fh) == 0);
    CHECK(fh.total_matches == 1);
    CHECK(stelog_tag_agent(fh.items[0].tag) == STELOG_AGENT_AGY);

    /* 6. Bilingual OR query (`simdutf | gateway`) + --group-by session */
    memset(&q, 0, sizeof(q));
    q.query = "simdutf | gateway";
    q.group_by_session = true;
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.session_group_count == 2); /* deadbeef (codex) + aaaaaaaa (agy) */

    /* 7. Session digest collapses intermediate tools cleanly */
    StelogDigestResult dig;
    CHECK(stelog_index_digest(idx, "@deadbeef", 8192, false, &dig) == 0);
    CHECK(dig.total_dialogue_turns == 2); /* 1 real user + 1 assistant */
    CHECK(dig.total_tools_collapsed == 1); /* 1 apply_patch collapsed */

    stelog_index_close(idx);
    stelog_rmdir_recursive(root);
    printf("[PASS] test_csift_upgrades_purification_absence_files_countby\n");
}

static void test_simd_substring_and_rg_query_syntax(void) {
    /* 0. Verify Delta global doc_id stays strictly within IEEE-754 MAX_SAFE_INTEGER (< 2^53) for JSON/MCP hosts */
    uint64_t delta_id = stelog_make_doc_id(true, 12345u);
    CHECK(stelog_doc_is_delta(delta_id) == true);
    CHECK(stelog_doc_local_id(delta_id) == 12345u);
    CHECK(delta_id < (1ull << 53));

    /* 1. Direct SIMD memmem / memcasemem / word-prefix unit checks */
    const char *hay = "0123456789abcdef_stelog_truncate_middle_POST /api/parts/mirror-rules_end";
    size_t hlen = strlen(hay);
    CHECK(stelog_memmem(hay, hlen, "POST /api/parts/mirror-rules", 28) != NULL);
    CHECK(stelog_memmem(hay, hlen, "post /api/parts/mirror-rules", 28) == NULL);
    const char *ci_hit = stelog_memcasemem(hay, hlen, "post /API/parts/MIRROR-rules", 28);
    CHECK(ci_hit != NULL && memcmp(ci_hit, "POST", 4) == 0);
    const char *tr_hit = stelog_memcasemem(hay, hlen, "truncat", 7);
    CHECK(tr_hit != NULL && stelog_is_word_prefix(hay, hlen, tr_hit) == true);
    const char *mid_hit = stelog_memcasemem(hay, hlen, "runcat", 6);
    CHECK(mid_hit != NULL && stelog_is_word_prefix(hay, hlen, mid_hit) == false);
    CHECK(stelog_memcasemem(hay, hlen, "E", 1) != NULL);
    CHECK(stelog_memcasemem(hay, hlen, "end", 3) == hay + hlen - 3);

    /* 2. End-to-end index test for negation, inline modifiers, smart-case, -F, prefix*, and 0-malloc invariant */
    char root[] = "/tmp/stelog_rg_syntax_XXXXXX";
    CHECK(mkdtemp(root) != NULL);

    char codex_home[512], codex_sess[512], agy_home[512], agy_log[512], pi_home[512], idx_dir[512];
    snprintf(codex_home, sizeof(codex_home), "%s/codex", root);
    snprintf(codex_sess, sizeof(codex_sess), "%s/codex/sessions/2026/11/19", root);
    snprintf(agy_home, sizeof(agy_home), "%s/agy", root);
    snprintf(agy_log, sizeof(agy_log), "%s/agy/brain/cafe1234-1111-2222-3333-444444444444/.system_generated/logs", root);
    snprintf(pi_home, sizeof(pi_home), "%s/pi", root);
    snprintf(idx_dir, sizeof(idx_dir), "%s/index", root);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s' '%s'", codex_sess, agy_log, pi_home);
    CHECK(system(cmd) == 0);

    char f_codex[600], f_agy[600];
    snprintf(f_codex, sizeof(f_codex), "%s/rollout-2026-11-19T01-00-00-feedface-1111-2222-3333-444444444444.jsonl", codex_sess);
    snprintf(f_agy, sizeof(f_agy), "%s/transcript.jsonl", agy_log);

    write_str(f_codex,
        "{\"timestamp\":\"2026-11-19T01:00:00.000Z\",\"type\":\"session_meta\",\"payload\":{\"id\":\"feedface-1111-2222-3333-444444444444\",\"cwd\":\"/repos/unicus-studio\"}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:01.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"Python GIL contention vs gilbert graph algorithm\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:02.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Check lowercase gil behavior and POST /api/parts/mirror-rules endpoint\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:03.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"设计镜像规则：零件号 QZ7061501A 走对称映射\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:04.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"已实现 docker compose 镜像构建脚本，注意不要和业务镜像混淆\"}]}}\n");

    write_str(f_agy,
        "{\"step_index\":0,\"source\":\"USER_EXPLICIT\",\"type\":\"USER_INPUT\",\"created_at\":\"2026-11-19T02:00:00Z\",\"content\":\"<USER_REQUEST>\\n调整 stelog_truncate_middle 截断策略\\n</USER_REQUEST>\"}\n"
        "{\"step_index\":1,\"source\":\"MODEL\",\"type\":\"PLANNER_RESPONSE\",\"created_at\":\"2026-11-19T02:00:05Z\",\"content\":\"Mirror rules documented in src/store/roaring.c without docker.\"}\n");

    StelogIndexConfig icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.index_dir = idx_dir;
    icfg.codex_home = codex_home;
    icfg.agy_home = agy_home;
    icfg.pi_home = pi_home;
    icfg.threads = 2;
    CHECK(stelog_index_sync(&icfg, NULL) == 0);

    StelogIndex *idx = stelog_index_open(idx_dir);
    CHECK(idx != NULL);

    StelogQuery q;
    StelogSearchResult sr;

    /* A. Negation: `镜像 -docker` excludes docker doc, keeps QZ7061501A doc */
    memset(&q, 0, sizeof(q));
    q.query = "镜像 -docker";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);
    CHECK(strstr(sr.hits[0].snippet.ptr, "QZ7061501A") != NULL);

    /* A2. OR + negated phrase: `镜像 | mirror -"docker compose"` */
    memset(&q, 0, sizeof(q));
    q.query = "镜像 | mirror -\"docker compose\"";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 3); /* QZ7061501A + POST /api/parts/mirror-rules + AGY Mirror rules */

    /* B. Inline modifiers: `agent:agy role:user truncat*` and `@feedface` */
    memset(&q, 0, sizeof(q));
    q.query = "agent:agy role:user truncat*";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);
    CHECK(stelog_tag_agent(sr.hits[0].tag) == STELOG_AGENT_AGY);
    CHECK(stelog_tag_role(sr.hits[0].tag) == STELOG_ROLE_USER);

    memset(&q, 0, sizeof(q));
    q.query = "@cafe1234";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 2);

    /* C. Smart-case vs -i vs -s */
    memset(&q, 0, sizeof(q));
    q.query = "GIL"; /* SMART -> uppercase triggers case-sensitive! */
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);

    q.query = "gil"; /* SMART -> all lowercase -> case-insensitive -> matches both GIL and gil! */
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 2);

    q.query = "GIL";
    q.case_mode = STELOG_CASE_INSENSITIVE; /* -i -> matches both! */
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 2);

    q.query = "gil";
    q.case_mode = STELOG_CASE_SENSITIVE; /* -s -> matches only lowercase gil! */
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);

    /* D. Fixed-strings (-F): exact path/HTTP method + mid-word substring */
    memset(&q, 0, sizeof(q));
    q.query = "POST /api/parts/mirror-rules";
    q.fixed_strings = true;
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);

    memset(&q, 0, sizeof(q));
    q.query = "api/part"; /* mid-word substring -> falls back cleanly to contiguous SIMD scan! */
    q.fixed_strings = true;
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);

    /* E. Standalone prefix* wildcard (`qz706*` -> matches QZ7061501A via contiguous text.bin SIMD scan) */
    memset(&q, 0, sizeof(q));
    q.query = "qz706*";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 1);
    CHECK(strstr(sr.hits[0].snippet.ptr, "QZ7061501A") != NULL);

    /* E2. Non-word-boundary prefix must NOT match mid-word ("runcat*" must NOT match "truncate") */
    memset(&q, 0, sizeof(q));
    q.query = "runcat*";
    CHECK(stelog_index_search(idx, &q, &sr) == 0);
    CHECK(sr.total_matches == 0);

    /* F. PCRE2 JIT Regex (-E) across all 3 acceleration tiers + smart-case + lookahead + error reporting */
    {
        char ebuf[128];
        StelogRegex *re = stelog_regex_compile("POST /api/parts/[a-z-]+", 23, STELOG_CASE_SMART, ebuf, sizeof(ebuf));
        CHECK(re != NULL);
        CHECK(stelog_regex_is_jit(re) == true);
        const StelogRegexPrefilter *pf = stelog_regex_prefilter(re);
        CHECK(pf->literal_len == 16); /* "POST /api/parts/" */
        CHECK(pf->term_count >= 2);   /* "api", "parts" -> Tier 1 SwissTable! */
        stelog_regex_destroy(re);

        /* Tier 1: SwissTable + PCRE2 JIT */
        memset(&q, 0, sizeof(q));
        q.query = "POST /api/parts/[a-z-]+";
        q.regex_mode = true;
        CHECK(stelog_index_search(idx, &q, &sr) == 0);
        CHECK(sr.total_matches == 1);

        /* Tier 2: NEON literal prefilter (`QZ706`) + PCRE2 JIT */
        memset(&q, 0, sizeof(q));
        q.query = "QZ706\\d+[A-Z]?";
        q.regex_mode = true;
        CHECK(stelog_index_search(idx, &q, &sr) == 0);
        CHECK(sr.total_matches == 1);
        CHECK(strstr(sr.hits[0].snippet.ptr, "QZ7061501A") != NULL);

        /* Tier 3: Top-level alternation + multiline anchor (P-core parallel PCRE2 JIT) */
        memset(&q, 0, sizeof(q));
        q.query = "^Mirror\\s+rules|Python\\s+GIL";
        q.regex_mode = true;
        CHECK(stelog_index_search(idx, &q, &sr) == 0);
        CHECK(sr.total_matches == 2);

        /* Smart-case regex: \D / \S must NOT trigger case-sensitive mode! */
        memset(&q, 0, sizeof(q));
        q.query = "\\D*mirror\\S*";
        q.regex_mode = true;
        CHECK(stelog_index_search(idx, &q, &sr) == 0);
        CHECK(sr.total_matches == 2); /* matches both "mirror-rules" and "Mirror rules" */

        /* PCRE2 lookahead */
        memset(&q, 0, sizeof(q));
        q.query = "docker(?=\\s+compose)";
        q.regex_mode = true;
        CHECK(stelog_index_search(idx, &q, &sr) == 0);
        CHECK(sr.total_matches == 1); /* matches "docker compose", excludes "without docker." */

        /* Invalid regex returns clean syntax error without crashing */
        memset(&q, 0, sizeof(q));
        q.query = "[unclosed";
        q.regex_mode = true;
        CHECK(stelog_index_search(idx, &q, &sr) != 0);
        CHECK(sr.error_msg.len > 0);
    }

    /* G. Zero-malloc QueryScratchArena invariant across repeated varied queries */
    size_t alloc_before = stelog_index_scratch_allocated_bytes(idx);
    static const char *loop_q[] = {
        "镜像 -docker", "GIL", "qz706*", "agent:agy truncat*", "镜像 | mirror -\"docker compose\""
    };
    for (int iter = 0; iter < 50; ++iter) {
        memset(&q, 0, sizeof(q));
        q.query = loop_q[iter % 5];
        CHECK(stelog_index_search(idx, &q, &sr) == 0);
    }
    CHECK(stelog_index_scratch_allocated_bytes(idx) == alloc_before);

    stelog_index_close(idx);
    stelog_rmdir_recursive(root);
    printf("[PASS] test_simd_substring_and_rg_query_syntax (SIMD + negation + modifiers + smart-case + -F + prefix* + 0-malloc)\n");
}

extern const char stelog_mcp_initialize_json[];
extern const size_t stelog_mcp_initialize_json_len;
extern const char stelog_mcp_tools_list_json[];
extern const size_t stelog_mcp_tools_list_json_len;

static void test_embedded_mcp_schemas(void) {
    CHECK(stelog_mcp_initialize_json_len > 100);
    CHECK(stelog_mcp_initialize_json[stelog_mcp_initialize_json_len] == '\0');
    CHECK(strlen(stelog_mcp_initialize_json) == stelog_mcp_initialize_json_len);
    CHECK(memchr(stelog_mcp_initialize_json, '\n', stelog_mcp_initialize_json_len) == NULL);
    CHECK(memchr(stelog_mcp_initialize_json, '\r', stelog_mcp_initialize_json_len) == NULL);
    CHECK(strstr(stelog_mcp_initialize_json, "\"protocolVersion\"") != NULL);
    CHECK(strstr(stelog_mcp_initialize_json, "\"instructions\"") != NULL);

    CHECK(stelog_mcp_tools_list_json_len > 1000);
    CHECK(stelog_mcp_tools_list_json[stelog_mcp_tools_list_json_len] == '\0');
    CHECK(strlen(stelog_mcp_tools_list_json) == stelog_mcp_tools_list_json_len);
    CHECK(memchr(stelog_mcp_tools_list_json, '\n', stelog_mcp_tools_list_json_len) == NULL);
    CHECK(memchr(stelog_mcp_tools_list_json, '\r', stelog_mcp_tools_list_json_len) == NULL);
    CHECK(strstr(stelog_mcp_tools_list_json, "\"search_history\"") != NULL);
    CHECK(strstr(stelog_mcp_tools_list_json, "\"doc_ids\"") != NULL);
    printf("[PASS] test_embedded_mcp_schemas (init=%zuB, tools=%zuB, single-line NDJSON safe)\n",
           stelog_mcp_initialize_json_len, stelog_mcp_tools_list_json_len);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_json_writer_and_mcp_request_parser();
    test_embedded_mcp_schemas();
    test_csift_upgrades_purification_absence_files_countby();
    test_simd_substring_and_rg_query_syntax();
    printf("All Phase 3 & 4 unit & integration tests passed!\n");
    return 0;
}
