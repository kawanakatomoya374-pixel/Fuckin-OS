/* cos_audio.c - audio output for ring-3 programs (SYS_AUDIO_*).
 *
 * One owning process at a time. The owner opens the device with a sample
 * rate and channel count, then writes interleaved signed 16-bit PCM; the
 * samples go into the AC97 driver's software FIFO and are played by its
 * refill thread (see ac97.c), so writes never block on the hardware.
 * Ownership is dropped when the owner closes the device or exits
 * (cos_audio_release_for_pid(), called from process_exit()).
 *
 * While a ring-3 program owns audio, the in-kernel music backend
 * (mk_mp3_backend.c) is stopped and refuses to start - two producers
 * interleaving into one FIFO would be noise, not mixing. A real mixer is
 * future work. */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "ac97.h"
#include "usb_audio.h"
#include "cos_audio.h"


/* ---- output sink: a USB audio device when one is present, else the AC97 codec ---- */
static inline bool snk_usb(void) { return usb_audio_ready(); }
static bool snk_available(void) { return snk_usb() || ac97_is_available(); }
static void snk_stop(void) { if (snk_usb()) usb_audio_stop(); else ac97_stop(); }
static void snk_configure(uint32_t rate) { if (snk_usb()) usb_audio_configure(rate); else ac97_configure(rate, 2); }
static uint64_t snk_write(const int16_t *p, uint64_t n) { return snk_usb() ? usb_audio_write(p, n) : ac97_write_samples(p, n); }
static uint64_t snk_free(void) { return snk_usb() ? usb_audio_free_space() : ac97_free_space(); }
static uint64_t snk_pending(void) { return snk_usb() ? usb_audio_pending_samples() : ac97_pending_samples(); }
static uint64_t snk_underruns(void) { return snk_usb() ? usb_audio_underruns() : ac97_underrun_count(); }
static void snk_flush(void) { if (snk_usb()) usb_audio_flush(); else ac97_flush(); }

extern void usb_poll(void);
static void usb_poll_for_audio(void) { usb_poll(); }
static uint32_t s_owner = 0;       /* 0 = free */
static uint32_t s_rate = 44100;
static uint32_t s_channels = 2;

/* Per-stream (per-app) volume: a software gain applied to the owner's
 * samples, 0..100 on the same perceptual curve as the master, in Q15.
 * The OS master volume (hardware, persisted, keyboard volume keys) is
 * separate - see cos_audio_master_*(). */
static uint32_t s_gain_q15 = 32768;
static uint32_t s_master = 70;
static bool s_muted = false;

extern int mk_mp3_stop(void);

uint32_t cos_audio_owner(void) { return s_owner; }

int64_t cos_audio_open(uint32_t pid, uint32_t rate, uint32_t channels) {
    if (!snk_available()) return -2;
    if (s_owner && s_owner != pid) return -16;        /* EBUSY */
    if (rate < 8000 || rate > 48000) return -22;      /* EINVAL */
    if (channels != 1 && channels != 2) return -22;
    if (!s_owner) (void)mk_mp3_stop();                /* in-kernel player yields */
    s_owner = pid;
    s_rate = rate;
    s_channels = channels;
    snk_stop();
    snk_configure(rate);
    serial_puts("[AUDIO] pid ");
    serial_putdec(pid);
    serial_puts(" opened audio ");
    serial_putdec(rate);
    serial_puts(channels == 2 ? " Hz stereo\n" : " Hz mono\n");
    return 0;
}

/* `samples` = number of int16 values in `pcm` (interleaved if stereo).
 * Returns how many of them were accepted (a whole number of frames). */
static inline int16_t apply_gain(int16_t v) {
    int32_t x = ((int32_t)v * (int32_t)s_gain_q15) >> 15;
    return (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
}

int64_t cos_audio_write(uint32_t pid, const int16_t* pcm, uint64_t samples) {
    if (!s_owner || s_owner != pid) return -1;
    if (!pcm || samples == 0) return 0;
    if (s_channels == 2) {
        samples &= ~(uint64_t)1;
        if (s_gain_q15 == 32768) return (int64_t)snk_write(pcm, samples);
        int16_t tmp[1024];
        uint64_t done = 0;
        while (done < samples) {
            uint64_t n = samples - done;
            if (n > 1024) n = 1024;
            for (uint64_t i = 0; i < n; ++i) tmp[i] = apply_gain(pcm[done + i]);
            uint64_t w = snk_write(tmp, n);
            done += w;
            if (w < n) break;
        }
        return (int64_t)done;
    }
    /* mono -> stereo, in bounded chunks */
    int16_t tmp[1024];
    uint64_t done = 0;
    while (done < samples) {
        uint64_t n = samples - done;
        if (n > 512) n = 512;
        if (snk_free() < n * 2) break;
        for (uint64_t i = 0; i < n; ++i) { int16_t v = apply_gain(pcm[done + i]); tmp[i * 2] = v; tmp[i * 2 + 1] = v; }
        uint64_t w = snk_write(tmp, n * 2);
        done += w / 2;
        if (w < n * 2) break;
    }
    return (int64_t)done;
}

int64_t cos_audio_status(uint32_t pid, cos_audio_status_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->available = snk_available() ? 1u : 0u;
    out->owned_by_caller = (s_owner && s_owner == pid) ? 1u : 0u;
    out->busy = (s_owner && s_owner != pid) ? 1u : 0u;
    out->rate = s_rate;
    out->channels = s_channels;
    uint64_t free_st = snk_free();
    uint64_t pend_st = snk_pending();
    /* report in the caller's own units (per-channel interleaved samples) */
    out->free_samples = s_channels == 2 ? free_st : free_st / 2;
    out->pending_samples = s_channels == 2 ? pend_st : pend_st / 2;
    out->underruns = snk_underruns();
    return 0;
}

int64_t cos_audio_drain(uint32_t pid) { if (s_owner != pid || !s_owner) return -1; snk_flush(); return 0; }
int64_t cos_audio_stop(uint32_t pid)  { if (s_owner != pid || !s_owner) return -1; snk_stop(); return 0; }
int64_t cos_audio_set_volume(uint32_t pid, uint32_t vol) {
    if (s_owner && s_owner != pid) return -16;
    if (vol > 100) vol = 100;
    s_gain_q15 = (uint32_t)((uint64_t)vol * vol * 32768u / 10000u);   /* (v/100)^2 */
    return 0;
}

/* ---- OS master volume -------------------------------------------------- */
static void master_apply(void) { ac97_set_volume(s_muted ? 0 : s_master); usb_audio_set_volume(s_muted ? 0 : s_master); }
void cos_audio_master_init(uint32_t vol, bool muted) {
    s_master = vol > 100 ? 100 : vol;
    s_muted = muted;
    master_apply();
}
uint32_t cos_audio_master_get(void) { return s_master; }
bool cos_audio_master_muted(void) { return s_muted; }
void cos_audio_master_set(uint32_t vol) {
    s_master = vol > 100 ? 100 : vol;
    s_muted = false;
    master_apply();
}
void cos_audio_master_toggle_mute(void) { s_muted = !s_muted; master_apply(); }
int64_t cos_audio_close(uint32_t pid) {
    if (!s_owner || s_owner != pid) return -1;
    snk_flush();            /* let what was written finish playing */
    s_owner = 0;
    serial_puts("[AUDIO] released by pid ");
    serial_putdec(pid);
    serial_puts("\n");
    return 0;
}
void cos_audio_release_for_pid(uint32_t pid) {
    if (s_owner && s_owner == pid) {
        snk_stop();         /* owner died: silence now, don't play its leftovers */
        s_owner = 0;
        serial_puts("[AUDIO] owner exited - device released\n");
    }
}

/* Square-wave test tone through whichever sink is active (48 kHz stereo); returns false when there is no audio output. */
bool cos_audio_beep(uint32_t freq, uint32_t ms) {
    if (!snk_available() || freq == 0 || ms == 0) return false;
    if (ms > 2000) ms = 2000;
    if (!snk_usb()) { ac97_beep(freq, ms); return true; }
    snk_stop();
    snk_configure(48000);
    uint32_t total = 48u * ms, half = 48000u / (freq * 2u ? freq * 2u : 1u);
    if (half == 0) half = 1;
    int16_t buf[512];
    uint32_t done = 0, ph = 0;
    while (done < total) {
        uint32_t n = total - done > 256 ? 256 : total - done;
        for (uint32_t i = 0; i < n; ++i) { int16_t v = ((ph++ / half) & 1) ? 6000 : -6000; buf[i * 2] = v; buf[i * 2 + 1] = v; }
        uint64_t w = snk_write(buf, (uint64_t)n * 2) / 2;
        if (w == 0) { usb_poll_for_audio(); continue; }
        if (w < n) ph -= (n - (uint32_t)w);
        done += (uint32_t)w;
    }
    snk_flush();
    return true;
}
