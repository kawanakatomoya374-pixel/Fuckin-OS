/*
 * Memory allocator for TinyGL
 */
#include "zgl.h"
#include "memory.h"
#include <string.h>

void gl_free(void *p)
{
    if (p) {
        kfree(p);
    }
}

void *gl_malloc(int size)
{
    if (size <= 0) {
        return NULL;
    }
    return kmalloc((size_t)size);
}

void *gl_zalloc(int size)
{
    void *p = gl_malloc(size);
    if (p) {
        memset(p, 0, (size_t)size);
    }
    return p;
}
