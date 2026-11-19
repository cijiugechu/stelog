#include "store/roaring.h"

#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define STELOG_HAS_NEON 1
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define STELOG_HAS_SSE2 1
#endif

/* ============================================================================
 * Roaring 64K-Chunk Encoder
 * ============================================================================ */

static uint32_t count_runs_u16(const uint32_t *docs, uint32_t start, uint32_t end) {
    if (start >= end) return 0;
    uint32_t runs = 1;
    uint16_t prev = (uint16_t)(docs[start] & 0xFFFFu);
    for (uint32_t i = start + 1; i < end; ++i) {
        uint16_t cur = (uint16_t)(docs[i] & 0xFFFFu);
        if (cur != (uint16_t)(prev + 1u)) {
            runs++;
        }
        prev = cur;
    }
    return runs;
}

int stelog_roaring_encode(const uint32_t *docs,
                          uint32_t count,
                          uint8_t **out_buf,
                          size_t *out_len) {
    if (count == 0) {
        uint8_t *buf = (uint8_t *)calloc(1, sizeof(StelogRoaringHeader));
        if (!buf) return -1;
        *out_buf = buf;
        *out_len = sizeof(StelogRoaringHeader);
        return 0;
    }

    /* Pass 1: count chunks and compute exact payload size */
    uint32_t num_chunks = 0;
    uint32_t i = 0;
    while (i < count) {
        uint16_t key = (uint16_t)(docs[i] >> 16);
        num_chunks++;
        while (i < count && (uint16_t)(docs[i] >> 16) == key) i++;
    }

    StelogRoaringChunkMeta *metas = (StelogRoaringChunkMeta *)calloc(num_chunks, sizeof(StelogRoaringChunkMeta));
    uint32_t *chunk_starts = (uint32_t *)malloc((num_chunks + 1u) * sizeof(uint32_t));
    if (!metas || !chunk_starts) {
        free(metas);
        free(chunk_starts);
        return -1;
    }

    uint32_t payload_bytes = 0;
    uint32_t c = 0;
    i = 0;
    while (i < count) {
        uint16_t key = (uint16_t)(docs[i] >> 16);
        uint32_t start = i;
        while (i < count && (uint16_t)(docs[i] >> 16) == key) i++;
        uint32_t card = i - start;
        uint32_t runs = count_runs_u16(docs, start, i);

        uint32_t array_sz  = card * 2u;
        uint32_t run_sz    = runs * 4u;
        uint32_t bitset_sz = 8192u;

        StelogRoaringKind kind;
        uint32_t chosen_sz;
        if (run_sz < array_sz && run_sz < bitset_sz && runs <= 0xFFFFu) {
            kind = STELOG_ROARING_RUN16;
            chosen_sz = run_sz;
        } else if (array_sz <= bitset_sz) {
            kind = STELOG_ROARING_ARRAY16;
            chosen_sz = array_sz;
        } else {
            kind = STELOG_ROARING_BITSET8K;
            chosen_sz = bitset_sz;
        }

        /* 8-byte align bitset chunks for fast SIMD / uint64_t loads */
        if (kind == STELOG_ROARING_BITSET8K) {
            payload_bytes = (uint32_t)stelog_align8(payload_bytes);
        } else {
            payload_bytes = (payload_bytes + 3u) & ~3u;
        }

        metas[c].key = key;
        metas[c].kind = (uint8_t)kind;
        metas[c].card_minus_1 = (uint16_t)(card - 1u);
        metas[c].run_count = (kind == STELOG_ROARING_RUN16) ? (uint16_t)runs : 0u;
        metas[c].data_off = payload_bytes;
        chunk_starts[c] = start;
        payload_bytes += chosen_sz;
        c++;
    }
    chunk_starts[num_chunks] = count;

    uint64_t meta_bytes = sizeof(StelogRoaringHeader) + (uint64_t)num_chunks * sizeof(StelogRoaringChunkMeta);
    uint64_t data_base = stelog_align8(meta_bytes);
    uint64_t total_size = stelog_align8(data_base + payload_bytes);

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)total_size);
    if (!buf) {
        free(metas);
        free(chunk_starts);
        return -1;
    }

    StelogRoaringHeader *hdr = (StelogRoaringHeader *)buf;
    hdr->total_card = count;
    hdr->num_chunks = num_chunks;
    memcpy(buf + sizeof(StelogRoaringHeader), metas, (size_t)num_chunks * sizeof(StelogRoaringChunkMeta));

    uint8_t *payload = buf + data_base;
    for (uint32_t ci = 0; ci < num_chunks; ++ci) {
        uint32_t start = chunk_starts[ci];
        uint32_t end = chunk_starts[ci + 1];
        uint8_t *dst = payload + metas[ci].data_off;

        if (metas[ci].kind == STELOG_ROARING_ARRAY16) {
            uint16_t *arr = (uint16_t *)dst;
            for (uint32_t k = start; k < end; ++k) {
                arr[k - start] = (uint16_t)(docs[k] & 0xFFFFu);
            }
        } else if (metas[ci].kind == STELOG_ROARING_BITSET8K) {
            uint64_t *bits = (uint64_t *)dst;
            for (uint32_t k = start; k < end; ++k) {
                uint16_t lo = (uint16_t)(docs[k] & 0xFFFFu);
                bits[lo >> 6] |= (1ull << (lo & 63u));
            }
        } else { /* RUN16 */
            uint16_t *runs = (uint16_t *)dst;
            uint32_t ridx = 0;
            uint16_t rstart = (uint16_t)(docs[start] & 0xFFFFu);
            uint16_t rprev = rstart;
            for (uint32_t k = start + 1; k < end; ++k) {
                uint16_t cur = (uint16_t)(docs[k] & 0xFFFFu);
                if (cur != (uint16_t)(rprev + 1u)) {
                    runs[ridx * 2u] = rstart;
                    runs[ridx * 2u + 1u] = (uint16_t)(rprev - rstart);
                    ridx++;
                    rstart = cur;
                }
                rprev = cur;
            }
            runs[ridx * 2u] = rstart;
            runs[ridx * 2u + 1u] = (uint16_t)(rprev - rstart);
        }
    }

    free(metas);
    free(chunk_starts);
    *out_buf = buf;
    *out_len = (size_t)total_size;
    return 0;
}

/* ============================================================================
 * Roaring Zero-Copy Membership & Range-Pruned SIMD Intersection
 * ============================================================================ */

static inline const StelogRoaringChunkMeta *roaring_chunks(const uint8_t *blob, uint32_t *out_n, const uint8_t **out_data) {
    const StelogRoaringHeader *h = (const StelogRoaringHeader *)blob;
    *out_n = h->num_chunks;
    uint64_t meta_bytes = sizeof(StelogRoaringHeader) + (uint64_t)h->num_chunks * sizeof(StelogRoaringChunkMeta);
    *out_data = blob + stelog_align8(meta_bytes);
    return (const StelogRoaringChunkMeta *)(blob + sizeof(StelogRoaringHeader));
}

static const StelogRoaringChunkMeta *find_chunk(const StelogRoaringChunkMeta *chunks, uint32_t n, uint16_t key) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (chunks[mid].key < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo < n && chunks[lo].key == key) return &chunks[lo];
    return NULL;
}

static bool chunk_contains_u16(const StelogRoaringChunkMeta *cm, const uint8_t *data_base, uint16_t v) {
    const uint8_t *p = data_base + cm->data_off;
    if (cm->kind == STELOG_ROARING_BITSET8K) {
        const uint64_t *bits = (const uint64_t *)p;
        return (bits[v >> 6] & (1ull << (v & 63u))) != 0;
    } else if (cm->kind == STELOG_ROARING_ARRAY16) {
        const uint16_t *arr = (const uint16_t *)p;
        uint32_t lo = 0, hi = (uint32_t)cm->card_minus_1 + 1u;
        while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            if (arr[mid] < v) lo = mid + 1;
            else hi = mid;
        }
        return lo < ((uint32_t)cm->card_minus_1 + 1u) && arr[lo] == v;
    } else { /* RUN16 */
        const uint16_t *runs = (const uint16_t *)p;
        uint32_t lo = 0, hi = cm->run_count;
        while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            uint16_t rstart = runs[mid * 2u];
            uint16_t rend = (uint16_t)(rstart + runs[mid * 2u + 1u]);
            if (rend < v) lo = mid + 1;
            else if (v < rstart) hi = mid;
            else return true;
        }
        return false;
    }
}

bool stelog_docset_contains(const StelogDocSet *ds, uint32_t doc_id) {
    if (!ds) return false;
    switch (ds->kind) {
        case STELOG_DOCSET_EMPTY:
            return false;
        case STELOG_DOCSET_INLINE1:
            return ds->inline_doc == doc_id;
        case STELOG_DOCSET_COMPACT: {
            uint32_t lo = 0, hi = ds->card;
            while (lo < hi) {
                uint32_t mid = lo + ((hi - lo) >> 1);
                if (ds->compact_docs[mid] < doc_id) lo = mid + 1;
                else hi = mid;
            }
            return lo < ds->card && ds->compact_docs[lo] == doc_id;
        }
        case STELOG_DOCSET_ROARING: {
            uint32_t n = 0;
            const uint8_t *data = NULL;
            const StelogRoaringChunkMeta *chunks = roaring_chunks(ds->roaring_blob, &n, &data);
            const StelogRoaringChunkMeta *cm = find_chunk(chunks, n, (uint16_t)(doc_id >> 16));
            if (!cm) return false;
            return chunk_contains_u16(cm, data, (uint16_t)(doc_id & 0xFFFFu));
        }
        default:
            return false;
    }
}

bool stelog_docset_any_contains(const StelogDocSet *sets, uint32_t count, uint32_t doc_id) {
    for (uint32_t i = 0; i < count; ++i) {
        if (stelog_docset_contains(&sets[i], doc_id)) return true;
    }
    return false;
}

/* Emit all doc_ids in a single Roaring chunk that lie within [lo_u16, hi_u16] */
static uint32_t emit_chunk_range(const StelogRoaringChunkMeta *cm,
                                 const uint8_t *data_base,
                                 uint32_t key_hi,
                                 uint16_t lo_u16,
                                 uint16_t hi_u16,
                                 uint32_t *out) {
    const uint8_t *p = data_base + cm->data_off;
    uint32_t n = 0;

    if (cm->kind == STELOG_ROARING_ARRAY16) {
        const uint16_t *arr = (const uint16_t *)p;
        uint32_t card = (uint32_t)cm->card_minus_1 + 1u;
        uint32_t lo = 0, hi = card;
        while (lo < hi) {
            uint32_t mid = lo + ((hi - lo) >> 1);
            if (arr[mid] < lo_u16) lo = mid + 1;
            else hi = mid;
        }
        for (uint32_t i = lo; i < card && arr[i] <= hi_u16; ++i) {
            out[n++] = key_hi | arr[i];
        }
    } else if (cm->kind == STELOG_ROARING_BITSET8K) {
        const uint64_t *bits = (const uint64_t *)p;
        uint32_t w_lo = lo_u16 >> 6;
        uint32_t w_hi = hi_u16 >> 6;
        for (uint32_t w = w_lo; w <= w_hi; ++w) {
            uint64_t word = bits[w];
            if (w == w_lo) word &= (~0ull << (lo_u16 & 63u));
            if (w == w_hi && (hi_u16 & 63u) < 63u) word &= ((1ull << ((hi_u16 & 63u) + 1u)) - 1ull);
            while (word) {
                unsigned bit = (unsigned)__builtin_ctzll(word);
                out[n++] = key_hi | ((w << 6) + bit);
                word &= (word - 1ull);
            }
        }
    } else { /* RUN16 */
        const uint16_t *runs = (const uint16_t *)p;
        for (uint32_t r = 0; r < cm->run_count; ++r) {
            uint32_t rs = runs[r * 2u];
            uint32_t re = rs + runs[r * 2u + 1u];
            if (re < lo_u16) continue;
            if (rs > hi_u16) break;
            if (rs < lo_u16) rs = lo_u16;
            if (re > hi_u16) re = hi_u16;
            for (uint32_t v = rs; v <= re; ++v) {
                out[n++] = key_hi | v;
            }
        }
    }
    return n;
}

/* SIMD bitset-bitset AND for two 8KB BITSET8K chunks within [lo_u16, hi_u16] */
static uint32_t intersect_two_bitsets(const uint64_t *a,
                                      const uint64_t *b,
                                      uint32_t key_hi,
                                      uint16_t lo_u16,
                                      uint16_t hi_u16,
                                      uint32_t *out) {
    uint64_t tmp[1024];
    uint32_t w_lo = lo_u16 >> 6;
    uint32_t w_hi = hi_u16 >> 6;
    uint32_t w = w_lo;

#if defined(STELOG_HAS_NEON)
    for (; w + 2 <= w_hi + 1; w += 2) {
        uint64x2_t va = vld1q_u64(a + w);
        uint64x2_t vb = vld1q_u64(b + w);
        vst1q_u64(tmp + w, vandq_u64(va, vb));
    }
#elif defined(STELOG_HAS_SSE2)
    for (; w + 2 <= w_hi + 1; w += 2) {
        __m128i va = _mm_loadu_si128((const __m128i *)(a + w));
        __m128i vb = _mm_loadu_si128((const __m128i *)(b + w));
        _mm_storeu_si128((__m128i *)(tmp + w), _mm_and_si128(va, vb));
    }
#endif
    for (; w <= w_hi; ++w) {
        tmp[w] = a[w] & b[w];
    }

    uint32_t n = 0;
    for (uint32_t i = w_lo; i <= w_hi; ++i) {
        uint64_t word = tmp[i];
        if (i == w_lo) word &= (~0ull << (lo_u16 & 63u));
        if (i == w_hi && (hi_u16 & 63u) < 63u) word &= ((1ull << ((hi_u16 & 63u) + 1u)) - 1ull);
        while (word) {
            unsigned bit = (unsigned)__builtin_ctzll(word);
            out[n++] = key_hi | ((i << 6) + bit);
            word &= (word - 1ull);
        }
    }
    return n;
}

uint32_t stelog_docset_intersect(const StelogDocSet *sets,
                                 uint32_t set_count,
                                 uint32_t lo_doc,
                                 uint32_t hi_doc,
                                 StelogArena *scratch,
                                 uint32_t **out_docs) {
    *out_docs = NULL;
    if (set_count == 0 || lo_doc > hi_doc) return 0;

    /* Copy & sort indices by ascending cardinality (smallest set drives intersection!) */
    StelogDocSet ordered[64];
    if (set_count > 64) set_count = 64;
    for (uint32_t i = 0; i < set_count; ++i) {
        if (sets[i].kind == STELOG_DOCSET_EMPTY || sets[i].card == 0) return 0;
        ordered[i] = sets[i];
    }
    for (uint32_t i = 1; i < set_count; ++i) {
        StelogDocSet key = ordered[i];
        uint32_t j = i;
        while (j > 0 && ordered[j - 1].card > key.card) {
            ordered[j] = ordered[j - 1];
            j--;
        }
        ordered[j] = key;
    }

    const StelogDocSet *driver = &ordered[0];

    /* Fast path 1: driver is DF == 1 inline */
    if (driver->kind == STELOG_DOCSET_INLINE1) {
        uint32_t d = driver->inline_doc;
        if (d < lo_doc || d > hi_doc) return 0;
        for (uint32_t s = 1; s < set_count; ++s) {
            if (!stelog_docset_contains(&ordered[s], d)) return 0;
        }
        uint32_t *res = (uint32_t *)stelog_arena_alloc(scratch, sizeof(uint32_t));
        if (!res) return 0;
        res[0] = d;
        *out_docs = res;
        return 1;
    }

    /* Fast path 2: driver is compact uint32_t[] (DF < 64) */
    if (driver->kind == STELOG_DOCSET_COMPACT) {
        uint32_t *res = (uint32_t *)stelog_arena_alloc(scratch, driver->card * sizeof(uint32_t));
        if (!res) return 0;
        uint32_t n = 0;
        for (uint32_t i = 0; i < driver->card; ++i) {
            uint32_t d = driver->compact_docs[i];
            if (d < lo_doc) continue;
            if (d > hi_doc) break;
            bool ok = true;
            for (uint32_t s = 1; s < set_count; ++s) {
                if (!stelog_docset_contains(&ordered[s], d)) {
                    ok = false;
                    break;
                }
            }
            if (ok) res[n++] = d;
        }
        *out_docs = res;
        return n;
    }

    /* General Roaring path: range-pruned by [lo_doc >> 16, hi_doc >> 16], SIMD bitset AND when applicable */
    uint32_t d_n = 0;
    const uint8_t *d_data = NULL;
    const StelogRoaringChunkMeta *d_chunks = roaring_chunks(driver->roaring_blob, &d_n, &d_data);

    uint16_t lo_key = (uint16_t)(lo_doc >> 16);
    uint16_t hi_key = (uint16_t)(hi_doc >> 16);

    /* Upper bound on result size is min(driver->card, hi_doc - lo_doc + 1) */
    uint32_t range_cap = hi_doc - lo_doc + 1u;
    uint32_t max_out = driver->card < range_cap ? driver->card : range_cap;
    uint32_t *res = (uint32_t *)stelog_arena_alloc(scratch, (size_t)max_out * sizeof(uint32_t));
    if (!res) return 0;

    uint32_t out_n = 0;
    uint32_t chunk_tmp[65536];

    /* Binary search first driver chunk with key >= lo_key */
    uint32_t ci = 0, chi = d_n;
    while (ci < chi) {
        uint32_t mid = ci + ((chi - ci) >> 1);
        if (d_chunks[mid].key < lo_key) ci = mid + 1;
        else chi = mid;
    }

    for (; ci < d_n && d_chunks[ci].key <= hi_key; ++ci) {
        uint16_t key = d_chunks[ci].key;
        uint32_t key_hi = ((uint32_t)key) << 16;
        uint16_t c_lo = (key == lo_key) ? (uint16_t)(lo_doc & 0xFFFFu) : 0u;
        uint16_t c_hi = (key == hi_key) ? (uint16_t)(hi_doc & 0xFFFFu) : 0xFFFFu;

        /* Pre-check if every other Roaring set has this key (O(log chunks) skip of entire 64K block!) */
        const StelogRoaringChunkMeta *other_cm[64];
        const uint8_t *other_data[64];
        bool all_have_key = true;
        for (uint32_t s = 1; s < set_count; ++s) {
            other_cm[s] = NULL;
            other_data[s] = NULL;
            if (ordered[s].kind != STELOG_DOCSET_ROARING) continue;
            uint32_t on = 0;
            const StelogRoaringChunkMeta *och = roaring_chunks(ordered[s].roaring_blob, &on, &other_data[s]);
            other_cm[s] = find_chunk(och, on, key);
            if (!other_cm[s]) {
                all_have_key = false;
                break;
            }
        }
        if (!all_have_key) continue;

        uint32_t cn = 0;
        uint32_t start_filter = 1;

        /* Fast SIMD path: if driver & second set are both BITSET8K, SIMD AND directly! */
        if (set_count >= 2 &&
            ordered[1].kind == STELOG_DOCSET_ROARING &&
            other_cm[1] != NULL &&
            d_chunks[ci].kind == STELOG_ROARING_BITSET8K &&
            other_cm[1]->kind == STELOG_ROARING_BITSET8K) {
            const uint64_t *ba = (const uint64_t *)(d_data + d_chunks[ci].data_off);
            const uint64_t *bb = (const uint64_t *)(other_data[1] + other_cm[1]->data_off);
            cn = intersect_two_bitsets(ba, bb, key_hi, c_lo, c_hi, chunk_tmp);
            start_filter = 2;
        } else {
            cn = emit_chunk_range(&d_chunks[ci], d_data, key_hi, c_lo, c_hi, chunk_tmp);
        }

        for (uint32_t i = 0; i < cn; ++i) {
            uint32_t d = chunk_tmp[i];
            uint16_t u16 = (uint16_t)(d & 0xFFFFu);
            bool ok = true;
            for (uint32_t s = start_filter; s < set_count; ++s) {
                if (ordered[s].kind == STELOG_DOCSET_ROARING) {
                    if (!chunk_contains_u16(other_cm[s], other_data[s], u16)) {
                        ok = false;
                        break;
                    }
                } else {
                    if (!stelog_docset_contains(&ordered[s], d)) {
                        ok = false;
                        break;
                    }
                }
            }
            if (ok) res[out_n++] = d;
        }
    }

    *out_docs = res;
    return out_n;
}
