#include "stelog.h"
#include "stelog_json.h"
#include "adapters/adapter.h"
#include "base/arena.h"
#include "base/hash.h"
#include "base/simd.h"
#include "base/time_iso8601.h"
#include "base/utf8.h"

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
#undef assert
#define assert CHECK

static void test_iso8601(void) {
    int64_t t1 = stelog_parse_iso8601_ms("2026-11-04T05:35:46.162Z", 24);
    assert(t1 > 0);
    int64_t t2 = stelog_parse_iso8601_ms("2026-11-04T05:35:46Z", 20);
    assert(t1 - t2 == 162);

    /* +08:00 timezone equivalence */
    int64_t t3 = stelog_parse_iso8601_ms("2026-11-04T13:35:46.162+08:00", 29);
    assert(t3 == t1);

    /* Epoch check */
    int64_t ep = stelog_parse_iso8601_ms("1970-01-01T00:00:00.000Z", 24);
    assert(ep == 0);

    /* Invalid */
    assert(stelog_parse_iso8601_ms("not-a-timestamp", 15) == 0);
    printf("[PASS] test_iso8601\n");
}

static void test_truncate_middle_utf8(void) {
    StelogArena arena;
    stelog_arena_init(&arena, 4096);

    /* Chinese + Emoji string that would break if sliced mid-codepoint */
    const char *src = "😀😀我们决定使用simdjson配合C11做面向数据设计的本地历史检索系统，并在Phase2压测后决定是否引入mimalloc🚀🎉";
    size_t slen = strlen(src);
    bool truncated = false;

    StelogStr out = stelog_truncate_middle(&arena, src, slen, 72, &truncated);
    assert(truncated == true);
    assert(out.len <= 80);
    assert(stelog_utf8_validate(out.ptr, out.len));
    assert(strstr(out.ptr, "chars truncated") != NULL);

    /* No truncation when fits */
    StelogStr same = stelog_truncate_middle(&arena, "hello", 5, 64, &truncated);
    assert(truncated == false);
    assert(same.len == 5 && memcmp(same.ptr, "hello", 5) == 0);

    stelog_arena_destroy(&arena);
    printf("[PASS] test_truncate_middle_utf8\n");
}

static void test_prefilters_and_simdjson(void) {
    /* Pre-filter checks */
    const char *codex_compacted = "{\"timestamp\":\"2026-11-04T05:35:46.162Z\",\"type\":\"compacted\",\"payload\":{\"message\":\"huge\"}}";
    assert(stelog_codex_should_skip_line(codex_compacted, strlen(codex_compacted), false, false) == true);

    const char *codex_tool_out = "{\"timestamp\":\"2026-11-04T05:35:54.828Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"custom_tool_call_output\",\"output\":\"big\"}}";
    assert(stelog_codex_should_skip_line(codex_tool_out, strlen(codex_tool_out), false, false) == true);
    assert(stelog_codex_should_skip_line(codex_tool_out, strlen(codex_tool_out), true, false) == false);

    const char *codex_msg = "{\"timestamp\":\"2026-11-04T05:38:19.377Z\",\"ordinal\":78,\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"id\":\"msg_1\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"我们决定用 Zig 构建 C11 + simdjson\"}]}}";
    assert(stelog_codex_should_skip_line(codex_msg, strlen(codex_msg), false, false) == false);

    /* Parse Codex, AGY, Pi lines via simdjson bridge */
    StelogJsonParser *jp = stelog_json_parser_create();
    StelogArena arena;
    stelog_arena_init(&arena, 65536);
    StelogParseOptions opts = { .tool_call_max_bytes = 256, .include_tool_results = false, .include_thinking = false, .include_system = false };
    StelogStreamContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.session_id = (StelogStr){ "sess-1", 6 };
    ctx.cwd = (StelogStr){ "/Users/baka/repos/stelog", 24 };

    StelogLineResult res;
    assert(stelog_json_parse_codex_line(jp, codex_msg, strlen(codex_msg), strlen(codex_msg),
                                        0, 1234, &opts, &ctx, &arena, &res) == 0);
    assert(res.count == 1);
    assert(stelog_tag_agent(res.events[0].tag) == STELOG_AGENT_CODEX);
    assert(stelog_tag_role(res.events[0].tag) == STELOG_ROLE_ASSISTANT);
    assert(strstr(res.events[0].text.ptr, "simdjson") != NULL);

    /* AGY USER_REQUEST tag stripping */
    const char *agy_user = "{\"step_index\":0,\"source\":\"USER_EXPLICIT\",\"type\":\"USER_INPUT\",\"created_at\":\"2026-11-11T02:52:27Z\",\"content\":\"<USER_REQUEST>\\n改用 Roaring Bitmap\\n</USER_REQUEST>\"}";
    assert(stelog_agy_should_skip_line(agy_user, strlen(agy_user), false, false) == false);
    assert(stelog_json_parse_agy_line(jp, agy_user, strlen(agy_user), strlen(agy_user),
                                      1, 0, &opts, &ctx, &arena, &res) == 0);
    assert(res.count == 1);
    assert(stelog_tag_role(res.events[0].tag) == STELOG_ROLE_USER);
    assert(strcmp(res.events[0].text.ptr, "改用 Roaring Bitmap") == 0);

    /* AGY MODEL:GENERIC skipped by default */
    const char *agy_gen = "{\"step_index\":2,\"source\":\"MODEL\",\"type\":\"GENERIC\",\"created_at\":\"2026-11-11T02:52:37Z\",\"content\":\"stdout...\"}";
    assert(stelog_agy_should_skip_line(agy_gen, strlen(agy_gen), false, false) == true);

    /* Pi model_change + assistant message with text + toolCall */
    const char *pi_mc = "{\"type\":\"model_change\",\"id\":\"m1\",\"timestamp\":\"2026-08-17T04:06:45.847Z\",\"provider\":\"anthropic\",\"modelId\":\"claude-opus\"}";
    const char *pi_asst = "{\"type\":\"message\",\"id\":\"m2\",\"parentId\":\"m1\",\"timestamp\":\"2026-08-17T04:07:57.468Z\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"先写方案文档\"},{\"type\":\"toolCall\",\"name\":\"write_file\",\"arguments\":{\"path\":\"DESIGN.md\"}}]}}";
    assert(stelog_json_parse_pi_line(jp, pi_mc, strlen(pi_mc), strlen(pi_mc),
                                     2, 0, &opts, &ctx, &arena, &res) == 0);
    assert(res.updated_context == true);
    assert(strcmp(ctx.actor.ptr, "pi:anthropic/claude-opus") == 0);

    assert(stelog_json_parse_pi_line(jp, pi_asst, strlen(pi_asst), strlen(pi_asst),
                                     2, 100, &opts, &ctx, &arena, &res) == 0);
    assert(res.count == 2);
    assert(stelog_tag_role(res.events[0].tag) == STELOG_ROLE_ASSISTANT);
    assert(strcmp(res.events[0].actor.ptr, "pi:anthropic/claude-opus") == 0);
    assert(strcmp(res.events[0].parent_id.ptr, "m1") == 0);
    assert(stelog_tag_role(res.events[1].tag) == STELOG_ROLE_TOOL_CALL);

    stelog_arena_destroy(&arena);
    stelog_json_parser_destroy(jp);
    printf("[PASS] test_prefilters_and_simdjson\n");
}

static void test_chunk_boundary_invariance(void) {
    char tmpl[] = "/tmp/stelog_chunk_test_XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    FILE *fp = fdopen(fd, "w");
    assert(fp != NULL);

    /* Mix of small valid lines, giant compacted noise line (> 2KB), and giant valid assistant line (> 2KB) */
    fprintf(fp, "{\"timestamp\":\"2026-11-04T05:35:46.162Z\",\"type\":\"session_meta\",\"payload\":{\"id\":\"s1\",\"cwd\":\"/repo\"}}\n");
    for (int i = 0; i < 15; ++i) {
        fprintf(fp, "{\"timestamp\":\"2026-11-04T05:35:%02d.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"decision #%d\"}]}}\n", i % 60, i);
        if (i == 5) {
            /* Giant compacted noise line spanning many tiny chunks */
            fprintf(fp, "{\"timestamp\":\"2026-11-04T05:35:50.000Z\",\"type\":\"compacted\",\"payload\":{\"blob\":\"");
            for (int k = 0; k < 4000; ++k) fputc('X', fp);
            fprintf(fp, "\"}}\n");
        }
        if (i == 10) {
            /* Giant VALID line spanning many tiny chunks */
            fprintf(fp, "{\"timestamp\":\"2026-11-04T05:35:55.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"");
            for (int k = 0; k < 3000; ++k) fputc('A', fp);
            fprintf(fp, "\"}]}}\n");
        }
    }
    /* Final line WITHOUT trailing newline */
    fprintf(fp, "{\"timestamp\":\"2026-11-04T05:36:00.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"final no newline\"}]}}");
    fclose(fp);

    StelogParseOptions opts = { .tool_call_max_bytes = 512, .include_tool_results = false, .include_thinking = false, .include_system = false };
    StelogAgentStats baseline;
    assert(stelog_scan_single_file_for_test(tmpl, STELOG_AGENT_CODEX, 1024 * 1024, &opts, &baseline) == 0);
    assert(baseline.parse_errors == 0);
    assert(baseline.events_emitted == 17); /* 15 user + 1 giant assistant + 1 final assistant */

    /* Test multiple adversarial chunk sizes (including prime sizes & tiny 64B chunks!) */
    uint32_t sizes[] = { 64, 127, 128, 257, 512, 1009, 4096 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        StelogAgentStats s;
        assert(stelog_scan_single_file_for_test(tmpl, STELOG_AGENT_CODEX, sizes[i], &opts, &s) == 0);
        assert(s.parse_errors == 0);
        assert(s.events_emitted == baseline.events_emitted);
        assert(s.text_bytes_emitted == baseline.text_bytes_emitted);
        assert(s.lines_parsed_simdjson == baseline.lines_parsed_simdjson);
    }

    unlink(tmpl);
    printf("[PASS] test_chunk_boundary_invariance (verified across 7 chunk sizes down to 64B)\n");
}

int main(void) {
    test_iso8601();
    test_truncate_middle_utf8();
    test_prefilters_and_simdjson();
    test_chunk_boundary_invariance();
    printf("All Phase 1 unit tests passed!\n");
    return 0;
}
