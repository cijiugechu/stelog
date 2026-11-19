#ifndef STELOG_QUERY_REGEX_ENGINE_H
#define STELOG_QUERY_REGEX_ENGINE_H

#include "stelog.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STELOG_REGEX_MAX_PREFILTER_LIT   64
#define STELOG_REGEX_MAX_PREFILTER_TERMS 16

typedef struct StelogRegex StelogRegex;
typedef struct StelogRegexMatcher StelogRegexMatcher;

typedef struct {
    char     literal[STELOG_REGEX_MAX_PREFILTER_LIT + 1];
    uint32_t literal_len;
    bool     literal_cs;
    uint64_t term_hashes[STELOG_REGEX_MAX_PREFILTER_TERMS];
    uint32_t term_count;
} StelogRegexPrefilter;

/*
 * Compile a UTF-8 PCRE2 regex with JIT + automatic required-literal prefilter extraction.
 * - Uses PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE (ripgrep semantics: ^ and $ match line boundaries).
 * - Honors STELOG_CASE_SMART (ignores escaped uppercase like \D, \W, \S, \B, \p{Han}),
 *   STELOG_CASE_INSENSITIVE (-i), and STELOG_CASE_SENSITIVE (-s).
 * Returns NULL on syntax error and populates `err_buf` if provided.
 */
StelogRegex *stelog_regex_compile(const char *pattern,
                                  size_t pattern_len,
                                  StelogCaseMode case_mode,
                                  char *err_buf,
                                  size_t err_cap);

void stelog_regex_destroy(StelogRegex *re);

bool stelog_regex_is_jit(const StelogRegex *re);
const StelogRegexPrefilter *stelog_regex_prefilter(const StelogRegex *re);

/* Per-thread match state (match_data + match_context with ReDoS limits + JIT stack) */
StelogRegexMatcher *stelog_regex_matcher_create(const StelogRegex *re);
void stelog_regex_matcher_destroy(StelogRegexMatcher *m);

/* Find first match in [text, text + text_len) starting at `start_off`.
   Assumes `text` is valid UTF-8 (guaranteed for all indexed stelog text via simdutf).
   Returns true and sets [out_start, out_end) if matched. */
bool stelog_regex_find(const StelogRegex *re,
                       StelogRegexMatcher *m,
                       const char *text,
                       size_t text_len,
                       size_t start_off,
                       size_t *out_start,
                       size_t *out_end);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_QUERY_REGEX_ENGINE_H */
