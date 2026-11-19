#include "base/utf8.h"

#include "simdutf.h"

extern "C" bool stelog_utf8_validate(const char *s, size_t len) {
    if (!s || len == 0) return true;
    return simdutf::validate_utf8(s, len);
}

extern "C" bool stelog_utf8_validate_ascii(const char *s, size_t len) {
    if (!s || len == 0) return true;
    return simdutf::validate_ascii(s, len);
}

extern "C" size_t stelog_utf8_count_codepoints(const char *s, size_t len) {
    if (!s || len == 0) return 0;
    return simdutf::count_utf8(s, len);
}

extern "C" size_t stelog_utf8_to_utf32(const char *s, size_t len, uint32_t *out_u32) {
    if (!s || len == 0 || !out_u32) return 0;
    return simdutf::convert_utf8_to_utf32(s, len, reinterpret_cast<char32_t *>(out_u32));
}
