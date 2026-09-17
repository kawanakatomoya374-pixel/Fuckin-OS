/**
 * pci.c - PCI Bus Driver Implementation
 */

#include "pci.h"
#include "io.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "mm/paging.h"

/* 64-bit I/O macros if not defined */
#ifndef outl
#define outl(port, val) \
    __asm__ __volatile__ ("outl %%eax, %%dx" : : "a"((uint64_t)(val)), "d"((uint64_t)(port)))
#endif
#ifndef inl
#define inl(port) ({ \
    uint64_t val; \
    __asm__ __volatile__ ("inl %%dx, %%eax" : "=a"(val) : "d"((uint64_t)(port))); \
    val; \
})
#endif

static pci_dev_t* pci_devices = NULL;
static int pci_device_count = 0;

uint64_t pci_make_address(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset) {
    return (uint64_t)((bus << 16) | (slot << 11) | (func << 8) | 
                      (offset & 0xFC) | 0x80000000);
}

uint64_t pci_read_word(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset) {
    uint64_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDR, address);
    return (uint64_t)((inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8)) & 0xFFFF);
}

uint64_t pci_read_dword(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset) {
    uint64_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_word(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset, uint64_t value) {
    uint64_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDR, address);
    outw(PCI_CONFIG_DATA, value);
}


void pci_write_dword(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset, uint64_t value) {
    uint64_t address = pci_make_address(bus, slot, func, offset);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

uint64_t pci_get_vendor(uint64_t bus, uint64_t slot, uint64_t func) {
    return pci_read_word(bus, slot, func, PCI_VENDOR_ID);
}

uint64_t pci_get_device_id(uint64_t bus, uint64_t slot, uint64_t func) {
    return pci_read_word(bus, slot, func, PCI_DEVICE_ID);
}

uint64_t pci_get_status(uint64_t bus, uint64_t slot, uint64_t func) {
    return pci_read_word(bus, slot, func, PCI_STATUS);
}

uint64_t pci_get_class(uint64_t bus, uint64_t slot, uint64_t func) {
    return pci_read_dword(bus, slot, func, PCI_REVISION);
}

void pci_enable_mmio(pci_dev_t* dev) {
    if (!dev) return;
    uint64_t cmd = pci_read_word(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= 0x02;  // Memory space enable
    cmd |= 0x04;  // Bus mastering for DMA-capable controllers
    pci_write_word(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

void pci_scan_slot(uint64_t bus, uint64_t slot) {
    uint64_t vendor = pci_get_vendor(bus, slot, 0);
    if (vendor == 0xFFFF) return;
    
    uint8_t header_type = (uint8_t)(pci_read_word(bus, slot, 0, PCI_HEADER_TYPE) & 0xFF);
    uint8_t func_count = (header_type & 0x80) ? 8 : 1;
    
    for (uint64_t func = 0; func < func_count; func++) {
        vendor = pci_get_vendor(bus, slot, func);
        if (vendor == 0xFFFF) continue;
        
        pci_dev_t* dev = (pci_dev_t*)kmalloc(sizeof(pci_dev_t));
        if (!dev) continue;
        
        memset(dev, 0, sizeof(pci_dev_t));
        dev->bus = bus;
        dev->slot = slot;
        dev->func = func;
        dev->vendor = vendor;
        dev->device = pci_get_device_id(bus, slot, func);
        
        uint64_t class_reg = pci_read_dword(bus, slot, func, PCI_REVISION);
        dev->revision = class_reg & 0xFF;
        dev->prog_if = (class_reg >> 8) & 0xFF;
        dev->subclass = (class_reg >> 16) & 0xFF;
        dev->class_code = (class_reg >> 24) & 0xFF;
        dev->header_type = header_type & 0x7F;
        
        for (int i = 0; i < 6; i++) {
            dev->bar[i] = pci_read_dword(bus, slot, func, PCI_BAR0 + i * 4);
        }
        
        dev->irq_line = (uint8_t)(pci_read_word(bus, slot, func, PCI_IRQ_LINE) & 0xFF);
        dev->irq_pin = (uint8_t)(pci_read_word(bus, slot, func, PCI_IRQ_PIN) & 0xFF);
        
        dev->next = pci_devices;
        pci_devices = dev;
        pci_device_count++;
        
        serial_puts("[PCI] Found: ");
        serial_puthex(vendor);
        serial_puts(":");
        serial_puthex(dev->device);
        serial_puts(" class=");
        serial_puthex(dev->class_code);
        serial_puts(":");
        serial_puthex(dev->subclass);
        serial_puts("\n");
    }
}

void pci_scan_bus(uint64_t bus) {
    for (uint64_t slot = 0; slot < PCI_MAX_SLOTS; slot++) {
        pci_scan_slot(bus, slot);
    }
}

void pci_scan(void) {
    serial_puts("[PCI] Scanning all buses...\n");
    for (uint64_t bus = 0; bus < PCI_MAX_BUSES; bus++) {
        pci_scan_bus(bus);
    }
    serial_puts("[PCI] Found ");
    serial_putdec(pci_device_count);
    serial_puts(" devices\n");
}

void pci_init(void) {
    serial_puts("[PCI] PCI driver initializing...\n");
    pci_devices = NULL;
    pci_device_count = 0;
    pci_scan();
}

pci_dev_t* pci_get_device(uint64_t vendor, uint64_t device) {
    pci_dev_t* dev = pci_devices;
    while (dev) {
        if (dev->vendor == vendor && dev->device == device) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

pci_dev_t* pci_get_device_by_class(uint8_t class_code, uint8_t subclass) {
    pci_dev_t* dev = pci_devices;
    while (dev) {
        if (dev->class_code == class_code && dev->subclass == subclass) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

pci_dev_t* pci_get_device_by_class_and_prog_if(uint8_t class_code, uint8_t subclass, uint8_t prog_if) {
    pci_dev_t* dev = pci_devices;
    while (dev) {
        if (dev->class_code == class_code && dev->subclass == subclass && dev->prog_if == prog_if) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

pci_dev_t* pci_get_first_device(void) {
    return pci_devices;
}

pci_dev_t* pci_get_next_device(pci_dev_t* dev) {
    return dev ? dev->next : NULL;
}

void pci_enable_bus_mastering(pci_dev_t* dev) {
    if (!dev) return;
    uint64_t cmd = pci_read_word(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= 0x04;  // Bus mastering bit
    pci_write_word(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

void pci_dump_devices(void) {
    serial_puts("=== PCI Devices ===\n");
    pci_dev_t* dev = pci_devices;
    while (dev) {
        serial_puts("  Bus ");
        serial_putdec(dev->bus);
        serial_puts(" Slot ");
        serial_putdec(dev->slot);
        serial_puts(" Func ");
        serial_putdec(dev->func);
        serial_puts(": VID=");
        serial_puthex(dev->vendor);
        serial_puts(" DID=");
        serial_puthex(dev->device);
        serial_puts("\n");
        dev = dev->next;
    }
    serial_puts("===================\n");
}
