#define _POSIX_C_SOURCE 200809L
#include "base/time_iso8601.h"

#include <string.h>
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

static int64_t realtime_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000ll + (int64_t)(ts.tv_nsec / 1000000l);
}

static bool is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m) {
    static const int dim[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29 : dim[m - 1];
}

/* mktime normalizes out-of-range fields, so day+1 is a valid way to reach the next local midnight. */
static bool local_civil_to_ms(int y, int mon, int day, int h, int mi, int sec, int64_t *out) {
    struct tm tm_v;
    memset(&tm_v, 0, sizeof(tm_v));
    tm_v.tm_year = y - 1900;
    tm_v.tm_mon = mon - 1;
    tm_v.tm_mday = day;
    tm_v.tm_hour = h;
    tm_v.tm_min = mi;
    tm_v.tm_sec = sec;
    tm_v.tm_isdst = -1;
    time_t t = mktime(&tm_v);
    if (t == (time_t)-1) return false;
    *out = (int64_t)t * 1000ll;
    return true;
}

static bool local_today(int *y, int *mon, int *day) {
    time_t now = time(NULL);
    struct tm tm_v;
    if (!localtime_r(&now, &tm_v)) return false;
    *y = tm_v.tm_year + 1900;
    *mon = tm_v.tm_mon + 1;
    *day = tm_v.tm_mday;
    return true;
}

static bool str_ieq(const char *s, size_t len, const char *lit) {
    size_t n = strlen(lit);
    if (len != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lit[i]) return false;
    }
    return true;
}

bool stelog_parse_time_arg(const char *s, size_t len, bool end_of_range, int64_t *out_ms) {
    if (!s || !out_ms) return false;
    while (len > 0 && (s[0] == ' ' || s[0] == '\t')) { s++; len--; }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len == 0) return false;

    if (str_ieq(s, len, "now")) {
        *out_ms = realtime_now_ms();
        return true;
    }
    if (str_ieq(s, len, "today") || str_ieq(s, len, "yesterday")) {
        int y, mon, day;
        if (!local_today(&y, &mon, &day)) return false;
        if (len == 9) day -= 1;
        int64_t start;
        if (!local_civil_to_ms(y, mon, day, 0, 0, 0, &start)) return false;
        if (end_of_range) {
            int64_t next;
            if (!local_civil_to_ms(y, mon, day + 1, 0, 0, 0, &next)) return false;
            *out_ms = next - 1;
        } else {
            *out_ms = start;
        }
        return true;
    }

    /* Relative: <N>m / <N>h / <N>d / <N>w (minutes, hours, days, weeks before now) */
    char unit = s[len - 1];
    if (len >= 2 && len <= 10 && (unit == 'm' || unit == 'h' || unit == 'd' || unit == 'w')) {
        int64_t val = 0;
        bool digits = true;
        for (size_t i = 0; i + 1 < len; ++i) {
            if (s[i] < '0' || s[i] > '9') { digits = false; break; }
            val = val * 10 + (s[i] - '0');
        }
        if (digits) {
            int64_t mult = unit == 'm' ? 60000ll : unit == 'h' ? 3600000ll : unit == 'd' ? 86400000ll : 604800000ll;
            *out_ms = realtime_now_ms() - val * mult;
            return true;
        }
    }

    if (len < 10 || s[4] != '-' || s[7] != '-') return false;
    int year = parse_4d(s);
    int mon  = parse_2d(s + 5);
    int day  = parse_2d(s + 8);
    if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > days_in_month(year, mon)) return false;

    int hour = 0, min = 0, sec = 0;
    int64_t frac_ms = 0;
    int precision = 0; /* 0 = day, 1 = minute, 2 = second, 3 = sub-second */
    size_t pos = 10;
    if (pos < len && (s[pos] == 'T' || s[pos] == 't' || s[pos] == ' ')) {
        if (pos + 6 > len || s[pos + 3] != ':') return false;
        hour = parse_2d(s + pos + 1);
        min  = parse_2d(s + pos + 4);
        if (hour < 0 || hour > 23 || min < 0 || min > 59) return false;
        precision = 1;
        pos += 6;
        if (pos < len && s[pos] == ':') {
            if (pos + 3 > len) return false;
            sec = parse_2d(s + pos + 1);
            if (sec < 0 || sec > 60) return false;
            precision = 2;
            pos += 3;
            if (pos < len && s[pos] == '.') {
                pos++;
                int scale = 100;
                size_t digits_start = pos;
                while (pos < len && s[pos] >= '0' && s[pos] <= '9') {
                    if (scale > 0) {
                        frac_ms += (int64_t)(s[pos] - '0') * scale;
                        scale /= 10;
                    }
                    pos++;
                }
                if (pos == digits_start) return false;
                precision = 3;
            }
        }
    }

    bool has_tz = false;
    int64_t tz_offset_sec = 0;
    if (pos < len) {
        if ((s[pos] == 'Z' || s[pos] == 'z') && pos + 1 == len) {
            has_tz = true;
            pos++;
        } else if (s[pos] == '+' || s[pos] == '-') {
            int sign = (s[pos] == '+') ? 1 : -1;
            size_t rem = len - pos - 1;
            const char *tz = s + pos + 1;
            int tzh = -1, tzm = 0;
            if (rem == 5 && tz[2] == ':') { tzh = parse_2d(tz); tzm = parse_2d(tz + 3); }
            else if (rem == 4) { tzh = parse_2d(tz); tzm = parse_2d(tz + 2); }
            else if (rem == 2) { tzh = parse_2d(tz); }
            if (tzh < 0 || tzh > 23 || tzm < 0 || tzm > 59) return false;
            tz_offset_sec = sign * ((int64_t)tzh * 3600 + (int64_t)tzm * 60);
            has_tz = true;
            pos = len;
        } else {
            return false;
        }
    }
    if (pos != len) return false;

    int64_t start_ms;
    int64_t day_span_ms = 86400000ll;
    if (has_tz) {
        int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
        start_ms = (days * 86400 + (int64_t)hour * 3600 + (int64_t)min * 60 + (int64_t)sec - tz_offset_sec) * 1000 + frac_ms;
    } else {
        if (!local_civil_to_ms(year, mon, day, hour, min, sec, &start_ms)) return false;
        start_ms += frac_ms;
        if (precision == 0 && end_of_range) {
            int64_t next;
            if (!local_civil_to_ms(year, mon, day + 1, 0, 0, 0, &next)) return false;
            day_span_ms = next - start_ms; /* 23h/25h on DST transition days */
        }
    }
    if (end_of_range) {
        int64_t span_ms = precision == 0 ? day_span_ms : precision == 1 ? 60000ll : precision == 2 ? 1000ll : 1ll;
        *out_ms = start_ms + span_ms - 1;
    } else {
        *out_ms = start_ms;
    }
    return true;
}

double stelog_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
