/**
 * nvme.c - NVMe controller driver (PCIe SSDs).
 *
 * Polled admin queue plus an I/O queue pair 64 entries deep; large requests are cut into commands of up to 256 KiB (or the
 * controller's MDTS, if smaller) and kept in flight together, doorbells rung once per batch. Namespace 1 with 512-byte
 * logical blocks. Each transfer is described with PRPs built page by page (a per-command PRP list page for more than two
 * pages), so buffers need not be physically contiguous.
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

#define NVME_CAP   0x00
#define NVME_VS    0x08
#define NVME_CC    0x14
#define NVME_CSTS  0x1C
#define NVME_AQA   0x24
#define NVME_ASQ   0x28
#define NVME_ACQ   0x30
#define ADMIN_QD   16u
#define IO_QD      64u
#define CHUNK_SECTORS 512u               /* 256 KiB */

typedef struct {
    uint8_t *sq; uint64_t sq_phys; uint32_t sq_tail, sq_head;
    uint8_t *cq; uint64_t cq_phys; uint32_t cq_head; uint32_t phase;
    uint32_t qid, depth;
} nvme_q_t;

static volatile uint8_t *s_bar;
static uint32_t s_stride;
static nvme_q_t s_admin, s_io;
static uint16_t s_cid;
static bool s_probed, s_ready, s_trim;
static uint64_t s_nsze;
static uint32_t s_chunk = CHUNK_SECTORS;
static uint8_t *s_prp[IO_QD]; static uint64_t s_prp_phys[IO_QD];

static inline int popc64(uint64_t v) { int n = 0; while (v) { v &= v - 1; ++n; } return n; }
static inline uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(s_bar + off); }
static inline void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(s_bar + off) = v; }
static inline uint64_t rd64(uint32_t off) { return (uint64_t)rd32(off) | ((uint64_t)rd32(off + 4) << 32); }
static inline void wr64(uint32_t off, uint64_t v) { wr32(off, (uint32_t)v); wr32(off + 4, (uint32_t)(v >> 32)); }

static void *dma_page(uint64_t *phys) {
    void *p = kmalloc_aligned(4096, 4096);
    if (!p) return NULL;
    memset(p, 0, 4096);
    *phys = paging_virt_to_phys((uint64_t)(uintptr_t)p);
    return p;
}

static bool wait_rdy(uint32_t want, uint32_t ms) {
    uint64_t t0 = get_timer_ticks();
    for (uint64_t spin = 0; spin < (uint64_t)ms * 2000000ull; ++spin) {
        if ((rd32(NVME_CSTS) & 1u) == want) return true;
        if (get_timer_ticks() - t0 > ms) break;
        __asm__ volatile("pause");
    }
    return (rd32(NVME_CSTS) & 1u) == want;
}

static void put_sqe(nvme_q_t *q, uint32_t opcode, uint16_t cid, uint32_t nsid, uint64_t prp1, uint64_t prp2, uint32_t c10, uint32_t c11, uint32_t c12) {
    uint32_t *sqe = (uint32_t *)(q->sq + (size_t)q->sq_tail * 64u);
    memset(sqe, 0, 64);
    sqe[0] = opcode | ((uint32_t)cid << 16);
    sqe[1] = nsid;
    sqe[6] = (uint32_t)prp1; sqe[7] = (uint32_t)(prp1 >> 32);
    sqe[8] = (uint32_t)prp2; sqe[9] = (uint32_t)(prp2 >> 32);
    sqe[10] = c10; sqe[11] = c11; sqe[12] = c12;
    q->sq_tail = (q->sq_tail + 1u) % q->depth;
}
static void ring_sq(nvme_q_t *q) { __sync_synchronize(); wr32(0x1000u + (2u * q->qid) * s_stride, q->sq_tail); }

/* Takes the next completion if there is one: returns 1 and its cid/status, else 0. */
static int reap(nvme_q_t *q, uint16_t *cid, int *status) {
    volatile uint32_t *cqe = (volatile uint32_t *)(q->cq + (size_t)q->cq_head * 16u);
    uint32_t dw3 = cqe[3];
    if (((dw3 >> 16) & 1u) != q->phase) return 0;
    q->sq_head = cqe[2] & 0xFFFFu;
    *cid = (uint16_t)(dw3 & 0xFFFFu);
    *status = (int)((dw3 >> 17) & 0x7FFFu);
    q->cq_head = (q->cq_head + 1u) % q->depth;
    if (q->cq_head == 0) q->phase ^= 1u;
    return 1;
}
static void ring_cq(nvme_q_t *q) { wr32(0x1000u + (2u * q->qid + 1u) * s_stride, q->cq_head); }

/* Admin command, synchronous. Returns the status (0 = success) or -1 on timeout. */
static int admin(uint32_t opcode, uint32_t nsid, uint64_t prp1, uint32_t c10, uint32_t c11) {
    nvme_q_t *q = &s_admin;
    uint16_t cid = ++s_cid;
    put_sqe(q, opcode, cid, nsid, prp1, 0, c10, c11, 0);
    ring_sq(q);
    uint64_t t0 = get_timer_ticks();
    for (;;) {
        uint16_t c; int st;
        if (reap(q, &c, &st)) { ring_cq(q); return st; }
        if (get_timer_ticks() - t0 > 15000) { serial_puts("[NVMe] admin command timeout\n"); return -1; }
        __asm__ volatile("pause");
    }
}

static bool probe_and_init(void) {
    if (s_probed) return s_ready;
    s_probed = true;
    pci_dev_t *dev = pci_get_device_by_class_and_prog_if(PCI_CLASS_STORAGE, 0x08, 0x02);
    if (!dev) return false;
    uint64_t bar = dev->bar[0];
    uint64_t base = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2) base |= ((uint64_t)pci_read_dword(dev->bus, dev->slot, dev->func, PCI_BAR1) << 32);
    if (!base || (bar & 1)) { serial_puts("[NVMe] BAR0 unassigned\n"); return false; }
    pci_enable_bus_mastering(dev);
    pci_enable_mmio(dev);
    for (uint64_t off = 0; off < 0x4000; off += 4096) {
        uint64_t virt = PHYS_TO_VIRT(base + off);
        if (!paging_map_range(virt, base + off, 4096, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE) && !paging_is_present(virt)) {
            serial_puts("[NVMe] cannot map the controller registers\n");
            return false;
        }
    }
    phys_memory_reserve_range((phys_addr_t)base, 0x4000);
    s_bar = (volatile uint8_t *)(uintptr_t)PHYS_TO_VIRT(base);

    uint64_t cap = rd64(NVME_CAP);
    s_stride = 4u << ((cap >> 32) & 0xFu);
    uint32_t to_ms = (uint32_t)(((cap >> 24) & 0xFFu) + 1u) * 500u;
    uint32_t mqes = (uint32_t)(cap & 0xFFFFu) + 1u;                 /* maximum queue entries supported */
    serial_puts("[NVMe] controller, version 0x"); serial_puthex(rd32(NVME_VS)); serial_puts("\n");

    wr32(NVME_CC, 0);
    if (!wait_rdy(0, to_ms)) { serial_puts("[NVMe] controller would not reset\n"); return false; }
    s_admin.qid = 0; s_admin.depth = ADMIN_QD; s_io.qid = 1; s_io.depth = IO_QD > mqes ? mqes : IO_QD;
    s_admin.sq = (uint8_t *)dma_page(&s_admin.sq_phys); s_admin.cq = (uint8_t *)dma_page(&s_admin.cq_phys);
    s_io.sq = (uint8_t *)dma_page(&s_io.sq_phys);       s_io.cq = (uint8_t *)dma_page(&s_io.cq_phys);
    if (!s_admin.sq || !s_admin.cq || !s_io.sq || !s_io.cq) { serial_puts("[NVMe] out of DMA memory\n"); return false; }
    s_admin.phase = s_io.phase = 1;
    wr32(NVME_AQA, ((ADMIN_QD - 1u) << 16) | (ADMIN_QD - 1u));
    wr64(NVME_ASQ, s_admin.sq_phys);
    wr64(NVME_ACQ, s_admin.cq_phys);
    wr32(NVME_CC, (6u << 16) | (4u << 20) | 1u);             /* 64-byte SQE, 16-byte CQE, 4 KiB pages, NVM command set, enable */
    if (!wait_rdy(1, to_ms)) { serial_puts("[NVMe] controller did not become ready\n"); return false; }

    uint64_t idp; uint8_t *id = (uint8_t *)dma_page(&idp);
    if (!id) return false;
    /* Identify controller: maximum data transfer size */
    if (admin(0x06, 0, idp, 1 /* CNS 1 */, 0) == 0) {
        uint8_t mdts = id[77];
        if (mdts) { uint32_t max_sectors = (1u << mdts) * 8u; if (max_sectors < s_chunk) s_chunk = max_sectors; }
    }
    s_trim = (id[520] & (1u << 2)) != 0;                      /* ONCS: Dataset Management supported */
    memset(id, 0, 4096);
    if (admin(0x06, 1, idp, 0 /* CNS 0: namespace */, 0) != 0) { serial_puts("[NVMe] identify namespace failed\n"); return false; }
    s_nsze = *(uint64_t *)id;
    uint8_t flbas = id[26] & 0xF;
    uint32_t lbaf = *(uint32_t *)(id + 128 + 4u * flbas);
    uint32_t lbads = (lbaf >> 16) & 0xFFu;
    if (lbads != 9) { serial_puts("[NVMe] namespace uses "); serial_putdec(1ull << lbads); serial_puts("-byte blocks; only 512 is supported\n"); return false; }
    if (s_nsze == 0) return false;

    if (admin(0x05, 0, s_io.cq_phys, ((s_io.depth - 1u) << 16) | 1u, 1u /* physically contiguous */) != 0) { serial_puts("[NVMe] create CQ failed\n"); return false; }
    if (admin(0x01, 0, s_io.sq_phys, ((s_io.depth - 1u) << 16) | 1u, (1u << 16) | 1u) != 0) { serial_puts("[NVMe] create SQ failed\n"); return false; }
    kfree(id);
    for (uint32_t i = 0; i < s_io.depth; ++i) { s_prp[i] = (uint8_t *)dma_page(&s_prp_phys[i]); if (!s_prp[i]) { serial_puts("[NVMe] out of DMA memory\n"); return false; } }
    s_ready = true;
    return true;
}

/* Describes one chunk with PRPs. cid selects the PRP list page. Returns false if the buffer cannot be mapped. */
static bool prp_for(uint32_t cid, const uint8_t *buf, size_t len, uint64_t *prp1, uint64_t *prp2) {
    uint64_t pages[80]; int np = 0;
    uintptr_t cur = (uintptr_t)buf; size_t left = len;
    while (left) {
        size_t chunk = 4096 - (cur & 4095u); if (chunk > left) chunk = left;
        uint64_t ph = paging_virt_to_phys((uint64_t)cur);
        if (!ph || np >= 80) return false;
        pages[np++] = ph;
        cur += chunk; left -= chunk;
    }
    *prp1 = pages[0]; *prp2 = 0;
    if (np == 2) *prp2 = pages[1];
    else if (np > 2) {
        uint64_t *list = (uint64_t *)s_prp[cid];
        for (int i = 1; i < np; ++i) list[i - 1] = pages[i];
        *prp2 = s_prp_phys[cid];
    }
    return true;
}

/* ---- concurrent command engine
 * Commands are tracked per command id. Submitting (write the SQ entry, ring the doorbell) is done under a short lock;
 * waiting is lock-free: every waiter polls its own id's state, and whoever gets the reap lock drains the completion queue
 * for everybody. So several threads can have commands in flight at once, up to the queue depth. */
typedef struct { volatile int state; volatile int status; } cmdslot_t;     /* state: 0 free, 1 in flight, 2 completed */
static cmdslot_t s_cs[IO_QD];
static volatile uint32_t s_sub_lock, s_reap_lock;
static uint64_t s_busy;

static void sub_lock(void) { cos_spin_lock(&s_sub_lock); }
static void sub_unlock(void) { cos_spin_unlock(&s_sub_lock); }

/* Claims a free command id (at most depth-1 in flight, so the submission queue can never overflow). */
static int cid_claim(void) {
    int cid = -1;
    sub_lock();
    for (uint32_t i = 0; i + 1u < s_io.depth; ++i)
        if (!(s_busy & (1ull << i))) { s_busy |= 1ull << i; s_cs[i].state = 1; cid = (int)i; break; }
    sub_unlock();
    return cid;
}
static void cid_release(int cid) { sub_lock(); s_busy &= ~(1ull << cid); s_cs[cid].state = 0; sub_unlock(); }

static void reap_some(void) {
    if (__atomic_exchange_n(&s_reap_lock, 1u, __ATOMIC_ACQUIRE)) return;      /* somebody else is already draining */
    uint16_t cid; int st; bool got = false;
    while (reap(&s_io, &cid, &st)) {
        if (cid < IO_QD) { s_cs[cid].status = st; __sync_synchronize(); s_cs[cid].state = 2; }
        got = true;
    }
    if (got) ring_cq(&s_io);
    __atomic_store_n(&s_reap_lock, 0u, __ATOMIC_RELEASE);
}

/* Polls once: -1 still in flight, otherwise the NVMe status (0 = success). */
static int cid_poll(int cid) {
    if (s_cs[cid].state != 2) reap_some();
    return s_cs[cid].state == 2 ? s_cs[cid].status : -1;
}

/* Single-command helper (flush, dataset management): submit and wait for it. */
static bool simple_io(uint32_t opcode, uint64_t prp1, uint32_t c10, uint32_t c11) {
    int cid;
    uint32_t spin = 0;
    while ((cid = cid_claim()) < 0) { if ((++spin & 0x3Fu) == 0) thread_yield(); else __asm__ volatile("pause"); }
    sub_lock();
    put_sqe(&s_io, opcode, (uint16_t)cid, 1, prp1, 0, c10, c11, 0);
    ring_sq(&s_io);
    sub_unlock();
    uint64_t t0 = get_timer_ticks();
    int st;
    spin = 0;
    while ((st = cid_poll(cid)) < 0) {
        if (get_timer_ticks() - t0 > 30000) { serial_puts("[NVMe] command timeout\n"); return false; }     /* the id stays claimed */
        if ((++spin & 0xFFu) == 0) thread_yield(); else __asm__ volatile("pause");
    }
    cid_release(cid);
    return st == 0;
}

static bool nvme_rw(bool write, uint64_t lba, uint32_t count, uint8_t *buf) {
    uint8_t mine[32]; uint32_t n_mine = 0;
    uint32_t issued = 0, spin = 0;
    uint64_t t0 = get_timer_ticks();
    bool ok = true;
    while (issued < count || n_mine) {
        bool progress = false;
        while (ok && issued < count && n_mine < 32) {
            int cid = cid_claim();
            if (cid < 0) break;
            uint32_t n = count - issued < s_chunk ? count - issued : s_chunk;
            uint64_t p1, p2;
            if (!prp_for((uint32_t)cid, buf + (size_t)issued * 512u, (size_t)n * 512u, &p1, &p2)) { cid_release(cid); ok = false; break; }
            uint64_t l = lba + issued;
            sub_lock();
            put_sqe(&s_io, write ? 0x01 : 0x02, (uint16_t)cid, 1, p1, p2, (uint32_t)l, (uint32_t)(l >> 32), n - 1u);
            ring_sq(&s_io);
            sub_unlock();
            mine[n_mine++] = (uint8_t)cid;
            issued += n; progress = true;
        }
        if (!ok && issued < count) issued = count;                                      /* stop submitting, drain */
        for (uint32_t i = 0; i < n_mine; ) {
            int st = cid_poll(mine[i]);
            if (st >= 0) {
                if (st != 0) { serial_puts("[NVMe] I/O failed, status 0x"); serial_puthex((uint64_t)st); serial_puts("\n"); ok = false; }
                cid_release(mine[i]);
                mine[i] = mine[n_mine - 1]; --n_mine;
                progress = true; t0 = get_timer_ticks();
            } else ++i;
        }
        if (!progress) {
            if (get_timer_ticks() - t0 > 20000) { serial_puts("[NVMe] I/O timeout\n"); ok = false; break; }   /* ids stay claimed */
            if ((++spin & 0xFFu) == 0) thread_yield(); else __asm__ volatile("pause");
        } else spin = 0;
    }
    return ok;
}

static bool nvme_read(blk_dev_t *d, uint64_t lba, uint32_t count, void *buf) { (void)d; return nvme_rw(false, lba, count, (uint8_t *)buf); }
static bool nvme_write(blk_dev_t *d, uint64_t lba, uint32_t count, const void *buf) { (void)d; return nvme_rw(true, lba, count, (uint8_t *)buf); }
static bool nvme_flush(blk_dev_t *d) { (void)d; return simple_io(0x00, 0, 0, 0); }

static bool nvme_trim(blk_dev_t *d, uint64_t lba, uint64_t count) {
    (void)d;
    uint64_t ph;
    uint32_t *r = (uint32_t *)dma_page(&ph);
    if (!r) return false;
    bool ok = true;
    while (count && ok) {
        uint32_t n = 0;
        memset(r, 0, 4096);
        while (count && n < 256) {                          /* 16-byte range entries: attributes, length, start LBA */
            uint32_t len = count > 65536 ? 65536u : (uint32_t)count;
            r[n * 4 + 1] = len; r[n * 4 + 2] = (uint32_t)lba; r[n * 4 + 3] = (uint32_t)(lba >> 32);
            ++n; lba += len; count -= len;
        }
        ok = simple_io(0x09, ph, n - 1u, 0x4u /* AD: deallocate */);
    }
    kfree(r);
    return ok;
}

int nvme_enumerate(blk_dev_t *out, int max) {
    if (max < 1 || !probe_and_init()) return 0;
    blk_dev_t *d = &out[0];
    memset(d, 0, sizeof *d);
    d->sectors = s_nsze;
    d->read = nvme_read; d->write = nvme_write; d->flush = nvme_flush; d->trim = s_trim ? nvme_trim : NULL;
    const char *n = "NVMe ns 1";
    for (int i = 0; n[i]; ++i) d->name[i] = n[i];
    serial_puts("[NVMe] namespace 1: "); serial_putdec(s_nsze / 2048ULL); serial_puts(" MiB, queue depth "); serial_putdec(s_io.depth);
    serial_puts(", up to "); serial_putdec(s_chunk / 2); serial_puts(" KiB per command\n");
    return 1;
}
