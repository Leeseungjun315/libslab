#include "../include/slab.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    size_t n = 2000000;
    if (argc >= 2) n = (size_t)strtoull(argv[1], NULL, 10);

    slab_init(NULL);

    // slab benchmark (alloc+free)
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < n; i++) {
        void* p = slab_alloc(SLAB_64);
        slab_free(p);
    }
    uint64_t t1 = now_ns();

    slab_stats_t st = slab_get_stats();
    double ms = (double)(t1 - t0) / 1e6;
    printf("slab: %zu alloc+free pairs in %.3f ms (%.3f Mops/s)\n",
           n, ms, (double)n / (ms * 1000.0));
    printf("thread_cache_hits=%llu thread_cache_puts=%llu\n",
           (unsigned long long)st.thread_cache_hits,
           (unsigned long long)st.thread_cache_puts);

    slab_shutdown();
    return 0;
}
