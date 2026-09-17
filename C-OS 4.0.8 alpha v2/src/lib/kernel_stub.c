/**
 * kernel_stub.c - Compatibility entry for small modules
 */

#include "types.h"
#include "serial.h"

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

void WEAK kernel_stub_init(void) {
    serial_puts("[KERNEL-STUB] init\n");
}
