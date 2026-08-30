/**
 * ffsystem.c - C-OS kernel bindings for FatFs system hooks
 *
 * Replaces the stock ffsystem.c (which only has platform sections for
 * Windows/ITRON/FreeRTOS/CMSIS) with straightforward kmalloc/kfree
 * bindings. get_fattime() isn't needed since ffconf.h has
 * FF_FS_NORTC = 1 (fixed timestamp, no RTC wiring required).
 */
#include "ff.h"
#include "memory.h"

#if FF_USE_LFN == 3

void* ff_memalloc(UINT msize) {
    return kmalloc((size_t)msize);
}

void ff_memfree(void* mblock) {
    kfree(mblock);
}

#endif
