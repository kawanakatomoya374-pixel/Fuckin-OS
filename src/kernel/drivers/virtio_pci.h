/**
 * virtio_pci.h - Minimal "modern" (virtio 1.0+) VirtIO-over-PCI transport.
 *
 * Scope, stated plainly (same discipline as Server/fileserver's README):
 * this is a small, generic transport layer that any virtio-pci device
 * driver can sit on top of - it does PCI capability discovery, feature
 * negotiation, and split-virtqueue setup/notify, and nothing else. It
 * does NOT implement legacy (pre-1.0) virtio-pci, does not use MSI-X
 * (every queue is left at VIRTIO_MSI_NO_VECTOR, so devices fall back to
 * legacy INTx signalling, which this driver simply never unmasks - see
 * virtio_pci_isr_ack()), and only supports a single in-flight
 * request/response pair per queue (see virtio_pci_queue_send_and_wait
 * in the .c file) rather than a full free-descriptor allocator. That is
 * a deliberate limit: the only consumer today (virtio_gpu.c) issues one
 * 2D control command at a time and waits for its reply, so a fuller
 * multi-request virtqueue implementation would be unused complexity
 * without also being tested by anything.
 *
 * Reference: Virtio 1.0/1.1 spec, section 4.1 (Virtio Over PCI Bus).
 */
#ifndef COS_VIRTIO_PCI_H
#define COS_VIRTIO_PCI_H

#include "types.h"
#include "pci.h"

#define VIRTIO_PCI_VENDOR_ID 0x1AF4

/* PCI capability cfg_type values (virtio spec 4.1.4). */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/* Device status bits (virtio spec 2.1). */
#define VIRTIO_STATUS_ACKNOWLEDGE        1
#define VIRTIO_STATUS_DRIVER              2
#define VIRTIO_STATUS_DRIVER_OK           4
#define VIRTIO_STATUS_FEATURES_OK         8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED             128

/* VIRTIO_F_VERSION_1 is feature bit 32 (i.e. bit 0 of the high dword). We
 * only ever negotiate this single feature - no device-specific bits. */
#define VIRTIO_F_VERSION_1_BIT 32

#define VIRTIO_MSI_NO_VECTOR 0xFFFF

/* One split virtqueue. Fixed-size (VIRTQ_MAX_SIZE), synchronous,
 * single-request-in-flight - see the file header comment above for why. */
#define VIRTQ_MAX_SIZE 8

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_MAX_SIZE];
} __attribute__((packed)) virtq_avail_t;

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTQ_MAX_SIZE];
} __attribute__((packed)) virtq_used_t;

typedef struct {
    uint16_t queue_index;   /* which device queue this is (0 = controlq) */
    uint16_t size;          /* negotiated size, <= VIRTQ_MAX_SIZE */
    virtq_desc_t*  desc;    /* kmalloc'd, physically contiguous */
    virtq_avail_t* avail;
    volatile virtq_used_t* used;
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;
    volatile uint8_t* notify_addr; /* precomputed notify MMIO address */
    uint16_t last_used_idx;
    /* Two fixed DMA bounce buffers reused by every request on this
     * queue (see the "single in-flight request" note above). */
    uint8_t* req_buf;
    uint64_t req_buf_phys;
    uint32_t req_buf_size;
    uint8_t* resp_buf;
    uint64_t resp_buf_phys;
    uint32_t resp_buf_size;
} virtio_queue_t;

typedef struct {
    pci_dev_t* pci;
    volatile uint8_t* common_cfg;  /* VIRTIO_PCI_CAP_COMMON_CFG, mapped */
    volatile uint8_t* notify_base; /* VIRTIO_PCI_CAP_NOTIFY_CFG, mapped */
    uint32_t notify_off_multiplier;
    volatile uint8_t* isr_cfg;     /* VIRTIO_PCI_CAP_ISR_CFG, mapped (1 byte) */
    bool valid;
} virtio_pci_dev_t;

/* Find the Nth (instance 0 = first) PCI device with vendor 0x1AF4 and
 * the given modern device id (e.g. 0x1050 for virtio-gpu), map its
 * common/notify/isr capabilities, and reset it to a clean state.
 * Returns false (and leaves *out zeroed) if no such device exists or a
 * required capability is missing - this is an ordinary "not present"
 * outcome, not a fault. */
bool virtio_pci_open(uint16_t device_id, virtio_pci_dev_t* out);

/* Device status byte (spec 2.1 / 4.1.4.3). */
void virtio_pci_reset(virtio_pci_dev_t* v);
uint8_t virtio_pci_get_status(virtio_pci_dev_t* v);
void virtio_pci_set_status(virtio_pci_dev_t* v, uint8_t status);
void virtio_pci_add_status(virtio_pci_dev_t* v, uint8_t bits);

/* Negotiate exactly VIRTIO_F_VERSION_1 and nothing else. Returns false
 * if the device does not offer VIRTIO_F_VERSION_1 or rejects the
 * negotiated set (FEATURES_OK did not stick). */
bool virtio_pci_negotiate_basic_features(virtio_pci_dev_t* v);
bool virtio_pci_negotiate_features(virtio_pci_dev_t* v, uint32_t want_lo, uint32_t* got_lo);

/* Select queue_index, read its device-side max size, allocate and wire
 * up desc/avail/used plus the two DMA bounce buffers (req_buf_size /
 * resp_buf_size bytes each), and enable the queue. */
bool virtio_pci_queue_setup(virtio_pci_dev_t* v, uint16_t queue_index,
                             uint32_t req_buf_size, uint32_t resp_buf_size,
                             virtio_queue_t* q);

/* Copy req (req_len bytes) into q's request bounce buffer, submit a
 * two-descriptor chain (device-readable request, device-writable
 * response), kick the queue, and poll the used ring until the device
 * replies or max_spin iterations elapse. On success, up to resp_len
 * bytes of the device's reply are copied into resp and true is
 * returned. This is synchronous and single-request - callers must not
 * call it again on the same queue until it returns. */
bool virtio_pci_queue_send_and_wait(virtio_pci_dev_t* v, virtio_queue_t* q,
                                     const void* req, uint32_t req_len,
                                     void* resp, uint32_t resp_len,
                                     uint32_t max_spin);

/* Read (and thereby acknowledge/clear) the legacy ISR status byte, so a
 * pending INTx line the device raised on this driver's behalf does not
 * stay asserted forever - see the header comment about not using
 * MSI-X. Safe to call even if nothing is pending. */
void virtio_pci_isr_ack(virtio_pci_dev_t* v);

#endif /* COS_VIRTIO_PCI_H */
