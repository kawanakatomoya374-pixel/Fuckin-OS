/* Host shim for the kernel's memory.h: kmalloc/kfree map to malloc/free,
 * with optional accounting so tests can detect leaks and cap allocations. */
#ifndef IMGTEST_SHIM_MEMORY_H
#define IMGTEST_SHIM_MEMORY_H
#include <stddef.h>
#include <stdint.h>
void* kmalloc(size_t n);
void  kfree(void* p);
extern long   imgtest_live_allocs;      /* outstanding kmalloc blocks */
extern size_t imgtest_live_bytes;       /* outstanding bytes */
extern size_t imgtest_peak_bytes;       /* high-water mark since last reset */
extern size_t imgtest_alloc_limit;      /* single-allocation cap (0 = none) */
#endif
