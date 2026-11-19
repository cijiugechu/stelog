#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "stelog.h"
#include "base/arena.h"
#include "base/hash.h"
#include "store/format.h"
#include "store/roaring.h"
#include "store/segment_builder.h"
#include "store/swisstable.h"
#include "store/tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)
#undef assert
#define assert CHECK

static void test_swisstable_simd(void) {
    const uint32_t N = 10000;
    StelogTermSlot *terms = (StelogTermSlot *)malloc(N * sizeof(StelogTermSlot));
    for (uint32_t i = 0; i < N; ++i) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "term_%u", i);
        terms[i].term_hash = stelog_wyhash(buf, (size_t)n, STELOG_TOKEN_HASH_SEED);
        terms[i].doc_freq = (i % 5 == 0) ? 1u : (i % 100 + 2u);
        terms[i].payload_or_off = i * 7u;
    }

    uint8_t *img = NULL;
    size_t img_len = 0;
    assert(stelog_swiss_build(terms, N, &img, &img_len) == 0);

    StelogSwissView view;
    assert(stelog_swiss_open(img, img_len, &view) == 0);
    assert(view.term_count == N);

    /* All inserted terms must hit */
    for (uint32_t i = 0; i < N; ++i) {
        const StelogTermSlot *hit = stelog_swiss_find(&view, terms[i].term_hash);
        assert(hit != NULL);
        assert(hit->term_hash == terms[i].term_hash);
        assert(hit->doc_freq == terms[i].doc_freq);
        assert(hit->payload_or_off == terms[i].payload_or_off);
    }

    /* Absent terms must miss */
    for (uint32_t i = 0; i < 1000; ++i) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "absent_xyz_%u", i);
        uint64_t h = stelog_wyhash(buf, (size_t)n, STELOG_TOKEN_HASH_SEED);
        assert(stelog_swiss_find(&view, h) == NULL);
    }

    free(img);
    free(terms);
    printf("[PASS] test_swisstable_simd (10,000 terms + 1,000 misses)\n");
}

static void test_roaring_containers_and_intersect(void) {
    /* Build 3 sets spanning 2 64K chunks (0..131071) exercising ARRAY16, BITSET8K, and RUN16 */
    uint32_t *setA = (uint32_t *)malloc(20000 * sizeof(uint32_t));
    uint32_t cntA = 0;
    /* Chunk 0: dense bitset (even numbers 0..20000 -> 10001 docs -> BITSET8K) */
    for (uint32_t d = 0; d <= 20000; d += 2) setA[cntA++] = d;
    /* Chunk 1: sparse array (65536 + 100, 65536 + 500, 65536 + 900 -> ARRAY16) */
    setA[cntA++] = 65536 + 100;
    setA[cntA++] = 65536 + 500;
    setA[cntA++] = 65536 + 900;

    uint32_t *setB = (uint32_t *)malloc(20000 * sizeof(uint32_t));
    uint32_t cntB = 0;
    /* Chunk 0: dense bitset (multiples of 3 from 0..30000 -> 10001 docs -> BITSET8K) */
    for (uint32_t d = 0; d <= 30000; d += 3) setB[cntB++] = d;
    /* Chunk 1: contiguous run 65536 + [400..1000] -> RUN16 */
    for (uint32_t d = 65536 + 400; d <= 65536 + 1000; ++d) setB[cntB++] = d;

    uint8_t *blobA = NULL, *blobB = NULL;
    size_t lenA = 0, lenB = 0;
    assert(stelog_roaring_encode(setA, cntA, &blobA, &lenA) == 0);
    assert(stelog_roaring_encode(setB, cntB, &blobB, &lenB) == 0);

    StelogDocSet ds[2] = {
        stelog_docset_from_roaring(blobA),
        stelog_docset_from_roaring(blobB)
    };
    assert(stelog_docset_contains(&ds[0], 12) == true);
    assert(stelog_docset_contains(&ds[0], 13) == false);
    assert(stelog_docset_contains(&ds[1], 65536 + 500) == true);
    assert(stelog_docset_contains(&ds[1], 65536 + 100) == false);

    StelogArena scratch;
    stelog_arena_init(&scratch, 1024 * 1024);

    /* Full intersection: multiples of 6 in [0..20000] (3334 docs) + {65536+500, 65536+900} = 3336 docs */
    uint32_t *out = NULL;
    uint32_t n = stelog_docset_intersect(ds, 2, 0, 200000, &scratch, &out);
    assert(n == 3336);
    assert(out[0] == 0);
    assert(out[1] == 6);
    assert(out[n - 2] == 65536 + 500);
    assert(out[n - 1] == 65536 + 900);

    /* Range-pruned intersection: [65536, 70000] must skip Chunk 0 entirely! */
    stelog_arena_reset(&scratch);
    n = stelog_docset_intersect(ds, 2, 65536, 70000, &scratch, &out);
    assert(n == 2);
    assert(out[0] == 65536 + 500);
    assert(out[1] == 65536 + 900);

    stelog_arena_destroy(&scratch);
    free(blobA);
    free(blobB);
    free(setA);
    free(setB);
    printf("[PASS] test_roaring_containers_and_intersect (ARRAY16 + BITSET8K + RUN16 + range pruning)\n");
}

static void test_tokenizer_mixed_code_and_cjk(void) {
    StelogQueryTokens qt;
    const char *q = "RolloutItem 决策溯源 \"exact phrase\"";
    stelog_tokenize_query(q, strlen(q), &qt);
    assert(qt.term_count >= 5); /* rolloutitem + 3 CJK bigrams + exact + phrase */
    assert(qt.verify_count == 2); /* "exact phrase" + "决策溯源" (>=3 CJK chars) */
    printf("[PASS] test_tokenizer_mixed_code_and_cjk\n");
}

static void write_file_str(const char *path, const char *content, bool append) {
    FILE *fp = fopen(path, append ? "a" : "w");
    assert(fp != NULL);
    fputs(content, fp);
    fclose(fp);
}

static void test_end_to_end_base_delta_query_compact(void) {
    char root[] = "/tmp/stelog_p2_test_XXXXXX";
    assert(mkdtemp(root) != NULL);

    char codex_home[512], codex_sess[512], agy_home[512], agy_log[512], pi_home[512], pi_sess[512], idx_dir[512];
    snprintf(codex_home, sizeof(codex_home), "%s/codex", root);
    snprintf(codex_sess, sizeof(codex_sess), "%s/codex/sessions/2026/11/19", root);
    snprintf(agy_home, sizeof(agy_home), "%s/agy", root);
    snprintf(agy_log, sizeof(agy_log), "%s/agy/brain/conv-1/.system_generated/logs", root);
    snprintf(pi_home, sizeof(pi_home), "%s/pi", root);
    snprintf(pi_sess, sizeof(pi_sess), "%s/pi/sessions/proj", root);
    snprintf(idx_dir, sizeof(idx_dir), "%s/index", root);

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s' '%s' '%s'", codex_sess, agy_log, pi_sess);
    assert(system(cmd) == 0);

    char f_codex[600], f_agy[600], f_pi[600];
    snprintf(f_codex, sizeof(f_codex), "%s/rollout-2026-11-19T01-00-00-11111111-2222-3333-4444-555555555555.jsonl", codex_sess);
    snprintf(f_agy, sizeof(f_agy), "%s/transcript.jsonl", agy_log);
    snprintf(f_pi, sizeof(f_pi), "%s/2026-11-19T02-00-00Z_pi-sess-1.jsonl", pi_sess);

    write_file_str(f_codex,
        "{\"timestamp\":\"2026-11-19T01:00:00.000Z\",\"type\":\"session_meta\",\"payload\":{\"id\":\"11111111-2222-3333-4444-555555555555\",\"cwd\":\"/repos/stelog\"}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:01.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"我们决定用 simdjson 和 RoaringBitmap 来做本地决策溯源\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:05.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"同意，使用 SwissTable 加速词典查找，并采用 Frozen Base 加 Hot Delta 分段。\"}]}}\n"
        "{\"timestamp\":\"2026-11-19T01:00:06.000Z\",\"type\":\"compacted\",\"payload\":{\"message\":\"HUGE NOISE THAT MUST BE SKIPPED\"}}\n",
        false);

    write_file_str(f_agy,
        "{\"step_index\":0,\"source\":\"USER_EXPLICIT\",\"type\":\"USER_INPUT\",\"created_at\":\"2026-11-19T02:00:00Z\",\"content\":\"<USER_REQUEST>\\n排查 gateway 内存泄漏，确认是否与 simdjson 有关\\n</USER_REQUEST>\"}\n"
        "{\"step_index\":1,\"source\":\"MODEL\",\"type\":\"PLANNER_RESPONSE\",\"created_at\":\"2026-11-19T02:00:10Z\",\"content\":\"已确认不是 simdjson 问题，而是缓存未设置 TTL。\"}\n",
        false);

    write_file_str(f_pi,
        "{\"type\":\"session\",\"version\":3,\"id\":\"pi-sess-1\",\"timestamp\":\"2026-11-19T03:00:00.000Z\",\"cwd\":\"/repos/stelog\"}\n"
        "{\"type\":\"model_change\",\"id\":\"m0\",\"timestamp\":\"2026-11-19T03:00:01.000Z\",\"provider\":\"anthropic\",\"modelId\":\"claude-opus\"}\n"
        "{\"type\":\"message\",\"id\":\"m1\",\"parentId\":\"m0\",\"timestamp\":\"2026-11-19T03:00:02.000Z\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"把默认截断策略改成 MiddleTruncation\"}]}}\n"
        "{\"type\":\"message\",\"id\":\"m2\",\"parentId\":\"m1\",\"timestamp\":\"2026-11-19T03:00:05.000Z\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"已在 DESIGN.md 确认决策溯源默认采用 MiddleTruncation。\"}]}}\n",
        false);

    /* 1. Initial full Base build */
    StelogIndexConfig icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.index_dir = idx_dir;
    icfg.codex_home = codex_home;
    icfg.agy_home = agy_home;
    icfg.pi_home = pi_home;
    icfg.droid_home = "/nonexistent/stelog-test-droid"; /* keeps ~/.factory out of fixtures */
    icfg.threads = 2;

    StelogIndexReport rep;
    assert(stelog_index_sync(&icfg, &rep) == 0);
    assert(rep.was_rebuild == true);
    assert(rep.base_gen == 1 && rep.delta_gen == 0);
    assert(rep.base_docs == 6); /* 2 codex + 2 agy + 2 pi */

    /* 2. No-op sync when unchanged */
    assert(stelog_index_sync(&icfg, &rep) == 0);
    assert(rep.noop_up_to_date == true);
    assert(rep.base_gen == 1 && rep.delta_gen == 0);

    /* 3. Append-only hot tail to Codex file -> MUST go into Hot Delta without rebuilding Base! */
    write_file_str(f_codex,
        "{\"timestamp\":\"2026-11-19T04:00:00.000Z\",\"type\":\"response_item\",\"payload\":{\"type\":\"message\",\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"新增决策：mimalloc 延后到 Phase2 profile 后再决定\"}]}}\n",
        true);

    assert(stelog_index_sync(&icfg, &rep) == 0);
    assert(rep.noop_up_to_date == false);
    assert(rep.was_rebuild == false);
    assert(rep.base_gen == 1 && rep.delta_gen == 1);
    assert(rep.base_docs == 6 && rep.delta_docs == 1);
    assert(rep.new_docs_added == 1);

    /* 4. Open mmap index and verify queries across BOTH Base and Hot Delta */
    StelogIndex *idx = stelog_index_open(idx_dir);
    assert(idx != NULL);

    /* 4a. Query spanning Base (Codex + AGY) */
    StelogQuery q;
    memset(&q, 0, sizeof(q));
    q.query = "simdjson";
    StelogSearchResult sr;
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.total_matches == 3); /* 1 codex + 2 agy */

    /* 4b. CamelCase subtoken query: searching "roaring" matches "RoaringBitmap"! */
    q.query = "roaring";
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.total_matches == 1);
    assert(stelog_tag_agent(sr.hits[0].tag) == STELOG_AGENT_CODEX);

    /* 4c. CJK >= 3-char exact verification + cross-agent timeline */
    q.query = "决策溯源";
    q.sort_by_time = true;
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.total_matches == 2); /* codex at 01:00:01, pi at 03:00:05 */
    assert(stelog_tag_agent(sr.hits[0].tag) == STELOG_AGENT_CODEX);
    assert(stelog_tag_agent(sr.hits[1].tag) == STELOG_AGENT_PI);
    assert(sr.hits[0].ts_ms < sr.hits[1].ts_ms);

    /* 4d. Query hitting Hot Delta only ("mimalloc") */
    q.query = "mimalloc";
    q.sort_by_time = false;
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.total_matches == 1);
    uint64_t mimalloc_id = sr.hits[0].doc_id;
    uint64_t internal_id = 0;
    assert(mimalloc_id > 0 && mimalloc_id <= STELOG_DOC_KEY_MASK);
    assert(stelog_index_resolve_doc_id(idx, mimalloc_id, &internal_id) && stelog_doc_is_delta(internal_id));
    assert(!stelog_index_resolve_doc_id(idx, stelog_make_doc_id(true, 0), NULL));

    /* 4e. Context window lookup via CSR session_idx.bin */
    q.query = "SwissTable";
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.count == 1);
    uint64_t anchor_id = sr.hits[0].doc_id;
    StelogContextResult ctx_res;
    assert(stelog_index_context(idx, anchor_id, 3, 1024, false, &ctx_res) == 0);
    assert(ctx_res.count == 2);
    assert(ctx_res.anchor_index == 1);

    /* 4f. get_document: both clean text and O(1) pread raw JSONL */
    StelogDocResult doc_res;
    assert(stelog_index_get_document(idx, anchor_id, false, 0, 4096, false, &doc_res) == 0);
    assert(strstr(doc_res.content.ptr, "SwissTable") != NULL);
    assert(stelog_index_get_document(idx, anchor_id, true, 0, 4096, false, &doc_res) == 0);
    assert(strstr(doc_res.content.ptr, "\"response_item\"") != NULL);

    /* 4g. list_sessions across Base + Delta */
    StelogSessionListResult sess_res;
    assert(stelog_index_list_sessions(idx, 0, NULL, 0, 0, 10, 0, &sess_res) == 0);
    assert(sess_res.count == 3);
    /* Most recent session must be Codex (appended at 04:00:00 -> merged Base 2 + Delta 1 = 3 docs) */
    assert(sess_res.items[0].agent == STELOG_AGENT_CODEX);
    assert(sess_res.items[0].doc_count == 3);

    /* 5. Compact Base + Delta -> Base gen 2, Delta cleared, reload_if_changed works! */
    icfg.force_compact = true;
    assert(stelog_index_sync(&icfg, &rep) == 0);
    assert(rep.did_compact == true);
    assert(rep.base_gen == 2 && rep.delta_gen == 0);
    assert(rep.base_docs == 7);

    assert(stelog_index_reload_if_changed(idx) == 1);
    q.query = "mimalloc";
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.total_matches == 1);
    /* Moved into Base gen 2, but the public doc_id is unchanged */
    assert(sr.hits[0].doc_id == mimalloc_id);
    assert(stelog_index_resolve_doc_id(idx, mimalloc_id, &internal_id) && !stelog_doc_is_delta(internal_id));
    q.query = "SwissTable";
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.count == 1 && sr.hits[0].doc_id == anchor_id);

    /* 6. A full rebuild from the transcripts reproduces the same doc_ids */
    icfg.force_compact = false;
    icfg.force_rebuild = true;
    assert(stelog_index_sync(&icfg, &rep) == 0);
    assert(rep.was_rebuild == true);
    assert(stelog_index_reload_if_changed(idx) == 1);
    q.query = "mimalloc";
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.total_matches == 1 && sr.hits[0].doc_id == mimalloc_id);
    q.query = "SwissTable";
    assert(stelog_index_search(idx, &q, &sr) == 0);
    assert(sr.count == 1 && sr.hits[0].doc_id == anchor_id);
    assert(stelog_index_context(idx, anchor_id, 3, 1024, false, &ctx_res) == 0);
    assert(ctx_res.count == 3 && ctx_res.items[ctx_res.anchor_index].doc_id == anchor_id); /* + appended mimalloc turn */

    stelog_index_close(idx);
    stelog_rmdir_recursive(root);
    printf("[PASS] test_end_to_end_base_delta_query_compact (Base + Hot Delta + Reload + Compact)\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_swisstable_simd();
    test_roaring_containers_and_intersect();
    test_tokenizer_mixed_code_and_cjk();
    test_end_to_end_base_delta_query_compact();
    printf("All Phase 2 unit & integration tests passed!\n");
    return 0;
}
