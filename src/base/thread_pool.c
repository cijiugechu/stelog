#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "base/thread_pool.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

uint32_t stelog_detect_pcore_count(void) {
#ifdef __APPLE__
    int pcores = 0;
    size_t sz = sizeof(pcores);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &pcores, &sz, NULL, 0) == 0 && pcores > 0) {
        return (uint32_t)pcores;
    }
    int ncpu = 0;
    sz = sizeof(ncpu);
    if (sysctlbyname("hw.physicalcpu", &ncpu, &sz, NULL, 0) == 0 && ncpu > 0) {
        return (uint32_t)ncpu;
    }
#endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) {
        if (n > 16) n = 16;
        return (uint32_t)n;
    }
    return 4;
}

typedef struct {
    uint32_t worker_id;
    uint32_t num_workers;
    StelogWorkerFn fn;
    void *user_ctx;
} WorkerArg;

static void *worker_trampoline(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    wa->fn(wa->worker_id, wa->num_workers, wa->user_ctx);
    return NULL;
}

int stelog_parallel_run(uint32_t num_workers, StelogWorkerFn fn, void *user_ctx) {
    if (num_workers <= 1) {
        fn(0, 1, user_ctx);
        return 0;
    }

    pthread_t *threads = (pthread_t *)calloc(num_workers, sizeof(pthread_t));
    WorkerArg *args = (WorkerArg *)calloc(num_workers, sizeof(WorkerArg));
    if (!threads || !args) {
        free(threads);
        free(args);
        fn(0, 1, user_ctx);
        return -1;
    }

    uint32_t spawned = 0;
    for (uint32_t i = 1; i < num_workers; ++i) {
        args[i].worker_id = i;
        args[i].num_workers = num_workers;
        args[i].fn = fn;
        args[i].user_ctx = user_ctx;
        if (pthread_create(&threads[i], NULL, worker_trampoline, &args[i]) == 0) {
            spawned = i;
        } else {
            break;
        }
    }

    /* Main thread executes worker 0 directly (zero idle main thread) */
    fn(0, num_workers, user_ctx);

    for (uint32_t i = 1; i <= spawned; ++i) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    free(args);
    return 0;
}
