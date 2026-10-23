/* imgdec - host test driver for the C-OS image decoders.
 *
 *   imgdec crop|fit  <in> <out.raw> <max_w> <max_h> [jpeg_flags]
 *   imgdec probe     <in>
 *   imgdec gunzip    <in.gz> <out> <cap>
 *
 * Writes densely packed BGRA to <out.raw> and prints one status line:
 *   OK <w> <h> <src_w> <src_h> <fmt> <extra...>      or      FAIL <fmt> <code>
 * Links the real src/apps decoders against the shim headers (tools/imgtest/shim).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "png_decoder.h"
#include "jpeg_codec.h"
#include "bmp_codec.h"
#include "memory.h"

static uint8_t* slurp(const char* path, size_t* n) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* b = (uint8_t*)malloc(l ? (size_t)l : 1);
    if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)l;
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const char* mode = argv[1];
    size_t n;
    uint8_t* d = slurp(argv[2], &n);
    if (!d) { printf("FAIL io 0\n"); return 1; }

    if (!strcmp(mode, "gunzip")) {
        uint64_t cap = strtoull(argv[4], NULL, 10), len = 0;
        uint8_t* out = (uint8_t*)malloc(cap ? cap : 1);
        bool ok = gzip_inflate(d, n, out, cap, &len);
        if (ok) { FILE* o = fopen(argv[3], "wb"); fwrite(out, 1, len, o); fclose(o); }
        printf(ok ? "OK %llu\n" : "FAIL gz %llu\n", (unsigned long long)len);
        free(out); free(d);
        return ok ? 0 : 1;
    }

    const char* fmt = "unknown";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) fmt = "jpeg";
    else if (n >= 8 && d[0] == 0x89 && d[1] == 'P') fmt = "png";
    else if (n >= 2 && d[0] == 'B' && d[1] == 'M') fmt = "bmp";

    if (!strcmp(mode, "probe")) {
        if (!strcmp(fmt, "jpeg")) {
            jpeg_info_t i;
            int rc = cos_jpeg_codec_probe(d, n, &i);
            if (rc) { printf("FAIL jpeg %d\n", rc); return 1; }
            printf("OK %u %u %u %u jpeg ncomp=%d prec=%d prog=%d ori=%d cs=%d\n", i.width, i.height,
                   i.src_width, i.src_height, i.ncomp, i.precision, i.progressive, i.orientation, i.colorspace);
        } else if (!strcmp(fmt, "png")) {
            uint32_t w, h;
            if (!cos_png_probe(d, n, &w, &h)) { printf("FAIL png 0\n"); return 1; }
            printf("OK %u %u %u %u png\n", w, h, w, h);
        } else if (!strcmp(fmt, "bmp")) {
            uint32_t w, h;
            if (!cos_bmp_probe(d, n, &w, &h)) { printf("FAIL bmp 0\n"); return 1; }
            printf("OK %u %u %u %u bmp\n", w, h, w, h);
        } else { printf("FAIL unknown 0\n"); return 1; }
        return 0;
    }

    if (argc < 6) return 2;
    bool fit = !strcmp(mode, "fit");
    uint64_t mw = strtoull(argv[4], NULL, 10), mh = strtoull(argv[5], NULL, 10);
    unsigned jflags = argc > 6 ? (unsigned)strtoul(argv[6], NULL, 10) : 0;
    uint8_t* out = (uint8_t*)malloc((size_t)(mw * mh * 4 + 1));
    uint64_t w = 0, h = 0, sw = 0, sh = 0;
    int rc = 0; bool ok = false;
    char extra[256] = "";

    if (!strcmp(fmt, "jpeg")) {
        jpeg_info_t i;
        rc = cos_jpeg_codec_decode(d, n, out, mw, mh, (fit ? JPEG_DEC_FIT : 0) | jflags, &i);
        ok = rc == 0;
        w = i.width; h = i.height; sw = i.src_width; sh = i.src_height;
        snprintf(extra, sizeof extra, "den=%d ori=%d/%d trunc=%d cs=%d prog=%d prec=%d ncomp=%d",
                 i.scale_denom, i.orientation, i.orientation_applied, i.truncated, i.colorspace,
                 i.progressive, i.precision, i.ncomp);
    } else if (!strcmp(fmt, "png")) {
        ok = fit ? cos_png_decode_fit(d, n, out, mw, mh, &w, &h, &sw, &sh)
                 : png_decode(d, n, out, mw, mh, &w, &h);
        if (!fit) cos_png_probe(d, n, (uint32_t*)&sw, (uint32_t*)&sh);
    } else if (!strcmp(fmt, "bmp")) {
        ok = fit ? cos_bmp_decode_fit(d, n, out, mw, mh, &w, &h, &sw, &sh)
                 : cos_bmp_decode(d, n, out, mw, mh, &w, &h);
    }
    if (imgtest_live_allocs != 0) { printf("FAIL leak %ld\n", imgtest_live_allocs); return 1; }
    if (!ok) { printf("FAIL %s %d %llu %llu\n", fmt, rc, (unsigned long long)w, (unsigned long long)h); free(out); free(d); return 1; }
    FILE* o = fopen(argv[3], "wb");
    fwrite(out, 1, (size_t)(w * h * 4), o);
    fclose(o);
    printf("OK %llu %llu %llu %llu %s %s\n", (unsigned long long)w, (unsigned long long)h,
           (unsigned long long)sw, (unsigned long long)sh, fmt, extra);
    free(out); free(d);
    return 0;
}
