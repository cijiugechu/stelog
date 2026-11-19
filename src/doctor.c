#define _POSIX_C_SOURCE 200809L
#include "doctor.h"
#include "adapters/adapter.h"
#include "json/json_writer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool check_source(const StelogSourceDir *source, StelogJsonWriter *json) {
    char path[1024];
    int error = stelog_source_path(source, NULL, path, sizeof(path));
    StelogFileList files;
    stelog_filelist_init(&files);
    if (!error && access(path, R_OK | X_OK) != 0) error = errno;
    if (!error) error = source->discover(path, &files);

    uint32_t unreadable = 0;
    for (uint32_t i = 0; i < files.count; ++i) {
        if (access(files.items[i].path, R_OK) != 0) ++unreadable;
    }
    const char *status = "readable";
    if (!path[0]) status = "HOME unset";
    else if (error == ENOENT) status = "missing";
    else if (error || unreadable) status = "error";
    const char *agent = stelog_agent_name(source->agent);
    if (json) {
        stelog_jw_obj_begin(json);
        stelog_jw_kv_cstr(json, "agent", agent);
        stelog_jw_kv_cstr(json, "path", path);
        stelog_jw_kv_cstr(json, "status", status);
        stelog_jw_kv_u64(json, "files", files.count);
        stelog_jw_kv_u64(json, "unreadable_files", unreadable);
        if (error && error != ENOENT) stelog_jw_kv_cstr(json, "error", strerror(error));
        stelog_jw_obj_end(json);
    } else {
        printf("  %-5s  %s  [%s, %u file%s", agent, path, status, files.count, files.count == 1 ? "" : "s");
        if (unreadable) printf(", %u unreadable", unreadable);
        if (error && error != ENOENT) printf(", %s", strerror(error));
        puts("]");
    }
    stelog_filelist_destroy(&files);
    return path[0] && (!error || error == ENOENT) && !unreadable;
}

int stelog_doctor(const char *index_dir, bool json) {
    StelogJsonWriter w;
    if (json) {
        stelog_jw_init(&w, 2048);
        stelog_jw_obj_begin(&w);
        stelog_jw_key(&w, "sources");
        stelog_jw_arr_begin(&w);
    } else {
        puts("Transcript directories:");
    }
    bool healthy = true;
    for (size_t i = 0; i < stelog_source_dir_count; ++i) {
        if (!check_source(&stelog_source_dirs[i], json ? &w : NULL)) healthy = false;
    }

    char path[1024], meta[1280];
    stelog_resolve_index_dir(index_dir, path, sizeof(path));
    StelogIndex *idx = stelog_index_open(path);
    const char *status = "readable";
    if (!idx) {
        struct stat st;
        snprintf(meta, sizeof(meta), "%s/current.meta", path);
        status = stat(meta, &st) != 0 && errno == ENOENT ? "missing" : "unreadable or invalid";
        healthy = false;
    }
    if (json) {
        stelog_jw_arr_end(&w);
        stelog_jw_key(&w, "index");
        stelog_jw_obj_begin(&w);
        stelog_jw_kv_cstr(&w, "path", path);
        stelog_jw_kv_cstr(&w, "status", status);
        stelog_jw_obj_end(&w);
        stelog_jw_kv_bool(&w, "healthy", healthy);
        stelog_jw_obj_end(&w);
        puts(w.buf);
        stelog_jw_destroy(&w);
    } else {
        printf("Index: %s  [%s]\n", path, status);
        if (!idx) puts("Run 'stelog index' with the same index directory to build or refresh it.");
    }
    stelog_index_close(idx);
    return healthy ? 0 : 1;
}
