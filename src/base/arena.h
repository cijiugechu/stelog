#ifndef STELOG_BASE_ARENA_H
#define STELOG_BASE_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StelogArenaBlock {
    struct StelogArenaBlock *next;
    size_t cap;
    size_t used;
    _Alignas(64) uint8_t data[];
} StelogArenaBlock;

typedef struct {
    StelogArenaBlock *head;
    StelogArenaBlock *current;
    size_t default_block_size;
    size_t total_allocated;
} StelogArena;

void stelog_arena_init(StelogArena *a, size_t default_block_size);
void *stelog_arena_alloc_aligned(StelogArena *a, size_t size, size_t align);
void *stelog_arena_alloc(StelogArena *a, size_t size);
char *stelog_arena_strndup(StelogArena *a, const char *s, size_t len);
char *stelog_arena_strdup(StelogArena *a, const char *s);
void stelog_arena_reset(StelogArena *a);
void stelog_arena_destroy(StelogArena *a);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_ARENA_H */
