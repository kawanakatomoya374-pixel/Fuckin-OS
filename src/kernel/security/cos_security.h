#ifndef COS_SECURITY_H
#define COS_SECURITY_H
#include "types.h"

/* Per-CPU hardening (SMEP: ring 0 can never execute a user page). Call on the BSP and on every AP. */
void cos_security_harden_cpu(void);
/* Unpredictable 64-bit value: RDRAND when the CPU has it, otherwise a TSC/timer-seeded generator. */
uint64_t cos_random_u64(void);


/* SMAP: ring 0 must not touch user memory except on purpose. The interrupt stubs clear AC on entry; code that deliberately
 * accesses another address space's user pages (ELF loader, TLS setup) brackets that with these. */
extern volatile uint8_t cos_smap_active;
static inline void cos_user_access_begin(void) { if (cos_smap_active) __asm__ volatile("stac" ::: "cc"); }
static inline void cos_user_access_end(void)   { if (cos_smap_active) __asm__ volatile("clac" ::: "cc"); }

#endif
