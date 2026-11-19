#include "store/tokenizer.h"
#include "base/hash.h"
#include "base/utf8.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool is_ascii_alnum(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static inline bool is_ascii_upper(uint8_t c) {
    return c >= 'A' && c <= 'Z';
}

static inline bool is_ascii_lower(uint8_t c) {
    return c >= 'a' && c <= 'z';
}

static inline uint8_t to_ascii_lower(uint8_t c) {
    return is_ascii_upper(c) ? (uint8_t)(c + ('a' - 'A')) : c;
}

/* Decode one UTF-8 codepoint; returns bytes consumed (1..4) */
static inline size_t utf8_decode(const uint8_t *p, size_t rem, uint32_t *out_cp) {
    uint8_t b0 = p[0];
    if (b0 < 0x80u) {
        *out_cp = b0;
        return 1;
    }
    if ((b0 & 0xE0u) == 0xC0u && rem >= 2) {
        *out_cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        return 2;
    }
    if ((b0 & 0xF0u) == 0xE0u && rem >= 3) {
        *out_cp = ((uint32_t)(b0 & 0x0Fu) << 12) |
                  ((uint32_t)(p[1] & 0x3Fu) << 6) |
                  (uint32_t)(p[2] & 0x3Fu);
        return 3;
    }
    if ((b0 & 0xF8u) == 0xF0u && rem >= 4) {
        *out_cp = ((uint32_t)(b0 & 0x07u) << 18) |
                  ((uint32_t)(p[1] & 0x3Fu) << 12) |
                  ((uint32_t)(p[2] & 0x3Fu) << 6) |
                  (uint32_t)(p[3] & 0x3Fu);
        return 4;
    }
    *out_cp = b0;
    return 1;
}

static inline bool is_cjk_codepoint(uint32_t cp) {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) ||   /* CJK Unified Ideographs */
           (cp >= 0x3400u && cp <= 0x4DBFu) ||   /* CJK Extension A */
           (cp >= 0x3040u && cp <= 0x30FFu) ||   /* Hiragana & Katakana */
           (cp >= 0xAC00u && cp <= 0xD7AFu) ||   /* Hangul */
           (cp >= 0x20000u && cp <= 0x2A6DFu);   /* CJK Extension B */
}

static inline void emit_ascii_lower(const char *s, size_t len, StelogTokenEmitFn emit, void *ctx, uint32_t *count) {
    if (len == 0) return;
    if (len > 64) len = 64;
    char buf[64];
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (char)to_ascii_lower((uint8_t)s[i]);
    }
    emit(stelog_wyhash(buf, len, STELOG_TOKEN_HASH_SEED), ctx);
    (*count)++;
}

static void emit_identifier_with_subtokens(const char *s, size_t len, StelogTokenEmitFn emit, void *ctx, uint32_t *count) {
    /* Strip leading/trailing underscores */
    while (len > 0 && s[0] == '_') { s++; len--; }
    while (len > 0 && s[len - 1] == '_') { len--; }
    if (len < 2) return;

    /* 1. Full identifier token (lowercased) */
    emit_ascii_lower(s, len, emit, ctx, count);

    /* 2. Split on underscores and camelCase boundaries */
    size_t part_start = 0;
    bool has_split = false;
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = (uint8_t)s[i];
        if (c == '_') {
            has_split = true;
            if (i > part_start && (i - part_start) >= 2) {
                emit_ascii_lower(s + part_start, i - part_start, emit, ctx, count);
            }
            part_start = i + 1;
        } else if (i > part_start && is_ascii_upper(c)) {
            uint8_t prev = (uint8_t)s[i - 1];
            bool next_lower = (i + 1 < len) && is_ascii_lower((uint8_t)s[i + 1]);
            if (is_ascii_lower(prev) || (is_ascii_upper(prev) && next_lower)) {
                has_split = true;
                if ((i - part_start) >= 2) {
                    emit_ascii_lower(s + part_start, i - part_start, emit, ctx, count);
                }
                part_start = i;
            }
        }
    }
    if (has_split && len > part_start && (len - part_start) >= 2) {
        emit_ascii_lower(s + part_start, len - part_start, emit, ctx, count);
    }
}

static inline uint64_t hash_cjk_bigram(uint32_t cp1, uint32_t cp2) {
    uint32_t pair[2] = { cp1, cp2 };
    return stelog_wyhash(pair, sizeof(pair), STELOG_TOKEN_HASH_SEED ^ 0xC1C1C1C1ull);
}

static inline uint64_t hash_cjk_unigram(uint32_t cp) {
    return stelog_wyhash(&cp, sizeof(cp), STELOG_TOKEN_HASH_SEED ^ 0xD2D2D2D2ull);
}

/* Emit CJK bigrams/unigrams from a contiguous UTF-32 codepoint buffer produced by simdutf */
static void emit_cjk_from_u32(const uint32_t *cps, size_t n, StelogTokenEmitFn emit, void *ctx, uint32_t *count) {
    size_t i = 0;
    while (i < n) {
        if (!is_cjk_codepoint(cps[i])) {
            i++;
            continue;
        }
        size_t start = i;
        i++;
        while (i < n && is_cjk_codepoint(cps[i])) i++;
        size_t run_len = i - start;
        if (run_len == 1) {
            emit(hash_cjk_unigram(cps[start]), ctx);
            (*count)++;
        } else {
            for (size_t k = start; k + 1 < i; ++k) {
                emit(hash_cjk_bigram(cps[k], cps[k + 1]), ctx);
                (*count)++;
            }
        }
    }
}

uint32_t stelog_tokenize_text(const char *text, size_t len, StelogTokenEmitFn emit, void *ctx) {
    if (!text || len == 0 || !emit) return 0;
    const uint8_t *p = (const uint8_t *)text;
    size_t i = 0;
    uint32_t count = 0;

    /* Fast path: simdutf SIMD ASCII check (~20+ GB/s). For pure-ASCII code/logs (70-85% of docs),
       skip all UTF-8 decoding and branch directly into tight ASCII identifier loop! */
    if (stelog_utf8_validate_ascii(text, len)) {
        while (i < len) {
            uint8_t b = p[i];
            if (is_ascii_alnum(b) || b == '_') {
                size_t start = i;
                while (i < len && (is_ascii_alnum(p[i]) || p[i] == '_')) i++;
                emit_identifier_with_subtokens(text + start, i - start, emit, ctx, &count);
            } else {
                i++;
            }
        }
        return count;
    }

    /* Mixed ASCII + non-ASCII path: batch-convert each contiguous non-ASCII span via simdutf::convert_utf8_to_utf32 */
    uint32_t stack_u32[512];
    while (i < len) {
        uint8_t b = p[i];
        if (b < 0x80u) {
            if (is_ascii_alnum(b) || b == '_') {
                size_t start = i;
                while (i < len && p[i] < 0x80u && (is_ascii_alnum(p[i]) || p[i] == '_')) {
                    i++;
                }
                emit_identifier_with_subtokens(text + start, i - start, emit, ctx, &count);
            } else {
                i++;
            }
        } else {
            size_t span_start = i;
            while (i < len && p[i] >= 0x80u) i++;
            size_t span_len = i - span_start;

            uint32_t *u32_buf = stack_u32;
            uint32_t *heap_u32 = NULL;
            if (span_len > 512) {
                heap_u32 = (uint32_t *)malloc(span_len * sizeof(uint32_t));
                if (heap_u32) u32_buf = heap_u32;
            }

            if (u32_buf != stack_u32 || span_len <= 512) {
                size_t ncps = stelog_utf8_to_utf32(text + span_start, span_len, u32_buf);
                if (ncps > 0) {
                    emit_cjk_from_u32(u32_buf, ncps, emit, ctx, &count);
                    free(heap_u32);
                    continue;
                }
            }
            free(heap_u32);

            /* Fallback only if span contained malformed UTF-8 bytes */
            size_t j = span_start;
            while (j < i) {
                uint32_t cp = 0;
                size_t adv = utf8_decode(p + j, i - j, &cp);
                if (is_cjk_codepoint(cp)) {
                    uint32_t prev_cp = cp;
                    uint32_t run_len = 1;
                    j += adv;
                    while (j < i) {
                        uint32_t next_cp = 0;
                        size_t nadv = utf8_decode(p + j, i - j, &next_cp);
                        if (!is_cjk_codepoint(next_cp)) break;
                        emit(hash_cjk_bigram(prev_cp, next_cp), ctx);
                        count++;
                        prev_cp = next_cp;
                        run_len++;
                        j += nadv;
                    }
                    if (run_len == 1) {
                        emit(hash_cjk_unigram(prev_cp), ctx);
                        count++;
                    }
                } else {
                    j += adv;
                }
            }
        }
    }
    return count;
}

static void query_emit_cb(uint64_t term_hash, void *ctx) {
    StelogQueryTokens *qt = (StelogQueryTokens *)ctx;
    for (uint32_t i = 0; i < qt->term_count; ++i) {
        if (qt->term_hashes[i] == term_hash) return;
    }
    if (qt->term_count < STELOG_MAX_QUERY_TERMS) {
        qt->term_hashes[qt->term_count++] = term_hash;
    }
}

static inline bool is_ascii_alpha(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline bool is_ascii_hex_or_dash(uint8_t c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-';
}

static bool span_has_ascii_upper(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (is_ascii_upper((uint8_t)s[i])) return true;
    }
    return false;
}

static bool span_has_ascii_alpha(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (is_ascii_alpha((uint8_t)s[i])) return true;
    }
    return false;
}

static bool span_has_code_punct(const char *s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80u && !is_ascii_alnum(c) && c != '_') return true;
    }
    return false;
}

static void qt_push_verify_ex(StelogQueryTokens *qt, const char *ptr, size_t len, bool cs, bool whole_word) {
    if (!ptr || len == 0) return;
    for (uint32_t i = 0; i < qt->verify_count; ++i) {
        if (qt->verify_lens[i] == (uint32_t)len && memcmp(qt->verify_ptrs[i], ptr, len) == 0) {
            if (cs) qt->verify_cs[i] = 1;
            return;
        }
    }
    if (qt->verify_count < STELOG_MAX_VERIFY_PHRASES) {
        qt->verify_ptrs[qt->verify_count] = ptr;
        qt->verify_lens[qt->verify_count] = (uint32_t)len;
        qt->verify_cs[qt->verify_count]   = cs ? 1u : 0u;
        qt->verify_word[qt->verify_count] = whole_word ? 1u : 0u;
        qt->verify_count++;
    }
}

static inline void qt_push_verify(StelogQueryTokens *qt, const char *ptr, size_t len, bool cs) {
    qt_push_verify_ex(qt, ptr, len, cs, false);
}

static void qt_push_prefix(StelogQueryTokens *qt, const char *ptr, size_t len, bool cs) {
    if (!ptr || len < 2) return;
    for (uint32_t i = 0; i < qt->prefix_count; ++i) {
        if (qt->prefix_lens[i] == (uint32_t)len && memcmp(qt->prefix_ptrs[i], ptr, len) == 0) {
            if (cs) qt->prefix_cs[i] = 1;
            return;
        }
    }
    if (qt->prefix_count < STELOG_MAX_PREFIX_TERMS) {
        qt->prefix_ptrs[qt->prefix_count] = ptr;
        qt->prefix_lens[qt->prefix_count] = (uint32_t)len;
        qt->prefix_cs[qt->prefix_count]   = cs ? 1u : 0u;
        qt->prefix_count++;
    }
}

static void qt_push_highlight(StelogQueryTokens *qt, const char *ptr, size_t len, bool cs) {
    if (!ptr || len == 0) return;
    for (uint32_t i = 0; i < qt->highlight_count; ++i) {
        if (qt->highlight_lens[i] == (uint32_t)len && memcmp(qt->highlight_ptrs[i], ptr, len) == 0) {
            return;
        }
    }
    if (qt->highlight_count < STELOG_MAX_HIGHLIGHT_TERMS) {
        qt->highlight_ptrs[qt->highlight_count] = ptr;
        qt->highlight_lens[qt->highlight_count] = (uint32_t)len;
        qt->highlight_cs[qt->highlight_count]   = cs ? 1u : 0u;
        qt->highlight_count++;
    }
}

static void plan_push_neg_term(StelogQueryPlan *plan, uint64_t h) {
    for (uint32_t i = 0; i < plan->neg_term_count; ++i) {
        if (plan->neg_term_hashes[i] == h) return;
    }
    if (plan->neg_term_count < STELOG_MAX_NEG_TERMS) {
        plan->neg_term_hashes[plan->neg_term_count++] = h;
    }
}

static void plan_push_neg_verify(StelogQueryPlan *plan, const char *ptr, size_t len,
                                 bool cs, bool is_prefix, uint64_t guard_hash) {
    if (!ptr || len == 0) return;
    for (uint32_t i = 0; i < plan->neg_verify_count; ++i) {
        if (plan->neg_verify_lens[i] == (uint32_t)len &&
            plan->neg_verify_is_prefix[i] == (is_prefix ? 1u : 0u) &&
            memcmp(plan->neg_verify_ptrs[i], ptr, len) == 0) {
            return;
        }
    }
    if (plan->neg_verify_count < STELOG_MAX_NEG_PHRASES) {
        uint32_t idx = plan->neg_verify_count++;
        plan->neg_verify_ptrs[idx]       = ptr;
        plan->neg_verify_lens[idx]       = (uint32_t)len;
        plan->neg_verify_cs[idx]         = cs ? 1u : 0u;
        plan->neg_verify_is_prefix[idx]  = is_prefix ? 1u : 0u;
        plan->neg_verify_guard_hash[idx] = guard_hash;
    }
}

/* Emit query term hashes from a positive token or phrase (also pushes CJK >= 3 runs to verify) */
static void emit_query_span_terms(const char *s, size_t len, StelogQueryTokens *out) {
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < len) {
        uint8_t b = p[i];
        if (b < 0x80u) {
            if (is_ascii_alnum(b) || b == '_') {
                size_t start = i;
                while (i < len && p[i] < 0x80u && (is_ascii_alnum(p[i]) || p[i] == '_')) i++;
                size_t tok_len = i - start;
                const char *tok = s + start;
                while (tok_len > 0 && tok[0] == '_') { tok++; tok_len--; }
                while (tok_len > 0 && tok[tok_len - 1] == '_') { tok_len--; }
                if (tok_len >= 2) {
                    uint32_t dummy = 0;
                    emit_ascii_lower(tok, tok_len, query_emit_cb, out, &dummy);
                }
            } else {
                i++;
            }
        } else {
            size_t run_start = i;
            uint32_t cp = 0;
            size_t adv = utf8_decode(p + i, len - i, &cp);
            if (is_cjk_codepoint(cp)) {
                uint32_t prev_cp = cp;
                uint32_t run_len = 1;
                i += adv;
                while (i < len && p[i] >= 0x80u) {
                    uint32_t ncp = 0;
                    size_t nadv = utf8_decode(p + i, len - i, &ncp);
                    if (!is_cjk_codepoint(ncp)) break;
                    query_emit_cb(hash_cjk_bigram(prev_cp, ncp), out);
                    prev_cp = ncp;
                    run_len++;
                    i += nadv;
                }
                if (run_len == 1) {
                    query_emit_cb(hash_cjk_unigram(prev_cp), out);
                } else if (run_len >= 3) {
                    qt_push_verify(out, s + run_start, i - run_start, false);
                }
            } else {
                i += adv;
            }
        }
    }
}

/* Compute a single inverted-index guard hash for a negative phrase/token (longest ASCII word >= 2 or first CJK bigram).
   Returns 0 if no guaranteed indexed token exists inside the span. */
static uint64_t compute_guard_hash_for_span(const char *s, size_t len, bool strict_interior_ascii) {
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    uint64_t best_hash = 0;
    size_t best_ascii_len = 0;
    uint64_t first_cjk_hash = 0;

    while (i < len) {
        uint8_t b = p[i];
        if (b < 0x80u) {
            if (is_ascii_alnum(b) || b == '_') {
                size_t start = i;
                while (i < len && p[i] < 0x80u && (is_ascii_alnum(p[i]) || p[i] == '_')) i++;
                bool ok = !strict_interior_ascii || (start > 0 && i < len);
                if (ok) {
                    const char *tok = s + start;
                    size_t tok_len = i - start;
                    while (tok_len > 0 && tok[0] == '_') { tok++; tok_len--; }
                    while (tok_len > 0 && tok[tok_len - 1] == '_') { tok_len--; }
                    if (tok_len >= 2 && tok_len <= 64 && tok_len > best_ascii_len) {
                        char buf[64];
                        for (size_t k = 0; k < tok_len; ++k) buf[k] = (char)to_ascii_lower((uint8_t)tok[k]);
                        best_hash = stelog_wyhash(buf, tok_len, STELOG_TOKEN_HASH_SEED);
                        best_ascii_len = tok_len;
                    }
                }
            } else {
                i++;
            }
        } else {
            uint32_t cp = 0;
            size_t adv = utf8_decode(p + i, len - i, &cp);
            if (is_cjk_codepoint(cp)) {
                uint32_t prev_cp = cp;
                uint32_t run_len = 1;
                i += adv;
                while (i < len && p[i] >= 0x80u) {
                    uint32_t ncp = 0;
                    size_t nadv = utf8_decode(p + i, len - i, &ncp);
                    if (!is_cjk_codepoint(ncp)) break;
                    if (!first_cjk_hash) first_cjk_hash = hash_cjk_bigram(prev_cp, ncp);
                    prev_cp = ncp;
                    run_len++;
                    i += nadv;
                }
                if (run_len == 1 && !strict_interior_ascii && !first_cjk_hash) {
                    first_cjk_hash = hash_cjk_unigram(prev_cp);
                }
            } else {
                i += adv;
            }
        }
    }
    return best_hash ? best_hash : first_cjk_hash;
}

static bool try_consume_inline_modifier(const char *tok, size_t tlen, StelogQueryPlan *plan) {
    if (!plan || tlen < 3) return false;
    /* @<hex-or-dash> session shortcut (len >= 4 after '@', e.g. @8956a5cb) */
    if (tok[0] == '@' && tlen >= 5) {
        bool all_hex = true;
        for (size_t k = 1; k < tlen; ++k) {
            if (!is_ascii_hex_or_dash((uint8_t)tok[k])) { all_hex = false; break; }
        }
        if (all_hex) {
            plan->mod_session = (StelogStr){ tok + 1, (uint32_t)(tlen - 1) };
            plan->has_modifiers = true;
            return true;
        }
        return false;
    }

    const char *colon = (const char *)memchr(tok, ':', tlen);
    if (!colon || colon == tok || (size_t)(colon - tok) + 1 >= tlen) return false;
    size_t klen = (size_t)(colon - tok);
    const char *val = colon + 1;
    size_t vlen = tlen - klen - 1;
    if (vlen == 0) return false;

    /* Strip surrounding quotes on modifier value if any (e.g. project:"my repo") */
    if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
        val++;
        vlen -= 2;
        if (vlen == 0) return false;
    }

    if (klen == 5 && memcmp(tok, "agent", 5) == 0) {
        plan->mod_agent = (StelogStr){ val, (uint32_t)vlen };
    } else if (klen == 4 && memcmp(tok, "role", 4) == 0) {
        plan->mod_role = (StelogStr){ val, (uint32_t)vlen };
    } else if ((klen == 12 && memcmp(tok, "exclude_role", 12) == 0) ||
               (klen == 5 && memcmp(tok, "-role", 5) == 0)) {
        plan->mod_exclude_role = (StelogStr){ val, (uint32_t)vlen };
    } else if ((klen == 7 && memcmp(tok, "project", 7) == 0) ||
               (klen == 4 && memcmp(tok, "proj", 4) == 0)) {
        plan->mod_project = (StelogStr){ val, (uint32_t)vlen };
    } else if (klen == 5 && memcmp(tok, "actor", 5) == 0) {
        plan->mod_actor = (StelogStr){ val, (uint32_t)vlen };
    } else if (klen == 7 && memcmp(tok, "session", 7) == 0) {
        if (val[0] == '@' && vlen > 1) { val++; vlen--; }
        plan->mod_session = (StelogStr){ val, (uint32_t)vlen };
    } else if (klen == 5 && memcmp(tok, "since", 5) == 0) {
        plan->mod_since = (StelogStr){ val, (uint32_t)vlen };
    } else if (klen == 5 && memcmp(tok, "until", 5) == 0) {
        plan->mod_until = (StelogStr){ val, (uint32_t)vlen };
    } else {
        return false;
    }
    plan->has_modifiers = true;
    return true;
}

/* Check if a negative token can be represented as a single exact inverted-index term hash (0 text.bin touch) */
static bool try_single_exact_term_hash(const char *tok, size_t len, uint64_t *out_hash) {
    if (len == 0) return false;
    const uint8_t *p = (const uint8_t *)tok;
    if (p[0] < 0x80u) {
        /* Must be a pure single ASCII identifier [a-zA-Z0-9_]+ */
        for (size_t i = 0; i < len; ++i) {
            if (p[i] >= 0x80u || (!is_ascii_alnum(p[i]) && p[i] != '_')) return false;
        }
        const char *s = tok;
        size_t sl = len;
        while (sl > 0 && s[0] == '_') { s++; sl--; }
        while (sl > 0 && s[sl - 1] == '_') { sl--; }
        if (sl < 2 || sl > 64) return false;
        char buf[64];
        for (size_t i = 0; i < sl; ++i) buf[i] = (char)to_ascii_lower((uint8_t)s[i]);
        *out_hash = stelog_wyhash(buf, sl, STELOG_TOKEN_HASH_SEED);
        return true;
    }
    /* Pure 1-char or 2-char CJK token -> single unigram or bigram hash */
    uint32_t cps[3];
    uint32_t ncps = 0;
    size_t i = 0;
    while (i < len) {
        if (p[i] < 0x80u || ncps >= 3) return false;
        uint32_t cp = 0;
        size_t adv = utf8_decode(p + i, len - i, &cp);
        if (!is_cjk_codepoint(cp)) return false;
        cps[ncps++] = cp;
        i += adv;
    }
    if (ncps == 1) {
        *out_hash = hash_cjk_unigram(cps[0]);
        return true;
    }
    if (ncps == 2) {
        *out_hash = hash_cjk_bigram(cps[0], cps[1]);
        return true;
    }
    return false;
}

typedef struct {
    const char *ptr;
    size_t len;
    bool is_quoted;
    bool is_prefix;
} PendingNeg;

typedef struct {
    const char *ptr;
    size_t len;
} PendingWord;

static void parse_branch_tokens(const char *seg, size_t len,
                                StelogQueryPlan *plan,
                                StelogQueryTokens *bt,
                                bool *io_has_upper,
                                PendingNeg *negs, uint32_t *neg_cnt, uint32_t neg_cap,
                                PendingWord *alpha_words, uint32_t *aw_cnt, uint32_t aw_cap) {
    memset(bt, 0, sizeof(*bt));
    bt->raw_ptr = seg;
    bt->raw_len = (uint32_t)len;

    size_t i = 0;
    while (i < len) {
        while (i < len && (seg[i] == ' ' || seg[i] == '\t' || seg[i] == '\n' || seg[i] == '\r')) i++;
        if (i >= len) break;

        /* 1. Negated quoted phrase: -"..." */
        if (seg[i] == '-' && i + 1 < len && seg[i + 1] == '"') {
            size_t start = i + 2;
            const char *endq = (const char *)memchr(seg + start, '"', len - start);
            size_t end_idx = endq ? (size_t)(endq - seg) : len;
            size_t plen = end_idx - start;
            if (plen > 0 && plan && *neg_cnt < neg_cap) {
                if (span_has_ascii_upper(seg + start, plen)) *io_has_upper = true;
                negs[(*neg_cnt)++] = (PendingNeg){ seg + start, plen, true, false };
            }
            i = endq ? (end_idx + 1) : len;
            continue;
        }

        /* 2. Positive quoted phrase: "..." */
        if (seg[i] == '"') {
            size_t start = i + 1;
            const char *endq = (const char *)memchr(seg + start, '"', len - start);
            size_t end_idx = endq ? (size_t)(endq - seg) : len;
            size_t plen = end_idx - start;
            if (plen > 0) {
                if (span_has_ascii_upper(seg + start, plen)) *io_has_upper = true;
                qt_push_verify(bt, seg + start, plen, false);
                qt_push_highlight(bt, seg + start, plen, false);
                emit_query_span_terms(seg + start, plen, bt);
            }
            i = endq ? (end_idx + 1) : len;
            continue;
        }

        /* 3. Unquoted whitespace-delimited token (allow quoted value right after modifier colon, e.g. project:"my repo") */
        size_t tstart = i;
        bool in_q = false;
        while (i < len) {
            if (seg[i] == '"') in_q = !in_q;
            else if (!in_q && (seg[i] == ' ' || seg[i] == '\t' || seg[i] == '\n' || seg[i] == '\r')) break;
            i++;
        }
        const char *tok = seg + tstart;
        size_t tlen = i - tstart;
        if (tlen == 0) continue;

        /* 3a. Inline modifiers (agent:, role:, -role:, project:, actor:, session:, since:, until:, @hex) */
        if (plan && try_consume_inline_modifier(tok, tlen, plan)) {
            continue;
        }

        /* 3b. Negated unquoted token: -term or -prefix* */
        if (plan && tok[0] == '-' && tlen >= 2) {
            const char *nt = tok + 1;
            size_t nl = tlen - 1;
            bool is_pref = false;
            if (nl >= 3 && nt[nl - 1] == '*') {
                while (nl > 0 && nt[nl - 1] == '*') nl--;
                is_pref = (nl >= 2);
            }
            if (nl >= 1 && *neg_cnt < neg_cap) {
                if (span_has_ascii_upper(nt, nl)) *io_has_upper = true;
                negs[(*neg_cnt)++] = (PendingNeg){ nt, nl, false, is_pref };
            }
            continue;
        }

        /* 3c. Positive prefix wildcard: prefix* (len >= 2 before '*') */
        if (tok[tlen - 1] == '*') {
            size_t plen = tlen;
            while (plen > 0 && tok[plen - 1] == '*') plen--;
            if (plen >= 2) {
                if (span_has_ascii_upper(tok, plen)) *io_has_upper = true;
                qt_push_prefix(bt, tok, plen, false);
                qt_push_highlight(bt, tok, plen, false);
            }
            continue;
        }

        /* 3d. Normal positive token (word, CJK, or code/path punctuation token) */
        if (span_has_ascii_upper(tok, tlen)) *io_has_upper = true;
        emit_query_span_terms(tok, tlen, bt);
        if (span_has_code_punct(tok, tlen) && tlen >= 2) {
            qt_push_verify(bt, tok, tlen, false);
        }
        if (tlen >= 2 || (uint8_t)tok[0] >= 0x80u) {
            qt_push_highlight(bt, tok, tlen, false);
        }
        if (span_has_ascii_alpha(tok, tlen) && *aw_cnt < aw_cap) {
            alpha_words[(*aw_cnt)++] = (PendingWord){ tok, tlen };
        }
    }
}

void stelog_tokenize_query(const char *query, size_t len, StelogQueryTokens *out) {
    memset(out, 0, sizeof(*out));
    if (!query || len == 0) return;
    bool dummy_upper = false;
    PendingNeg negs[1];
    uint32_t neg_cnt = 0;
    PendingWord aw[1];
    uint32_t aw_cnt = 0;
    parse_branch_tokens(query, len, NULL, out, &dummy_upper, negs, &neg_cnt, 0, aw, &aw_cnt, 0);
}

void stelog_parse_query_plan_ex(const char *query,
                                size_t len,
                                StelogCaseMode case_mode,
                                bool fixed_strings,
                                StelogQueryPlan *out) {
    memset(out, 0, sizeof(*out));
    if (!query || len == 0) return;

    /* Fixed-strings (-F) mode: literal substring search with guaranteed-safe interior token recall */
    if (fixed_strings) {
        bool cs = (case_mode == STELOG_CASE_SENSITIVE) ||
                  (case_mode == STELOG_CASE_SMART && span_has_ascii_upper(query, len));
        out->effective_case_sensitive = cs;
        out->branch_count = 1;
        StelogQueryTokens *bt = &out->branches[0];
        bt->raw_ptr = query;
        bt->raw_len = (uint32_t)len;
        qt_push_verify(bt, query, len, cs);
        qt_push_highlight(bt, query, len, cs);

        /* Extract strictly interior ASCII tokens & CJK bigrams (run >= 2) for zero-false-negative SwissTable recall */
        const uint8_t *p = (const uint8_t *)query;
        size_t i = 0;
        while (i < len) {
            if (p[i] < 0x80u) {
                if (is_ascii_alnum(p[i]) || p[i] == '_') {
                    size_t start = i;
                    while (i < len && p[i] < 0x80u && (is_ascii_alnum(p[i]) || p[i] == '_')) i++;
                    if (start > 0 && i < len) {
                        const char *tok = query + start;
                        size_t tl = i - start;
                        while (tl > 0 && tok[0] == '_') { tok++; tl--; }
                        while (tl > 0 && tok[tl - 1] == '_') { tl--; }
                        if (tl >= 2 && tl <= 64) {
                            uint32_t dummy = 0;
                            emit_ascii_lower(tok, tl, query_emit_cb, bt, &dummy);
                        }
                    }
                } else {
                    i++;
                }
            } else {
                uint32_t cp = 0;
                size_t adv = utf8_decode(p + i, len - i, &cp);
                if (is_cjk_codepoint(cp)) {
                    uint32_t prev_cp = cp;
                    i += adv;
                    while (i < len && p[i] >= 0x80u) {
                        uint32_t ncp = 0;
                        size_t nadv = utf8_decode(p + i, len - i, &ncp);
                        if (!is_cjk_codepoint(ncp)) break;
                        query_emit_cb(hash_cjk_bigram(prev_cp, ncp), bt);
                        prev_cp = ncp;
                        i += nadv;
                    }
                } else {
                    i += adv;
                }
            }
        }
        return;
    }

    /* Normal syntax mode: split on top-level `|` or ` OR ` outside double quotes */
    bool has_query_upper = false;
    PendingNeg negs[STELOG_MAX_NEG_TERMS + STELOG_MAX_NEG_PHRASES];
    uint32_t neg_cnt = 0;
    PendingWord branch_aw[STELOG_MAX_QUERY_BRANCHES][STELOG_MAX_VERIFY_PHRASES];
    uint32_t branch_aw_cnt[STELOG_MAX_QUERY_BRANCHES];
    memset(branch_aw_cnt, 0, sizeof(branch_aw_cnt));

    bool in_quote = false;
    size_t seg_start = 0;
    for (size_t i = 0; i <= len; ++i) {
        bool is_split = false;
        size_t skip = 0;
        if (i == len) {
            is_split = true;
        } else if (query[i] == '"') {
            in_quote = !in_quote;
        } else if (!in_quote) {
            if (query[i] == '|') {
                is_split = true;
                skip = 1;
            } else if (i + 4 <= len && (query[i] == ' ' || query[i] == '\t') &&
                       query[i + 1] == 'O' && query[i + 2] == 'R' &&
                       (query[i + 3] == ' ' || query[i + 3] == '\t')) {
                is_split = true;
                skip = 4;
            }
        }
        if (is_split) {
            size_t s = seg_start, e = i;
            while (s < e && (query[s] == ' ' || query[s] == '\t')) s++;
            while (e > s && (query[e - 1] == ' ' || query[e - 1] == '\t')) e--;
            if (e > s && out->branch_count < STELOG_MAX_QUERY_BRANCHES) {
                uint32_t bidx = out->branch_count;
                StelogQueryTokens *bt = &out->branches[bidx];
                parse_branch_tokens(query + s, e - s, out, bt, &has_query_upper,
                                    negs, &neg_cnt, (uint32_t)(sizeof(negs) / sizeof(negs[0])),
                                    branch_aw[bidx], &branch_aw_cnt[bidx], STELOG_MAX_VERIFY_PHRASES);
                if (bt->term_count > 0 || bt->verify_count > 0 || bt->prefix_count > 0) {
                    out->branch_count++;
                }
            }
            if (skip > 0) i += skip - 1;
            seg_start = i + 1;
        }
    }

    bool cs = (case_mode == STELOG_CASE_SENSITIVE) ||
              (case_mode == STELOG_CASE_SMART && has_query_upper);
    out->effective_case_sensitive = cs;

    /* Propagate effective case sensitivity into branch verify/prefix/highlight arrays */
    for (uint32_t b = 0; b < out->branch_count; ++b) {
        StelogQueryTokens *bt = &out->branches[b];
        if (cs) {
            for (uint32_t v = 0; v < bt->verify_count; ++v) bt->verify_cs[v] = 1u;
            for (uint32_t p = 0; p < bt->prefix_count; ++p) bt->prefix_cs[p] = 1u;
            for (uint32_t h = 0; h < bt->highlight_count; ++h) bt->highlight_cs[h] = 1u;
            for (uint32_t w = 0; w < branch_aw_cnt[b]; ++w) {
                bool is_word = !span_has_code_punct(branch_aw[b][w].ptr, branch_aw[b][w].len);
                qt_push_verify_ex(bt, branch_aw[b][w].ptr, branch_aw[b][w].len, true, is_word);
            }
        }
    }

    /* Finalize query-wide negations: fast inverted-index neg_term_hashes when exact, else guarded neg_verify */
    for (uint32_t ni = 0; ni < neg_cnt; ++ni) {
        const PendingNeg *pn = &negs[ni];
        if (pn->is_prefix) {
            plan_push_neg_verify(out, pn->ptr, pn->len, cs, true, 0);
            continue;
        }
        bool need_case_verify = cs && span_has_ascii_alpha(pn->ptr, pn->len);
        uint64_t exact_h = 0;
        if (!pn->is_quoted && !need_case_verify && try_single_exact_term_hash(pn->ptr, pn->len, &exact_h)) {
            plan_push_neg_term(out, exact_h);
        } else {
            uint64_t guard = compute_guard_hash_for_span(pn->ptr, pn->len, pn->is_quoted);
            plan_push_neg_verify(out, pn->ptr, pn->len, cs, false, guard);
        }
    }
}
