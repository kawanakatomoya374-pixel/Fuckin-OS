#ifndef IDE_H
#define IDE_H

#include "types.h"

#define IDE_PRIMARY_DATA        0x1F0
#define IDE_PRIMARY_ERROR       0x1F1
#define IDE_PRIMARY_SECTOR_CNT  0x1F2
#define IDE_PRIMARY_LBA_LOW     0x1F3
#define IDE_PRIMARY_LBA_MID     0x1F4
#define IDE_PRIMARY_LBA_HIGH    0x1F5
#define IDE_PRIMARY_DRIVE_HEAD  0x1F6
#define IDE_PRIMARY_STATUS      0x1F7
#define IDE_PRIMARY_COMMAND     0x1F7
#define IDE_PRIMARY_ALT_STATUS  0x3F6
#define IDE_PRIMARY_CONTROL     0x3F6

#define IDE_CMD_READ_SECTORS    0x20
#define IDE_CMD_WRITE_SECTORS   0x30
#define IDE_CMD_IDENTIFY        0xEC
#define IDE_CMD_CACHE_FLUSH     0xE7

#define IDE_STATUS_ERR          0x01
#define IDE_STATUS_DRQ          0x08
#define IDE_STATUS_SRV          0x10
#define IDE_STATUS_DF           0x20
#define IDE_STATUS_RDY          0x40
#define IDE_STATUS_BSY          0x80

#define IDE_DRIVE_MASTER        0xA0
#define IDE_DRIVE_SLAVE         0xB0
#define IDE_LBA_MODE            0x40

#define SECTOR_SIZE             512
#define MAX_SECTORS             256
#define STORAGE_SIGNATURE       0x434F5300
#define CONFIG_SECTOR           0
#define SETTINGS_SECTOR         1
#define PASSWORD_SECTOR         2
#define USER_DATA_START         8

typedef struct {
    uint64_t signature;
    uint64_t version;
    uint64_t screen_width;
    uint64_t screen_height;
    uint64_t color_depth;
    uint64_t refresh_rate;
    uint64_t font_scale;
    uint64_t font_resolution;
    uint64_t font_smoothing;
    uint64_t theme_id;
    uint64_t accent_color;
    uint64_t transparency;
    uint64_t mouse_sensitivity;
    uint64_t mouse_acceleration;
    uint64_t mouse_trails;
    uint64_t sound_enabled;
    uint64_t volume_level;
    uint64_t auto_save;
    uint64_t screen_timeout;
    uint64_t require_password;
    uint64_t checksum;
} __attribute__((packed)) system_settings_t;

typedef struct {
    uint64_t signature;
    uint8_t password_hash[32];
    uint8_t salt[16];
    uint64_t attempts;
    uint64_t locked;
    uint64_t checksum;
} __attribute__((packed)) password_data_t;

typedef struct {
    bool present;
    bool is_atapi;
    uint64_t size_sectors;
    uint64_t size_bytes;
    char model[41];
    char serial[21];
} ide_drive_t;

void ide_init(void);
bool ide_detect_drive(uint8_t drive);
bool ide_read_sectors(uint64_t lba, uint8_t count, uint8_t* buffer);
bool ide_write_sectors(uint64_t lba, uint8_t count, uint8_t* buffer);
void ide_wait_ready(void);
bool ide_poll(void);

bool storage_init(void);
bool storage_format(void);
bool storage_read_settings(system_settings_t* settings);
bool storage_write_settings(const system_settings_t* settings);
bool storage_set_password(const char* password);
bool storage_verify_password(const char* password);
bool storage_has_password(void);
bool storage_clear_password(void);
uint64_t storage_get_free_sectors(void);

void show_password_screen(void);
bool password_screen_loop(void);

#endif
