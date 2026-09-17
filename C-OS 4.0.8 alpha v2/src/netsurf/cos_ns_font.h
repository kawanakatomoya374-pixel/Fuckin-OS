/**
 * cos_ns_font.h - CSS-aware text metrics and glyph synthesis.
 *
 * The single place a NetSurf plot_font_style_t becomes concrete metrics
 * and pixels. See cos_ns_font.c for why it exists: layout and the text
 * plotter were both discarding the style, so every page rendered in one
 * uniform font no matter what CSS said.
 *
 * THE INVARIANT
 * -------------
 * cos_ns_font_advance() is the only definition of how wide a character
 * is, and BOTH the layout callbacks (cos_ns_layout_width / _position /
 * _split) and the text plotter must use it. If measurement and drawing
 * disagree by even one pixel per character, text drifts out of the box
 * the engine reserved for it. validation/nsfont checks this directly.
 */
#ifndef COS_NS_FONT_H
#define COS_NS_FONT_H

/* This build's CFLAGS define PLOT_FONT_FAMILY_SANS_SERIF=1 as a numeric
 * macro (cos_nsoptions.c needs it as a source-level stand-in for a
 * config.h value desktop/options.h references). NetSurf's plot_style.h
 * declares the same name as an enum VALUE, so textually substituting it
 * with `1` turns `PLOT_FONT_FAMILY_SANS_SERIF = 0,` into `1 = 0,` - a
 * syntax error caught only by actually building through the real
 * Makefile with its real flags, not by an ad-hoc per-file syntax check.
 *
 * cos_netsurf_adapter.c already carries this exact undef for the same
 * reason. It is repeated here, before the include below, so that
 * cos_ns_font.h is self-contained for any translation unit that
 * includes it directly rather than depending on inclusion order. */
#undef PLOT_FONT_FAMILY_SANS_SERIF

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "netsurf/plot_style.h"

/* The largest mask cos_ns_font_build_mask() will produce. A page can ask
 * for font-size: 4000px; clamping is a policy choice that keeps the
 * per-glyph buffer on the stack. Text above the cap still lays out and
 * draws, just clamped - far better than vanishing. */
#define COS_NS_FONT_MAX_PX 128
#define COS_NS_FONT_MAX_W  (COS_NS_FONT_MAX_PX + 2)
#define COS_NS_FONT_MASK_BYTES (((COS_NS_FONT_MAX_W + 7) / 8) * COS_NS_FONT_MAX_PX)

typedef struct {
    int  height;          /* cell height in device pixels               */
    int  ascent;          /* baseline offset from the cell top          */
    int  descent;
    int  advance_narrow;  /* horizontal advance for a latin glyph       */
    int  advance_wide;    /* ... for a CJK/fullwidth glyph              */
    int  weight;          /* CSS 100-900, as computed                   */
    int  family;          /* plot_font_generic_family_t - recorded and
                           * ignored: there is one bitmap face, and
                           * inventing a "serif" by thickening stems
                           * would be a worse answer than an honest one  */
    int  slant_den;       /* italic shear: 1 px per this many rows      */
    bool bold;
    bool italic;
    bool smallcaps;
} cos_ns_font_t;

/* Resolves a CSS-computed style into metrics. `s` may be NULL, which
 * yields the native cell rather than a zero-height font. */
void cos_ns_font_from_style(const struct plot_font_style *s, cos_ns_font_t *out);

int  cos_ns_font_height(const cos_ns_font_t *f);
int  cos_ns_font_ascent(const cos_ns_font_t *f);

/* Horizontal advance for one codepoint. THE definition - see the
 * invariant above. Zero-width for joiners and combining marks, which
 * this font has no glyphs to compose with. */
int  cos_ns_font_advance(const cos_ns_font_t *f, uint32_t codepoint);

/* Total advance of a UTF-8 run. */
int  cos_ns_font_string_width(const cos_ns_font_t *f, const char *s, size_t len);

/* True for codepoints the 16x16 face covers at double width. Must agree
 * with what vga_glyph_mask() actually returns a wide mask for. */
bool cos_ns_font_is_wide(uint32_t codepoint);

/* Decodes one UTF-8 scalar. A malformed or truncated sequence consumes
 * exactly ONE byte, so a single bad byte cannot cascade into skipping
 * the start of the next valid character. */
uint32_t cos_ns_font_decode(const char *s, size_t remaining, size_t *consumed);

/* Builds the transformed 1bpp mask for one codepoint - resampled to the
 * font's height, emboldened and slanted as the style asks. Returns the
 * mask width (equal to the advance, so an opaque background covers the
 * whole cell with no seam), or 0 if nothing should be drawn.
 * `out_bits` must hold COS_NS_FONT_MASK_BYTES. */
int cos_ns_font_build_mask(const cos_ns_font_t *f, uint32_t codepoint,
                           uint8_t *out_bits, int *out_stride);

#endif /* COS_NS_FONT_H */
