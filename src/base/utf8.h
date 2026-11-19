#ifndef STELOG_BASE_UTF8_H
#define STELOG_BASE_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Battle-tested SIMD UTF-8 primitives backed by simdutf */
bool stelog_utf8_validate(const char *s, size_t len);
bool stelog_utf8_validate_ascii(const char *s, size_t len);
size_t stelog_utf8_count_codepoints(const char *s, size_t len);

/* Convert valid UTF-8 [s, s+len) into UTF-32 codepoints in out_u32 (must have room for >= len u32s).
   Returns number of codepoints written (0 on invalid UTF-8). */
size_t stelog_utf8_to_utf32(const char *s, size_t len, uint32_t *out_u32);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_UTF8_H */
