/**
 * usb_audio.c - USB Audio Class 1.0 host driver (playback only).
 *
 * Registered with TinyUSB as an application class driver. open() walks the audio function (AudioControl interface and
 * its AudioStreaming interfaces), picks the alternate setting with a 2-channel 16-bit PCM isochronous OUT endpoint
 * (preferring 48 kHz), and set_config() then selects that alternate setting, programs the sampling frequency on the
 * endpoint and opens it. Packets are queued through xhci_iso_submit(): the host controller sends one per service
 * interval (1 ms at full speed), so the pump below fills each packet with rate/1000 frames, resampling linearly from
 * the application's sample rate and carrying the fractional remainder (44.1 kHz alternates 44/45-frame packets).
 */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "tusb.h"
#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "xhci_hcd.h"
#include "usb_audio.h"
#include "cos_devices.h"

#define UA_FIFO_FRAMES   8192u                    /* application-rate frames queued before the pump (>= 170 ms at 48 kHz) */
#define UA_TARGET_INFLIGHT 20u                    /* packets (ms) kept on the endpoint */
#define UA_PREBUFFER_MS  8u

typedef struct {
    bool present, ready;
    uint8_t daddr, ac_itf, as_itf, alt, ep_addr;
    uint16_t mps;
    uint32_t dev_rate;
    tusb_desc_endpoint_t ep;
    uint8_t ep_raw[9];
    bool ep_open;
} ua_t;
static ua_t s_ua;

/* playback state */
static int16_t s_fifo[UA_FIFO_FRAMES * 2];
static volatile uint32_t s_rd, s_wr;               /* frame counters */
static uint32_t s_src_rate = 48000;
static uint64_t s_pos_q16;                         /* read position between frame s_rd and s_rd+1, in Q16 */
static uint32_t s_frac;                            /* fractional frames carried between packets, in 1/1000 */
static bool s_playing, s_started;
static uint64_t s_underruns;
static uint32_t s_sent, s_failed;

static uint32_t s_gain_q15 = 32768;
#ifndef COS_VALIDATION_USBAUDIO
#define COS_VALIDATION_USBAUDIO 0
#endif
static uint32_t s_tone_frames, s_tone_phase;       /* boot self-test tone (validation builds) */

void usb_audio_set_volume(uint32_t vol) { if (vol > 100) vol = 100; s_gain_q15 = (uint32_t)((uint64_t)vol * vol * 32768u / 10000u); }

static inline uint32_t fifo_used(void) { return s_wr - s_rd; }

/* ------------------------------------------------------------------ descriptor parsing */
static uint16_t parse_function(uint8_t daddr, tusb_desc_interface_t const *itf, uint16_t max_len) {
    uint8_t const *p = (uint8_t const *)itf, *end = p + max_len;
    if (itf->bInterfaceClass != 1 || itf->bInterfaceSubClass != 1) return 0;          /* AudioControl first */
    serial_puts("[USBAUDIO] audio function found, parsing\n");
    tuh_bus_info_t bus; memset(&bus, 0, sizeof bus);
    tuh_bus_info_get(daddr, &bus);
    if (bus.rhport < XHCI_RHPORT_BASE) return 0;                                      /* isochronous OUT exists on xHCI only */
    if (s_ua.present) return 0;

    uint8_t ac = itf->bInterfaceNumber, cur_itf = 0, cur_alt = 0;
    bool cur_ok = false;
    int best_score = 0;
    uint16_t consumed = 0;
    uint32_t cur_rate_mask = 0;                     /* bit0: 48000 offered, bit1: 44100 offered */
    uint8_t cur_channels = 0, cur_bits = 0;
    ua_t cand; memset(&cand, 0, sizeof cand);

    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        uint8_t type = p[1];
        if (type == TUSB_DESC_INTERFACE) {
            tusb_desc_interface_t const *i = (tusb_desc_interface_t const *)p;
            if (i->bInterfaceClass != 1) break;                                       /* end of the audio function */
            cur_itf = i->bInterfaceNumber; cur_alt = i->bAlternateSetting;
            cur_ok = (i->bInterfaceSubClass == 2);
            cur_channels = cur_bits = 0; cur_rate_mask = 0;
        } else if (type == 0x24 && cur_ok && p[0] >= 8 && p[2] == 2) {                /* FORMAT_TYPE */
            if (p[3] == 1) {                                                          /* type I (PCM) */
                cur_channels = p[4]; cur_bits = p[6];
                uint8_t n = p[7];
                if (n == 0 && p[0] >= 14) {                                           /* continuous range */
                    uint32_t lo = p[8] | (p[9] << 8) | (p[10] << 16), hi = p[11] | (p[12] << 8) | (p[13] << 16);
                    if (lo <= 48000 && hi >= 48000) cur_rate_mask |= 1; if (lo <= 44100 && hi >= 44100) cur_rate_mask |= 2;
                } else {
                    for (uint8_t k = 0; k < n && (uint32_t)(8 + 3 * k + 3) <= p[0]; ++k) {
                        uint32_t r = p[8 + 3 * k] | (p[9 + 3 * k] << 8) | (p[10 + 3 * k] << 16);
                        if (r == 48000) cur_rate_mask |= 1; if (r == 44100) cur_rate_mask |= 2;
                    }
                }
            }
        } else if (type == TUSB_DESC_ENDPOINT && cur_ok) {
            tusb_desc_endpoint_t const *e = (tusb_desc_endpoint_t const *)p;
            if (e->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS && !(e->bEndpointAddress & 0x80) &&
                cur_channels == 2 && cur_bits == 16 && cur_rate_mask) {
                uint32_t rate = (cur_rate_mask & 1) ? 48000 : 44100;
                int score = (rate == 48000) ? 2 : 1;
                uint16_t need = (uint16_t)(((rate + 999) / 1000) * 4);
                if (tu_edpt_packet_size(e) >= need && score > best_score) {
                    best_score = score;
                    cand.as_itf = cur_itf; cand.alt = cur_alt; cand.ep_addr = e->bEndpointAddress;
                    cand.mps = tu_edpt_packet_size(e); cand.dev_rate = rate; cand.ep = *e;
                }
            }
        }
        p += p[0];
    }
    consumed = (uint16_t)(p - (uint8_t const *)itf);
    if (!best_score) { serial_puts("[USBAUDIO] no usable 2ch 16-bit PCM isochronous OUT alternate setting\n"); return 0; }
    cand.present = true; cand.daddr = daddr; cand.ac_itf = ac;
    s_ua = cand;
    return consumed;
}

/* ------------------------------------------------------------------ class driver */
static bool ua_init(void) { memset(&s_ua, 0, sizeof s_ua); return true; }
static bool ua_deinit(void) { return true; }
static uint16_t ua_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    (void)rhport;
    return parse_function(dev_addr, itf_desc, max_len);
}

static void step_freq_done(tuh_xfer_t *x);
static void step_alt_done(tuh_xfer_t *x) {
    uint8_t d = x->daddr;
    if (x->result != XFER_RESULT_SUCCESS || !s_ua.present || s_ua.daddr != d) { serial_puts("[USBAUDIO] set interface failed\n"); usbh_driver_set_config_complete(d, s_ua.ac_itf); return; }
    /* SET_CUR(SAMPLING_FREQ_CONTROL) on the endpoint */
    static uint8_t freq[4] __attribute__((aligned(4)));
    freq[0] = (uint8_t)s_ua.dev_rate; freq[1] = (uint8_t)(s_ua.dev_rate >> 8); freq[2] = (uint8_t)(s_ua.dev_rate >> 16);
    tusb_control_request_t req = { .bmRequestType = 0x22, .bRequest = 0x01, .wValue = 0x0100, .wIndex = s_ua.ep_addr, .wLength = 3 };
    tuh_xfer_t xf = { .daddr = d, .ep_addr = 0, .setup = &req, .buffer = freq, .complete_cb = step_freq_done, .user_data = 0 };
    if (!tuh_control_xfer(&xf)) step_freq_done(&(tuh_xfer_t){ .daddr = d, .result = XFER_RESULT_FAILED });
}
static void step_freq_done(tuh_xfer_t *x) {
    uint8_t d = x->daddr;
    /* a device with a fixed rate may STALL the request: that is fine, the alternate setting already fixes it */
    if (!tuh_edpt_open(d, &s_ua.ep)) { serial_puts("[USBAUDIO] endpoint open failed\n"); }
    else {
        s_ua.ep_open = true; s_ua.ready = true;
        s_rd = s_wr = 0; s_pos_q16 = 0; s_frac = 0; s_playing = s_started = false;
        serial_puts("[USBAUDIO] ready: 2ch 16-bit "); serial_putdec(s_ua.dev_rate);
        serial_puts(" Hz, isochronous OUT ep "); serial_puthex(s_ua.ep_addr); serial_puts(", packet "); serial_putdec(s_ua.mps); serial_puts(" bytes\n");
        cos_devices_usb_role(d, COS_DEVROLE_AUDIO);
#if COS_VALIDATION_USBAUDIO
        s_src_rate = COS_VALIDATION_USBAUDIO == 2 ? 44100 : 48000;       /* 2: exercise the resampler */
        s_tone_frames = s_src_rate / 2; s_tone_phase = 0;                /* 0.5 s of 1 kHz */
#endif
    }
    usbh_driver_set_config_complete(d, s_ua.ac_itf);
}
static bool ua_set_config(uint8_t dev_addr, uint8_t itf_num) {
    if (s_ua.present && s_ua.daddr == dev_addr && itf_num == s_ua.ac_itf) {
        if (!tuh_interface_set(dev_addr, s_ua.as_itf, s_ua.alt, step_alt_done, 0)) usbh_driver_set_config_complete(dev_addr, itf_num);
        return true;
    }
    usbh_driver_set_config_complete(dev_addr, itf_num);                            /* the AudioStreaming interfaces */
    return true;
}
static bool ua_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred) { (void)dev_addr; (void)ep_addr; (void)result; (void)xferred; return true; }
static void ua_close(uint8_t dev_addr) {
    if (s_ua.present && s_ua.daddr == dev_addr) {
        serial_puts("[USBAUDIO] device removed\n");
        memset(&s_ua, 0, sizeof s_ua);
        s_rd = s_wr = 0; s_playing = false;
    }
}

static usbh_class_driver_t const s_driver = {
    .name = "UAC1", .init = ua_init, .deinit = ua_deinit, .open = ua_open, .set_config = ua_set_config,
    .xfer_cb = ua_xfer_cb, .close = ua_close,
};
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) { *driver_count = 1; return &s_driver; }

/* ------------------------------------------------------------------ playback */
bool usb_audio_ready(void) { return s_ua.ready; }

void usb_audio_configure(uint32_t rate_hz) {
    if (rate_hz < 8000) rate_hz = 8000; if (rate_hz > 48000) rate_hz = 48000;
    s_src_rate = rate_hz;
    s_rd = s_wr = 0; s_pos_q16 = 0; s_playing = s_started = false;
}

uint64_t usb_audio_write(const int16_t *pcm, uint64_t samples) {
    if (!s_ua.ready) return 0;
    uint64_t frames = samples / 2, done = 0;
    while (done < frames && fifo_used() < UA_FIFO_FRAMES) {
        uint32_t i = s_wr % UA_FIFO_FRAMES;
        s_fifo[i * 2] = (int16_t)(((int32_t)pcm[done * 2] * (int32_t)s_gain_q15) >> 15);
        s_fifo[i * 2 + 1] = (int16_t)(((int32_t)pcm[done * 2 + 1] * (int32_t)s_gain_q15) >> 15);
        __sync_synchronize();
        s_wr++; done++;
    }
    if (done) s_playing = true;
    usb_audio_pump();
    return done * 2;
}

uint64_t usb_audio_free_space(void) { return (uint64_t)(UA_FIFO_FRAMES - fifo_used()) * 2; }
uint64_t usb_audio_pending_samples(void) { return (uint64_t)fifo_used() * 2; }
uint64_t usb_audio_underruns(void) { return s_underruns; }
void usb_audio_get_stats(uint32_t *sent, uint32_t *failed) { if (sent) *sent = s_sent; if (failed) *failed = s_failed; }

void usb_audio_stop(void) { s_rd = s_wr = 0; s_pos_q16 = 0; s_playing = false; }

extern void usb_poll(void);
void usb_audio_flush(void) {
    for (uint32_t spin = 0; spin < 4000000 && s_ua.ready && (fifo_used() || xhci_iso_inflight(s_ua.daddr, s_ua.ep_addr)); ++spin) {
        usb_poll();
        for (volatile int k = 0; k < 2000; ++k) { }
    }
    s_playing = false;
}

static inline void frame_at(uint32_t idx, int32_t *l, int32_t *r) { uint32_t i = idx % UA_FIFO_FRAMES; *l = s_fifo[i * 2]; *r = s_fifo[i * 2 + 1]; }

void usb_audio_pump(void) {
    if (!s_ua.ready) return;
    uint8_t d = s_ua.daddr, ep = s_ua.ep_addr;
    uint32_t guard = 0;
    while (s_tone_frames && fifo_used() < UA_FIFO_FRAMES) {                         /* validation self-test tone */
        int16_t v = (((uint64_t)s_tone_phase++ * 2000u / s_src_rate) & 1) ? 8000 : -8000;   /* 1 kHz square */
        uint32_t i = s_wr % UA_FIFO_FRAMES;
        s_fifo[i * 2] = s_fifo[i * 2 + 1] = v;
        s_wr++; s_tone_frames--; s_playing = true;
    }
    while (xhci_iso_inflight(d, ep) < UA_TARGET_INFLIGHT && guard++ < UA_TARGET_INFLIGHT) {
        uint32_t used = fifo_used();
        if (!s_started) {                                                       /* prebuffer so the first packets are not starved */
            if (used * 1000u < s_src_rate * UA_PREBUFFER_MS) return;
            s_started = true;
        }
        if (!used || (used == 1 && s_src_rate != s_ua.dev_rate)) {
            if (s_playing && used == 0 && s_started) { s_underruns++; s_playing = false; s_started = false; }
            return;
        }
        uint32_t frames_out = s_ua.dev_rate / 1000;                              /* + carry of the fractional part */
        s_frac += s_ua.dev_rate % 1000;
        if (s_frac >= 1000) { s_frac -= 1000; frames_out++; }
        int16_t pkt[256 / 2];
        if (frames_out * 4 > sizeof pkt || frames_out * 4 > s_ua.mps) return;
        uint32_t step = (uint32_t)(((uint64_t)s_src_rate << 16) / s_ua.dev_rate);       /* source frames per output frame, Q16 */
        for (uint32_t f = 0; f < frames_out; ++f) {
            uint32_t avail = fifo_used();
            if (avail == 0) { pkt[f * 2] = pkt[f * 2 + 1] = 0; continue; }     /* ran dry mid-packet: silence for the tail */
            int32_t l0, r0, l1, r1;
            frame_at(s_rd, &l0, &r0);
            if (step == 65536u) { pkt[f * 2] = (int16_t)l0; pkt[f * 2 + 1] = (int16_t)r0; }
            else {
                if (avail > 1) frame_at(s_rd + 1, &l1, &r1); else { l1 = l0; r1 = r0; }
                int32_t fr = (int32_t)(s_pos_q16 & 0xFFFF);
                pkt[f * 2] = (int16_t)(l0 + (((l1 - l0) * fr) >> 16));
                pkt[f * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * fr) >> 16));
            }
            s_pos_q16 += step;
            uint32_t adv = (uint32_t)(s_pos_q16 >> 16);
            if (adv > avail) adv = avail;
            s_rd += adv; s_pos_q16 &= 0xFFFF;
        }
        if (xhci_iso_submit(d, ep, pkt, (uint16_t)(frames_out * 4))) s_sent++; else { s_failed++; return; }
    }
}
