#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifndef SLAB_PAGE_SIZE_DEFAULT
#define SLAB_PAGE_SIZE_DEFAULT 4096u
#endif

// 컴파일 옵션:
// -DSLAB_DEBUG=1              : 가드패턴/더블프리/오버런 검사 활성화
// -DSLAB_ENABLE_THREAD_CACHE=1: per-thread cache 활성화

#ifndef SLAB_DEBUG
#define SLAB_DEBUG 0
#endif

#ifndef SLAB_ENABLE_THREAD_CACHE
#define SLAB_ENABLE_THREAD_CACHE 0
#endif

#define SLAB_MAGIC_PAGE  0x534C4142u  // "SLAB"
#define SLAB_MAGIC_OBJ   0x0B1EC7u

#if SLAB_DEBUG
// 디버그: redzone + header
// [obj_hdr][guard A][payload(user_size)][guard B]
#define SLAB_GUARD_A 0xA5
#define SLAB_GUARD_B 0x5A
#define SLAB_GUARD_SZ 16u
#else
#define SLAB_GUARD_SZ 0u
#endif

typedef struct slab_page slab_page_t;

typedef struct obj_hdr {
#if SLAB_DEBUG
    uint32_t magic_obj;
    uint16_t sc_idx;     // size class index (0..3)
    uint16_t slot_idx;   // page 내 slot index
    slab_page_t* owner;
#endif
    struct obj_hdr* next_free; // free list 연결(헤더를 free node로 재사용)
} obj_hdr_t;

typedef struct slab_page {
    uint32_t magic_page;
    uint16_t sc_idx;         // size class index
    uint16_t capacity;       // slot 개수
    _Atomic(uint32_t) inuse;  // 사용중 slot 수
    slab_page_t* next;        // 글로벌 리스트 연결
    obj_hdr_t* freelist;      // 페이지 내부 free list

#if SLAB_DEBUG
    // 더블 프리 탐지: slot 당 1bit (0=free,1=inuse)
    // capacity 최대가 커질 수 있어 가변 길이로 page 뒤에 붙여 저장
    // bitmap은 slab_page_alloc()에서 page 메모리 내부에 배치
    uint32_t* bitmap_words;
    uint32_t bitmap_word_count;
#endif

    // page의 raw 메모리 시작(바로 뒤에 slot 영역/bitmap이 붙음)
    uint8_t* base;
} slab_page_t;

typedef struct slab_global {
    size_t page_size;
    size_t thread_cache_max;
    _Atomic(int) initialized;

    // 4개의 size class
    slab_page_t* class_pages[4];  // 각 클래스의 page list head
    atomic_flag class_locks[4];   // 간단한 spin lock

    _Atomic(uint64_t) alloc_calls;
    _Atomic(uint64_t) free_calls;
    _Atomic(uint64_t) page_allocs;
    _Atomic(uint64_t) page_frees;
    _Atomic(uint64_t) thread_cache_hits;
    _Atomic(uint64_t) thread_cache_puts;
} slab_global_t;

extern slab_global_t g_slab;

static inline int sc_to_index(size_t sc) {
    switch (sc) {
        case 16:  return 0;
        case 32:  return 1;
        case 64:  return 2;
        case 128: return 3;
        default:  return -1;
    }
}

static inline size_t index_to_sc(int idx) {
    static const size_t k[4] = {16,32,64,128};
    return k[idx];
}

static inline void spin_lock(atomic_flag* f) {
    while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) { /* spin */ }
}
static inline void spin_unlock(atomic_flag* f) {
    atomic_flag_clear_explicit(f, memory_order_release);
}
