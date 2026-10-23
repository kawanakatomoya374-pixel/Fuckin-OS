#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_NONE
#endif

/* C-OS is a PC kernel, not one of TinyUSB's enumerated MCU targets. Select
 * the vendored generic EHCI HCD explicitly so the implementation is emitted
 * instead of compiling as an empty translation unit. */
#ifndef TUP_USBIP_EHCI
#define TUP_USBIP_EHCI 1
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#ifndef CFG_TUH_ENABLED
#define CFG_TUH_ENABLED 1
#endif

#ifndef CFG_TUH_MAX3421
#define CFG_TUH_MAX3421 0
#endif

/* Also the size of TinyUSB's deferred-attach queue: with several xHCI root ports every device that is plugged in at
 * boot has to wait for the one being enumerated, so the queue must hold one entry per port. */
#ifndef CFG_TUH_HUB
#define CFG_TUH_HUB 16
#endif

#ifndef CFG_TUH_DEVICE_MAX
#define CFG_TUH_DEVICE_MAX 16  /* hubs count as devices too: keyboard + mouse + hub + its children + storage */
#endif

#ifndef CFG_TUH_ENDPOINT_MAX
#define CFG_TUH_ENDPOINT_MAX 16
#endif

#ifndef CFG_TUH_API_EDPT_XFER
#define CFG_TUH_API_EDPT_XFER 0
#endif

/* CDC host is intentionally disabled until its serial-device bridge is
 * implemented. HID and MSC are fully linked below; leaving CDC enabled
 * without cdc_host.c creates unresolved class callbacks. */
#ifndef CFG_TUH_CDC
#define CFG_TUH_CDC 0
#endif
#ifndef CFG_TUH_MSC
#define CFG_TUH_MSC 1
#endif
#ifndef CFG_TUH_HID
/* One slot per HID *interface*, not per device: a keyboard + mouse, or a
 * single composite device (keyboard + media keys + pointer), needs several.
 * This was 1, which silently left every HID interface after the first
 * unmounted. */
#define CFG_TUH_HID 12
#endif
#ifndef CFG_TUH_MIDI
#define CFG_TUH_MIDI 0
#endif
#ifndef CFG_TUH_MIDI2
#define CFG_TUH_MIDI2 0
#endif
#ifndef CFG_TUH_VENDOR
#define CFG_TUH_VENDOR 0
#endif

#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED OPT_MODE_HIGH_SPEED
#endif

#ifndef CFG_TUH_MEM_SECTION
#define CFG_TUH_MEM_SECTION
#endif
#ifndef CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(4)))
#endif

#ifdef __cplusplus
}
#endif

#endif
