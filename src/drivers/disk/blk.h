/**
 * blk.h - block devices the persistent-storage layer can sit on (legacy IDE, AHCI/SATA, NVMe).
 *
 * All devices present 512-byte sectors; the storage layer reads and writes runs of at most BLK_RUN_MAX sectors per
 * call, from/to ordinary kernel memory (the drivers translate each page for DMA themselves).
 */
#ifndef COS_BLK_H
#define COS_BLK_H

#include "types.h"
#include <stdbool.h>

/* read/write accept any sector count (the driver splits it into controller-sized commands and keeps as many in flight as the
 * hardware allows); buffers are ordinary kernel memory, not necessarily physically contiguous. */
#define BLK_RUN_MAX 64u

typedef struct blk_dev blk_dev_t;
struct blk_dev {
    char name[32];                   /* e.g. "AHCI port 0", "NVMe ns 1" */
    char model[44];                  /* from IDENTIFY, may be empty */
    uint64_t sectors;                /* capacity in 512-byte sectors */
    void* ctx;
    bool (*read)(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf);
    bool (*write)(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf);
    bool (*flush)(blk_dev_t* d);
    bool (*trim)(blk_dev_t* d, uint64_t lba, uint64_t count);   /* NULL when the device cannot discard blocks */
};

/* Fill 'out' with the usable disks found on that kind of controller; return how many. */
int ide_enumerate(blk_dev_t* out, int max);
int ahci_enumerate(blk_dev_t* out, int max);
int nvme_enumerate(blk_dev_t* out, int max);
/* USB mass storage (a boot stick): the disks TinyUSB has enumerated, 512-byte sectors only. usb_msc_wait() polls the controller until the
 * devices that are going to show up have (at most max_ms), so call it only when a USB disk is what is being looked for. */
int  usb_msc_enumerate(blk_dev_t* out, int max);
bool usb_msc_wait(uint32_t max_ms);

#endif
