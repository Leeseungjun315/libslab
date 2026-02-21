#include "../include/slab.h"
#include <stdio.h>

int main(void) {
    slab_init(NULL);

    void* p = slab_alloc(SLAB_32);
    if (!p) {
        printf("alloc failed\n");
        return 1;
    }
    slab_free(p);
    printf("About to double-free (expected abort in SLAB_DEBUG=1)...\n");
    slab_free(p); // expected abort

    slab_shutdown();
    return 0;
}
