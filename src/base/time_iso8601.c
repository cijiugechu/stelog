#define _POSIX_C_SOURCE 200809L
#include "base/time_iso8601.h"

#include <time.h>

static inline int parse_2d(const char *p) {
    unsigned d0 = (unsigned)(p[0] - '0');
    unsigned d1 = (unsigned)(p[1] - '0');
    if (d0 > 9 || d1 > 9) return -1;
    return (int)(d0 * 10u + d1);
}

static inline int parse_4d(const char *p) {
    int hi = parse_2d(p);
    int lo = parse_2d(p + 2);
    if (hi < 0 || lo < 0) return -1;
    return hi * 100 + lo;
}

/* Howard Hinnant's civil_from_days / days_from_civil algorithm (O(1), branchless) */
static inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int64_t stelog_parse_iso8601_ms(const char *s, size_t len) {
    if (!s || len < 19) return 0;
    if (s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') ||
        s[13] != ':' || s[16] != ':') {
        return 0;
    }

    int year = parse_4d(s);
    int mon  = parse_2d(s + 5);
    int day  = parse_2d(s + 8);
    int hour = parse_2d(s + 11);
    int min  = parse_2d(s + 14);
    int sec  = parse_2d(s + 17);

    if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) {
        return 0;
    }

    size_t pos = 19;
    int64_t ms = 0;
    if (pos < len && s[pos] == '.') {
        pos++;
        int scale = 100;
        while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
            if (scale > 0) {
                ms += (int64_t)(s[pos] - '0') * scale;
                scale /= 10;
            }
            pos++;
        }
    }

    int64_t tz_offset_sec = 0;
    if (pos < len && (s[pos] == '+' || s[pos] == '-')) {
        int sign = (s[pos] == '+') ? 1 : -1;
        pos++;
        if (pos + 5 <= len && s[pos + 2] == ':') {
            int tzh = parse_2d(s + pos);
            int tzm = parse_2d(s + pos + 3);
            if (tzh >= 0 && tzm >= 0) {
                tz_offset_sec = sign * ((int64_t)tzh * 3600 + (int64_t)tzm * 60);
            }
        } else if (pos + 4 <= len) {
            int tzh = parse_2d(s + pos);
            int tzm = parse_2d(s + pos + 2);
            if (tzh >= 0 && tzm >= 0) {
                tz_offset_sec = sign * ((int64_t)tzh * 3600 + (int64_t)tzm * 60);
            }
        }
    }

    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    int64_t total_sec = days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + (int64_t)sec - tz_offset_sec;
    return total_sec * 1000 + ms;
}

double stelog_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
