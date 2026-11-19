#define _POSIX_C_SOURCE 200809L
#include "stelog.h"
#include "base/arena.h"
#include "base/hash.h"
#include "base/simd.h"
#include "base/thread_pool.h"
#include "base/time_iso8601.h"
#include "store/format.h"
#include "store/roaring.h"
#include "store/segment_reader.h"
#include "store/swisstable.h"
#include "store/tokenizer.h"
#include "query/regex_engine.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Helpers: UTF-8 Safe Snippet Window & Inline Modifier Parsers
 * ============================================================================ */

static inline bool utf8_is_cont(uint8_t b) {
    return (b & 0xC0u) == 0x80u;
}

static size_t utf8_floor(const char *s, size_t pos) {
    while (pos > 0 && utf8_is_cont((uint8_t)s[pos])) pos--;
    return pos;
}

static size_t utf8_ceil(const char *s, size_t len, size_t pos) {
    while (pos < len && utf8_is_cont((uint8_t)s[pos])) pos++;
    return pos;
}

static inline bool is_dialogue_role(StelogRoleKind r) {
    return r == STELOG_ROLE_USER || r == STELOG_ROLE_ASSISTANT || r == STELOG_ROLE_INTER_AGENT;
}

static uint32_t parse_mod_agent_csv(StelogStr s) {
    if (!s.ptr || s.len == 0) return 0;
    uint32_t mask = 0;
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        const char *c = (const char *)memchr(p, ',', (size_t)(end - p));
        size_t l = c ? (size_t)(c - p) : (size_t)(end - p);
        if (l == 5 && memcmp(p, "codex", 5) == 0) mask |= (1u << STELOG_AGENT_CODEX);
        else if (l == 3 && memcmp(p, "agy", 3) == 0) mask |= (1u << STELOG_AGENT_AGY);
        else if (l == 2 && memcmp(p, "pi", 2) == 0) mask |= (1u << STELOG_AGENT_PI);
        else if (l == 5 && memcmp(p, "droid", 5) == 0) mask |= (1u << STELOG_AGENT_DROID);
        if (!c) break;
        p = c + 1;
    }
    return mask;
}

static uint32_t parse_mod_role_csv(StelogStr s) {
    if (!s.ptr || s.len == 0) return 0;
    uint32_t mask = 0;
    const char *p = s.ptr;
    const char *end = s.ptr + s.len;
    while (p < end) {
        const char *c = (const char *)memchr(p, ',', (size_t)(end - p));
        size_t l = c ? (size_t)(c - p) : (size_t)(end - p);
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

/* Returns false when a non-empty inline since:/until: value cannot be parsed. */
static bool parse_mod_time_ms(StelogStr s, bool end_of_range, int64_t *out_ms) {
    *out_ms = 0;
    if (!s.ptr || s.len == 0) return true;
    return stelog_parse_time_arg(s.ptr, s.len, end_of_range, out_ms);
}

/* Extract a UTF-8 safe snippet: centers around any matched branch keyword or regex span when found deep in text;
   otherwise uses Middle Truncation (30% head / 70% tail for assistant replies so ending conclusions survive!). */
static StelogStr build_snippet(StelogArena *arena,
                               const char *text,
                               uint32_t text_len,
                               StelogRoleKind role,
                               const StelogQueryPlan *plan,
                               const StelogRegex *re,
                               StelogRegexMatcher *re_matcher,
                               uint32_t max_bytes,
                               bool *out_truncated) {
    if (out_truncated) *out_truncated = false;
    if (!text || text_len == 0) return (StelogStr){ "", 0 };
    if (text_len <= max_bytes) return (StelogStr){ text, text_len };

    const char *hit = NULL;
    if (re && re_matcher) {
        size_t ms = 0, me = 0;
        if (stelog_regex_find(re, re_matcher, text, text_len, 0, &ms, &me)) {
            hit = text + ms;
        }
    } else if (plan && plan->branch_count > 0) {
        for (uint32_t b = 0; b < plan->branch_count && !hit; ++b) {
            const StelogQueryTokens *bt = &plan->branches[b];
            for (uint32_t h = 0; h < bt->highlight_count && !hit; ++h) {
                if (bt->highlight_lens[h] >= 2) {
                    hit = stelog_find_substr(text, text_len, bt->highlight_ptrs[h],
                                             bt->highlight_lens[h], bt->highlight_cs[h] != 0);
                }
            }
        }
    }

    uint32_t head_pct = (role == STELOG_ROLE_ASSISTANT) ? 30u : 50u;
    if (!hit || (size_t)(hit - text) < max_bytes / 3) {
        return stelog_truncate_middle_weighted(arena, text, text_len, max_bytes, head_pct, out_truncated);
    }

    if (out_truncated) *out_truncated = true;
    size_t match_off = (size_t)(hit - text);
    size_t half = max_bytes > 32 ? (max_bytes - 16) / 2 : max_bytes / 2;
    size_t win_start = (match_off > half) ? (match_off - half) : 0;
    size_t win_end = win_start + (max_bytes > 16 ? max_bytes - 12 : max_bytes);
    if (win_end > text_len) win_end = text_len;

    win_start = utf8_ceil(text, text_len, win_start);
    win_end = utf8_floor(text, win_end);
    if (win_end < win_start) win_end = win_start;

    bool lead = win_start > 0;
    bool trail = win_end < text_len;
    size_t slice_len = win_end - win_start;
    size_t total = (lead ? 3u : 0u) + slice_len + (trail ? 3u : 0u);

    char *buf = (char *)stelog_arena_alloc_aligned(arena, total + 1, 1);
    if (!buf) return (StelogStr){ "", 0 };
    size_t pos = 0;
    if (lead) { memcpy(buf + pos, "\xE2\x80\xA6", 3); pos += 3; }
    if (slice_len > 0) { memcpy(buf + pos, text + win_start, slice_len); pos += slice_len; }
    if (trail) { memcpy(buf + pos, "\xE2\x80\xA6", 3); pos += 3; }
    buf[pos] = '\0';
    return (StelogStr){ buf, (uint32_t)pos };
}

/* ============================================================================
 * Monotonic ts_ms Binary Search Range [lo_doc, hi_doc]
 * ============================================================================ */

static bool find_time_doc_range(const StelogSegment *seg,
                                int64_t since_ms,
                                int64_t until_ms,
                                uint32_t *out_lo,
                                uint32_t *out_hi) {
    if (!seg || !seg->loaded || seg->doc_count == 0) return false;
    uint32_t lo = 0;
    uint32_t hi = seg->doc_count - 1u;

    if (since_ms > 0) {
        uint32_t l = 0, r = seg->doc_count;
        while (l < r) {
            uint32_t mid = l + ((r - l) >> 1);
            if (seg->col_ts_ms[mid] < since_ms) l = mid + 1;
            else r = mid;
        }
        if (l >= seg->doc_count) return false;
        lo = l;
    }

    if (until_ms > 0) {
        uint32_t l = lo, r = seg->doc_count;
        while (l < r) {
            uint32_t mid = l + ((r - l) >> 1);
            if (seg->col_ts_ms[mid] <= until_ms) l = mid + 1;
            else r = mid;
        }
        if (l == 0) return false;
        hi = l - 1u;
    }

    if (lo > hi) return false;
    *out_lo = lo;
    *out_hi = hi;
    return true;
}

static inline float role_decision_weight(StelogRoleKind r) {
    switch (r) {
        case STELOG_ROLE_USER:        return 1.40f;
        case STELOG_ROLE_ASSISTANT:   return 1.20f;
        case STELOG_ROLE_INTER_AGENT: return 1.20f;
        case STELOG_ROLE_TOOL_CALL:   return 0.70f;
        case STELOG_ROLE_THINKING:    return 0.60f;
        case STELOG_ROLE_TOOL_RESULT: return 0.30f;
        default:                      return 0.20f;
    }
}

/* ============================================================================
 * Candidate Scoring & OR-Branch Segment Query Execution (0 malloc!)
 * ============================================================================ */

typedef struct {
    uint32_t local_doc;
    uint8_t  is_delta;
    uint32_t dup_count;
    int64_t  ts_ms;
    float    score;
} ScoredCand;

typedef struct {
    ScoredCand *items;
    uint32_t count;
    uint32_t cap;
} CandList;

static void cand_reserve(StelogArena *arena, CandList *cl, uint32_t need) {
    if (need <= cl->cap) return;
    uint32_t ncap = cl->cap ? cl->cap : 256;
    while (ncap < need) ncap *= 2;
    ScoredCand *ni = (ScoredCand *)stelog_arena_alloc_aligned(arena, ncap * sizeof(ScoredCand), 8);
    if (!ni) return;
    if (cl->count > 0) memcpy(ni, cl->items, cl->count * sizeof(ScoredCand));
    cl->items = ni;
    cl->cap = ncap;
}

static inline void cand_push(StelogArena *arena, CandList *cl, ScoredCand c) {
    if (cl->count == cl->cap) {
        cand_reserve(arena, cl, cl->cap ? cl->cap * 2 : 256);
        if (cl->count >= cl->cap) return;
    }
    cl->items[cl->count++] = c;
}

static bool text_has_word_prefix(const char *txt, uint32_t tlen,
                                 const char *pref, uint32_t plen, bool cs) {
    if (plen == 0 || plen > tlen) return false;
    const char *cur = txt;
    const char *end = txt + tlen;
    while (cur < end && (size_t)(end - cur) >= plen) {
        const char *hit = stelog_find_substr(cur, (size_t)(end - cur), pref, plen, cs);
        if (!hit) return false;
        if (stelog_is_word_prefix(txt, tlen, hit)) return true;
        cur = hit + 1;
    }
    return false;
}

static bool text_has_whole_word(const char *txt, uint32_t tlen,
                                const char *word, uint32_t wlen, bool cs) {
    if (wlen == 0 || wlen > tlen) return false;
    const char *cur = txt;
    const char *end = txt + tlen;
    while (cur < end && (size_t)(end - cur) >= wlen) {
        const char *hit = stelog_find_substr(cur, (size_t)(end - cur), word, wlen, cs);
        if (!hit) return false;
        if (stelog_is_word_prefix(txt, tlen, hit) && stelog_is_word_suffix(txt, tlen, hit, wlen)) {
            return true;
        }
        cur = hit + 1;
    }
    return false;
}

static bool doc_passes_branch_verify(const StelogSegment *seg, uint32_t d, const StelogQueryTokens *bt) {
    if (bt->verify_count == 0 && bt->prefix_count == 0) return true;
    const char *txt = seg->text_base + seg->col_text_off[d];
    uint32_t tlen = seg->col_text_len[d];
    for (uint32_t v = 0; v < bt->verify_count; ++v) {
        if (bt->verify_word[v]) {
            if (!text_has_whole_word(txt, tlen, bt->verify_ptrs[v], bt->verify_lens[v], bt->verify_cs[v] != 0)) {
                return false;
            }
        } else {
            if (!stelog_find_substr(txt, tlen, bt->verify_ptrs[v], bt->verify_lens[v], bt->verify_cs[v] != 0)) {
                return false;
            }
        }
    }
    for (uint32_t p = 0; p < bt->prefix_count; ++p) {
        if (!text_has_word_prefix(txt, tlen, bt->prefix_ptrs[p], bt->prefix_lens[p], bt->prefix_cs[p] != 0)) {
            return false;
        }
    }
    return true;
}

typedef struct {
    StelogDocSet neg_term_sets[STELOG_MAX_NEG_TERMS];
    uint32_t     neg_term_count;

    StelogDocSet guard_sets[STELOG_MAX_NEG_PHRASES];
    uint8_t      guard_state[STELOG_MAX_NEG_PHRASES]; /* 0 = no guard, 1 = check guard, 2 = impossible in seg (skip) */
    uint32_t     active_neg_verify_count;
} SegNegFilter;

static void seg_neg_filter_init(const StelogSegment *seg, const StelogQueryPlan *plan, SegNegFilter *nf) {
    memset(nf, 0, sizeof(*nf));
    if (!plan) return;
    for (uint32_t i = 0; i < plan->neg_term_count; ++i) {
        const StelogTermSlot *slot = stelog_swiss_find(&seg->terms, plan->neg_term_hashes[i]);
        if (slot && slot->doc_freq > 0) {
            nf->neg_term_sets[nf->neg_term_count++] = stelog_docset_from_term(slot, seg->postings_base);
        }
    }
    for (uint32_t i = 0; i < plan->neg_verify_count; ++i) {
        uint64_t gh = plan->neg_verify_guard_hash[i];
        if (gh != 0) {
            const StelogTermSlot *slot = stelog_swiss_find(&seg->terms, gh);
            if (!slot || slot->doc_freq == 0) {
                nf->guard_state[i] = 2u; /* Guard absent in segment -> negative phrase cannot occur */
            } else {
                nf->guard_sets[i] = stelog_docset_from_term(slot, seg->postings_base);
                nf->guard_state[i] = 1u;
                nf->active_neg_verify_count++;
            }
        } else {
            nf->guard_state[i] = 0u;
            nf->active_neg_verify_count++;
        }
    }
}

static inline bool doc_excluded_by_neg_terms(const SegNegFilter *nf, uint32_t d) {
    if (nf->neg_term_count == 0) return false;
    return stelog_docset_any_contains(nf->neg_term_sets, nf->neg_term_count, d);
}

static bool doc_excluded_by_neg_verify(const StelogSegment *seg, uint32_t d,
                                       const StelogQueryPlan *plan, const SegNegFilter *nf) {
    if (nf->active_neg_verify_count == 0) return false;
    const char *txt = NULL;
    uint32_t tlen = 0;
    for (uint32_t i = 0; i < plan->neg_verify_count; ++i) {
        if (nf->guard_state[i] == 2u) continue;
        if (nf->guard_state[i] == 1u && !stelog_docset_contains(&nf->guard_sets[i], d)) continue;
        if (!txt) {
            txt = seg->text_base + seg->col_text_off[d];
            tlen = seg->col_text_len[d];
        }
        if (plan->neg_verify_is_prefix[i]) {
            if (text_has_word_prefix(txt, tlen, plan->neg_verify_ptrs[i],
                                     plan->neg_verify_lens[i], plan->neg_verify_cs[i] != 0)) {
                return true;
            }
        } else {
            if (stelog_find_substr(txt, tlen, plan->neg_verify_ptrs[i],
                                   plan->neg_verify_lens[i], plan->neg_verify_cs[i] != 0) != NULL) {
                return true;
            }
        }
    }
    return false;
}

/* ============================================================================
 * DOD Contiguous text.bin Parallel SIMD Scan (for standalone prefix* / -F)
 * Zero-malloc (uses QueryScratchArena bitset), O(log N) col_text_off mapping.
 * ============================================================================ */

typedef struct {
    const StelogSegment *seg;
    uint32_t lo_doc;
    uint32_t hi_doc;
    const char *needle;
    size_t needle_len;
    bool case_sensitive;
    bool require_word_prefix;
    uint8_t *hit_bits;
    uint32_t worker_ranges[9]; /* 512-doc (64B cache-line) aligned split points */
} ContigTextScanCtx;

static void scan_doc_slice(const ContigTextScanCtx *C, uint32_t w_lo, uint32_t w_hi) {
    if (w_lo > w_hi) return;
    const StelogSegment *seg = C->seg;
    uint64_t b_start = seg->col_text_off[w_lo];
    uint64_t b_end   = seg->col_text_off[w_hi] + seg->col_text_len[w_hi];
    if (b_end <= b_start || b_end > seg->text_len) return;

    const char *base = seg->text_base;
    const char *cur  = base + b_start;
    const char *end  = base + b_end;
    uint32_t cur_lo  = w_lo;
    size_t nlen      = C->needle_len;

    while (cur < end && (size_t)(end - cur) >= nlen) {
        const char *hit = stelog_find_substr(cur, (size_t)(end - cur), C->needle, nlen, C->case_sensitive);
        if (!hit) break;

        uint64_t hit_off = (uint64_t)(hit - base);
        /* Upper-bound - 1: largest d in [cur_lo, w_hi] with col_text_off[d] <= hit_off */
        uint32_t l = cur_lo, r = w_hi + 1u;
        while (l < r) {
            uint32_t mid = l + ((r - l) >> 1);
            if (seg->col_text_off[mid] <= hit_off) l = mid + 1u;
            else r = mid;
        }
        uint32_t d = (l > cur_lo) ? (l - 1u) : cur_lo;
        uint64_t doc_start = seg->col_text_off[d];
        uint32_t doc_len   = seg->col_text_len[d];
        uint64_t doc_end   = doc_start + doc_len;
        cur_lo = d;

        /* Reject false match crossing doc d / doc d+1 boundary */
        if (hit_off + nlen > doc_end) {
            cur = base + doc_end;
            if (cur_lo < w_hi) cur_lo++;
            continue;
        }
        if (C->require_word_prefix && !stelog_is_word_prefix(base + doc_start, doc_len, hit)) {
            cur = hit + 1;
            continue;
        }

        C->hit_bits[d >> 3] |= (uint8_t)(1u << (d & 7u));
        cur = base + doc_end;
        if (cur_lo < w_hi) cur_lo++;
    }
}

static void contig_text_scan_worker(uint32_t wid, uint32_t nworkers, void *ud) {
    (void)nworkers;
    const ContigTextScanCtx *C = (const ContigTextScanCtx *)ud;
    uint32_t w_lo = C->worker_ranges[wid];
    uint32_t w_hi_excl = C->worker_ranges[wid + 1];
    if (w_lo < w_hi_excl) {
        scan_doc_slice(C, w_lo, w_hi_excl - 1u);
    }
}

static uint32_t scan_text_literal_simd(const StelogSegment *seg,
                                       uint32_t lo_doc,
                                       uint32_t hi_doc,
                                       const char *needle,
                                       size_t needle_len,
                                       bool cs,
                                       bool req_prefix,
                                       StelogArena *scratch,
                                       uint32_t **out_docs) {
    *out_docs = NULL;
    if (!seg || !seg->text_base || lo_doc > hi_doc || !needle || needle_len == 0) return 0;

    uint64_t b_start = seg->col_text_off[lo_doc];
    uint64_t b_end   = seg->col_text_off[hi_doc] + seg->col_text_len[hi_doc];
    if (b_end <= b_start || b_end > seg->text_len) return 0;
    uint64_t scan_bytes = b_end - b_start;

    uint32_t nwords = (seg->doc_count + 63u) / 64u;
    size_t bits_bytes = (size_t)nwords * 8u;
    uint8_t *hit_bits = (uint8_t *)stelog_arena_alloc_aligned(scratch, bits_bytes, 64);
    if (!hit_bits) return 0;
    memset(hit_bits, 0, bits_bytes);

    ContigTextScanCtx ctx;
    ctx.seg = seg;
    ctx.lo_doc = lo_doc;
    ctx.hi_doc = hi_doc;
    ctx.needle = needle;
    ctx.needle_len = needle_len;
    ctx.case_sensitive = cs;
    ctx.require_word_prefix = req_prefix;
    ctx.hit_bits = hit_bits;

    uint32_t total_docs = hi_doc - lo_doc + 1u;
    uint32_t nworkers = 1u;
    if (scan_bytes >= 2u * 1024u * 1024u && total_docs >= 2048u) {
        nworkers = stelog_detect_pcore_count();
        if (nworkers > 8u) nworkers = 8u;
        if (nworkers < 1u) nworkers = 1u;
    }

    if (nworkers == 1u) {
        scan_doc_slice(&ctx, lo_doc, hi_doc);
    } else {
        /* Split by byte-offset balance, snapping each boundary to a 512-doc (64B cache-line) multiple */
        ctx.worker_ranges[0] = lo_doc;
        ctx.worker_ranges[nworkers] = hi_doc + 1u;
        for (uint32_t w = 1; w < nworkers; ++w) {
            uint64_t target_off = b_start + (scan_bytes * w) / nworkers;
            uint32_t l = lo_doc, r = hi_doc + 1u;
            while (l < r) {
                uint32_t mid = l + ((r - l) >> 1);
                if (seg->col_text_off[mid] < target_off) l = mid + 1u;
                else r = mid;
            }
            uint32_t snapped = (l + 255u) & ~511u;
            if (snapped < ctx.worker_ranges[w - 1]) snapped = ctx.worker_ranges[w - 1];
            if (snapped > hi_doc + 1u) snapped = hi_doc + 1u;
            ctx.worker_ranges[w] = snapped;
        }
        stelog_parallel_run(nworkers, contig_text_scan_worker, &ctx);
    }

    /* Count & materialize sorted doc_ids from hit_bits via 64-bit ctzll */
    const uint64_t *wbits = (const uint64_t *)(const void *)hit_bits;
    uint32_t w_start = lo_doc >> 6;
    uint32_t w_end   = hi_doc >> 6;
    uint32_t count = 0;
    for (uint32_t wi = w_start; wi <= w_end; ++wi) {
        count += (uint32_t)__builtin_popcountll(wbits[wi]);
    }
    if (count == 0) return 0;

    uint32_t *docs = (uint32_t *)stelog_arena_alloc_aligned(scratch, count * sizeof(uint32_t), 8);
    if (!docs) return 0;
    uint32_t out_n = 0;
    for (uint32_t wi = w_start; wi <= w_end; ++wi) {
        uint64_t word = wbits[wi];
        uint32_t base_d = wi << 6;
        while (word) {
            uint32_t bit = (uint32_t)__builtin_ctzll(word);
            word &= word - 1ull;
            uint32_t d = base_d + bit;
            if (d >= lo_doc && d <= hi_doc) docs[out_n++] = d;
        }
    }
    *out_docs = docs;
    return out_n;
}

static uint32_t scan_text_contiguous_simd(const StelogSegment *seg,
                                          uint32_t lo_doc,
                                          uint32_t hi_doc,
                                          const StelogQueryTokens *bt,
                                          StelogArena *scratch,
                                          uint32_t **out_docs) {
    *out_docs = NULL;
    if (!seg || !seg->text_base || lo_doc > hi_doc) return 0;

    /* Pick longest verify or prefix needle for maximum 2-byte SIMD selectivity */
    const char *needle = NULL;
    size_t needle_len = 0;
    bool cs = false;
    bool req_prefix = false;
    for (uint32_t v = 0; v < bt->verify_count; ++v) {
        if (bt->verify_lens[v] > needle_len) {
            needle = bt->verify_ptrs[v];
            needle_len = bt->verify_lens[v];
            cs = (bt->verify_cs[v] != 0);
            req_prefix = false;
        }
    }
    if (!needle) {
        for (uint32_t p = 0; p < bt->prefix_count; ++p) {
            if (bt->prefix_lens[p] > needle_len) {
                needle = bt->prefix_ptrs[p];
                needle_len = bt->prefix_lens[p];
                cs = (bt->prefix_cs[p] != 0);
                req_prefix = true;
            }
        }
    }
    return scan_text_literal_simd(seg, lo_doc, hi_doc, needle, needle_len, cs, req_prefix, scratch, out_docs);
}

/* ============================================================================
 * Tier 3: P-Core Parallel PCRE2 JIT Per-Doc Scan (Filter-Before-Match, 0 cross-doc leak!)
 * ============================================================================ */

typedef struct {
    const StelogIndex   *idx;
    const StelogSegment *seg;
    bool                 is_delta;
    const StelogRegex   *re;
    bool                 no_subagents;
    uint32_t             agent_mask;
    uint32_t             role_mask;
    const uint8_t       *sess_ok;
    const uint8_t       *proj_ok;
    const uint8_t       *actor_ok;
    uint8_t             *hit_bits;
    uint32_t             worker_ranges[9];
} RegexScanCtx;

static void regex_scan_doc_slice(const RegexScanCtx *C, uint32_t w_lo, uint32_t w_hi, StelogRegexMatcher *m) {
    const StelogSegment *seg = C->seg;
    for (uint32_t d = w_lo; d <= w_hi; ++d) {
        if (!C->is_delta && stelog_index_base_file_tombstoned(C->idx, seg->col_file_id[d])) continue;
        StelogTag tag = seg->col_tag[d];
        if (C->no_subagents && (stelog_tag_flags(tag) & STELOG_FLAG_IS_SUBAGENT)) continue;
        if (C->agent_mask && !(C->agent_mask & (1u << (uint32_t)stelog_tag_agent(tag)))) continue;
        if (C->role_mask && !(C->role_mask & (1u << (uint32_t)stelog_tag_role(tag)))) continue;
        if (C->sess_ok && !(C->sess_ok[seg->col_session_id[d] >> 3] & (1u << (seg->col_session_id[d] & 7u)))) continue;
        if (C->proj_ok && !(C->proj_ok[seg->col_project_id[d] >> 3] & (1u << (seg->col_project_id[d] & 7u)))) continue;
        if (C->actor_ok && !(C->actor_ok[seg->col_actor_id[d] >> 3] & (1u << (seg->col_actor_id[d] & 7u)))) continue;

        uint32_t tlen = seg->col_text_len[d];
        if (tlen == 0) continue;
        if (stelog_regex_find(C->re, m, seg->text_base + seg->col_text_off[d], tlen, 0, NULL, NULL)) {
            C->hit_bits[d >> 3] |= (uint8_t)(1u << (d & 7u));
        }
    }
}

static void regex_scan_worker(uint32_t wid, uint32_t nworkers, void *ud) {
    (void)nworkers;
    const RegexScanCtx *C = (const RegexScanCtx *)ud;
    uint32_t w_lo = C->worker_ranges[wid];
    uint32_t w_hi_excl = C->worker_ranges[wid + 1];
    if (w_lo >= w_hi_excl) return;
    StelogRegexMatcher *m = stelog_regex_matcher_create(C->re);
    if (!m) return;
    regex_scan_doc_slice(C, w_lo, w_hi_excl - 1u, m);
    stelog_regex_matcher_destroy(m);
}

static uint32_t scan_text_regex_parallel(const StelogIndex *idx,
                                         const StelogSegment *seg,
                                         bool is_delta,
                                         uint32_t lo_doc,
                                         uint32_t hi_doc,
                                         const StelogRegex *re,
                                         StelogRegexMatcher *single_matcher,
                                         bool no_subagents,
                                         uint32_t agent_mask,
                                         uint32_t role_mask,
                                         const uint8_t *sess_ok,
                                         const uint8_t *proj_ok,
                                         const uint8_t *actor_ok,
                                         StelogArena *scratch,
                                         uint32_t **out_docs) {
    *out_docs = NULL;
    if (!seg || !seg->text_base || !re || lo_doc > hi_doc) return 0;

    uint64_t b_start = seg->col_text_off[lo_doc];
    uint64_t b_end   = seg->col_text_off[hi_doc] + seg->col_text_len[hi_doc];
    if (b_end <= b_start || b_end > seg->text_len) return 0;
    uint64_t scan_bytes = b_end - b_start;

    uint32_t nwords = (seg->doc_count + 63u) / 64u;
    size_t bits_bytes = (size_t)nwords * 8u;
    uint8_t *hit_bits = (uint8_t *)stelog_arena_alloc_aligned(scratch, bits_bytes, 64);
    if (!hit_bits) return 0;
    memset(hit_bits, 0, bits_bytes);

    RegexScanCtx ctx;
    ctx.idx = idx;
    ctx.seg = seg;
    ctx.is_delta = is_delta;
    ctx.re = re;
    ctx.no_subagents = no_subagents;
    ctx.agent_mask = agent_mask;
    ctx.role_mask = role_mask;
    ctx.sess_ok = sess_ok;
    ctx.proj_ok = proj_ok;
    ctx.actor_ok = actor_ok;
    ctx.hit_bits = hit_bits;

    uint32_t total_docs = hi_doc - lo_doc + 1u;
    uint32_t nworkers = 1u;
    if (scan_bytes >= 512u * 1024u && total_docs >= 512u) {
        nworkers = stelog_detect_pcore_count();
        if (nworkers > 8u) nworkers = 8u;
        if (nworkers < 1u) nworkers = 1u;
    }

    if (nworkers == 1u) {
        regex_scan_doc_slice(&ctx, lo_doc, hi_doc, single_matcher);
    } else {
        ctx.worker_ranges[0] = lo_doc;
        ctx.worker_ranges[nworkers] = hi_doc + 1u;
        for (uint32_t w = 1; w < nworkers; ++w) {
            uint64_t target_off = b_start + (scan_bytes * w) / nworkers;
            uint32_t l = lo_doc, r = hi_doc + 1u;
            while (l < r) {
                uint32_t mid = l + ((r - l) >> 1);
                if (seg->col_text_off[mid] < target_off) l = mid + 1u;
                else r = mid;
            }
            uint32_t snapped = (l + 255u) & ~511u;
            if (snapped < ctx.worker_ranges[w - 1]) snapped = ctx.worker_ranges[w - 1];
            if (snapped > hi_doc + 1u) snapped = hi_doc + 1u;
            ctx.worker_ranges[w] = snapped;
        }
        stelog_parallel_run(nworkers, regex_scan_worker, &ctx);
    }

    const uint64_t *wbits = (const uint64_t *)(const void *)hit_bits;
    uint32_t w_start = lo_doc >> 6;
    uint32_t w_end   = hi_doc >> 6;
    uint32_t count = 0;
    for (uint32_t wi = w_start; wi <= w_end; ++wi) {
        count += (uint32_t)__builtin_popcountll(wbits[wi]);
    }
    if (count == 0) return 0;

    uint32_t *docs = (uint32_t *)stelog_arena_alloc_aligned(scratch, count * sizeof(uint32_t), 8);
    if (!docs) return 0;
    uint32_t out_n = 0;
    for (uint32_t wi = w_start; wi <= w_end; ++wi) {
        uint64_t word = wbits[wi];
        uint32_t base_d = wi << 6;
        while (word) {
            uint32_t bit = (uint32_t)__builtin_ctzll(word);
            word &= word - 1ull;
            uint32_t d = base_d + bit;
            if (d >= lo_doc && d <= hi_doc) docs[out_n++] = d;
        }
    }
    *out_docs = docs;
    return out_n;
}

typedef struct {
    uint32_t doc_id;
    float idf;
} BranchHit;

static int cmp_branch_hit(const void *a, const void *b) {
    uint32_t da = ((const BranchHit *)a)->doc_id;
    uint32_t db = ((const BranchHit *)b)->doc_id;
    return (da < db) ? -1 : (da > db ? 1 : 0);
}

static void query_one_segment(const StelogIndex *idx,
                              const StelogSegment *seg,
                              bool is_delta,
                              const StelogQuery *q,
                              const StelogQueryPlan *plan,
                              const StelogRegex *re,
                              StelogRegexMatcher *re_matcher,
                              uint32_t agent_mask,
                              uint32_t role_mask,
                              const char *proj_substr,
                              const char *actor_substr,
                              const char *session_id,
                              int64_t since_ms,
                              int64_t until_ms,
                              int64_t max_ts_ms,
                              StelogArena *scratch,
                              CandList *out_cands,
                              uint32_t *out_docs_in_scope) {
    if (!seg || !seg->loaded || seg->doc_count == 0) return;

    uint32_t lo_doc = 0, hi_doc = 0;
    if (!find_time_doc_range(seg, since_ms, until_ms, &lo_doc, &hi_doc)) return;
    if (out_docs_in_scope) *out_docs_in_scope += (hi_doc - lo_doc + 1u);

    /* Dictionary filters evaluated once in O(dict_size) */
    uint8_t *proj_ok = NULL;
    uint32_t single_proj_id = UINT32_MAX;
    uint32_t proj_match_cnt = 0;
    if (proj_substr && proj_substr[0]) {
        size_t plen = strlen(proj_substr);
        proj_ok = (uint8_t *)stelog_arena_alloc(scratch, (seg->project_count + 7u) / 8u + 1u);
        memset(proj_ok, 0, (seg->project_count + 7u) / 8u + 1u);
        for (uint32_t p = 0; p < seg->project_count; ++p) {
            StelogStr ps = stelog_seg_str(seg, seg->projects[p]);
            if (ps.len > 0 && stelog_memcasemem(ps.ptr, ps.len, proj_substr, plen)) {
                proj_ok[p >> 3] |= (uint8_t)(1u << (p & 7u));
                single_proj_id = p;
                proj_match_cnt++;
            }
        }
        if (proj_match_cnt == 0) return;
    }

    uint8_t *actor_ok = NULL;
    if (actor_substr && actor_substr[0]) {
        size_t alen = strlen(actor_substr);
        actor_ok = (uint8_t *)stelog_arena_alloc(scratch, (seg->actor_count + 7u) / 8u + 1u);
        memset(actor_ok, 0, (seg->actor_count + 7u) / 8u + 1u);
        uint32_t acnt = 0;
        for (uint32_t a = 0; a < seg->actor_count; ++a) {
            StelogStr as = stelog_seg_str(seg, seg->actors[a]);
            if (as.len > 0 && stelog_memcasemem(as.ptr, as.len, actor_substr, alen)) {
                actor_ok[a >> 3] |= (uint8_t)(1u << (a & 7u));
                acnt++;
            }
        }
        if (acnt == 0) return;
    }

    uint8_t *sess_ok = NULL;
    uint32_t single_sid = UINT32_MAX;
    uint32_t sess_match_cnt = 0;
    if (session_id && session_id[0]) {
        const char *sp = session_id;
        if (sp[0] == '@') sp++;
        size_t slen = strlen(sp);
        sess_ok = (uint8_t *)stelog_arena_alloc(scratch, (seg->session_count + 7u) / 8u + 1u);
        memset(sess_ok, 0, (seg->session_count + 7u) / 8u + 1u);
        for (uint32_t s = 0; s < seg->session_count; ++s) {
            StelogStr ss = stelog_seg_str(seg, seg->sessions[s].session_id);
            if (ss.len >= slen && stelog_memcasemem(ss.ptr, slen, sp, slen) == ss.ptr) {
                sess_ok[s >> 3] |= (uint8_t)(1u << (s & 7u));
                single_sid = s;
                sess_match_cnt++;
            }
        }
        if (sess_match_cnt == 0) return;
    }

    /* Pre-collect single-bit dimension bitmaps (shared across all OR branches) */
    StelogDocSet dim_sets[4];
    uint32_t dim_count = 0;
    if (agent_mask && (agent_mask & (agent_mask - 1u)) == 0) {
        StelogDocSet ds = stelog_seg_agent_bitmap(seg, (uint32_t)__builtin_ctz(agent_mask));
        if (ds.kind == STELOG_DOCSET_EMPTY) return;
        dim_sets[dim_count++] = ds;
    }
    if (role_mask && (role_mask & (role_mask - 1u)) == 0) {
        StelogDocSet ds = stelog_seg_role_bitmap(seg, (uint32_t)__builtin_ctz(role_mask));
        if (ds.kind == STELOG_DOCSET_EMPTY) return;
        dim_sets[dim_count++] = ds;
    }
    if (proj_match_cnt == 1u && single_proj_id != UINT32_MAX) {
        StelogDocSet ds = stelog_seg_project_bitmap(seg, single_proj_id);
        if (ds.kind == STELOG_DOCSET_EMPTY) return;
        dim_sets[dim_count++] = ds;
    }

    /* Resolve query-wide negations once per segment */
    SegNegFilter neg_filter;
    seg_neg_filter_init(seg, plan, &neg_filter);

    /* Candidate gathering: supports 0 branches (filter-only), 1 branch (fast path), or >=2 OR branches! */
    const float avg_dl = (float)seg->avg_text_len;
    const float k1 = 1.2f, b_param = 0.75f;

    BranchHit *bhits = NULL;
    uint32_t bhit_count = 0;
    uint32_t bhit_cap = 0;
    bool use_bhits = (plan->branch_count >= 2);

    uint32_t *single_cands = NULL;
    uint32_t single_count = 0;
    float single_idf = 1.0f;
    bool direct_range_scan = false;
    bool single_branch_already_verified = false;
    bool regex_needs_verify = false;

    if (re != NULL) {
        /* 3-Tier PCRE2 JIT Regex Acceleration:
           Tier 1: SwissTable + Roaring intersection on interior tokens of required literal
           Tier 2: P-core NEON/SSE2 2-byte SIMD scan on required literal >= 2 bytes
           Tier 3: P-core parallel PCRE2 JIT per-doc scan (Filter-Before-Match, exact ^/$ anchors) */
        const StelogRegexPrefilter *pf = stelog_regex_prefilter(re);
        single_idf = 2.0f;
        if (pf && pf->term_count > 0) {
            StelogDocSet sets[64];
            uint32_t sc = 0;
            single_idf = 0.0f;
            for (uint32_t t = 0; t < pf->term_count && sc < 60; ++t) {
                const StelogTermSlot *slot = stelog_swiss_find(&seg->terms, pf->term_hashes[t]);
                if (!slot || slot->doc_freq == 0) return;
                sets[sc++] = stelog_docset_from_term(slot, seg->postings_base);
                single_idf += logf(1.0f + ((float)seg->doc_count + 0.5f) / ((float)slot->doc_freq + 0.5f));
            }
            for (uint32_t di = 0; di < dim_count && sc < 64; ++di) sets[sc++] = dim_sets[di];
            if (single_idf == 0.0f) single_idf = 2.0f;
            single_count = stelog_docset_intersect(sets, sc, lo_doc, hi_doc, scratch, &single_cands);
            if (single_count == 0) return;
            regex_needs_verify = true;
        } else if (sess_match_cnt == 1u && single_sid != UINT32_MAX) {
            uint32_t s_begin = seg->session_offsets[single_sid];
            uint32_t s_end   = seg->session_offsets[single_sid + 1];
            single_cands = (uint32_t *)(uintptr_t)(seg->session_docs + s_begin);
            single_count = s_end - s_begin;
            regex_needs_verify = true;
        } else {
            uint32_t *dim_docs = NULL;
            uint32_t dim_n = 0;
            if (dim_count > 0) {
                dim_n = stelog_docset_intersect(dim_sets, dim_count, lo_doc, hi_doc, scratch, &dim_docs);
                if (dim_n == 0) return;
            }
            if (dim_count > 0 && dim_n <= 4096u) {
                single_cands = dim_docs;
                single_count = dim_n;
                regex_needs_verify = true;
            } else if (pf && pf->literal_len >= 2) {
                single_count = scan_text_literal_simd(seg, lo_doc, hi_doc,
                                                      pf->literal, pf->literal_len, pf->literal_cs,
                                                      false, scratch, &single_cands);
                if (single_count == 0) return;
                regex_needs_verify = true;
            } else {
                single_count = scan_text_regex_parallel(idx, seg, is_delta, lo_doc, hi_doc,
                                                        re, re_matcher, q->no_subagents,
                                                        agent_mask, role_mask, sess_ok, proj_ok, actor_ok,
                                                        scratch, &single_cands);
                if (single_count == 0) return;
                single_branch_already_verified = true;
            }
        }
    } else if (plan->branch_count == 0) {
        if (dim_count > 0) {
            single_count = stelog_docset_intersect(dim_sets, dim_count, lo_doc, hi_doc, scratch, &single_cands);
            if (single_count == 0) return;
        } else if (sess_match_cnt == 1u && single_sid != UINT32_MAX) {
            uint32_t s_begin = seg->session_offsets[single_sid];
            uint32_t s_end   = seg->session_offsets[single_sid + 1];
            single_cands = (uint32_t *)(uintptr_t)(seg->session_docs + s_begin);
            single_count = s_end - s_begin;
        } else {
            direct_range_scan = true;
            single_count = hi_doc - lo_doc + 1u;
        }
    } else if (plan->branch_count == 1) {
        const StelogQueryTokens *bt = &plan->branches[0];
        if (bt->term_count > 0) {
            StelogDocSet sets[64];
            uint32_t sc = 0;
            single_idf = 0.0f;
            for (uint32_t t = 0; t < bt->term_count && sc < 60; ++t) {
                const StelogTermSlot *slot = stelog_swiss_find(&seg->terms, bt->term_hashes[t]);
                if (!slot || slot->doc_freq == 0) return;
                sets[sc++] = stelog_docset_from_term(slot, seg->postings_base);
                single_idf += logf(1.0f + ((float)seg->doc_count + 0.5f) / ((float)slot->doc_freq + 0.5f));
            }
            for (uint32_t di = 0; di < dim_count && sc < 64; ++di) sets[sc++] = dim_sets[di];
            if (single_idf == 0.0f) single_idf = 1.0f;
            single_count = stelog_docset_intersect(sets, sc, lo_doc, hi_doc, scratch, &single_cands);
            if (single_count == 0) return;
        } else {
            /* Standalone prefix* or literal -F with no interior indexed tokens */
            single_idf = 1.5f;
            if (sess_match_cnt == 1u && single_sid != UINT32_MAX) {
                uint32_t s_begin = seg->session_offsets[single_sid];
                uint32_t s_end   = seg->session_offsets[single_sid + 1];
                single_cands = (uint32_t *)(uintptr_t)(seg->session_docs + s_begin);
                single_count = s_end - s_begin;
            } else {
                uint32_t *dim_docs = NULL;
                uint32_t dim_n = 0;
                if (dim_count > 0) {
                    dim_n = stelog_docset_intersect(dim_sets, dim_count, lo_doc, hi_doc, scratch, &dim_docs);
                    if (dim_n == 0) return;
                }
                if (dim_count > 0 && dim_n <= 4096u) {
                    single_cands = dim_docs;
                    single_count = dim_n;
                } else {
                    single_count = scan_text_contiguous_simd(seg, lo_doc, hi_doc, bt, scratch, &single_cands);
                    if (single_count == 0) return;
                    if (bt->verify_count + bt->prefix_count == 1u) {
                        single_branch_already_verified = true;
                    }
                }
            }
        }
    } else {
        /* Multi-branch OR: union results across branches (filter cheap metadata BEFORE text.bin verify!) */
        for (uint32_t bi = 0; bi < plan->branch_count; ++bi) {
            const StelogQueryTokens *bt = &plan->branches[bi];
            uint32_t *bc = NULL;
            uint32_t bn = 0;
            float bidf = 1.0f;
            bool branch_already_verified = false;

            if (bt->term_count > 0) {
                StelogDocSet sets[64];
                uint32_t sc = 0;
                bidf = 0.0f;
                bool missing = false;
                for (uint32_t t = 0; t < bt->term_count && sc < 60; ++t) {
                    const StelogTermSlot *slot = stelog_swiss_find(&seg->terms, bt->term_hashes[t]);
                    if (!slot || slot->doc_freq == 0) { missing = true; break; }
                    sets[sc++] = stelog_docset_from_term(slot, seg->postings_base);
                    bidf += logf(1.0f + ((float)seg->doc_count + 0.5f) / ((float)slot->doc_freq + 0.5f));
                }
                if (missing || sc == 0) continue;
                for (uint32_t di = 0; di < dim_count && sc < 64; ++di) sets[sc++] = dim_sets[di];
                if (bidf == 0.0f) bidf = 1.0f;
                bn = stelog_docset_intersect(sets, sc, lo_doc, hi_doc, scratch, &bc);
            } else {
                bidf = 1.5f;
                bn = scan_text_contiguous_simd(seg, lo_doc, hi_doc, bt, scratch, &bc);
                if (bt->verify_count + bt->prefix_count == 1u) {
                    branch_already_verified = true;
                }
            }

            for (uint32_t i = 0; i < bn; ++i) {
                uint32_t d = bc[i];
                if (d < lo_doc || d > hi_doc) continue;
                if (!is_delta && stelog_index_base_file_tombstoned(idx, seg->col_file_id[d])) continue;
                StelogTag tag = seg->col_tag[d];
                if (q->no_subagents && (stelog_tag_flags(tag) & STELOG_FLAG_IS_SUBAGENT)) continue;
                if (agent_mask && !(agent_mask & (1u << (uint32_t)stelog_tag_agent(tag)))) continue;
                if (role_mask && !(role_mask & (1u << (uint32_t)stelog_tag_role(tag)))) continue;
                if (sess_ok && !(sess_ok[seg->col_session_id[d] >> 3] & (1u << (seg->col_session_id[d] & 7u)))) continue;
                if (proj_ok && !(proj_ok[seg->col_project_id[d] >> 3] & (1u << (seg->col_project_id[d] & 7u)))) continue;
                if (actor_ok && !(actor_ok[seg->col_actor_id[d] >> 3] & (1u << (seg->col_actor_id[d] & 7u)))) continue;
                if (doc_excluded_by_neg_terms(&neg_filter, d)) continue;
                if (!branch_already_verified && !doc_passes_branch_verify(seg, d, bt)) continue;

                if (bhit_count == bhit_cap) {
                    uint32_t ncap = bhit_cap ? bhit_cap * 2 : 256;
                    BranchHit *nb = (BranchHit *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(BranchHit), 8);
                    if (!nb) return;
                    if (bhit_count > 0) memcpy(nb, bhits, bhit_count * sizeof(BranchHit));
                    bhits = nb;
                    bhit_cap = ncap;
                }
                bhits[bhit_count++] = (BranchHit){ d, bidf };
            }
        }
        if (bhit_count == 0) return;
        qsort(bhits, bhit_count, sizeof(BranchHit), cmp_branch_hit);
        /* Deduplicate doc_ids, taking max IDF across matched branches */
        uint32_t w = 0;
        for (uint32_t i = 0; i < bhit_count; ++i) {
            if (w > 0 && bhits[w - 1].doc_id == bhits[i].doc_id) {
                if (bhits[i].idf > bhits[w - 1].idf) bhits[w - 1].idf = bhits[i].idf;
            } else {
                bhits[w++] = bhits[i];
            }
        }
        bhit_count = w;
    }

    uint32_t iter_n = use_bhits ? bhit_count : single_count;
    if (iter_n > 0 && !q->existence_only) {
        uint32_t reserve_n = iter_n > 8192u ? 8192u : iter_n;
        cand_reserve(scratch, out_cands, out_cands->count + reserve_n);
    }
    const float inv_avg_dl = 1.0f / avg_dl;
    const float k1_one_minus_b = 1.0f + k1 * (1.0f - b_param);
    const float k1_b_inv_avg = k1 * b_param * inv_avg_dl;
    const float inv_30d_ms = 1.0f / (30.0f * 86400.0f * 1000.0f);

    for (uint32_t i = 0; i < iter_n; ++i) {
        uint32_t d = use_bhits ? bhits[i].doc_id : (direct_range_scan ? (lo_doc + i) : single_cands[i]);
        StelogRoleKind rk;

        if (!use_bhits) {
            if (!direct_range_scan && (d < lo_doc || d > hi_doc)) continue;
            if (!is_delta && stelog_index_base_file_tombstoned(idx, seg->col_file_id[d])) continue;

            StelogTag tag = seg->col_tag[d];
            if (q->no_subagents && (stelog_tag_flags(tag) & STELOG_FLAG_IS_SUBAGENT)) continue;

            StelogAgentKind ak = stelog_tag_agent(tag);
            rk = stelog_tag_role(tag);
            if (agent_mask && !(agent_mask & (1u << (uint32_t)ak))) continue;
            if (role_mask && !(role_mask & (1u << (uint32_t)rk))) continue;

            if (sess_ok) {
                uint32_t sid = seg->col_session_id[d];
                if (!(sess_ok[sid >> 3] & (1u << (sid & 7u)))) continue;
            }
            if (proj_ok) {
                uint32_t pid = seg->col_project_id[d];
                if (!(proj_ok[pid >> 3] & (1u << (pid & 7u)))) continue;
            }
            if (actor_ok) {
                uint32_t aid = seg->col_actor_id[d];
                if (!(actor_ok[aid >> 3] & (1u << (aid & 7u)))) continue;
            }

            /* Fast O(1) inverted-index negation BEFORE any text.bin access! */
            if (doc_excluded_by_neg_terms(&neg_filter, d)) continue;

            /* Positive text verify (only touches text.bin when needed!) */
            if (regex_needs_verify) {
                uint32_t tlen = seg->col_text_len[d];
                if (tlen == 0 || !stelog_regex_find(re, re_matcher,
                                                    seg->text_base + seg->col_text_off[d],
                                                    tlen, 0, NULL, NULL)) {
                    continue;
                }
            } else if (!re && plan->branch_count == 1 && !single_branch_already_verified &&
                       !doc_passes_branch_verify(seg, d, &plan->branches[0])) {
                continue;
            }
        } else {
            rk = stelog_tag_role(seg->col_tag[d]);
        }

        /* Guarded negative phrase/case/prefix verify (runs once per unique candidate) */
        if (doc_excluded_by_neg_verify(seg, d, plan, &neg_filter)) continue;

        float idf_val = use_bhits ? bhits[i].idf : single_idf;
        float dl = (float)(seg->col_text_len[d] ? seg->col_text_len[d] : 1u);
        /* Tool calls are short by construction (name + capped args), so BM25's short-document boost would
           otherwise rank a bare "Read: path" above the discussion that explains the decision. */
        if (rk == STELOG_ROLE_TOOL_CALL && dl < avg_dl) dl = avg_dl;
        float len_norm = (k1 + 1.0f) / (k1_one_minus_b + k1_b_inv_avg * dl);
        float rw = role_decision_weight(rk);
        float recency = 1.0f;
        int64_t ts = seg->col_ts_ms[d];
        if (max_ts_ms > 0 && ts > 0) {
            float age_30d = (ts < max_ts_ms) ? ((float)(max_ts_ms - ts) * inv_30d_ms) : 0.0f;
            recency = 1.0f + 0.25f / (1.0f + age_30d);
        }

        ScoredCand sc;
        sc.local_doc = d;
        sc.is_delta = is_delta ? 1u : 0u;
        sc.dup_count = 0;
        sc.ts_ms = ts;
        sc.score = idf_val * len_norm * rw * recency;
        cand_push(scratch, out_cands, sc);
        if (q->existence_only) return;
    }
}

static void probe_unfiltered_segment(const StelogIndex *idx,
                                     const StelogSegment *seg,
                                     bool is_delta,
                                     const StelogQueryPlan *plan,
                                     const StelogRegex *re,
                                     StelogRegexMatcher *re_matcher,
                                     StelogArena *scratch,
                                     uint32_t *total_out,
                                     uint32_t role_counts[STELOG_ROLE_COUNT]) {
    if (!seg || !seg->loaded || seg->doc_count == 0) return;
    if (re != NULL) {
        uint32_t *docs = NULL;
        uint32_t n = scan_text_regex_parallel(idx, seg, is_delta, 0, seg->doc_count - 1u,
                                              re, re_matcher, false, 0, 0, NULL, NULL, NULL,
                                              scratch, &docs);
        for (uint32_t i = 0; i < n; ++i) {
            StelogRoleKind r = stelog_tag_role(seg->col_tag[docs[i]]);
            if ((unsigned)r < STELOG_ROLE_COUNT) role_counts[r]++;
            (*total_out)++;
        }
        return;
    }
    if (plan->branch_count == 0) return;
    uint8_t *seen = (uint8_t *)stelog_arena_alloc(scratch, (seg->doc_count + 7u) / 8u + 1u);
    memset(seen, 0, (seg->doc_count + 7u) / 8u + 1u);

    SegNegFilter neg_filter;
    seg_neg_filter_init(seg, plan, &neg_filter);

    for (uint32_t bi = 0; bi < plan->branch_count; ++bi) {
        const StelogQueryTokens *bt = &plan->branches[bi];
        uint32_t *docs = NULL;
        uint32_t n = 0;
        if (bt->term_count > 0) {
            StelogDocSet sets[64];
            uint32_t sc = 0;
            bool missing = false;
            for (uint32_t t = 0; t < bt->term_count && sc < 64; ++t) {
                const StelogTermSlot *slot = stelog_swiss_find(&seg->terms, bt->term_hashes[t]);
                if (!slot || slot->doc_freq == 0) { missing = true; break; }
                sets[sc++] = stelog_docset_from_term(slot, seg->postings_base);
            }
            if (missing) continue;
            n = stelog_docset_intersect(sets, sc, 0, seg->doc_count - 1u, scratch, &docs);
        } else {
            n = scan_text_contiguous_simd(seg, 0, seg->doc_count - 1u, bt, scratch, &docs);
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t d = docs[i];
            if (seen[d >> 3] & (1u << (d & 7u))) continue;
            if (!is_delta && stelog_index_base_file_tombstoned(idx, seg->col_file_id[d])) continue;
            if (doc_excluded_by_neg_terms(&neg_filter, d)) continue;
            if (!doc_passes_branch_verify(seg, d, bt)) continue;
            if (doc_excluded_by_neg_verify(seg, d, plan, &neg_filter)) continue;
            seen[d >> 3] |= (uint8_t)(1u << (d & 7u));
            StelogRoleKind r = stelog_tag_role(seg->col_tag[d]);
            if ((unsigned)r < STELOG_ROLE_COUNT) role_counts[r]++;
            (*total_out)++;
        }
    }
}

static int cmp_cand_score_desc(const void *a, const void *b) {
    const ScoredCand *ca = (const ScoredCand *)a;
    const ScoredCand *cb = (const ScoredCand *)b;
    if (ca->score > cb->score) return -1;
    if (ca->score < cb->score) return 1;
    if (ca->ts_ms > cb->ts_ms) return -1;
    if (ca->ts_ms < cb->ts_ms) return 1;
    return 0;
}

static int cmp_cand_time_asc(const void *a, const void *b) {
    const ScoredCand *ca = (const ScoredCand *)a;
    const ScoredCand *cb = (const ScoredCand *)b;
    if (ca->ts_ms < cb->ts_ms) return -1;
    if (ca->ts_ms > cb->ts_ms) return 1;
    return (ca->local_doc < cb->local_doc) ? -1 : 1;
}

static int cmp_cand_time_desc(const void *a, const void *b) {
    return -cmp_cand_time_asc(a, b);
}

/* Fold exact duplicate texts (Codex fork copies / AGY retried prompts) in O(N) using scratch hash set */
typedef struct {
    uint64_t hash;
    uint32_t out_idx;
    uint32_t text_len;
    uint8_t  role;
    uint8_t  occupied;
} DedupSlot;

static inline bool cand_better(const ScoredCand *a, const ScoredCand *b, StelogSortMode mode) {
    if (mode == STELOG_SORT_OLDEST) {
        if (a->ts_ms != b->ts_ms) return a->ts_ms < b->ts_ms;
        return a->local_doc < b->local_doc;
    }
    if (mode == STELOG_SORT_NEWEST) {
        if (a->ts_ms != b->ts_ms) return a->ts_ms > b->ts_ms;
        return a->local_doc > b->local_doc;
    }
    if (a->score != b->score) return a->score > b->score;
    return a->ts_ms > b->ts_ms;
}

/* O(N log K) Top-K selection using an in-place min-heap of size K (root = worst of top K).
   Avoids O(N log N) qsort and avoids touching text.bin for thousands of non-Top-K candidates! */
static void select_top_k_cands(CandList *cands, uint32_t k, StelogSortMode mode) {
    if (k == 0 || k >= cands->count) return;
    ScoredCand *h = cands->items;

    /* Build worst-at-root heap on h[0..k) */
    for (int32_t start = (int32_t)(k / 2u) - 1; start >= 0; --start) {
        uint32_t idx = (uint32_t)start;
        for (;;) {
            uint32_t l = idx * 2u + 1u;
            if (l >= k) break;
            uint32_t r = l + 1u;
            /* Pick the WORSE child (not better) to bubble toward root */
            uint32_t worst = (r < k && cand_better(&h[l], &h[r], mode)) ? r : l;
            if (cand_better(&h[idx], &h[worst], mode)) {
                ScoredCand tmp = h[idx]; h[idx] = h[worst]; h[worst] = tmp;
                idx = worst;
            } else {
                break;
            }
        }
    }

    /* Stream remaining items: >95% rejected in 1 comparison against h[0] */
    for (uint32_t i = k; i < cands->count; ++i) {
        if (!cand_better(&h[i], &h[0], mode)) continue;
        h[0] = h[i];
        uint32_t idx = 0;
        for (;;) {
            uint32_t l = idx * 2u + 1u;
            if (l >= k) break;
            uint32_t r = l + 1u;
            uint32_t worst = (r < k && cand_better(&h[l], &h[r], mode)) ? r : l;
            if (cand_better(&h[idx], &h[worst], mode)) {
                ScoredCand tmp = h[idx]; h[idx] = h[worst]; h[worst] = tmp;
                idx = worst;
            } else {
                break;
            }
        }
    }
    cands->count = k;
}

static inline uint64_t fast_text_fingerprint(const char *txt, uint32_t tlen, uint8_t role) {
    uint64_t seed = ((uint64_t)role << 32) ^ (uint64_t)tlen ^ 0x55aa55aa55aa55aaull;
    if (tlen <= 256u) {
        return stelog_wyhash(txt, tlen, seed);
    }
    uint64_t h1 = stelog_wyhash(txt, 192u, seed);
    return stelog_wyhash(txt + tlen - 64u, 64u, h1);
}

static uint32_t dedup_candidates(const StelogIndex *idx, CandList *cands, StelogArena *scratch) {
    if (cands->count <= 1) return 0;
    uint32_t cap = 64;
    while (cap < cands->count * 2u) cap <<= 1u;
    DedupSlot *tab = (DedupSlot *)stelog_arena_alloc_aligned(scratch, cap * sizeof(DedupSlot), 8);
    if (!tab) return 0;
    memset(tab, 0, cap * sizeof(DedupSlot));
    uint32_t mask = cap - 1u;

    uint32_t write_n = 0;
    uint32_t folded = 0;
    for (uint32_t i = 0; i < cands->count; ++i) {
        const StelogSegment *seg = cands->items[i].is_delta ? &idx->delta : &idx->base;
        uint32_t d = cands->items[i].local_doc;
        uint8_t role = (uint8_t)stelog_tag_role(seg->col_tag[d]);
        const char *txt = seg->text_base + seg->col_text_off[d];
        uint32_t tlen = seg->col_text_len[d];
        uint64_t h = fast_text_fingerprint(txt, tlen, role);

        uint32_t pos = (uint32_t)h & mask;
        bool is_dup = false;
        while (tab[pos].occupied) {
            if (tab[pos].hash == h && tab[pos].role == role && tab[pos].text_len == tlen) {
                uint32_t prev_i = tab[pos].out_idx;
                const StelogSegment *pseg = cands->items[prev_i].is_delta ? &idx->delta : &idx->base;
                const char *ptxt = pseg->text_base + pseg->col_text_off[cands->items[prev_i].local_doc];
                if (tlen == 0 || memcmp(ptxt, txt, tlen) == 0) {
                    cands->items[prev_i].dup_count++;
                    folded++;
                    is_dup = true;
                    break;
                }
            }
            pos = (pos + 1u) & mask;
        }
        if (!is_dup) {
            tab[pos].hash = h;
            tab[pos].out_idx = write_n;
            tab[pos].text_len = tlen;
            tab[pos].role = role;
            tab[pos].occupied = 1;
            cands->items[write_n++] = cands->items[i];
        }
    }
    cands->count = write_n;
    return folded;
}

/* Arena-backed open-addressing map from string key to a dense u32 index (grows at 50% load). */
typedef struct {
    uint64_t hash;
    StelogStr key;
    uint32_t val;
    uint32_t used;
} StrMapSlot;

typedef struct {
    StrMapSlot *slots;
    uint32_t mask;
    uint32_t count;
    StelogArena *arena;
} StrMap;

static void strmap_init(StrMap *m, StelogArena *arena, uint32_t initial) {
    uint32_t cap = 64;
    while (cap < initial * 2u && cap < (1u << 30)) cap <<= 1u;
    m->arena = arena;
    m->slots = (StrMapSlot *)stelog_arena_alloc_aligned(arena, cap * sizeof(StrMapSlot), 8);
    memset(m->slots, 0, cap * sizeof(StrMapSlot));
    m->mask = cap - 1u;
    m->count = 0;
}

static void strmap_grow(StrMap *m) {
    uint32_t old_cap = m->mask + 1u;
    uint32_t cap = old_cap * 2u;
    StrMapSlot *ns = (StrMapSlot *)stelog_arena_alloc_aligned(m->arena, cap * sizeof(StrMapSlot), 8);
    memset(ns, 0, cap * sizeof(StrMapSlot));
    uint32_t mask = cap - 1u;
    for (uint32_t i = 0; i < old_cap; ++i) {
        if (!m->slots[i].used) continue;
        uint32_t pos = (uint32_t)m->slots[i].hash & mask;
        while (ns[pos].used) pos = (pos + 1u) & mask;
        ns[pos] = m->slots[i];
    }
    m->slots = ns;
    m->mask = mask;
}

/* Returns the existing value for key, or inserts new_val and sets *inserted. */
static uint32_t strmap_get_or_put(StrMap *m, StelogStr key, uint32_t new_val, bool *inserted) {
    if ((m->count + 1u) * 2u > m->mask + 1u) strmap_grow(m);
    uint64_t h = stelog_wyhash(key.ptr, key.len, 0x9e3779b97f4a7c15ull);
    uint32_t pos = (uint32_t)h & m->mask;
    while (m->slots[pos].used) {
        StrMapSlot *s = &m->slots[pos];
        if (s->hash == h && s->key.len == key.len && memcmp(s->key.ptr, key.ptr, key.len) == 0) {
            *inserted = false;
            return s->val;
        }
        pos = (pos + 1u) & m->mask;
    }
    m->slots[pos] = (StrMapSlot){ h, key, new_val, 1u };
    m->count++;
    *inserted = true;
    return new_val;
}

static int cmp_bucket_desc(const void *a, const void *b) {
    const StelogCountBucket *ba = (const StelogCountBucket *)a;
    const StelogCountBucket *bb = (const StelogCountBucket *)b;
    if (ba->count > bb->count) return -1;
    if (ba->count < bb->count) return 1;
    if (ba->meta_last_ts_ms > bb->meta_last_ts_ms) return -1;
    if (ba->meta_last_ts_ms < bb->meta_last_ts_ms) return 1;
    return 0;
}

static void build_count_by_buckets(const StelogIndex *idx,
                                   const CandList *cands,
                                   StelogCountByAxis axis,
                                   StelogArena *scratch,
                                   StelogCountBucket **out_buckets,
                                   uint32_t *out_count) {
    uint32_t cap = 64;
    StelogCountBucket *bk = (StelogCountBucket *)stelog_arena_alloc_aligned(scratch, cap * sizeof(StelogCountBucket), 8);
    uint32_t cnt = 0;
    StrMap keymap;
    strmap_init(&keymap, scratch, 64);

    for (uint32_t i = 0; i < cands->count; ++i) {
        const StelogSegment *seg = cands->items[i].is_delta ? &idx->delta : &idx->base;
        uint32_t d = cands->items[i].local_doc;
        StelogStr key = { "", 0 };
        const StelogSessionRecord *sr = &seg->sessions[seg->col_session_id[d]];
        switch (axis) {
            case STELOG_COUNT_BY_AGENT: {
                const char *an = stelog_agent_name(stelog_tag_agent(seg->col_tag[d]));
                key = (StelogStr){ an, (uint32_t)strlen(an) };
                break;
            }
            case STELOG_COUNT_BY_ROLE: {
                const char *rn = stelog_role_name(stelog_tag_role(seg->col_tag[d]));
                key = (StelogStr){ rn, (uint32_t)strlen(rn) };
                break;
            }
            case STELOG_COUNT_BY_PROJECT:
                key = stelog_seg_str(seg, seg->projects[seg->col_project_id[d]]);
                if (key.len == 0) key = (StelogStr){ "(none)", 6 };
                break;
            case STELOG_COUNT_BY_ACTOR:
                key = stelog_seg_str(seg, seg->actors[seg->col_actor_id[d]]);
                if (key.len == 0) key = (StelogStr){ "(unknown)", 9 };
                break;
            case STELOG_COUNT_BY_SESSION:
                key = stelog_seg_str(seg, sr->session_id);
                break;
            default:
                continue;
        }

        bool inserted = false;
        uint32_t k = strmap_get_or_put(&keymap, key, cnt, &inserted);
        if (!inserted) {
            bk[k].count++;
            if (seg->col_ts_ms[d] > bk[k].meta_last_ts_ms) bk[k].meta_last_ts_ms = seg->col_ts_ms[d];
        } else {
            if (cnt == cap) {
                uint32_t ncap = cap * 2;
                StelogCountBucket *nb = (StelogCountBucket *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(StelogCountBucket), 8);
                memcpy(nb, bk, cnt * sizeof(StelogCountBucket));
                bk = nb;
                cap = ncap;
            }
            memset(&bk[cnt], 0, sizeof(StelogCountBucket));
            bk[cnt].key = key;
            bk[cnt].count = 1;
            if (axis == STELOG_COUNT_BY_SESSION) {
                bk[cnt].meta_agent = (StelogAgentKind)sr->agent_kind;
                bk[cnt].meta_title = stelog_seg_str(seg, sr->title);
                bk[cnt].meta_project = stelog_seg_str(seg, sr->cwd);
                bk[cnt].meta_last_ts_ms = sr->last_ts_ms;
            }
            cnt++;
        }
    }

    if (cnt > 1) qsort(bk, cnt, sizeof(StelogCountBucket), cmp_bucket_desc);
    *out_buckets = bk;
    *out_count = cnt;
}

/* Relevance ranking: the best single hit dominates, hit volume only adds a log-scaled boost,
   so a long session with many weak mentions does not outrank a short one that is squarely on topic. */
static int cmp_session_hit_rank_desc(const void *a, const void *b) {
    const StelogSessionHit *sa = (const StelogSessionHit *)a;
    const StelogSessionHit *sb = (const StelogSessionHit *)b;
    if (sa->rank_score > sb->rank_score) return -1;
    if (sa->rank_score < sb->rank_score) return 1;
    if (sa->last_hit_ts_ms > sb->last_hit_ts_ms) return -1;
    if (sa->last_hit_ts_ms < sb->last_hit_ts_ms) return 1;
    return 0;
}

static int cmp_session_hit_newest(const void *a, const void *b) {
    const StelogSessionHit *sa = (const StelogSessionHit *)a;
    const StelogSessionHit *sb = (const StelogSessionHit *)b;
    if (sa->last_hit_ts_ms > sb->last_hit_ts_ms) return -1;
    if (sa->last_hit_ts_ms < sb->last_hit_ts_ms) return 1;
    return 0;
}

static int cmp_session_hit_oldest(const void *a, const void *b) {
    const StelogSessionHit *sa = (const StelogSessionHit *)a;
    const StelogSessionHit *sb = (const StelogSessionHit *)b;
    if (sa->first_hit_ts_ms < sb->first_hit_ts_ms) return -1;
    if (sa->first_hit_ts_ms > sb->first_hit_ts_ms) return 1;
    return 0;
}

static void build_session_groups(const StelogIndex *idx,
                                 const CandList *cands,
                                 const StelogQueryPlan *plan,
                                 const StelogRegex *re,
                                 StelogRegexMatcher *re_matcher,
                                 StelogSortMode sort_mode,
                                 StelogArena *scratch,
                                 StelogSessionHit **out_groups,
                                 uint32_t *out_count) {
    uint32_t cap = 64;
    StelogSessionHit *grp = (StelogSessionHit *)stelog_arena_alloc_aligned(scratch, cap * sizeof(StelogSessionHit), 8);
    float *best_scores = (float *)stelog_arena_alloc_aligned(scratch, cap * sizeof(float), 8);
    float *snip_scores = (float *)stelog_arena_alloc_aligned(scratch, cap * sizeof(float), 8);
    uint8_t *snip_dialogue = (uint8_t *)stelog_arena_alloc_aligned(scratch, cap, 8);
    uint32_t cnt = 0;
    StrMap sidmap;
    strmap_init(&sidmap, scratch, 64);

    for (uint32_t i = 0; i < cands->count; ++i) {
        const ScoredCand *sc = &cands->items[i];
        const StelogSegment *seg = sc->is_delta ? &idx->delta : &idx->base;
        uint32_t d = sc->local_doc;
        const StelogSessionRecord *sr = &seg->sessions[seg->col_session_id[d]];
        StelogStr sid = stelog_seg_str(seg, sr->session_id);

        bool inserted = false;
        uint32_t idx_found = strmap_get_or_put(&sidmap, sid, cnt, &inserted);
        if (inserted) {
            if (cnt == cap) {
                uint32_t ncap = cap * 2;
                StelogSessionHit *ng = (StelogSessionHit *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(StelogSessionHit), 8);
                float *ns = (float *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(float), 8);
                float *nss = (float *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(float), 8);
                uint8_t *nsd = (uint8_t *)stelog_arena_alloc_aligned(scratch, ncap, 8);
                memcpy(ng, grp, cnt * sizeof(StelogSessionHit));
                memcpy(ns, best_scores, cnt * sizeof(float));
                memcpy(nss, snip_scores, cnt * sizeof(float));
                memcpy(nsd, snip_dialogue, cnt);
                grp = ng; best_scores = ns; snip_scores = nss; snip_dialogue = nsd; cap = ncap;
            }
            idx_found = cnt++;
            memset(&grp[idx_found], 0, sizeof(StelogSessionHit));
            grp[idx_found].session_id      = sid;
            grp[idx_found].title           = stelog_seg_str(seg, sr->title);
            grp[idx_found].project         = stelog_seg_str(seg, sr->cwd);
            grp[idx_found].agent           = (StelogAgentKind)sr->agent_kind;
            grp[idx_found].first_hit_ts_ms = sc->ts_ms;
            grp[idx_found].last_hit_ts_ms  = sc->ts_ms;
            best_scores[idx_found]         = -1.0f;
            snip_scores[idx_found]         = -1.0f;
            snip_dialogue[idx_found]       = 0;
        }
        StelogSessionHit *g = &grp[idx_found];
        g->hit_count++;
        if (sc->ts_ms < g->first_hit_ts_ms) g->first_hit_ts_ms = sc->ts_ms;
        if (sc->ts_ms > g->last_hit_ts_ms)  g->last_hit_ts_ms  = sc->ts_ms;
        if (sc->score > best_scores[idx_found]) best_scores[idx_found] = sc->score;
        /* The representative snippet should show the conversation, so dialogue hits win over tool records
           even when a tool call scores higher; ranking still uses the overall best score. */
        StelogRoleKind rk = stelog_tag_role(seg->col_tag[d]);
        uint8_t dlg = is_dialogue_role(rk) ? 1u : 0u;
        if (dlg > snip_dialogue[idx_found] || (dlg == snip_dialogue[idx_found] && sc->score > snip_scores[idx_found])) {
            snip_dialogue[idx_found] = dlg;
            snip_scores[idx_found] = sc->score;
            g->best_doc_id = seg->col_doc_key[d];
            const char *txt = seg->text_base + seg->col_text_off[d];
            g->best_snippet = build_snippet(scratch, txt, seg->col_text_len[d],
                                            rk, plan, re, re_matcher, 220, NULL);
        }
    }

    for (uint32_t k = 0; k < cnt; ++k) {
        float best = best_scores[k] > 0.0f ? best_scores[k] : 0.0f;
        grp[k].best_score = best;
        grp[k].rank_score = best * (1.0f + 0.35f * log2f(1.0f + (float)grp[k].hit_count));
    }
    if (cnt > 1) {
        if (sort_mode == STELOG_SORT_NEWEST)      qsort(grp, cnt, sizeof(StelogSessionHit), cmp_session_hit_newest);
        else if (sort_mode == STELOG_SORT_OLDEST) qsort(grp, cnt, sizeof(StelogSessionHit), cmp_session_hit_oldest);
        else                                      qsort(grp, cnt, sizeof(StelogSessionHit), cmp_session_hit_rank_desc);
    }
    *out_groups = grp;
    *out_count = cnt;
}

static void populate_hit(const StelogSegment *seg,
                         bool is_delta,
                         uint32_t d,
                         float score,
                         uint32_t dup_count,
                         const StelogQueryPlan *plan,
                         const StelogRegex *re,
                         StelogRegexMatcher *re_matcher,
                         uint32_t snippet_max,
                         StelogArena *scratch,
                         StelogHit *out) {
    memset(out, 0, sizeof(*out));
    uint32_t sid = seg->col_session_id[d];
    const StelogSessionRecord *sr = &seg->sessions[sid];

    (void)is_delta;
    out->doc_id        = seg->col_doc_key[d];
    out->ts_ms         = seg->col_ts_ms[d];
    out->score         = score;
    out->tag           = seg->col_tag[d];
    out->full_text_len = seg->col_text_len[d];
    out->dup_count     = dup_count;
    out->raw_offset    = seg->col_raw_off[d];
    out->raw_len       = seg->col_raw_len[d];
    out->session_id    = stelog_seg_str(seg, sr->session_id);
    out->title         = stelog_seg_str(seg, sr->title);
    out->project       = stelog_seg_str(seg, seg->projects[seg->col_project_id[d]]);
    out->actor         = stelog_seg_str(seg, seg->actors[seg->col_actor_id[d]]);
    if (seg->col_file_id[d] < seg->file_count) {
        out->file_path = stelog_seg_str(seg, seg->files[seg->col_file_id[d]].path);
    }

    const char *txt = seg->text_base + seg->col_text_off[d];
    out->snippet = build_snippet(scratch, txt, seg->col_text_len[d],
                                 stelog_tag_role(out->tag), plan, re, re_matcher, snippet_max, &out->truncated);
}

int stelog_index_search(StelogIndex *idx, const StelogQuery *q, StelogSearchResult *out) {
    if (!idx || !q || !out) return -1;
    memset(out, 0, sizeof(*out));
    double t0 = stelog_now_ms();
    stelog_arena_reset(&idx->scratch);

    StelogQueryPlan plan;
    memset(&plan, 0, sizeof(plan));
    StelogRegex *re = NULL;
    StelogRegexMatcher *re_matcher = NULL;
    int rc = 0;

    if (q->regex_mode && q->query && q->query[0]) {
        char err_buf[256];
        re = stelog_regex_compile(q->query, strlen(q->query), q->case_mode, err_buf, sizeof(err_buf));
        if (!re) {
            char *copied = stelog_arena_strdup(&idx->scratch, err_buf);
            out->error_msg = (StelogStr){ copied, (uint32_t)strlen(err_buf) };
            out->elapsed_ms = stelog_now_ms() - t0;
            return -2;
        }
        re_matcher = stelog_regex_matcher_create(re);
        if (!re_matcher) {
            stelog_regex_destroy(re);
            out->elapsed_ms = stelog_now_ms() - t0;
            return -1;
        }
    } else {
        stelog_parse_query_plan_ex(q->query, q->query ? strlen(q->query) : 0,
                                   q->case_mode, q->fixed_strings, &plan);
    }

    /* Merge inline query modifiers (`agent:`, `role:`, `-role:`, `project:`, `actor:`, `session:`, `since:`, `until:`, `@hex`) */
    uint32_t agent_mask = q->agent_mask ? q->agent_mask : parse_mod_agent_csv(plan.mod_agent);
    uint32_t mod_role   = parse_mod_role_csv(plan.mod_role);
    uint32_t role_mask  = q->role_mask ? q->role_mask : (mod_role ? mod_role : STELOG_DEFAULT_ROLE_MASK);
    uint32_t excl_mask  = q->exclude_role_mask | parse_mod_role_csv(plan.mod_exclude_role);
    role_mask &= ~excl_mask;
    if (role_mask == 0) {
        goto done;
    }

    const char *proj_substr = (q->project_substr && q->project_substr[0])
        ? q->project_substr
        : (plan.mod_project.len ? stelog_arena_strndup(&idx->scratch, plan.mod_project.ptr, plan.mod_project.len) : NULL);
    const char *actor_substr = (q->actor_substr && q->actor_substr[0])
        ? q->actor_substr
        : (plan.mod_actor.len ? stelog_arena_strndup(&idx->scratch, plan.mod_actor.ptr, plan.mod_actor.len) : NULL);
    const char *session_id = (q->session_id && q->session_id[0])
        ? q->session_id
        : (plan.mod_session.len ? stelog_arena_strndup(&idx->scratch, plan.mod_session.ptr, plan.mod_session.len) : NULL);
    int64_t since_ms = q->since_ms;
    int64_t until_ms = q->until_ms;
    {
        int64_t mod_since = 0, mod_until = 0;
        bool ok_since = parse_mod_time_ms(plan.mod_since, false, &mod_since);
        bool ok_until = parse_mod_time_ms(plan.mod_until, true, &mod_until);
        if (!ok_since || !ok_until) {
            StelogStr bad = !ok_since ? plan.mod_since : plan.mod_until;
            char err_buf[320];
            snprintf(err_buf, sizeof(err_buf),
                     "Invalid inline %s:%.*s. Use 30m/24h/7d/2w, today, yesterday, YYYY-MM-DD, or YYYY-MM-DDTHH:MM[:SS][Z|+HH:MM].",
                     !ok_since ? "since" : "until", (int)(bad.len > 64 ? 64 : bad.len), bad.ptr);
            char *copied = stelog_arena_strdup(&idx->scratch, err_buf);
            out->error_msg = (StelogStr){ copied, (uint32_t)strlen(err_buf) };
            rc = -2;
            goto done;
        }
        if (!since_ms) since_ms = mod_since;
        if (!until_ms) until_ms = mod_until;
    }

    uint32_t limit    = q->limit ? q->limit : 20u;
    uint32_t offset   = q->offset;
    uint32_t snip_max = q->snippet_max_bytes ? q->snippet_max_bytes : 600u;
    StelogSortMode sort_mode = q->sort_mode;
    if (sort_mode == STELOG_SORT_RELEVANCE && q->sort_by_time) {
        sort_mode = STELOG_SORT_OLDEST;
    }

    int64_t max_ts = 0;
    if (idx->base.loaded && idx->base.doc_count > 0) max_ts = idx->base.col_ts_ms[idx->base.doc_count - 1];
    if (idx->delta.loaded && idx->delta.doc_count > 0) {
        int64_t dt = idx->delta.col_ts_ms[idx->delta.doc_count - 1];
        if (dt > max_ts) max_ts = dt;
    }

    ScoredCand first_match;
    CandList cands = { NULL, 0, 0 };
    if (q->existence_only) cands = (CandList){ &first_match, 0, 1 };
    uint32_t docs_in_scope = 0;
    query_one_segment(idx, &idx->base, false, q, &plan, re, re_matcher, agent_mask, role_mask,
                      proj_substr, actor_substr, session_id, since_ms, until_ms,
                      max_ts, &idx->scratch, &cands, &docs_in_scope);
    if (!q->existence_only || !cands.count) {
        query_one_segment(idx, &idx->delta, true,  q, &plan, re, re_matcher, agent_mask, role_mask,
                          proj_substr, actor_substr, session_id, since_ms, until_ms,
                          max_ts, &idx->scratch, &cands, &docs_in_scope);
    }
    if (q->existence_only) {
        out->total_matches = cands.count;
        goto done;
    }

    out->docs_in_scope = docs_in_scope;

    if (cands.count == 0) {
        if (re || plan.branch_count > 0) {
            probe_unfiltered_segment(idx, &idx->base, false, &plan, re, re_matcher, &idx->scratch,
                                     &out->unfiltered_total_matches, out->other_role_counts);
            probe_unfiltered_segment(idx, &idx->delta, true,  &plan, re, re_matcher, &idx->scratch,
                                     &out->unfiltered_total_matches, out->other_role_counts);
        }
        goto done;
    }

    bool need_all_cands = q->group_by_session || (q->count_by != STELOG_COUNT_BY_NONE) || q->sessions_only;
    uint32_t raw_total_matches = cands.count;
    uint32_t top_k_pool = (offset + limit) * 2u + 32u;
    bool trimmed_top_k = false;

    if (!need_all_cands && top_k_pool < cands.count) {
        select_top_k_cands(&cands, top_k_pool, sort_mode);
        trimmed_top_k = true;
    }

    /* Sort kept candidates so primary kept hit on dedup respects relevance/oldest/newest */
    if (sort_mode == STELOG_SORT_OLDEST) {
        if (trimmed_top_k || (idx->base.loaded && idx->delta.loaded)) {
            qsort(cands.items, cands.count, sizeof(ScoredCand), cmp_cand_time_asc);
        }
    } else if (sort_mode == STELOG_SORT_NEWEST) {
        qsort(cands.items, cands.count, sizeof(ScoredCand), cmp_cand_time_desc);
    } else {
        qsort(cands.items, cands.count, sizeof(ScoredCand), cmp_cand_score_desc);
    }

    /* Fold exact duplicate copies (Codex forks / AGY retries) unless --no-dedup */
    if (!q->no_dedup) {
        out->duplicates_folded = dedup_candidates(idx, &cands, &idx->scratch);
    }
    out->total_matches = trimmed_top_k ? (raw_total_matches - out->duplicates_folded) : cands.count;

    if (q->group_by_session) {
        StelogSessionHit *grps = NULL;
        uint32_t gcnt = 0;
        build_session_groups(idx, &cands, &plan, re, re_matcher, sort_mode, &idx->scratch, &grps, &gcnt);
        uint32_t avail = offset < gcnt ? (gcnt - offset) : 0u;
        uint32_t take = avail < limit ? avail : limit;
        out->session_groups = grps + offset;
        out->session_group_count = take;
        out->session_group_total = gcnt;
        out->truncated = (offset + take < gcnt);
        goto done;
    }

    if (q->count_by != STELOG_COUNT_BY_NONE) {
        build_count_by_buckets(idx, &cands, q->count_by, &idx->scratch, &out->buckets, &out->bucket_count);
    }

    if (q->sessions_only) {
        StelogCountBucket *sb = NULL;
        uint32_t scnt = 0;
        build_count_by_buckets(idx, &cands, STELOG_COUNT_BY_SESSION, &idx->scratch, &sb, &scnt);
        StelogStr *sids = (StelogStr *)stelog_arena_alloc_aligned(&idx->scratch, scnt * sizeof(StelogStr), 8);
        for (uint32_t i = 0; i < scnt; ++i) sids[i] = sb[i].key;
        out->matched_sessions = sids;
        out->matched_session_count = scnt;
        goto done;
    }

    if (offset >= cands.count) {
        goto done;
    }

    uint32_t avail = cands.count - offset;
    uint32_t take = avail < limit ? avail : limit;
    out->truncated = (offset + take < cands.count);

    StelogHit *hits = (StelogHit *)stelog_arena_alloc_aligned(&idx->scratch, take * sizeof(StelogHit), 8);
    if (!hits) { rc = -1; goto done; }

    for (uint32_t i = 0; i < take; ++i) {
        const ScoredCand *sc = &cands.items[offset + i];
        const StelogSegment *seg = sc->is_delta ? &idx->delta : &idx->base;
        populate_hit(seg, sc->is_delta != 0, sc->local_doc, sc->score, sc->dup_count,
                     &plan, re, re_matcher, snip_max, &idx->scratch, &hits[i]);
    }

    out->hits = hits;
    out->count = take;

done:
    if (re_matcher) stelog_regex_matcher_destroy(re_matcher);
    if (re) stelog_regex_destroy(re);
    out->elapsed_ms = stelog_now_ms() - t0;
    return rc;
}

/* ============================================================================
 * File-Touch Provenance (stelog files / MCP search_file_history)
 * ============================================================================ */

#define MAX_TOUCH_PATHS 16u

static bool is_write_tool_name(const char *t, size_t n) {
    static const char *names[] = {
        "edit_file", "write_file", "write_to_file", "apply_patch", "replace_file_content",
        "multi_replace_file_content", "edit", "write", "Create", "Edit", "MultiEdit", "Write",
        "ApplyPatch", "NotebookEdit"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        size_t l = strlen(names[i]);
        if (n == l && memcmp(t, names[i], l) == 0) return true;
    }
    return false;
}

static bool push_touch_path(StelogArena *arena, const char *p, const char *pe,
                            StelogStr *out_paths, uint32_t *n, uint32_t max_paths) {
    while (pe > p && (pe[-1] == ' ' || pe[-1] == '\t')) pe--;
    if (pe <= p || *n >= max_paths) return false;
    size_t l = (size_t)(pe - p);
    for (uint32_t i = 0; i < *n; ++i) {
        if (out_paths[i].len == l && memcmp(out_paths[i].ptr, p, l) == 0) return true;
    }
    out_paths[*n] = (StelogStr){ stelog_arena_strndup(arena, p, l), (uint32_t)l };
    (*n)++;
    return true;
}

/* Extracts every file a write-style tool call touched. Patch bodies may sit inside exec/shell wrappers,
   so "*** <Op> File:" markers count for any tool once the text contains "*** Begin Patch". */
static uint32_t extract_file_touches_from_tool_call(const char *txt,
                                                    uint32_t len,
                                                    StelogArena *arena,
                                                    StelogStr *out_tool,
                                                    StelogStr *out_paths,
                                                    uint32_t max_paths) {
    const char *colon = (const char *)memchr(txt, ':', len < 64 ? len : 64);
    if (!colon) return 0;
    size_t tlen = (size_t)(colon - txt);
    const char *args = colon + 1;
    size_t alen = len - tlen - 1;
    const char *end = args + alen;

    bool is_write_tool = is_write_tool_name(txt, tlen);
    bool has_patch = stelog_memmem(args, alen, "*** Begin Patch", 15) != NULL;
    if (!is_write_tool && !has_patch) return 0;

    *out_tool = is_write_tool ? (StelogStr){ txt, (uint32_t)tlen } : (StelogStr){ "apply_patch", 11 };
    uint32_t n = 0;

    static const char *markers[] = { "*** Update File: ", "*** Add File: ", "*** Delete File: " };
    for (size_t m = 0; m < 3; ++m) {
        size_t ml = strlen(markers[m]);
        const char *scan = args;
        while (scan < end) {
            const char *pm = stelog_memmem(scan, (size_t)(end - scan), markers[m], ml);
            if (!pm) break;
            const char *p = pm + ml;
            const char *pe = p;
            while (pe < end && *pe != '\n' && *pe != '\r' && *pe != '\\' && *pe != '"') pe++;
            push_touch_path(arena, p, pe, out_paths, &n, max_paths);
            scan = pe;
        }
    }
    if (n > 0 || !is_write_tool) return n;

    static const char *keys[] = {
        "\"Path\"", "\"path\"", "\"TargetFile\"", "\"file_path\"", "\"filePath\"", "\"FilePath\"",
        "\"AbsolutePath\"", "\"absolute_path\"", "\"target_file\"", "\"notebook_path\""
    };
    const char *best = NULL;
    size_t best_kl = 0;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
        size_t kl = strlen(keys[k]);
        const char *hit = stelog_memmem(args, alen, keys[k], kl);
        if (hit && (!best || hit < best)) { best = hit; best_kl = kl; }
    }
    if (best) {
        const char *p = best + best_kl;
        while (p < end && (*p == ' ' || *p == ':' || *p == '"' || *p == '\\')) p++;
        const char *pe = p;
        while (pe < end && *pe != '"' && *pe != '\\' && *pe != ',' && *pe != '}' && *pe != '\n') pe++;
        push_touch_path(arena, p, pe, out_paths, &n, max_paths);
    }
    return n;
}
static int cmp_file_touch_desc(const void *a, const void *b) {
    const StelogFileTouch *fa = (const StelogFileTouch *)a;
    const StelogFileTouch *fb = (const StelogFileTouch *)b;
    if (fa->ts_ms > fb->ts_ms) return -1;
    if (fa->ts_ms < fb->ts_ms) return 1;
    return 0;
}

static void scan_segment_files(const StelogIndex *idx,
                               const StelogSegment *seg,
                               bool is_delta,
                               const char *file_substr,
                               uint32_t agent_mask,
                               const char *project_substr,
                               const char *session_prefix,
                               int64_t since_ms,
                               int64_t until_ms,
                               bool no_subagents,
                               StelogArena *scratch,
                               StelogFileTouch **io_items,
                               uint32_t *io_count,
                               uint32_t *io_cap) {
    if (!seg || !seg->loaded || seg->doc_count == 0) return;
    uint32_t lo_doc = 0, hi_doc = 0;
    if (!find_time_doc_range(seg, since_ms, until_ms, &lo_doc, &hi_doc)) return;

    StelogDocSet role_bm = stelog_seg_role_bitmap(seg, STELOG_ROLE_TOOL_CALL);
    if (role_bm.kind == STELOG_DOCSET_EMPTY) return;

    StelogDocSet sets[2];
    uint32_t sc = 0;
    sets[sc++] = role_bm;
    if (agent_mask && (agent_mask & (agent_mask - 1u)) == 0) {
        StelogDocSet abm = stelog_seg_agent_bitmap(seg, (uint32_t)__builtin_ctz(agent_mask));
        if (abm.kind == STELOG_DOCSET_EMPTY) return;
        sets[sc++] = abm;
    }

    uint32_t *docs = NULL;
    uint32_t n = stelog_docset_intersect(sets, sc, lo_doc, hi_doc, scratch, &docs);
    size_t flen = file_substr ? strlen(file_substr) : 0;
    size_t plen = project_substr ? strlen(project_substr) : 0;
    const char *sp = session_prefix;
    if (sp && sp[0] == '@') sp++;
    size_t slen = sp ? strlen(sp) : 0;

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t d = docs[i];
        if (!is_delta && stelog_index_base_file_tombstoned(idx, seg->col_file_id[d])) continue;
        StelogTag tag = seg->col_tag[d];
        if (no_subagents && (stelog_tag_flags(tag) & STELOG_FLAG_IS_SUBAGENT)) continue;
        if (agent_mask && !(agent_mask & (1u << (uint32_t)stelog_tag_agent(tag)))) continue;

        uint32_t sid = seg->col_session_id[d];
        const StelogSessionRecord *sr = &seg->sessions[sid];
        StelogStr sstr = stelog_seg_str(seg, sr->session_id);
        if (slen > 0 && (sstr.len < slen || stelog_memcasemem(sstr.ptr, slen, sp, slen) != sstr.ptr)) continue;

        StelogStr proj = stelog_seg_str(seg, seg->projects[seg->col_project_id[d]]);
        if (plen > 0 && (!proj.len || !stelog_memcasemem(proj.ptr, proj.len, project_substr, plen))) continue;

        const char *txt = seg->text_base + seg->col_text_off[d];
        uint32_t tlen = seg->col_text_len[d];
        StelogStr tool_name;
        StelogStr touched[MAX_TOUCH_PATHS];
        uint32_t nt = extract_file_touches_from_tool_call(txt, tlen, scratch, &tool_name, touched, MAX_TOUCH_PATHS);
        for (uint32_t ti = 0; ti < nt; ++ti) {
            if (flen > 0 && !stelog_memcasemem(touched[ti].ptr, touched[ti].len, file_substr, flen)) continue;
            if (*io_count == *io_cap) {
                uint32_t ncap = *io_cap ? (*io_cap * 2) : 128;
                StelogFileTouch *ni = (StelogFileTouch *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(StelogFileTouch), 8);
                if (*io_count > 0) memcpy(ni, *io_items, (*io_count) * sizeof(StelogFileTouch));
                *io_items = ni;
                *io_cap = ncap;
            }
            StelogFileTouch *ft = &(*io_items)[(*io_count)++];
            ft->doc_id       = seg->col_doc_key[d];
            ft->ts_ms        = seg->col_ts_ms[d];
            ft->tag          = tag;
            ft->file_touched = touched[ti];
            ft->tool_name    = tool_name;
            ft->session_id   = sstr;
            ft->title        = stelog_seg_str(seg, sr->title);
            ft->project      = proj;
            ft->actor        = stelog_seg_str(seg, seg->actors[seg->col_actor_id[d]]);
        }
    }
}

int stelog_index_files(StelogIndex *idx,
                       const char *file_substr,
                       uint32_t agent_mask,
                       const char *project_substr,
                       const char *session_prefix,
                       int64_t since_ms,
                       int64_t until_ms,
                       bool no_subagents,
                       uint32_t limit,
                       uint32_t offset,
                       StelogFileHistoryResult *out) {
    if (!idx || !out) return -1;
    memset(out, 0, sizeof(*out));
    double t0 = stelog_now_ms();
    stelog_arena_reset(&idx->scratch);

    StelogFileTouch *items = NULL;
    uint32_t count = 0, cap = 0;
    scan_segment_files(idx, &idx->base,  false, file_substr, agent_mask, project_substr, session_prefix,
                       since_ms, until_ms, no_subagents, &idx->scratch, &items, &count, &cap);
    scan_segment_files(idx, &idx->delta, true,  file_substr, agent_mask, project_substr, session_prefix,
                       since_ms, until_ms, no_subagents, &idx->scratch, &items, &count, &cap);

    out->total_matches = count;
    if (count == 0 || offset >= count) {
        out->elapsed_ms = stelog_now_ms() - t0;
        return 0;
    }

    qsort(items, count, sizeof(StelogFileTouch), cmp_file_touch_desc);
    uint32_t lim = limit ? limit : 30u;
    uint32_t avail = count - offset;
    uint32_t take = avail < lim ? avail : lim;
    out->items = items + offset;
    out->count = take;
    out->truncated = (offset + take < count);
    out->elapsed_ms = stelog_now_ms() - t0;
    return 0;
}

/* ============================================================================
 * Context Window Lookup (Dialogue-only by default, collapsing intermediate tools!)
 * ============================================================================ */

int stelog_index_context(StelogIndex *idx,
                         uint64_t anchor_doc_id,
                         uint32_t window,
                         uint32_t max_bytes_per_item,
                         bool include_tools,
                         StelogContextResult *out) {
    if (!idx || !out) return -1;
    memset(out, 0, sizeof(*out));
    double t0 = stelog_now_ms();
    stelog_arena_reset(&idx->scratch);

    uint64_t internal_id;
    if (!stelog_index_resolve_doc_id(idx, anchor_doc_id, &internal_id)) return -1;
    bool is_delta = stelog_doc_is_delta(internal_id);
    uint32_t d = stelog_doc_local_id(internal_id);
    const StelogSegment *seg = is_delta ? &idx->delta : &idx->base;
    if (!seg->loaded || d >= seg->doc_count) return -1;

    uint32_t sid = seg->col_session_id[d];
    const StelogSessionRecord *sr = &seg->sessions[sid];
    out->session_id = stelog_seg_str(seg, sr->session_id);
    out->title      = stelog_seg_str(seg, sr->title);
    out->project    = stelog_seg_str(seg, sr->cwd);

    uint32_t s_begin = seg->session_offsets[sid];
    uint32_t s_end   = seg->session_offsets[sid + 1];
    uint32_t s_len   = s_end - s_begin;
    const uint32_t *s_docs = seg->session_docs + s_begin;

    uint32_t lo = 0, hi = s_len;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (s_docs[mid] < d) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= s_len || s_docs[lo] != d) return -1;

    uint32_t w = window ? window : 5u;
    uint32_t max_b = max_bytes_per_item ? max_bytes_per_item : 1800u;

    /* Collect up to `w` preceding & `w` following turns (skipping tool_calls when !include_tools) */
    uint32_t *picked = (uint32_t *)stelog_arena_alloc_aligned(&idx->scratch, (2u * w + 2u) * sizeof(uint32_t), 8);
    uint32_t *tools_before = (uint32_t *)stelog_arena_alloc_aligned(&idx->scratch, (2u * w + 2u) * sizeof(uint32_t), 8);

    uint32_t *prev_ids = (uint32_t *)stelog_arena_alloc_aligned(&idx->scratch, (w + 1u) * sizeof(uint32_t), 8);
    uint32_t *prev_tools = (uint32_t *)stelog_arena_alloc_aligned(&idx->scratch, (w + 1u) * sizeof(uint32_t), 8);
    uint32_t prev_cnt = 0;
    uint32_t tools_acc = 0;

    for (uint32_t pos = lo; pos > 0 && prev_cnt < w; ) {
        pos--;
        uint32_t cd = s_docs[pos];
        StelogRoleKind rk = stelog_tag_role(seg->col_tag[cd]);
        if (!include_tools && !is_dialogue_role(rk)) {
            tools_acc++;
            continue;
        }
        prev_ids[prev_cnt] = cd;
        prev_tools[prev_cnt] = tools_acc;
        tools_acc = 0;
        prev_cnt++;
    }

    uint32_t count = 0;
    for (uint32_t i = prev_cnt; i > 0; --i) {
        picked[count] = prev_ids[i - 1];
        /* tools_before[count] is the tools between previous emitted turn and this one */
        tools_before[count] = (i == prev_cnt) ? 0u : prev_tools[i];
        count++;
    }
    uint32_t anchor_idx = count;
    picked[count] = d;
    tools_before[count] = (prev_cnt > 0) ? prev_tools[0] : tools_acc;
    count++;

    uint32_t next_cnt = 0;
    tools_acc = 0;
    for (uint32_t pos = lo + 1; pos < s_len && next_cnt < w; ++pos) {
        uint32_t cd = s_docs[pos];
        StelogRoleKind rk = stelog_tag_role(seg->col_tag[cd]);
        if (!include_tools && !is_dialogue_role(rk)) {
            tools_acc++;
            continue;
        }
        picked[count] = cd;
        tools_before[count] = tools_acc;
        tools_acc = 0;
        count++;
        next_cnt++;
    }

    StelogHit *items = (StelogHit *)stelog_arena_alloc_aligned(&idx->scratch, count * sizeof(StelogHit), 8);
    if (!items) return -1;

    for (uint32_t i = 0; i < count; ++i) {
        populate_hit(seg, is_delta, picked[i], 1.0f, 0, NULL, NULL, NULL, max_b, &idx->scratch, &items[i]);
        items[i].collapsed_tools_before = tools_before[i];
    }

    out->items = items;
    out->count = count;
    out->anchor_index = anchor_idx;
    out->elapsed_ms = stelog_now_ms() - t0;
    return 0;
}

/* ============================================================================
 * Session Decision Digest (stelog digest @<prefix> / MCP get_session_digest)
 * ============================================================================ */

typedef struct {
    const StelogSegment *seg;
    bool is_delta;
    uint32_t local_doc;
    int64_t ts_ms;
} DigestRef;

static int cmp_digest_ref(const void *a, const void *b) {
    const DigestRef *ra = (const DigestRef *)a;
    const DigestRef *rb = (const DigestRef *)b;
    if (ra->ts_ms < rb->ts_ms) return -1;
    if (ra->ts_ms > rb->ts_ms) return 1;
    return (ra->local_doc < rb->local_doc) ? -1 : 1;
}

static bool str_prefix_ieq(StelogStr s, const char *p, size_t plen) {
    if (s.len < plen) return false;
    for (size_t i = 0; i < plen; ++i) {
        char a = s.ptr[i], b = p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static int cmp_summary_last_ts_desc(const void *a, const void *b) {
    const StelogSessionSummary *sa = (const StelogSessionSummary *)a;
    const StelogSessionSummary *sb = (const StelogSessionSummary *)b;
    if (sa->last_ts_ms > sb->last_ts_ms) return -1;
    if (sa->last_ts_ms < sb->last_ts_ms) return 1;
    return 0;
}

#define RESOLVE_TRACK_MAX 64u

uint32_t stelog_index_resolve_session(const StelogIndex *idx,
                                      const char *id_or_prefix,
                                      StelogSessionSummary *out,
                                      uint32_t cap) {
    if (!idx || !id_or_prefix) return 0;
    const char *p = id_or_prefix;
    if (p[0] == '@') p++;
    size_t plen = strlen(p);
    if (plen == 0) return 0;

    StelogSessionSummary found[RESOLVE_TRACK_MAX];
    uint32_t nfound = 0;
    uint32_t total = 0;
    bool have_exact = false;

    const StelogSegment *segs[2] = { &idx->base, &idx->delta };
    for (int si = 0; si < 2; ++si) {
        const StelogSegment *seg = segs[si];
        if (!seg->loaded) continue;
        for (uint32_t s = 0; s < seg->session_count; ++s) {
            const StelogSessionRecord *sr = &seg->sessions[s];
            if (sr->doc_count == 0) continue;
            StelogStr ss = stelog_seg_str(seg, sr->session_id);
            if (!str_prefix_ieq(ss, p, plen)) continue;
            bool is_exact = (ss.len == plen);
            if (have_exact && !is_exact) continue;
            if (is_exact && !have_exact) {
                have_exact = true;
                nfound = 0;
                total = 0;
            }
            uint32_t j = 0;
            for (; j < nfound; ++j) {
                if (found[j].session_id.len == ss.len && memcmp(found[j].session_id.ptr, ss.ptr, ss.len) == 0) break;
            }
            if (j < nfound) {
                if (sr->first_ts_ms < found[j].first_ts_ms) found[j].first_ts_ms = sr->first_ts_ms;
                if (sr->last_ts_ms > found[j].last_ts_ms) found[j].last_ts_ms = sr->last_ts_ms;
                found[j].doc_count += sr->doc_count;
                if (found[j].title.len == 0) found[j].title = stelog_seg_str(seg, sr->title);
                continue;
            }
            total++;
            if (nfound < RESOLVE_TRACK_MAX) {
                StelogSessionSummary *dst = &found[nfound++];
                memset(dst, 0, sizeof(*dst));
                dst->session_id        = ss;
                dst->title             = stelog_seg_str(seg, sr->title);
                dst->project           = stelog_seg_str(seg, sr->cwd);
                dst->git_branch        = stelog_seg_str(seg, sr->git_branch);
                dst->parent_session_id = stelog_seg_str(seg, sr->parent_session_id);
                dst->first_ts_ms       = sr->first_ts_ms;
                dst->last_ts_ms        = sr->last_ts_ms;
                dst->doc_count         = sr->doc_count;
                dst->agent             = (StelogAgentKind)sr->agent_kind;
                dst->flags             = sr->flags;
            }
        }
    }
    if (nfound > 1) qsort(found, nfound, sizeof(StelogSessionSummary), cmp_summary_last_ts_desc);
    uint32_t n = nfound < cap ? nfound : cap;
    if (out && n > 0) memcpy(out, found, n * sizeof(StelogSessionSummary));
    return total;
}

uint32_t stelog_index_unique_prefix_len(const StelogIndex *idx, StelogStr session_id, uint32_t min_len) {
    if (session_id.len <= min_len) return session_id.len;
    if (!idx) return min_len;
    uint32_t longest_common = 0;
    const StelogSegment *segs[2] = { &idx->base, &idx->delta };
    for (int si = 0; si < 2; ++si) {
        const StelogSegment *seg = segs[si];
        if (!seg->loaded) continue;
        for (uint32_t s = 0; s < seg->session_count; ++s) {
            StelogStr ss = stelog_seg_str(seg, seg->sessions[s].session_id);
            if (ss.len < min_len) continue;
            uint32_t n = ss.len < session_id.len ? ss.len : session_id.len;
            uint32_t c = 0;
            while (c < n) {
                char a = ss.ptr[c], b = session_id.ptr[c];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
                if (a != b) break;
                c++;
            }
            if (c == ss.len && c == session_id.len) continue; /* same session (base/delta copy) */
            if (c > longest_common) longest_common = c;
        }
    }
    uint32_t need = longest_common + 1u;
    if (need < min_len) need = min_len;
    if (need > session_id.len) need = session_id.len;
    return need;
}

static void collect_session_docs_for_digest(const StelogIndex *idx,
                                            const StelogSegment *seg,
                                            bool is_delta,
                                            StelogStr session_id,
                                            StelogArena *scratch,
                                            DigestRef **io_refs,
                                            uint32_t *io_cnt,
                                            uint32_t *io_cap) {
    if (!seg || !seg->loaded) return;
    for (uint32_t s = 0; s < seg->session_count; ++s) {
        StelogStr ss = stelog_seg_str(seg, seg->sessions[s].session_id);
        if (ss.len != session_id.len || memcmp(ss.ptr, session_id.ptr, ss.len) != 0) continue;
        uint32_t sb = seg->session_offsets[s];
        uint32_t se = seg->session_offsets[s + 1];
        for (uint32_t i = sb; i < se; ++i) {
            uint32_t d = seg->session_docs[i];
            if (!is_delta && stelog_index_base_file_tombstoned(idx, seg->col_file_id[d])) continue;
            if (*io_cnt == *io_cap) {
                uint32_t ncap = *io_cap ? *io_cap * 2 : 128;
                DigestRef *nr = (DigestRef *)stelog_arena_alloc_aligned(scratch, ncap * sizeof(DigestRef), 8);
                if (*io_cnt > 0) memcpy(nr, *io_refs, (*io_cnt) * sizeof(DigestRef));
                *io_refs = nr;
                *io_cap = ncap;
            }
            (*io_refs)[(*io_cnt)++] = (DigestRef){ seg, is_delta, d, seg->col_ts_ms[d] };
        }
    }
}

/* Per-turn byte cap so short turns donate unused budget to substantive ones. `rem_turns` is how many
   turns still compete for `rem` bytes. */
static uint32_t digest_item_cap(StelogRoleKind rk, uint32_t rem, uint32_t rem_turns) {
    if (rem_turns == 0) rem_turns = 1;
    uint32_t overhead = rem_turns * 72u;
    uint32_t dyn = (rem > overhead) ? ((rem - overhead) / rem_turns) : 120u;
    if (dyn < 120u) dyn = 120u;
    if (dyn > 1200u) dyn = 1200u;
    uint32_t cap = dyn;
    if (rk == STELOG_ROLE_USER) {
        cap = (dyn * 3u) / 2u;
        if (cap < 320u) cap = 320u;
        if (cap > 600u) cap = 600u;
    }
    uint32_t max_allowed = rem > 72u ? (rem - 72u) : 120u;
    return cap < max_allowed ? cap : max_allowed;
}

/* Emits kept[i] for i in [start, end) moving forward until the budget runs out; returns the number emitted. */
static uint32_t digest_emit_forward(const DigestRef *kept, const uint32_t *tools_before, uint32_t start, uint32_t end,
                                    uint32_t budget, uint32_t turn_estimate_cap, StelogArena *scratch, StelogHit *items) {
    uint32_t used = 0, emitted = 0;
    for (uint32_t i = start; i < end; ++i) {
        if (used + 80u > budget && emitted > 0) break;
        const StelogSegment *seg = kept[i].seg;
        uint32_t d = kept[i].local_doc;
        StelogRoleKind rk = stelog_tag_role(seg->col_tag[d]);
        uint32_t rem_turns = end - i;
        if (turn_estimate_cap && rem_turns > turn_estimate_cap) rem_turns = turn_estimate_cap;
        uint32_t rem = budget > used ? (budget - used) : 0u;
        uint32_t cap = digest_item_cap(rk, rem, rem_turns);
        populate_hit(seg, kept[i].is_delta, d, 1.0f, 0, NULL, NULL, NULL, cap, scratch, &items[emitted]);
        items[emitted].collapsed_tools_before = tools_before[i];
        used += items[emitted].snippet.len + 72u;
        emitted++;
    }
    return emitted;
}

int stelog_index_digest_ex(StelogIndex *idx, const StelogDigestOptions *opts, StelogDigestResult *out) {
    if (!idx || !opts || !opts->session || !opts->session[0] || !out) return -1;
    memset(out, 0, sizeof(*out));
    double t0 = stelog_now_ms();
    stelog_arena_reset(&idx->scratch);

    uint32_t matches = stelog_index_resolve_session(idx, opts->session, out->candidates, STELOG_MAX_SESSION_CANDIDATES);
    out->match_count = matches;
    out->candidate_count = matches < STELOG_MAX_SESSION_CANDIDATES ? matches : STELOG_MAX_SESSION_CANDIDATES;
    if (matches == 0) return -1;
    if (matches > 1) {
        out->elapsed_ms = stelog_now_ms() - t0;
        return -2;
    }
    const StelogSessionSummary *target = &out->candidates[0];
    out->session_id  = target->session_id;
    out->title       = target->title;
    out->project     = target->project;
    out->agent       = target->agent;
    out->first_ts_ms = target->first_ts_ms;
    out->last_ts_ms  = target->last_ts_ms;

    DigestRef *refs = NULL;
    uint32_t ref_cnt = 0, ref_cap = 0;
    collect_session_docs_for_digest(idx, &idx->base,  false, out->session_id, &idx->scratch, &refs, &ref_cnt, &ref_cap);
    collect_session_docs_for_digest(idx, &idx->delta, true,  out->session_id, &idx->scratch, &refs, &ref_cnt, &ref_cap);

    if (ref_cnt == 0) return -1;
    if (ref_cnt > 1) qsort(refs, ref_cnt, sizeof(DigestRef), cmp_digest_ref);
    out->first_ts_ms = refs[0].ts_ms;
    out->last_ts_ms  = refs[ref_cnt - 1].ts_ms;

    /* Pass 1: filter to dialogue turns (unless include_tools), collapse intermediate tools, dedup consecutive retries */
    DigestRef *kept = (DigestRef *)stelog_arena_alloc_aligned(&idx->scratch, ref_cnt * sizeof(DigestRef), 8);
    uint32_t *tools_before = (uint32_t *)stelog_arena_alloc_aligned(&idx->scratch, ref_cnt * sizeof(uint32_t), 8);
    uint32_t kept_cnt = 0;
    uint32_t tools_acc = 0;
    uint64_t prev_hash = 0;
    uint32_t prev_len = UINT32_MAX;
    uint8_t  prev_role = 255;

    for (uint32_t i = 0; i < ref_cnt; ++i) {
        const StelogSegment *seg = refs[i].seg;
        uint32_t d = refs[i].local_doc;
        StelogRoleKind rk = stelog_tag_role(seg->col_tag[d]);
        if (!opts->include_tools && !is_dialogue_role(rk)) {
            tools_acc++;
            out->total_tools_collapsed++;
            continue;
        }
        const char *txt = seg->text_base + seg->col_text_off[d];
        uint32_t tlen = seg->col_text_len[d];
        uint64_t h = stelog_wyhash(txt, tlen, (uint64_t)rk);
        if ((uint8_t)rk == prev_role && tlen == prev_len && h == prev_hash) {
            out->duplicates_skipped++;
            continue;
        }
        prev_role = (uint8_t)rk;
        prev_len = tlen;
        prev_hash = h;

        kept[kept_cnt] = refs[i];
        tools_before[kept_cnt] = tools_acc;
        tools_acc = 0;
        kept_cnt++;
    }

    out->total_dialogue_turns = kept_cnt;
    uint32_t off = opts->turn_offset;
    out->turn_offset = off;
    if (kept_cnt == 0 || off >= kept_cnt) {
        out->elapsed_ms = stelog_now_ms() - t0;
        return 0;
    }

    uint32_t budget = opts->max_total_bytes ? opts->max_total_bytes : 7200u;
    StelogHit *items = (StelogHit *)stelog_arena_alloc_aligned(&idx->scratch, kept_cnt * sizeof(StelogHit), 8);
    uint32_t emitted = digest_emit_forward(kept, tools_before, off, kept_cnt, budget, 0, &idx->scratch, items);

    if (off + emitted < kept_cnt) {
        out->truncated = true;
        if (off == 0 && kept_cnt >= 3) {
            /* Head + tail sampling: the opening request and the final conclusions are what readers need most,
               so the omitted span sits in the middle where it can be paged with turn_offset. */
            uint32_t head_budget = (budget * 2u) / 5u;
            uint32_t tail_budget = budget - head_budget;
            uint32_t head_n = digest_emit_forward(kept, tools_before, 0, kept_cnt - 1u, head_budget, 8u, &idx->scratch, items);

            StelogHit *tail_tmp = (StelogHit *)stelog_arena_alloc_aligned(&idx->scratch, kept_cnt * sizeof(StelogHit), 8);
            uint32_t tail_n = 0, used = 0;
            for (uint32_t i = kept_cnt; i > head_n; --i) {
                uint32_t k = i - 1u;
                if (used + 80u > tail_budget && tail_n > 0) break;
                const StelogSegment *seg = kept[k].seg;
                uint32_t d = kept[k].local_doc;
                StelogRoleKind rk = stelog_tag_role(seg->col_tag[d]);
                uint32_t rem_turns = k - head_n + 1u;
                if (rem_turns > 8u) rem_turns = 8u;
                uint32_t rem = tail_budget > used ? (tail_budget - used) : 0u;
                uint32_t cap = digest_item_cap(rk, rem, rem_turns);
                populate_hit(seg, kept[k].is_delta, d, 1.0f, 0, NULL, NULL, NULL, cap, &idx->scratch, &tail_tmp[tail_n]);
                tail_tmp[tail_n].collapsed_tools_before = tools_before[k];
                used += tail_tmp[tail_n].snippet.len + 72u;
                tail_n++;
            }
            for (uint32_t j = 0; j < tail_n; ++j) items[head_n + j] = tail_tmp[tail_n - 1u - j];
            emitted = head_n + tail_n;
            uint32_t tail_start = kept_cnt - tail_n;
            out->gap_item_index = head_n;
            out->omitted_begin  = head_n;
            out->omitted_count  = tail_start > head_n ? tail_start - head_n : 0u;
            out->next_offset    = head_n;
            if (out->omitted_count == 0) {
                out->truncated = false;
                out->gap_item_index = 0;
                out->next_offset = 0;
            }
        } else {
            out->gap_item_index = emitted;
            out->omitted_begin  = off + emitted;
            out->omitted_count  = kept_cnt - (off + emitted);
            out->next_offset    = off + emitted;
        }
    }

    out->items = items;
    out->count = emitted;
    out->elapsed_ms = stelog_now_ms() - t0;
    return 0;
}

int stelog_index_digest(StelogIndex *idx,
                        const char *session_prefix,
                        uint32_t max_total_bytes,
                        bool include_tools,
                        StelogDigestResult *out) {
    StelogDigestOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.session = session_prefix;
    opts.max_total_bytes = max_total_bytes;
    opts.include_tools = include_tools;
    return stelog_index_digest_ex(idx, &opts, out);
}

/* ============================================================================
 * Single Document / O(1) pread Raw JSONL Retrieval
 * ============================================================================ */

static bool segment_find_doc_key(const StelogSegment *seg, uint64_t key, uint32_t *out_doc) {
    if (!seg->loaded || seg->doc_count == 0) return false;
    uint32_t lo = 0, hi = seg->doc_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (seg->col_doc_key[seg->col_key_order[mid]] < key) lo = mid + 1u;
        else hi = mid;
    }
    if (lo >= seg->doc_count || seg->col_doc_key[seg->col_key_order[lo]] != key) return false;
    *out_doc = seg->col_key_order[lo];
    return true;
}

bool stelog_index_resolve_doc_id(const StelogIndex *idx, uint64_t doc_id, uint64_t *out_internal_id) {
    if (!idx || doc_id == 0 || doc_id > STELOG_DOC_KEY_MASK) return false;
    uint32_t d;
    /* Delta first: a re-ingested transcript lives there while its Base copy is tombstoned. */
    if (segment_find_doc_key(&idx->delta, doc_id, &d)) {
        if (out_internal_id) *out_internal_id = stelog_make_doc_id(true, d);
        return true;
    }
    if (segment_find_doc_key(&idx->base, doc_id, &d) &&
        !stelog_index_base_file_tombstoned(idx, idx->base.col_file_id[d])) {
        if (out_internal_id) *out_internal_id = stelog_make_doc_id(false, d);
        return true;
    }
    return false;
}

int stelog_index_get_document(StelogIndex *idx,
                              uint64_t doc_id,
                              bool raw_jsonl,
                              uint32_t byte_offset,
                              uint32_t max_bytes,
                              bool tail_mode,
                              StelogDocResult *out) {
    if (!idx || !out) return -1;
    memset(out, 0, sizeof(*out));
    double t0 = stelog_now_ms();
    stelog_arena_reset(&idx->scratch);

    uint64_t internal_id;
    if (!stelog_index_resolve_doc_id(idx, doc_id, &internal_id)) return -1;
    bool is_delta = stelog_doc_is_delta(internal_id);
    uint32_t d = stelog_doc_local_id(internal_id);
    const StelogSegment *seg = is_delta ? &idx->delta : &idx->base;
    if (!seg->loaded || d >= seg->doc_count) return -1;

    uint32_t sid = seg->col_session_id[d];
    const StelogSessionRecord *sr = &seg->sessions[sid];
    uint32_t fid = seg->col_file_id[d];

    out->doc_id     = doc_id;
    out->ts_ms      = seg->col_ts_ms[d];
    out->tag        = seg->col_tag[d];
    out->session_id = stelog_seg_str(seg, sr->session_id);
    out->title      = stelog_seg_str(seg, sr->title);
    out->project    = stelog_seg_str(seg, seg->projects[seg->col_project_id[d]]);
    out->actor      = stelog_seg_str(seg, seg->actors[seg->col_actor_id[d]]);
    out->raw_offset = seg->col_raw_off[d];
    out->raw_len    = seg->col_raw_len[d];
    if (fid < seg->file_count) {
        out->file_path = stelog_seg_str(seg, seg->files[fid].path);
    }

    uint32_t limit = max_bytes ? max_bytes : 32768u;
    const char *src = NULL;
    uint32_t total = 0;

    if (!raw_jsonl) {
        src = seg->text_base + seg->col_text_off[d];
        total = seg->col_text_len[d];
    } else {
        if (fid >= seg->file_count || seg->files[fid].is_zstd || out->raw_len == 0) return -1;
        int fd = open(out->file_path.ptr, O_RDONLY);
        if (fd < 0) return -1;
        total = out->raw_len;
        uint32_t off = tail_mode ? (total > limit ? total - limit : 0u) : byte_offset;
        if (off > total) off = total;
        uint32_t to_read = (total - off) < limit ? (total - off) : limit;
        char *buf = (char *)stelog_arena_alloc_aligned(&idx->scratch, to_read + 1, 1);
        if (!buf) { close(fd); return -1; }
        ssize_t n = pread(fd, buf, to_read, (off_t)(out->raw_offset + off));
        close(fd);
        if (n < 0) return -1;
        buf[n] = '\0';
        out->content = (StelogStr){ buf, (uint32_t)n };
        out->total_bytes = total;
        out->returned_offset = off;
        out->truncated = (off > 0) || (off + (uint32_t)n < total);
        out->elapsed_ms = stelog_now_ms() - t0;
        return 0;
    }

    uint32_t off = tail_mode ? (total > limit ? total - limit : 0u) : byte_offset;
    if (off > total) off = total;
    size_t start = utf8_ceil(src, total, off);
    size_t end = start + limit;
    if (end > total) end = total;
    end = utf8_floor(src, end);
    if (end < start) end = start;

    out->content = (StelogStr){ src + start, (uint32_t)(end - start) };
    out->total_bytes = total;
    out->returned_offset = (uint32_t)start;
    out->truncated = (start > 0) || (end < total);
    out->elapsed_ms = stelog_now_ms() - t0;
    return 0;
}

/* ============================================================================
 * List Sessions Across Base + Delta
 * ============================================================================ */

static int cmp_session_last_ts_desc(const void *a, const void *b) {
    const StelogSessionSummary *sa = (const StelogSessionSummary *)a;
    const StelogSessionSummary *sb = (const StelogSessionSummary *)b;
    if (sa->last_ts_ms > sb->last_ts_ms) return -1;
    if (sa->last_ts_ms < sb->last_ts_ms) return 1;
    return 0;
}

static void collect_segment_sessions(const StelogSegment *seg,
                                     uint32_t agent_mask,
                                     const char *project_substr,
                                     int64_t since_ms,
                                     int64_t until_ms,
                                     StrMap *sidmap,
                                     StelogSessionSummary *arr,
                                     uint32_t *io_count) {
    if (!seg || !seg->loaded) return;
    size_t plen = project_substr ? strlen(project_substr) : 0;

    for (uint32_t s = 0; s < seg->session_count; ++s) {
        const StelogSessionRecord *sr = &seg->sessions[s];
        if (sr->doc_count == 0) continue;
        if (agent_mask && !(agent_mask & (1u << sr->agent_kind))) continue;
        if (since_ms > 0 && sr->last_ts_ms < since_ms) continue;
        if (until_ms > 0 && sr->first_ts_ms > until_ms) continue;

        StelogStr proj = stelog_seg_str(seg, sr->cwd);
        if (plen > 0 && (!proj.len || !stelog_memcasemem(proj.ptr, proj.len, project_substr, plen))) continue;

        StelogStr sid = stelog_seg_str(seg, sr->session_id);
        bool inserted = false;
        uint32_t i = strmap_get_or_put(sidmap, sid, *io_count, &inserted);
        if (!inserted) {
            if (sr->first_ts_ms < arr[i].first_ts_ms) arr[i].first_ts_ms = sr->first_ts_ms;
            if (sr->last_ts_ms > arr[i].last_ts_ms) arr[i].last_ts_ms = sr->last_ts_ms;
            arr[i].doc_count += sr->doc_count;
            if (arr[i].title.len == 0 && sr->title.len > 0) arr[i].title = stelog_seg_str(seg, sr->title);
            continue;
        }

        StelogSessionSummary *dst = &arr[(*io_count)++];
        dst->session_id        = sid;
        dst->title             = stelog_seg_str(seg, sr->title);
        dst->project           = proj;
        dst->git_branch        = stelog_seg_str(seg, sr->git_branch);
        dst->parent_session_id = stelog_seg_str(seg, sr->parent_session_id);
        dst->first_ts_ms       = sr->first_ts_ms;
        dst->last_ts_ms        = sr->last_ts_ms;
        dst->doc_count         = sr->doc_count;
        dst->agent             = (StelogAgentKind)sr->agent_kind;
        dst->flags             = sr->flags;
    }
}

int stelog_index_list_sessions(StelogIndex *idx,
                               uint32_t agent_mask,
                               const char *project_substr,
                               int64_t since_ms,
                               int64_t until_ms,
                               uint32_t limit,
                               uint32_t offset,
                               StelogSessionListResult *out) {
    if (!idx || !out) return -1;
    memset(out, 0, sizeof(*out));
    double t0 = stelog_now_ms();
    stelog_arena_reset(&idx->scratch);

    uint32_t max_sess = (idx->base.loaded ? idx->base.session_count : 0u) +
                        (idx->delta.loaded ? idx->delta.session_count : 0u);
    if (max_sess == 0) {
        out->elapsed_ms = stelog_now_ms() - t0;
        return 0;
    }

    StelogSessionSummary *all = (StelogSessionSummary *)stelog_arena_alloc_aligned(
        &idx->scratch, max_sess * sizeof(StelogSessionSummary), 8);
    if (!all) return -1;

    uint32_t count = 0;
    StrMap sidmap;
    strmap_init(&sidmap, &idx->scratch, max_sess);
    collect_segment_sessions(&idx->base,  agent_mask, project_substr, since_ms, until_ms, &sidmap, all, &count);
    collect_segment_sessions(&idx->delta, agent_mask, project_substr, since_ms, until_ms, &sidmap, all, &count);

    out->total_matches = count;
    if (count == 0 || offset >= count) {
        out->elapsed_ms = stelog_now_ms() - t0;
        return 0;
    }

    qsort(all, count, sizeof(StelogSessionSummary), cmp_session_last_ts_desc);
    uint32_t lim = limit ? limit : 20u;
    uint32_t avail = count - offset;
    uint32_t take = avail < lim ? avail : lim;

    out->items = all + offset;
    out->count = take;
    out->elapsed_ms = stelog_now_ms() - t0;
    return 0;
}
