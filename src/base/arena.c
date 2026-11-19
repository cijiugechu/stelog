#include "base/arena.h"

#include <stdlib.h>
#include <string.h>

static StelogArenaBlock *arena_new_block(size_t cap) {
    size_t total = sizeof(StelogArenaBlock) + cap;
    StelogArenaBlock *blk = (StelogArenaBlock *)malloc(total);
    if (!blk) return NULL;
    blk->next = NULL;
    blk->cap = cap;
    blk->used = 0;
    return blk;
}

void stelog_arena_init(StelogArena *a, size_t default_block_size) {
    a->head = NULL;
    a->current = NULL;
    a->default_block_size = default_block_size ? default_block_size : (1u << 20); /* 1 MiB */
    a->total_allocated = 0;
}

void *stelog_arena_alloc_aligned(StelogArena *a, size_t size, size_t align) {
    if (size == 0) size = 1;
    if (align < 8) align = 8;
    size_t mask = align - 1;

    StelogArenaBlock *blk = a->current;
    while (blk) {
        size_t pos = (blk->used + mask) & ~mask;
        if (pos + size <= blk->cap) {
            blk->used = pos + size;
            a->current = blk;
            return blk->data + pos;
        }
        if (blk->next) {
            blk = blk->next;
            blk->used = 0;
        } else {
            break;
        }
    }

    size_t next_cap = a->default_block_size;
    if (size + align > next_cap) {
        next_cap = (size + align + 4095u) & ~(size_t)4095u;
    }
    StelogArenaBlock *nb = arena_new_block(next_cap);
    if (!nb) return NULL;
    if (blk) {
        blk->next = nb;
    } else {
        a->head = nb;
    }
    a->current = nb;
    a->total_allocated += next_cap;

    size_t pos = (nb->used + mask) & ~mask;
    nb->used = pos + size;
    return nb->data + pos;
}

void *stelog_arena_alloc(StelogArena *a, size_t size) {
    return stelog_arena_alloc_aligned(a, size, 8);
}

char *stelog_arena_strndup(StelogArena *a, const char *s, size_t len) {
    char *dst = (char *)stelog_arena_alloc_aligned(a, len + 1, 1);
    if (!dst) return NULL;
    if (len > 0 && s) {
        memcpy(dst, s, len);
    }
    dst[len] = '\0';
    return dst;
}

char *stelog_arena_strdup(StelogArena *a, const char *s) {
    if (!s) return NULL;
    return stelog_arena_strndup(a, s, strlen(s));
}

void stelog_arena_reset(StelogArena *a) {
    if (a->head) {
        a->head->used = 0;
        a->current = a->head;
    }
}

void stelog_arena_destroy(StelogArena *a) {
    StelogArenaBlock *blk = a->head;
    while (blk) {
        StelogArenaBlock *next = blk->next;
        free(blk);
        blk = next;
    }
    a->head = NULL;
    a->current = NULL;
    a->total_allocated = 0;
}
