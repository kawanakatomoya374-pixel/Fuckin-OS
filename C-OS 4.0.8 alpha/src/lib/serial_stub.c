/**
 * serial_stub.c - Serial compatibility shim
 *
 * Weak no-op fallbacks for tiny builds. Real serial.c should override these.
 */

#include "serial.h"

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

void WEAK serial_puts(const char* str) {
    (void)str;
}

void WEAK serial_putdec(uint64_t value) {
    (void)value;
}

void WEAK serial_puthex(uint64_t value) {
    (void)value;
}
