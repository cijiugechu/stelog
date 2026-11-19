#ifndef STELOG_STORE_TOKENIZER_H
#define STELOG_STORE_TOKENIZER_H

#include "stelog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STELOG_TOKEN_HASH_SEED 0x9e3779b97f4a7c15ull

typedef void (*StelogTokenEmitFn)(uint64_t term_hash, void *ctx);

/*
 * Mixed ASCII/Code + CJK UTF-8 Bigram tokenizer.
 * - ASCII/Code: lowercased alphanumeric/underscore runs, plus camelCase & snake_case sub-tokens.
 * - CJK (Chinese/Japanese/Korean): UTF-8 Bigrams (2-gram) for runs >= 2 chars, unigram for single CJK char.
 * Calls `emit(term_hash, ctx)` for every token (may emit duplicates within the same doc; deduplicated downstream).
 * Returns total approximate token count.
 */
uint32_t stelog_tokenize_text(const char *text, size_t len, StelogTokenEmitFn emit, void *ctx);

/*
 * Parse a user search query into unique term hashes + optional phrase/case/punctuation verify spans,
 * prefix wildcards (`foo*`), query-wide negations (`-term`, `-"phrase"`), and inline modifiers
 * (`agent:`, `role:`, `project:`, `actor:`, `session:`, `since:`, `until:`, `@hexprefix`).
 */
#define STELOG_MAX_QUERY_TERMS     32
#define STELOG_MAX_VERIFY_PHRASES  16
#define STELOG_MAX_PREFIX_TERMS    8
#define STELOG_MAX_HIGHLIGHT_TERMS 16
#define STELOG_MAX_NEG_TERMS       16
#define STELOG_MAX_NEG_PHRASES     8
#define STELOG_MAX_QUERY_BRANCHES  8

typedef struct {
    uint64_t term_hashes[STELOG_MAX_QUERY_TERMS];
    uint32_t term_count;

    const char *verify_ptrs[STELOG_MAX_VERIFY_PHRASES];
    uint32_t    verify_lens[STELOG_MAX_VERIFY_PHRASES];
    uint8_t     verify_cs[STELOG_MAX_VERIFY_PHRASES];   /* 1 = case-sensitive, 0 = case-insensitive */
    uint8_t     verify_word[STELOG_MAX_VERIFY_PHRASES]; /* 1 = require word boundaries (e.g. -s gil vs gilbert) */
    uint32_t    verify_count;

    const char *prefix_ptrs[STELOG_MAX_PREFIX_TERMS];
    uint32_t    prefix_lens[STELOG_MAX_PREFIX_TERMS];
    uint8_t     prefix_cs[STELOG_MAX_PREFIX_TERMS];
    uint32_t    prefix_count;

    const char *highlight_ptrs[STELOG_MAX_HIGHLIGHT_TERMS];
    uint32_t    highlight_lens[STELOG_MAX_HIGHLIGHT_TERMS];
    uint8_t     highlight_cs[STELOG_MAX_HIGHLIGHT_TERMS];
    uint32_t    highlight_count;

    /* Primary raw branch slice */
    const char *raw_ptr;
    uint32_t raw_len;
} StelogQueryTokens;

/* Top-level query plan: inline modifiers + query-wide negations + OR branches */
typedef struct {
    StelogQueryTokens branches[STELOG_MAX_QUERY_BRANCHES];
    uint32_t branch_count;

    /* Query-wide fast inverted-index negations (-term: O(1) SwissTable, 0 text.bin touch) */
    uint64_t neg_term_hashes[STELOG_MAX_NEG_TERMS];
    uint32_t neg_term_count;

    /* Query-wide text-verified negations (-"phrase", case-sensitive -Term, -foo/bar, -3+CJK, -prefix*) */
    const char *neg_verify_ptrs[STELOG_MAX_NEG_PHRASES];
    uint32_t    neg_verify_lens[STELOG_MAX_NEG_PHRASES];
    uint8_t     neg_verify_cs[STELOG_MAX_NEG_PHRASES];
    uint8_t     neg_verify_is_prefix[STELOG_MAX_NEG_PHRASES];
    uint64_t    neg_verify_guard_hash[STELOG_MAX_NEG_PHRASES]; /* 0 = no inverted-index pre-filter */
    uint32_t    neg_verify_count;

    /* Inline modifiers extracted from query */
    StelogStr mod_agent;
    StelogStr mod_role;
    StelogStr mod_exclude_role;
    StelogStr mod_project;
    StelogStr mod_actor;
    StelogStr mod_session;
    StelogStr mod_since;
    StelogStr mod_until;

    bool effective_case_sensitive;
    bool has_modifiers;
} StelogQueryPlan;

/* Legacy helper (case-insensitive single branch) */
void stelog_tokenize_query(const char *query, size_t len, StelogQueryTokens *out);

/* Full query plan parser with smart-case / ignore-case / case-sensitive and fixed-strings (-F) */
void stelog_parse_query_plan_ex(const char *query,
                                size_t len,
                                StelogCaseMode case_mode,
                                bool fixed_strings,
                                StelogQueryPlan *out);

static inline void stelog_parse_query_plan(const char *query, size_t len, StelogQueryPlan *out) {
    stelog_parse_query_plan_ex(query, len, STELOG_CASE_SMART, false, out);
}

#ifdef __cplusplus
}
#endif

#endif /* STELOG_STORE_TOKENIZER_H */
