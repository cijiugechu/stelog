#ifndef STELOG_BASE_TIME_ISO8601_H
#define STELOG_BASE_TIME_ISO8601_H

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

/* Monotonic wall clock in milliseconds for benchmarking */
double stelog_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_TIME_ISO8601_H */
