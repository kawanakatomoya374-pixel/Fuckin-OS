/**
 * tusb_msc_bridge.c - TinyUSB mass-storage host integration
 *
 * TinyUSB owns BOT/SCSI enumeration and transfer scheduling.  This bridge
 * records mount state for C-OS and exposes the validated device geometry to
 * the storage/UI layers without ever issuing a blocking transfer from a USB
 * callback or interrupt context.
 */
#include "types.h"
#include "string.h"
#include "serial.h"

#include "tusb.h"
#include "cos_devices.h"
#include "class/msc/msc_host.h"
#include "usb.h"
#include "blk.h"
#include "memory.h"
#include "timer.h"
#include "task.h"

#define COS_USB_MSC_MAX_DEVICES CFG_TUH_DEVICE_MAX

typedef struct {
    bool mounted;
    uint32_t blocks;
    uint32_t block_size;
} cos_usb_msc_device_t;

static cos_usb_msc_device_t s_msc[COS_USB_MSC_MAX_DEVICES + 1u];
static int s_msc_count = 0;

int tusb_msc_bridge_device_count(void)
{
    return s_msc_count;
}

bool tusb_msc_bridge_get_geometry(uint8_t dev_addr, uint32_t* blocks,
                                  uint32_t* block_size)
{
    if (dev_addr == 0 || dev_addr > COS_USB_MSC_MAX_DEVICES ||
        !s_msc[dev_addr].mounted) return false;
    if (blocks) *blocks = s_msc[dev_addr].blocks;
    if (block_size) *block_size = s_msc[dev_addr].block_size;
    return true;
}

void tuh_msc_mount_cb(uint8_t dev_addr)
{
    if (dev_addr == 0 || dev_addr > COS_USB_MSC_MAX_DEVICES) return;

    cos_usb_msc_device_t* dev = &s_msc[dev_addr];
    if (!dev->mounted) ++s_msc_count;
    dev->mounted = true;
    /* TinyUSB updates capacity as enumeration finishes.  Querying through
     * its public API is nonblocking and is safe in this completion callback. */
    dev->blocks = tuh_msc_get_block_count(dev_addr, 0);
    dev->block_size = tuh_msc_get_block_size(dev_addr, 0);

    cos_devices_usb_storage(dev_addr, dev->blocks, dev->block_size);
    serial_puts("[USB] Mass-storage connected: blocks=");
    serial_putdec(dev->blocks);
    serial_puts(" block-size=");
    serial_putdec(dev->block_size);
    serial_puts("\n");
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
    if (dev_addr == 0 || dev_addr > COS_USB_MSC_MAX_DEVICES) return;
    if (s_msc[dev_addr].mounted && s_msc_count > 0) --s_msc_count;
    memset(&s_msc[dev_addr], 0, sizeof(s_msc[dev_addr]));
    serial_puts("[USB] Mass-storage disconnected.\n");
}

/* ---- block device: the stick as a disk for the persistent-storage layer ---------------------------------------------------------- */

static uint32_t now_ms(void) { return (uint32_t)((get_timer_ticks() * 1000ULL) / TIMER_TICKS_PER_SEC); }

static volatile int s_op_done, s_op_ok;
static bool op_done_cb(uint8_t dev_addr, const tuh_msc_complete_data_t* d)
{
    (void)dev_addr;
    s_op_ok = d && d->csw && d->csw->status == MSC_CSW_STATUS_PASSED;
    s_op_done = 1;
    return true;
}

#define MSC_CHUNK 64u                          /* sectors per SCSI command = BLK_RUN_MAX */
static uint8_t* s_bounce;                      /* page aligned, identity-mapped kernel memory the controller can DMA to and from */
static volatile int s_io_busy;

static bool msc_xfer(blk_dev_t* d, bool write, uint64_t lba, uint32_t count, void* buf)
{
    uint8_t addr = (uint8_t)(uintptr_t)d->ctx;
    if (lba + count > d->sectors || lba > 0xFFFFFFFFull) return false;
    while (__sync_lock_test_and_set(&s_io_busy, 1)) thread_yield();
    if (!s_bounce) s_bounce = (uint8_t*)kmalloc_aligned(MSC_CHUNK * 512u, 4096);
    bool ok = s_bounce != NULL;
    uint8_t* p = (uint8_t*)buf;
    while (ok && count) {
        uint32_t n = count < MSC_CHUNK ? count : MSC_CHUNK;
        if (write) memcpy(s_bounce, p, (size_t)n * 512u);
        s_op_done = 0; s_op_ok = 0;
        bool queued = false;
        uint32_t t0 = now_ms();
        for (;;) {                                        /* the controller may be busy with the GUI loop's poll: try again */
            if (usb_try_lock()) {
                queued = write ? tuh_msc_write10(addr, 0, s_bounce, (uint32_t)lba, (uint16_t)n, op_done_cb, 0)
                               : tuh_msc_read10(addr, 0, s_bounce, (uint32_t)lba, (uint16_t)n, op_done_cb, 0);
                usb_unlock();
                if (queued) break;
            }
            if (now_ms() - t0 > 2000 || !s_msc[addr].mounted) break;
            usb_service(); thread_yield();
        }
        if (!queued) { ok = false; break; }
        while (!s_op_done) {
            usb_service();
            if (now_ms() - t0 > 8000 || !s_msc[addr].mounted) break;
            thread_yield();
        }
        if (!s_op_done || !s_op_ok) { ok = false; break; }
        if (!write) memcpy(p, s_bounce, (size_t)n * 512u);
        p += (size_t)n * 512u; lba += n; count -= n;
    }
    __sync_lock_release(&s_io_busy);
    return ok;
}
static bool msc_read(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf) { return msc_xfer(d, false, lba, count, buf); }
static bool msc_write(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf) { return msc_xfer(d, true, lba, count, (void*)buf); }

int usb_msc_enumerate(blk_dev_t* out, int max)
{
    int n = 0;
    for (uint8_t a = 1; a <= COS_USB_MSC_MAX_DEVICES && n < max; ++a) {
        if (!s_msc[a].mounted || s_msc[a].block_size != 512u || s_msc[a].blocks == 0) continue;
        blk_dev_t* d = &out[n++];
        memset(d, 0, sizeof *d);
        strncpy(d->name, "USB mass storage", sizeof d->name - 1);
        d->sectors = s_msc[a].blocks;
        d->ctx = (void*)(uintptr_t)a;
        d->read = msc_read;
        d->write = msc_write;
    }
    return n;
}

bool usb_msc_wait(uint32_t max_ms)
{
    if (!usb_is_initialized()) return false;
    uint32_t t0 = now_ms(), quiet_since = t0;
    int seen = usb_mounted_count();
    for (;;) {
        usb_service();
        uint32_t t = now_ms();
        int m = usb_mounted_count();
        if (m != seen) { seen = m; quiet_since = t; }
        if (s_msc_count > 0 && t - quiet_since > 300) return true;            /* the stick is up (give a hub's other devices a moment) */
        if (t - t0 > max_ms) return s_msc_count > 0;
        if (t - t0 > 1500 && t - quiet_since > 700) return s_msc_count > 0;   /* nothing new is arriving and no stick came */
        thread_yield();
    }
}
