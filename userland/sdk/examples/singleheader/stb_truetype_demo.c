/* stb_truetype_demo.c - stb_truetype.h (single header) built on C-OS with the on-device TinyCC.
 *
 *   tcc stb_truetype_demo.c -o /tmp/stb_truetype_demo.c-os && /tmp/stb_truetype_demo.c-os [font.ttf]
 *
 * Loads a TrueType font from disk, measures and rasterises a few glyphs and prints the numbers (the same lines a host build prints). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/system/fonts/NotoSansJP-Regular.ttf";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { printf("read failed\n"); return 1; }
    fclose(f);
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, buf, stbtt_GetFontOffsetForIndex(buf, 0))) { printf("not a font\n"); return 1; }
    int asc, desc, gap; stbtt_GetFontVMetrics(&font, &asc, &desc, &gap);
    printf("vmetrics %d %d %d\n", asc, desc, gap);
    float scale = stbtt_ScaleForPixelHeight(&font, 32.0f);
    const int cps[] = { 'A', 'g', 0x3042 };                       /* A, g, hiragana A */
    for (int k = 0; k < 3; k++) {
        int adv, lsb, x0, y0, x1, y1;
        int gi = stbtt_FindGlyphIndex(&font, cps[k]);
        stbtt_GetGlyphHMetrics(&font, gi, &adv, &lsb);
        stbtt_GetGlyphBitmapBox(&font, gi, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0; unsigned char *bm = calloc((size_t)(w > 0 && h > 0 ? w * h : 1), 1);
        stbtt_MakeGlyphBitmap(&font, bm, w, h, w, scale, scale, gi);
        unsigned long sum = 0; for (int i = 0; i < w * h; i++) sum += bm[i];
        printf("glyph U+%04X index=%d adv=%d lsb=%d box=%d,%d..%d,%d coverage=%lu\n", cps[k], gi, adv, lsb, x0, y0, x1, y1, sum);
        free(bm);
    }
    free(buf);
    return 0;
}
