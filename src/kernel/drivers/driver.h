/**
 * driver.h - Device Driver Framework
 * 
 * Unified driver model supporting character, block, and network devices.
 */

#ifndef DRIVER_H
#define DRIVER_H

#include "types.h"

/* File offset type (for compatibility) */
typedef int64_t off_t;

/* Device types */
typedef enum {
    DEV_TYPE_UNKNOWN = 0,
    DEV_TYPE_CHAR,      // Character device (keyboard, serial)
    DEV_TYPE_BLOCK,     // Block device (disk)
    DEV_TYPE_NET,       // Network device
    DEV_TYPE_USB,       // USB device
    DEV_TYPE_PCI,       // PCI device
    DEV_TYPE_PLATFORM,  // Platform device
    DEV_TYPE_MISC,      // Miscellaneous
} device_type_t;

/* Device states */
typedef enum {
    DEV_STATE_UNINITIALIZED = 0,
    DEV_STATE_PROBING,
    DEV_STATE_ACTIVE,
    DEV_STATE_SUSPENDED,
    DEV_STATE_DISABLED,
    DEV_STATE_ERROR,
} device_state_t;

/* Maximum devices */
#define MAX_DEVICES         256
#define MAX_DRIVERS         64
#define DEV_NAME_LEN        32

/* Device IDs */
typedef struct {
    uint64_t vendor;
    uint64_t device;
    uint64_t subvendor;
    uint64_t subdevice;
    uint64_t class_code;
} device_id_t;

/* Forward declaration */
struct device;

/* Device operations */
typedef struct device_ops {
    int (*init)(struct device* dev);
    int (*probe)(struct device* dev);
    int (*remove)(struct device* dev);
    int (*suspend)(struct device* dev);
    int (*resume)(struct device* dev);
    int (*shutdown)(struct device* dev);
    
    /* I/O operations */
    ssize_t (*read)(struct device* dev, void* buf, size_t count, off_t offset);
    ssize_t (*write)(struct device* dev, const void* buf, size_t count, off_t offset);
    int (*ioctl)(struct device* dev, unsigned int cmd, void* arg);
    int (*mmap)(struct device* dev, void** addr, size_t length, int prot);
    
    /* Interrupt handling */
    int (*irq_handler)(struct device* dev, int irq);
    void (*irq_enable)(struct device* dev);
    void (*irq_disable)(struct device* dev);
} device_ops_t;

/* Device structure */
typedef struct device {
    char name[DEV_NAME_LEN];
    device_type_t type;
    device_state_t state;
    
    device_id_t id;
    
    uint64_t base_addr;     // MMIO base address
    uint64_t io_base;       // I/O port base
    uint8_t irq;            // IRQ number
    
    struct driver* driver;  // Associated driver
    device_ops_t* ops;      // Device operations
    
    void* private_data;     // Driver private data
    
    struct device* parent;  // Parent device (bus controller)
    struct device* children; // Child devices
    struct device* next;    // Next sibling
    struct device* prev;    // Previous sibling
    
    /* Power management */
    int power_state;
    uint64_t power_usage;   // Power consumption in mW
    
    /* Statistics */
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t irq_count;
    uint64_t errors;
} device_t;

/* Driver structure */
typedef struct driver {
    char name[DEV_NAME_LEN];
    device_type_t type;
    
    const device_id_t* id_table;  // Supported device IDs
    int num_ids;
    
    int (*init)(void);
    void (*exit)(void);
    
    int (*probe)(device_t* dev);
    int (*remove)(device_t* dev);
    
    device_ops_t* ops;
    
    struct driver* next;
} driver_t;

/* Bus types */
typedef enum {
    BUS_TYPE_PCI = 0,
    BUS_TYPE_USB,
    BUS_TYPE_ISA,
    BUS_TYPE_PLATFORM,
    BUS_TYPE_VIRTUAL,
} bus_type_t;

/* Bus structure */
typedef struct bus {
    char name[DEV_NAME_LEN];
    bus_type_t type;
    
    device_t* devices;      // Devices on this bus
    int device_count;
    
    int (*scan)(struct bus* bus);
    int (*match)(struct bus* bus, device_t* dev, driver_t* drv);
    
    struct bus* next;
} bus_t;

/* Device manager functions */
void driver_init(void);
int driver_register(driver_t* driver);
void driver_unregister(driver_t* driver);
int driver_probe_device(device_t* dev);

/* Device management */
device_t* device_create(const char* name, device_type_t type, device_t* parent);
void device_destroy(device_t* dev);
int device_add(device_t* dev);
void device_remove(device_t* dev);
device_t* device_find(const char* name);
device_t* device_find_by_id(device_id_t* id);
device_t* device_find_by_type(device_type_t type);

/* Device I/O */
ssize_t device_read(device_t* dev, void* buf, size_t count, off_t offset);
ssize_t device_write(device_t* dev, const void* buf, size_t count, off_t offset);
int device_ioctl(device_t* dev, unsigned int cmd, void* arg);
int device_mmap(device_t* dev, void** addr, size_t length, int prot);

/* Power management */
int device_suspend(device_t* dev);
int device_resume(device_t* dev);
void device_shutdown(device_t* dev);

/* Interrupt management */
int device_request_irq(device_t* dev, int irq, void (*handler)(void));
void device_free_irq(device_t* dev, int irq);
void device_enable_irq(device_t* dev);
void device_disable_irq(device_t* dev);

/* DMA management */
void* dma_alloc_coherent(size_t size, uint64_t* phys_addr);
void dma_free_coherent(void* virt_addr, size_t size);
int dma_map_sg(device_t* dev, void* sg, int nents, int direction);
void dma_unmap_sg(device_t* dev, void* sg, int nents, int direction);

/* Device tree / hierarchy */
void device_attach(device_t* parent, device_t* child);
void device_detach(device_t* dev);
device_t* device_get_parent(device_t* dev);
device_t* device_get_child(device_t* dev, const char* name);
void device_for_each_child(device_t* parent, void (*fn)(device_t* dev));

/* Bus management */
int bus_register(bus_t* bus);
void bus_unregister(bus_t* bus);
int bus_scan(bus_t* bus);

/* Debugging */
void driver_dump_devices(void);
void driver_dump_drivers(void);
void driver_dump_stats(device_t* dev);

/* Device major/minor numbers (for filesystem interface) */
typedef uint64_t dev_t;
#define MAJOR(dev) ((dev) >> 8)
#define MINOR(dev) ((dev) & 0xFF)
#define MKDEV(major, minor) (((major) << 8) | (minor))

/* Device file interface */
device_t* device_from_dev_t(dev_t dev);
dev_t device_to_dev_t(device_t* dev);

#endif /* DRIVER_H */
