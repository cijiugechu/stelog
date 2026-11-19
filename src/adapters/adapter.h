#ifndef STELOG_ADAPTERS_ADAPTER_H
#define STELOG_ADAPTERS_ADAPTER_H

#include "stelog.h"
#include "stelog_json.h"
#include "base/arena.h"
#include "base/hash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t file_id;
    StelogAgentKind agent;
    const char *path;
    const char *session_id_hint; /* extracted from path/dir/filename when available */
    const char *cwd_hint;        /* extracted from metadata or directory */
    uint64_t file_size;
    bool is_zstd;
    bool is_archived;
} StelogFileEntry;

typedef struct {
    StelogFileEntry *items;
    uint32_t count;
    uint32_t cap;
    StelogArena arena;
} StelogFileList;

void stelog_filelist_init(StelogFileList *list);
void stelog_filelist_destroy(StelogFileList *list);
uint32_t stelog_filelist_add(StelogFileList *list,
                             StelogAgentKind agent,
                             const char *path,
                             const char *session_id_hint,
                             const char *cwd_hint,
                             uint64_t file_size,
                             bool is_zstd,
                             bool is_archived);

/* Recursive directory walker helper */
typedef void (*StelogWalkCb)(const char *full_path, const char *filename, uint64_t size, void *ctx);
void stelog_walk_dir(const char *dir_path, StelogWalkCb cb, void *ctx);

/* Agent-specific discovery & SQLite metadata loaders */
void stelog_codex_discover(const char *codex_home, StelogFileList *out_files);
void stelog_codex_load_metadata(const char *codex_home, StelogMetaMap *out_meta);

void stelog_agy_discover(const char *agy_home, StelogFileList *out_files);
void stelog_agy_load_metadata(const char *agy_home, StelogMetaMap *out_meta);

void stelog_pi_discover(const char *pi_home, StelogFileList *out_files);

/* Exposed for unit testing chunk boundary invariance */
int stelog_scan_single_file_for_test(const char *path,
                                     StelogAgentKind agent,
                                     uint32_t chunk_bytes,
                                     const StelogParseOptions *opts,
                                     StelogAgentStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_ADAPTERS_ADAPTER_H */
