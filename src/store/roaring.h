#ifndef STELOG_STORE_ROARING_H
#define STELOG_STORE_ROARING_H

#include "base/arena.h"
#include "store/format.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize a strictly sorted, unique array of uint32_t doc_ids into a compact
 * Roaring 64K-chunk binary blob (choosing ARRAY16 / BITSET8K / RUN16 per chunk).
 * Allocates output buffer via malloc(); caller frees *out_buf.
 */
int stelog_roaring_encode(const uint32_t *docs,
                          uint32_t count,
                          uint8_t **out_buf,
                          size_t *out_len);

/* Unified zero-copy posting/bitmap set abstraction for query intersection */
typedef enum {
    STELOG_DOCSET_EMPTY   = 0,
    STELOG_DOCSET_INLINE1 = 1, /* DF == 1 stored directly in inline_doc */
    STELOG_DOCSET_COMPACT = 2, /* DF in [2..63]: sorted uint32_t[] */
    STELOG_DOCSET_ROARING = 3  /* DF >= 64 or dimension bitmap: Roaring blob */
} StelogDocSetKind;

typedef struct {
    StelogDocSetKind kind;
    uint32_t card;
    uint32_t inline_doc;
    const uint32_t *compact_docs;
    const uint8_t *roaring_blob;
} StelogDocSet;

static inline StelogDocSet stelog_docset_from_term(const StelogTermSlot *slot, const uint8_t *postings_base) {
    StelogDocSet ds;
    ds.kind = STELOG_DOCSET_EMPTY;
    ds.card = 0;
    ds.inline_doc = 0;
    ds.compact_docs = NULL;
    ds.roaring_blob = NULL;
    if (!slot || slot->doc_freq == 0) return ds;
    ds.card = slot->doc_freq;
    if (slot->doc_freq == 1u) {
        ds.kind = STELOG_DOCSET_INLINE1;
        ds.inline_doc = slot->payload_or_off;
    } else if (slot->doc_freq <= STELOG_POSTING_COMPACT_MAX) {
        ds.kind = STELOG_DOCSET_COMPACT;
        ds.compact_docs = (const uint32_t *)(postings_base + ((uint64_t)slot->payload_or_off * 4ull));
    } else {
        ds.kind = STELOG_DOCSET_ROARING;
        ds.roaring_blob = postings_base + ((uint64_t)slot->payload_or_off * 4ull);
    }
    return ds;
}

static inline StelogDocSet stelog_docset_from_roaring(const uint8_t *blob) {
    StelogDocSet ds;
    ds.kind = STELOG_DOCSET_EMPTY;
    ds.card = 0;
    ds.inline_doc = 0;
    ds.compact_docs = NULL;
    ds.roaring_blob = NULL;
    if (!blob) return ds;
    const StelogRoaringHeader *h = (const StelogRoaringHeader *)blob;
    if (h->total_card == 0) return ds;
    ds.kind = STELOG_DOCSET_ROARING;
    ds.card = h->total_card;
    ds.roaring_blob = blob;
    return ds;
}

/* Membership test O(log n) or O(1) */
bool stelog_docset_contains(const StelogDocSet *ds, uint32_t doc_id);

/*
 * Intersect `set_count` StelogDocSets restricted to the inclusive range [lo_doc, hi_doc].
 * Automatically sorts sets by ascending cardinality, skips Roaring chunks outside
 * [lo_doc >> 16, hi_doc >> 16], and uses SIMD bitset AND when intersecting BITSET8K chunks.
 * Allocates output strictly from `scratch` arena (0 malloc!).
 * Returns number of surviving doc_ids written to *out_docs.
 */
uint32_t stelog_docset_intersect(const StelogDocSet *sets,
                                 uint32_t set_count,
                                 uint32_t lo_doc,
                                 uint32_t hi_doc,
                                 StelogArena *scratch,
                                 uint32_t **out_docs);

/* Union multiple dimension Roaring bitmaps restricted to [lo_doc, hi_doc] (e.g. role=user|assistant)
   into a scratch sorted uint32_t[] or test membership via OR list. */
bool stelog_docset_any_contains(const StelogDocSet *sets, uint32_t count, uint32_t doc_id);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_STORE_ROARING_H */
