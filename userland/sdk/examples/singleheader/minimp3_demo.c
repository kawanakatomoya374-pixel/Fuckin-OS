/* minimp3_demo.c - minimp3.h (single header) built on C-OS with the on-device TinyCC.
 *
 *   tcc minimp3_demo.c -o /tmp/minimp3_demo.c-os && /tmp/minimp3_demo.c-os [beep.mp3]
 *
 * Decodes a whole MP3 file frame by frame and prints the format and a checksum of the PCM samples (the same line a host build prints). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/desktop/beep.mp3";
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) { printf("read failed\n"); return 1; }
    fclose(f);
    static mp3dec_t dec; mp3dec_init(&dec);
    mp3dec_frame_info_t info; short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long total = 0, frames = 0; unsigned long sum = 0; int hz = 0, ch = 0; unsigned char *p = buf; int left = (int)size;
    for (;;) {
        int n = mp3dec_decode_frame(&dec, p, left, pcm, &info);
        if (info.frame_bytes <= 0) break;
        p += info.frame_bytes; left -= info.frame_bytes;
        if (n > 0) { hz = info.hz; ch = info.channels; for (int i = 0; i < n * ch; i++) sum = sum * 31 + (unsigned short)pcm[i]; total += n; frames++; }
        if (left <= 0) break;
    }
    printf("mp3 hz=%d channels=%d frames=%ld samples=%ld hash=%08lx\n", hz, ch, frames, total, sum & 0xffffffffUL);
    free(buf);
    return 0;
}
