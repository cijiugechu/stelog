#ifndef STELOG_JSON_WRITER_H
#define STELOG_JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Zero-dependency, contiguous-buffer JSON serializer for CLI --json & MCP JSON-RPC 2.0 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool need_comma;
} StelogJsonWriter;

void stelog_jw_init(StelogJsonWriter *w, size_t initial_cap);
void stelog_jw_reset(StelogJsonWriter *w);
void stelog_jw_destroy(StelogJsonWriter *w);

void stelog_jw_obj_begin(StelogJsonWriter *w);
void stelog_jw_obj_end(StelogJsonWriter *w);
void stelog_jw_arr_begin(StelogJsonWriter *w);
void stelog_jw_arr_end(StelogJsonWriter *w);

void stelog_jw_key(StelogJsonWriter *w, const char *key);
void stelog_jw_str(StelogJsonWriter *w, const char *s, size_t len);
void stelog_jw_cstr(StelogJsonWriter *w, const char *s);
void stelog_jw_int(StelogJsonWriter *w, int64_t v);
void stelog_jw_u64(StelogJsonWriter *w, uint64_t v);
void stelog_jw_double(StelogJsonWriter *w, double v);
void stelog_jw_bool(StelogJsonWriter *w, bool v);
void stelog_jw_null(StelogJsonWriter *w);
void stelog_jw_raw(StelogJsonWriter *w, const char *raw_json, size_t len);

/* Convenience key-value helpers */
void stelog_jw_kv_str(StelogJsonWriter *w, const char *key, const char *s, size_t len);
void stelog_jw_kv_cstr(StelogJsonWriter *w, const char *key, const char *s);
void stelog_jw_kv_int(StelogJsonWriter *w, const char *key, int64_t v);
void stelog_jw_kv_u64(StelogJsonWriter *w, const char *key, uint64_t v);
void stelog_jw_kv_double(StelogJsonWriter *w, const char *key, double v);
void stelog_jw_kv_bool(StelogJsonWriter *w, const char *key, bool v);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_JSON_WRITER_H */
