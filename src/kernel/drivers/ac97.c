/**
 * ac97.c - Intel AC'97 (ICH) audio codec driver
 *
 * See ac97.h for context. Register layout follows the standard ICH
 * AC97 native audio (NAM) + bus-master (NABM) split:
 *
 *   NAMBAR (BAR0, I/O)  - mixer/codec registers (volume, rate, ...)
 *   NABMBAR (BAR1, I/O) - PCM-out DMA engine registers
 */
#include "ac97.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "pci.h"
#include "mm/paging.h"
#include "hal_api.h"
#include "sync.h"
#include "task.h"
#include "timer.h"

/* ---- NAM (mixer) register offsets, relative to BAR0 ---- */
#define AC97_NAM_RESET              0x00
#define AC97_NAM_MASTER_VOLUME      0x02
#define AC97_NAM_PCM_OUT_VOLUME     0x18
#define AC97_NAM_EXT_AUDIO_ID       0x28
#define AC97_NAM_EXT_AUDIO_CTRL     0x2A
#define AC97_NAM_FRONT_DAC_RATE     0x2C
#define AC97_EXT_ID_VRA             0x0001
#define AC97_EXT_CTRL_VRA           0x0001

/* ---- NABM (bus master) register offsets, relative to BAR1 ---- */
#define AC97_NABM_PO_BDBAR          0x10 /* PCM out buffer descriptor base addr (u32) */
#define AC97_NABM_PO_CIV            0x14 /* current index value (u8) */
#define AC97_NABM_PO_LVI            0x15 /* last valid index (u8) */
#define AC97_NABM_PO_SR             0x16 /* status register (u16) */
#define AC97_NABM_PO_PICB           0x18 /* position in current buffer (u16) */
#define AC97_NABM_PO_CR             0x1B /* control register (u8) */
#define AC97_NABM_GLOB_CNT          0x2C /* global control (u32) */
#define AC97_NABM_GLOB_STA          0x30 /* global status (u32) */

#define AC97_GLOB_CNT_COLD_RESET    0x00000002u /* bit1: 1 = AC-link out of cold reset */
#define AC97_GLOB_STA_PCR           0x00000100u /* bit8: primary codec ready */

#define AC97_CR_RPBM                0x01 /* run/pause bus master */
#define AC97_CR_RR                  0x02 /* reset registers */
#define AC97_CR_LVBIE               0x04
#define AC97_CR_FEIE                0x08
#define AC97_CR_IOCE                0x10

/* 1024 interleaved 16-bit words = 512 stereo frames: about 10.7ms at
 * 48kHz. The old 4096-word buffers required short UI test tones to fill two
 * 42ms descriptors before DMA even started, which made the Settings sound
 * test completely silent. */
#define AC97_BDL_FLAG_IOC           0x8000

typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

/* ---- ring geometry ----------------------------------------------------
 * 32 descriptors x 1024 stereo frames (2048 16-bit samples) = ~23 ms per
 * buffer at 44.1 kHz, ~0.74 s for the whole ring. The pump keeps up to
 * AC97_AHEAD buffers (~0.55 s) queued in front of the one playing. */
#define AC97_BDL_ENTRIES    32
#define AC97_BUF_SAMPLES    2048
#define AC97_AHEAD          24
#define AC97_FIFO_SAMPLES   (48000u * 2u * 2u)     /* 2 s of 48 kHz stereo */
#define AC97_PUMP_MS        5u
#define AC97_TAIL_IDLE_MS   30u   /* partial tail with no new data for this long -> play it padded */

/* ======================================================================
 * Design (rewritten; the ring logic of the previous driver is gone).
 *
 * The previous driver stopped and restarted the DMA engine around every
 * track, and its idea of "which buffer to write next" (a software index
 * reset to 0 by ac97_stop()) was never re-synchronised with the engine's
 * own Current Index Value, which a stop does not reset. After the boot
 * beep left CIV at 13, the free-space arithmetic mixed two unrelated
 * positions: the engine ran past the last valid index into unfilled
 * buffers, and a 3 s MP3 was "consumed" in 0.66 s with most of it never
 * heard. (Traced with [AC97T] logging of CIV/LVI/SR during playback.)
 *
 * The approach here takes its shape from ToaruOS's C driver
 * (klange/toaruos, kernel/audio/ac97.c): the producer never touches the
 * hardware ring. Samples go into a software FIFO; the hardware buffers
 * are refilled *relative to CIV*, just ahead of the one being played.
 * ToaruOS does that refill from the buffer-completion interrupt. C-OS
 * keeps AC97 completion interrupts off (the IRQ line is shared with the
 * E1000 and there is no shared-IRQ dispatcher yet - see the old notes),
 * so a dedicated kernel thread does it on a 5 ms period instead, which
 * also keeps audio independent of GUI frame stalls.
 *
 * Every (re)start first resets the channel (CR.RR: CIV/LVI/status back to
 * 0, per the ICH spec) and rewrites BDBAR, so the software and hardware
 * positions are always derived from the same origin. An underrun (engine
 * halted with DCH because the FIFO ran dry) is simply the "not running"
 * state: the next pump restarts cleanly once data is available.
 * ==================================================================== */

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} ac97_bdl_t;

static pci_dev_t* s_dev = NULL;
static uint16_t s_nambar = 0;
static uint16_t s_nabmbar = 0;
static bool s_available = false;
static bool s_vra_capable = false;

static ac97_bdl_t* s_bdl = NULL;
static int16_t* s_buffers = NULL;
static uint32_t s_bdl_phys = 0;

/* hardware-side state (pump thread only, under s_lock) */
static bool s_running = false;
static int  s_fill_idx = 0;          /* next descriptor to fill; engine owns [CIV, s_fill_idx) */
static uint64_t s_underruns = 0;

/* software FIFO */
static int16_t* s_fifo = NULL;
static uint32_t s_fifo_head = 0, s_fifo_tail = 0, s_fifo_count = 0;
static bool s_drain = false;                  /* play a partial tail padded, don't wait for more */
static uint64_t s_last_write_ms = 0;
static uint32_t s_rate = 48000;

static mutex_t s_lock;
static bool s_lock_ready = false;
static bool s_pump_thread = false;

static void lock(void)   { if (!s_lock_ready) { mutex_init(&s_lock); s_lock_ready = true; } mutex_lock(&s_lock); }
static void unlock(void) { mutex_unlock(&s_lock); }

static uint64_t ac97_phys_of(void* ptr) {
    if (!ptr) return 0;
    return paging_virt_to_phys((uint64_t)(uintptr_t)ptr);
}

static uint16_t ac97_io_base_from_bar(uint64_t bar) {
    /* AC97 BARs are I/O-space (bit0 == 1); the base address is the
     * rest of the value with the low 2 bits (I/O indicator bits) masked off. */
    if ((bar & 0x1) == 0) return 0; /* not I/O space - unexpected, bail */
    return (uint16_t)(bar & 0xFFFC);
}


static void channel_reset(void) {
    outb(s_nabmbar + AC97_NABM_PO_CR, 0);                 /* stop bus master */
    outb(s_nabmbar + AC97_NABM_PO_CR, AC97_CR_RR);        /* reset CIV/LVI/SR/PICB */
    for (uint32_t w = 0; w < 100000u; ++w) {
        if ((inb(s_nabmbar + AC97_NABM_PO_CR) & AC97_CR_RR) == 0) break;
    }
    outw(s_nabmbar + AC97_NABM_PO_SR, 0x001Cu);           /* clear LVBCI/BCIS/FIFOE (W1C) */
    outl(s_nabmbar + AC97_NABM_PO_BDBAR, s_bdl_phys);
    s_running = false;
    s_fill_idx = 0;
}

/* Moves up to one buffer of FIFO data into descriptor `idx`; pads with
 * silence. Returns the number of real (non-padding) samples used. */
static uint32_t fill_descriptor(int idx) {
    int16_t* dst = &s_buffers[(size_t)idx * AC97_BUF_SAMPLES];
    uint32_t n = s_fifo_count < AC97_BUF_SAMPLES ? s_fifo_count : AC97_BUF_SAMPLES;
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = s_fifo[s_fifo_tail];
        s_fifo_tail = (s_fifo_tail + 1) % AC97_FIFO_SAMPLES;
    }
    s_fifo_count -= n;
    for (uint32_t i = n; i < AC97_BUF_SAMPLES; ++i) dst[i] = 0;
    s_bdl[idx].samples = AC97_BUF_SAMPLES;
    s_bdl[idx].flags = AC97_BDL_FLAG_IOC;
    return n;
}

/* Can the pump submit another buffer right now? A full buffer of data is
 * always submittable; a partial one only when draining or when no more
 * data has arrived for AC97_TAIL_IDLE_MS (end of a short sound). */
static bool have_submittable(uint64_t now_ms) {
    if (s_fifo_count >= AC97_BUF_SAMPLES) return true;
    if (s_fifo_count == 0) return false;
    return s_drain || (now_ms - s_last_write_ms) >= AC97_TAIL_IDLE_MS;
}

static void pump_locked(void) {
    if (!s_available) return;
    uint64_t now = get_timer_ticks();

    if (s_running) {
        uint16_t sr = inw(s_nabmbar + AC97_NABM_PO_SR);
        if (sr & 0x0001u) {                      /* DCH: engine halted at LVI */
            if (s_fifo_count > 0) ++s_underruns; /* ran dry while data was still coming */
            s_running = false;
        }
    }

    if (!s_running) {
        if (!have_submittable(now)) { if (s_fifo_count == 0) s_drain = false; return; }
        channel_reset();
        int queued = 0;
        while (queued < AC97_AHEAD && have_submittable(now)) {
            fill_descriptor(s_fill_idx);
            s_fill_idx = (s_fill_idx + 1) % AC97_BDL_ENTRIES;
            ++queued;
        }
        outb(s_nabmbar + AC97_NABM_PO_LVI, (uint8_t)((s_fill_idx + AC97_BDL_ENTRIES - 1) % AC97_BDL_ENTRIES));
        outb(s_nabmbar + AC97_NABM_PO_CR, AC97_CR_RPBM);
        s_running = true;
        return;
    }

    /* Running: top up in front of CIV. */
    int civ = inb(s_nabmbar + AC97_NABM_PO_CIV) % AC97_BDL_ENTRIES;
    int queued = (s_fill_idx - civ + AC97_BDL_ENTRIES) % AC97_BDL_ENTRIES;
    bool added = false;
    while (queued < AC97_AHEAD && have_submittable(now)) {
        fill_descriptor(s_fill_idx);
        s_fill_idx = (s_fill_idx + 1) % AC97_BDL_ENTRIES;
        ++queued;
        added = true;
    }
    if (added) {
        outb(s_nabmbar + AC97_NABM_PO_LVI, (uint8_t)((s_fill_idx + AC97_BDL_ENTRIES - 1) % AC97_BDL_ENTRIES));
    }
    if (s_fifo_count == 0) s_drain = false;
}

void ac97_pump(void) {
    if (!s_available) return;
    lock();
    pump_locked();
    unlock();
}

static void ac97_pump_thread(void* arg) {
    (void)arg;
    for (;;) {
        ac97_pump();
        thread_sleep(AC97_PUMP_MS);
    }
}

int ac97_init(void) {
    s_available = false;
    s_dev = pci_get_device_by_class(PCI_CLASS_MEDIA, 0x01);
    if (!s_dev) {
        /* Fall back to the exact ID QEMU's -device AC97 exposes. */
        s_dev = pci_get_device(0x8086, 0x2415);
    }
    if (!s_dev) {
        serial_puts("[AC97] no AC97 audio device found\n");
        return -1;
    }

    s_nambar = ac97_io_base_from_bar(s_dev->bar[0]);
    s_nabmbar = ac97_io_base_from_bar(s_dev->bar[1]);
    if (!s_nambar || !s_nabmbar) {
        serial_puts("[AC97] device found but BARs are not I/O-mapped, aborting\n");
        return -1;
    }

    pci_enable_bus_mastering(s_dev);

    /* Bring the AC-link out of cold reset and wait for the primary codec
     * to signal ready before touching any NAM (mixer) register. This is
     * the documented ICH AC97 bring-up sequence (Intel's datasheet has
     * software set GLOB_CNT's Cold Reset bit, then poll GLOB_STA's
     * Primary Codec Ready bit before programming the codec) - QEMU's own
     * AC97 model is lenient enough to tolerate skipping it, which is
     * almost certainly why this had never been noticed here, but real
     * hardware and other virtualization stacks are not guaranteed to be:
     * NAM register writes issued before the codec is ready can be
     * silently dropped rather than merely muted, up to and including the
     * volume registers ac97_set_volume() below relies on - a device that
     * enumerates, DMAs, and reports success while never actually
     * producing sound. */
    uint32_t glob_cnt = inl(s_nabmbar + AC97_NABM_GLOB_CNT);
    outl(s_nabmbar + AC97_NABM_GLOB_CNT, glob_cnt | AC97_GLOB_CNT_COLD_RESET);

    bool codec_ready = false;
    for (uint32_t wait = 0; wait < 400000u; ++wait) {
        if (inl(s_nabmbar + AC97_NABM_GLOB_STA) & AC97_GLOB_STA_PCR) {
            codec_ready = true;
            break;
        }
    }
    if (!codec_ready) {
        serial_puts("[AC97] primary codec did not report ready after cold reset - "
                    "continuing, but audio may not work on this hardware\n");
    }

    /* Power up / reset the codec. */
    outw(s_nambar + AC97_NAM_RESET, 0x0000);

    /* Unmute + set master and PCM-out volume to a sane default (~80%). */
    ac97_set_volume(80);

    /* Check + enable Variable Rate Audio so we're not stuck at 48kHz. */
    uint16_t ext_id = inw(s_nambar + AC97_NAM_EXT_AUDIO_ID);
    s_vra_capable = (ext_id & AC97_EXT_ID_VRA) != 0;
    if (s_vra_capable) {
        uint16_t ctrl = inw(s_nambar + AC97_NAM_EXT_AUDIO_CTRL);
        outw(s_nambar + AC97_NAM_EXT_AUDIO_CTRL, ctrl | AC97_EXT_CTRL_VRA);
    }

    s_bdl = (ac97_bdl_t*)kmalloc_aligned(sizeof(ac97_bdl_t) * AC97_BDL_ENTRIES, 8);
    s_buffers = (int16_t*)kmalloc_aligned(sizeof(int16_t) * AC97_BUF_SAMPLES * AC97_BDL_ENTRIES, 16);
    s_fifo = (int16_t*)kmalloc(sizeof(int16_t) * AC97_FIFO_SAMPLES);
    if (!s_bdl || !s_buffers || !s_fifo) {
        serial_puts("[AC97] failed to allocate BDL/buffers/FIFO\n");
        return -1;
    }
    memset(s_bdl, 0, sizeof(ac97_bdl_t) * AC97_BDL_ENTRIES);
    memset(s_buffers, 0, sizeof(int16_t) * AC97_BUF_SAMPLES * AC97_BDL_ENTRIES);
    for (int i = 0; i < AC97_BDL_ENTRIES; ++i) {
        s_bdl[i].addr = (uint32_t)ac97_phys_of(&s_buffers[(size_t)i * AC97_BUF_SAMPLES]);
        s_bdl[i].samples = AC97_BUF_SAMPLES;
        s_bdl[i].flags = AC97_BDL_FLAG_IOC;
    }
    s_bdl_phys = (uint32_t)ac97_phys_of(s_bdl);
    if (s_bdl_phys == 0) {
        serial_puts("[AC97] BDL DMA mapping failed\n");
        return -1;
    }
    s_fifo_head = s_fifo_tail = s_fifo_count = 0;
    s_available = true;
    channel_reset();

    /* The refill thread. If it cannot be created, producers still pump
     * opportunistically (see ac97_write_samples / ac97_free_space). */
    s_pump_thread = thread_create_kernel("ac97_pump", (void*)ac97_pump_thread, NULL) != NULL;

    serial_puts("[AC97] audio device initialized (VRA ");
    serial_puts(s_vra_capable ? "supported" : "unsupported, fixed 48kHz");
    serial_puts(s_pump_thread ? ", refill thread running)\n" : ", no refill thread - polled)\n");
    return 0;
}

bool ac97_is_available(void) { return s_available; }

void ac97_configure(uint32_t sample_rate_hz, uint32_t channels) {
    (void)channels; /* PCM-out is stereo; mono producers duplicate samples */
    if (!s_available) return;
    if (sample_rate_hz == 0) sample_rate_hz = 48000;
    if (sample_rate_hz < 8000) sample_rate_hz = 8000;
    if (sample_rate_hz > 48000) sample_rate_hz = 48000;
    lock();
    if (sample_rate_hz != s_rate) {
        /* The DAC rate applies to everything queued: never change it under
         * audio of the old rate. */
        channel_reset();
        s_fifo_head = s_fifo_tail = s_fifo_count = 0;
        s_drain = false;
    }
    s_rate = sample_rate_hz;
    if (s_vra_capable) outw(s_nambar + AC97_NAM_FRONT_DAC_RATE, (uint16_t)sample_rate_hz);
    unlock();
}

uint64_t ac97_free_space(void) {
    if (!s_available) return 0;
    if (!s_pump_thread) ac97_pump();
    lock();
    uint64_t f = AC97_FIFO_SAMPLES - s_fifo_count;
    unlock();
    return f;
}

uint64_t ac97_write_samples(const int16_t* samples, uint64_t count) {
    if (!s_available || !samples || count == 0) return 0;
    lock();
    uint64_t space = AC97_FIFO_SAMPLES - s_fifo_count;
    if (count > space) count = space;
    count &= ~(uint64_t)1;                  /* whole stereo frames only */
    for (uint64_t i = 0; i < count; ++i) {
        s_fifo[s_fifo_head] = samples[i];
        s_fifo_head = (s_fifo_head + 1) % AC97_FIFO_SAMPLES;
    }
    s_fifo_count += (uint32_t)count;
    s_last_write_ms = get_timer_ticks();
    if (!s_pump_thread) pump_locked();
    unlock();
    return count;
}

/* Everything written but not yet heard: FIFO plus queued hardware buffers
 * (the playing one counted by its remaining PICB samples). */
uint64_t ac97_pending_samples(void) {
    if (!s_available) return 0;
    lock();
    uint64_t pending = s_fifo_count;
    if (s_running) {
        uint16_t sr = inw(s_nabmbar + AC97_NABM_PO_SR);
        if (!(sr & 0x0001u)) {
            int civ = inb(s_nabmbar + AC97_NABM_PO_CIV) % AC97_BDL_ENTRIES;
            int queued = (s_fill_idx - civ + AC97_BDL_ENTRIES) % AC97_BDL_ENTRIES;
            uint16_t picb = inw(s_nabmbar + AC97_NABM_PO_PICB);
            if (queued > 0) pending += (uint64_t)(queued - 1) * AC97_BUF_SAMPLES + picb;
        }
    }
    unlock();
    return pending;
}

/* End of a stream: play whatever partial tail is left without waiting. */
void ac97_flush(void) {
    if (!s_available) return;
    lock();
    s_drain = true;
    pump_locked();
    unlock();
}

/* Immediate silence: drop queued audio and reset the channel. */
void ac97_stop(void) {
    if (!s_available) return;
    lock();
    channel_reset();
    s_fifo_head = s_fifo_tail = s_fifo_count = 0;
    s_drain = false;
    unlock();
}

uint64_t ac97_underrun_count(void) { return s_underruns; }

/* Master volume, 0..100, on a perceptual curve.
 *
 * Previously the same linear attenuation was written to BOTH the master and
 * the PCM-out register, so at the default 80% the signal was cut twice by
 * ~19.5 dB (about -39 dB overall) - "far too quiet". Now:
 *   - PCM-out stays at 0 dB (0x08 per channel: its 5-bit scale is +12 dB at
 *     0x00 down to -34.5 dB at 0x1F), and
 *   - only the master register carries the volume, with amplitude
 *     proportional to v^2 (dB = 40*log10(v/100)): 80% = -3.9 dB,
 *     50% = -12 dB, 20% = -28 dB. The master register has 1.5 dB steps down
 *     to -46.5 dB; 0 mutes. */
void ac97_set_volume(uint32_t volume_0_100) {
    if (!s_nambar) return;
    if (volume_0_100 > 100) volume_0_100 = 100;
    outw(s_nambar + AC97_NAM_PCM_OUT_VOLUME, 0x0808);
    if (volume_0_100 == 0) {
        outw(s_nambar + AC97_NAM_MASTER_VOLUME, 0x8000);          /* mute */
        return;
    }
    /* 40*log10(v/100) without libm: log10 via a short table of v^2 steps */
    double amp = (double)volume_0_100 / 100.0;
    amp = amp * amp;                                             /* perceptual */
    int steps = 0;                                               /* 1.5 dB = x0.8414 */
    while (amp < 0.9173 && steps < 31) { amp /= 0.8414; ++steps; }   /* rounds to nearest step */
    uint16_t reg = (uint16_t)((steps << 8) | steps);
    outw(s_nambar + AC97_NAM_MASTER_VOLUME, reg);
}

void ac97_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (!s_available) return;
    if (freq_hz == 0) freq_hz = 880;
    if (duration_ms == 0) duration_ms = 150;
    if (duration_ms > 1000) duration_ms = 1000;
    ac97_configure(48000, 2);
    const uint32_t rate = 48000;
    const int16_t amplitude = 8000;
    uint32_t half_period = rate / (freq_hz * 2);
    if (half_period == 0) half_period = 1;
    uint32_t total_frames = (rate * duration_ms) / 1000;   /* <= 1 s: always fits the 2 s FIFO */
    int16_t chunk[1024];
    uint32_t frame = 0;
    while (frame < total_frames) {
        int n = 0;
        while (frame < total_frames && n < 512) {
            int16_t s = ((frame % (half_period * 2)) < half_period) ? amplitude : (int16_t)-amplitude;
            chunk[n * 2] = s;
            chunk[n * 2 + 1] = s;
            ++n; ++frame;
        }
        ac97_write_samples(chunk, (uint64_t)n * 2);
    }
    ac97_flush();
}
