#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 고정 크기 클래스(바이트 단위)
typedef enum slab_size_class {
    SLAB_16  = 16,
    SLAB_32  = 32,
    SLAB_64  = 64,
    SLAB_128 = 128
} slab_size_class_t;

// 라이브러리 옵션 설정
typedef struct slab_config {
    size_t page_size;        // 기본 4096 권장
    int enable_thread_cache; // 0/1 (컴파일 옵션과 함께 사용 가능)
    size_t thread_cache_max; // per-thread 캐시에 보관할 최대 객체 수(클래스당)
} slab_config_t;

// 초기화/종료 (초기화 없이 호출하면 내부 기본값으로 lazy init)
int  slab_init(const slab_config_t* cfg);
void slab_shutdown(void);

// 핵심 API
void* slab_alloc(slab_size_class_t sc);
void  slab_free(void* ptr);

// 통계/디버그(선택)
typedef struct slab_stats {
    uint64_t alloc_calls;
    uint64_t free_calls;
    uint64_t page_allocs;
    uint64_t page_frees;
    uint64_t thread_cache_hits;
    uint64_t thread_cache_puts;
} slab_stats_t;

slab_stats_t slab_get_stats(void);

#ifdef __cplusplus
}
#endif
