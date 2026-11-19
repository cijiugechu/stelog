#define _POSIX_C_SOURCE 200809L
#include "adapters/adapter.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool ends_with(const char *s, size_t len, const char *suf) {
    size_t slen = strlen(suf);
    return len >= slen && memcmp(s + len - slen, suf, slen) == 0;
}

/* The cwd only appears in the session_start header line; hoisting it into the file entry lets every
   intra-file chunk carry the project, not just the chunk that contains line 1. */
static void read_header_cwd(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    char *nl = memchr(buf, '\n', (size_t)n);
    if (nl) *nl = '\0';
    if (!strstr(buf, "\"type\":\"session_start\"")) return;
    const char *k = strstr(buf, "\"cwd\":\"");
    if (!k) return;
    k += 7;
    size_t o = 0;
    while (*k && *k != '"' && o + 1 < cap) {
        if (*k == '\\') {
            k++;
            if (!*k) break;
        }
        out[o++] = *k++;
    }
    if (*k != '"') o = 0;
    out[o] = '\0';
}

/* Droid session layout: ~/.factory/sessions/<cwd-slug>/<session-uuid>.jsonl (+ .settings.json sidecars) */
static void droid_walk_cb(const char *full_path, const char *filename, uint64_t size, void *ud) {
    StelogFileList *list = (StelogFileList *)ud;
    size_t flen = strlen(filename);
    if (!ends_with(filename, flen, ".jsonl")) return;

    char sid[128];
    size_t slen = flen - 6;
    if (slen >= sizeof(sid)) slen = sizeof(sid) - 1;
    memcpy(sid, filename, slen);
    sid[slen] = '\0';

    char cwd[1024];
    read_header_cwd(full_path, cwd, sizeof(cwd));
    stelog_filelist_add(list, STELOG_AGENT_DROID, full_path,
                        sid[0] ? sid : NULL, cwd[0] ? cwd : NULL, size, false, false);
}

int stelog_droid_discover(const char *dir, StelogFileList *files) {
    return stelog_walk_dir(dir, droid_walk_cb, files);
}
