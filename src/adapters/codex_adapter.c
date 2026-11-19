#define _POSIX_C_SOURCE 200809L
#include "adapters/adapter.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

static bool ends_with(const char *s, size_t len, const char *suf) {
    size_t slen = strlen(suf);
    return len >= slen && memcmp(s + len - slen, suf, slen) == 0;
}

/*
 * Codex filenames look like:
 *   rollout-2026-11-04T13-35-46-01a1a50e-63b7-79e3-b272-72256660f64e.jsonl(.zst)
 * The UUID is the last 36 characters before ".jsonl" or ".jsonl.zst".
 */
static void extract_codex_session_id(const char *filename, char *out, size_t out_cap) {
    out[0] = '\0';
    size_t len = strlen(filename);
    if (ends_with(filename, len, ".jsonl.zst")) {
        len -= 10;
    } else if (ends_with(filename, len, ".jsonl")) {
        len -= 6;
    } else {
        return;
    }
    if (len >= 36 && out_cap > 36) {
        const char *cand = filename + (len - 36);
        if (cand[8] == '-' && cand[13] == '-' && cand[18] == '-' && cand[23] == '-') {
            memcpy(out, cand, 36);
            out[36] = '\0';
            return;
        }
    }
    size_t copy = len < out_cap - 1 ? len : out_cap - 1;
    memcpy(out, filename, copy);
    out[copy] = '\0';
}

typedef struct {
    StelogFileList *list;
    bool is_archived;
} CodexWalkCtx;

static void codex_walk_cb(const char *full_path, const char *filename, uint64_t size, void *ud) {
    CodexWalkCtx *ctx = (CodexWalkCtx *)ud;
    size_t flen = strlen(filename);
    bool is_zst = ends_with(filename, flen, ".jsonl.zst");
    bool is_jsonl = !is_zst && ends_with(filename, flen, ".jsonl");
    if (!is_jsonl && !is_zst) return;

    char sid[128];
    extract_codex_session_id(filename, sid, sizeof(sid));
    stelog_filelist_add(ctx->list, STELOG_AGENT_CODEX, full_path,
                        sid[0] ? sid : NULL, NULL, size, is_zst, ctx->is_archived);
}

void stelog_codex_discover(const char *codex_home, StelogFileList *out_files) {
    if (!codex_home || !codex_home[0]) return;
    char dir[1024];

    snprintf(dir, sizeof(dir), "%s/sessions", codex_home);
    CodexWalkCtx ctx = { out_files, false };
    stelog_walk_dir(dir, codex_walk_cb, &ctx);

    snprintf(dir, sizeof(dir), "%s/archived_sessions", codex_home);
    ctx.is_archived = true;
    stelog_walk_dir(dir, codex_walk_cb, &ctx);
}

void stelog_codex_load_metadata(const char *codex_home, StelogMetaMap *out_meta) {
    if (!codex_home || !codex_home[0]) return;
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/state_5.sqlite", codex_home);

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 100);

    const char *sql =
        "SELECT id, title, cwd, git_branch, agent_nickname, agent_role, model, archived "
        "FROM threads";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *id = (const char *)sqlite3_column_text(stmt, 0);
            if (!id || !id[0]) continue;
            StelogSessionMeta m;
            memset(&m, 0, sizeof(m));
            m.title       = (const char *)sqlite3_column_text(stmt, 1);
            m.cwd         = (const char *)sqlite3_column_text(stmt, 2);
            m.git_branch  = (const char *)sqlite3_column_text(stmt, 3);
            m.agent_name  = (const char *)sqlite3_column_text(stmt, 4);
            m.agent_role  = (const char *)sqlite3_column_text(stmt, 5);
            m.model       = (const char *)sqlite3_column_text(stmt, 6);
            m.is_archived = sqlite3_column_int(stmt, 7) != 0;
            stelog_metamap_put(out_meta, id, strlen(id), &m);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
}
