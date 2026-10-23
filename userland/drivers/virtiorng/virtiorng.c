/* virtiorng.c - a real device driver written as a kernel module: the virtio random-number device (legacy / transitional PCI interface, I/O port BAR0).
 *
 * It uses everything a driver needs from the DEVICE level: PCI configuration reads and enabling, I/O ports, DMA memory for the virtqueue and the receive
 * buffer, and an interrupt line (when a free one is routed to the device; otherwise it polls). Programs get the random bytes through:
 *   0x1030  rng_read(buf, n)   fills buf with n (1..4096) random bytes from the device; returns n, -1 (bad buffer or no answer), -2 (busy)
 *   0x1031  rng_info()         (interrupt count << 8) | mode  (mode 1 = polling, 2 = interrupt driven)
 * Any program may call them (no capability). Unloading resets the device first, so it stops using the DMA memory the kernel then frees. */
#include "cosys.h"

#define VENDOR_VIRTIO 0x1AF4u
#define DEVICE_RNG_LEGACY 0x1005u
#define R_FEATURES 0x00
#define R_GUEST_FEATURES 0x04
#define R_QUEUE_PFN 0x08
#define R_QUEUE_SIZE 0x0C
#define R_QUEUE_SEL 0x0E
#define R_QUEUE_NOTIFY 0x10
#define R_STATUS 0x12
#define R_ISR 0x13
#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4

struct desc { uint64_t addr; uint32_t len; uint16_t flags, next; };

static uint16_t g_base;                                    /* I/O port base of the device */
static uint16_t g_qsize;
static volatile struct desc *g_desc;
static volatile uint16_t *g_avail;                         /* flags, idx, ring[] */
static volatile uint16_t *g_used;                          /* flags, idx, then {id, len} pairs */
static volatile uint8_t *g_buf; static uint64_t g_buf_phys;
static uint16_t g_last_used, g_avail_idx;
static volatile uint32_t g_irqs, g_busy;
static int g_mode, g_line = -1;

static void on_irq(void *ctx) { (void)ctx; (void)cosys_inb((uint16_t)(g_base + R_ISR)); g_irqs++; }    /* reading the ISR status acknowledges the device */

static int find_device(uint8_t *bus, uint8_t *dev, uint8_t *fn) {
    for (uint32_t b = 0; b < 8; b++)
        for (uint32_t d = 0; d < 32; d++)
            for (uint32_t f = 0; f < 8; f++) {
                uint32_t id = cosys_pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, 0);
                if ((id & 0xFFFFu) == VENDOR_VIRTIO && (id >> 16) == DEVICE_RNG_LEGACY) { *bus = (uint8_t)b; *dev = (uint8_t)d; *fn = (uint8_t)f; return 0; }
            }
    return -1;
}

static int64_t rng_read(uint64_t user_buf, uint64_t n, uint64_t c) {
    (void)c;
    if (!g_base) return -1;
    if (n == 0 || n > 4096) return -1;
    if (__sync_lock_test_and_set(&g_busy, 1)) return -2;
    uint64_t done = 0; int64_t rc = 0;
    while (done < n) {
        uint32_t want = (uint32_t)(n - done > 256 ? 256 : n - done);
        g_desc[0].addr = g_buf_phys; g_desc[0].len = want; g_desc[0].flags = 2 /* device writes */; g_desc[0].next = 0;
        g_avail[2 + (g_avail_idx % g_qsize)] = 0;
        __asm__ volatile("mfence" ::: "memory");
        g_avail_idx++; g_avail[1] = g_avail_idx;
        __asm__ volatile("mfence" ::: "memory");
        cosys_outw((uint16_t)(g_base + R_QUEUE_NOTIFY), 0);
        uint64_t t0 = cosys_time_ms();
        while (g_used[1] == g_last_used) {
            if (cosys_time_ms() - t0 > 1000) { rc = -1; break; }
            cosys_yield();
        }
        if (rc) break;
        uint32_t got = ((volatile uint32_t *)(g_used + 2))[(g_last_used % g_qsize) * 2 + 1];
        g_last_used++;
        if (got > want) got = want;
        if (got == 0) { rc = -1; break; }
        if (cosys_copy_to_user(user_buf + done, (const void *)g_buf, got) != 0) { rc = -1; break; }
        done += got;
    }
    __sync_lock_release(&g_busy);
    return rc ? rc : (int64_t)done;
}
static int64_t rng_info(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; return (int64_t)(((uint64_t)g_irqs << 8) | (uint64_t)g_mode); }

static int rng_init(void) {
    uint8_t bus, dev, fn;
    if (find_device(&bus, &dev, &fn) != 0) { cosys_log("no virtio-rng device (legacy interface)"); return -1; }
    uint32_t bar0 = cosys_pci_read32(bus, dev, fn, 0x10);
    if (!(bar0 & 1u)) { cosys_log("BAR0 is not an I/O range"); return -1; }
    g_base = (uint16_t)(bar0 & ~3u);
    if (cosys_pci_enable(bus, dev, fn, COSYS_PCI_IO | COSYS_PCI_BUSMASTER) != 0) return -1;
    cosys_outb((uint16_t)(g_base + R_STATUS), 0);                                          /* reset */
    cosys_outb((uint16_t)(g_base + R_STATUS), ST_ACK);
    cosys_outb((uint16_t)(g_base + R_STATUS), ST_ACK | ST_DRIVER);
    cosys_outl((uint16_t)(g_base + R_GUEST_FEATURES), 0);                                  /* no optional features */
    cosys_outw((uint16_t)(g_base + R_QUEUE_SEL), 0);
    g_qsize = cosys_inw((uint16_t)(g_base + R_QUEUE_SIZE));
    if (g_qsize == 0 || g_qsize > 256) { cosys_log("unusable queue size"); goto fail; }
    {   /* legacy virtqueue layout: descriptors, available ring, then the used ring on the next page boundary */
        uint32_t avail_off = 16u * g_qsize, used_off = (avail_off + 6u + 2u * g_qsize + 4095u) & ~4095u, total = used_off + 6u + 8u * g_qsize;
        uint64_t phys;
        uint8_t *ring = cosys_dma_alloc(total, &phys);
        if (!ring) { cosys_log("no DMA memory for the queue"); goto fail; }
        g_desc = (volatile struct desc *)ring; g_avail = (volatile uint16_t *)(ring + avail_off); g_used = (volatile uint16_t *)(ring + used_off);
        g_buf = cosys_dma_alloc(4096, &g_buf_phys);
        if (!g_buf) { cosys_log("no DMA memory for the buffer"); goto fail; }
        cosys_outl((uint16_t)(g_base + R_QUEUE_PFN), (uint32_t)(phys >> 12));
    }
    g_line = (int)(cosys_pci_read32(bus, dev, fn, 0x3C) & 0xFFu);
    g_mode = 1;
    if (g_line > 0 && g_line < 16 && cosys_irq_register(g_line, on_irq, 0) == 0) g_mode = 2;
    cosys_outb((uint16_t)(g_base + R_STATUS), ST_ACK | ST_DRIVER | ST_DRIVER_OK);
    cosys_log_hex("virtio-rng at I/O port ", g_base);
    cosys_log_dec("PCI interrupt line ", (uint64_t)g_line);
    cosys_log(g_mode == 2 ? "interrupt driven" : "polling (that line is not free)");
    if (cosys_register_syscall(0x1030, rng_read, 0) != 0 || cosys_register_syscall(0x1031, rng_info, 0) != 0) goto fail;
    return 0;
fail:
    /* A failed init is not followed by exit(): the device must stop using the queue before the kernel frees the DMA memory, so reset it here. */
    if (g_base) { cosys_outb((uint16_t)(g_base + R_STATUS), 0); cosys_outl((uint16_t)(g_base + R_QUEUE_PFN), 0); }
    if (g_mode == 2) cosys_irq_unregister(g_line, on_irq);
    g_base = 0; g_mode = 0;
    return -1;
}
static void rng_exit(void) {
    cosys_unregister_syscall(0x1030); cosys_unregister_syscall(0x1031);
    if (g_base) { cosys_outb((uint16_t)(g_base + R_STATUS), 0); cosys_outl((uint16_t)(g_base + R_QUEUE_PFN), 0); }       /* the device stops using the queue before it is freed */
    if (g_mode == 2) cosys_irq_unregister(g_line, on_irq);
    g_base = 0; g_mode = 0;
}
COSYS_MODULE("virtiorng", "1.0", COSYS_LEVEL_DEVICE, rng_init, rng_exit);
