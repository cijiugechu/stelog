#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_STATIC 1
#include <pcre2.h>

#include "query/regex_engine.h"
#include "base/hash.h"
#include "store/tokenizer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct StelogRegex {
    pcre2_code *code;
    bool has_jit;
    bool case_sensitive;
    StelogRegexPrefilter prefilter;
};

struct StelogRegexMatcher {
    pcre2_match_data *mdata;
    pcre2_match_context *mctx;
    pcre2_jit_stack *jit_stack;
};

static inline bool is_ascii_alnum_r(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static inline uint8_t to_lower_r(uint8_t c) {
    return (c >= 'A' && c <= 'Z') ? (uint8_t)(c + ('a' - 'A')) : c;
}

static inline size_t utf8_char_len(const uint8_t *p, size_t rem) {
    if (rem == 0) return 0;
    uint8_t b = p[0];
    if (b < 0x80u) return 1;
    if ((b & 0xE0u) == 0xC0u && rem >= 2) return 2;
    if ((b & 0xF0u) == 0xE0u && rem >= 3) return 3;
    if ((b & 0xF8u) == 0xF0u && rem >= 4) return 4;
    return 1;
}

static inline size_t utf8_decode_r(const uint8_t *p, size_t rem, uint32_t *out_cp) {
    uint8_t b0 = p[0];
    if (b0 < 0x80u) { *out_cp = b0; return 1; }
    if ((b0 & 0xE0u) == 0xC0u && rem >= 2) {
        *out_cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
        return 2;
    }
    if ((b0 & 0xF0u) == 0xE0u && rem >= 3) {
        *out_cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (uint32_t)(p[2] & 0x3Fu);
        return 3;
    }
    if ((b0 & 0xF8u) == 0xF0u && rem >= 4) {
        *out_cp = ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(p[1] & 0x3Fu) << 12) |
                  ((uint32_t)(p[2] & 0x3Fu) << 6) | (uint32_t)(p[3] & 0x3Fu);
        return 4;
    }
    *out_cp = b0;
    return 1;
}

static inline bool is_cjk_r(uint32_t cp) {
    return (cp >= 0x4E00u && cp <= 0x9FFFu) ||
           (cp >= 0x3400u && cp <= 0x4DBFu) ||
           (cp >= 0x3040u && cp <= 0x30FFu) ||
           (cp >= 0xAC00u && cp <= 0xD7AFu) ||
           (cp >= 0x20000u && cp <= 0x2A6DFu);
}

/* Check if regex contains an unescaped uppercase ASCII literal (ignoring \D, \W, \S, \B, \p{...}, \P{...}) */
static bool regex_has_unescaped_upper(const char *pat, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        uint8_t c = (uint8_t)pat[i];
        if (c == '\\' && i + 1 < len) {
            uint8_t next = (uint8_t)pat[i + 1];
            i++;
            if ((next == 'p' || next == 'P') && i + 1 < len && pat[i + 1] == '{') {
                i += 2;
                while (i < len && pat[i] != '}') i++;
            }
            continue;
        }
        if (c >= 'A' && c <= 'Z') return true;
    }
    return false;
}

/* Check if pattern contains inline case-insensitivity toggle (?i or (?...i...) */
static bool regex_has_inline_caseless(const char *pat, size_t len) {
    for (size_t i = 0; i + 2 < len; ++i) {
        if (pat[i] == '\\') { i++; continue; }
        if (pat[i] == '(' && pat[i + 1] == '?') {
            size_t j = i + 2;
            while (j < len && pat[j] != ')' && pat[j] != ':') {
                if (pat[j] == 'i') return true;
                if (pat[j] == '-') break;
                j++;
            }
        }
    }
    return false;
}

static bool is_escaped_literal_punct(uint8_t c) {
    return c >= 0x20u && c < 0x7Fu && !is_ascii_alnum_r(c);
}

static bool followed_by_optional_quantifier(const char *pat, size_t len, size_t next_pos) {
    if (next_pos >= len) return false;
    char q = pat[next_pos];
    if (q == '?' || q == '*') return true;
    if (q == '{') {
        size_t j = next_pos + 1;
        bool all_zero = false;
        while (j < len && pat[j] >= '0' && pat[j] <= '9') {
            all_zero = (pat[j] == '0') && (j == next_pos + 1 || all_zero);
            j++;
        }
        if (j > next_pos + 1 && all_zero && j < len && (pat[j] == ',' || pat[j] == '}')) {
            return true;
        }
    }
    return false;
}

static void pf_push_term(StelogRegexPrefilter *pf, uint64_t h) {
    for (uint32_t i = 0; i < pf->term_count; ++i) {
        if (pf->term_hashes[i] == h) return;
    }
    if (pf->term_count < STELOG_REGEX_MAX_PREFILTER_TERMS) {
        pf->term_hashes[pf->term_count++] = h;
    }
}

/* Extract strictly-interior ASCII tokens and CJK bigrams from a guaranteed required literal */
static void extract_prefilter_terms(StelogRegexPrefilter *pf) {
    if (pf->literal_len < 2) return;
    const uint8_t *p = (const uint8_t *)pf->literal;
    size_t len = pf->literal_len;
    size_t i = 0;

    while (i < len) {
        if (p[i] < 0x80u) {
            if (is_ascii_alnum_r(p[i]) || p[i] == '_') {
                size_t start = i;
                while (i < len && p[i] < 0x80u && (is_ascii_alnum_r(p[i]) || p[i] == '_')) i++;
                /* Strictly interior inside required literal => guaranteed full token in inverted index! */
                if (start > 0 && i < len) {
                    const char *tok = pf->literal + start;
                    size_t tl = i - start;
                    while (tl > 0 && tok[0] == '_') { tok++; tl--; }
                    while (tl > 0 && tok[tl - 1] == '_') { tl--; }
                    if (tl >= 2 && tl <= 64) {
                        char buf[64];
                        for (size_t k = 0; k < tl; ++k) buf[k] = (char)to_lower_r((uint8_t)tok[k]);
                        pf_push_term(pf, stelog_wyhash(buf, tl, STELOG_TOKEN_HASH_SEED));
                    }
                }
            } else {
                i++;
            }
        } else {
            uint32_t cp = 0;
            size_t adv = utf8_decode_r(p + i, len - i, &cp);
            if (is_cjk_r(cp)) {
                uint32_t prev_cp = cp;
                i += adv;
                while (i < len && p[i] >= 0x80u) {
                    uint32_t ncp = 0;
                    size_t nadv = utf8_decode_r(p + i, len - i, &ncp);
                    if (!is_cjk_r(ncp)) break;
                    uint32_t pair[2] = { prev_cp, ncp };
                    pf_push_term(pf, stelog_wyhash(pair, sizeof(pair), STELOG_TOKEN_HASH_SEED ^ 0xC1C1C1C1ull));
                    prev_cp = ncp;
                    i += nadv;
                }
            } else {
                i += adv;
            }
        }
    }
}

/* Conservative, zero-false-negative required literal extractor (ripgrep-style) */
static void extract_required_literal(const char *pat, size_t len, bool case_sensitive, StelogRegexPrefilter *pf) {
    memset(pf, 0, sizeof(*pf));
    if (!pat || len == 0) return;

    /* Pass 1: Reject if there is any top-level alternation `|` outside (...) and [...] */
    int paren_depth = 0;
    bool in_class = false;
    for (size_t i = 0; i < len; ++i) {
        char c = pat[i];
        if (c == '\\' && i + 1 < len) { i++; continue; }
        if (in_class) {
            if (c == ']') in_class = false;
            continue;
        }
        if (c == '[') {
            in_class = true;
            if (i + 1 < len && pat[i + 1] == '^') i++;
            if (i + 1 < len && pat[i + 1] == ']') i++;
        } else if (c == '(') {
            paren_depth++;
        } else if (c == ')') {
            if (paren_depth > 0) paren_depth--;
        } else if (c == '|' && paren_depth == 0) {
            return; /* Top-level alternation -> no single required literal */
        }
    }

    /* Pass 2: Extract longest contiguous required literal run at top level */
    char cur[STELOG_REGEX_MAX_PREFILTER_LIT + 1];
    size_t cur_len = 0;
    char best[STELOG_REGEX_MAX_PREFILTER_LIT + 1];
    size_t best_len = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t c = (uint8_t)pat[i];
        if (c == '[') {
            if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
            cur_len = 0;
            i++;
            if (i < len && pat[i] == '^') i++;
            if (i < len && pat[i] == ']') i++;
            while (i < len && pat[i] != ']') {
                if (pat[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len) i++;
            continue;
        }
        if (c == '(') {
            if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
            cur_len = 0;
            int depth = 1;
            i++;
            bool ic = false;
            while (i < len && depth > 0) {
                if (pat[i] == '\\' && i + 1 < len) { i += 2; continue; }
                if (ic) { if (pat[i] == ']') ic = false; i++; continue; }
                if (pat[i] == '[') {
                    ic = true;
                    i++;
                    if (i < len && pat[i] == '^') i++;
                    if (i < len && pat[i] == ']') i++;
                    continue;
                }
                if (pat[i] == '(') depth++;
                else if (pat[i] == ')') depth--;
                i++;
            }
            continue;
        }
        if (c == '.' || c == '^' || c == '$' || c == '*' || c == '+' || c == '?') {
            if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
            cur_len = 0;
            i++;
            continue;
        }
        if (c == '{') {
            if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
            cur_len = 0;
            while (i < len && pat[i] != '}') i++;
            if (i < len) i++;
            continue;
        }
        if (c == '\\' && i + 1 < len) {
            uint8_t nx = (uint8_t)pat[i + 1];
            if (is_escaped_literal_punct(nx)) {
                size_t after = i + 2;
                if (followed_by_optional_quantifier(pat, len, after)) {
                    if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
                    cur_len = 0;
                } else if (cur_len < STELOG_REGEX_MAX_PREFILTER_LIT) {
                    cur[cur_len++] = (char)nx;
                }
                i = after;
                continue;
            }
            /* Meta escape (\d, \w, \s, \b, \p{...}, \xHH, etc.) -> flush */
            if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
            cur_len = 0;
            i += 2;
            if ((nx == 'p' || nx == 'P') && i < len && pat[i] == '{') {
                while (i < len && pat[i] != '}') i++;
                if (i < len) i++;
            }
            continue;
        }

        /* Normal UTF-8 literal atom */
        size_t clen = utf8_char_len((const uint8_t *)pat + i, len - i);
        size_t after = i + clen;
        if (followed_by_optional_quantifier(pat, len, after)) {
            if (cur_len > best_len) { memcpy(best, cur, cur_len); best_len = cur_len; }
            cur_len = 0;
        } else if (cur_len + clen <= STELOG_REGEX_MAX_PREFILTER_LIT) {
            memcpy(cur + cur_len, pat + i, clen);
            cur_len += clen;
        }
        i = after;
    }

    if (cur_len > best_len) {
        memcpy(best, cur, cur_len);
        best_len = cur_len;
    }

    if (best_len >= 2) {
        memcpy(pf->literal, best, best_len);
        pf->literal[best_len] = '\0';
        pf->literal_len = (uint32_t)best_len;
        pf->literal_cs = case_sensitive && !regex_has_inline_caseless(pat, len);
        extract_prefilter_terms(pf);
    }
}

StelogRegex *stelog_regex_compile(const char *pattern,
                                  size_t pattern_len,
                                  StelogCaseMode case_mode,
                                  char *err_buf,
                                  size_t err_cap) {
    if (!pattern || pattern_len == 0) {
        if (err_buf && err_cap > 0) snprintf(err_buf, err_cap, "empty regex pattern");
        return NULL;
    }

    bool cs = (case_mode == STELOG_CASE_SENSITIVE) ||
              (case_mode == STELOG_CASE_SMART && regex_has_unescaped_upper(pattern, pattern_len));

    uint32_t opts = PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE;
    if (!cs) opts |= PCRE2_CASELESS;

    int errcode = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, (PCRE2_SIZE)pattern_len, opts,
                                     &errcode, &erroff, NULL);
    if (!code) {
        if (err_buf && err_cap > 0) {
            PCRE2_UCHAR msg[256];
            pcre2_get_error_message(errcode, msg, sizeof(msg));
            snprintf(err_buf, err_cap, "regex error at offset %zu: %s", (size_t)erroff, (const char *)msg);
        }
        return NULL;
    }

    bool has_jit = (pcre2_jit_compile(code, PCRE2_JIT_COMPLETE) == 0);

    StelogRegex *re = (StelogRegex *)calloc(1, sizeof(StelogRegex));
    if (!re) {
        pcre2_code_free(code);
        return NULL;
    }
    re->code = code;
    re->has_jit = has_jit;
    re->case_sensitive = cs;
    extract_required_literal(pattern, pattern_len, cs, &re->prefilter);
    return re;
}

void stelog_regex_destroy(StelogRegex *re) {
    if (!re) return;
    if (re->code) pcre2_code_free(re->code);
    free(re);
}

bool stelog_regex_is_jit(const StelogRegex *re) {
    return re ? re->has_jit : false;
}

const StelogRegexPrefilter *stelog_regex_prefilter(const StelogRegex *re) {
    return re ? &re->prefilter : NULL;
}

StelogRegexMatcher *stelog_regex_matcher_create(const StelogRegex *re) {
    if (!re || !re->code) return NULL;
    StelogRegexMatcher *m = (StelogRegexMatcher *)calloc(1, sizeof(StelogRegexMatcher));
    if (!m) return NULL;
    m->mdata = pcre2_match_data_create_from_pattern(re->code, NULL);
    m->mctx  = pcre2_match_context_create(NULL);
    if (!m->mdata || !m->mctx) {
        stelog_regex_matcher_destroy(m);
        return NULL;
    }
    /* ReDoS guard: bound backtracking steps and recursion depth */
    pcre2_set_match_limit(m->mctx, 100000u);
    pcre2_set_depth_limit(m->mctx, 1000u);
    if (re->has_jit) {
        m->jit_stack = pcre2_jit_stack_create(32u * 1024u, 1024u * 1024u, NULL);
        if (m->jit_stack) {
            pcre2_jit_stack_assign(m->mctx, NULL, m->jit_stack);
        }
    }
    return m;
}

void stelog_regex_matcher_destroy(StelogRegexMatcher *m) {
    if (!m) return;
    if (m->mdata) pcre2_match_data_free(m->mdata);
    if (m->mctx)  pcre2_match_context_free(m->mctx);
    if (m->jit_stack) pcre2_jit_stack_free(m->jit_stack);
    free(m);
}

bool stelog_regex_find(const StelogRegex *re,
                       StelogRegexMatcher *m,
                       const char *text,
                       size_t text_len,
                       size_t start_off,
                       size_t *out_start,
                       size_t *out_end) {
    if (!re || !m || !text || start_off > text_len) return false;
    int rc;
    if (re->has_jit) {
        rc = pcre2_jit_match(re->code, (PCRE2_SPTR)text, (PCRE2_SIZE)text_len,
                             (PCRE2_SIZE)start_off, PCRE2_NO_UTF_CHECK, m->mdata, m->mctx);
        if (rc == PCRE2_ERROR_JIT_STACKLIMIT) {
            rc = pcre2_match(re->code, (PCRE2_SPTR)text, (PCRE2_SIZE)text_len,
                             (PCRE2_SIZE)start_off, PCRE2_NO_UTF_CHECK, m->mdata, m->mctx);
        }
    } else {
        rc = pcre2_match(re->code, (PCRE2_SPTR)text, (PCRE2_SIZE)text_len,
                         (PCRE2_SIZE)start_off, PCRE2_NO_UTF_CHECK, m->mdata, m->mctx);
    }
    if (rc < 0) return false;
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(m->mdata);
    if (out_start) *out_start = (size_t)ov[0];
    if (out_end)   *out_end   = (size_t)ov[1];
    return true;
}
