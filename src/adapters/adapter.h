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
int stelog_walk_dir(const char *dir_path, StelogWalkCb cb, void *ctx);

/* Discovery returns zero or an errno, and retains any files already found. */
typedef struct {
    StelogAgentKind agent;
    const char *subdir;
    int (*discover)(const char *dir, StelogFileList *files);
} StelogSourceDir;

extern const StelogSourceDir stelog_source_dirs[];
extern const size_t stelog_source_dir_count;
void stelog_load_metadata(const char *const homes[], uint32_t mask, StelogMetaMap *meta);
int stelog_source_path(const StelogSourceDir *source, const char *override_home, char *out, size_t cap);
void stelog_discover_files(const char *const homes[], uint32_t mask, StelogFileList *files);

int stelog_codex_discover(const char *dir, StelogFileList *files);
int stelog_codex_discover_archived(const char *dir, StelogFileList *files);
int stelog_agy_discover(const char *dir, StelogFileList *files);
int stelog_pi_discover(const char *dir, StelogFileList *files);
int stelog_droid_discover(const char *dir, StelogFileList *files);
void stelog_codex_load_metadata(const char *codex_home, StelogMetaMap *out_meta);
void stelog_agy_load_metadata(const char *agy_home, StelogMetaMap *out_meta);

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
