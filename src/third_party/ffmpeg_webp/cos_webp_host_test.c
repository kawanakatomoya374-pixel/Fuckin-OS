#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* Minimal stand-ins for the two kernel symbols this port's freestanding
 * code calls that a host build doesn't have: serial_puts (just print),
 * and nothing else - malloc/free/etc. all come from the host libc here
 * instead of src/include's kernel versions, which is fine since this
 * test only cares whether the decode logic itself is correct, not
 * kernel integration (that gets verified separately, in-kernel). */
void serial_puts(const char* s) { fputs(s, stderr); }

extern bool cos_webp_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                             uint64_t max_out_w, uint64_t max_out_h,
                             uint64_t* out_w, uint64_t* out_h);

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.webp [out.ppm]\n", argv[0]);
        return 1;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc((size_t)size);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    uint64_t maxw = 4096, maxh = 4096;
    uint8_t* out = (uint8_t*)calloc(maxw * maxh, 4);
    uint64_t w = 0, h = 0;
    bool ok = cos_webp_decode(buf, (uint64_t)size, out, maxw, maxh, &w, &h);
    printf("decode result: %s, %llux%llu\n", ok ? "OK" : "FAILED",
           (unsigned long long)w, (unsigned long long)h);
    if (!ok) return 1;

    if (argc >= 3) {
        FILE* out_f = fopen(argv[2], "wb");
        fprintf(out_f, "P6\n%llu %llu\n255\n", (unsigned long long)w, (unsigned long long)h);
        for (uint64_t y = 0; y < h; y++) {
            for (uint64_t x = 0; x < w; x++) {
                uint8_t* px = out + (y * w + x) * 4;
                uint8_t rgb[3] = { px[2], px[1], px[0] }; /* BGRA -> RGB */
                fwrite(rgb, 1, 3, out_f);
            }
        }
        fclose(out_f);
        printf("wrote %s\n", argv[2]);
    }

    /* Print a handful of sample pixels so their RGB values can be
     * sanity-checked against the source image's known generation
     * formula (see the Python script that created test.webp: R=x*4,
     * G=y*5, B=(x+y)*2, all mod 256) directly from this log, without
     * needing to open the PPM. */
    for (int i = 0; i < 4 && (uint64_t)i < h; i++) {
        int x = (int)(i * 13) % (int)w;
        int y = (int)(i * 7) % (int)h;
        uint8_t* px = out + ((uint64_t)y * w + (uint64_t)x) * 4;
        printf("pixel(%d,%d) = B=%d G=%d R=%d A=%d\n", x, y, px[0], px[1], px[2], px[3]);
    }
    return 0;
}
