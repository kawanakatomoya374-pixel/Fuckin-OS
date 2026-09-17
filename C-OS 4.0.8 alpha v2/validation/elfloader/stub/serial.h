#ifndef STUB_SERIAL_H
#define STUB_SERIAL_H
#include <stdint.h>
void serial_putc(char c);
void serial_puts(const char* s);
void serial_puthex(uint64_t v);
void serial_putdec(uint64_t v);
#endif
