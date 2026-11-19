#include "base/simd.h"
#include "base/utf8.h"

#include <stdio.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#define STELOG_SIMD_NEON 1
#include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#define STELOG_SIMD_SSE2 1
#include <emmintrin.h>
#endif

static inline uint8_t simd_ascii_lc(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

static inline uint8_t simd_ascii_uc(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - ('a' - 'A')) : c;
}

static inline bool simd_ascii_alnum(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

#ifdef STELOG_SIMD_NEON
static inline uint16_t neon_movemask_u8(uint8x16_t v) {
    static const uint8_t kBitMask[16] = {
        1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u,
        1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u
    };
    uint8x16_t bits = vandq_u8(v, vld1q_u8(kBitMask));
    uint16_t lo = (uint16_t)vaddv_u8(vget_low_u8(bits));
    uint16_t hi = (uint16_t)vaddv_u8(vget_high_u8(bits));
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}
#endif

const char *stelog_memmem(const char *haystack, size_t hay_len,
                          const char *needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (needle_len > hay_len) return NULL;
    uint8_t first = (uint8_t)needle[0];
    size_t limit = hay_len - needle_len + 1;

    if (needle_len == 1) {
        return (const char *)memchr(haystack, (int)first, hay_len);
    }

#if defined(STELOG_SIMD_NEON) || defined(STELOG_SIMD_SSE2)
    if (limit >= 16) {
        const uint8_t *h = (const uint8_t *)haystack;
        uint8_t last = (uint8_t)needle[needle_len - 1];
        size_t last_off = needle_len - 1;
        size_t i = 0;

#ifdef STELOG_SIMD_NEON
        uint8x16_t vfirst = vdupq_n_u8(first);
        uint8x16_t vlast  = vdupq_n_u8(last);
        while (i + 16 <= limit) {
            uint8x16_t hf = vld1q_u8(h + i);
            uint8x16_t hl = vld1q_u8(h + i + last_off);
            uint8x16_t eq = vandq_u8(vceqq_u8(hf, vfirst), vceqq_u8(hl, vlast));
            if (vmaxvq_u32(vreinterpretq_u32_u8(eq)) != 0u) {
                uint32_t mask = (uint32_t)neon_movemask_u8(eq);
                while (mask) {
                    uint32_t bit = (uint32_t)__builtin_ctz(mask);
                    mask &= mask - 1u;
                    size_t pos = i + bit;
                    if (needle_len == 2 || memcmp(haystack + pos + 1, needle + 1, needle_len - 2) == 0) {
                        return haystack + pos;
                    }
                }
            }
            i += 16;
        }
#elif defined(STELOG_SIMD_SSE2)
        __m128i vfirst = _mm_set1_epi8((char)first);
        __m128i vlast  = _mm_set1_epi8((char)last);
        while (i + 16 <= limit) {
            __m128i hf = _mm_loadu_si128((const __m128i *)(const void *)(h + i));
            __m128i hl = _mm_loadu_si128((const __m128i *)(const void *)(h + i + last_off));
            __m128i eq = _mm_and_si128(_mm_cmpeq_epi8(hf, vfirst), _mm_cmpeq_epi8(hl, vlast));
            uint32_t mask = (uint32_t)_mm_movemask_epi8(eq);
            while (mask) {
                uint32_t bit = (uint32_t)__builtin_ctz(mask);
                mask &= mask - 1u;
                size_t pos = i + bit;
                if (needle_len == 2 || memcmp(haystack + pos + 1, needle + 1, needle_len - 2) == 0) {
                    return haystack + pos;
                }
            }
            i += 16;
        }
#endif
        const char *p = haystack + i;
        const char *end = haystack + limit;
        while (p < end) {
            const char *hit = (const char *)memchr(p, (int)first, (size_t)(end - p));
            if (!hit) return NULL;
            if ((uint8_t)hit[last_off] == last &&
                (needle_len == 2 || memcmp(hit + 1, needle + 1, needle_len - 2) == 0)) {
                return hit;
            }
            p = hit + 1;
        }
        return NULL;
    }
#endif

    const char *p = haystack;
    const char *end = haystack + limit;
    while (p < end) {
        const char *hit = (const char *)memchr(p, (int)first, (size_t)(end - p));
        if (!hit) return NULL;
        if (memcmp(hit + 1, needle + 1, needle_len - 1) == 0) {
            return hit;
        }
        p = hit + 1;
    }
    return NULL;
}

static inline bool mem_eq_ci_inner(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t k = 0; k < n; ++k) {
        if (simd_ascii_lc(a[k]) != simd_ascii_lc(b[k])) return false;
    }
    return true;
}

const char *stelog_memcasemem(const char *haystack, size_t hay_len,
                              const char *needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (needle_len > hay_len) return NULL;

    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *nd = (const uint8_t *)needle;
    uint8_t f0 = simd_ascii_lc(nd[0]);
    uint8_t f1 = simd_ascii_uc(nd[0]);

    if (needle_len == 1) {
        if (f0 == f1) {
            return (const char *)memchr(haystack, (int)f0, hay_len);
        }
        size_t i = 0;
#ifdef STELOG_SIMD_NEON
        uint8x16_t vf0 = vdupq_n_u8(f0);
        uint8x16_t vf1 = vdupq_n_u8(f1);
        while (i + 16 <= hay_len) {
            uint8x16_t v = vld1q_u8(h + i);
            uint8x16_t eq = vorrq_u8(vceqq_u8(v, vf0), vceqq_u8(v, vf1));
            if (vmaxvq_u32(vreinterpretq_u32_u8(eq)) != 0u) {
                uint32_t mask = (uint32_t)neon_movemask_u8(eq);
                return haystack + i + (uint32_t)__builtin_ctz(mask);
            }
            i += 16;
        }
#elif defined(STELOG_SIMD_SSE2)
        __m128i vf0 = _mm_set1_epi8((char)f0);
        __m128i vf1 = _mm_set1_epi8((char)f1);
        while (i + 16 <= hay_len) {
            __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(h + i));
            __m128i eq = _mm_or_si128(_mm_cmpeq_epi8(v, vf0), _mm_cmpeq_epi8(v, vf1));
            uint32_t mask = (uint32_t)_mm_movemask_epi8(eq);
            if (mask) {
                return haystack + i + (uint32_t)__builtin_ctz(mask);
            }
            i += 16;
        }
#endif
        for (; i < hay_len; ++i) {
            if (h[i] == f0 || h[i] == f1) return haystack + i;
        }
        return NULL;
    }

    uint8_t l0 = simd_ascii_lc(nd[needle_len - 1]);
    uint8_t l1 = simd_ascii_uc(nd[needle_len - 1]);
    size_t last_off = needle_len - 1;
    size_t limit = hay_len - needle_len + 1;
    size_t i = 0;

#ifdef STELOG_SIMD_NEON
    if (limit >= 16) {
        uint8x16_t vf0 = vdupq_n_u8(f0);
        uint8x16_t vf1 = vdupq_n_u8(f1);
        uint8x16_t vl0 = vdupq_n_u8(l0);
        uint8x16_t vl1 = vdupq_n_u8(l1);
        while (i + 16 <= limit) {
            uint8x16_t hf = vld1q_u8(h + i);
            uint8x16_t hl = vld1q_u8(h + i + last_off);
            uint8x16_t eqf = vorrq_u8(vceqq_u8(hf, vf0), vceqq_u8(hf, vf1));
            uint8x16_t eql = vorrq_u8(vceqq_u8(hl, vl0), vceqq_u8(hl, vl1));
            uint8x16_t eq  = vandq_u8(eqf, eql);
            if (vmaxvq_u32(vreinterpretq_u32_u8(eq)) != 0u) {
                uint32_t mask = (uint32_t)neon_movemask_u8(eq);
                while (mask) {
                    uint32_t bit = (uint32_t)__builtin_ctz(mask);
                    mask &= mask - 1u;
                    size_t pos = i + bit;
                    if (needle_len == 2 || mem_eq_ci_inner(h + pos + 1, nd + 1, needle_len - 2)) {
                        return haystack + pos;
                    }
                }
            }
            i += 16;
        }
    }
#elif defined(STELOG_SIMD_SSE2)
    if (limit >= 16) {
        __m128i vf0 = _mm_set1_epi8((char)f0);
        __m128i vf1 = _mm_set1_epi8((char)f1);
        __m128i vl0 = _mm_set1_epi8((char)l0);
        __m128i vl1 = _mm_set1_epi8((char)l1);
        while (i + 16 <= limit) {
            __m128i hf = _mm_loadu_si128((const __m128i *)(const void *)(h + i));
            __m128i hl = _mm_loadu_si128((const __m128i *)(const void *)(h + i + last_off));
            __m128i eqf = _mm_or_si128(_mm_cmpeq_epi8(hf, vf0), _mm_cmpeq_epi8(hf, vf1));
            __m128i eql = _mm_or_si128(_mm_cmpeq_epi8(hl, vl0), _mm_cmpeq_epi8(hl, vl1));
            __m128i eq  = _mm_and_si128(eqf, eql);
            uint32_t mask = (uint32_t)_mm_movemask_epi8(eq);
            while (mask) {
                uint32_t bit = (uint32_t)__builtin_ctz(mask);
                mask &= mask - 1u;
                size_t pos = i + bit;
                if (needle_len == 2 || mem_eq_ci_inner(h + pos + 1, nd + 1, needle_len - 2)) {
                    return haystack + pos;
                }
            }
            i += 16;
        }
    }
#endif

    for (; i < limit; ++i) {
        if ((h[i] == f0 || h[i] == f1) &&
            (h[i + last_off] == l0 || h[i + last_off] == l1)) {
            if (needle_len == 2 || mem_eq_ci_inner(h + i + 1, nd + 1, needle_len - 2)) {
                return haystack + i;
            }
        }
    }
    return NULL;
}

bool stelog_is_word_prefix(const char *text, size_t text_len, const char *match_ptr) {
    if (!text || !match_ptr || match_ptr < text || match_ptr >= text + text_len) return false;
    if (match_ptr == text) return true;
    uint8_t cur = (uint8_t)match_ptr[0];
    if (cur >= 0x80u) return true; /* Non-ASCII / CJK starts are always valid prefix boundaries */
    uint8_t prev = (uint8_t)match_ptr[-1];
    if (!simd_ascii_alnum(prev)) return true; /* Whitespace, punctuation, '_' etc. */
    /* Also allow camelCase subtoken boundary (e.g. MiddleTruncation -> Truncat*) */
    if ((prev >= 'a' && prev <= 'z') && (cur >= 'A' && cur <= 'Z')) return true;
    return false;
}

bool stelog_is_word_suffix(const char *text, size_t text_len, const char *match_ptr, size_t match_len) {
    if (!text || !match_ptr || match_len == 0 || match_ptr < text) return false;
    const char *end_ptr = match_ptr + match_len;
    const char *txt_end = text + text_len;
    if (end_ptr > txt_end) return false;
    if (end_ptr == txt_end) return true;
    uint8_t last = (uint8_t)match_ptr[match_len - 1];
    if (last >= 0x80u) return true;
    uint8_t next = (uint8_t)end_ptr[0];
    if (!simd_ascii_alnum(next)) return true;
    /* Allow camelCase end boundary (e.g. Roaring in RoaringBitmap) */
    if ((last >= 'a' && last <= 'z') && (next >= 'A' && next <= 'Z')) return true;
    return false;
}

/*
 * Helper: extract string value right after `"key":"` inside [buf, buf + cap).
 * Handles optional single whitespace after colon (`"key": "val"` or `"key":"val"`).
 */
static bool extract_prefix_string_field(const char *buf, size_t cap,
                                        const char *key_quote, size_t key_len,
                                        const char **out_val, size_t *out_len) {
    const char *k = stelog_memmem(buf, cap, key_quote, key_len);
    if (!k) return false;
    const char *p = k + key_len;
    const char *end = buf + cap;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '"') return false;
    p++;
    const char *val_start = p;
    const char *val_end = (const char *)memchr(p, '"', (size_t)(end - p));
    if (!val_end) return false;
    *out_val = val_start;
    *out_len = (size_t)(val_end - val_start);
    return true;
}

static inline bool str_eq(const char *s, size_t len, const char *lit) {
    size_t lit_len = strlen(lit);
    return len == lit_len && memcmp(s, lit, lit_len) == 0;
}

bool stelog_codex_should_skip_line(const char *line, size_t line_len,
                                   bool include_tool_results,
                                   bool include_thinking) {
    if (line_len < 12) return true;
    /* Codex rollout lines place top-level "type" within the first ~128 bytes,
       and "payload":{"type":"..."} within the first ~256 bytes. */
    size_t scan_len = line_len < 256 ? line_len : 256;

    const char *top_type = NULL;
    size_t top_len = 0;
    if (!extract_prefix_string_field(line, scan_len, "\"type\"", 6, &top_type, &top_len)) {
        return false; /* Uncertain -> let simdjson parse safely */
    }

    /* Definite high-volume noise top-level types */
    if (str_eq(top_type, top_len, "compacted") ||
        str_eq(top_type, top_len, "token_usage_record") ||
        str_eq(top_type, top_len, "world_state") ||
        str_eq(top_type, top_len, "dynamic_tools_updated") ||
        str_eq(top_type, top_len, "security_risk_score") ||
        str_eq(top_type, top_len, "realtime_item") ||
        str_eq(top_type, top_len, "inter_agent_communication_metadata") ||
        str_eq(top_type, top_len, "retained_context")) {
        return true;
    }

    /* event_msg duplicates response_item for user/agent messages and carries token_count/item_completed noise */
    if (str_eq(top_type, top_len, "event_msg")) {
        return true;
    }

    if (str_eq(top_type, top_len, "response_item")) {
        /* Look for payload's nested "type" right after top_type */
        const char *after_top = top_type + top_len + 1;
        size_t rem = (size_t)((line + scan_len) - after_top);
        const char *payload_type = NULL;
        size_t payload_len = 0;
        if (extract_prefix_string_field(after_top, rem, "\"type\"", 6, &payload_type, &payload_len)) {
            if (str_eq(payload_type, payload_len, "additional_tools")) {
                return true;
            }
            if (!include_tool_results &&
                (str_eq(payload_type, payload_len, "custom_tool_call_output") ||
                 str_eq(payload_type, payload_len, "function_call_output"))) {
                return true;
            }
            if (!include_thinking && str_eq(payload_type, payload_len, "reasoning")) {
                return true;
            }
        }
    }

    return false;
}

bool stelog_agy_should_skip_line(const char *line, size_t line_len,
                                 bool include_tool_results,
                                 bool include_system) {
    if (line_len < 12) return true;
    size_t scan_len = line_len < 220 ? line_len : 220;

    const char *src = NULL;
    size_t src_len = 0;
    const char *type = NULL;
    size_t type_len = 0;

    bool has_src = extract_prefix_string_field(line, scan_len, "\"source\"", 8, &src, &src_len);
    bool has_type = extract_prefix_string_field(line, scan_len, "\"type\"", 6, &type, &type_len);
    if (!has_src || !has_type) return false;

    if (!include_system && str_eq(src, src_len, "SYSTEM")) {
        return true;
    }
    if (!include_tool_results && str_eq(src, src_len, "MODEL") && str_eq(type, type_len, "GENERIC")) {
        return true;
    }
    return false;
}

bool stelog_pi_should_skip_line(const char *line, size_t line_len,
                                bool include_tool_results) {
    if (line_len < 12) return true;
    size_t scan_len = line_len < 220 ? line_len : 220;

    const char *top_type = NULL;
    size_t top_len = 0;
    if (!extract_prefix_string_field(line, scan_len, "\"type\"", 6, &top_type, &top_len)) {
        return false;
    }

    if (str_eq(top_type, top_len, "thinking_level_change") ||
        str_eq(top_type, top_len, "compaction") ||
        str_eq(top_type, top_len, "custom") ||
        str_eq(top_type, top_len, "context_edit") ||
        str_eq(top_type, top_len, "session_info")) {
        return true;
    }

    if (!include_tool_results && str_eq(top_type, top_len, "message")) {
        const char *role = NULL;
        size_t role_len = 0;
        if (extract_prefix_string_field(line, scan_len, "\"role\"", 6, &role, &role_len)) {
            if (str_eq(role, role_len, "toolResult") || str_eq(role, role_len, "system")) {
                return true;
            }
        }
    }

    return false;
}

static bool mem_contains(const char *hay, size_t hay_len, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0 || hay_len < nl) return false;
    const char *end = hay + hay_len - nl;
    for (const char *p = hay; p <= end; ) {
        const char *hit = (const char *)memchr(p, needle[0], (size_t)(end - p) + 1);
        if (!hit) return false;
        if (memcmp(hit, needle, nl) == 0) return true;
        p = hit + 1;
    }
    return false;
}

bool stelog_droid_should_skip_line(const char *line, size_t line_len,
                                   bool include_tool_results) {
    if (line_len < 12) return true;
    size_t scan_len = line_len < 220 ? line_len : 220;

    const char *top_type = NULL;
    size_t top_len = 0;
    if (!extract_prefix_string_field(line, scan_len, "\"type\"", 6, &top_type, &top_len)) {
        return false;
    }
    if (str_eq(top_type, top_len, "todo_state") ||
        str_eq(top_type, top_len, "agent_turn_outcome") ||
        str_eq(top_type, top_len, "compaction_state")) {
        return true;
    }
    /* Pure tool-result turns are the bulk of a Droid transcript; drop them before simdjson unless
       requested, but keep any line that also carries a text block (user typed alongside results). */
    if (!include_tool_results && str_eq(top_type, top_len, "message") &&
        mem_contains(line, scan_len + 64 < line_len ? scan_len + 64 : line_len, "\"type\":\"tool_result\"") &&
        !mem_contains(line, line_len, "\"type\":\"text\"")) {
        return true;
    }
    return false;
}
/* UTF-8 continuation byte check: 10xxxxxx */
static inline bool utf8_is_cont(uint8_t b) {
    return (b & 0xC0u) == 0x80u;
}

/* Floor byte offset to valid UTF-8 codepoint boundary */
static size_t utf8_floor_boundary(const char *s, size_t pos) {
    while (pos > 0 && utf8_is_cont((uint8_t)s[pos])) {
        pos--;
    }
    return pos;
}

/* Ceil byte offset to valid UTF-8 codepoint boundary */
static size_t utf8_ceil_boundary(const char *s, size_t len, size_t pos) {
    while (pos < len && utf8_is_cont((uint8_t)s[pos])) {
        pos++;
    }
    return pos;
}

StelogStr stelog_truncate_middle_weighted(StelogArena *arena,
                                          const char *s,
                                          size_t len,
                                          size_t max_bytes,
                                          uint32_t head_pct,
                                          bool *out_truncated) {
    if (out_truncated) *out_truncated = false;
    if (!s || len <= max_bytes) {
        StelogStr r = { s, (uint32_t)len };
        return r;
    }
    if (out_truncated) *out_truncated = true;

    if (head_pct < 10) head_pct = 10;
    if (head_pct > 90) head_pct = 90;

    if (max_bytes < 48) {
        size_t cut = utf8_floor_boundary(s, max_bytes);
        char *buf = stelog_arena_strndup(arena, s, cut);
        StelogStr r = { buf, (uint32_t)cut };
        return r;
    }

    size_t marker_reserve = 32;
    size_t body_budget = max_bytes - marker_reserve;
    size_t head_budget = (body_budget * head_pct) / 100u;
    size_t tail_budget = body_budget - head_budget;

    size_t head_end = utf8_floor_boundary(s, head_budget);
    size_t tail_start = utf8_ceil_boundary(s, len, len - tail_budget);
    if (tail_start < head_end) tail_start = head_end;

    size_t removed_chars = stelog_utf8_count_codepoints(s + head_end, tail_start - head_end);
    char marker[64];
    int mlen = snprintf(marker, sizeof(marker), "\xE2\x80\xA6%zu chars truncated\xE2\x80\xA6", removed_chars);
    if (mlen < 0) mlen = 0;

    size_t total = head_end + (size_t)mlen + (len - tail_start);
    char *out = (char *)stelog_arena_alloc_aligned(arena, total + 1, 1);
    if (!out) {
        StelogStr r = { "", 0 };
        return r;
    }
    if (head_end > 0) memcpy(out, s, head_end);
    if (mlen > 0) memcpy(out + head_end, marker, (size_t)mlen);
    if (len > tail_start) memcpy(out + head_end + (size_t)mlen, s + tail_start, len - tail_start);
    out[total] = '\0';

    StelogStr r = { out, (uint32_t)total };
    return r;
}

StelogStr stelog_truncate_middle(StelogArena *arena,
                                 const char *s,
                                 size_t len,
                                 size_t max_bytes,
                                 bool *out_truncated) {
    return stelog_truncate_middle_weighted(arena, s, len, max_bytes, 50, out_truncated);
}
