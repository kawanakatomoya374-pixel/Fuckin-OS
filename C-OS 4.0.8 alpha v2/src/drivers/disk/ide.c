#include "ide.h"
#include "crc32_common.h"
#include "io.h"
#include "memory.h"
#include "vga.h"
#include "gui.h"
#include "keyboard.h"
#include "rtc.h"
#include "serial.h"
#include "string.h"
#include <stddef.h>

#ifndef true
#define true 1
#define false 0
#endif

static ide_drive_t drives[2];

/* CRC32実装は crc32_common.c に一本化済み（旧: storage.c / storage_image.c と3重複していた）。 */
uint64_t crc32(const uint8_t* data, size_t length) {
    return cos_crc32(data, length);
}

static void ide_delay(void) {
    inb(IDE_PRIMARY_ALT_STATUS);
    inb(IDE_PRIMARY_ALT_STATUS);
    inb(IDE_PRIMARY_ALT_STATUS);
    inb(IDE_PRIMARY_ALT_STATUS);
}

void ide_wait_ready(void) {
    uint32_t timeout = 1000000U;
    while ((inb(IDE_PRIMARY_STATUS) & IDE_STATUS_BSY) && --timeout) {
        ;
    }
    if (timeout == 0U) {
        serial_puts("\[IDE\] wait_ready timeout\\n");
    }
}

bool ide_poll(void) {
    uint32_t timeout = 1000000U;
    while ((inb(IDE_PRIMARY_STATUS) & IDE_STATUS_BSY) && --timeout) {
        ;
    }
    if (timeout == 0U) {
        serial_puts("\[IDE\] poll busy timeout\\n");
        return false;
    }
    
    uint8_t status = inb(IDE_PRIMARY_STATUS);
    if (status & IDE_STATUS_ERR) return false;
    if (status & IDE_STATUS_DF) return false;
    
    timeout = 1000000U;
    while (!(status & IDE_STATUS_DRQ) && --timeout) {
        if (status & (IDE_STATUS_ERR | IDE_STATUS_DF)) return false;
        status = inb(IDE_PRIMARY_STATUS);
    }
    if (timeout == 0U) {
        serial_puts("\[IDE\] poll DRQ timeout\\n");
        return false;
    }
    
    return true;
}

static void ide_select_drive(uint8_t drive) {
    uint8_t drive_select = (drive == 0) ? IDE_DRIVE_MASTER : IDE_DRIVE_SLAVE;
    outb(IDE_PRIMARY_DRIVE_HEAD, drive_select | IDE_LBA_MODE);
    ide_delay();
}

bool ide_detect_drive(uint8_t drive) {
    if (drive > 1) return false;
    
    ide_select_drive(drive);
    
    // Send IDENTIFY command
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_IDENTIFY);
    ide_delay();
    
    // Check if drive exists
    uint8_t status = inb(IDE_PRIMARY_STATUS);
    if (status == 0) {
        drives[drive].present = false;
        return false;
    }
    
    ide_wait_ready();
    
    if (!ide_poll()) {
        uint8_t mid = inb(IDE_PRIMARY_LBA_MID);
        uint8_t high = inb(IDE_PRIMARY_LBA_HIGH);
        if (mid == 0x14 && high == 0xEB) {
            drives[drive].is_atapi = true;
            drives[drive].present = true;
        } else {
            drives[drive].present = false;
        }
        return drives[drive].present;
    }
    
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(IDE_PRIMARY_DATA);
    }
    
    drives[drive].present = true;
    drives[drive].is_atapi = false;
    
    drives[drive].size_sectors = ((uint64_t)identify_data[61] << 16) | identify_data[60];
    drives[drive].size_bytes = (uint64_t)drives[drive].size_sectors * SECTOR_SIZE;
    
    for (int i = 0; i < 20; i++) {
        drives[drive].model[i * 2] = (identify_data[27 + i] >> 8) & 0xFF;
        drives[drive].model[i * 2 + 1] = identify_data[27 + i] & 0xFF;
    }
    drives[drive].model[40] = '\0';
    
    for (int i = 39; i >= 0 && drives[drive].model[i] == ' '; i--) {
        drives[drive].model[i] = '\0';
    }
    return true;
}

void ide_init(void) {
    outb(IDE_PRIMARY_CONTROL, 0x02);
    
    drives[0].present = false;
        drives[0].present = false;
    drives[1].present = false;
    
        ide_detect_drive(0);
    ide_detect_drive(1);
}

bool ide_read_sectors(uint64_t lba, uint8_t count, uint8_t* buffer) {
    if (!drives[0].present || count == 0) return false;
    
    // Select master drive
    ide_select_drive(0);
    
        outb(IDE_PRIMARY_SECTOR_CNT, count);
    outb(IDE_PRIMARY_LBA_LOW, lba & 0xFF);
    outb(IDE_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(IDE_PRIMARY_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(IDE_PRIMARY_DRIVE_HEAD, IDE_DRIVE_MASTER | IDE_LBA_MODE | ((lba >> 24) & 0x0F));
    
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_READ_SECTORS);
    
    for (uint8_t sector = 0; sector < count; sector++) {
        if (!ide_poll()) return false;
        
        for (int round = 0; round < 256; round++) {
            uint64_t data = inw(IDE_PRIMARY_DATA);
            buffer[sector * SECTOR_SIZE + round * 2] = data & 0xFF;
            buffer[sector * SECTOR_SIZE + round * 2 + 1] = (data >> 8) & 0xFF;
        }
    }
    
    return true;
}

bool ide_write_sectors(uint64_t lba, uint8_t count, uint8_t* buffer) {
    if (!drives[0].present || count == 0) return false;
    
    ide_select_drive(0);
    
    outb(IDE_PRIMARY_SECTOR_CNT, count);
    outb(IDE_PRIMARY_LBA_LOW, lba & 0xFF);
    outb(IDE_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(IDE_PRIMARY_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(IDE_PRIMARY_DRIVE_HEAD, IDE_DRIVE_MASTER | IDE_LBA_MODE | ((lba >> 24) & 0x0F));
    
    outb(IDE_PRIMARY_COMMAND, IDE_CMD_WRITE_SECTORS);
    
    for (uint8_t sector = 0; sector < count; sector++) {
        if (!ide_poll()) return false;
        
        for (int i = 0; i < 256; i++) {
            uint64_t data = buffer[sector * SECTOR_SIZE + i * 2] |
                          (buffer[sector * SECTOR_SIZE + i * 2 + 1] << 8);
            outw(IDE_PRIMARY_DATA, data);
        }
        
        ide_delay();
    }
    
//     outb(IDE_PRIMARY_COMMAND, IDE_CMD_CACHE_FLUSH);
    // Flush disabled to avoid hangs on controllers that do not support it.
    ide_wait_ready();
    
    return true;
}

/* NOTE: storage_init()/storage_format() are implemented in storage.c, which
 * owns the real on-disk catalog/VFS layout used for persistence. This file
 * used to also define lightweight versions of these two functions that only
 * wrote a signature to sector 0, but that collided with storage.c's catalog
 * header (also at sector 0) and left storage.c's own "disk present" flag
 * permanently false - so file persistence silently never worked. Only the
 * low-level sector I/O helpers below (ide_read_sectors/ide_write_sectors)
 * are still used directly, by fat32.c and disk_compat.c. */

bool storage_write_settings(const system_settings_t* settings) {
    if (!settings) return false;
    
    system_settings_t settings_copy = *settings;
    settings_copy.signature = STORAGE_SIGNATURE;
    settings_copy.checksum = 0;
    settings_copy.checksum = crc32((uint8_t*)&settings_copy, offsetof(system_settings_t, checksum));
    
    uint8_t sector[SECTOR_SIZE];
    memcpy(sector, &settings_copy, sizeof(system_settings_t));
    
    return ide_write_sectors(SETTINGS_SECTOR, 1, sector);
}
