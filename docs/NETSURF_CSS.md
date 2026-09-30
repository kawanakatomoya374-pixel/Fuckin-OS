# NetSurf CSS rendering — closing the font/style gap

What this phase fixes, why it existed, and how it's verified.

Companion to `docs/ELF_LOADER.md` and `docs/COS_RUNTIME.md`.

---

## 1. The gap

libcss parses and cascades CSS correctly. NetSurf's `select.c` resolves
computed styles correctly. `layout.c` and `redraw.c` carry a fully
computed `plot_font_style_t` / `plot_style_t` all the way to the C-OS
frontend's plotter callbacks — and there, every one of them threw it
away:

```c
static nserror cos_ns_layout_width(const struct plot_font_style *style,
        const char *string, size_t length, int *width)
{
    (void)style;                                    /* <-- here */
    ...
    for (...) px += vga_get_font_width();            /* always 8px */
```

The visible result: **every page rendered in one uniform font.** An
`<h1>` measured exactly as wide as body text, so text wrapped in the
wrong place. `font-size: 40px` laid out as though it were 16px.
`font-weight: bold` and `font-style: italic` were invisible.
`border-style: dotted` and `dashed` drew solid. Border corners with a
fill drew as hollow outlines. CSS was computed perfectly and discarded
one function short of the screen.

## 2. What changed

| File | Change |
|---|---|
| `src/drivers/video/vga.h/.c` | Exposed raw 1bpp glyph masks (`vga_glyph_mask`, `vga_blit_mask`) so a caller can transform a glyph instead of only scaling it by a whole number |
| `src/netsurf/cos_ns_font.c/.h` | **New.** The single place a `plot_font_style_t` becomes concrete metrics and pixels |
| `src/netsurf/cos_netsurf_adapter.c` | Layout callbacks and the text plotter now go through `cos_ns_font.c`; rectangle/line stroking honours `stroke-width` and dotted/dashed patterns; polygon fill uses a real scanline rasteriser (was edges-only) |

### Font size becomes real

There is no outline rasteriser in this tree — the font is an 8×16 bitmap
(16×16 for the Japanese range). An arbitrary CSS pixel height is produced
by **nearest-neighbour resampling** the source glyph into a mask of the
requested size, at the row/column centre (not the edge, which would drop
the last row whenever the scale isn't an exact multiple and cut off
descenders).

This is genuinely worse-looking than a hinted outline font at small
sizes, and the code says so. What it buys: `font-size` is **real** — a
32px heading measures 32px, so it wraps, centres and overflows where the
layout engine computed it should. Getting the geometry right matters
more than glyph quality, because wrong geometry moves everything else on
the page.

### Bold and italic are synthesised, not selected

There is one bitmap face — no serif, no cursive, no separate bold
outline. `font-weight ≥ 600` OR-embolds each glyph one pixel right at
draw time (and the advance grows by one pixel to match — otherwise bold
text overruns its measured box). `font-style: italic`/`oblique` shears
each row rightward, more at the top than the bottom. **The advance does
not change for italic** — slanting moves ink within the cell, it doesn't
widen the character; widening it would reflow the page differently for
italic runs than upright ones.

`font-family` selects nothing and is recorded only. Inventing a "serif"
by thickening stems would be a worse answer than an honest one face.

### The one invariant

`cos_ns_font_advance()` is the **only** definition of how wide a
character is. Both the layout callbacks (`cos_ns_layout_width/_position/
_split`) and the text plotter call it. If they disagreed by even one
pixel per character, text would drift out of the box the engine reserved
for it — overflowing on the right, or leaving a growing gap. This is what
`validation/nsfont`'s `test_measure_matches_draw` checks directly, across
every size/weight/italic combination and eleven sample codepoints
including CJK and combining marks.

### Border styles and filled corners

- `plot_style_t.stroke_width` is now honoured on rectangles (was always
  1px).
- `PLOT_OP_TYPE_DOT`/`_DASH` now produce actual dotted/dashed runs
  (periods matching NetSurf's own framebuffer frontend), for
  axis-aligned strokes. A diagonal dotted/dashed line falls back to
  solid — patterning one correctly needs arc-length stepping, and a
  wrong pattern would be worse than none.
- `cos_ns_plot_polygon()` now does a real even-odd scanline fill.
  `redraw_border.c` draws every mitred border corner as a filled
  four-point polygon; the previous edge-only implementation left every
  border thicker than a hairline with hollow corners.

---

## 3. Testing

```sh
make check-nsfont        # just this
make check-host          # this + loader + runtime (3,226 checks total)
```

`validation/nsfont/` compiles the real `cos_ns_font.c` against a stub
glyph driver whose fake font has **ink on every row and edge column** —
so a resampling bug (a dropped row, a truncated column) shows up as
*wrong ink*, not as a mask that happens to look empty and passes anyway.

What's checked, **2,884 checks total**:

- `font-size` → pixels, swept from 6pt to 72pt against the DPI formula,
  and clamped (not dropped) at 0.01pt and 10,000px
- `font-weight` 100–900 → bold at exactly the CSS ≥600 boundary
- italic does not change the advance
- **measurement equals drawing** for every (size × bold × italic ×
  codepoint) combination in the sweep — the invariant
- string width for mixed narrow/wide runs
- UTF-8 decoding, including that a malformed/truncated sequence consumes
  exactly one byte (so it can't cascade into skipping a following valid
  character)
- mask synthesis never overruns its buffer, checked with a guard region
  at 68 (size, weight, italic) combinations × 65 codepoints each
- resampling doesn't silently drop rows (every row of the fake font's
  ink must survive)
- bold genuinely has more ink than upright, not just a wider advance

---

## 4. Known gaps

- **No outline font.** Everything is bitmap-resampled. Small sizes will
  look rougher than a hinted TrueType renderer; this trades that for
  correct geometry, which is the more visible failure mode in a browser
  (mislaid-out pages vs. slightly ugly small text).
- **`font-family` has no effect.** One face; the value is recorded and
  ignored rather than faked.
- **Diagonal dotted/dashed lines draw solid.** Only axis-aligned strokes
  (borders, `<hr>`, table rules — the overwhelming majority of what CSS
  actually asks to dash) get the pattern.
- **No subpixel/anti-aliased text.** Glyph edges are hard 1-bit.
- **`small-caps` is parsed into the style struct but not yet
  synthesised** — recorded in `cos_ns_font_t.smallcaps`, not acted on.
- **Bezier curves in `cos_ns_plot_path` are flattened at a fixed 16
  subdivisions** regardless of curve size — fine at page scale, visibly
  faceted if zoomed.
