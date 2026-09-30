/**
 * driver_api.h - Unified Driver API
 * C-OS 4.0.8 alpha Driver Interface
 * 
 * Provides unified driver interface for all device types.
 * Drivers implement these operations and register with driver manager.
 */

#ifndef DRIVER_API_H
#define DRIVER_API_H

#include "types.h"
#include <stdint.h>

/* Driver Error Codes */
#define DRIVER_ERROR_OK 0
#define DRIVER_ERROR_INVALID_PARAM -1
#define DRIVER_ERROR_NOT_SUPPORTED -2
#define DRIVER_ERROR_HARDWARE -3
#define DRIVER_ERROR_TIMEOUT -4
#define DRIVER_ERROR_BUSY -5

/* Device Classes */
#define DEVICE_CLASS_UNKNOWN 0
#define DEVICE_CLASS_STORAGE 1
#define DEVICE_CLASS_NETWORK 2
#define DEVICE_CLASS_INPUT 3
#define DEVICE_CLASS_DISPLAY 4
#define DEVICE_CLASS_AUDIO 5
#define DEVICE_CLASS_TIMER 6

/* Device States */
#define DEVICE_STATE_UNINITIALIZED 0
#define DEVICE_STATE_INITIALIZING 1
#define DEVICE_STATE_ACTIVE 2
#define DEVICE_STATE_ERROR 3
#define DEVICE_STATE_REMOVED 4
#define DEVICE_STATE_SUSPENDED 5

/* Driver Capabilities */
#define DRIVER_CAP_READ 0x01
#define DRIVER_CAP_WRITE 0x02
#define DRIVER_CAP_IOCTL 0x04
#define DRIVER_CAP_MMAP 0x08
#define DRIVER_CAP_POLL 0x10
#define DRIVER_CAP_INTERRUPT 0x20

/* I/O Control Commands */
#define DRIVER_IOCTL_GET_INFO 0x1000
#define DRIVER_IOCTL_GET_STATUS 0x1001
#define DRIVER_IOCTL_SET_CONFIG 0x1002
#define DRIVER_IOCTL_RESET 0x1003
#define DRIVER_IOCTL_POWER_ON 0x1004
#define DRIVER_IOCTL_POWER_OFF 0x1005

/* Driver Operations Structure */
typedef struct driver_ops {
    /* Core operations */
    int (*probe)(void* device_data);
    int (*init)(void* device_data);
    int (*cleanup)(void* device_data);
    int (*suspend)(void* device_data);
    int (*resume)(void* device_data);
    
    /* I/O operations */
    int (*read)(void* device_data, void* buffer, size_t count);
    int (*write)(void* device_data, const void* buffer, size_t count);
    int (*ioctl)(void* device_data, uint64_t cmd, void* arg);
    
    /* Memory operations */
    int (*mmap)(void* device_data, virt_addr_t* vaddr, size_t size, uint64_t flags);
    int (*munmap)(void* device_data, virt_addr_t vaddr, size_t size);
    
    /* Interrupt operations */
    int (*enable_interrupts)(void* device_data);
    int (*disable_interrupts)(void* device_data);
    int (*get_interrupt_vector)(void* device_data);
    
    /* Power management */
    int (*set_power_state)(void* device_data, uint64_t state);
    int (*get_power_state)(void* device_data);
    
    /* Device information */
    int (*get_device_info)(void* device_data, char* name, uint64_t* capabilities);
    int (*get_statistics)(void* device_data, void* stats);
} driver_ops_t;

/* Driver Structure */
typedef struct driver {
    char name[32];
    char class[16];
    uint64_t version;
    uint64_t capabilities;
    uint64_t priority;
    driver_ops_t* ops;
    bool registered;
    struct driver* next;
} driver_t;

/* Device Structure */
typedef struct device {
    char name[32];
    uint64_t class;
    uint64_t id;
    uint64_t vendor_id;
    uint64_t device_id;
    uint64_t capabilities;
    uint8_t state;
    void* private_data;
    driver_t* driver;
    struct device* next;
} device_t;

/* Driver Manager Functions */
int driver_register(const char* name, const char* class, uint64_t version, 
                  driver_ops_t* ops);
int driver_unregister(const char* name);
int device_register(const char* name, uint64_t class, uint64_t vendor_id, 
                  uint64_t device_id, void* private_data);
int device_unregister(uint64_t device_id);
int device_probe_all(void);

/* Device Operations */
int device_read(uint64_t device_id, void* buffer, size_t count);
int device_write(uint64_t device_id, const void* buffer, size_t count);
int device_ioctl(uint64_t device_id, uint64_t cmd, void* arg);
int device_mmap(uint64_t device_id, virt_addr_t* vaddr, size_t size, uint64_t flags);
int device_munmap(uint64_t device_id, virt_addr_t vaddr, size_t size);

/* Device Information */
int device_get_info(uint64_t device_id, char* name, uint64_t* class, 
                   uint64_t* state, uint64_t* capabilities);
int device_get_driver_info(uint64_t device_id, char* driver_name, uint64_t* version);

/* Device Management */
int device_enable(uint64_t device_id);
int device_disable(uint64_t device_id);
int device_suspend(uint64_t device_id);
int device_resume(uint64_t device_id);
int device_reset(uint64_t device_id);

/* Power Management */
int device_set_power_state(uint64_t device_id, uint64_t state);
int device_get_power_state(uint64_t device_id);

/* Driver Manager Statistics */
typedef struct {
    uint64_t total_drivers;
    uint64_t total_devices;
    uint64_t active_devices;
    uint64_t failed_probes;
    uint64_t registration_count;
    uint64_t unregistration_count;
    uint64_t read_operations;
    uint64_t write_operations;
    uint64_t ioctl_operations;
} driver_stats_t;

/* Driver Manager Initialization */
int driver_manager_init(void);
int driver_manager_cleanup(void);
driver_stats_t* driver_get_stats(void);

/* Helper Macros */
#define DRIVER_READ(dev, buf, count) device_read(dev, buf, count)
#define DRIVER_WRITE(dev, buf, count) device_write(dev, buf, count)
#define DRIVER_IOCTL(dev, cmd, arg) device_ioctl(dev, cmd, arg)
#define DRIVER_MMAP(dev, vaddr, size, flags) device_mmap(dev, vaddr, size, flags)

/* Device Class Helpers */
#define DEVICE_CLASS_STORAGE_NAME "storage"
#define DEVICE_CLASS_NETWORK_NAME "network"
#define DEVICE_CLASS_INPUT_NAME "input"
#define DEVICE_CLASS_DISPLAY_NAME "display"
#define DEVICE_CLASS_AUDIO_NAME "audio"
#define DEVICE_CLASS_TIMER_NAME "timer"

/* Driver Version Helper */
#define DRIVER_VERSION(major, minor, patch) (((major) << 24) | ((minor) << 16) | (patch))

#endif /* DRIVER_API_H */
