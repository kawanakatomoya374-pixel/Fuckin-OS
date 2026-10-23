/**
 * test_nsfont.c - host tests for src/netsurf/cos_ns_font.c.
 *
 * The property these protect is not "the glyphs look nice" - they are
 * resampled bitmaps and at small sizes they will not. It is that the
 * GEOMETRY is right: that a CSS font-size produces text of that pixel
 * height, that measurement and drawing agree on every character's
 * advance, and that no input can make the mask builder write outside
 * its buffer.
 *
 * Geometry matters more than glyph quality here because wrong geometry
 * moves everything else on the page: a heading measured at body-text
 * width wraps in the wrong place, centres off-centre, and overflows its
 * box.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "netsurf/plot_style.h"
#include "cos_ns_font.h"

static int g_checks, g_failures;
static const char *g_case = "";
static void begin(const char *n) { g_case = n; }

#define CHECK(cond, msg, ...) do { g_checks++;                            \
    if (!(cond)) { g_failures++;                                          \
        fprintf(stderr, "FAIL [%s] " msg "\n", g_case, ##__VA_ARGS__); }  \
} while (0)

static plot_font_style_t style(double pt, int weight, int flags)
{
    plot_font_style_t s;
    memset(&s, 0, sizeof(s));
    s.size = (plot_style_fixed)(pt * PLOT_STYLE_SCALE);
    s.weight = weight;
    s.flags = (plot_font_flags_t)flags;
    s.family = PLOT_FONT_FAMILY_SANS_SERIF;
    return s;
}

/* px = pt * 90 / 72, matching nscss_screen_dpi's default of 90. */
static int expect_px(double pt) { return (int)(pt * 90.0 / 72.0 + 0.5); }

static void test_size(void)
{
    begin("font-size becomes real pixels");

    /* The default NetSurf font size is 12.8pt, which at 90dpi is 16px -
     * exactly the native cell, so the common case costs no resampling. */
    cos_ns_font_t f;
    plot_font_style_t s = style(12.8, 400, FONTF_NONE);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.height == 16, "12.8pt should be the native 16px cell, got %d", f.height);
    CHECK(f.advance_narrow == 8, "16px cell advance should be 8, got %d",
          f.advance_narrow);

    /* An <h1> is 2em = 25.6pt = 32px. The whole point: this must NOT
     * measure the same as body text, which is what the old code did. */
    s = style(25.6, 700, FONTF_NONE);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.height == 32, "25.6pt should be 32px, got %d", f.height);
    CHECK(f.bold, "weight 700 must be bold");
    CHECK(f.advance_narrow == 17, "32px bold advance should be 16+1, got %d",
          f.advance_narrow);

    /* A sweep: every size must land where the DPI conversion says. */
    for (double pt = 6.0; pt <= 72.0; pt += 0.5) {
        s = style(pt, 400, FONTF_NONE);
        cos_ns_font_from_style(&s, &f);
        int want = expect_px(pt);
        if (want < 6) want = 6;
        if (want > COS_NS_FONT_MAX_PX) want = COS_NS_FONT_MAX_PX;
        g_checks++;
        if (f.height != want) {
            g_failures++;
            fprintf(stderr, "FAIL [%s] %.1fpt -> %dpx, want %dpx\n",
                    g_case, pt, f.height, want);
            break;
        }
    }

    begin("font-size is clamped, not dropped");
    /* A page may legally ask for font-size: 4000px. The result must be
     * clamped and still drawable, not zero and not a buffer overrun. */
    s = style(10000.0, 400, FONTF_NONE);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.height == COS_NS_FONT_MAX_PX, "huge size not clamped: %d", f.height);
    CHECK(f.advance_narrow > 0, "clamped font has no advance");

    s = style(0.01, 400, FONTF_NONE);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.height >= 6, "tiny size collapsed to %d px", f.height);
    CHECK(f.advance_narrow >= 1, "tiny font has zero advance - text would "
                                 "occupy no width and overlap");

    /* A NULL style must not produce a zero-height font. */
    cos_ns_font_from_style(NULL, &f);
    CHECK(f.height == 16 && f.advance_narrow == 8,
          "NULL style should give the native cell, got %dx%d",
          f.advance_narrow, f.height);
}

static void test_weight_and_style(void)
{
    begin("font-weight and font-style");
    cos_ns_font_t f;

    for (int w = 100; w <= 900; w += 100) {
        plot_font_style_t s = style(12.8, w, FONTF_NONE);
        cos_ns_font_from_style(&s, &f);
        bool want_bold = (w >= 600);
        g_checks++;
        if (f.bold != want_bold) {
            g_failures++;
            fprintf(stderr, "FAIL [%s] weight %d bold=%d, want %d\n",
                    g_case, w, (int)f.bold, (int)want_bold);
        }
    }

    plot_font_style_t s = style(12.8, 400, FONTF_ITALIC);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.italic, "FONTF_ITALIC not honoured");
    /* Oblique is a distinct CSS value that synthesises the same way. */
    s = style(12.8, 400, FONTF_OBLIQUE);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.italic, "FONTF_OBLIQUE not honoured");

    /* Italic must NOT change the advance: slanting shifts ink within the
     * cell, it does not make the character wider. Widening it would make
     * every italic run measure differently from its upright equivalent
     * and reflow the page. */
    cos_ns_font_t upright;
    s = style(12.8, 400, FONTF_NONE);
    cos_ns_font_from_style(&s, &upright);
    s = style(12.8, 400, FONTF_ITALIC);
    cos_ns_font_from_style(&s, &f);
    CHECK(f.advance_narrow == upright.advance_narrow,
          "italic changed the advance (%d vs %d)",
          f.advance_narrow, upright.advance_narrow);
}

/* THE invariant: whatever the mask builder produces must be exactly as
 * wide as what the measurement function promised. */
static void test_measure_matches_draw(void)
{
    begin("measurement and drawing agree");

    static uint8_t mask[COS_NS_FONT_MASK_BYTES];
    const uint32_t samples[] = {
        'A', 'i', ' ', '0', '~', 0x7F,
        0x3042 /* hiragana A */, 0x4E9C /* CJK */, 0xFF21 /* fullwidth A */,
        0x00E9 /* e-acute */, 0x2014 /* em dash */,
    };

    for (double pt = 6.0; pt <= 60.0; pt += 1.0) {
        for (int bold = 0; bold <= 1; ++bold) {
            for (int ital = 0; ital <= 1; ++ital) {
                plot_font_style_t s = style(pt, bold ? 700 : 400,
                                            ital ? FONTF_ITALIC : FONTF_NONE);
                cos_ns_font_t f;
                cos_ns_font_from_style(&s, &f);

                for (size_t i = 0; i < sizeof(samples)/sizeof(samples[0]); ++i) {
                    int adv = cos_ns_font_advance(&f, samples[i]);
                    int stride = 0;
                    int w = cos_ns_font_build_mask(&f, samples[i], mask, &stride);
                    g_checks++;
                    if (w != adv) {
                        g_failures++;
                        fprintf(stderr, "FAIL [%s] U+%04X at %.0fpt b=%d i=%d: "
                                        "mask %dpx, advance %dpx\n",
                                g_case, samples[i], pt, bold, ital, w, adv);
                        return;
                    }
                    if (stride != (w + 7) / 8) {
                        g_failures++;
                        fprintf(stderr, "FAIL [%s] stride %d for width %d\n",
                                g_case, stride, w);
                        return;
                    }
                }
            }
        }
    }
}

static void test_string_width(void)
{
    begin("string measurement");
    plot_font_style_t s = style(12.8, 400, FONTF_NONE);
    cos_ns_font_t f;
    cos_ns_font_from_style(&s, &f);

    CHECK(cos_ns_font_string_width(&f, "", 0) == 0, "empty string is not zero wide");
    CHECK(cos_ns_font_string_width(&f, "abc", 3) == 24,
          "3 chars at 8px should be 24, got %d",
          cos_ns_font_string_width(&f, "abc", 3));

    /* A wide character must measure double. Getting this wrong makes
     * Japanese text overflow its box by a factor of two. */
    const char *ja = "\xE3\x81\x82";            /* U+3042 */
    CHECK(cos_ns_font_string_width(&f, ja, 3) == 16,
          "a fullwidth glyph should advance 16px, got %d",
          cos_ns_font_string_width(&f, ja, 3));

    /* Mixed runs sum correctly. */
    const char *mixed = "a\xE3\x81\x82" "b";
    CHECK(cos_ns_font_string_width(&f, mixed, 5) == 8 + 16 + 8,
          "mixed-width run measured %d, want 32",
          cos_ns_font_string_width(&f, mixed, 5));

    /* At double the size, everything doubles. */
    s = style(25.6, 400, FONTF_NONE);
    cos_ns_font_from_style(&s, &f);
    CHECK(cos_ns_font_string_width(&f, "abc", 3) == 48,
          "3 chars at 32px should be 48, got %d",
          cos_ns_font_string_width(&f, "abc", 3));
}

static void test_utf8(void)
{
    begin("UTF-8 decoding");
    size_t used;

    CHECK(cos_ns_font_decode("A", 1, &used) == 'A' && used == 1, "ASCII");
    CHECK(cos_ns_font_decode("\xC3\xA9", 2, &used) == 0xE9 && used == 2, "2-byte");
    CHECK(cos_ns_font_decode("\xE3\x81\x82", 3, &used) == 0x3042 && used == 3, "3-byte");
    CHECK(cos_ns_font_decode("\xF0\x9F\x98\x80", 4, &used) == 0x1F600 && used == 4,
          "4-byte");

    /* A truncated sequence must consume exactly ONE byte. Consuming the
     * whole would-be sequence can skip past the start of the next valid
     * character, turning one bad byte into a cascade. */
    cos_ns_font_decode("\xE3\x81", 2, &used);
    CHECK(used == 1, "truncated 3-byte sequence consumed %zu bytes, want 1", used);
    cos_ns_font_decode("\xE3\x41\x42", 3, &used);
    CHECK(used == 1, "malformed continuation consumed %zu bytes, want 1", used);
    cos_ns_font_decode("\x80", 1, &used);
    CHECK(used == 1, "stray continuation byte consumed %zu bytes", used);
    cos_ns_font_decode("", 0, &used);
    CHECK(used == 0, "empty input consumed %zu bytes", used);

    /* Walking a mixed string must never stall or run past the end. */
    const char *s = "a\xE3\x81\x82" "b\xC3\xA9\xF0\x9F\x98\x80z";
    size_t len = strlen(s), off = 0, steps = 0;
    while (off < len && steps < 100) {
        size_t step = 0;
        cos_ns_font_decode(s + off, len - off, &step);
        if (step == 0) break;
        off += step;
        steps++;
    }
    CHECK(off == len, "decoder walked %zu of %zu bytes", off, len);
}

static void test_mask_bounds(void)
{
    begin("mask synthesis stays in bounds");

    /* The buffer is deliberately over-allocated with a guard region that
     * must stay untouched at every size, weight and slant. A mask
     * builder that miscomputes its stride writes past the end, and on
     * the real system that is kernel memory. */
    enum { GUARD = 256 };
    static uint8_t buf[COS_NS_FONT_MASK_BYTES + GUARD];

    for (double pt = 4.0; pt <= 200.0; pt += 3.0) {
        for (int bold = 0; bold <= 1; ++bold) {
            for (int ital = 0; ital <= 1; ++ital) {
                memset(buf, 0xCC, sizeof(buf));
                plot_font_style_t s = style(pt, bold ? 900 : 100,
                                            ital ? FONTF_ITALIC : FONTF_NONE);
                cos_ns_font_t f;
                cos_ns_font_from_style(&s, &f);

                for (uint32_t cp = 0x20; cp < 0x80; ++cp) {
                    int stride = 0;
                    int w = cos_ns_font_build_mask(&f, cp, buf, &stride);
                    (void)w;
                }
                (void)cos_ns_font_build_mask(&f, 0x3042, buf, NULL);

                g_checks++;
                bool guard_ok = true;
                for (int i = 0; i < GUARD; ++i) {
                    if (buf[COS_NS_FONT_MASK_BYTES + i] != 0xCC) { guard_ok = false; break; }
                }
                if (!guard_ok) {
                    g_failures++;
                    fprintf(stderr, "FAIL [%s] mask overran its buffer at "
                                    "%.0fpt bold=%d italic=%d\n",
                            g_case, pt, bold, ital);
                    return;
                }
            }
        }
    }

    begin("mask actually contains ink");
    /* A mask builder that silently produced nothing would pass every
     * bounds check above. The fake font has ink on every row, so a
     * correctly resampled mask must too. */
    static uint8_t mask[COS_NS_FONT_MASK_BYTES];
    for (double pt = 6.0; pt <= 64.0; pt += 2.0) {
        plot_font_style_t s = style(pt, 400, FONTF_NONE);
        cos_ns_font_t f;
        cos_ns_font_from_style(&s, &f);
        int stride = 0;
        int w = cos_ns_font_build_mask(&f, 'A', mask, &stride);

        int rows_with_ink = 0;
        for (int r = 0; r < f.height; ++r) {
            for (int b = 0; b < stride; ++b) {
                if (mask[r * stride + b]) { rows_with_ink++; break; }
            }
        }
        g_checks++;
        if (rows_with_ink != f.height) {
            g_failures++;
            fprintf(stderr, "FAIL [%s] %.0fpt (%dpx, w=%d): only %d of %d rows "
                            "have ink - resampling is dropping rows\n",
                    g_case, pt, f.height, w, rows_with_ink, f.height);
            return;
        }
    }

    begin("bold really is wider than upright");
    /* Emboldening must add ink, not just advance. */
    cos_ns_font_t up, bd;
    plot_font_style_t su = style(25.6, 400, FONTF_NONE);
    plot_font_style_t sb = style(25.6, 700, FONTF_NONE);
    cos_ns_font_from_style(&su, &up);
    cos_ns_font_from_style(&sb, &bd);

    static uint8_t m1[COS_NS_FONT_MASK_BYTES], m2[COS_NS_FONT_MASK_BYTES];
    int st1 = 0, st2 = 0;
    cos_ns_font_build_mask(&up, 'A', m1, &st1);
    cos_ns_font_build_mask(&bd, 'A', m2, &st2);

    int ink1 = 0, ink2 = 0;
    for (int r = 0; r < up.height; ++r)
        for (int b = 0; b < st1; ++b)
            ink1 += __builtin_popcount(m1[r * st1 + b]);
    for (int r = 0; r < bd.height; ++r)
        for (int b = 0; b < st2; ++b)
            ink2 += __builtin_popcount(m2[r * st2 + b]);
    CHECK(ink2 > ink1, "bold has %d ink pixels vs upright %d - emboldening "
                       "is not adding anything", ink2, ink1);
}

int main(void)
{
    test_size();
    test_weight_and_style();
    test_measure_matches_draw();
    test_string_width();
    test_utf8();
    test_mask_bounds();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
