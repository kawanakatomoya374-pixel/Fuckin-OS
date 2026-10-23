/**
 * ahci.c - AHCI (SATA) host controller driver: the path to HDDs and SSDs on machines without legacy IDE.
 *
 * Polled, with up to 32 commands in flight: Native Command Queuing (READ/WRITE FPDMA QUEUED) when the drive and the HBA
 * support it - which is what lets an SSD overlap its internal channels and a HDD reorder seeks - otherwise one DMA EXT
 * command at a time. Large requests are cut into 256 KiB commands that are all kept in flight. Firmware and boot loaders
 * leave the ports running with their own command lists, so every port is stopped and re-programmed with C-OS's own
 * structures.
 */
#include "sync.h"
#include "blk.h"
#include "pci.h"
#include "mm/paging.h"
#include "memory_physical.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"

#define HBA_CAP     0x00
#define HBA_GHC     0x04
#define HBA_PI      0x0C
#define HBA_CAP2    0x24
#define HBA_BOHC    0x28
#define PORT(p, r)  (0x100u + (p) * 0x80u + (r))
#define P_CLB 0x00
#define P_CLBU 0x04
#define P_FB 0x08
#define P_FBU 0x0C
#define P_IS 0x10
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_SACT 0x34
#define P_CI 0x38

#define CMD_ST  (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)

#define CHUNK_SECTORS 512u             /* 256 KiB per command */
#define MAX_PRD       160u             /* 256 KiB unaligned = 65 pages, merged entries are fewer; a page holds (4096-0x80)/16 = 248 */
#define NSLOTS_MAX    32

typedef struct { uint32_t dba, dbau, rsvd, dbc; } prd_t;

typedef struct {
    volatile uint8_t *abar;
    uint32_t port;
    uint8_t *clb; uint64_t clb_phys;
    uint8_t *fis; uint64_t fis_phys;
    uint8_t *ctba[NSLOTS_MAX]; uint64_t ctba_phys[NSLOTS_MAX];
    uint32_t nslots;
    bool ncq;
    bool trim;
    volatile uint32_t lock;
    uint32_t busy;               /* slots with a command in flight (under lock) */
    bool exclusive;              /* a non-queued command owns the port: no other command may be in flight */
    uint32_t gen;                /* bumped when the port is reset: commands issued before it have failed */
} ahci_port_t;

static ahci_port_t s_ports[16];
static int s_nports;

static inline int popc64(uint64_t v) { int n = 0; while (v) { v &= v - 1; ++n; } return n; }
static inline uint32_t rd(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }

static bool wait_clear(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t ms) {
    uint64_t t0 = get_timer_ticks();
    for (uint64_t spin = 0; spin < (uint64_t)ms * 2000000ull; ++spin) {
        if (!(rd(b, off) & mask)) return true;
        if (get_timer_ticks() - t0 > ms) break;
        __asm__ volatile("pause");
    }
    return !(rd(b, off) & mask);
}

static void *dma_page(uint64_t *phys) {
    void *p = kmalloc_aligned(4096, 4096);
    if (!p) return NULL;
    memset(p, 0, 4096);
    *phys = paging_virt_to_phys((uint64_t)(uintptr_t)p);
    return p;
}

static bool port_stop(ahci_port_t *p) {
    volatile uint8_t *b = p->abar;
    uint32_t o = PORT(p->port, 0);
    wr(b, o + P_CMD, rd(b, o + P_CMD) & ~CMD_ST);
    if (!wait_clear(b, o + P_CMD, CMD_CR, 500)) return false;
    wr(b, o + P_CMD, rd(b, o + P_CMD) & ~CMD_FRE);
    return wait_clear(b, o + P_CMD, CMD_FR, 500);
}

static bool port_start(ahci_port_t *p) {
    volatile uint8_t *b = p->abar;
    uint32_t o = PORT(p->port, 0);
    wr(b, o + P_CLB, (uint32_t)p->clb_phys); wr(b, o + P_CLBU, (uint32_t)(p->clb_phys >> 32));
    wr(b, o + P_FB, (uint32_t)p->fis_phys);  wr(b, o + P_FBU, (uint32_t)(p->fis_phys >> 32));
    wr(b, o + P_SERR, 0xFFFFFFFFu);
    wr(b, o + P_IS, 0xFFFFFFFFu);
    wr(b, o + P_CMD, rd(b, o + P_CMD) | CMD_FRE);
    if (!wait_clear(b, o + P_TFD, 0x88u, 1000)) return false;     /* BSY | DRQ must be clear before ST may be set */
    wr(b, o + P_CMD, rd(b, o + P_CMD) | CMD_ST);
    return true;
}

/* Maps a kernel buffer to PRD entries one page at a time (the buffer need not be physically contiguous). */
static int build_prds(prd_t *prd, const void *buf, size_t len) {
    uintptr_t v = (uintptr_t)buf;
    int n = 0;
    uint64_t prev_end = 0;
    while (len) {
        size_t chunk = 4096 - (v & 4095u);
        if (chunk > len) chunk = len;
        uint64_t ph = paging_virt_to_phys((uint64_t)v);
        if (!ph) return -1;
        if (n > 0 && ph == prev_end && (prd[n - 1].dbc & 0x3FFFFFu) + 1u + chunk <= (4u << 20)) {
            prd[n - 1].dbc += (uint32_t)chunk;
        } else {
            if (n >= (int)MAX_PRD) return -1;
            prd[n].dba = (uint32_t)ph; prd[n].dbau = (uint32_t)(ph >> 32); prd[n].rsvd = 0;
            prd[n].dbc = (uint32_t)chunk - 1u;
            ++n;
        }
        prev_end = ph + chunk;
        v += chunk; len -= chunk;
    }
    return n;
}

/* Fills slot 'slot' with one command and starts it. cmd/features follow the ATA command set; for NCQ the sector count
 * goes in the features field and the tag in the count field. */
static bool slot_issue(ahci_port_t *p, uint32_t slot, uint8_t cmd, uint8_t feat, uint64_t lba, uint32_t count, bool write, void *buf, size_t len, bool ncq) {
    volatile uint8_t *b = p->abar;
    uint32_t o = PORT(p->port, 0);
    uint8_t *tbl = p->ctba[slot];
    prd_t *prd = (prd_t *)(tbl + 0x80);
    int nprd = len ? build_prds(prd, buf, len) : 0;
    if (nprd < 0) return false;

    memset(tbl, 0, 0x80);
    tbl[0] = 0x27; tbl[1] = 0x80; tbl[2] = cmd;
    if (ncq) { tbl[3] = (uint8_t)count; tbl[11] = (uint8_t)(count >> 8); tbl[12] = (uint8_t)(slot << 3); tbl[7] = (uint8_t)(1u << 6); }
    else     { tbl[3] = feat; tbl[12] = (uint8_t)count; tbl[13] = (uint8_t)(count >> 8); tbl[7] = (uint8_t)(1u << 6); }
    tbl[4] = (uint8_t)lba; tbl[5] = (uint8_t)(lba >> 8); tbl[6] = (uint8_t)(lba >> 16);
    tbl[8] = (uint8_t)(lba >> 24); tbl[9] = (uint8_t)(lba >> 32); tbl[10] = (uint8_t)(lba >> 40);

    uint32_t *h = (uint32_t *)(p->clb + (size_t)slot * 32u);
    h[0] = 5u | (write ? (1u << 6) : 0u) | ((uint32_t)nprd << 16);
    h[1] = 0;
    h[2] = (uint32_t)p->ctba_phys[slot]; h[3] = (uint32_t)(p->ctba_phys[slot] >> 32);
    __sync_synchronize();
    if (ncq) wr(b, o + P_SACT, 1u << slot);                       /* writing 1s only sets those bits */
    wr(b, o + P_CI, 1u << slot);
    return true;
}

static void lock_port(ahci_port_t *p) { cos_spin_lock(&p->lock); }
static void unlock_port(ahci_port_t *p) { cos_spin_unlock(&p->lock); }

static void port_recover(ahci_port_t *p) {
    (void)port_stop(p);
    (void)port_start(p);
}

/* ---- concurrent command engine
 * Any number of threads may have commands in flight on one port: a slot is claimed and its command started under the port
 * lock (a few register writes), then the owner waits for its own slot's completion bit WITHOUT holding any lock, so other
 * threads keep submitting. Queued (NCQ) commands share the port; a non-queued command (IDENTIFY, FLUSH, TRIM, DMA EXT on
 * a drive without NCQ) must have it to itself ("exclusive"). A port reset bumps 'gen' and fails everything issued before. */
static bool slot_claim(ahci_port_t *p, bool queued, uint32_t *slot, uint32_t *gen) {
    bool ok = false;
    lock_port(p);
    if (!p->exclusive) {
        if (!queued) {
            if (p->busy == 0) { p->exclusive = true; p->busy = 1u; *slot = 0; ok = true; }
        } else {
            uint32_t mask = p->nslots >= 32 ? 0xFFFFFFFFu : ((1u << p->nslots) - 1u);
            uint32_t freebits = ~p->busy & mask;
            if (freebits) { uint32_t sl = (uint32_t)__builtin_ctz(freebits); p->busy |= 1u << sl; *slot = sl; ok = true; }
        }
        if (ok) *gen = p->gen;
    }
    unlock_port(p);
    return ok;
}

static void slot_release(ahci_port_t *p, uint32_t slot, uint32_t gen) {
    lock_port(p);
    if (p->gen == gen) { p->busy &= ~(1u << slot); if (p->exclusive) p->exclusive = false; }
    unlock_port(p);
}

/* Waits for the command in 'slot'. Returns true on success. */
static bool slot_wait(ahci_port_t *p, uint32_t slot, uint32_t gen, bool queued, uint32_t timeout_ms) {
    volatile uint8_t *b = p->abar;
    uint32_t o = PORT(p->port, 0);
    uint64_t t0 = get_timer_ticks();
    uint32_t spin = 0;
    for (;;) {
        if (p->gen != gen) return false;                              /* the port was reset under us */
        uint32_t active = queued ? rd(b, o + P_SACT) : rd(b, o + P_CI);
        if (!(active & (1u << slot))) return !(rd(b, o + P_TFD) & 1u);
        bool timed_out = get_timer_ticks() - t0 > timeout_ms;
        if ((rd(b, o + P_IS) & (1u << 30)) || timed_out) {
            serial_puts(timed_out ? "[AHCI] command timeout\n" : "[AHCI] task file error\n");
            lock_port(p);
            if (p->gen == gen) { port_recover(p); p->gen++; p->busy = 0; p->exclusive = false; }
            unlock_port(p);
            return false;
        }
        /* A disk answers in microseconds (SSD) to milliseconds (HDD seek): poll briefly, then let other threads run. */
        if ((++spin & 0xFFu) == 0) thread_yield(); else __asm__ volatile("pause");
    }
}

/* One non-queued command (no data, or a single buffer), alone on the port. */
static bool simple_cmd(ahci_port_t *p, uint8_t cmd, uint8_t feat, uint32_t count, bool write, void *buf, size_t len) {
    uint32_t slot, gen;
    uint32_t spin = 0;
    while (!slot_claim(p, false, &slot, &gen)) { if ((++spin & 0x3Fu) == 0) thread_yield(); else __asm__ volatile("pause"); }
    lock_port(p);
    bool ok = slot_issue(p, slot, cmd, feat, 0, count, write, buf, len, false);
    unlock_port(p);
    if (ok) ok = slot_wait(p, slot, gen, false, 30000);
    slot_release(p, slot, gen);
    return ok;
}

/* Reads or writes 'count' sectors with up to nslots commands in flight (shared with other threads' requests). */
static bool port_rw(ahci_port_t *p, uint64_t lba, uint32_t count, void *buf, bool write) {
    bool queued = p->ncq;
    uint8_t cmd = queued ? (write ? 0x61 : 0x60) : (write ? 0x35 : 0x25);
    uint32_t limit = queued ? p->nslots : 1;
    uint8_t *cur = (uint8_t *)buf;
    uint8_t mine[NSLOTS_MAX]; uint32_t gens[NSLOTS_MAX]; uint32_t n_mine = 0;
    uint32_t issued = 0, spin = 0;
    bool ok = true;

    while (issued < count || n_mine) {
        bool progress = false;
        while (ok && issued < count && n_mine < limit) {
            uint32_t slot, gen;
            if (!slot_claim(p, queued, &slot, &gen)) break;
            uint32_t n = count - issued < CHUNK_SECTORS ? count - issued : CHUNK_SECTORS;
            lock_port(p);
            bool issued_ok = slot_issue(p, slot, cmd, 0, lba + issued, n, write, cur + (size_t)issued * 512u, (size_t)n * 512u, queued);
            unlock_port(p);
            if (!issued_ok) { slot_release(p, slot, gen); ok = false; break; }
            mine[n_mine] = (uint8_t)slot; gens[n_mine] = gen; ++n_mine;
            issued += n; progress = true;
        }
        if (!ok && issued < count) issued = count;                       /* stop submitting, drain what is in flight */
        volatile uint8_t *b = p->abar;
        uint32_t o = PORT(p->port, 0);
        for (uint32_t i = 0; i < n_mine; ) {
            uint32_t slot = mine[i], gen = gens[i];
            bool failed = false, finished = false;
            if (p->gen != gen) { failed = true; finished = true; }
            else {
                uint32_t active = queued ? rd(b, o + P_SACT) : rd(b, o + P_CI);
                if (!(active & (1u << slot))) { finished = true; failed = (rd(b, o + P_TFD) & 1u) != 0; }
                else if (rd(b, o + P_IS) & (1u << 30)) { finished = slot_wait(p, slot, gen, queued, 1) || true; failed = true; }
            }
            if (finished) {
                if (failed) ok = false;
                slot_release(p, slot, gen);
                mine[i] = mine[n_mine - 1]; gens[i] = gens[n_mine - 1]; --n_mine;
                progress = true;
            } else ++i;
        }
        if (!progress) {
            /* nothing moved: wait a little (and bound how long a stuck command can hold us) */
            if ((++spin & 0xFFu) == 0) thread_yield(); else __asm__ volatile("pause");
            if (spin > 40000000u && n_mine) {                           /* ~tens of seconds of polling */
                for (uint32_t i = 0; i < n_mine; ++i) (void)slot_wait(p, mine[i], gens[i], queued, 1);
                ok = false; break;
            }
        } else spin = 0;
    }
    return ok;
}

static bool ahci_read(blk_dev_t *d, uint64_t lba, uint32_t count, void *buf) { return port_rw((ahci_port_t *)d->ctx, lba, count, buf, false); }
static bool ahci_write(blk_dev_t *d, uint64_t lba, uint32_t count, const void *buf) { return port_rw((ahci_port_t *)d->ctx, lba, count, (void *)buf, true); }
/* DATA SET MANAGEMENT / TRIM: tells an SSD which blocks no longer hold data, so it can erase them ahead of time instead of
 * copying them around during garbage collection. */
static bool ahci_trim(blk_dev_t *d, uint64_t lba, uint64_t count) {
    ahci_port_t *p = (ahci_port_t *)d->ctx;
    uint64_t ph;
    uint64_t *ranges = (uint64_t *)dma_page(&ph);
    if (!ranges) return false;
    bool ok = true;
    while (count && ok) {
        uint32_t n = 0;
        memset(ranges, 0, 512);
        while (count && n < 64) {                         /* 64 range entries of up to 65535 sectors fit one 512-byte block */
            uint32_t len = count > 65535 ? 65535 : (uint32_t)count;
            ranges[n++] = (lba & 0xFFFFFFFFFFFFull) | ((uint64_t)len << 48);
            lba += len; count -= len;
        }
        ok = simple_cmd(p, 0x06 /* DATA SET MANAGEMENT */, 0x01 /* TRIM */, 1, true, ranges, 512);
    }
    kfree(ranges);
    return ok;
}

static bool ahci_flush(blk_dev_t *d) { return simple_cmd((ahci_port_t *)d->ctx, 0xEA /* FLUSH CACHE EXT */, 0, 0, false, NULL, 0); }

static void ata_string(char *out, const uint16_t *w, int words, int outsz) {
    int n = 0;
    for (int i = 0; i < words && n + 2 < outsz; ++i) { out[n++] = (char)(w[i] >> 8); out[n++] = (char)(w[i] & 0xFF); }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == 0)) --n;
    out[n] = 0;
}

static bool map_controller(pci_dev_t *dev, volatile uint8_t **abar_out) {
    uint64_t base = dev->bar[5] & ~0xFULL;
    if (!base || (dev->bar[5] & 1)) { serial_puts("[AHCI] ABAR (BAR5) unassigned\n"); return false; }
    pci_enable_bus_mastering(dev);
    pci_enable_mmio(dev);
    for (uint64_t off = 0; off < 0x2000; off += 4096) {
        uint64_t virt = PHYS_TO_VIRT(base + off);
        if (!paging_map_range(virt, base + off, 4096, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE) && !paging_is_present(virt)) {
            serial_puts("[AHCI] cannot map the HBA registers\n");
            return false;
        }
    }
    phys_memory_reserve_range((phys_addr_t)base, 0x2000);
    *abar_out = (volatile uint8_t *)(uintptr_t)PHYS_TO_VIRT(base);
    return true;
}

static int scan_controller(volatile uint8_t *b, blk_dev_t *out, int max) {
    /* BIOS/OS handoff, then AHCI mode on */
    if (rd(b, HBA_CAP2) & 1u) {
        wr(b, HBA_BOHC, rd(b, HBA_BOHC) | 2u);
        (void)wait_clear(b, HBA_BOHC, 1u, 1000);
    }
    wr(b, HBA_GHC, rd(b, HBA_GHC) | (1u << 31));
    uint32_t cap = rd(b, HBA_CAP), pi = rd(b, HBA_PI);
    uint32_t hba_slots = ((cap >> 8) & 0x1Fu) + 1u;
    bool hba_ncq = (cap & (1u << 30)) != 0;
    int found = 0;
    serial_puts("[AHCI] controller, version 0x"); serial_puthex(rd(b, 0x10)); serial_puts(", ports 0x"); serial_puthex(pi);
    serial_puts(", command slots "); serial_putdec(hba_slots); serial_puts(hba_ncq ? ", NCQ\n" : "\n");
    for (uint32_t port = 0; port < 32 && found < max && s_nports < 16; ++port) {
        if (!(pi & (1u << port))) continue;
        uint32_t ssts = rd(b, PORT(port, P_SSTS));
        if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) continue;           /* no device / link not active */
        if (rd(b, PORT(port, P_SIG)) != 0x00000101u) continue;                 /* ATAPI, port multiplier, enclosure: not a disk */

        ahci_port_t *p = &s_ports[s_nports];
        memset(p, 0, sizeof *p);
        p->abar = b; p->port = port;
        p->nslots = hba_slots > NSLOTS_MAX ? NSLOTS_MAX : hba_slots;
        p->clb = (uint8_t *)dma_page(&p->clb_phys);
        p->fis = (uint8_t *)dma_page(&p->fis_phys);
        bool mem_ok = p->clb && p->fis;
        for (uint32_t s = 0; s < p->nslots && mem_ok; ++s) { p->ctba[s] = (uint8_t *)dma_page(&p->ctba_phys[s]); mem_ok = p->ctba[s] != NULL; }
        if (!mem_ok) { serial_puts("[AHCI] out of DMA memory\n"); break; }
        if (!port_stop(p)) { serial_puts("[AHCI] port would not stop, skipped\n"); continue; }
        if (!port_start(p)) { serial_puts("[AHCI] port would not become ready, skipped\n"); continue; }

        uint64_t idp;
        uint16_t *idbuf = (uint16_t *)dma_page(&idp);
        if (!idbuf || !simple_cmd(p, 0xEC /* IDENTIFY */, 0, 0, false, idbuf, 512)) { serial_puts("[AHCI] IDENTIFY failed\n"); continue; }
        bool ext48 = (idbuf[83] & (1u << 10)) != 0;
        uint64_t sectors = ((uint64_t)idbuf[100]) | ((uint64_t)idbuf[101] << 16) | ((uint64_t)idbuf[102] << 32) | ((uint64_t)idbuf[103] << 48);
        if (!ext48 || sectors == 0) sectors = (uint64_t)idbuf[60] | ((uint64_t)idbuf[61] << 16);
        bool drive_ncq = (idbuf[76] & (1u << 8)) != 0;
        uint32_t qd = (idbuf[75] & 0x1Fu) + 1u;                                   /* the drive's own queue depth */
        p->ncq = hba_ncq && drive_ncq && ext48;
        if (p->ncq && qd < p->nslots) p->nslots = qd;
        p->trim = (idbuf[169] & 1u) != 0;
        bool big_sectors = (idbuf[106] & (1u << 14)) && (idbuf[106] & (1u << 12));
        blk_dev_t *d = &out[found];
        memset(d, 0, sizeof *d);
        ata_string(d->model, idbuf + 27, 20, sizeof d->model);
        kfree(idbuf);
        if (big_sectors) { serial_puts("[AHCI] drive uses logical sectors larger than 512 bytes; not supported\n"); continue; }
        if (!ext48) { serial_puts("[AHCI] drive lacks LBA48; not supported\n"); continue; }
        d->sectors = sectors;
        d->ctx = p; d->read = ahci_read; d->write = ahci_write; d->flush = ahci_flush; d->trim = p->trim ? ahci_trim : NULL;
        d->name[0] = 'A'; d->name[1] = 'H'; d->name[2] = 'C'; d->name[3] = 'I'; d->name[4] = ' '; d->name[5] = 'p'; d->name[6] = (char)('0' + port % 10); d->name[7] = 0;
        serial_puts("[AHCI] port "); serial_putdec(port); serial_puts(": "); serial_puts(d->model); serial_puts(", ");
        serial_putdec(sectors / 2048ULL); serial_puts(" MiB, "); if (p->trim) serial_puts("TRIM, ");
        if (p->ncq) { serial_puts("NCQ depth "); serial_putdec(p->nslots); serial_puts("\n"); } else serial_puts("no NCQ\n");
        ++s_nports; ++found;
    }
    return found;
}

int ahci_enumerate(blk_dev_t *out, int max) {
    int total = 0;
    for (pci_dev_t *dev = pci_get_first_device(); dev && total < max; dev = pci_get_next_device(dev)) {
        if (dev->class_code != PCI_CLASS_STORAGE || dev->subclass != 0x06 || dev->prog_if != 0x01) continue;
        volatile uint8_t *abar;
        if (!map_controller(dev, &abar)) continue;
        total += scan_controller(abar, out + total, max - total);
    }
    return total;
}
