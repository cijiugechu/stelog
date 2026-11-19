#include "base/hash.h"

#include <stdlib.h>
#include <string.h>

static inline uint64_t wymix(uint64_t a, uint64_t b) {
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

static inline uint64_t wyr8(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline uint64_t wyr4(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint64_t wyr3(const uint8_t *p, size_t k) {
    return (((uint64_t)p[0]) << 16) | (((uint64_t)p[k >> 1]) << 8) | p[k - 1];
}

uint64_t stelog_wyhash(const void *key, size_t len, uint64_t seed) {
    static const uint64_t s0 = 0xa0761d6478bd642full;
    static const uint64_t s1 = 0xe7037ed1a0b428dbull;
    static const uint64_t s2 = 0x8ebc6af09c88c6e3ull;
    static const uint64_t s3 = 0x589965cc75374cc3ull;

    const uint8_t *p = (const uint8_t *)key;
    seed ^= wymix(seed ^ s0, s1);
    uint64_t a, b;

    if (len <= 16) {
        if (len >= 4) {
            a = (wyr4(p) << 32) | wyr4(p + ((len >> 3) << 2));
            b = (wyr4(p + len - 4) << 32) | wyr4(p + len - 4 - ((len >> 3) << 2));
        } else if (len > 0) {
            a = wyr3(p, len);
            b = 0;
        } else {
            a = 0;
            b = 0;
        }
    } else {
        size_t i = len;
        if (i > 48) {
            uint64_t see1 = seed, see2 = seed;
            do {
                seed = wymix(wyr8(p) ^ s1, wyr8(p + 8) ^ seed);
                see1 = wymix(wyr8(p + 16) ^ s2, wyr8(p + 24) ^ see1);
                see2 = wymix(wyr8(p + 32) ^ s3, wyr8(p + 40) ^ see2);
                p += 48;
                i -= 48;
            } while (i > 48);
            seed ^= see1 ^ see2;
        }
        while (i > 16) {
            seed = wymix(wyr8(p) ^ s1, wyr8(p + 8) ^ seed);
            i -= 16;
            p += 16;
        }
        a = wyr8(p + i - 16);
        b = wyr8(p + i - 8);
    }
    return wymix(s1 ^ len, wymix(a ^ s1, b ^ seed));
}

void stelog_metamap_init(StelogMetaMap *m, uint32_t initial_cap) {
    uint32_t cap = 64;
    while (cap < initial_cap) cap <<= 1;
    m->slots = (StelogMetaSlot *)calloc(cap, sizeof(StelogMetaSlot));
    m->cap = cap;
    m->count = 0;
    stelog_arena_init(&m->arena, 256u * 1024u);
}

void stelog_metamap_destroy(StelogMetaMap *m) {
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->count = 0;
    stelog_arena_destroy(&m->arena);
}

static void metamap_grow(StelogMetaMap *m) {
    uint32_t new_cap = m->cap * 2;
    StelogMetaSlot *ns = (StelogMetaSlot *)calloc(new_cap, sizeof(StelogMetaSlot));
    if (!ns) return;
    uint32_t mask = new_cap - 1;
    for (uint32_t i = 0; i < m->cap; ++i) {
        if (!m->slots[i].key) continue;
        uint32_t idx = (uint32_t)m->slots[i].hash & mask;
        while (ns[idx].key) idx = (idx + 1) & mask;
        ns[idx] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = new_cap;
}

StelogSessionMeta *stelog_metamap_put(StelogMetaMap *m, const char *key, size_t key_len, const StelogSessionMeta *meta) {
    if (!key || key_len == 0) return NULL;
    if ((m->count + 1) * 10 >= m->cap * 7) {
        metamap_grow(m);
    }
    uint64_t h = stelog_wyhash(key, key_len, 0x517cc1b727220a95ull);
    uint32_t mask = m->cap - 1;
    uint32_t idx = (uint32_t)h & mask;

    while (m->slots[idx].key) {
        if (m->slots[idx].hash == h && m->slots[idx].key_len == key_len &&
            memcmp(m->slots[idx].key, key, key_len) == 0) {
            break;
        }
        idx = (idx + 1) & mask;
    }

    StelogSessionMeta *dst = m->slots[idx].val;
    if (!dst) {
        dst = (StelogSessionMeta *)stelog_arena_alloc(&m->arena, sizeof(StelogSessionMeta));
        memset(dst, 0, sizeof(*dst));
        m->slots[idx].hash = h;
        m->slots[idx].key = stelog_arena_strndup(&m->arena, key, key_len);
        m->slots[idx].key_len = (uint32_t)key_len;
        m->slots[idx].val = dst;
        m->count++;
    }

    dst->session_id = m->slots[idx].key;
    if (meta->title && meta->title[0]) dst->title = stelog_arena_strdup(&m->arena, meta->title);
    if (meta->cwd && meta->cwd[0]) dst->cwd = stelog_arena_strdup(&m->arena, meta->cwd);
    if (meta->git_branch && meta->git_branch[0]) dst->git_branch = stelog_arena_strdup(&m->arena, meta->git_branch);
    if (meta->agent_name && meta->agent_name[0]) dst->agent_name = stelog_arena_strdup(&m->arena, meta->agent_name);
    if (meta->agent_role && meta->agent_role[0]) dst->agent_role = stelog_arena_strdup(&m->arena, meta->agent_role);
    if (meta->model && meta->model[0]) dst->model = stelog_arena_strdup(&m->arena, meta->model);
    if (meta->parent_id && meta->parent_id[0]) dst->parent_id = stelog_arena_strdup(&m->arena, meta->parent_id);
    dst->is_archived = meta->is_archived;
    dst->is_subagent = meta->is_subagent;
    return dst;
}

const StelogSessionMeta *stelog_metamap_get(const StelogMetaMap *m, const char *key, size_t key_len) {
    if (!m || !m->slots || !key || key_len == 0) return NULL;
    uint64_t h = stelog_wyhash(key, key_len, 0x517cc1b727220a95ull);
    uint32_t mask = m->cap - 1;
    uint32_t idx = (uint32_t)h & mask;
    while (m->slots[idx].key) {
        if (m->slots[idx].hash == h && m->slots[idx].key_len == key_len &&
            memcmp(m->slots[idx].key, key, key_len) == 0) {
            return m->slots[idx].val;
        }
        idx = (idx + 1) & mask;
    }
    return NULL;
}
