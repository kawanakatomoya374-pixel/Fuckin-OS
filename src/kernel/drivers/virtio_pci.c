/**
 * virtio_pci.c - Minimal "modern" (virtio 1.0+) VirtIO-over-PCI transport.
 * See virtio_pci.h for the scope this deliberately does and does not cover.
 *
 * Reference: Virtio 1.0/1.1 spec section 4.1 (Virtio Over PCI Bus). Field
 * names/offsets below mirror struct virtio_pci_cap / virtio_pci_common_cfg
 * from the spec (and Linux's include/uapi/linux/virtio_pci.h) exactly.
 */
#include "virtio_pci.h"
#include "mm/paging.h"
#include "memory.h"
#include "serial.h"
#include <string.h>

/* PCI_CAP_ID_VNDR: the generic PCI "vendor-specific capability" id that
 * every virtio-pci capability structure is wrapped in. */
#define PCI_CAP_ID_VNDR 0x09
#define PCI_STATUS_CAP_LIST 0x10

/* virtio_pci_common_cfg field byte offsets (spec 4.1.4.3). */
#define VCFG_DEV_FEATURE_SEL    0x00
#define VCFG_DEV_FEATURE        0x04
#define VCFG_DRV_FEATURE_SEL    0x08
#define VCFG_DRV_FEATURE        0x0C
#define VCFG_MSIX_CONFIG        0x10
#define VCFG_NUM_QUEUES         0x12
#define VCFG_DEVICE_STATUS      0x14
#define VCFG_CONFIG_GEN         0x15
#define VCFG_QUEUE_SELECT       0x16
#define VCFG_QUEUE_SIZE         0x18
#define VCFG_QUEUE_MSIX_VECTOR  0x1A
#define VCFG_QUEUE_ENABLE       0x1C
#define VCFG_QUEUE_NOTIFY_OFF   0x1E
#define VCFG_QUEUE_DESC         0x20
#define VCFG_QUEUE_DRIVER       0x28
#define VCFG_QUEUE_DEVICE       0x30

static inline volatile uint8_t*  cc8(virtio_pci_dev_t* v, uint32_t off) {
    return (volatile uint8_t*)(v->common_cfg + off);
}
static inline volatile uint16_t* cc16(virtio_pci_dev_t* v, uint32_t off) {
    return (volatile uint16_t*)(v->common_cfg + off);
}
static inline volatile uint32_t* cc32(virtio_pci_dev_t* v, uint32_t off) {
    return (volatile uint32_t*)(v->common_cfg + off);
}
static inline volatile uint64_t* cc64(virtio_pci_dev_t* v, uint32_t off) {
    return (volatile uint64_t*)(v->common_cfg + off);
}

static uint8_t pci_cfg_read_u8(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset) {
    uint64_t dw = pci_read_dword(bus, slot, func, offset & ~3ULL);
    return (uint8_t)((dw >> ((offset & 3ULL) * 8ULL)) & 0xFFULL);
}

/* Map [bar_base + cap_offset, bar_base + cap_offset + cap_len) of the
 * given BAR into the kernel's direct-mapped virtual address range,
 * exactly like vga.c already does for the legacy framebuffer BAR (see
 * vga_reserve_physical_regions()) - same reasoning applies here: this
 * physical range is device MMIO, not ordinary RAM, so without an
 * explicit mapping the first access page-faults.
 *
 * already_mapped/already_mapped_count track every 4K-aligned page this
 * function has already mapped IN THIS SAME virtio_pci_open() call (see
 * its caller) and are checked before calling paging_map_range() again.
 * This matters because two capabilities can legitimately share one 4K
 * page at different byte offsets within it - confirmed happening for
 * real: under QEMU's `virtio-vga` (as opposed to standalone
 * `virtio-gpu-pci`), the common-cfg and isr-cfg capabilities sit at
 * offsets 0x1000 (length 0x800) and 0x1800 (length 0x800) of the same
 * BAR - i.e. packed into the two halves of the single page at
 * 0x1000-0x1fff, not each given their own page the way standalone
 * virtio-gpu-pci happens to. paging_map_range() rejects mapping a page
 * that is already present (a reasonable general safety check - see its
 * own comment) rather than treating an identical re-map as a no-op, so
 * calling it a second time for isr-cfg's page here would fail even
 * though nothing is actually wrong: the page it needs is already
 * mapped, just for a different capability's sake. */
#define VIRTIO_PCI_MAX_MAPPED_PAGES 8
typedef struct {
    uint64_t page_phys[VIRTIO_PCI_MAX_MAPPED_PAGES];
    int count;
} virtio_pci_mapped_pages_t;

static volatile uint8_t* virtio_pci_map_bar(pci_dev_t* dev, uint8_t bar_index,
                                             uint32_t cap_offset, uint32_t cap_len,
                                             virtio_pci_mapped_pages_t* mapped) {
    if (!dev || bar_index > 5) return NULL;

    uint64_t raw = pci_read_dword(dev->bus, dev->slot, dev->func, PCI_BAR0 + (uint64_t)bar_index * 4);
    if (raw & 0x1ULL) {
        serial_puts("[virtio-pci] BAR is I/O space, not supported\n");
        return NULL;
    }
    uint8_t mem_type = (uint8_t)((raw >> 1) & 0x3ULL);
    uint64_t base = raw & ~0xFULL;
    if (mem_type == 2) {
        uint64_t high = pci_read_dword(dev->bus, dev->slot, dev->func,
                                        PCI_BAR0 + (uint64_t)(bar_index + 1) * 4);
        base |= (high << 32);
    }
    if (base == 0) return NULL;

    uint64_t phys = base + (uint64_t)cap_offset;
    uint64_t virt = (uint64_t)PHYS_TO_VIRT(phys);

    uint64_t page_phys = phys & ~4095ULL;
    uint64_t page_virt = virt & ~4095ULL;
    uint64_t within_page = phys - page_phys;
    uint64_t total = within_page + (cap_len ? cap_len : 4);
    total = (total + 4095ULL) & ~4095ULL;

    /* Map every page in [page_phys, page_phys+total) that is not
     * already recorded as mapped from an earlier capability in this
     * same virtio_pci_open() call - see this function's header comment
     * for why a naive unconditional paging_map_range() over the whole
     * range would fail as soon as two capabilities share a page. */
    extern bool paging_map_range(uint64_t vs, uint64_t ps_arg, uint64_t size, uint64_t flags);
    for (uint64_t off = 0; off < total; off += 4096ULL) {
        uint64_t this_page = page_phys + off;
        bool already = false;
        for (int i = 0; mapped && i < mapped->count; ++i) {
            if (mapped->page_phys[i] == this_page) { already = true; break; }
        }
        if (already) continue;
        if (!paging_map_range(page_virt + off, this_page, 4096, 0x3 /* present|writable */)) {
            serial_puts("[virtio-pci] failed to map BAR region into page tables\n");
            return NULL;
        }
        if (mapped && mapped->count < VIRTIO_PCI_MAX_MAPPED_PAGES) {
            mapped->page_phys[mapped->count++] = this_page;
        }
    }
    return (volatile uint8_t*)(uintptr_t)virt;
}

bool virtio_pci_open(uint16_t device_id, virtio_pci_dev_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    pci_dev_t* dev = pci_get_device(VIRTIO_PCI_VENDOR_ID, device_id);
    if (!dev) return false;

    uint64_t pci_status = pci_read_word(dev->bus, dev->slot, dev->func, PCI_STATUS);
    if (!(pci_status & PCI_STATUS_CAP_LIST)) {
        serial_puts("[virtio-pci] device has no PCI capability list\n");
        return false;
    }

    uint8_t ptr = (uint8_t)(pci_read_word(dev->bus, dev->slot, dev->func, PCI_CAP_PTR) & 0xFF);

    bool have_common = false, have_notify = false, have_isr = false;
    uint8_t  common_bar = 0, notify_bar = 0, isr_bar = 0;
    uint32_t common_off = 0, common_len = 0;
    uint32_t notify_off = 0, notify_len = 0, notify_mult = 0;
    uint32_t isr_off = 0, isr_len = 0;

    int guard = 0;
    while (ptr != 0 && guard++ < 64) {
        uint8_t cap_vndr = pci_cfg_read_u8(dev->bus, dev->slot, dev->func, ptr + 0);
        uint8_t cap_next = pci_cfg_read_u8(dev->bus, dev->slot, dev->func, ptr + 1);
        if (cap_vndr == PCI_CAP_ID_VNDR) {
            uint8_t cfg_type = pci_cfg_read_u8(dev->bus, dev->slot, dev->func, ptr + 3);
            uint8_t bar      = pci_cfg_read_u8(dev->bus, dev->slot, dev->func, ptr + 4);
            uint32_t off = (uint32_t)pci_read_dword(dev->bus, dev->slot, dev->func, ptr + 8);
            uint32_t len = (uint32_t)pci_read_dword(dev->bus, dev->slot, dev->func, ptr + 12);
            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG && !have_common) {
                have_common = true; common_bar = bar; common_off = off; common_len = len;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && !have_notify) {
                have_notify = true; notify_bar = bar; notify_off = off; notify_len = len;
                notify_mult = (uint32_t)pci_read_dword(dev->bus, dev->slot, dev->func, ptr + 16);
            } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG && !have_isr) {
                have_isr = true; isr_bar = bar; isr_off = off; isr_len = len;
            }
        }
        ptr = cap_next;
    }

    if (!have_common || !have_notify || !have_isr) {
        serial_puts("[virtio-pci] missing a required capability (common/notify/isr)\n");
        return false;
    }

    pci_enable_mmio(dev);

    virtio_pci_mapped_pages_t mapped_pages;
    memset(&mapped_pages, 0, sizeof(mapped_pages));
    volatile uint8_t* common_va = virtio_pci_map_bar(dev, common_bar, common_off, common_len, &mapped_pages);
    volatile uint8_t* notify_va = virtio_pci_map_bar(dev, notify_bar, notify_off, notify_len, &mapped_pages);
    volatile uint8_t* isr_va    = virtio_pci_map_bar(dev, isr_bar, isr_off, isr_len ? isr_len : 1, &mapped_pages);
    if (!common_va || !notify_va || !isr_va) {
        return false;
    }

    out->pci = dev;
    out->common_cfg = common_va;
    out->notify_base = notify_va;
    out->notify_off_multiplier = notify_mult;
    out->isr_cfg = isr_va;
    out->valid = true;

    virtio_pci_reset(out);
    virtio_pci_add_status(out, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_pci_add_status(out, VIRTIO_STATUS_DRIVER);
    return true;
}

void virtio_pci_reset(virtio_pci_dev_t* v) {
    if (!v || !v->valid) return;
    virtio_pci_set_status(v, 0);
    uint32_t guard = 0;
    while (virtio_pci_get_status(v) != 0 && guard++ < 1000000u) {
        __asm__ __volatile__("pause");
    }
}

uint8_t virtio_pci_get_status(virtio_pci_dev_t* v) {
    if (!v || !v->valid) return 0;
    return *cc8(v, VCFG_DEVICE_STATUS);
}

void virtio_pci_set_status(virtio_pci_dev_t* v, uint8_t status) {
    if (!v || !v->valid) return;
    *cc8(v, VCFG_DEVICE_STATUS) = status;
}

void virtio_pci_add_status(virtio_pci_dev_t* v, uint8_t bits) {
    virtio_pci_set_status(v, (uint8_t)(virtio_pci_get_status(v) | bits));
}

bool virtio_pci_negotiate_basic_features(virtio_pci_dev_t* v) {
    if (!v || !v->valid) return false;

    *cc32(v, VCFG_DEV_FEATURE_SEL) = 1; /* select the high dword (bits 32-63) */
    uint32_t feat_hi = *cc32(v, VCFG_DEV_FEATURE);
    serial_puts("[virtio-pci] device_feature[hi]=0x");
    serial_puthex(feat_hi);
    serial_puts("\n");
    if (!(feat_hi & 0x1u)) {
        serial_puts("[virtio-pci] device does not offer VIRTIO_F_VERSION_1\n");
        return false;
    }

    *cc32(v, VCFG_DRV_FEATURE_SEL) = 0;
    *cc32(v, VCFG_DRV_FEATURE) = 0;
    *cc32(v, VCFG_DRV_FEATURE_SEL) = 1;
    *cc32(v, VCFG_DRV_FEATURE) = 0x1u; /* VIRTIO_F_VERSION_1 only */

    virtio_pci_add_status(v, VIRTIO_STATUS_FEATURES_OK);
    uint8_t st_after = virtio_pci_get_status(v);
    serial_puts("[virtio-pci] status after FEATURES_OK=0x");
    serial_puthex(st_after);
    serial_puts("\n");
    if (!(st_after & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-pci] device rejected FEATURES_OK\n");
        return false;
    }
    return true;
}

/* Like virtio_pci_negotiate_basic_features(), but additionally accepts
 * whichever of the device-specific low-dword feature bits in `want_lo`
 * the device actually offers (e.g. VIRTIO_GPU_F_VIRGL, bit 0). Only the
 * intersection is ever written back, so asking for a bit the device
 * lacks is harmless - *got_lo reports what was really agreed. */
bool virtio_pci_negotiate_features(virtio_pci_dev_t* v, uint32_t want_lo, uint32_t* got_lo) {
    if (got_lo) *got_lo = 0;
    if (!v || !v->valid) return false;

    *cc32(v, VCFG_DEV_FEATURE_SEL) = 0;
    uint32_t feat_lo = *cc32(v, VCFG_DEV_FEATURE);
    *cc32(v, VCFG_DEV_FEATURE_SEL) = 1;
    uint32_t feat_hi = *cc32(v, VCFG_DEV_FEATURE);
    serial_puts("[virtio-pci] device_feature lo=0x");
    serial_puthex(feat_lo);
    serial_puts(" hi=0x");
    serial_puthex(feat_hi);
    serial_puts("\n");
    if (!(feat_hi & 0x1u)) {
        serial_puts("[virtio-pci] device does not offer VIRTIO_F_VERSION_1\n");
        return false;
    }

    uint32_t accept_lo = feat_lo & want_lo;
    *cc32(v, VCFG_DRV_FEATURE_SEL) = 0;
    *cc32(v, VCFG_DRV_FEATURE) = accept_lo;
    *cc32(v, VCFG_DRV_FEATURE_SEL) = 1;
    *cc32(v, VCFG_DRV_FEATURE) = 0x1u; /* VIRTIO_F_VERSION_1 */

    virtio_pci_add_status(v, VIRTIO_STATUS_FEATURES_OK);
    if (!(virtio_pci_get_status(v) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("[virtio-pci] device rejected FEATURES_OK\n");
        return false;
    }
    if (got_lo) *got_lo = accept_lo;
    return true;
}

bool virtio_pci_queue_setup(virtio_pci_dev_t* v, uint16_t queue_index,
                             uint32_t req_buf_size, uint32_t resp_buf_size,
                             virtio_queue_t* q) {
    if (!v || !v->valid || !q) return false;
    memset(q, 0, sizeof(*q));

    *cc16(v, VCFG_QUEUE_SELECT) = queue_index;
    uint16_t max_size = *cc16(v, VCFG_QUEUE_SIZE);
    if (max_size == 0) {
        serial_puts("[virtio-pci] requested queue is not available on this device\n");
        return false;
    }

    uint16_t size = (max_size < VIRTQ_MAX_SIZE) ? max_size : VIRTQ_MAX_SIZE;
    uint16_t p = 1;
    while ((uint16_t)(p * 2) <= size && p < 0x8000) p = (uint16_t)(p * 2);
    size = p;
    if (size < 2) {
        serial_puts("[virtio-pci] device queue size too small\n");
        return false;
    }

    q->queue_index = queue_index;
    q->size = size;

    size_t desc_bytes  = sizeof(virtq_desc_t) * (size_t)size;
    size_t avail_bytes = sizeof(uint16_t) * 2 + sizeof(uint16_t) * (size_t)size;
    size_t used_bytes  = sizeof(uint16_t) * 2 + sizeof(virtq_used_elem_t) * (size_t)size;

    q->desc  = (virtq_desc_t*)kmalloc_aligned(desc_bytes, 16);
    q->avail = (virtq_avail_t*)kmalloc_aligned(avail_bytes, 2);
    q->used  = (volatile virtq_used_t*)kmalloc_aligned(used_bytes, 4);
    q->req_buf  = (uint8_t*)kmalloc_aligned(req_buf_size ? req_buf_size : 16, 16);
    q->resp_buf = (uint8_t*)kmalloc_aligned(resp_buf_size ? resp_buf_size : 16, 16);
    if (!q->desc || !q->avail || !q->used || !q->req_buf || !q->resp_buf) {
        serial_puts("[virtio-pci] queue allocation failed\n");
        return false;
    }
    memset(q->desc, 0, desc_bytes);
    memset((void*)q->avail, 0, avail_bytes);
    memset((void*)q->used, 0, used_bytes);
    q->req_buf_size = req_buf_size;
    q->resp_buf_size = resp_buf_size;

    /* This kernel identity-maps its own image and kmalloc heap at low
     * addresses (virt == phys there) rather than exposing them through
     * the separate high-half PHYS_TO_VIRT/VIRT_TO_PHYS alias used for
     * arbitrary/MMIO physical ranges elsewhere in this driver (see
     * virtio_pci_map_bar() above) - paging_virt_to_phys() is the
     * general, correct way to resolve a kernel pointer to the real
     * guest-physical address a DMA-capable device needs, exactly as
     * cos_elf.c/task.c/cos_futex.c already do for other kernel
     * pointers. Using VIRT_TO_PHYS() here instead would silently wrap
     * around to a bogus, out-of-RAM "physical" address for these
     * low/identity-mapped heap pointers - which is exactly the bug
     * that made the very first RESOURCE_CREATE_2D command time out
     * during bring-up (the device read the avail ring from a physical
     * address far outside the machine's actual RAM window and never
     * saw anything there).
     */
    q->desc_phys      = paging_virt_to_phys((uint64_t)(uintptr_t)q->desc);
    q->avail_phys     = paging_virt_to_phys((uint64_t)(uintptr_t)q->avail);
    q->used_phys      = paging_virt_to_phys((uint64_t)(uintptr_t)q->used);
    q->req_buf_phys   = paging_virt_to_phys((uint64_t)(uintptr_t)q->req_buf);
    q->resp_buf_phys  = paging_virt_to_phys((uint64_t)(uintptr_t)q->resp_buf);

    *cc16(v, VCFG_QUEUE_SELECT) = queue_index;
    *cc16(v, VCFG_QUEUE_SIZE)   = size;
    *cc64(v, VCFG_QUEUE_DESC)   = q->desc_phys;
    *cc64(v, VCFG_QUEUE_DRIVER) = q->avail_phys;
    *cc64(v, VCFG_QUEUE_DEVICE) = q->used_phys;
    *cc16(v, VCFG_QUEUE_MSIX_VECTOR) = VIRTIO_MSI_NO_VECTOR;

    uint16_t notify_off = *cc16(v, VCFG_QUEUE_NOTIFY_OFF);
    q->notify_addr = v->notify_base + (uint64_t)notify_off * (uint64_t)v->notify_off_multiplier;

    *cc16(v, VCFG_QUEUE_ENABLE) = 1;

    q->avail->flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    q->last_used_idx = q->used->idx;

    serial_puts("[virtio-pci] queue setup: index=");
    serial_putdec(queue_index);
    serial_puts(" size=");
    serial_putdec(size);
    serial_puts(" notify_off=");
    serial_putdec(notify_off);
    serial_puts(" notify_mult=");
    serial_putdec(v->notify_off_multiplier);
    serial_puts(" desc_phys=0x");
    serial_puthex(q->desc_phys);
    serial_puts(" avail_phys=0x");
    serial_puthex(q->avail_phys);
    serial_puts(" used_phys=0x");
    serial_puthex(q->used_phys);
    serial_puts(" queue_enable_readback=");
    serial_putdec(*cc16(v, VCFG_QUEUE_ENABLE));
    serial_puts("\n");
    return true;
}

bool virtio_pci_queue_send_and_wait(virtio_pci_dev_t* v, virtio_queue_t* q,
                                     const void* req, uint32_t req_len,
                                     void* resp, uint32_t resp_len,
                                     uint32_t max_spin) {
    if (!v || !v->valid || !q || !q->desc || !req) return false;
    if (req_len > q->req_buf_size || resp_len > q->resp_buf_size) {
        serial_puts("[virtio-pci] request/response larger than bounce buffer\n");
        return false;
    }

    memcpy(q->req_buf, req, req_len);
    if (resp_len) memset(q->resp_buf, 0, resp_len);

    q->desc[0].addr  = q->req_buf_phys;
    q->desc[0].len   = req_len;
    q->desc[0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[0].next  = 1;
    q->desc[1].addr  = q->resp_buf_phys;
    q->desc[1].len   = resp_len;
    q->desc[1].flags = VIRTQ_DESC_F_WRITE;
    q->desc[1].next  = 0;

    __sync_synchronize();

    uint16_t slot = (uint16_t)(q->avail->idx % q->size);
    q->avail->ring[slot] = 0; /* head descriptor index */
    __sync_synchronize();
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    __sync_synchronize();

    *(volatile uint16_t*)q->notify_addr = q->queue_index;

    uint32_t spins = 0;
    while (q->used->idx == q->last_used_idx) {
        if (++spins > max_spin) {
            serial_puts("[virtio-pci] timed out waiting for device response\n");
            return false;
        }
        __asm__ __volatile__("pause");
    }

    uint16_t used_slot = (uint16_t)(q->last_used_idx % q->size);
    uint32_t used_len = q->used->ring[used_slot].len;
    q->last_used_idx = (uint16_t)(q->last_used_idx + 1);

    uint32_t copy_len = (used_len < resp_len) ? used_len : resp_len;
    if (resp && copy_len) memcpy(resp, q->resp_buf, copy_len);

    virtio_pci_isr_ack(v);
    return true;
}

void virtio_pci_isr_ack(virtio_pci_dev_t* v) {
    if (!v || !v->valid || !v->isr_cfg) return;
    (void)*v->isr_cfg; /* reading clears the legacy INTx cause */
}
