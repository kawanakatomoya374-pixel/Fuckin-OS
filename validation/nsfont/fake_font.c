#include <stdint.h>
#include <string.h>

/* An 8x16 latin cell and a 16x16 wide cell, filled with a pattern that
 * has ink on every row and in the first and last columns. That makes
 * resampling errors visible: a dropped edge row or column changes the
 * mask's bounding box, which the tests assert on. */
static uint8_t narrow[16 * 1];
static uint8_t wide[16 * 2];
static int built;

static void build(void)
{
    for (int r = 0; r < 16; ++r) {
        /* left and right edge columns set, plus a diagonal */
        narrow[r] = (uint8_t)(0x81u | (0x80u >> (r % 8)));
        wide[r * 2 + 0] = 0x80u | (uint8_t)(0x80u >> (r % 8));
        wide[r * 2 + 1] = 0x01u;
    }
    built = 1;
}

const uint8_t *vga_glyph_mask(uint32_t codepoint, int *w, int *h, int *stride)
{
    if (!built) build();
    /* Mirrors the real driver: a codepoint above 0xFF that the wide face
     * does not cover falls back to the narrow '?' glyph. The tests rely
     * on this matching cos_ns_font_is_wide(). */
    int is_wide = (codepoint >= 0x3041u && codepoint <= 0x9FFFu);
    if (is_wide) { *w = 16; *h = 16; *stride = 2; return wide; }
    *w = 8; *h = 16; *stride = 1; return narrow;
}
