#define _POSIX_C_SOURCE 200809L
#include "adapters/adapter.h"

#include <dirent.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int stelog_agy_discover(const char *brain_dir, StelogFileList *out_files) {
    DIR *d = opendir(brain_dir);
    if (!d) return errno;
    int error = 0;

    struct dirent *ent;
    char path[1536];
    while (errno = 0, (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "%s/%s/.system_generated/logs/transcript.jsonl",
                 brain_dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            if (errno != ENOENT && errno != ENOTDIR) error = errno;
        } else if (S_ISREG(st.st_mode) && st.st_size > 0) {
            stelog_filelist_add(out_files, STELOG_AGENT_AGY, path,
                                ent->d_name, NULL, (uint64_t)st.st_size, false, false);
        }
    }
    if (errno) error = errno;
    closedir(d);
    return error;
}

/* Extract first clean local path from AGY workspace_uris (may be JSON array or file:// URI) */
static void clean_workspace_uri(const char *raw, char *out, size_t cap) {
    out[0] = '\0';
    if (!raw || !raw[0]) return;

    const char *p = strstr(raw, "file://");
    if (p) {
        p += 7;
    } else {
        p = raw;
        while (*p == '[' || *p == '"' || *p == ' ') p++;
    }
    size_t i = 0;
    while (p[i] && p[i] != '"' && p[i] != ',' && p[i] != ']' && p[i] != '\n' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

void stelog_agy_load_metadata(const char *agy_home, StelogMetaMap *out_meta) {
    if (!agy_home || !agy_home[0]) return;
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/conversation_summaries.db", agy_home);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 100);

    const char *sql =
        "SELECT conversation_id, title, workspace_uris, agent_name, parent_conversation_id, nesting_depth "
        "FROM conversation_summaries";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        char cwd_buf[1024];
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(stmt, 0);
            if (!id || !id[0]) continue;
            const char *ws_raw = (const char *)sqlite3_column_text(stmt, 2);
            clean_workspace_uri(ws_raw, cwd_buf, sizeof(cwd_buf));

            StelogSessionMeta m;
            memset(&m, 0, sizeof(m));
            m.title      = (const char *)sqlite3_column_text(stmt, 1);
            m.cwd        = cwd_buf[0] ? cwd_buf : NULL;
            m.agent_name = (const char *)sqlite3_column_text(stmt, 3);
            m.parent_id  = (const char *)sqlite3_column_text(stmt, 4);
            m.is_subagent = (m.parent_id && m.parent_id[0] != '\0') || sqlite3_column_int(stmt, 5) > 0;
            stelog_metamap_put(out_meta, id, strlen(id), &m);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
}
