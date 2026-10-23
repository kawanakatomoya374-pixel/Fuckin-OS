/* cos_devices.h - inventory of the input / USB devices the kernel has detected (PS/2 and USB), for the Settings app.
 * The struct layout is shared with userland/include/cos.h (cos_device_info_t) - keep them identical. */
#ifndef COS_DEVICES_H
#define COS_DEVICES_H
#include "types.h"
#include <stdbool.h>

enum { COS_DEV_NONE = 0, COS_DEV_PS2_CONTROLLER = 1, COS_DEV_PS2_KEYBOARD = 2, COS_DEV_PS2_MOUSE = 3,
       COS_DEV_USB_CONTROLLER = 4, COS_DEV_USB_DEVICE = 5 };
enum { COS_DEVROLE_KEYBOARD = 1, COS_DEVROLE_MOUSE = 2, COS_DEVROLE_POINTER_ABS = 4, COS_DEVROLE_STORAGE = 8,
       COS_DEVROLE_HUB = 16, COS_DEVROLE_WHEEL = 32, COS_DEVROLE_AUDIO = 64 };
enum { COS_USB_SPEED_UNKNOWN = 0, COS_USB_SPEED_FULL = 1, COS_USB_SPEED_LOW = 2, COS_USB_SPEED_HIGH = 3, COS_USB_SPEED_SUPER = 4 };

typedef struct {
    uint8_t kind, present, usb_speed, usb_ctrl;          /* usb_ctrl: 1 EHCI, 2 xHCI */
    uint8_t usb_addr, usb_root_port, usb_hub_addr, usb_hub_port;
    uint8_t dev_class, dev_subclass, dev_protocol, roles;
    uint16_t vid, pid, bcd_usb, bcd_dev;
    uint32_t extra0, extra1;                              /* storage: blocks, block size; PS/2 mouse: id, buttons; controller: ports, slots */
    char name[64];
    char vendor[48];
    char serial[40];
    char detail[96];
} cos_device_info_t;

int  cos_devices_list(cos_device_info_t *out, int max);

/* registration hooks (called by drivers) */
void cos_devices_ps2_keyboard(bool present, uint8_t id0, uint8_t id1);
void cos_devices_ps2_mouse(bool present, uint8_t device_id, bool wheel, uint8_t buttons);
void cos_devices_usb_role(uint8_t daddr, uint8_t role);
void cos_devices_usb_storage(uint8_t daddr, uint32_t blocks, uint32_t block_size);
void cos_devices_usb_mounted(uint8_t daddr);              /* from TinyUSB's tuh_mount_cb */
void cos_devices_usb_unmounted(uint8_t daddr);
#endif
