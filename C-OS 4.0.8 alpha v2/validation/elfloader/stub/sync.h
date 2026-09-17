#ifndef STUB_SYNC_H
#define STUB_SYNC_H
#include <stdint.h>
static inline uint64_t sync_irq_save(void) { return 0; }
static inline void sync_irq_restore(uint64_t f) { (void)f; }
#endif
