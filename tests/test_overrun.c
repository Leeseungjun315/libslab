#include "../include/slab.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int main(void) {
    slab_init(NULL);

    uint8_t* p = (uint8_t*)slab_alloc(SLAB_64);
    if (!p) {
        printf("alloc failed\n");
        return 1;
    }

    // 의도적으로 64바이트보다 더 써서 guard B를 깨뜨림
    printf("About to overflow 64-byte object by 16 bytes (expected abort on free in SLAB_DEBUG=1)...\n");
    memset(p, 0xAB, 64 + 16);

    slab_free(p); // expected abort (guard corrupted)
    slab_shutdown();
    return 0;
}
