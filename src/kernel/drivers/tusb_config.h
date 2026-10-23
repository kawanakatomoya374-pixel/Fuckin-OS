/**
 * tusb_config.h - TinyUSB configuration for C-OS (x86 PC / EHCI)
 *
 * There is no TinyUSB-recognized "MCU" for a generic PC, so
 * CFG_TUSB_MCU is set to a value tusb_mcu.h's OPT_MCU_* table never
 * matches; every derived macro that table would normally set just
 * falls through to tusb_mcu.h's own generic defaults. The one
 * default worth overriding by hand is high-speed support: EHCI is a
 * USB2 controller, so we say so explicitly rather than accept the
 * fallback fullspeed-only default.
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU          9999
#define TUP_RHPORT_HIGHSPEED  1

/* tusb_mcu.h normally sets this from whichever MCU branch matches
 * CFG_TUSB_MCU - since 9999 matches none of them, it has to be set
 * by hand. portable/ehci/ehci.c's entire contents are wrapped in
 * "#if CFG_TUH_ENABLED && defined(TUP_USBIP_EHCI)"; without this,
 * the file silently compiles to an empty translation unit instead
 * of an error, which is easy to miss. */
#define TUP_USBIP_EHCI

/* Single-threaded: usb_task() polls tuh_task() from the GUI main
 * loop (see kernel.c), the same way net_poll() already ticks the
 * e1000 driver - no RTOS integration needed. */
#define CFG_TUSB_OS           OPT_OS_NONE
#define CFG_TUSB_DEBUG        0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN    __attribute__((aligned(4)))

/* -------------------- Host configuration -------------------- */

#define CFG_TUH_ENABLED       1
#define CFG_TUH_MAX_SPEED     OPT_MODE_DEFAULT_SPEED

#define BOARD_TUH_RHPORT      0

#define CFG_TUH_ENUMERATION_BUFSIZE  256

/* No external-hub support yet - devices plugged directly into a root
 * port enumerate fine, a hub plugged into a root port won't show its
 * downstream ports. Storage/CDC/vendor classes are out of scope for
 * this pass; only HID (keyboard/mouse) is wired up. */
#define CFG_TUH_HUB           0
#define CFG_TUH_CDC           0
#define CFG_TUH_MSC           0
#define CFG_TUH_VENDOR        0

#define CFG_TUH_DEVICE_MAX    4

#define CFG_TUH_HID           (2 * CFG_TUH_DEVICE_MAX)
#define CFG_TUH_HID_EP_BUFSIZE  64

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
