#ifndef STELOG_BASE_THREAD_POOL_H
#define STELOG_BASE_THREAD_POOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns Apple Silicon P-core count on macOS, or online CPU count on Linux. */
uint32_t stelog_detect_pcore_count(void);

typedef void (*StelogWorkerFn)(uint32_t worker_id, uint32_t num_workers, void *user_ctx);

/* Runs `fn` concurrently on `num_workers` threads (worker 0 runs on the calling thread). */
int stelog_parallel_run(uint32_t num_workers, StelogWorkerFn fn, void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif /* STELOG_BASE_THREAD_POOL_H */
