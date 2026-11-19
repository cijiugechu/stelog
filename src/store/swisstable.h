#ifndef STELOG_STORE_SWISSTABLE_H
#define STELOG_STORE_SWISSTABLE_H

#include "store/format.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a serialized static SwissTable binary image ready to write directly to terms.ht.
   Caller frees *out_buf with free(). Returns 0 on success. */
int stelog_swiss_build(const StelogTermSlot *terms,
                       uint32_t term_count,
                       uint8_t **out_buf,
                       size_t *out_len);

/* Zero-copy read-only mmap view of terms.ht */
typedef struct {
    const uint8_t *ctrl;
    const StelogTermSlot *slots;
    uint32_t group_mask;
    uint32_t term_count;
} StelogSwissView;

int stelog_swiss_open(const uint8_t *data, size_t len, StelogSwissView *out);

/* SIMD 16-lane parallel lookup (NEON on ARM64, SSE2 on x86_64, portable fallback).
   Returns pointer to matching StelogTermSlot in mmap memory, or NULL if not found. */
const StelogTermSlot *stelog_swiss_find(const StelogSwissView *view, uint64_t term_hash);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_STORE_SWISSTABLE_H */
