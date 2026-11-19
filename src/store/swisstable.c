#include "store/swisstable.h"

#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define STELOG_HAS_NEON 1
#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define STELOG_HAS_SSE2 1
#endif

int stelog_swiss_build(const StelogTermSlot *terms,
                       uint32_t term_count,
                       uint8_t **out_buf,
                       size_t *out_len) {
    /* Load factor <= 75% (max 12 occupied per 16-slot group) */
    uint32_t min_slots = (term_count * 4u) / 3u + 16u;
    uint32_t num_groups = 1u;
    while (num_groups * 16u < min_slots) {
        num_groups <<= 1u;
    }
    uint32_t total_slots = num_groups * 16u;

    uint64_t hdr_size = stelog_align64(sizeof(StelogSwissHeader));
    uint64_t ctrl_bytes = stelog_align64((uint64_t)total_slots + 16u);
    uint64_t slots_bytes = (uint64_t)total_slots * sizeof(StelogTermSlot);
    uint64_t total_bytes = hdr_size + ctrl_bytes + slots_bytes;

    uint8_t *buf = (uint8_t *)calloc(1, (size_t)total_bytes);
    if (!buf) return -1;

    StelogSwissHeader *hdr = (StelogSwissHeader *)buf;
    hdr->magic = STELOG_INDEX_MAGIC;
    hdr->format_version = STELOG_FORMAT_VERSION;
    hdr->term_count = term_count;
    hdr->num_groups = num_groups;
    hdr->ctrl_off = hdr_size;
    hdr->slots_off = hdr_size + ctrl_bytes;

    uint8_t *ctrl = buf + hdr->ctrl_off;
    StelogTermSlot *slots = (StelogTermSlot *)(buf + hdr->slots_off);
    memset(ctrl, STELOG_SWISS_EMPTY, (size_t)total_slots + 16u);

    uint32_t group_mask = num_groups - 1u;
    for (uint32_t i = 0; i < term_count; ++i) {
        uint64_t h = terms[i].term_hash;
        uint8_t h2 = (uint8_t)(h & 0x7Fu);
        uint32_t g = (uint32_t)((h >> 7) & group_mask);
        uint32_t step = 0;
        for (;;) {
            uint32_t base = g * 16u;
            uint32_t placed = UINT32_MAX;
            for (uint32_t k = 0; k < 16u; ++k) {
                if (ctrl[base + k] == STELOG_SWISS_EMPTY) {
                    placed = base + k;
                    break;
                }
            }
            if (placed != UINT32_MAX) {
                ctrl[placed] = h2;
                slots[placed] = terms[i];
                break;
            }
            step++;
            g = (g + step) & group_mask; /* triangular probing guarantees visiting every group */
        }
    }

    *out_buf = buf;
    *out_len = (size_t)total_bytes;
    return 0;
}

int stelog_swiss_open(const uint8_t *data, size_t len, StelogSwissView *out) {
    memset(out, 0, sizeof(*out));
    if (!data || len < sizeof(StelogSwissHeader)) return -1;
    const StelogSwissHeader *hdr = (const StelogSwissHeader *)data;
    if (hdr->magic != STELOG_INDEX_MAGIC || hdr->format_version != STELOG_FORMAT_VERSION) return -1;
    if (hdr->num_groups == 0 || (hdr->num_groups & (hdr->num_groups - 1u)) != 0) return -1;

    uint64_t total_slots = (uint64_t)hdr->num_groups * 16u;
    if (hdr->ctrl_off + total_slots + 16u > len) return -1;
    if (hdr->slots_off + total_slots * sizeof(StelogTermSlot) > len) return -1;

    out->ctrl = data + hdr->ctrl_off;
    out->slots = (const StelogTermSlot *)(data + hdr->slots_off);
    out->group_mask = hdr->num_groups - 1u;
    out->term_count = hdr->term_count;
    return 0;
}

#if defined(STELOG_HAS_NEON)
/* Extract 16-bit mask from 16-byte NEON comparison result (0xFF -> 1, 0x00 -> 0) */
static inline uint16_t neon_movemask_u8(uint8x16_t cmp) {
    static const uint8_t kBits[16] = {
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128
    };
    uint8x16_t masked = vandq_u8(cmp, vld1q_u8(kBits));
    uint8x8_t lo = vget_low_u8(masked);
    uint8x8_t hi = vget_high_u8(masked);
    uint8_t m_lo = vaddv_u8(lo);
    uint8_t m_hi = vaddv_u8(hi);
    return (uint16_t)m_lo | ((uint16_t)m_hi << 8);
}
#endif

const StelogTermSlot *stelog_swiss_find(const StelogSwissView *view, uint64_t term_hash) {
    if (!view || !view->ctrl || view->term_count == 0) return NULL;

    uint8_t h2 = (uint8_t)(term_hash & 0x7Fu);
    uint32_t g = (uint32_t)((term_hash >> 7) & view->group_mask);
    uint32_t step = 0;

#if defined(STELOG_HAS_NEON)
    uint8x16_t vh2 = vdupq_n_u8(h2);
    uint8x16_t vempty = vdupq_n_u8(STELOG_SWISS_EMPTY);
#elif defined(STELOG_HAS_SSE2)
    __m128i vh2 = _mm_set1_epi8((char)h2);
    __m128i vempty = _mm_set1_epi8((char)STELOG_SWISS_EMPTY);
#endif

    for (;;) {
        uint32_t base = g * 16u;
        const uint8_t *grp = view->ctrl + base;

        uint16_t match_mask = 0;
        uint16_t empty_mask = 0;

#if defined(STELOG_HAS_NEON)
        uint8x16_t vctrl = vld1q_u8(grp);
        match_mask = neon_movemask_u8(vceqq_u8(vctrl, vh2));
        empty_mask = neon_movemask_u8(vceqq_u8(vctrl, vempty));
#elif defined(STELOG_HAS_SSE2)
        __m128i vctrl = _mm_loadu_si128((const __m128i *)grp);
        match_mask = (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(vctrl, vh2));
        empty_mask = (uint16_t)_mm_movemask_epi8(_mm_cmpeq_epi8(vctrl, vempty));
#else
        for (uint32_t k = 0; k < 16u; ++k) {
            if (grp[k] == h2) match_mask |= (uint16_t)(1u << k);
            if (grp[k] == STELOG_SWISS_EMPTY) empty_mask |= (uint16_t)(1u << k);
        }
#endif

        while (match_mask) {
            unsigned bit = (unsigned)__builtin_ctz((unsigned)match_mask);
            const StelogTermSlot *slot = &view->slots[base + bit];
            if (slot->term_hash == term_hash) {
                return slot;
            }
            match_mask &= (uint16_t)(match_mask - 1u);
        }

        if (empty_mask) {
            return NULL;
        }
        step++;
        if (step > view->group_mask) return NULL;
        g = (g + step) & view->group_mask;
    }
}
