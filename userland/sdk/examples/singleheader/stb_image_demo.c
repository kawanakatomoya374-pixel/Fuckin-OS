/* stb_image_demo.c - a single-header library (stb_image.h) built on C-OS with the on-device TinyCC.
 *
 *   cd /system/sdk/examples/singleheader
 *   tcc stb_image_demo.c -o /tmp/stb_image_demo.c-os
 *   /tmp/stb_image_demo.c-os [photo.jpg]
 *
 * Decodes a PNG that is part of this file and a JPEG from disk, and prints what came out (the same lines a host build prints). */
#include <stdio.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#include "stb_image.h"

static const unsigned char png3x2[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66, 0x1a, 0x00, 0x00, 0x00,
    0x1a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x19, 0xfe, 0x03, 0x49, 0x20, 0x60, 0x00, 0xb1, 0xfe, 0x33,
    0x34, 0x00, 0x00, 0xa7, 0xf2, 0x0d, 0x74, 0xfc, 0xa3, 0x6c, 0x80, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

int main(int argc, char **argv) {
    int w, h, n;
    unsigned char *p = stbi_load_from_memory(png3x2, (int)sizeof png3x2, &w, &h, &n, 4);
    if (!p) { printf("png failed: %s\n", stbi_failure_reason()); return 1; }
    printf("png %dx%d comps=%d\n", w, h, n);
    for (int i = 0; i < w * h; i++) printf("px%d %02x%02x%02x%02x\n", i, p[i * 4], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
    stbi_image_free(p);
    const char *path = argc > 1 ? argv[1] : "/desktop/photo.jpg";
    p = stbi_load(path, &w, &h, &n, 3);
    if (!p) { printf("jpg failed: %s\n", stbi_failure_reason()); return 1; }
    unsigned long sum = 0;
    for (long i = 0; i < (long)w * h * 3; i++) sum = sum * 31 + p[i];
    printf("jpg %dx%d comps=%d hash=%08lx\n", w, h, n, sum & 0xffffffffUL);
    stbi_image_free(p);
    return 0;
}
