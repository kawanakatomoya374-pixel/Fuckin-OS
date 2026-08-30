/**
 * tusb_port_pc.c - PC/PCI board glue for TinyUSB's EHCI host driver
 *
 * TinyUSB ships a real, generic EHCI host controller driver
 * (portable/ehci/ehci.c) - the actual USB2 protocol handling is
 * already written and doesn't need touching. What every non-embedded
 * board has to supply for itself is: where are the controller's
 * registers, what time is it, and (on a 64-bit host with paging,
 * unlike this driver's original 32-bit/no-MMU targets) how to
 * translate between this driver's own pointers and the 32-bit
 * addresses it hands the hardware. That's what this file does:
 *
 *   - tusb_port_pc_probe() finds the EHCI controller over PCI (class
 *     0x0C, subclass 0x03, prog-if 0x20 - the standard PCI class code
 *     for "USB2 EHCI host controller"), maps its MMIO BAR the same
 *     way e1000.c maps the NIC's registers (paging_map_range() at a
 *     virtual address equal to the physical one, i.e. an on-demand
 *     identity map - see paging.h's PHYS_TO_VIRT comment for why a
 *     raw physical address can't just be dereferenced without this),
 *     and remembers the result.
 *   - hcd_init(), called once by tuh_init(), reads the EHCI
 *     capability registers' CAPLENGTH byte to find where the
 *     operational registers start, then hands both addresses to
 *     ehci_init() (declared in portable/ehci/ehci_api.h) - the one
 *     function the EHCI port expects its board glue to call.
 *   - tup_ehci_virt_to_dma()/tup_ehci_dma_to_virt() are called from
 *     ehci.c itself (patched to use them - see the comment at the
 *     top of that file) everywhere it stores or reads a hardware
 *     address.
 *   - hcd_int_enable()/hcd_int_disable() are mandatory per hcd.h but
 *     ehci.c doesn't implement them; see the comment above them below
 *     for why plain no-ops are correct for this poll-driven
 *     integration.
 *   - tusb_time_millis_api(), the millisecond clock TinyUSB's
 *     OPT_OS_NONE build asks the platform for (see osal_none.h),
 *     already exists in timer.c - nothing to add here.
 */

#include "types.h"
#include "memory.h"
#include "serial.h"
#include "pci.h"
#include "mm/paging.h"
#include "memory_physical.h"
#include "tusb_port_pc.h"

#include "tusb.h"
#include "host/hcd.h"
#include "portable/ehci/ehci_api.h"

/* 4KB comfortably covers an EHCI controller's capability registers,
 * operational registers, and per-port status/control registers (up
 * to 15 ports @ 4 bytes each) - real controllers use far less. */
#define EHCI_MMIO_WINDOW_SIZE 0x1000ULL

static bool s_has_controller = false;
static uint64_t s_base_addr = 0;
static const char* s_last_error = NULL;

bool tusb_port_pc_probe(void) {
    s_has_controller = false;
    s_base_addr = 0;
    s_last_error = NULL;

    pci_dev_t* dev = pci_get_device_by_class_and_prog_if(PCI_CLASS_SERIAL, 0x03, 0x20);
    if (!dev) {
        s_last_error = "No EHCI (USB2) controller found on this system";
        serial_puts("[USB] No EHCI controller found on PCI bus.\n");
        return false;
    }

    uint64_t bar0 = dev->bar[0];
    if (bar0 & 0x1) {
        /* Bit 0 set means an I/O-space BAR. EHCI always exposes an
         * MMIO register window, never I/O ports - if this bit is
         * set, bar[0] isn't the register window (or this isn't a
         * spec-compliant EHCI controller), so don't guess further. */
        s_last_error = "EHCI controller BAR0 is not memory-mapped";
        serial_puts("[USB] EHCI BAR0 is I/O space, not MMIO - unsupported.\n");
        return false;
    }
    uint64_t base = bar0 & ~0xFULL;

    pci_enable_bus_mastering(dev);
    pci_enable_mmio(dev);

    if (!paging_map_range(base, base, EHCI_MMIO_WINDOW_SIZE, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE)) {
        s_last_error = "Failed to map EHCI MMIO registers";
        serial_puts("[USB] Failed to map EHCI MMIO window.\n");
        return false;
    }
    phys_memory_reserve_range((phys_addr_t)base, EHCI_MMIO_WINDOW_SIZE);

    s_base_addr = base;
    s_has_controller = true;
    serial_puts("[USB] EHCI controller found, MMIO base 0x");
    serial_puthex(base);
    serial_puts("\n");
    return true;
}

bool tusb_port_pc_has_controller(void) {
    return s_has_controller;
}

uint64_t tusb_port_pc_base_addr(void) {
    return s_base_addr;
}

const char* tusb_port_pc_last_error(void) {
    return s_last_error;
}

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
    (void)rh_init;
    if (!s_has_controller) {
        serial_puts("[USB] hcd_init() called with no controller mapped.\n");
        return false;
    }

    uint32_t cap_base = (uint32_t)s_base_addr;
    uint8_t caplength = *(volatile uint8_t*)(uintptr_t)cap_base;
    uint32_t op_base = cap_base + caplength;

    serial_puts("[USB] Starting EHCI controller...\n");
    bool initialized = ehci_init(rhport, cap_base, op_base);
    if (initialized) {
        /* Standard EHCI operational offsets: CONFIGFLAG=0x40, PORTSC1=0x44.
         * One boot-time snapshot makes root-port ownership/connection state
         * diagnosable without logging in the ISR or poll hot path. */
        volatile uint32_t* op = (volatile uint32_t*)(uintptr_t)op_base;
        serial_puts("[USB] EHCI CONFIGFLAG=0x");
        serial_puthex(op[0x40 / sizeof(uint32_t)]);
        serial_puts(" PORTSC1=0x");
        serial_puthex(op[0x44 / sizeof(uint32_t)]);
        serial_puts("\n");
    }
    return initialized;
}

/* tusb_time_millis_api() - the millisecond clock TinyUSB's
 * OPT_OS_NONE build needs (see osal_none.h) - already exists in
 * timer.c, calling hal_timer_get_ms(); nothing more to add here. */

/* ------------------------------------------------------------------
 * EHCI DMA address translation
 * ------------------------------------------------------------------
 * TinyUSB's EHCI port keeps its own queue heads/descriptors
 * (ehci_data, a static struct inside ehci.c) as plain C pointers,
 * and stores/reads them straight into 32-bit hardware address
 * fields with a bare (uint32_t) cast - correct on the 32-bit MCUs
 * this driver was originally written for, where a pointer already
 * *is* 4 bytes and phys==virt (no MMU). Neither holds here:
 * pointers are 8 bytes, and ehci_data lives in the kernel's own
 * mapped image, not at an address numerically equal to its physical
 * location. See ehci.c's TUP_EHCI_VIRT_TO_DMA/TUP_EHCI_DMA_TO_VIRT
 * call sites for where this gets used.
 *
 * virt->dma goes through paging_virt_to_phys() (a real page-table
 * walk, needed because ehci_data is kernel static data, not heap
 * memory reachable through the fixed-offset HHDM window - the same
 * reason ac97.c's ac97_phys_of() uses it for its DMA buffers).
 * dma->virt goes through PHYS_TO_VIRT(), which is safe here because
 * every address this driver stores is itself the result of a prior
 * virt_to_dma call - i.e. always a physical address inside ordinary
 * RAM, always within the HHDM's covered range - even though it
 * won't numerically match the pointer ehci_data's fields actually
 * have in the kernel's own image mapping; both are equally valid
 * windows onto the same physical memory. */
uint32_t tup_ehci_virt_to_dma(const void* virt) {
    return (uint32_t)paging_virt_to_phys((uint64_t)(uintptr_t)virt);
}

void* tup_ehci_dma_to_virt(uint32_t dma_addr) {
    return (void*)(uintptr_t)PHYS_TO_VIRT((uint64_t)dma_addr);
}

/* usbh.c calls these unconditionally (both around init/deinit and to
 * guard its internal event queue against a concurrent hcd_int_handler
 * call), but ehci.c - unlike other TinyUSB host controller ports -
 * doesn't implement them itself, which would otherwise be a link
 * error. This integration never registers a real hardware IRQ (see
 * usb_poll() in usb.c, which calls hcd_int_handler() from ordinary
 * polled context instead, the same way net_poll() ticks e1000), so
 * there's no actual interrupt source for these to mask - they're
 * correct as no-ops here specifically because nothing ever calls
 * hcd_int_handler() from any context these would need to guard
 * against. */
void hcd_int_enable(uint8_t rhport) {
    (void)rhport;
}

void hcd_int_disable(uint8_t rhport) {
    (void)rhport;
}
