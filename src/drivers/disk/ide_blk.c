/**
 * ide_blk.c - legacy IDE (PATA / SATA in compatibility mode) block driver, primary channel master.
 *
 * Bus-master DMA (READ/WRITE DMA EXT, up to 128 KiB per command) when the PCI IDE controller offers it, otherwise
 * programmed I/O; LBA48 throughout, so disks beyond 128 GiB work. A soft reset on probe clears whatever a firmware or
 * boot loader left in the task file.
 */
#include "blk.h"
#include "pci.h"
#include "io.h"
#include "mm/paging.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"

#define IO_BASE   0x1F0
#define CTRL_BASE 0x3F6
#define R_DATA    (IO_BASE + 0)
#define R_ERR     (IO_BASE + 1)
#define R_COUNT   (IO_BASE + 2)
#define R_LBA0    (IO_BASE + 3)
#define R_LBA1    (IO_BASE + 4)
#define R_LBA2    (IO_BASE + 5)
#define R_DEV     (IO_BASE + 6)
#define R_CMD     (IO_BASE + 7)
#define R_ALT     CTRL_BASE

#define ST_BSY 0x80
#define ST_DRDY 0x40
#define ST_DF 0x20
#define ST_DRQ 0x08
#define ST_ERR 0x01

#define DMA_CHUNK 256u                 /* sectors per DMA command (128 KiB) */

static bool g_lba48, g_trim;
static uint16_t g_bm;                  /* bus-master I/O base of the primary channel, 0 = no DMA */
static uint64_t *g_prd; static uint64_t g_prd_phys;
static volatile uint32_t g_lock;

static void lock(void) {
    uint32_t spin = 0;                      /* the owner may be waiting on the disk (and yielding): do not burn the CPU against it */
    while (__atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE)) { if ((++spin & 0x3Fu) == 0) thread_yield(); else __asm__ volatile("pause"); }
}
static void unlock(void) { __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE); }

static void delay400(void) { for (int i = 0; i < 4; ++i) (void)inb(R_ALT); }
static bool wait_not_busy(void) {
    for (uint32_t i = 0; i < 4000000u; ++i) if (!(inb(R_CMD) & ST_BSY)) return true;
    return false;
}
/* The waits used by transfers give up after a bounded number of polls AND a bounded time: on a hypervisor every poll is a VM exit and the guest
 * clock can run many times slower than real time, so a limit in timer ticks alone (or a huge poll count) never ends in practice. */
#define IO_POLLS 300000u
#define IO_TICKS 8000u
static bool wait_not_busy_io(void) {
    uint64_t t0 = get_timer_ticks();
    for (uint32_t i = 0; i < IO_POLLS; ++i) {
        if (!(inb(R_CMD) & ST_BSY)) return true;
        if ((i & 0xFFu) == 0xFFu && get_timer_ticks() - t0 > IO_TICKS) break;
    }
    return false;
}
static bool wait_drq(void) {
    uint64_t t0 = get_timer_ticks();
    for (uint32_t i = 0; i < IO_POLLS; ++i) {
        if ((i & 0xFFu) == 0xFFu && get_timer_ticks() - t0 > IO_TICKS) break;
        uint8_t s = inb(R_CMD);
        if (s & (ST_ERR | ST_DF)) return false;
        if (!(s & ST_BSY) && (s & ST_DRQ)) return true;
    }
    return false;
}

/* A transfer that fits in 28 bits uses the 28-bit commands even on a 48-bit drive (as Linux does): no high-order registers are involved, so
 * nothing depends on how a controller or emulator latches the "previous" register values of the 48-bit protocol. */
static bool use48(uint64_t lba, uint32_t count) { return g_lba48 && (lba + count > (1ull << 28) || count > 256u); }

/* Programs LBA/count on the master. */
static void setup(uint64_t lba, uint32_t count, bool ext) {
    if (ext) {
        outb(R_DEV, 0x40);
        outb(R_COUNT, (uint8_t)(count >> 8)); outb(R_LBA0, (uint8_t)(lba >> 24)); outb(R_LBA1, (uint8_t)(lba >> 32)); outb(R_LBA2, (uint8_t)(lba >> 40));
        outb(R_COUNT, (uint8_t)count);        outb(R_LBA0, (uint8_t)lba);         outb(R_LBA1, (uint8_t)(lba >> 8));  outb(R_LBA2, (uint8_t)(lba >> 16));
    } else {
        outb(R_DEV, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
        outb(R_COUNT, (uint8_t)count); outb(R_LBA0, (uint8_t)lba); outb(R_LBA1, (uint8_t)(lba >> 8)); outb(R_LBA2, (uint8_t)(lba >> 16));      /* count 256 is written as 0 */
    }
}

/* ---- PIO */
static bool pio_run(uint64_t lba, uint32_t count, void* buf, bool write) {
    if (!wait_not_busy_io()) return false;
    bool ext = use48(lba, count);
    setup(lba, count, ext);
    outb(R_CMD, ext ? (write ? 0x34 : 0x24) : (write ? 0x30 : 0x20));
    uint16_t* w = (uint16_t*)buf;
    for (uint32_t s = 0; s < count; ++s) {
        if (!wait_drq()) return false;
        if (write) { for (int i = 0; i < 256; ++i) outw(R_DATA, w[(size_t)s * 256 + i]); delay400(); }
        else for (int i = 0; i < 256; ++i) w[(size_t)s * 256 + i] = inw(R_DATA);
    }
    if (write) { if (!wait_not_busy_io()) return false; }
    return !(inb(R_CMD) & (ST_ERR | ST_DF));
}

/* ---- bus-master DMA */
static int build_prd(const void* buf, size_t len) {
    uintptr_t v = (uintptr_t)buf;
    int n = 0;
    while (len) {
        size_t chunk = 4096 - (v & 4095u); if (chunk > len) chunk = len;
        uint64_t ph = paging_virt_to_phys((uint64_t)v);
        if (!ph || ph + chunk > 0x100000000ull) return -1;                 /* the engine only reaches 32 bits */
        /* merge with the previous entry when contiguous and within the same 64 KiB window */
        if (n > 0) {
            uint32_t pa = (uint32_t)g_prd[n - 1], pc = (uint32_t)(g_prd[n - 1] >> 32) & 0xFFFF; if (!pc) pc = 0x10000;
            if (pa + pc == ph && ((pa & 0xFFFF) + pc + chunk) <= 0x10000u) { uint32_t nc = pc + (uint32_t)chunk; g_prd[n - 1] = pa | ((uint64_t)(nc & 0xFFFF) << 32); v += chunk; len -= chunk; continue; }
        }
        if (n >= 512) return -1;
        g_prd[n++] = (uint32_t)ph | ((uint64_t)(chunk & 0xFFFF) << 32);
        v += chunk; len -= chunk;
    }
    g_prd[n - 1] |= (uint64_t)0x8000 << 48;                                 /* end of table */
    return n;
}

/* Returns 0 on success, 1 when the drive reported an error (do not fall back: the command itself is refused), 2 when the engine did not finish
 * or could not be used (the caller may try programmed I/O). */
static int dma_run(uint64_t lba, uint32_t count, void* buf, bool write) {
    if (build_prd(buf, (size_t)count * 512u) < 0) return 2;
    if (!wait_not_busy_io()) return 2;
    bool ext = use48(lba, count);
    setup(lba, count, ext);
    outl(g_bm + 4, (uint32_t)g_prd_phys);
    outb(g_bm + 2, 0x06);                                                   /* clear error + interrupt */
    outb(g_bm + 0, write ? 0x00 : 0x08);                                    /* direction: bit 3 set = device -> memory */
    outb(R_CMD, ext ? (write ? 0x35 : 0x25) : (write ? 0xCA : 0xC8));
    outb(g_bm + 0, (write ? 0x00 : 0x08) | 0x01);                           /* start */
    uint64_t t0 = get_timer_ticks();
    uint32_t spin = 0;
    int rc = 2;
    for (;;) {
        uint8_t bs = inb(g_bm + 2);
        if (bs & 0x02) { rc = 1; break; }                                   /* DMA error */
        if (bs & 0x04) { rc = 0; break; }                                   /* interrupt: transfer complete */
        if (!(bs & 0x01)) { rc = 0; break; }                                /* engine finished */
        if ((spin & 0x1Fu) == 0x1Fu) {                                      /* the drive may have refused the command: its status says so (no IRQ, engine idle) */
            uint8_t as = inb(R_ALT);
            if (!(as & ST_BSY) && (as & (ST_ERR | ST_DF))) { rc = 1; break; }
        }
        if (++spin > IO_POLLS * 4u) break;
        if ((spin & 0xFFu) == 0 && get_timer_ticks() - t0 > 20000) break;
        if ((spin & 0xFFu) == 0) thread_yield(); else __asm__ volatile("pause");
    }
    outb(g_bm + 0, 0);
    uint8_t st = inb(R_CMD);
    if (rc == 0 && (st & (ST_ERR | ST_DF))) rc = 1;
    for (uint32_t i = 0; rc == 0 && i < 100000u && (inb(R_CMD) & ST_BSY); ++i) { }
    outb(g_bm + 2, 0x06);
    return rc;
}

static bool ide_rw(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf, bool write) {
    (void)d;
    uint8_t* p = (uint8_t*)buf;
    lock();
    bool ok = true;
    while (count && ok) {
        uint32_t n = count > DMA_CHUNK ? DMA_CHUNK : count;
        if (!g_lba48 && n > 255) n = 255;
        bool done = false;
        int dr = g_bm ? dma_run(lba, n, p, write) : 2;
        if (dr == 0) done = true;
        else if (dr == 2) done = pio_run(lba, n, p, write);                 /* no DMA, the buffer is not DMA-able, or the engine did not finish */
        ok = done;                                                          /* dr == 1: the drive refused the command; trying again by PIO would only wait */
        lba += n; p += (size_t)n * 512u; count -= n;
    }
    unlock();
    return ok;
}
static bool ide_read(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf) { return ide_rw(d, lba, count, buf, false); }
static bool ide_write(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf) { return ide_rw(d, lba, count, (void*)buf, true); }
/* DATA SET MANAGEMENT / TRIM through bus-master DMA (the command moves a 512-byte block of range entries to the drive). */
static bool ide_trim(blk_dev_t* d, uint64_t lba, uint64_t count) {
    (void)d;
    if (!g_bm || !g_lba48) return false;
    uint8_t* blk = (uint8_t*)kmalloc_aligned(4096, 4096);
    if (!blk) return false;
    bool ok = true;
    lock();
    while (count && ok) {
        uint64_t* r = (uint64_t*)blk;
        memset(blk, 0, 512);
        uint32_t n = 0;
        while (count && n < 64) { uint32_t len = count > 65535 ? 65535 : (uint32_t)count; r[n++] = (lba & 0xFFFFFFFFFFFFull) | ((uint64_t)len << 48); lba += len; count -= len; }
        if (build_prd(blk, 512) < 0 || !wait_not_busy()) { ok = false; break; }
        outb(R_DEV, 0x40);
        outb(R_COUNT, 0); outb(R_LBA0, 0); outb(R_LBA1, 0); outb(R_LBA2, 0);              /* features high / count high */
        outb(R_ERR, 0x01);                                                              /* feature 1 = TRIM */
        outb(R_COUNT, 1); outb(R_LBA0, 0); outb(R_LBA1, 0); outb(R_LBA2, 0);
        outl(g_bm + 4, (uint32_t)g_prd_phys);
        outb(g_bm + 2, 0x06);
        outb(g_bm + 0, 0x00);                                                           /* memory -> device */
        outb(R_CMD, 0x06);
        outb(g_bm + 0, 0x01);
        uint64_t t0 = get_timer_ticks();
        ok = false;
        for (;;) {
            uint8_t bs = inb(g_bm + 2);
            if (bs & 0x02) break;
            if ((bs & 0x04) || !(bs & 0x01)) { ok = true; break; }
            if (get_timer_ticks() - t0 > 20000) break;
            __asm__ volatile("pause");
        }
        outb(g_bm + 0, 0);
        if (ok && (inb(R_CMD) & (ST_ERR | ST_DF))) ok = false;
        outb(g_bm + 2, 0x06);
    }
    unlock();
    kfree(blk);
    return ok;
}

static bool ide_flush(blk_dev_t* d) {
    (void)d;
    lock();
    bool ok = wait_not_busy();
    if (ok) {
        outb(R_DEV, g_lba48 ? 0x40 : 0xE0);
        delay400();
        outb(R_CMD, g_lba48 ? 0xEA : 0xE7);
        ok = false;
        for (uint32_t i = 0; i < 50000000u; ++i) { uint8_t s = inb(R_CMD); if (!(s & ST_BSY)) { ok = !(s & ST_DF); break; } }
    }
    unlock();
    return ok;
}

static void ata_string(char* out, const uint16_t* w, int words, int outsz) {
    int n = 0;
    for (int i = 0; i < words && n + 2 < outsz; ++i) { out[n++] = (char)(w[i] >> 8); out[n++] = (char)(w[i] & 0xFF); }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) --n;
    out[n] = 0;
}

int ide_enumerate(blk_dev_t* out, int max) {
    if (max < 1) return 0;
    if (inb(R_CMD) == 0xFF) { serial_puts("[IDE] no legacy IDE controller (floating bus)\n"); return 0; }
    /* Soft reset: after UEFI/GRUB used the drive its task-file registers hold leftovers that look like a non-ATA device. */
    outb(R_ALT, 0x04);
    for (int i = 0; i < 20; ++i) delay400();
    outb(R_ALT, 0x00);
    for (int i = 0; i < 2000; ++i) delay400();
    for (uint32_t i = 0; i < 4000000u; ++i) { uint8_t st = inb(R_CMD); if (!(st & ST_BSY) && (st & ST_DRDY)) break; }

    outb(R_DEV, 0xA0 | 0x40 * 0);
    outb(R_DEV, 0xE0);
    delay400();
    outb(R_COUNT, 0);
    outb(R_CMD, 0xEC);                                                      /* IDENTIFY DEVICE */
    delay400();
    if (inb(R_CMD) == 0) { serial_puts("[IDE] no drive on the primary master\n"); return 0; }
    if (!wait_not_busy()) return 0;
    if (inb(R_LBA1) != 0 || inb(R_LBA2) != 0) { serial_puts("[IDE] master is not an ATA disk (ATAPI/SATA signature)\n"); return 0; }
    if (!wait_drq()) { serial_puts("[IDE] IDENTIFY failed\n"); return 0; }
    uint16_t id[256];
    for (int i = 0; i < 256; ++i) id[i] = inw(R_DATA);

    g_lba48 = (id[83] & (1u << 10)) != 0;
    g_trim = (id[169] & 1u) != 0;
    uint64_t sectors = g_lba48 ? ((uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48))
                               : ((uint64_t)id[60] | ((uint64_t)id[61] << 16));
    if (g_lba48 && sectors == 0) sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (sectors == 0) return 0;

    /* Bus-master DMA: the PCI IDE function's BAR4 */
    g_bm = 0;
    for (pci_dev_t* dev = pci_get_first_device(); dev; dev = pci_get_next_device(dev)) {
        if (dev->class_code != PCI_CLASS_STORAGE || dev->subclass != 0x01) continue;
        uint64_t bar4 = dev->bar[4];
        if (!(bar4 & 1) || !(bar4 & ~3ull)) continue;
        pci_enable_bus_mastering(dev);
        pci_enable_mmio(dev);
        g_bm = (uint16_t)(bar4 & ~3ull);
        break;
    }
    if (g_bm) {
        g_prd = (uint64_t*)kmalloc_aligned(4096, 4096);
        if (g_prd) { memset(g_prd, 0, 4096); g_prd_phys = paging_virt_to_phys((uint64_t)(uintptr_t)g_prd); if (g_prd_phys >= 0x100000000ull) g_bm = 0; }
        else g_bm = 0;
    }
    outb(R_ALT, 0x02);                                                      /* nIEN: this driver polls */

    blk_dev_t* d = &out[0];
    memset(d, 0, sizeof *d);
    ata_string(d->model, id + 27, 20, sizeof d->model);
    d->sectors = sectors;
    d->read = ide_read; d->write = ide_write; d->flush = ide_flush; d->trim = g_trim ? ide_trim : NULL;
    const char* nm = "IDE primary master";
    for (int i = 0; nm[i]; ++i) d->name[i] = nm[i];
    serial_puts("[IDE] "); serial_puts(d->model); serial_puts(": "); serial_putdec(sectors / 2048ull); serial_puts(" MiB, ");
    serial_puts(g_lba48 ? "LBA48, " : "LBA28, "); serial_puts(g_bm ? "bus-master DMA\n" : "PIO\n");
    return 1;
}
