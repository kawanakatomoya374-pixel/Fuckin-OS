#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include "cos.h"

int main(void) {
    const uint32_t rate = 44100;
    int r = cos_audio_open(rate, 2);
    printf("[tone660] open -> %d\n", r);
    if (r != 0) return 1;
    static int16_t buf[2048];
    double phase = 0.0, step = 2.0 * M_PI * 660.0 / rate;
    uint64_t frames_total = rate * 2, frames_done = 0;
    while (frames_done < frames_total) {
        cos_audio_status_t st;
        cos_audio_status(&st);
        if (st.free_samples < 2048) { cos_sleep_ms(10); continue; }
        uint64_t n = 1024;
        if (n > frames_total - frames_done) n = frames_total - frames_done;
        for (uint64_t i = 0; i < n; ++i) {
            int16_t s = (int16_t)(sin(phase) * 9000.0);
            phase += step; if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            buf[i * 2] = s; buf[i * 2 + 1] = s;
        }
        int64_t acc = cos_audio_write(buf, n * 2);
        frames_done += (uint64_t)acc / 2;
    }
    cos_audio_drain();
    cos_audio_status_t st;
    do { cos_sleep_ms(20); cos_audio_status(&st); } while (st.pending_samples > 0);
    printf("[tone660] done: wrote %llu frames, underruns=%llu\n",
           (unsigned long long)frames_done, (unsigned long long)st.underruns);
    cos_audio_close();
    return 0;
}
