#ifndef STELOG_BASE_HASH_H
#define STELOG_BASE_HASH_H

#include "stelog.h"
#include "base/arena.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t stelog_wyhash(const void *key, size_t len, uint64_t seed);

/* Open-addressing string -> StelogSessionMeta* table backed by an Arena */
typedef struct {
    uint64_t hash;
    const char *key;
    uint32_t key_len;
    StelogSessionMeta *val;
} StelogMetaSlot;

typedef struct {
    StelogMetaSlot *slots;
    uint32_t cap;    /* power of 2 */
    uint32_t count;
    StelogArena arena;
} StelogMetaMap;

void stelog_metamap_init(StelogMetaMap *m, uint32_t initial_cap);
void stelog_metamap_destroy(StelogMetaMap *m);
StelogSessionMeta *stelog_metamap_put(StelogMetaMap *m, const char *key, size_t key_len, const StelogSessionMeta *meta);
const StelogSessionMeta *stelog_metamap_get(const StelogMetaMap *m, const char *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_HASH_H */
