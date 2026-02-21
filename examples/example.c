#include "../include/slab.h"
#include <stdio.h>

int main(void) {
    slab_init(NULL);

    void* a = slab_alloc(SLAB_16);
    void* b = slab_alloc(SLAB_128);

    printf("a=%p b=%p\n", a, b);

    slab_free(a);
    slab_free(b);

    slab_shutdown();
    return 0;
}
