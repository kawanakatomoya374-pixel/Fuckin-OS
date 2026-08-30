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

/* Implemented in tusb_hid_bridge.c - number of HID keyboards/mice
 * currently mounted, tracked there since TinyUSB itself doesn't
 * expose a mounted-device count. */
extern int tusb_hid_bridge_device_count(void);
extern int tusb_msc_bridge_device_count(void);

static bool s_usb_running = false;
static bool s_usb_poll_observed = false;
static bool s_usb_poll_trace_emitted = false;

void usb_init(void) {
    s_usb_running = false;
    s_usb_poll_observed = false;
    s_usb_poll_trace_emitted = false;

    if (!tusb_port_pc_probe()) {
        /* No EHCI controller on this machine/VM - nothing more to
         * do. tusb_port_pc_last_error() already explains why. */
        return;
    }

    /* C-OS is a USB host. `tuh_init()` is the host-stack entry point
     * that invokes hcd_init() for the mapped EHCI controller; the generic
     * `tusb_init()` path did not start this host-only configuration. */
    if (!tuh_init(BOARD_TUH_RHPORT)) {
        serial_puts("[USB] TinyUSB host initialization failed.\n");
        return;
    }

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
void usb_poll(void) {
    if (!s_usb_running) return;
    if (!s_usb_poll_observed) {
        s_usb_poll_observed = true;
        serial_puts("[USB] TinyUSB non-blocking service loop active.\n");
    }
    if (!s_usb_poll_trace_emitted) {
        serial_puts("[USB] poll trace: entering EHCI service\n");
    }
    hcd_int_handler(BOARD_TUH_RHPORT, false);
    if (!s_usb_poll_trace_emitted) {
        serial_puts("[USB] poll trace: EHCI service returned\n");
    }

    /* `tuh_task()` is a convenience wrapper around
     * tuh_task_ext(UINT32_MAX, false): with no queued event it blocks the
     * boot-service loop indefinitely, so hcd_int_handler() never observes a
     * later root-port change or completed transfer.  C-OS owns the outer
     * event loop; drain all currently queued TinyUSB events and return. */
    tuh_task_ext(0, false);
    if (!s_usb_poll_trace_emitted) {
        s_usb_poll_trace_emitted = true;
        serial_puts("[USB] poll trace: TinyUSB task returned\n");
    }
}

bool usb_is_initialized(void) {
    return s_usb_running;
}

bool usb_has_usb2(void) {
    return tusb_port_pc_has_controller();
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

    snprintf(out, out_size, "USB2 (EHCI) @ 0x%llx | HID: %d | MSC: %d",
             (unsigned long long)tusb_port_pc_base_addr(),
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
