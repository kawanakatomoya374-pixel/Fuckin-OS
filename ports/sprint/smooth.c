/* The smooth font of Sprint's gfx text on C-OS. Sprint draws text with a 5x7 bitmap font (6 pixel cells, 12 pixel Japanese glyphs). Settings > Sprint font chooses between
 * that ("Retro", /system/sprint_font = "retro") and C-OS's own proportional font ("C-OS", the default): the same canvas, the same positions, the glyphs come from the
 * font of the C-OS user interface (Inter, with Noto Sans JP behind it). The cap height of the smooth font is matched to the 7 rows of the bitmap font, so layouts that were made for
 * the bitmap font keep their line spacing; widths are the real ones (gfx.text_advance answers with them). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include "cos_ui.h"

int sp_smooth_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        char b[16]; ssize_t n = cos_read_file("/system/sprint_font", b, sizeof b - 1);
        mode = (n >= 5 && !strncmp(b, "retro", 5)) ? 0 : 1;
    }
    return mode;
}

#define MAXSCALE 8
static cui_font *fonts[MAXSCALE + 1];
static cui_font *font_for(int scale) {
    if (scale < 1) scale = 1; if (scale > MAXSCALE) scale = MAXSCALE;
    if (!fonts[scale]) {
        static uint8_t *ttf; static int tried;
        int px = 11 * scale; cui_font *f = cui_font_default(px);
        if (!tried) {
            tried = 1; cos_stat_t st;
            if (cos_stat("/system/fonts/NotoSansJP-Regular.ttf", &st) == 0 && st.size > 0 && st.size < (8u << 20) && (ttf = malloc(st.size)) && cos_read_file("/system/fonts/NotoSansJP-Regular.ttf", ttf, st.size) != (ssize_t)st.size) { free(ttf); ttf = NULL; }
        }
        if (ttf) cui_font_set_fallback(f, cui_font_load(ttf, px));
        fonts[scale] = f;
    }
    return fonts[scale];
}

/* the widest line, in pixels */
int sp_smooth_advance(const char *s, size_t n, int scale) {
    cui_font *f = font_for(scale); int best = 0; char line[512]; size_t i = 0;
    while (i <= n) {
        size_t j = i; while (j < n && s[j] != '\n') ++j;
        size_t l = j - i; if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, s + i, l); line[l] = 0;
        int w = l ? cui_text_width(f, line) : 0; if (w > best) best = w;
        i = j + 1;
    }
    return best;
}

void sp_smooth_draw(uint32_t *px, int w, int h, int cx0, int cy0, int cx1, int cy1, int x, int y, const char *s, size_t n, uint32_t col, int scale) {
    cui_font *f = font_for(scale); cui_canvas cv; cui_canvas_init(&cv, px, w, h, w);
    cui_clip(&cv, cx0, cy0, cx1 - cx0, cy1 - cy0);
    int top = 7 * scale - cui_font_ascent(f);                        /* the baseline of the 5x7 font is the bottom of its 7 rows */
    char line[512]; size_t i = 0; int ly = y;
    while (i <= n) {
        size_t j = i; while (j < n && s[j] != '\n') ++j;
        size_t l = j - i; if (l >= sizeof line) l = sizeof line - 1;
        memcpy(line, s + i, l); line[l] = 0;
        if (l) cui_text(&cv, f, x, ly + top, line, col & 0xFFFFFF);
        ly += 8 * scale; i = j + 1;
    }
}
