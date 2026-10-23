/* cmem.h - Craft's own heap. libcos's malloc is a first-fit list that only merges forward; Craft allocates and frees
 * thousands of differently sized chunk/vertex buffers, which fragmented it until the process ran out of memory.
 * This one keeps a free list per size class (freed blocks are reused as-is, never split or merged), like libcos's worker arenas.
 * It is pulled in for every Craft source with -Dmalloc=cm_malloc etc. (see the Makefile). */
#ifndef CMEM_H
#define CMEM_H
#include <stddef.h>
void *cm_malloc(size_t n);
void  cm_free(void *p);
void *cm_calloc(size_t n, size_t m);
void *cm_realloc(void *p, size_t n);
#endif
