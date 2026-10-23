/* audio_cos.c - the audio backend of mc-classic-c (audio/audio_backend.h) on the C-OS PCM device: a thread asks the mixer for stereo frames
 * and queues them with cos_audio_write(), which never blocks. */
#include <stdint.h>
#include <pthread.h>
#include "cos.h"

typedef void (*AudioFillCallback)(int16_t *buffer, int frames);
#define PERIOD_FRAMES 1024

static AudioFillCallback s_fill;
static pthread_t s_thread;
static volatile int s_running, s_open;

static void *audio_thread(void *arg) {
    (void)arg;
    static int16_t buf[PERIOD_FRAMES * 2];
    while (s_running) {
        cos_audio_status_t st;
        if (cos_audio_status(&st) == 0 && st.pending_samples > 44100 / 5 * 2) { cos_sleep_ms(10); continue; }   /* ~200 ms queued: enough */
        s_fill(buf, PERIOD_FRAMES);
        const int16_t *p = buf; uint64_t left = PERIOD_FRAMES * 2;
        while (left && s_running) {
            int64_t n = cos_audio_write(p, left);
            if (n <= 0) { cos_sleep_ms(5); continue; }
            p += n; left -= (uint64_t)n;
        }
    }
    return NULL;
}

int AudioBackend_init(AudioFillCallback fill) {
    s_fill = fill;
    if (cos_audio_open(44100, 2) != 0) return -1;
    s_open = 1; s_running = 1;
    if (pthread_create(&s_thread, NULL, audio_thread, NULL) != 0) { s_running = 0; s_open = 0; cos_audio_close(); return -1; }
    return 0;
}

void AudioBackend_shutdown(void) {
    if (!s_open) return;
    s_running = 0; pthread_join(s_thread, NULL);
    cos_audio_stop(); cos_audio_close(); s_open = 0;
}
