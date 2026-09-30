/**
 * pci.h - PCI Bus Driver
 */

#ifndef PCI_H
#define PCI_H

#include "types.h"
#include "driver.h"

/* PCI configuration ports */
#define PCI_CONFIG_ADDR     0xCF8
#define PCI_CONFIG_DATA     0xCFC

/* PCI configuration space offsets */
#define PCI_VENDOR_ID       0x00
#define PCI_DEVICE_ID       0x02
#define PCI_COMMAND         0x04
#define PCI_STATUS          0x06
#define PCI_REVISION        0x08
#define PCI_PROG_IF         0x09
#define PCI_SUBCLASS        0x0A
#define PCI_CLASS           0x0B
#define PCI_CACHE_LINE      0x0C
#define PCI_LATENCY         0x0D
#define PCI_HEADER_TYPE     0x0E
#define PCI_BIST            0x0F
#define PCI_BAR0            0x10
#define PCI_BAR1            0x14
#define PCI_BAR2            0x18
#define PCI_BAR3            0x1C
#define PCI_BAR4            0x20
#define PCI_BAR5            0x24
#define PCI_CARDBUS_CIS     0x28
#define PCI_SUB_VENDOR      0x2C
#define PCI_SUB_ID          0x2E
#define PCI_ROM_BASE        0x30
#define PCI_CAP_PTR         0x34
#define PCI_IRQ_LINE        0x3C
#define PCI_IRQ_PIN         0x3D
#define PCI_MIN_GNT         0x3E
#define PCI_MAX_LAT         0x3F

/* PCI class codes */
#define PCI_CLASS_OLD       0x00
#define PCI_CLASS_STORAGE   0x01
#define PCI_CLASS_NET       0x02
#define PCI_CLASS_DISPLAY   0x03
#define PCI_CLASS_MEDIA     0x04
#define PCI_CLASS_MEM       0x05
#define PCI_CLASS_BRIDGE    0x06
#define PCI_CLASS_COMM      0x07
#define PCI_CLASS_BASE      0x08
#define PCI_CLASS_INPUT     0x09
#define PCI_CLASS_DOCK      0x0A
#define PCI_CLASS_PROC      0x0B
#define PCI_CLASS_SERIAL    0x0C
#define PCI_CLASS_WIRELESS  0x0D
#define PCI_CLASS_INTEL     0x0E
#define PCI_CLASS_SAT       0x0F
#define PCI_CLASS_CRYPTO    0x10
#define PCI_CLASS_SIG       0x11
#define PCI_CLASS_ACCEL     0x12
#define PCI_CLASS_INSTR     0x13
#define PCI_CLASS_UNASSIGNED 0xFF

/* PCI max devices */
#define PCI_MAX_BUSES       256
#define PCI_MAX_SLOTS       32
#define PCI_MAX_FUNCS       8
#define PCI_MAX_DEVICES     (PCI_MAX_BUSES * PCI_MAX_SLOTS * PCI_MAX_FUNCS)

/* PCI device structure */
typedef struct pci_device {
    uint64_t bus;
    uint64_t slot;
    uint64_t func;
    
    uint64_t vendor;
    uint64_t device;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    
    uint64_t bar[6];
    uint64_t rom_base;
    uint8_t  irq_line;
    uint8_t  irq_pin;
    
    struct pci_device* next;
} pci_dev_t;

/* Function prototypes */
void pci_init(void);
uint64_t pci_read_word(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset);
uint64_t pci_read_dword(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset);
void pci_write_word(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset, uint64_t value);
void pci_write_dword(uint64_t bus, uint64_t slot, uint64_t func, uint64_t offset, uint64_t value);

uint64_t pci_get_vendor(uint64_t bus, uint64_t slot, uint64_t func);
uint64_t pci_get_device_id(uint64_t bus, uint64_t slot, uint64_t func);
uint64_t pci_get_status(uint64_t bus, uint64_t slot, uint64_t func);
uint64_t pci_get_class(uint64_t bus, uint64_t slot, uint64_t func);

void pci_scan(void);
void pci_scan_bus(uint64_t bus);
void pci_scan_slot(uint64_t bus, uint64_t slot);

pci_dev_t* pci_get_device(uint64_t vendor, uint64_t device);
pci_dev_t* pci_get_device_by_class(uint8_t class_code, uint8_t subclass);
pci_dev_t* pci_get_device_by_class_and_prog_if(uint8_t class_code, uint8_t subclass, uint8_t prog_if);
void pci_enable_bus_mastering(pci_dev_t* dev);
void pci_enable_mmio(pci_dev_t* dev);

void pci_dump_devices(void);
const char* pci_class_name(uint8_t class_code);

/* Device enumeration helpers */
pci_dev_t* pci_get_first_device(void);
pci_dev_t* pci_get_next_device(pci_dev_t* dev);

#endif /* PCI_H */
