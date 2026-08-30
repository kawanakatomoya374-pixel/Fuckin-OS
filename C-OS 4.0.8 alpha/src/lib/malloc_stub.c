/**
 * malloc_stub.c - Freestanding libc allocation compatibility layer
 *
 * This object is archived as a compatibility provider, but third-party
 * libraries such as NetSurf may resolve bare malloc/free symbols from it.
 * It must therefore use the same ownership domain as the kernel allocator:
 * returning bump-allocator blocks while free resolves to kfree corrupts
 * URL/DOM allocations and produces invalid-pointer diagnostics.
 */
#include <stddef.h>
#include <stdint.h>

#include "memory.h"

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

void* WEAK malloc(size_t size) {
    return size ? kmalloc(size) : NULL;
}

void WEAK free(void* ptr) {
    if (ptr) kfree(ptr);
}

void* WEAK realloc(void* ptr, size_t size) {
    if (size == 0) {
        if (ptr) kfree(ptr);
        return NULL;
    }
    return krealloc(ptr, size);
}

void* WEAK calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    if (total == 0) return NULL;
    uint8_t* ptr = (uint8_t*)kmalloc(total);
    if (!ptr) return NULL;
    for (size_t i = 0; i < total; ++i) ptr[i] = 0;
    return ptr;
}
