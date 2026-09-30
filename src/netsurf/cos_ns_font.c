/**
 * cos_ns_font.c - turns a CSS-computed font style into metrics and pixels.
 *
 * THE GAP THIS CLOSES
 * -------------------
 * libcss computes `font-size`, `font-weight` and `font-style` correctly;
 * NetSurf's select handler resolves them; layout.c and redraw.c carry
 * them faithfully into a `plot_font_style_t`. And then both ends of this
 * port threw them away:
 *
 *     static nserror cos_ns_layout_width(const struct plot_font_style *style,
 *             const char *string, size_t length, int *width)
 *     {
 *         (void)style;                                   <-- here
 *
 * and the same `(void)` in the position and split callbacks, and a text
 * plotter that called vga_draw_string_len() with the one fixed bitmap
 * size. The visible result: every page rendered in a single uniform
 * font. An `<h1>` occupied exactly as much width as body text, so it
 * wrapped in the wrong place; `font-size: 40px` laid out as though it
 * were 16px; bold and italic were invisible. CSS was being computed
 * perfectly and then discarded one function short of the screen.
 *
 * HOW SIZE IS HONOURED WITHOUT A SCALABLE FONT
 * --------------------------------------------
 * The font is an 8x16 bitmap (16x16 for the Japanese range). There is no
 * outline rasteriser in this tree. So an arbitrary CSS pixel height is
 * produced by nearest-neighbour resampling the source glyph into a mask
 * of the requested size.
 *
 * That is genuinely worse-looking than a hinted outline font at small
 * sizes, and it is not pretending otherwise. What it buys is that
 * `font-size` becomes REAL: a 24px heading is 24 pixels tall and
 * measures 24-pixel-wide advances, so it wraps, centres and overflows
 * exactly where the layout engine computed it should. Getting the
 * geometry right matters more than glyph quality, because wrong
 * geometry moves everything else on the page.
 *
 * MEASUREMENT AND DRAWING MUST AGREE
 * ----------------------------------
 * cos_ns_font_advance() is the single source of truth for how wide a
 * character is, and BOTH the layout callbacks and the text plotter go
 * through it. If they disagreed by even one pixel per character, text
 * would drift out of the box the engine reserved for it - overflowing
 * on the right, or leaving a growing gap. This is the invariant the host
 * test in validation/nsfont checks directly.
 *
 * WHAT CANNOT BE HONOURED
 * -----------------------
 * `font-family` selects nothing. There is one bitmap face; there is no
 * serif, no cursive, no fantasy. Monospace is indistinguishable because
 * the one face is already monospace. Rather than fake a difference, the
 * family is recorded and ignored, and this comment is the note saying
 * so - an invented "serif" made by thickening stems would be a worse
 * answer than an honest one face.
 */
#include "cos_ns_font.h"

#include <string.h>

#include "vga.h"

/* NetSurf's plot_font_style_t carries `size` in POINTS, as fixed point
 * with PLOT_STYLE_RADIX (10) fractional bits. Converting to device
 * pixels needs the same DPI the CSS cascade used, which is
 * nscss_screen_dpi - defaulting to 90 in content/handlers/css/css.c.
 *
 * Using a different DPI here than the cascade used is a subtle and
 * miserable bug: lengths already converted to px by libcss would be
 * consistent with one value while text metrics used another, so a box
 * sized in `em` would not match the text inside it. */
#ifndef COS_NS_FONT_DPI
#define COS_NS_FONT_DPI 90
#endif

/* The native cell the bitmap font is drawn at. */
#define BASE_W 8
#define BASE_H 16
#define WIDE_W 16

/* Bounds on the synthesised mask. A CSS page can legally ask for
 * font-size: 4000px; refusing to build a mask that large is a policy
 * decision, not a format one - the alternative is a multi-megabyte
 * stack buffer per glyph. Text above the cap still lays out and draws,
 * just clamped, which is far better than vanishing. */
#define COS_NS_FONT_MIN_PX 6
#define COS_NS_FONT_MAX_PX 128

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void cos_ns_font_from_style(const struct plot_font_style *s, cos_ns_font_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Default to the native cell when there is no style at all, so a
     * caller that has not got one still gets sane metrics rather than a
     * zero-height font. */
    int px = BASE_H;

    if (s != NULL) {
        /* size is pt in 10.10 fixed point: px = pt * dpi / 72. Done in
         * integer arithmetic with the rounding explicit, rather than
         * through a float, because this runs inside layout for every
         * text run on the page. */
        int64_t pt_fixed = (int64_t)s->size;
        int64_t px_fixed = (pt_fixed * COS_NS_FONT_DPI) / 72;
        px = (int)((px_fixed + (1 << 9)) >> 10);   /* round to nearest */

        out->weight = s->weight;
        out->bold   = (s->weight >= 600);
        out->italic = (s->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;
        out->smallcaps = (s->flags & FONTF_SMALLCAPS) != 0;
        out->family = (int)s->family;
    } else {
        out->weight = 400;
    }

    out->height = clampi(px, COS_NS_FONT_MIN_PX, COS_NS_FONT_MAX_PX);

    /* Latin advance scales with the cell: 8/16 of the height. Rounded up
     * so that two adjacent glyphs never overlap, which would look like
     * kerning damage. */
    out->advance_narrow = (out->height * BASE_W + BASE_H - 1) / BASE_H;
    out->advance_wide   = (out->height * WIDE_W + BASE_H - 1) / BASE_H;

    /* Emboldening is done by OR-ing the glyph with itself shifted one
     * pixel right, so it really is one pixel wider and the advance has
     * to say so - otherwise bold text overruns its measured box. */
    if (out->bold) {
        out->advance_narrow += 1;
        out->advance_wide += 1;
    }

    /* Baseline at ~80% of the cell, matching where the bitmap font puts
     * it. NetSurf plots text by BASELINE y, so this is what converts
     * that to the mask's top edge. */
    out->ascent = (out->height * 13 + 8) / 16;
    out->descent = out->height - out->ascent;

    /* The slant used for synthesised italics: one pixel of horizontal
     * shift per four rows. Steeper looks like a glitch at these sizes. */
    out->slant_den = 4;
}

int cos_ns_font_height(const cos_ns_font_t *f)
{
    return f ? f->height : BASE_H;
}

int cos_ns_font_ascent(const cos_ns_font_t *f)
{
    return f ? f->ascent : ((BASE_H * 13 + 8) / 16);
}

bool cos_ns_font_is_wide(uint32_t codepoint)
{
    /* The ranges the 16x16 Japanese face covers, plus the fullwidth
     * forms block. Anything else advances at the narrow width.
     *
     * This has to agree with what vga_glyph_mask() actually returns a
     * 16-wide mask for, or measurement and drawing disagree - the one
     * bug class this module exists to prevent. vga_glyph_mask() falls
     * back to '?' (narrow) for a codepoint above 0xFF with no wide
     * glyph, so the predicate is deliberately conservative: it reports
     * wide only for ranges the face is known to populate. */
    if (codepoint < 0x1100u) return false;
    return (codepoint >= 0x1100u  && codepoint <= 0x115Fu) ||  /* Hangul jamo   */
           (codepoint >= 0x2E80u  && codepoint <= 0x303Eu) ||  /* CJK radicals  */
           (codepoint >= 0x3041u  && codepoint <= 0x33FFu) ||  /* kana, CJK misc*/
           (codepoint >= 0x3400u  && codepoint <= 0x4DBFu) ||  /* CJK ext A     */
           (codepoint >= 0x4E00u  && codepoint <= 0x9FFFu) ||  /* CJK unified   */
           (codepoint >= 0xA000u  && codepoint <= 0xA4CFu) ||  /* Yi            */
           (codepoint >= 0xAC00u  && codepoint <= 0xD7A3u) ||  /* Hangul        */
           (codepoint >= 0xF900u  && codepoint <= 0xFAFFu) ||  /* CJK compat    */
           (codepoint >= 0xFE30u  && codepoint <= 0xFE6Fu) ||  /* CJK forms     */
           (codepoint >= 0xFF00u  && codepoint <= 0xFF60u) ||  /* fullwidth     */
           (codepoint >= 0xFFE0u  && codepoint <= 0xFFE6u);
}

int cos_ns_font_advance(const cos_ns_font_t *f, uint32_t codepoint)
{
    if (!f) return BASE_W;
    /* A zero-width joiner or a combining mark should not advance, but
     * this font has no combining glyphs to compose with, so treating
     * them as advancing would insert visible gaps where a real shaper
     * would insert none. Reported as zero-width; the glyph draws
     * nothing. */
    if (codepoint == 0x200Du || codepoint == 0xFEFFu) return 0;
    if (codepoint >= 0x0300u && codepoint <= 0x036Fu) return 0;
    return cos_ns_font_is_wide(codepoint) ? f->advance_wide : f->advance_narrow;
}

/* ---- UTF-8 -------------------------------------------------------------- */

uint32_t cos_ns_font_decode(const char *s, size_t remaining, size_t *consumed)
{
    if (consumed) *consumed = 0;
    if (!s || remaining == 0) return 0;

    unsigned char c = (unsigned char)s[0];
    if (c < 0x80u) { if (consumed) *consumed = 1; return c; }

    size_t need = ((c & 0xE0u) == 0xC0u) ? 2 :
                  ((c & 0xF0u) == 0xE0u) ? 3 :
                  ((c & 0xF8u) == 0xF0u) ? 4 : 1;

    /* A truncated or malformed sequence consumes exactly one byte and is
     * rendered by the replacement glyph. Consuming the whole would-be
     * sequence instead can skip past the start of the NEXT valid
     * character, which turns one bad byte into a cascade of them. */
    if (need == 1 || remaining < need) { if (consumed) *consumed = 1; return c; }
    for (size_t i = 1; i < need; ++i) {
        if (((unsigned char)s[i] & 0xC0u) != 0x80u) {
            if (consumed) *consumed = 1;
            return c;
        }
    }

    uint32_t v = (need == 2) ? (uint32_t)(c & 0x1Fu) :
                 (need == 3) ? (uint32_t)(c & 0x0Fu) : (uint32_t)(c & 0x07u);
    for (size_t i = 1; i < need; ++i) {
        v = (v << 6) | ((unsigned char)s[i] & 0x3Fu);
    }
    if (consumed) *consumed = need;
    return v;
}

int cos_ns_font_string_width(const cos_ns_font_t *f, const char *s, size_t len)
{
    int px = 0;
    for (size_t off = 0; off < len; ) {
        size_t step = 0;
        uint32_t cp = cos_ns_font_decode(s + off, len - off, &step);
        if (step == 0) break;
        px += cos_ns_font_advance(f, cp);
        off += step;
    }
    return px;
}

/* ---- glyph synthesis ---------------------------------------------------- */

/* Builds the transformed 1bpp mask for one codepoint: resampled to the
 * font's pixel height, emboldened and slanted as the style asks.
 *
 * Returns the mask width, or 0 if nothing should be drawn. `out_bits`
 * must be at least COS_NS_FONT_MASK_BYTES.
 */
int cos_ns_font_build_mask(const cos_ns_font_t *f, uint32_t codepoint,
                           uint8_t *out_bits, int *out_stride)
{
    if (!f || !out_bits) return 0;

    int src_w = 0, src_h = 0, src_stride = 0;
    const uint8_t *src = vga_glyph_mask(codepoint, &src_w, &src_h, &src_stride);
    if (!src || src_w <= 0 || src_h <= 0) return 0;

    int dst_h = f->height;
    /* The mask is as wide as the advance so the caller can blit an
     * opaque background across the full cell without leaving a seam
     * between characters. */
    int dst_w = cos_ns_font_advance(f, codepoint);
    if (dst_w <= 0 || dst_h <= 0) return 0;
    if (dst_w > COS_NS_FONT_MAX_W) dst_w = COS_NS_FONT_MAX_W;
    if (dst_h > COS_NS_FONT_MAX_PX) dst_h = COS_NS_FONT_MAX_PX;

    int stride = (dst_w + 7) / 8;
    memset(out_bits, 0, (size_t)stride * (size_t)dst_h);

    /* The glyph's ink occupies src_w columns of an 8- or 16-wide cell;
     * resample only that, into the same proportion of the destination,
     * so a bold advance's extra pixel becomes trailing space rather
     * than stretching the letterform. */
    int ink_w = dst_w - (f->bold ? 1 : 0);
    if (ink_w <= 0) ink_w = dst_w;

    for (int dy = 0; dy < dst_h; ++dy) {
        /* Nearest neighbour, sampling the centre of the destination
         * pixel. Sampling the top-left edge instead drops the last
         * source row whenever the scale is not an exact multiple, which
         * cuts the baseline off descenders. */
        int sy = ((dy * 2 + 1) * src_h) / (dst_h * 2);
        if (sy >= src_h) sy = src_h - 1;

        /* Italic shear: rows nearer the top shift further right. Applied
         * per row rather than per glyph so the slant is continuous
         * across the cell. */
        int shear = 0;
        if (f->italic && f->slant_den > 0) {
            shear = (dst_h - 1 - dy) / f->slant_den;
        }

        for (int dx = 0; dx < ink_w; ++dx) {
            int sx = ((dx * 2 + 1) * src_w) / (ink_w * 2);
            if (sx >= src_w) sx = src_w - 1;

            const uint8_t byte = src[sy * src_stride + (sx >> 3)];
            if (!(byte & (uint8_t)(0x80u >> (sx & 7)))) continue;

            int px = dx + shear;
            if (px >= 0 && px < dst_w) {
                out_bits[dy * stride + (px >> 3)] |= (uint8_t)(0x80u >> (px & 7));
            }
            /* Emboldening: the same pixel again, one column right. Done
             * here rather than as a second pass so it picks up the shear
             * automatically. */
            if (f->bold && px + 1 >= 0 && px + 1 < dst_w) {
                out_bits[dy * stride + ((px + 1) >> 3)] |=
                    (uint8_t)(0x80u >> ((px + 1) & 7));
            }
        }
    }

    if (out_stride) *out_stride = stride;
    return dst_w;
}
