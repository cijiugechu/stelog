#ifndef STELOG_BASE_TIME_ISO8601_H
#define STELOG_BASE_TIME_ISO8601_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fast hand-rolled ISO-8601 timestamp parser -> UTC milliseconds since Unix epoch.
 * Supports:
 *   YYYY-MM-DDTHH:MM:SSZ
 *   YYYY-MM-DDTHH:MM:SS.sssZ (up to 9 fractional digits, truncated to ms)
 *   YYYY-MM-DDTHH:MM:SS+HH:MM / -HH:MM
 * Returns 0 on invalid input.
 */
int64_t stelog_parse_iso8601_ms(const char *s, size_t len);

/*
 * Parse a user-supplied since/until filter into UTC milliseconds.
 * Accepts:
 *   relative offsets before now: 30m, 24h, 7d, 2w
 *   now, today, yesterday
 *   YYYY-MM-DD
 *   YYYY-MM-DD[T ]HH:MM[:SS[.sss]][Z|+HH:MM|-HH:MM|+HHMM|+HH]
 * Values without an explicit zone use the local time zone, matching how timestamps are displayed.
 * With end_of_range=true, the value resolves to the last millisecond of its precision
 * (e.g. until=2026-05-01 covers the whole day).
 * Returns false (leaving *out_ms untouched) on invalid input.
 */
bool stelog_parse_time_arg(const char *s, size_t len, bool end_of_range, int64_t *out_ms);

/* Monotonic wall clock in milliseconds for benchmarking */
double stelog_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_TIME_ISO8601_H */
