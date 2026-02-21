#include "../include/slab.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    slab_config_t cfg = {
        .page_size = 4096,
        .enable_thread_cache = 1,
        .thread_cache_max = 32
    };
    slab_init(&cfg);

    void* p = slab_alloc(SLAB_64);
    if (!p) {
        printf("alloc failed\n");
        return 1;
    }

    memset(p, 0x11, 64);
    slab_free(p);

#if 1
    // 더블 프리 테스트(디버그에서 abort 기대)
    // slab_free(p);
#endif

    slab_stats_t st = slab_get_stats();
    printf("alloc=%llu free=%llu page_allocs=%llu\n",
           (unsigned long long)st.alloc_calls,
           (unsigned long long)st.free_calls,
           (unsigned long long)st.page_allocs);

    slab_shutdown();
    return 0;
}
