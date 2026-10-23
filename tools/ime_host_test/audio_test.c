/* Host test for cosaudiodec: decodes a file and prints rate, duration, frames, RMS and the dominant frequency (zero crossings). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "cosaudiodec.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n); if (fread(b, 1, (size_t)n, f) != (size_t)n) return 1; fclose(f);
    adec *d = adec_open(b, (size_t)n);
    if (!d) { printf("%s: unsupported\n", argv[1]); return 2; }
    static short pcm[4096 * 2]; long frames = 0; double sum = 0; long zc = 0; short prev = 0; int got;
    while ((got = adec_read(d, pcm, 4096)) > 0) {
        for (int i = 0; i < got; i++) { short l = pcm[i * 2]; sum += (double)l * l; if ((l >= 0) != (prev >= 0)) zc++; prev = l; }
        frames += got;
    }
    printf("%s: rate %u duration %.2fs frames %ld rms %.0f freq~%.0f Hz\n", argv[1], adec_rate(d), adec_duration(d), frames, sqrt(sum / (frames ? frames : 1)), zc / 2.0 / ((double)frames / adec_rate(d)));
    return 0;
}
