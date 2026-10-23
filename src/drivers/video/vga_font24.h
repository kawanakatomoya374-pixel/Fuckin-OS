#ifndef VGA_FONT24_H
#define VGA_FONT24_H
#include <stdint.h>
typedef struct {
    uint16_t codepoint;
    uint8_t rows[16];
} vga_font24_glyph_t;
#endif
