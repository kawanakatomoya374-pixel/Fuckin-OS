#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* s);
void serial_puthex(uint64_t val);
void serial_putdec(uint64_t val);
char serial_getc(void);

#endif
