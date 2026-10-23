/* Host shim for the kernel's serial.h. */
#ifndef IMGTEST_SHIM_SERIAL_H
#define IMGTEST_SHIM_SERIAL_H
#include <stdint.h>
void serial_puts(const char* s);
void serial_putdec(uint64_t v);
void serial_puthex(uint64_t v);
#endif
