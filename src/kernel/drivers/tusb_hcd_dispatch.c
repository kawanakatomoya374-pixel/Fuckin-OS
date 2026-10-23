/**
 * tusb_hcd_dispatch.c - TinyUSB's hcd_* entry points, routed to the right host controller driver.
 *
 * TinyUSB links exactly one set of hcd_* functions. C-OS has two controller types: the EHCI driver that ships with
 * TinyUSB (its functions are renamed ehci_hcd_* at build time, see the Makefile) and xhci_hcd.c. A TinyUSB "rhport"
 * is one root-hub port: rhport 0 is the EHCI controller, rhports XHCI_RHPORT_BASE.. are the xHCI root ports.
 */
#include "types.h"
#include "tusb.h"
#include "host/hcd.h"
#include "tusb_port_pc.h"
#include "xhci_hcd.h"

#define IS_XHCI(rh) ((rh) >= XHCI_RHPORT_BASE)

/* EHCI driver (portable/ehci/ehci.c, symbols renamed by -D flags) */
extern uint32_t ehci_hcd_frame_number(uint8_t rhport);
extern void ehci_hcd_port_reset(uint8_t rhport);
extern void ehci_hcd_port_reset_end(uint8_t rhport);
extern bool ehci_hcd_port_connect_status(uint8_t rhport);
extern tusb_speed_t ehci_hcd_port_speed_get(uint8_t rhport);
extern void ehci_hcd_device_close(uint8_t rhport, uint8_t daddr);
extern bool ehci_hcd_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const *ep_desc);
extern bool ehci_hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);
extern bool ehci_hcd_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const setup_packet[8]);
extern bool ehci_hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen);
extern bool ehci_hcd_edpt_abort_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);
extern bool ehci_hcd_edpt_clear_stall(uint8_t rhport, uint8_t daddr, uint8_t ep_addr);
extern void ehci_hcd_int_handler(uint8_t rhport, bool in_isr);

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
    if (IS_XHCI(rhport)) return xhci_hcd_init(rhport);
    return tusb_port_pc_ehci_init(rhport, rh_init);
}

uint32_t hcd_frame_number(uint8_t rhport) { return IS_XHCI(rhport) ? xhci_hcd_frame_number(rhport) : ehci_hcd_frame_number(rhport); }
void hcd_port_reset(uint8_t rhport) { if (IS_XHCI(rhport)) xhci_hcd_port_reset(rhport); else ehci_hcd_port_reset(rhport); }
void hcd_port_reset_end(uint8_t rhport) { if (IS_XHCI(rhport)) xhci_hcd_port_reset_end(rhport); else ehci_hcd_port_reset_end(rhport); }
bool hcd_port_connect_status(uint8_t rhport) { return IS_XHCI(rhport) ? xhci_hcd_port_connect_status(rhport) : ehci_hcd_port_connect_status(rhport); }
tusb_speed_t hcd_port_speed_get(uint8_t rhport) { return IS_XHCI(rhport) ? xhci_hcd_port_speed_get(rhport) : ehci_hcd_port_speed_get(rhport); }
void hcd_device_close(uint8_t rhport, uint8_t daddr) { if (IS_XHCI(rhport)) xhci_hcd_device_close(rhport, daddr); else ehci_hcd_device_close(rhport, daddr); }
bool hcd_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const *d) { return IS_XHCI(rhport) ? xhci_hcd_edpt_open(rhport, daddr, d) : ehci_hcd_edpt_open(rhport, daddr, d); }
bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep) { return IS_XHCI(rhport) ? xhci_hcd_edpt_close(rhport, daddr, ep) : ehci_hcd_edpt_close(rhport, daddr, ep); }
bool hcd_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const s[8]) { return IS_XHCI(rhport) ? xhci_hcd_setup_send(rhport, daddr, s) : ehci_hcd_setup_send(rhport, daddr, s); }
bool hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep, uint8_t *buf, uint16_t len) { return IS_XHCI(rhport) ? xhci_hcd_edpt_xfer(rhport, daddr, ep, buf, len) : ehci_hcd_edpt_xfer(rhport, daddr, ep, buf, len); }
bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep) { return IS_XHCI(rhport) ? xhci_hcd_edpt_abort_xfer(rhport, daddr, ep) : ehci_hcd_edpt_abort_xfer(rhport, daddr, ep); }
bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t daddr, uint8_t ep) { return IS_XHCI(rhport) ? xhci_hcd_edpt_clear_stall(rhport, daddr, ep) : ehci_hcd_edpt_clear_stall(rhport, daddr, ep); }
void hcd_int_handler(uint8_t rhport, bool in_isr) { if (IS_XHCI(rhport)) xhci_hcd_int_handler(rhport); else ehci_hcd_int_handler(rhport, in_isr); }
