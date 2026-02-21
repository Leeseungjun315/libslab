#include "slab_internal.h"
#include "../include/slab.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#include <malloc.h>
static void* page_alloc(size_t sz) { return _aligned_malloc(sz, 4096); }
static void  page_free_win(void* p) { _aligned_free(p); }
#else
#include <sys/mman.h>
#include <unistd.h>
static void* page_alloc(size_t sz) {
    void* p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
}
static void page_free_posix(void* p, size_t sz) {
    if (p) munmap(p, sz);
}
#endif

slab_global_t g_slab = {
    .page_size = SLAB_PAGE_SIZE_DEFAULT,
    .thread_cache_max = 64,
    .initialized = 0
};

#if SLAB_ENABLE_THREAD_CACHE
typedef struct thread_cache {
    obj_hdr_t* head[4];
    uint32_t   count[4];
} thread_cache_t;

static _Thread_local thread_cache_t tcache;
#endif

static void slab_lazy_init(void);

static void debug_panic(const char* msg) {
#if SLAB_DEBUG
    fprintf(stderr, "[libslab][DEBUG PANIC] %s\n", msg);
    abort();
#else
    (void)msg;
#endif
}

#if SLAB_DEBUG
static inline void bitmap_set(uint32_t* w, uint32_t idx) {
    w[idx >> 5] |= (1u << (idx & 31));
}
static inline void bitmap_clear(uint32_t* w, uint32_t idx) {
    w[idx >> 5] &= ~(1u << (idx & 31));
}
static inline int bitmap_test(uint32_t* w, uint32_t idx) {
    return (w[idx >> 5] >> (idx & 31)) & 1u;
}

static void guard_fill(uint8_t* p, uint8_t v, size_t n) {
    memset(p, v, n);
}

static void guard_check(uint8_t* p, uint8_t v, size_t n, const char* what) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != v) {
            fprintf(stderr, "[libslab] guard corrupted: %s at +%zu\n", what, i);
            abort();
        }
    }
}
#endif

static slab_page_t* slab_page_create(int sc_idx) {
    const size_t user_sz = index_to_sc(sc_idx);

    // slot size = [obj_hdr][guards][payload][guards]
    const size_t hdr_sz  = sizeof(obj_hdr_t);
    const size_t slot_sz = hdr_sz + SLAB_GUARD_SZ + user_sz + SLAB_GUARD_SZ;

    const size_t page_sz = g_slab.page_size;

    void* mem = page_alloc(page_sz);
    if (!mem) return NULL;

    atomic_fetch_add(&g_slab.page_allocs, 1);

    uint8_t* base = (uint8_t*)mem;
    slab_page_t* pg = (slab_page_t*)base;
    memset(pg, 0, sizeof(*pg));
    pg->magic_page = SLAB_MAGIC_PAGE;
    pg->sc_idx = (uint16_t)sc_idx;
    pg->base = base;

    size_t offset = sizeof(slab_page_t);
    offset = (offset + 15u) & ~((size_t)15u);

#if SLAB_DEBUG
    // capacity 추정 -> bitmap 포함해서 재계산(1~2회 수렴)
    size_t cap_guess = (page_sz - offset) / slot_sz;
    if (cap_guess == 0) debug_panic("page too small for one slot");

    for (int iter = 0; iter < 2; iter++) {
        size_t word_count = (cap_guess + 31u) / 32u;
        size_t bitmap_bytes = word_count * sizeof(uint32_t);

        size_t off2 = offset + bitmap_bytes;
        off2 = (off2 + 15u) & ~((size_t)15u);

        size_t cap2 = (page_sz - off2) / slot_sz;
        if (cap2 == cap_guess) {
            pg->bitmap_words = (uint32_t*)(base + offset);
            pg->bitmap_word_count = (uint32_t)word_count;
            memset(pg->bitmap_words, 0, bitmap_bytes);

            offset = off2;
            pg->capacity = (uint16_t)cap2;
            break;
        }
        cap_guess = cap2;
    }
    if (pg->capacity == 0) debug_panic("failed to compute capacity");
#else
    size_t cap = (page_sz - offset) / slot_sz;
    if (cap == 0) debug_panic("page too small for one slot");
    pg->capacity = (uint16_t)cap;
#endif

    // freelist 구성
    obj_hdr_t* prev = NULL;
    for (uint16_t i = 0; i < pg->capacity; i++) {
        uint8_t* slot = base + offset + (size_t)i * slot_sz;
        obj_hdr_t* h = (obj_hdr_t*)slot;

#if SLAB_DEBUG
        h->magic_obj = SLAB_MAGIC_OBJ;
        h->sc_idx = (uint16_t)sc_idx;
        h->slot_idx = i;
        h->owner = pg;

        uint8_t* ga = slot + sizeof(obj_hdr_t);
        uint8_t* payload = ga + SLAB_GUARD_SZ;
        uint8_t* gb = payload + user_sz;

        guard_fill(ga, SLAB_GUARD_A, SLAB_GUARD_SZ);
        guard_fill(gb, SLAB_GUARD_B, SLAB_GUARD_SZ);
        // bitmap bit = 0 (free)
#endif

        h->next_free = prev;
        prev = h;
    }

    pg->freelist = prev;
    atomic_store(&pg->inuse, 0);
    return pg;
}

static void slab_page_destroy(slab_page_t* pg) {
    if (!pg) return;
    atomic_fetch_add(&g_slab.page_frees, 1);
#if defined(_WIN32)
    page_free_win(pg);
#else
    page_free_posix(pg, g_slab.page_size);
#endif
}

static obj_hdr_t* page_pop(slab_page_t* pg) {
    obj_hdr_t* h = pg->freelist;
    if (!h) return NULL;

    pg->freelist = h->next_free;
    h->next_free = NULL;

    atomic_fetch_add(&pg->inuse, 1);

#if SLAB_DEBUG
    // double-alloc 방지
    if (bitmap_test(pg->bitmap_words, h->slot_idx)) {
        debug_panic("bitmap already set on alloc");
    }
    bitmap_set(pg->bitmap_words, h->slot_idx);
#endif
    return h;
}

static void page_push(slab_page_t* pg, obj_hdr_t* h) {
#if SLAB_DEBUG
    // double-free 탐지
    if (!bitmap_test(pg->bitmap_words, h->slot_idx)) {
        debug_panic("double free detected");
    }
    bitmap_clear(pg->bitmap_words, h->slot_idx);
#endif

    h->next_free = pg->freelist;
    pg->freelist = h;

    atomic_fetch_sub(&pg->inuse, 1);
}

static slab_page_t* class_find_page_with_free(int sc_idx) {
    slab_page_t* pg = g_slab.class_pages[sc_idx];
    while (pg) {
        if (pg->freelist) return pg;
        pg = pg->next;
    }
    return NULL;
}

static void slab_lazy_init(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_slab.initialized, &expected, 1)) {
        for (int i = 0; i < 4; i++) atomic_flag_clear(&g_slab.class_locks[i]);

        // (선택) 각 클래스 1페이지 프리워밍
        for (int i = 0; i < 4; i++) {
            spin_lock(&g_slab.class_locks[i]);
            slab_page_t* pg = slab_page_create(i);
            if (pg) {
                pg->next = g_slab.class_pages[i];
                g_slab.class_pages[i] = pg;
            }
            spin_unlock(&g_slab.class_locks[i]);
        }
    }
}

int slab_init(const slab_config_t* cfg) {
    slab_lazy_init();
    if (cfg) {
        if (cfg->page_size >= 1024) g_slab.page_size = cfg->page_size;
        if (cfg->thread_cache_max > 0) g_slab.thread_cache_max = cfg->thread_cache_max;
        (void)cfg->enable_thread_cache; // 컴파일 옵션으로 결정
    }
    return 0;
}

void slab_shutdown(void) {
    if (!atomic_load(&g_slab.initialized)) return;

#if SLAB_ENABLE_THREAD_CACHE
    // TLS 캐시 flush (디버그 헤더가 있으므로 안전하게 global freelist로 반환)
    for (int i = 0; i < 4; i++) {
        while (tcache.head[i]) {
            obj_hdr_t* h = tcache.head[i];
            tcache.head[i] = h->next_free;
            tcache.count[i]--;

#if SLAB_DEBUG
            slab_page_t* pg = h->owner;
            if (!pg || pg->magic_page != SLAB_MAGIC_PAGE) debug_panic("bad page on shutdown flush");
            // tcache에 들어있는 것은 free 상태여야 함(bit=0). global freelist로 옮길 땐 page_push가 bit=0을 double-free로 봄.
            // 따라서 shutdown flush는 freelist 연결만 하되, bitmap/inuse는 건드리지 않는다(이미 free 상태 반영됨).
            // => freelist에 직접 push만 수행:
            spin_lock(&g_slab.class_locks[i]);
            h->next_free = pg->freelist;
            pg->freelist = h;
            spin_unlock(&g_slab.class_locks[i]);
#else
            (void)h;
#endif
        }
    }
#endif

    for (int i = 0; i < 4; i++) {
        spin_lock(&g_slab.class_locks[i]);
        slab_page_t* pg = g_slab.class_pages[i];
        g_slab.class_pages[i] = NULL;
        spin_unlock(&g_slab.class_locks[i]);

        while (pg) {
            slab_page_t* nxt = pg->next;
            slab_page_destroy(pg);
            pg = nxt;
        }
    }
}

static void* hdr_to_user(obj_hdr_t* h, int sc_idx) {
    uint8_t* p = (uint8_t*)h;
#if SLAB_DEBUG
    (void)sc_idx;
    return p + sizeof(obj_hdr_t) + SLAB_GUARD_SZ;
#else
    (void)sc_idx;
    return p + sizeof(obj_hdr_t);
#endif
}

static obj_hdr_t* user_to_hdr(void* user_ptr) {
    uint8_t* p = (uint8_t*)user_ptr;
#if SLAB_DEBUG
    return (obj_hdr_t*)(p - SLAB_GUARD_SZ - sizeof(obj_hdr_t));
#else
    return (obj_hdr_t*)(p - sizeof(obj_hdr_t));
#endif
}

void* slab_alloc(slab_size_class_t sc) {
    slab_lazy_init();
    atomic_fetch_add(&g_slab.alloc_calls, 1);

    int sc_idx = sc_to_index((size_t)sc);
    if (sc_idx < 0) return NULL;

#if SLAB_ENABLE_THREAD_CACHE
    // thread cache hit
    if (tcache.head[sc_idx]) {
        obj_hdr_t* h = tcache.head[sc_idx];
        tcache.head[sc_idx] = h->next_free;
        tcache.count[sc_idx]--;
        atomic_fetch_add(&g_slab.thread_cache_hits, 1);

#if SLAB_DEBUG
        // tcache에 넣을 때 bitmap/inuse를 free 상태로 반영했으므로,
        // alloc 시 다시 inuse/bitmap을 alloc 상태로 되돌린다.
        slab_page_t* pg = h->owner;
        if (!pg || pg->magic_page != SLAB_MAGIC_PAGE) debug_panic("bad page in tcache alloc");

        spin_lock(&g_slab.class_locks[sc_idx]);
        if (bitmap_test(pg->bitmap_words, h->slot_idx)) debug_panic("tcache alloc but bitmap already set");
        bitmap_set(pg->bitmap_words, h->slot_idx);
        atomic_fetch_add(&pg->inuse, 1);
        spin_unlock(&g_slab.class_locks[sc_idx]);

        // alloc 시 가드 확인(오염 탐지)
        uint8_t* slot = (uint8_t*)h;
        uint8_t* ga = slot + sizeof(obj_hdr_t);
        uint8_t* payload = ga + SLAB_GUARD_SZ;
        uint8_t* gb = payload + index_to_sc(sc_idx);
        guard_check(ga, SLAB_GUARD_A, SLAB_GUARD_SZ, "guard A (pre)");
        guard_check(gb, SLAB_GUARD_B, SLAB_GUARD_SZ, "guard B (pre)");
#endif
        return hdr_to_user(h, sc_idx);
    }
#endif

    // global path
    spin_lock(&g_slab.class_locks[sc_idx]);

    slab_page_t* pg = class_find_page_with_free(sc_idx);
    if (!pg) {
        pg = slab_page_create(sc_idx);
        if (!pg) {
            spin_unlock(&g_slab.class_locks[sc_idx]);
            return NULL;
        }
        pg->next = g_slab.class_pages[sc_idx];
        g_slab.class_pages[sc_idx] = pg;
    }

    obj_hdr_t* h = page_pop(pg);
    spin_unlock(&g_slab.class_locks[sc_idx]);
    if (!h) return NULL;

#if SLAB_DEBUG
    // alloc 시 가드 확인
    uint8_t* slot = (uint8_t*)h;
    uint8_t* ga = slot + sizeof(obj_hdr_t);
    uint8_t* payload = ga + SLAB_GUARD_SZ;
    uint8_t* gb = payload + index_to_sc(sc_idx);

    guard_check(ga, SLAB_GUARD_A, SLAB_GUARD_SZ, "guard A (pre)");
    guard_check(gb, SLAB_GUARD_B, SLAB_GUARD_SZ, "guard B (pre)");
#endif

    return hdr_to_user(h, sc_idx);
}

void slab_free(void* ptr) {
    if (!ptr) return;
    slab_lazy_init();
    atomic_fetch_add(&g_slab.free_calls, 1);

    obj_hdr_t* h = user_to_hdr(ptr);

#if SLAB_DEBUG
    if (h->magic_obj != SLAB_MAGIC_OBJ) debug_panic("free(): bad object magic");
    slab_page_t* pg = h->owner;
    if (!pg || pg->magic_page != SLAB_MAGIC_PAGE) debug_panic("free(): bad page magic");

    int sc_idx = (int)h->sc_idx;
    if (sc_idx < 0 || sc_idx > 3) debug_panic("free(): bad sc_idx");

    // guard check
    uint8_t* slot = (uint8_t*)h;
    uint8_t* ga = slot + sizeof(obj_hdr_t);
    uint8_t* payload = ga + SLAB_GUARD_SZ;
    uint8_t* gb = payload + index_to_sc(sc_idx);

    guard_check(ga, SLAB_GUARD_A, SLAB_GUARD_SZ, "guard A (free)");
    guard_check(gb, SLAB_GUARD_B, SLAB_GUARD_SZ, "guard B (free)");

#if SLAB_ENABLE_THREAD_CACHE
    if (tcache.count[sc_idx] < g_slab.thread_cache_max) {
        // 논리적 free 상태(비트/카운터)를 먼저 반영하고 TLS에 보관
        spin_lock(&g_slab.class_locks[sc_idx]);
        if (!bitmap_test(pg->bitmap_words, h->slot_idx)) debug_panic("double free detected (before tcache)");
        bitmap_clear(pg->bitmap_words, h->slot_idx);
        atomic_fetch_sub(&pg->inuse, 1);
        spin_unlock(&g_slab.class_locks[sc_idx]);

        h->next_free = tcache.head[sc_idx];
        tcache.head[sc_idx] = h;
        tcache.count[sc_idx]++;
        atomic_fetch_add(&g_slab.thread_cache_puts, 1);
        return;
    }
#endif

    // global freelist로 반환
    spin_lock(&g_slab.class_locks[sc_idx]);
    page_push(pg, h);
    spin_unlock(&g_slab.class_locks[sc_idx]);

#else
    // release 모드 안전 free는 이 템플릿 범위 밖(디버그 헤더 의존)
    (void)h;
    debug_panic("slab_free requires SLAB_DEBUG=1 in this template");
#endif
}

slab_stats_t slab_get_stats(void) {
    slab_stats_t s;
    memset(&s, 0, sizeof(s));
    s.alloc_calls       = atomic_load(&g_slab.alloc_calls);
    s.free_calls        = atomic_load(&g_slab.free_calls);
    s.page_allocs       = atomic_load(&g_slab.page_allocs);
    s.page_frees        = atomic_load(&g_slab.page_frees);
    s.thread_cache_hits = atomic_load(&g_slab.thread_cache_hits);
    s.thread_cache_puts = atomic_load(&g_slab.thread_cache_puts);
    return s;
}
