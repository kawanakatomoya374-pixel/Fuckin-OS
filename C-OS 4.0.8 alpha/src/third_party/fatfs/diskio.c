/**
 * diskio.c - FatFs <-> C-OS ATA disk glue
 *
 * FatFs sector 0 is mapped to FATFS_PARTITION_START_SECTOR on the real
 * disk (see fatfs_diskio.h) - a dedicated region past the end of
 * storage.c's own catalog/data/metadata-ring scheme, so this filesystem
 * and that one can never collide on the same physical sectors.
 */
#include "ff.h"
#include "diskio.h"
#include "fatfs_diskio.h"
#include "serial.h"

extern bool storage_raw_sector_read(uint64_t lba, void* buf);
extern bool storage_raw_sector_write(uint64_t lba, const void* buf);
extern uint64_t storage_get_real_disk_sectors(void);

static bool s_fatfs_disk_present = false;
static uint64_t s_fatfs_partition_sectors = 0;

void fatfs_diskio_probe(void) {
    uint64_t real_sectors = storage_get_real_disk_sectors();
    if (real_sectors <= FATFS_PARTITION_START_SECTOR) {
        serial_puts("[FATFS] WARNING: disk too small for the FAT32 partition"
                    " (need at least ");
        serial_putdec(FATFS_PARTITION_START_SECTOR / 2048ULL);
        serial_puts(" MiB before it even starts) - FAT32 unavailable\n");
        s_fatfs_disk_present = false;
        return;
    }
    s_fatfs_partition_sectors = real_sectors - FATFS_PARTITION_START_SECTOR;
    s_fatfs_disk_present = true;
    serial_puts("[FATFS] Partition region: ");
    serial_putdec(s_fatfs_partition_sectors / 2048ULL);
    serial_puts(" MiB available (starting at sector ");
    serial_putdec(FATFS_PARTITION_START_SECTOR);
    serial_puts(")\n");
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return s_fatfs_disk_present ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    if (!s_fatfs_disk_present) fatfs_diskio_probe();
    return s_fatfs_disk_present ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !s_fatfs_disk_present) return RES_NOTRDY;
    if ((uint64_t)sector + count > s_fatfs_partition_sectors) return RES_PARERR;
    for (UINT i = 0; i < count; ++i) {
        if (!storage_raw_sector_read(FATFS_PARTITION_START_SECTOR + sector + i, buff + i * 512)) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || !s_fatfs_disk_present) return RES_NOTRDY;
    if ((uint64_t)sector + count > s_fatfs_partition_sectors) return RES_PARERR;
    for (UINT i = 0; i < count; ++i) {
        if (!storage_raw_sector_write(FATFS_PARTITION_START_SECTOR + sector + i, buff + i * 512)) {
            return RES_ERROR;
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv != 0 || !s_fatfs_disk_present) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t*)buff = (LBA_t)s_fatfs_partition_sectors;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD*)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = 1; /* no meaningful erase-block concept over ATA PIO */
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
