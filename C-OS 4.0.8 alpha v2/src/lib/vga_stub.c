/**
 * vga_stub.c - VGA compatibility shim
 */

#include "vga.h"
#include "serial.h"

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

extern void vga_put_pixel(int x, int y, uint64_t color) WEAK;

void WEAK vga_set_color(uint64_t color) {
    (void)color;
}

void WEAK vga_printf(const char* format, ...) {
    (void)format;
    serial_puts("[VGA-FALLBACK] vga_printf ignored\n");
}
