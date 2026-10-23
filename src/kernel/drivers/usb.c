/**
 * usb.c - USB subsystem entry point
 *
 * Wires TinyUSB's host stack into C-OS. The actual USB protocol
 * handling (enumeration, transfers, the EHCI controller itself) is
 * all TinyUSB's own code - see tusb_port_pc.c for the PCI/paging
 * glue that tells it where its hardware is, and
 * tusb_hid_bridge.c for the HID keyboard/mouse report handling.
 * This file just starts it up and answers "is it working" for
 * everything else in the OS (the taskbar's USB indicator, the task
 * manager's hardware panel) - see usb.h for the API those callers
 * already expect.
 *
 * usb_init() is poll-driven rather than interrupt-driven: usb_poll()
 * calls tuh_task() from the GUI main loop, the same way net_poll()
 * already ticks the e1000 driver from that same loop instead of
 * using a hardware IRQ. That keeps this first integration free of
 * IDT/PIC interrupt-routing edge cases (shared IRQ lines etc.); a
 * real IRQ hookup can replace the poll later without changing
 * anything above this file.
 */

#include "types.h"
#include "string.h"
#include "usb.h"
#include "tusb_port_pc.h"
#include "serial.h"

#include "tusb.h"
#include "host/usbh.h"
#include "xhci_hcd.h"
#include "usb_audio.h"

#include "cos_devices.h"

/* TinyUSB: every device that finished enumeration */
static volatile int s_usb_mounted_total = 0;
int usb_mounted_count(void) { return s_usb_mounted_total; }
void tuh_mount_cb(uint8_t daddr) { __sync_fetch_and_add(&s_usb_mounted_total, 1); cos_devices_usb_mounted(daddr); }
void tuh_umount_cb(uint8_t daddr) { __sync_fetch_and_sub(&s_usb_mounted_total, 1); cos_devices_usb_unmounted(daddr); }

/* TinyUSB is not thread safe. The GUI loop polls it, and so does a thread waiting for a mass-storage transfer (the disk driver in
 * tusb_msc_bridge.c): whoever holds this flag talks to TinyUSB. */
static volatile int s_usb_busy = 0;
bool usb_try_lock(void) { return __sync_lock_test_and_set(&s_usb_busy, 1) == 0; }
void usb_unlock(void) { __sync_lock_release(&s_usb_busy); }

/* Implemented in tusb_hid_bridge.c - number of HID keyboards/mice
 * currently mounted, tracked there since TinyUSB itself doesn't
 * expose a mounted-device count. */
extern int tusb_hid_bridge_device_count(void);
extern int tusb_msc_bridge_device_count(void);

static bool s_usb_running = false;
static bool s_ehci_running = false, s_xhci_running = false;
static bool s_usb_poll_observed = false;
static bool s_usb_poll_trace_emitted = false;

void usb_init(void) {
    s_usb_running = false;
    s_usb_poll_observed = false;
    s_usb_poll_trace_emitted = false;

    bool ehci = tusb_port_pc_probe();
    bool xhci = xhci_probe();
    if (!ehci && !xhci) return;                 /* tusb_port_pc_last_error() / xhci_last_error() say why */

    /* TinyUSB sees every root-hub port as an "rhport": 0 is the EHCI controller, XHCI_RHPORT_BASE.. are the xHCI
     * root ports (see tusb_hcd_dispatch.c). */
    s_ehci_running = s_xhci_running = false;
    if (ehci) {
        if (tuh_init(BOARD_TUH_RHPORT)) s_ehci_running = true;
        else serial_puts("[USB] TinyUSB host initialization failed for EHCI.\n");
    }
    if (xhci) {
        if (tuh_init(XHCI_RHPORT_BASE) && xhci_root_port_count() > 0) {
            for (uint8_t k = 1; k < xhci_root_port_count(); ++k) (void)tuh_init((uint8_t)(XHCI_RHPORT_BASE + k));
            s_xhci_running = true;
        } else {
            serial_puts("[USB] xHCI host initialization failed");
            if (xhci_last_error()) { serial_puts(": "); serial_puts(xhci_last_error()); }
            serial_puts("\n");
        }
    }
    if (!s_ehci_running && !s_xhci_running) return;

    s_usb_running = true;
    serial_puts("[USB] USB host stack initialized.\n");
}

/* Call regularly from the main loop (see kernel.c) - drives
 * enumeration and HID report polling. A no-op until usb_init()
 * actually found and started a controller.
 *
 * hcd_int_handler() is normally invoked by a real hardware IRQ: it
 * checks the controller's status register and turns whatever it
 * finds into events on an internal queue, which tuh_task() then
 * drains. Since this integration never registers a real IRQ (see the
 * module comment above), calling hcd_int_handler() here each cycle
 * is what takes its place - without it tuh_task() would have nothing
 * to do, since nothing else ever checks the hardware. */
static void usb_poll_locked(void);
void usb_poll(void) {
    if (!s_usb_running || !usb_try_lock()) return;           /* somebody else is servicing the controller right now */
    usb_poll_locked();
    usb_unlock();
}
/* For a caller that waits for a transfer: services the controller once, whoever else is polling. */
void usb_service(void) {
    if (!s_usb_running) return;
    if (!usb_try_lock()) return;
    usb_poll_locked();
    usb_unlock();
}
static void usb_poll_locked(void) {
    if (!s_usb_poll_observed) {
        s_usb_poll_observed = true;
        serial_puts("[USB] TinyUSB non-blocking service loop active.\n");
    }
    if (!s_usb_poll_trace_emitted) {
        serial_puts("[USB] poll trace: entering host controller service\n");
    }
    if (s_ehci_running) hcd_int_handler(BOARD_TUH_RHPORT, false);
    if (s_xhci_running)
        for (uint8_t k = 0; k < xhci_root_port_count(); ++k) hcd_int_handler((uint8_t)(XHCI_RHPORT_BASE + k), false);
    if (!s_usb_poll_trace_emitted) {
        serial_puts("[USB] poll trace: host controller service returned\n");
    }

    /* `tuh_task()` is a convenience wrapper around
     * tuh_task_ext(UINT32_MAX, false): with no queued event it blocks the
     * boot-service loop indefinitely, so hcd_int_handler() never observes a
     * later root-port change or completed transfer.  C-OS owns the outer
     * event loop; drain all currently queued TinyUSB events and return. */
    tuh_task_ext(0, false);
    usb_audio_pump();
    if (!s_usb_poll_trace_emitted) {
        s_usb_poll_trace_emitted = true;
        serial_puts("[USB] poll trace: TinyUSB task returned\n");
    }
}

bool usb_ehci_active(void) { return s_ehci_running; }
bool usb_xhci_active(void) { return s_xhci_running; }

bool usb_is_initialized(void) {
    return s_usb_running;
}

bool usb_has_usb2(void) {
    return tusb_port_pc_has_controller() || xhci_present();
}

const char* usb_get_last_error(void) {
    return tusb_port_pc_last_error();
}

void usb_get_status(char* out, size_t out_size) {
    if (!out || out_size == 0) return;

    if (!s_usb_running) {
        snprintf(out, out_size, "USB: not running");
        return;
    }

    char ctl[64];
    ctl[0] = 0;
    if (s_ehci_running) snprintf(ctl, sizeof ctl, "EHCI");
    if (s_xhci_running) {
        xhci_info_t xi;
        xhci_get_info(&xi);
        char x[32];
        snprintf(x, sizeof x, "%sxHCI %u.%u", ctl[0] ? "+" : "", xi.usb_major, xi.usb_minor);
        size_t l = strlen(ctl);
        snprintf(ctl + l, sizeof ctl - l, "%s", x);
    }
    snprintf(out, out_size, "USB (%s) | HID: %d | MSC: %d", ctl,
             tusb_hid_bridge_device_count(), tusb_msc_bridge_device_count());
}

/* usb_control_transfer/usb_bulk_transfer/usb_interrupt_transfer are
 * declared in usb.h for callers that want to talk to a raw
 * usb_dev_t/usb_req_t directly, but nothing in C-OS does that -
 * TinyUSB's own usbh/hid_host layers own every actual transfer once
 * usb_init() hands control to them (see tusb_hid_bridge.c). These
 * stay as clearly-failing stubs so a future direct caller gets an
 * honest "not supported" instead of a missing symbol at link time. */
int usb_control_transfer(usb_dev_t* dev, usb_req_t* req) {
    (void)dev; (void)req;
    return -1;
}

int usb_bulk_transfer(usb_dev_t* dev, int endpoint, void* data, int len) {
    (void)dev; (void)endpoint; (void)data; (void)len;
    return -1;
}

int usb_interrupt_transfer(usb_dev_t* dev, int endpoint, void* data, int len) {
    (void)dev; (void)endpoint; (void)data; (void)len;
    return -1;
}
