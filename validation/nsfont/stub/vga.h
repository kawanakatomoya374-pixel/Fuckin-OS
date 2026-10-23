/* Host-test stub of the glyph accessor cos_ns_font.c needs. The fake
 * font is deliberately NOT blank: each glyph carries a recognisable
 * pattern so a resampling bug shows up as wrong ink rather than as
 * nothing at all. */
#ifndef STUB_VGA_H
#define STUB_VGA_H
#include <stdint.h>
const uint8_t *vga_glyph_mask(uint32_t codepoint, int *w, int *h, int *stride);
#endif
