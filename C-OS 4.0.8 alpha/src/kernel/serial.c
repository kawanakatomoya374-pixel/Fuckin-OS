#include "io.h"
#include "serial.h"

#define SERIAL_PORT 0x3F8

void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x03);
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
    outb(SERIAL_PORT + 4, 0x0B);
}

void serial_putc(char c) {
    while (!(inb(SERIAL_PORT + 5) & 0x20));
    outb(SERIAL_PORT, c);
}

void serial_puts(const char* s) {
    while (*s) {
        serial_putc(*s++);
    }
}

void serial_puthex(uint64_t val) {
    const char* hex = "0123456789ABCDEF";
    for (int i = 60; i >= 0; i -= 4) {
        serial_putc(hex[(val >> i) & 0xF]);
    }
}

void serial_putdec(uint64_t val) {
    if (val == 0) {
        serial_putc('0');
        return;
    }
    
    /* UINT64_MAX (18446744073709551615) is 20 digits - this buffer must
     * hold the worst case. It was previously only 11 bytes, which any
     * caller passing a negative int (implicitly converted to a huge
     * unsigned value close to UINT64_MAX) would overflow by up to 9
     * bytes, smashing adjacent stack contents including, in the crash
     * this fixes, the return address itself. */
    char buffer[20];
    int i = 0;
    
    while (val > 0 && i < (int)sizeof(buffer)) {
        buffer[i++] = '0' + (val % 10);
        val /= 10;
    }
    
    while (i > 0) {
        serial_putc(buffer[--i]);
    }
}

char serial_getc(void) {
    while (!(inb(SERIAL_PORT + 5) & 0x01));
    return inb(SERIAL_PORT);
}
