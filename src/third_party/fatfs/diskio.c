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
extern bool storage_raw_sectors_read(uint64_t lba, uint32_t count, void* buf);
extern bool storage_raw_sectors_write(uint64_t lba, uint32_t count, const void* buf);
extern bool storage_raw_flush(void);
extern bool storage_raw_trim(uint64_t lba, uint64_t count);

static bool s_fatfs_disk_present = false;
/* drive 1: the boot partition (ESP) of the disk, set by the kernel updater (cos_bootupd.c); nothing outside it can be reached through this drive */
extern bool storage_abs_read(uint64_t lba, uint32_t count, void* buf);
extern bool storage_abs_write(uint64_t lba, uint32_t count, const void* buf);
static uint64_t s_esp_first, s_esp_count;
void fatfs_diskio_set_esp(uint64_t first, uint64_t count) { s_esp_first = first; s_esp_count = count; }
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
    if (pdrv == 1) return s_esp_count ? 0 : STA_NOINIT;
    if (pdrv != 0) return STA_NOINIT;
    return s_fatfs_disk_present ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv == 1) return s_esp_count ? 0 : STA_NOINIT;
    if (pdrv != 0) return STA_NOINIT;
    if (!s_fatfs_disk_present) fatfs_diskio_probe();
    return s_fatfs_disk_present ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv == 1) { if (!s_esp_count || (uint64_t)sector + count > s_esp_count) return RES_PARERR; return storage_abs_read(s_esp_first + sector, count, buff) ? RES_OK : RES_ERROR; }
    if (pdrv != 0 || !s_fatfs_disk_present) return RES_NOTRDY;
    if ((uint64_t)sector + count > s_fatfs_partition_sectors) return RES_PARERR;
    return storage_raw_sectors_read(FATFS_PARTITION_START_SECTOR + sector, count, buff) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    if (pdrv == 1) { if (!s_esp_count || (uint64_t)sector + count > s_esp_count) return RES_PARERR; return storage_abs_write(s_esp_first + sector, count, buff) ? RES_OK : RES_ERROR; }
    if (pdrv != 0 || !s_fatfs_disk_present) return RES_NOTRDY;
    if ((uint64_t)sector + count > s_fatfs_partition_sectors) return RES_PARERR;
    return storage_raw_sectors_write(FATFS_PARTITION_START_SECTOR + sector, count, buff) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    if (pdrv == 1) {
        if (!s_esp_count) return RES_NOTRDY;
        switch (cmd) {
            case CTRL_SYNC: return storage_raw_flush() ? RES_OK : RES_ERROR;
            case GET_SECTOR_COUNT: *(LBA_t*)buff = (LBA_t)s_esp_count; return RES_OK;
            case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
            case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
            default: return RES_PARERR;
        }
    }
    if (pdrv != 0 || !s_fatfs_disk_present) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            /* FatFs calls this when it has finished a group of FAT / directory / data writes: make them durable. */
            return storage_raw_flush() ? RES_OK : RES_ERROR;
        case GET_SECTOR_COUNT:
            *(LBA_t*)buff = (LBA_t)s_fatfs_partition_sectors;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD*)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD*)buff = 2048; /* 1 MiB: align the FAT32 data area to SSD erase blocks / 4K physical sectors */
            return RES_OK;
        case CTRL_TRIM: {
            const LBA_t* range = (const LBA_t*)buff;     /* [0] first sector, [1] last sector, inclusive */
            (void)storage_raw_trim(FATFS_PARTITION_START_SECTOR + range[0] + 0, (uint64_t)(range[1] - range[0] + 1));
            return RES_OK;                               /* TRIM is advisory: never fail the file operation over it */
        }
        default:
            return RES_PARERR;
    }
}
