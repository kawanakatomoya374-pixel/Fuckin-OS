/**
 * xhci_hcd.h - xHCI (USB 3.x) host controller driver, presented to TinyUSB as a host controller driver (HCD).
 *
 * TinyUSB ships EHCI and OHCI HCDs but no xHCI one. This file is that missing piece: it implements TinyUSB's hcd_*
 * contract on top of the xHCI rings, so TinyUSB's own enumeration, hub, HID and mass-storage drivers work unchanged
 * on xHCI-only machines (most PCs built since ~2013).
 *
 * Every xHCI root port is exposed to TinyUSB as its own root-hub port (rhport 1..N; rhport 0 stays the EHCI
 * controller). The xHCI "Address Device" command performs SET_ADDRESS itself, so the SET_ADDRESS request TinyUSB
 * issues during enumeration is absorbed here (the slot is bound to the address TinyUSB picked).
 *
 * Polling driven, like the rest of the USB stack here: hcd_int_handler() drains the event ring.
 */
#ifndef XHCI_HCD_H
#define XHCI_HCD_H

#include "types.h"
#include <stdbool.h>
#include "tusb.h"
#include "host/hcd.h"

/* First TinyUSB rhport number used for xHCI root ports. */
#define XHCI_RHPORT_BASE 1
#define XHCI_MAX_ROOT_PORTS 24

bool xhci_probe(void);                 /* finds + maps an xHCI controller (no hardware start) */
bool xhci_present(void);
uint8_t xhci_root_port_count(void);    /* root ports exposed to TinyUSB (<= XHCI_MAX_ROOT_PORTS) */
const char* xhci_last_error(void);

typedef struct {
    bool present, running;
    uint8_t usb_major, usb_minor;      /* HCIVERSION as major.minor (e.g. 1.10) */
    uint8_t ports, slots, ctx64;
    uint16_t vendor, device;
    uint64_t mmio;
    uint8_t port_usb3[XHCI_MAX_ROOT_PORTS + 1];     /* index 1..N: 1 if the port belongs to a USB3 protocol range */
    uint8_t port_connected[XHCI_MAX_ROOT_PORTS + 1];
} xhci_info_t;
void xhci_get_info(xhci_info_t* out);

/* TinyUSB HCD entry points (dispatched from tusb_hcd_dispatch.c). */
bool     xhci_hcd_init(uint8_t rhport);
void     xhci_hcd_int_handler(uint8_t rhport);
uint32_t xhci_hcd_frame_number(uint8_t rhport);
bool     xhci_hcd_port_connect_status(uint8_t rhport);
void     xhci_hcd_port_reset(uint8_t rhport);
void     xhci_hcd_port_reset_end(uint8_t rhport);
tusb_speed_t xhci_hcd_port_speed_get(uint8_t rhport);
void     xhci_hcd_device_close(uint8_t rhport, uint8_t daddr);
bool     xhci_hcd_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const* ep_desc);
bool     xhci_hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);
bool     xhci_hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t* buffer, uint16_t buflen);
bool     xhci_hcd_edpt_abort_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);
bool     xhci_hcd_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const setup_packet[8]);
bool     xhci_hcd_edpt_clear_stall(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);

/* Isochronous OUT streaming (used by the USB audio driver): packets are copied and queued, completions only counted. */
bool     xhci_iso_submit(uint8_t daddr, uint8_t ep_addr, const void* data, uint16_t len);
uint32_t xhci_iso_inflight(uint8_t daddr, uint8_t ep_addr);
void     xhci_iso_stats(uint8_t daddr, uint8_t ep_addr, uint32_t* done, uint32_t* errors);

#endif
