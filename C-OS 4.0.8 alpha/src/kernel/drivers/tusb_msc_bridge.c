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
#include "class/msc/msc_host.h"

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
