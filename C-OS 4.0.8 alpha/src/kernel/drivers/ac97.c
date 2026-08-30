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

#define AC97_CR_RPBM                0x01 /* run/pause bus master */
#define AC97_CR_RR                  0x02 /* reset registers */
#define AC97_CR_LVBIE               0x04
#define AC97_CR_FEIE                0x08
#define AC97_CR_IOCE                0x10

#define AC97_BDL_ENTRIES            32
/* 1024 interleaved 16-bit words = 512 stereo frames: about 10.7ms at
 * 48kHz. The old 4096-word buffers required short UI test tones to fill two
 * 42ms descriptors before DMA even started, which made the Settings sound
 * test completely silent. */
#define AC97_BUF_SAMPLES            1024
#define AC97_BDL_FLAG_IOC           0x8000

typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bdl_entry_t;

static pci_dev_t* s_dev = NULL;
static uint16_t s_nambar = 0;
static uint16_t s_nabmbar = 0;
static bool s_available = false;
static bool s_vra_capable = false;
static bool s_running = false;

static ac97_bdl_entry_t* s_bdl = NULL;      /* AC97_BDL_ENTRIES entries, 8 bytes each */
static int16_t* s_buffers = NULL;           /* AC97_BDL_ENTRIES * AC97_BUF_SAMPLES samples */
static uint32_t s_bdl_phys = 0;

/* Software producer position: which buffer/offset we're currently filling. */
static int s_write_buf = 0;
static int s_write_off = 0;
static int s_filled_count = 0; /* number of buffers fully queued but not yet retired by CIV */

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

    /* Reset the PCM-out DMA engine and wait until the controller accepts
     * descriptor programming. QEMU generally clears RR immediately, but
     * real ICH controllers are permitted to take several bus cycles. */
    outb(s_nabmbar + AC97_NABM_PO_CR, AC97_CR_RR);
    for (uint32_t wait = 0; wait < 100000u; ++wait) {
        if ((inb(s_nabmbar + AC97_NABM_PO_CR) & AC97_CR_RR) == 0) break;
    }
    /* Clear stale FIFO/last-valid/buffer-completion status (W1C). */
    outw(s_nabmbar + AC97_NABM_PO_SR, 0x001Cu);

    /* Allocate the buffer descriptor list + backing sample buffers.
     * Both need to be physically addressable; this kernel's heap is
     * identity-mapped so a plain aligned kmalloc is sufficient (same
     * approach the e1000 NIC driver uses for its descriptor rings). */
    s_bdl = (ac97_bdl_entry_t*)kmalloc_aligned(sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 8);
    s_buffers = (int16_t*)kmalloc_aligned(sizeof(int16_t) * AC97_BUF_SAMPLES * AC97_BDL_ENTRIES, 16);
    if (!s_bdl || !s_buffers) {
        serial_puts("[AC97] failed to allocate BDL/sample buffers\n");
        return -1;
    }
    memset(s_bdl, 0, sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES);
    memset(s_buffers, 0, sizeof(int16_t) * AC97_BUF_SAMPLES * AC97_BDL_ENTRIES);

    for (int i = 0; i < AC97_BDL_ENTRIES; ++i) {
        uint64_t phys = ac97_phys_of(&s_buffers[(size_t)i * AC97_BUF_SAMPLES]);
        s_bdl[i].addr = (uint32_t)phys;
        s_bdl[i].samples = AC97_BUF_SAMPLES;
        s_bdl[i].flags = AC97_BDL_FLAG_IOC;
    }

    s_bdl_phys = (uint32_t)ac97_phys_of(s_bdl);
    if (s_bdl_phys == 0) {
        serial_puts("[AC97] BDL DMA mapping failed\n");
        return -1;
    }
    outl(s_nabmbar + AC97_NABM_PO_BDBAR, s_bdl_phys);
    outb(s_nabmbar + AC97_NABM_PO_LVI, 0);

    s_write_buf = 0;
    s_write_off = 0;
    s_filled_count = 0;
    s_running = false;
    s_available = true;

    serial_puts("[AC97] audio device initialized (VRA ");
    serial_puts(s_vra_capable ? "supported" : "unsupported, fixed 48kHz");
    serial_puts(")\n");
    return 0;
}

bool ac97_is_available(void) {
    return s_available;
}

void ac97_configure(uint32_t sample_rate_hz, uint32_t channels) {
    (void)channels; /* AC97 PCM-out is fixed stereo; mono callers should
                      * duplicate samples to both channels before writing. */
    if (!s_available) return;
    if (sample_rate_hz == 0) sample_rate_hz = 48000;
    if (s_vra_capable) {
        if (sample_rate_hz < 8000) sample_rate_hz = 8000;
        if (sample_rate_hz > 48000) sample_rate_hz = 48000;
        outw(s_nambar + AC97_NAM_FRONT_DAC_RATE, (uint16_t)sample_rate_hz);
    }
    /* If VRA isn't supported the codec plays everything at a fixed
     * 48kHz; without a resampler here that means pitch will be off for
     * non-48kHz tracks. Real hardware and QEMU's AC97 model both
     * support VRA, so this fallback path is a documented limitation
     * rather than a silent bug. */
}

static void ac97_kick_dma_if_ready(void) {
    if (s_running) return;
    if (s_filled_count < 2) return; /* keep a little headroom before starting */
    /* The PCM ring is advanced by polling CIV in ac97_free_space(). Keep
     * DMA completion interrupts disabled until the PCI shared-line dispatcher
     * exists: Q35 routes AC'97 and E1000 through a shared legacy PIRQ11, and
     * an AC'97 completion otherwise enters the E1000 handler with ICR=0,
     * starving network receive. Bus-master playback remains fully active. */
    outb(s_nabmbar + AC97_NABM_PO_CR, AC97_CR_RPBM);
    uint8_t control = inb(s_nabmbar + AC97_NABM_PO_CR);
    uint16_t status = inw(s_nabmbar + AC97_NABM_PO_SR);
    serial_puts("[AC97] PCM DMA start BDL=0x");
    serial_puthex(s_bdl_phys);
    serial_puts(" LVI=");
    serial_putdec((uint64_t)inb(s_nabmbar + AC97_NABM_PO_LVI));
    serial_puts(" CR=0x");
    serial_puthex(control);
    serial_puts(" SR=0x");
    serial_puthex(status);
    serial_puts("\n");
    s_running = (control & AC97_CR_RPBM) != 0;
}

uint64_t ac97_free_space(void) {
    if (!s_available) return 0;
    uint8_t civ = inb(s_nabmbar + AC97_NABM_PO_CIV);
    /* Buffers strictly between our write cursor and civ (going forward)
     * are owned by hardware; keep at least one slot free as a guard. */
    int used = s_write_buf - (int)civ;
    if (used < 0) used += AC97_BDL_ENTRIES;
    int free_bufs = AC97_BDL_ENTRIES - used - 1;
    if (free_bufs < 0) free_bufs = 0;
    uint64_t space = (uint64_t)free_bufs * AC97_BUF_SAMPLES;
    space += (uint64_t)(AC97_BUF_SAMPLES - s_write_off);
    return space;
}

uint64_t ac97_write_samples(const int16_t* samples, uint64_t count) {
    if (!s_available || !samples || count == 0) return 0;

    uint64_t written = 0;
    while (written < count) {
        uint64_t free_here = ac97_free_space();
        if (free_here == 0) break;

        int16_t* dst = &s_buffers[(size_t)s_write_buf * AC97_BUF_SAMPLES + s_write_off];
        uint64_t space_in_buf = (uint64_t)(AC97_BUF_SAMPLES - s_write_off);
        uint64_t remaining = count - written;
        uint64_t chunk = remaining < space_in_buf ? remaining : space_in_buf;
        if (chunk > free_here) chunk = free_here;
        if (chunk == 0) break;

        for (uint64_t i = 0; i < chunk; ++i) {
            dst[i] = samples[written + i];
        }
        written += chunk;
        s_write_off += (int)chunk;

        if (s_write_off >= AC97_BUF_SAMPLES) {
            /* Buffer complete: publish it to the hardware ring. */
            s_bdl[s_write_buf].samples = AC97_BUF_SAMPLES;
            s_bdl[s_write_buf].flags = AC97_BDL_FLAG_IOC;
            outb(s_nabmbar + AC97_NABM_PO_LVI, (uint8_t)s_write_buf);

            s_write_buf = (s_write_buf + 1) % AC97_BDL_ENTRIES;
            s_write_off = 0;
            if (s_filled_count < AC97_BDL_ENTRIES) s_filled_count++;
            ac97_kick_dma_if_ready();
        }
    }
    return written;
}

void ac97_set_volume(uint32_t volume_0_100) {
    if (!s_nambar) return;
    if (volume_0_100 > 100) volume_0_100 = 100;
    /* AC97 volume registers are 0 (loudest) .. 0x3F (silent) per
     * channel, packed as (left<<8)|right, bit15=mute. Convert our
     * 0-100 "louder is bigger" scale into that inverted attenuation. */
    uint8_t atten = (uint8_t)(0x3F - ((volume_0_100 * 0x3F) / 100));
    uint16_t reg = (uint16_t)((atten << 8) | atten);
    outw(s_nambar + AC97_NAM_MASTER_VOLUME, reg);
    outw(s_nambar + AC97_NAM_PCM_OUT_VOLUME, reg);
}

void ac97_stop(void) {
    if (!s_available) return;
    outb(s_nabmbar + AC97_NABM_PO_CR, 0);
    s_running = false;
    s_write_buf = 0;
    s_write_off = 0;
    s_filled_count = 0;
    outb(s_nabmbar + AC97_NABM_PO_LVI, 0);
}

void ac97_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (!s_available) return;
    if (freq_hz == 0) freq_hz = 880; /* A5 - a clear, simple reference tone */
    if (duration_ms == 0) duration_ms = 150;
    if (duration_ms > 1000) duration_ms = 1000; /* see the header comment: this ring holds ~1.36s */

    ac97_configure(48000, 2);

    const uint32_t rate = 48000;
    const int16_t amplitude = 8000; /* well under full scale - a square wave's harmonics make it read louder/harsher than a sine at the same amplitude */
    uint32_t half_period = rate / (freq_hz * 2);
    if (half_period == 0) half_period = 1;
    uint32_t total_frames = (rate * duration_ms) / 1000;

    int16_t chunk[1024]; /* 512 stereo frames per chunk, on the stack */
    uint32_t frame = 0;
    while (frame < total_frames) {
        int frames_in_chunk = 0;
        while (frame < total_frames && frames_in_chunk < 512) {
            bool high = (frame % (half_period * 2)) < half_period;
            int16_t s = high ? amplitude : (int16_t)-amplitude;
            chunk[frames_in_chunk * 2] = s;
            chunk[frames_in_chunk * 2 + 1] = s;
            frames_in_chunk++;
            frame++;
        }

        uint64_t to_write = (uint64_t)frames_in_chunk * 2;
        uint64_t written = 0;
        while (written < to_write) {
            uint64_t w = ac97_write_samples(chunk + written, to_write - written);
            if (w == 0) {
                hal_timer_delay_ms(5); /* ring momentarily full - let DMA drain a little */
            }
            written += w;
        }
    }
}
