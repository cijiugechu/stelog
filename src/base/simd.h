#ifndef STELOG_BASE_SIMD_H
#define STELOG_BASE_SIMD_H

#include "stelog.h"
#include "base/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fast SIMD-accelerated bounded substring scan in [haystack, haystack + hay_len) */
const char *stelog_memmem(const char *haystack, size_t hay_len,
                          const char *needle, size_t needle_len);

/* Fast SIMD-accelerated (NEON/SSE2 2-byte filter) ASCII case-insensitive substring scan */
const char *stelog_memcasemem(const char *haystack, size_t hay_len,
                              const char *needle, size_t needle_len);

/* Dispatch to stelog_memmem (case_sensitive=true) or stelog_memcasemem (case_sensitive=false) */
static inline const char *stelog_find_substr(const char *haystack, size_t hay_len,
                                             const char *needle, size_t needle_len,
                                             bool case_sensitive) {
    return case_sensitive
        ? stelog_memmem(haystack, hay_len, needle, needle_len)
        : stelog_memcasemem(haystack, hay_len, needle, needle_len);
}

/* Check whether match_ptr inside [text, text + text_len) begins at a token/word boundary
   (start of text, non-ASCII-alnum predecessor including '_', camelCase boundary, or non-ASCII start). */
bool stelog_is_word_prefix(const char *text, size_t text_len, const char *match_ptr);

/* Check whether match_ptr[0..match_len) ends at a token/word boundary (for whole-word case verification). */
bool stelog_is_word_suffix(const char *text, size_t text_len, const char *match_ptr, size_t match_len);

/*
 * Fast line-level pre-filters (inspect only the prefix of a JSONL line before invoking simdjson).
 * Return true if the line is guaranteed noise under current flags and can be skipped entirely.
 */
bool stelog_codex_should_skip_line(const char *line, size_t line_len,
                                   bool include_tool_results,
                                   bool include_thinking);

bool stelog_agy_should_skip_line(const char *line, size_t line_len,
                                 bool include_tool_results,
                                 bool include_system);

bool stelog_pi_should_skip_line(const char *line, size_t line_len,
                                bool include_tool_results);

/*
 * UTF-8 boundary-safe Middle Truncation (Codex & Pi convention):
 * Keeps ~half max_bytes at the head and ~half at the tail, inserting "…N chars truncated…" in the middle.
 * Allocates result inside `arena` (or returns original slice if already <= max_bytes).
 */
StelogStr stelog_truncate_middle(StelogArena *arena,
                                 const char *s,
                                 size_t len,
                                 size_t max_bytes,
                                 bool *out_truncated);

/* Weighted Middle Truncation: head_pct in [10..90] controls head vs tail split
   (e.g. head_pct=30 keeps 30% head + 70% tail, ideal for assistant replies whose options/conclusions sit at the end). */
StelogStr stelog_truncate_middle_weighted(StelogArena *arena,
                                          const char *s,
                                          size_t len,
                                          size_t max_bytes,
                                          uint32_t head_pct,
                                          bool *out_truncated);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_SIMD_H */
