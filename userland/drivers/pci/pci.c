/* pci.c - a kernel extension at DEVICE level: the PCI inventory, read straight from configuration space with cosys_pci_read32.
 *   0x1020  pci_count()               number of PCI functions present (buses 0..7)
 *   0x1021  pci_get(index, buf)       fills struct pci_info (12 bytes) for the index-th function; 0, or -1 (no such index / bad buffer)
 * Needs COS_CAP_DEVICES (the same capability as the device inventory of the kernel). */
#include "cosys.h"

struct pci_info { uint8_t bus, dev, fn, hdr; uint16_t vendor, device; uint32_t class_rev; };      /* mirrored in userland/include/cos.h (cos_pci_info_t) */

/* Calls f for each function present; stops when f returns non-zero. */
static int scan(int (*f)(const struct pci_info *, void *), void *ctx) {
    for (uint32_t bus = 0; bus < 8; bus++)
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id0 = cosys_pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0);
            if ((id0 & 0xFFFFu) == 0xFFFFu) continue;
            uint32_t hdr0 = (cosys_pci_read32((uint8_t)bus, (uint8_t)dev, 0, 0x0C) >> 16) & 0xFFu;
            uint32_t nfn = (hdr0 & 0x80u) ? 8 : 1;                   /* a multi-function device has the top bit of the header type set */
            for (uint32_t fn = 0; fn < nfn; fn++) {
                uint32_t id = cosys_pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0);
                if ((id & 0xFFFFu) == 0xFFFFu) continue;
                struct pci_info pi;
                pi.bus = (uint8_t)bus; pi.dev = (uint8_t)dev; pi.fn = (uint8_t)fn;
                pi.hdr = (uint8_t)((cosys_pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x0C) >> 16) & 0xFFu);
                pi.vendor = (uint16_t)(id & 0xFFFFu); pi.device = (uint16_t)(id >> 16);
                pi.class_rev = cosys_pci_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                int r = f(&pi, ctx);
                if (r) return r;
            }
        }
    return 0;
}
struct find { uint32_t want, seen; struct pci_info out; };
static int count_cb(const struct pci_info *p, void *ctx) { (void)p; (*(uint32_t *)ctx)++; return 0; }
static int find_cb(const struct pci_info *p, void *ctx) { struct find *f = ctx; if (f->seen++ == f->want) { f->out = *p; return 1; } return 0; }

static int64_t pci_count(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; uint32_t n = 0; scan(count_cb, &n); return n; }
static int64_t pci_get(uint64_t index, uint64_t user_buf, uint64_t c) {
    (void)c;
    struct find f = { (uint32_t)index, 0, { 0 } };
    if (index > 0xFFFF || scan(find_cb, &f) != 1) return -1;
    return cosys_copy_to_user(user_buf, &f.out, sizeof f.out) == 0 ? 0 : -1;
}
static int pci_init(void) {
    if (cosys_register_syscall(0x1020, pci_count, 0x80 /* COS_CAP_DEVICES */) != 0) return -1;
    if (cosys_register_syscall(0x1021, pci_get, 0x80) != 0) return -1;
    cosys_log("pci inventory ready");
    return 0;
}
static void pci_exit(void) { cosys_unregister_syscall(0x1020); cosys_unregister_syscall(0x1021); }
COSYS_MODULE("pci", "1.0", COSYS_LEVEL_DEVICE, pci_init, pci_exit);
