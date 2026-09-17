#ifndef FATFS_DISKIO_H
#define FATFS_DISKIO_H

/* Must be >= STORAGE_DISK_SECTORS (src/drivers/disk/storage.c) - the
 * size of the pre-existing catalog+data+metadata-ring region used for
 * the OS's own bookkeeping (settings, password hash, VFS catalog). The
 * FAT32 partition that the user-facing filesystem now lives on starts
 * immediately after that, so growing this kernel's own internal storage
 * scheme and growing the FAT32 partition can never overlap as long as
 * this stays >= that constant. Kept as a plain number (rather than
 * including storage.c's private header) to avoid coupling diskio.c,
 * which only needs to know where its own region starts, to storage.c's
 * internal layout details.
 */
#define FATFS_PARTITION_START_SECTOR 1048576ULL /* 512 MiB */

void fatfs_diskio_probe(void);

#endif /* FATFS_DISKIO_H */
