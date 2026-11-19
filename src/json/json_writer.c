#include "json/json_writer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jw_reserve(StelogJsonWriter *w, size_t extra) {
    if (w->len + extra + 1 <= w->cap) return;
    size_t ncap = w->cap ? w->cap : 4096u;
    while (ncap < w->len + extra + 1) ncap *= 2u;
    char *nb = (char *)realloc(w->buf, ncap);
    if (!nb) return;
    w->buf = nb;
    w->cap = ncap;
}

static inline void jw_putc(StelogJsonWriter *w, char c) {
    jw_reserve(w, 1);
    if (w->buf) {
        w->buf[w->len++] = c;
        w->buf[w->len] = '\0';
    }
}

static inline void jw_puts(StelogJsonWriter *w, const char *s, size_t len) {
    if (len == 0) return;
    jw_reserve(w, len);
    if (w->buf) {
        memcpy(w->buf + w->len, s, len);
        w->len += len;
        w->buf[w->len] = '\0';
    }
}

static inline void jw_comma(StelogJsonWriter *w) {
    if (w->need_comma) jw_putc(w, ',');
}

void stelog_jw_init(StelogJsonWriter *w, size_t initial_cap) {
    w->cap = initial_cap ? initial_cap : 4096u;
    w->buf = (char *)malloc(w->cap);
    w->len = 0;
    if (w->buf) w->buf[0] = '\0';
    w->need_comma = false;
}

void stelog_jw_reset(StelogJsonWriter *w) {
    w->len = 0;
    if (w->buf) w->buf[0] = '\0';
    w->need_comma = false;
}

void stelog_jw_destroy(StelogJsonWriter *w) {
    free(w->buf);
    w->buf = NULL;
    w->len = 0;
    w->cap = 0;
    w->need_comma = false;
}

void stelog_jw_obj_begin(StelogJsonWriter *w) {
    jw_comma(w);
    jw_putc(w, '{');
    w->need_comma = false;
}

void stelog_jw_obj_end(StelogJsonWriter *w) {
    jw_putc(w, '}');
    w->need_comma = true;
}

void stelog_jw_arr_begin(StelogJsonWriter *w) {
    jw_comma(w);
    jw_putc(w, '[');
    w->need_comma = false;
}

void stelog_jw_arr_end(StelogJsonWriter *w) {
    jw_putc(w, ']');
    w->need_comma = true;
}

/* Length of the valid UTF-8 sequence starting at s[i] (2..4 bytes), or 0 if it is truncated,
   malformed, overlong, a surrogate, or beyond U+10FFFF. */
static size_t utf8_valid_seq_len(const unsigned char *s, size_t i, size_t len) {
    unsigned char c = s[i];
    size_t need;
    unsigned char lo = 0x80u, hi = 0xBFu;
    if (c >= 0xC2u && c <= 0xDFu) need = 1;
    else if (c == 0xE0u) { need = 2; lo = 0xA0u; }
    else if (c >= 0xE1u && c <= 0xECu) need = 2;
    else if (c == 0xEDu) { need = 2; hi = 0x9Fu; }
    else if (c >= 0xEEu && c <= 0xEFu) need = 2;
    else if (c == 0xF0u) { need = 3; lo = 0x90u; }
    else if (c >= 0xF1u && c <= 0xF3u) need = 3;
    else if (c == 0xF4u) { need = 3; hi = 0x8Fu; }
    else return 0;
    if (i + need >= len) return 0;
    if (s[i + 1] < lo || s[i + 1] > hi) return 0;
    for (size_t k = 2; k <= need; ++k) {
        if ((s[i + k] & 0xC0u) != 0x80u) return 0;
    }
    return need + 1;
}

/* Escapes for JSON and replaces invalid UTF-8 with U+FFFD so every emitted document parses. */
static void jw_write_escaped_str(StelogJsonWriter *w, const char *s, size_t len) {
    jw_reserve(w, len + 2);
    jw_putc(w, '"');
    if (s && len > 0) {
        const unsigned char *u = (const unsigned char *)s;
        size_t chunk_start = 0;
        size_t i = 0;
        while (i < len) {
            unsigned char c = u[i];
            if (c >= 0x80u) {
                size_t sl = utf8_valid_seq_len(u, i, len);
                if (sl > 0) { i += sl; continue; }
                if (i > chunk_start) jw_puts(w, s + chunk_start, i - chunk_start);
                jw_puts(w, "\xEF\xBF\xBD", 3);
                i++;
                chunk_start = i;
                continue;
            }
            if (c >= 0x20u && c != '"' && c != '\\') { i++; continue; }
            if (i > chunk_start) jw_puts(w, s + chunk_start, i - chunk_start);
            switch (c) {
                case '"':  jw_puts(w, "\\\"", 2); break;
                case '\\': jw_puts(w, "\\\\", 2); break;
                case '\b': jw_puts(w, "\\b", 2); break;
                case '\f': jw_puts(w, "\\f", 2); break;
                case '\n': jw_puts(w, "\\n", 2); break;
                case '\r': jw_puts(w, "\\r", 2); break;
                case '\t': jw_puts(w, "\\t", 2); break;
                default: {
                    char esc[8];
                    int n = snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                    jw_puts(w, esc, (size_t)n);
                    break;
                }
            }
            i++;
            chunk_start = i;
        }
        if (len > chunk_start) jw_puts(w, s + chunk_start, len - chunk_start);
    }
    jw_putc(w, '"');
}
void stelog_jw_key(StelogJsonWriter *w, const char *key) {
    jw_comma(w);
    jw_write_escaped_str(w, key, strlen(key));
    jw_putc(w, ':');
    w->need_comma = false;
}

void stelog_jw_str(StelogJsonWriter *w, const char *s, size_t len) {
    jw_comma(w);
    jw_write_escaped_str(w, s, len);
    w->need_comma = true;
}

void stelog_jw_cstr(StelogJsonWriter *w, const char *s) {
    if (!s) { stelog_jw_null(w); return; }
    stelog_jw_str(w, s, strlen(s));
}

void stelog_jw_int(StelogJsonWriter *w, int64_t v) {
    jw_comma(w);
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%" PRId64, v);
    jw_puts(w, tmp, (size_t)n);
    w->need_comma = true;
}

void stelog_jw_u64(StelogJsonWriter *w, uint64_t v) {
    jw_comma(w);
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
    jw_puts(w, tmp, (size_t)n);
    w->need_comma = true;
}

void stelog_jw_double(StelogJsonWriter *w, double v) {
    jw_comma(w);
    char tmp[48];
    int n = snprintf(tmp, sizeof(tmp), "%.4f", v);
    jw_puts(w, tmp, (size_t)n);
    w->need_comma = true;
}

void stelog_jw_bool(StelogJsonWriter *w, bool v) {
    jw_comma(w);
    if (v) jw_puts(w, "true", 4);
    else   jw_puts(w, "false", 5);
    w->need_comma = true;
}

void stelog_jw_null(StelogJsonWriter *w) {
    jw_comma(w);
    jw_puts(w, "null", 4);
    w->need_comma = true;
}

void stelog_jw_raw(StelogJsonWriter *w, const char *raw_json, size_t len) {
    jw_comma(w);
    jw_puts(w, raw_json, len);
    w->need_comma = true;
}

void stelog_jw_kv_str(StelogJsonWriter *w, const char *key, const char *s, size_t len) {
    stelog_jw_key(w, key);
    stelog_jw_str(w, s, len);
}

void stelog_jw_kv_cstr(StelogJsonWriter *w, const char *key, const char *s) {
    stelog_jw_key(w, key);
    stelog_jw_cstr(w, s);
}

void stelog_jw_kv_int(StelogJsonWriter *w, const char *key, int64_t v) {
    stelog_jw_key(w, key);
    stelog_jw_int(w, v);
}

void stelog_jw_kv_u64(StelogJsonWriter *w, const char *key, uint64_t v) {
    stelog_jw_key(w, key);
    stelog_jw_u64(w, v);
}

void stelog_jw_kv_double(StelogJsonWriter *w, const char *key, double v) {
    stelog_jw_key(w, key);
    stelog_jw_double(w, v);
}

void stelog_jw_kv_bool(StelogJsonWriter *w, const char *key, bool v) {
    stelog_jw_key(w, key);
    stelog_jw_bool(w, v);
}
