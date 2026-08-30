/**
 * bios.h - C-OS 4.0.8 alpha BIOS Interface
 * Basic Input/Output System for hardware abstraction
 */

#ifndef BIOS_H
#define BIOS_H

#include "types.h"

/* BIOS Error Codes */
#define BIOS_OK             0
#define BIOS_ERR_MEMORY     1
#define BIOS_ERR_DISK       2
#define BIOS_ERR_VIDEO      3
#define BIOS_ERR_KEYBOARD   4
#define BIOS_ERR_MOUSE      5
#define BIOS_ERR_RTC        6
#define BIOS_ERR_SERIAL     7
#define BIOS_ERR_PARALLEL   8
#define BIOS_ERR_NETWORK    9
#define BIOS_ERR_POST       10
#define BIOS_ERR_UNKNOWN    99

/* Boot Device Types */
#define BOOT_DEVICE_FLOPPY  0x00
#define BOOT_DEVICE_HDD     0x80
#define BOOT_DEVICE_CDROM   0xE0
#define BOOT_DEVICE_USB     0xF0
#define BOOT_DEVICE_NETWORK 0xF1
#define BOOT_DEVICE_UNKNOWN 0xFF

/* Memory Map Entry */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint64_t type;
    uint64_t acpi_attrs;
} bios_memory_entry_t;

/* Memory Types */
#define MEMORY_TYPE_AVAILABLE   1
#define MEMORY_TYPE_RESERVED      2
#define MEMORY_TYPE_ACPI_RECLAIM  3
#define MEMORY_TYPE_ACPI_NVS      4
#define MEMORY_TYPE_BAD           5

/* Disk Info */
typedef struct {
    uint8_t  id;
    uint8_t  type;        /* 0x00=floppy, 0x80=hdd, 0xE0=cdrom */
    uint64_t cylinders;
    uint64_t heads;
    uint64_t sectors;
    uint64_t total_sectors;
    uint64_t bytes_per_sector;
    char     model[41];
    char     serial[20];
    uint64_t size_mb;
    int      present;
} bios_disk_info_t;

/* Video Mode */
typedef struct {
    uint64_t width;
    uint64_t height;
    uint8_t  bpp;
    uint64_t mode_num;
    uint64_t framebuffer;
    uint64_t pitch;
} bios_video_mode_t;

/* System Info */
typedef struct {
    char     vendor[16];
    char     product[16];
    char     version[16];
    char     serial[20];
    uint64_t memory_kb;
    uint8_t  boot_device;
    uint8_t  num_cpus;
    uint8_t  has_apic;
    uint8_t  has_acpi;
    char     cpu_vendor[16];
    char     cpu_model[32];
    uint64_t cpu_speed;
    char     bios_vendor[16];
    char     bios_version[16];
    char     bios_date[16];
    uint64_t total_memory_kb;
    uint64_t available_memory_kb;
} bios_system_info_t;

/* POST Status */
typedef struct {
    int memory_ok;
    int video_ok;
    int disk_ok;
    int keyboard_ok;
    int mouse_ok;
    int rtc_ok;
    int serial_ok;
    int cpu_test;
    int memory_test;
    int video_initialized;
    int keyboard_initialized;
    int rtc_initialized;
    int disk_initialized;
    int system_info_valid;
    bios_video_mode_t video_mode;
    int errors;
} bios_post_status_t;

/* BIOS Functions */

/* Initialization */
int bios_init(void);
void bios_shutdown(void);
void bios_reboot(void);

/* BOOT (Power-On Self Test) */
int bios_post(void);
void bios_post_display(const bios_post_status_t* status);

/* Memory */
int bios_detect_memory(void);
int bios_get_memory_map(bios_memory_entry_t* entries, int max_entries);
uint64_t bios_get_total_memory(void);
uint64_t bios_get_available_memory(void);

/* Disk */
void bios_detect_disks(void);
int bios_get_disk_info(uint8_t disk_id, bios_disk_info_t* info);
int bios_disk_read(uint8_t disk_id, uint64_t lba, uint8_t* buffer, uint64_t count);
int bios_disk_write(uint8_t disk_id, uint64_t lba, const uint8_t* buffer, uint64_t count);

/* Video */
int bios_detect_video(void);
int bios_get_video_modes(bios_video_mode_t* modes, int max_modes);
int bios_set_video_mode(uint64_t mode_num);
bios_video_mode_t* bios_get_current_video_mode(void);

/* System */
int bios_get_system_info(void);
void bios_get_system_info_copy(bios_system_info_t* out);
uint8_t bios_get_boot_device(void);
void bios_get_rtc_time(uint8_t* hour, uint8_t* min, uint8_t* sec);
void bios_get_rtc_date(uint64_t* year, uint8_t* month, uint8_t* day);

/* Configuration */
int bios_load_config(void);
int bios_save_config(void);
int bios_get_config(const char* key, char* value, int max_len);
int bios_set_config(const char* key, const char* value);

/* Boot */
void bios_boot_sequence(void);
int bios_boot_from_device(uint8_t device);

/* Error handling */
const char* bios_error_string(int error);
void bios_log_error(int error, const char* context);

/* Legacy BIOS Calls (for real hardware) */
#ifdef __BIOS_CALLS_ENABLED
int bios_int10_call(uint64_t ax, uint64_t bx, uint64_t cx, uint64_t dx);
int bios_int13_call(uint64_t ax, uint64_t bx, uint64_t cx, uint64_t dx, uint64_t es);
int bios_int15_call(uint64_t ax, uint64_t bx, uint64_t cx, uint64_t dx);
#endif

#endif
