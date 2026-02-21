#include "../include/slab.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
  #include <pthread.h>
  #define HAS_PTHREAD 1
#else
  #define HAS_PTHREAD 0
#endif

static uint64_t now_ns(void) {
#if defined(__unix__) || defined(__APPLE__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#else
    return (uint64_t)clock() * (1000000000ull / (uint64_t)CLOCKS_PER_SEC);
#endif
}

static void test_basic_once(void) {
    void* p16  = slab_alloc(SLAB_16);
    void* p32  = slab_alloc(SLAB_32);
    void* p64  = slab_alloc(SLAB_64);
    void* p128 = slab_alloc(SLAB_128);

    if (!p16 || !p32 || !p64 || !p128) {
        fprintf(stderr, "basic_once: alloc returned NULL\n");
        exit(1);
    }

    memset(p16,  0x11, 16);
    memset(p32,  0x22, 32);
    memset(p64,  0x33, 64);
    memset(p128, 0x44, 128);

    slab_free(p16);
    slab_free(p32);
    slab_free(p64);
    slab_free(p128);
}

static void test_loop(size_t iters) {
    for (size_t i = 0; i < iters; i++) {
        test_basic_once();
    }
}

static void test_thread_cache_hits(size_t iters) {
    // 같은 스레드에서 free->alloc 패턴으로 thread cache hit를 유도
    void* p = slab_alloc(SLAB_64);
    if (!p) { fprintf(stderr, "tcache_hits: initial alloc failed\n"); exit(1); }

    for (size_t i = 0; i < iters; i++) {
        slab_free(p);
        p = slab_alloc(SLAB_64);
        if (!p) { fprintf(stderr, "tcache_hits: alloc failed at %zu\n", i); exit(1); }
    }
    slab_free(p);
}

#if HAS_PTHREAD
typedef struct {
    int tid;
    size_t iters;
} worker_arg_t;

static void* worker_fn(void* vp) {
    worker_arg_t* a = (worker_arg_t*)vp;
    // 각 스레드가 다양한 size class를 섞어 할당/해제
    for (size_t i = 0; i < a->iters; i++) {
        void* p1 = slab_alloc(SLAB_16);
        void* p2 = slab_alloc(SLAB_32);
        void* p3 = slab_alloc(SLAB_64);
        void* p4 = slab_alloc(SLAB_128);
        if (!p1 || !p2 || !p3 || !p4) {
            fprintf(stderr, "thread %d: alloc NULL\n", a->tid);
            return (void*)1;
        }
        // 살짝 쓰기
        ((uint8_t*)p1)[0] = (uint8_t)a->tid;
        ((uint8_t*)p4)[127] = (uint8_t)(a->tid ^ 0x5A);

        slab_free(p1);
        slab_free(p2);
        slab_free(p3);
        slab_free(p4);
    }
    return NULL;
}

static void test_multithread(int threads, size_t iters_per_thread) {
    pthread_t* th = (pthread_t*)calloc((size_t)threads, sizeof(pthread_t));
    worker_arg_t* args = (worker_arg_t*)calloc((size_t)threads, sizeof(worker_arg_t));
    if (!th || !args) { fprintf(stderr, "oom\n"); exit(1); }

    for (int i = 0; i < threads; i++) {
        args[i].tid = i;
        args[i].iters = iters_per_thread;
        if (pthread_create(&th[i], NULL, worker_fn, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < threads; i++) {
        void* ret = NULL;
        pthread_join(th[i], &ret);
        if (ret != NULL) {
            fprintf(stderr, "thread %d returned error\n", i);
            exit(1);
        }
    }

    free(th);
    free(args);
}
#endif

static void print_stats(const char* label) {
    slab_stats_t st = slab_get_stats();
    printf("[%s]\n", label);
    printf("  alloc_calls       = %llu\n", (unsigned long long)st.alloc_calls);
    printf("  free_calls        = %llu\n", (unsigned long long)st.free_calls);
    printf("  page_allocs       = %llu\n", (unsigned long long)st.page_allocs);
    printf("  page_frees        = %llu\n", (unsigned long long)st.page_frees);
    printf("  thread_cache_hits = %llu\n", (unsigned long long)st.thread_cache_hits);
    printf("  thread_cache_puts = %llu\n", (unsigned long long)st.thread_cache_puts);
}

int main(int argc, char** argv) {
    // 기본값
    size_t iters = 200000;
    size_t tcache_iters = 200000;
    int threads = 4;
    size_t iters_per_thread = 150000;

    // 간단 CLI
    // 예: ./build_test_suite 100000
    if (argc >= 2) iters = (size_t)strtoull(argv[1], NULL, 10);

    slab_init(NULL);

    // 1) 기본 루프
    uint64_t t0 = now_ns();
    test_loop(iters);
    uint64_t t1 = now_ns();
    print_stats("after basic loop");
    printf("basic loop: %zu iters, %.3f ms\n", iters, (double)(t1 - t0) / 1e6);

    // 2) thread cache hit 유도
    uint64_t t2 = now_ns();
    test_thread_cache_hits(tcache_iters);
    uint64_t t3 = now_ns();
    print_stats("after tcache hit loop");
    printf("tcache loop: %zu iters, %.3f ms\n", tcache_iters, (double)(t3 - t2) / 1e6);

#if HAS_PTHREAD
    // 3) 멀티스레드 스모크 테스트
    uint64_t t4 = now_ns();
    test_multithread(threads, iters_per_thread);
    uint64_t t5 = now_ns();
    print_stats("after multithread");
    printf("multithread: %d threads x %zu iters, %.3f ms\n",
           threads, iters_per_thread, (double)(t5 - t4) / 1e6);
#else
    printf("multithread skipped (pthread not available)\n");
#endif

    slab_shutdown();

    // 간단 sanity: alloc/free 카운트가 0이 아니어야 함
    slab_stats_t st = slab_get_stats();
    if (st.alloc_calls == 0 || st.free_calls == 0) {
        fprintf(stderr, "stats sanity failed\n");
        return 1;
    }

    printf("OK\n");
    return 0;
}
