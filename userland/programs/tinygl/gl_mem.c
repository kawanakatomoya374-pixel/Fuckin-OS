/* TinyGL's allocator hooks, on top of the C-OS user heap (the kernel build uses kmalloc in src/third_party/tinygl/src/memory.c). */
#include <stdlib.h>
#include <string.h>

void gl_free(void *p) { free(p); }
void *gl_malloc(int size) { return size > 0 ? malloc((size_t)size) : NULL; }
void *gl_zalloc(int size) { void *p = gl_malloc(size); if (p) memset(p, 0, (size_t)size); return p; }
