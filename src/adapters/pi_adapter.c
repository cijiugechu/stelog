#define _POSIX_C_SOURCE 200809L
#include "adapters/adapter.h"

#include <stdio.h>
#include <string.h>

static bool ends_with(const char *s, size_t len, const char *suf) {
    size_t slen = strlen(suf);
    return len >= slen && memcmp(s + len - slen, suf, slen) == 0;
}

/* Pi session filename: 2026-08-17T04-06-45-656Z_<uuid>.jsonl */
static void extract_pi_session_id(const char *filename, char *out, size_t cap) {
    out[0] = '\0';
    size_t len = strlen(filename);
    if (!ends_with(filename, len, ".jsonl")) return;
    len -= 6;
    const char *us = strrchr(filename, '_');
    const char *start = (us && (size_t)(us - filename) < len) ? (us + 1) : filename;
    size_t slen = (size_t)(filename + len - start);
    if (slen >= cap) slen = cap - 1;
    memcpy(out, start, slen);
    out[slen] = '\0';
}

static void pi_walk_cb(const char *full_path, const char *filename, uint64_t size, void *ud) {
    StelogFileList *list = (StelogFileList *)ud;
    size_t flen = strlen(filename);
    if (!ends_with(filename, flen, ".jsonl")) return;

    char sid[128];
    extract_pi_session_id(filename, sid, sizeof(sid));
    stelog_filelist_add(list, STELOG_AGENT_PI, full_path,
                        sid[0] ? sid : NULL, NULL, size, false, false);
}

void stelog_pi_discover(const char *pi_home, StelogFileList *out_files) {
    if (!pi_home || !pi_home[0]) return;
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/sessions", pi_home);
    stelog_walk_dir(dir, pi_walk_cb, out_files);
}
