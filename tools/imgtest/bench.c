/* bench - decode speed (optimised build, no sanitizers).
 *   bench <file> <iterations> [fit]       prints ms per decode */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "png_decoder.h"
#include "jpeg_codec.h"
#include "bmp_codec.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc((size_t)n);
    if (fread(d, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    int it = atoi(argv[2]);
    int fit = argc > 3 && !strcmp(argv[3], "fit");
    uint64_t mw = fit ? 1920 : 8192, mh = fit ? 1080 : 8192, w = 0, h = 0, sw, sh;
    uint8_t* out = (uint8_t*)malloc(mw * mh * 4);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    int ok = 1;
    for (int i = 0; i < it; ++i) {
        if (d[0] == 0xFF) { jpeg_info_t inf; ok &= cos_jpeg_codec_decode(d, n, out, mw, mh, fit ? JPEG_DEC_FIT : 0, &inf) == 0; w = inf.width; h = inf.height; }
        else if (d[0] == 0x89) ok &= fit ? cos_png_decode_fit(d, n, out, mw, mh, &w, &h, &sw, &sh) : png_decode(d, n, out, mw, mh, &w, &h);
        else ok &= fit ? cos_bmp_decode_fit(d, n, out, mw, mh, &w, &h, &sw, &sh) : cos_bmp_decode(d, n, out, mw, mh, &w, &h);
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ms = ((b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6) / it;
    printf("%s: %s %llux%llu (file %.1f KB) %.1f ms/decode%s\n", argv[1], ok ? "ok" : "FAILED",
           (unsigned long long)w, (unsigned long long)h, n / 1024.0, ms, fit ? " [fit]" : "");
    return ok ? 0 : 1;
}
