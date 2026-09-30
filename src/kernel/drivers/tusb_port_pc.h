/**
 * tusb_port_pc.h - PC/PCI board glue for TinyUSB's EHCI host driver
 *
 * Bridges TinyUSB (a library with no idea what "PCI" or "paging" are)
 * to this kernel's own PCI enumerator and page mapper. tusb_port_pc.c
 * provides the two functions TinyUSB itself requires as extern
 * (hcd_init(), tusb_time_millis_api()) plus these status accessors so
 * usb.c can report what it found without redoing the PCI scan.
 */

#ifndef TUSB_PORT_PC_H
#define TUSB_PORT_PC_H

#include <stdint.h>
#include <stdbool.h>

/* Scans PCI for an EHCI (USB2) host controller, maps its MMIO BAR,
 * and remembers the result. Safe to call once, before tuh_init().
 * Returns true if a controller was found and mapped. */
bool tusb_port_pc_probe(void);

/* True once tusb_port_pc_probe() has found and mapped a controller. */
bool tusb_port_pc_has_controller(void);

/* Physical/identity-mapped base address of the controller's MMIO
 * registers (BAR0, masked). Only meaningful if
 * tusb_port_pc_has_controller() is true. */
uint64_t tusb_port_pc_base_addr(void);

/* NULL if the last probe/init succeeded, otherwise a short
 * human-readable reason it didn't. */
const char* tusb_port_pc_last_error(void);

/* Translates between this driver's own pointers and the 32-bit
 * hardware DMA addresses EHCI's registers/descriptors use - see the
 * long comment above these two functions in tusb_port_pc.c.
 * portable/ehci/ehci.c includes this header directly at the small
 * number of call sites that need it. */
uint32_t tup_ehci_virt_to_dma(const void* virt);
void* tup_ehci_dma_to_virt(uint32_t dma_addr);

#endif /* TUSB_PORT_PC_H */
