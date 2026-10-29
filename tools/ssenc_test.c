/* host test of src/gui/core/screenshot_enc.c: writes a PNG, JPEG and WebP of a test picture (tools/ssenc_test OUTDIR); Pillow then decodes and compares them */
#include <stdio.h>
#include <stdlib.h>
#include "screenshot_enc.h"
int main(int argc, char **argv) {
    int w = 301, h = 203; uint32_t *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { unsigned r = (x * 255) / w, g = (y * 255) / h, b = ((x / 16 + y / 16) & 1) ? 200 : 40; if (x > 100 && x < 150 && y > 50 && y < 90) { r = 255; g = 255; b = 0; } px[y * w + x] = (r << 16) | (g << 8) | b; }
    const char *dir = argc > 1 ? argv[1] : "/tmp"; char p[300]; size_t n; uint8_t *d; FILE *f;
    d = ss_encode_png(px, w, h, &n); snprintf(p, sizeof p, "%s/t.png", dir); f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); printf("png %zu\n", n); free(d);
    d = ss_encode_jpeg(px, w, h, 90, &n); snprintf(p, sizeof p, "%s/t.jpg", dir); f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); printf("jpg %zu\n", n); free(d);
    d = ss_encode_webp(px, w, h, &n); snprintf(p, sizeof p, "%s/t.webp", dir); f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); printf("webp %zu\n", n); free(d);
    return 0;
}
