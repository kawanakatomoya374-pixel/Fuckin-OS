/**
 * storage.c - Persistent filesystem storage backend
 * C-OS 4.0.8 alpha
 *
 * This backend keeps a RAM cache for fast access while persisting a compact
 * filesystem catalog and file payloads to a raw ATA disk image.
 *
 * Layout (512 MiB raw disk image):
 *   sector 0              : superblock
 *   sectors 1..64         : primary catalog copy
 *   sectors 65..128       : backup catalog copy
 *   sector 129            : password record
 *   sector 130            : reserved settings record
 *   data area             : file payload area
 *   final 4096 sectors    : metadata ring buffer
 */

#include "storage.h"
#include "memory.h"
#include "serial.h"
#include "crc32_common.h"
#include "rtc.h"
#include "io.h"
#include "sync.h"
#include <stddef.h>

/* The on-disk catalog (vfs_files[]), the sector bitmap, and the ATA
 * controller itself are all single shared resources - there's only
 * one disk, and this simple driver only ever has one command in
 * flight at a time anyway. Under real (cooperative or preemptive)
 * multitasking, more than one thread can now legitimately be calling
 * into here at once (e.g. the text editor's autosave thread saving
 * one file while the GUI thread reads another for the file manager).
 * Without a lock, two concurrent storage_write_file() calls could
 * both allocate the same free vfs_files[] slot, or interleave bitmap
 * updates and persist_catalog() writes, corrupting the on-disk
 * catalog. A single coarse mutex around the public entry points below
 * is the correct fix here - there's no meaningful parallelism to
 * preserve on a single ATA drive anyway. */
static mutex_t storage_mutex;
static bool storage_mutex_ready = false;
static void storage_ensure_mutex(void) {
    /* Best-effort init-once: the very first storage call in the whole
     * system happens during single-threaded early boot in practice, so
     * this isn't done with a fully atomic compare-and-swap. If that
     * assumption is ever violated (two threads both racing to make the
     * *very first* storage call ever), that specific narrow window
     * isn't covered - everything after mutex_init() has run is. */
    if (!storage_mutex_ready) {
        mutex_init(&storage_mutex);
        storage_mutex_ready = true;
    }
}

#ifndef true
#define true 1
#define false 0
#endif

/* Freestanding primitives -------------------------------------------------- */
void* memset(void* ptr, int value, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
char* strncpy(char* dest, const char* src, size_t n);

/* ATA PIO helpers ---------------------------------------------------------- */
#define ATA_PRIMARY_IO_BASE        0x1F0
#define ATA_PRIMARY_CTRL_BASE      0x3F6

#define ATA_REG_DATA               (ATA_PRIMARY_IO_BASE + 0)
#define ATA_REG_ERROR              (ATA_PRIMARY_IO_BASE + 1)
#define ATA_REG_SECTOR_COUNT       (ATA_PRIMARY_IO_BASE + 2)
#define ATA_REG_LBA0               (ATA_PRIMARY_IO_BASE + 3)
#define ATA_REG_LBA1               (ATA_PRIMARY_IO_BASE + 4)
#define ATA_REG_LBA2               (ATA_PRIMARY_IO_BASE + 5)
#define ATA_REG_DRIVE_HEAD         (ATA_PRIMARY_IO_BASE + 6)
#define ATA_REG_STATUS_COMMAND     (ATA_PRIMARY_IO_BASE + 7)
#define ATA_REG_ALT_STATUS_CTRL    (ATA_PRIMARY_CTRL_BASE + 0)

#define ATA_STATUS_BSY             0x80
#define ATA_STATUS_DRDY            0x40
#define ATA_STATUS_DRQ             0x08
#define ATA_STATUS_ERR             0x01
#define ATA_STATUS_DF              0x20

#define ATA_CMD_READ_SECTORS       0x20
#define ATA_CMD_WRITE_SECTORS      0x30
// cache flush disabled for qemu

#define ATA_MASTER                 0xE0
#define ATA_LBA                    0x40

#define SECTOR_SIZE                512u

/* Persistent layout -------------------------------------------------------- */
#define STORAGE_MAGIC              0x43535350464f5354ULL /* "STOFCSSC" style tag */
#define STORAGE_VERSION            4ULL

#define STORAGE_CATALOG_SECTOR      0u
#define STORAGE_CATALOG_SECTORS     64u
#define STORAGE_CATALOG_BACKUP      (1u + STORAGE_CATALOG_SECTORS)
#define STORAGE_PASSWORD_SECTOR     (STORAGE_CATALOG_BACKUP + STORAGE_CATALOG_SECTORS)
#define STORAGE_SETTINGS_SECTOR     (STORAGE_PASSWORD_SECTOR + 1u)
#define STORAGE_RESERVED_SECTOR     (STORAGE_SETTINGS_SECTOR + 1u)
#define STORAGE_META_RING_SECTORS   4096ULL
#define STORAGE_META_SLOT_SECTORS   64ULL
#define STORAGE_META_RING_START_SECTOR  (STORAGE_DISK_SECTORS - STORAGE_META_RING_SECTORS)
#define STORAGE_META_SLOT_COUNT     (STORAGE_META_RING_SECTORS / STORAGE_META_SLOT_SECTORS)
#define STORAGE_DATA_START_SECTOR   (STORAGE_RESERVED_SECTOR + 1u)
#define STORAGE_DATA_END_SECTOR     STORAGE_META_RING_START_SECTOR
#define STORAGE_DATA_SECTORS        (STORAGE_DATA_END_SECTOR - STORAGE_DATA_START_SECTOR)
#define STORAGE_META_SLOT_BYTES     (STORAGE_META_SLOT_SECTORS * SECTOR_SIZE)

/* The demo exposes a 512 MiB logical store backed by the raw disk image. */
#define VFS_MAX_FILES              128
#define VFS_MAX_NAME_LEN           128
/* Must comfortably exceed the worst-case fs.c snapshot size: up to
 * FS_MAX_ENTRIES (128) files, each up to FS_MAX_DATA (32KB) of data,
 * plus per-entry metadata - serialized as a single blob and saved
 * through this file table under one path (see fs_save_snapshot() /
 * FS_SNAPSHOT_PATH in fs.c). 128 * 32KB = 4MiB; round well up so a
 * change here doesn't need to track fs.c's constants exactly. */
#define VFS_MAX_FILE_SIZE          (8u * 1024u * 1024u)
#define VFS_TOTAL_SPACE            (512ULL * 1024ULL * 1024ULL)

#define STORAGE_DISK_SECTORS       1048576ULL   /* 512 MiB raw image */
#define STORAGE_BITMAP_BYTES       ((STORAGE_DATA_SECTORS + 7ULL) / 8ULL)

/* On-disk structures ------------------------------------------------------- */
typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint64_t version;
    uint64_t total_sectors;
    uint64_t catalog_sectors;
    uint64_t data_start_sector;
    uint64_t max_files;
    uint64_t used_files;
    uint64_t used_bytes;
    uint64_t checksum;
} storage_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  used;
    uint8_t  kind; /* 0=file, 1=dir */
    uint16_t reserved0;
    uint64_t first_sector;
    uint64_t sector_count;
    uint64_t size;
    uint64_t created_time;
    uint64_t modified_time;
    uint64_t accessed_time;
    char     path[VFS_MAX_NAME_LEN];
    uint64_t checksum;
} storage_disk_entry_t;

typedef struct __attribute__((packed)) {
    storage_header_t header;
    storage_disk_entry_t entries[VFS_MAX_FILES];
} storage_catalog_t;

typedef struct __attribute__((packed)) {
    uint64_t signature;
    uint8_t password_hash[32];
    uint8_t salt[16];
    uint64_t attempts;
    uint64_t locked;
    uint64_t checksum;
} password_record_t;

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint64_t version;
    uint64_t generation;
    uint64_t payload_size;
    uint64_t payload_checksum;
    uint64_t committed;
    uint64_t header_checksum;
} storage_meta_record_header_t;

typedef struct __attribute__((packed)) {
    storage_meta_record_header_t header;
    uint8_t payload[STORAGE_META_SLOT_BYTES - sizeof(storage_meta_record_header_t)];
} storage_meta_slot_t;

typedef struct __attribute__((packed)) {
    storage_catalog_t catalog;
    password_record_t password;
} storage_meta_payload_t;

/* In-memory file cache ----------------------------------------------------- */
typedef struct {
    bool used;
    bool is_dir;
    char path[VFS_MAX_NAME_LEN];
    uint64_t size;
    uint64_t first_sector;
    uint64_t sector_count;
    uint64_t created_time;
    uint64_t modified_time;
    uint64_t accessed_time;
} vfs_file_t;

static bool storage_initialized = false;
static bool storage_disk_present = false;
static uint64_t storage_disk_total_sectors = STORAGE_DISK_SECTORS;
static uint64_t g_real_disk_sectors = 0; /* true capacity from ATA IDENTIFY, kept for the FatFs partition */
static uint8_t storage_bitmap[STORAGE_BITMAP_BYTES];
static storage_catalog_t storage_catalog;
static vfs_file_t vfs_files[VFS_MAX_FILES];

/* Metadata ring buffer state. */
static uint64_t storage_meta_generation = 0;
static password_record_t storage_password_cache;
static bool storage_password_cache_valid = false;

/* RAM fallback password state. */
static bool password_present_ram = false;
static char password_ram[32] = {0};

/* CRC32 and password helpers ----------------------------------------------
 * 実装は crc32_common.c に一本化済み（旧: ide.c / storage_image.c と3重複していた）。
 * ここではその薄いラッパー名だけをこのファイル内の既存呼び出し規約に合わせて残す。
 */
#define crc32_calc(data, length) cos_crc32((data), (length))
#define crc32_skip(data, length, skip_off, skip_len) cos_crc32_skip((data), (length), (skip_off), (skip_len))

static void password_hash_compute(const char* password, const uint8_t salt[16], uint8_t out[32]) {
    memset(out, 0, 32);
    if (!password) return;

    int pass_len = 0;
    while (password[pass_len] && pass_len < 31) pass_len++;

    for (int round = 0; round < 256; ++round) {
        for (int i = 0; i < 32; ++i) {
            uint8_t p = (pass_len > 0) ? (uint8_t)password[(i + round) % pass_len] : 0u;
            uint8_t s = salt[i % 16];
            out[i] = (uint8_t)((out[i] * 33u) ^ (p + s + (uint8_t)round + (uint8_t)i));
        }
    }
}

static void storage_cache_password_record(const password_record_t* pwd) {
    if (!pwd) {
        storage_password_cache_valid = false;
        memset(&storage_password_cache, 0, sizeof(storage_password_cache));
        return;
    }
    storage_password_cache = *pwd;
    storage_password_cache_valid = true;
}

static bool storage_disk_io_read(uint64_t lba, uint8_t* buffer, size_t bytes);
static bool storage_disk_io_write(uint64_t lba, const uint8_t* buffer, size_t bytes);
static bool password_load_disk(password_record_t* pwd);
static bool write_file_to_disk(const vfs_file_t* file);

static bool storage_meta_build_payload(storage_meta_payload_t* payload) {
    if (!payload) return false;
    memset(payload, 0, sizeof(*payload));
    payload->catalog = storage_catalog;

    if (storage_password_cache_valid) {
        payload->password = storage_password_cache;
        return true;
    }

    if (storage_disk_present) {
        password_record_t pwd;
        if (password_load_disk(&pwd)) {
            storage_cache_password_record(&pwd);
            payload->password = pwd;
            return true;
        }
    }

    memset(&payload->password, 0, sizeof(payload->password));
    payload->password.signature = STORAGE_MAGIC;
    return true;
}

static bool storage_meta_write_snapshot(void) {
    if (!storage_disk_present) return true;

    storage_meta_payload_t payload;
    if (!storage_meta_build_payload(&payload)) return false;

    storage_meta_slot_t slot;
    memset(&slot, 0, sizeof(slot));

    uint64_t generation = storage_meta_generation + 1ULL;
    uint64_t slot_index = generation % STORAGE_META_SLOT_COUNT;
    uint64_t sector = STORAGE_META_RING_START_SECTOR + (slot_index * STORAGE_META_SLOT_SECTORS);

    slot.header.magic = STORAGE_MAGIC;
    slot.header.version = STORAGE_VERSION;
    slot.header.generation = generation;
    slot.header.payload_size = sizeof(storage_meta_payload_t);
    memcpy(slot.payload, &payload, sizeof(payload));
    slot.header.payload_checksum = crc32_calc(slot.payload, sizeof(payload));
    slot.header.committed = 0;
    slot.header.header_checksum = 0;
    slot.header.header_checksum = crc32_skip(&slot.header, sizeof(slot.header), offsetof(storage_meta_record_header_t, header_checksum), sizeof(uint64_t));

    if (!storage_disk_io_write(sector, (const uint8_t*)&slot, sizeof(slot))) return false;

    slot.header.committed = 1;
    slot.header.header_checksum = 0;
    slot.header.header_checksum = crc32_skip(&slot.header, sizeof(slot.header), offsetof(storage_meta_record_header_t, header_checksum), sizeof(uint64_t));
    if (!storage_disk_io_write(sector, (const uint8_t*)&slot, sizeof(slot))) return false;

    storage_meta_slot_t verify;
    memset(&verify, 0, sizeof(verify));
    if (!storage_disk_io_read(sector, (uint8_t*)&verify, sizeof(verify))) return false;
    if (verify.header.magic != STORAGE_MAGIC ||
        verify.header.version != STORAGE_VERSION ||
        verify.header.generation != generation ||
        verify.header.committed != 1ULL ||
        verify.header.payload_size != sizeof(storage_meta_payload_t)) {
        return false;
    }

    uint64_t saved_header = verify.header.header_checksum;
    uint64_t saved_payload = verify.header.payload_checksum;
    verify.header.header_checksum = 0;
    if (saved_header != crc32_skip(&verify.header, sizeof(verify.header), offsetof(storage_meta_record_header_t, header_checksum), sizeof(uint64_t))) return false;
    if (saved_payload != crc32_calc(verify.payload, sizeof(storage_meta_payload_t))) return false;

    storage_meta_generation = generation;
    return true;
}

static bool storage_meta_load_latest(storage_meta_payload_t* payload, uint64_t* generation_out) {
    if (!storage_disk_present || !payload) return false;

    bool found = false;
    uint64_t best_generation = 0;
    storage_meta_slot_t slot;

    for (uint64_t i = 0; i < STORAGE_META_SLOT_COUNT; ++i) {
        uint64_t sector = STORAGE_META_RING_START_SECTOR + (i * STORAGE_META_SLOT_SECTORS);
        memset(&slot, 0, sizeof(slot));
        if (!storage_disk_io_read(sector, (uint8_t*)&slot, sizeof(slot))) continue;
        if (slot.header.magic != STORAGE_MAGIC) continue;
        if (slot.header.version != STORAGE_VERSION) continue;
        if (slot.header.committed != 1ULL) continue;
        if (slot.header.payload_size != sizeof(storage_meta_payload_t)) continue;

        uint64_t header_sum = slot.header.header_checksum;
        uint64_t payload_sum = slot.header.payload_checksum;
        slot.header.header_checksum = 0;
        if (header_sum != crc32_skip(&slot.header, sizeof(slot.header), offsetof(storage_meta_record_header_t, header_checksum), sizeof(uint64_t))) continue;
        if (payload_sum != crc32_calc(slot.payload, sizeof(storage_meta_payload_t))) continue;

        if (!found || slot.header.generation >= best_generation) {
            memcpy(payload, slot.payload, sizeof(storage_meta_payload_t));
            best_generation = slot.header.generation;
            found = true;
        }
    }

    if (found) {
        if (generation_out) *generation_out = best_generation;
        storage_meta_generation = best_generation;
        return true;
    }

    return false;
}

static void storage_set_default_password_ram(void) {
    memset(password_ram, 0, sizeof(password_ram));
    password_present_ram = false;
    storage_cache_password_record(NULL);
}

static bool password_save_disk(const password_record_t* pwd);
static bool password_save_disk_verified(const password_record_t* pwd);
static bool password_load_disk(password_record_t* pwd);

static bool password_records_equal(const password_record_t* a, const password_record_t* b) {
    if (!a || !b) return false;
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    for (size_t i = 0; i < sizeof(*a); ++i) {
        if (pa[i] != pb[i]) return false;
    }
    return true;
}

static bool password_save_disk_verified(const password_record_t* pwd) {
    if (!password_save_disk(pwd)) return false;
    password_record_t verify;
    if (!password_load_disk(&verify)) return false;
    if (!password_records_equal(pwd, &verify)) return false;
    storage_cache_password_record(&verify);
    return true;
}

static bool storage_write_default_password_disk(void) {
    password_record_t pwd;
    memset(&pwd, 0, sizeof(pwd));
    pwd.signature = STORAGE_MAGIC;
    pwd.checksum = crc32_skip(&pwd, sizeof(pwd), offsetof(password_record_t, checksum), sizeof(uint64_t));
    if (!password_save_disk(&pwd)) return false;
    storage_cache_password_record(&pwd);
    return true;
}

/* ATA I/O ------------------------------------------------------------------ */
static void ata_io_delay(void) {
    (void)inb(ATA_REG_ALT_STATUS_CTRL);
    (void)inb(ATA_REG_ALT_STATUS_CTRL);
    (void)inb(ATA_REG_ALT_STATUS_CTRL);
    (void)inb(ATA_REG_ALT_STATUS_CTRL);
}

static bool ata_wait_ready(void) {
    for (uint32_t i = 0; i < 2000000u; ++i) {
        if (!(inb(ATA_REG_STATUS_COMMAND) & ATA_STATUS_BSY)) return true;
    }
    return false;
}

static bool ata_wait_drq(void) {
    for (uint32_t i = 0; i < 2000000u; ++i) {
        uint8_t status = inb(ATA_REG_STATUS_COMMAND);
        if (status & ATA_STATUS_ERR) return false;
        if (status & ATA_STATUS_DF) return false;
        if (status & ATA_STATUS_DRQ) return true;
    }
    return false;
}

static void ata_select_lba(uint64_t lba) {
    outb(ATA_REG_DRIVE_HEAD, (uint8_t)(ATA_MASTER | ATA_LBA | ((lba >> 24) & 0x0F)));
    outb(ATA_REG_SECTOR_COUNT, 1);
    outb(ATA_REG_LBA0, (uint8_t)(lba & 0xFFu));
    outb(ATA_REG_LBA1, (uint8_t)((lba >> 8) & 0xFFu));
    outb(ATA_REG_LBA2, (uint8_t)((lba >> 16) & 0xFFu));
}

static bool ata_read_sector(uint64_t lba, void* buffer) {
    if (!buffer) {
        serial_puts("[ATA] READ SECTOR lba=");
        serial_putdec(lba);
        serial_puts(" FAILED: null buffer\n");
        return false;
    }
    if (!ata_wait_ready()) {
        serial_puts("[ATA] READ SECTOR lba=");
        serial_putdec(lba);
        serial_puts(" FAILED: timeout waiting for BSY to clear (drive not responding)\n");
        return false;
    }
    ata_select_lba(lba);
    outb(ATA_REG_STATUS_COMMAND, ATA_CMD_READ_SECTORS);
    if (!ata_wait_drq()) {
        uint8_t status = inb(ATA_REG_STATUS_COMMAND);
        serial_puts("[ATA] READ SECTOR lba=");
        serial_putdec(lba);
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            serial_puts(" FAILED: drive reported ERR/DF, status=0x");
            serial_puthex(status);
            uint8_t err = inb(ATA_REG_ERROR);
            serial_puts(" error_reg=0x");
            serial_puthex(err);
        } else {
            serial_puts(" FAILED: timeout waiting for DRQ");
        }
        serial_puts("\n");
        return false;
    }

    uint16_t* out = (uint16_t*)buffer;
    for (int i = 0; i < 256; ++i) {
        out[i] = inw(ATA_REG_DATA);
    }
    return true;
}

static bool ata_write_sector(uint64_t lba, const void* buffer) {
    if (!buffer) {
        serial_puts("[ATA] WRITE SECTOR lba=");
        serial_putdec(lba);
        serial_puts(" FAILED: null buffer\n");
        return false;
    }
    if (!ata_wait_ready()) {
        serial_puts("[ATA] WRITE SECTOR lba=");
        serial_putdec(lba);
        serial_puts(" FAILED: timeout waiting for BSY to clear (drive not responding)\n");
        return false;
    }
    ata_select_lba(lba);
    outb(ATA_REG_STATUS_COMMAND, ATA_CMD_WRITE_SECTORS);
    if (!ata_wait_drq()) {
        uint8_t status = inb(ATA_REG_STATUS_COMMAND);
        serial_puts("[ATA] WRITE SECTOR lba=");
        serial_putdec(lba);
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            serial_puts(" FAILED: drive reported ERR/DF before data phase, status=0x");
            serial_puthex(status);
        } else {
            serial_puts(" FAILED: timeout waiting for DRQ");
        }
        serial_puts("\n");
        return false;
    }

    const uint16_t* in = (const uint16_t*)buffer;
    for (int i = 0; i < 256; ++i) {
        outw(ATA_REG_DATA, in[i]);
    }
    ata_io_delay();
// cache flush disabled for qemu
    if (!ata_wait_ready()) {
        serial_puts("[ATA] WRITE SECTOR lba=");
        serial_putdec(lba);
        serial_puts(" FAILED: timeout waiting for BSY to clear after data phase\n");
        return false;
    }

    /* ata_wait_ready() only confirms BSY cleared - it says nothing about
     * whether the write actually succeeded. Check ERR/DF on the status
     * register too, otherwise a failed sector write is silently reported
     * as success to every caller (persist_catalog_copy() happens to catch
     * this itself via its own read-back+checksum verify, but plain file
     * data writes through storage_disk_io_write() had no such check and
     * could lose data without anyone noticing). */
    uint8_t status = inb(ATA_REG_STATUS_COMMAND);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        serial_puts("[ATA] WRITE SECTOR lba=");
        serial_putdec(lba);
        serial_puts(" FAILED: drive reported ERR/DF after data phase, status=0x");
        serial_puthex(status);
        serial_puts("\n");
        return false;
    }
    return true;
}

/* IDENTIFY DEVICE (ATA_CMD_IDENTIFY = 0xEC).
 * Probes the primary master drive and returns true if a real ATA drive
 * responds. This is what decides whether storage.img is actually attached
 * and accessible. */
static bool ata_drive_present(uint64_t* out_sector_count) {
    /* Select master drive before issuing IDENTIFY. */
    outb(ATA_REG_DRIVE_HEAD, (uint8_t)(ATA_MASTER | ATA_LBA));
    ata_io_delay();

    /* Zero out the sector count register for required polling. */
    outb(ATA_REG_SECTOR_COUNT, 0);

    /* Issue IDENTIFY DEVICE. */
    outb(ATA_REG_STATUS_COMMAND, 0xECu);
    ata_io_delay();

    uint8_t status = inb(ATA_REG_STATUS_COMMAND);
    if (status == 0u) {
        /* No drive present on this port. */
        serial_puts("[ATA] IDENTIFY DEVICE FAILED: status=0x00, no drive present on primary port\n");
        return false;
    }

    /* Wait for BSY to clear. */
    for (uint32_t i = 0; i < 2000000u; ++i) {
        if (!(inb(ATA_REG_STATUS_COMMAND) & ATA_STATUS_BSY)) break;
    }

    status = inb(ATA_REG_STATUS_COMMAND);
    /* LBA mid / high should be 0 for non-ATAPI devices after IDENTIFY. */
    uint8_t mid = inb(ATA_REG_LBA1);
    uint8_t high = inb(ATA_REG_LBA2);
    if (mid != 0u || high != 0u) {
        /* ATAPI or SATA-signature device; we do not treat it as our IDE image. */
        serial_puts("[ATA] IDENTIFY DEVICE FAILED: non-ATA signature (LBA1=0x");
        serial_puthex(mid);
        serial_puts(" LBA2=0x");
        serial_puthex(high);
        serial_puts("), likely ATAPI/SATA - not usable for persistent storage\n");
        return false;
    }

    /* Wait for DRQ to assert (data ready). */
    uint32_t timeout = 2000000u;
    while (!(inb(ATA_REG_STATUS_COMMAND) & ATA_STATUS_DRQ) && --timeout) {
        uint8_t s = inb(ATA_REG_STATUS_COMMAND);
        if (s & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            uint8_t err = inb(ATA_REG_ERROR);
            serial_puts("[ATA] IDENTIFY DEVICE FAILED: drive reported ERR/DF, status=0x");
            serial_puthex(s);
            serial_puts(" error_reg=0x");
            serial_puthex(err);
            serial_puts("\n");
            return false;
        }
    }
    if (timeout == 0u) {
        serial_puts("[ATA] IDENTIFY DEVICE FAILED: timeout waiting for DRQ\n");
        return false;
    }

    /* Read the 256-word IDENTIFY response. Words 60-61 hold the LBA28
     * total addressable sector count (word 60 = low 16 bits, word 61 =
     * high 16 bits) - this is the drive's *actual* reported capacity,
     * which previously went completely unused: the code just drained
     * these words and assumed a fixed 512MiB disk regardless of what
     * was really attached. */
    uint16_t identify[256];
    for (int i = 0; i < 256; ++i) {
        identify[i] = inw(ATA_REG_DATA);
    }

    uint32_t lba28_sectors = (uint32_t)identify[60] | ((uint32_t)identify[61] << 16);
    serial_puts("[ATA] IDENTIFY DEVICE OK: reported ");
    serial_putdec(lba28_sectors);
    serial_puts(" sectors (");
    serial_putdec((uint64_t)lba28_sectors / 2048ULL);
    serial_puts(" MiB)\n");

    if (out_sector_count) {
        *out_sector_count = (uint64_t)lba28_sectors;
    }
    return true;
}

static bool storage_disk_io_read(uint64_t lba, uint8_t* buffer, size_t bytes) {
    if (!buffer || bytes == 0) return true;
    uint8_t sector[SECTOR_SIZE];
    while (bytes > 0) {
        if (lba >= storage_disk_total_sectors) return false;
        if (!ata_read_sector(lba++, sector)) return false;
        size_t chunk = (bytes > SECTOR_SIZE) ? SECTOR_SIZE : bytes;
        memcpy(buffer, sector, chunk);
        buffer += chunk;
        bytes -= chunk;
    }
    return true;
}

static bool storage_disk_io_write(uint64_t lba, const uint8_t* buffer, size_t bytes) {
    if (!buffer || bytes == 0) return true;
    uint8_t sector[SECTOR_SIZE];
    while (bytes > 0) {
        if (lba >= storage_disk_total_sectors) return false;
        memset(sector, 0, sizeof(sector));
        size_t chunk = (bytes > SECTOR_SIZE) ? SECTOR_SIZE : bytes;
        memcpy(sector, buffer, chunk);
        if (!ata_write_sector(lba++, sector)) return false;
        buffer += chunk;
        bytes -= chunk;
    }
    return true;
}

/* Bitmap and catalog helpers ----------------------------------------------- */
static void bitmap_clear_all(void) {
    memset(storage_bitmap, 0, sizeof(storage_bitmap));
}

static void bitmap_mark_range(uint64_t first_sector, uint64_t sector_count) {
    if (sector_count == 0) return;
    if (first_sector < STORAGE_DATA_START_SECTOR) return;
    uint64_t start = first_sector - STORAGE_DATA_START_SECTOR;
    for (uint64_t i = 0; i < sector_count; ++i) {
        uint64_t bit = start + i;
        uint64_t byte_index = bit / 8ULL;
        uint8_t bit_mask = (uint8_t)(1u << (bit % 8ULL));
        if (byte_index < sizeof(storage_bitmap)) {
            storage_bitmap[byte_index] |= bit_mask;
        }
    }
}

static void bitmap_clear_range(uint64_t first_sector, uint64_t sector_count) {
    if (sector_count == 0) return;
    if (first_sector < STORAGE_DATA_START_SECTOR) return;
    uint64_t start = first_sector - STORAGE_DATA_START_SECTOR;
    for (uint64_t i = 0; i < sector_count; ++i) {
        uint64_t bit = start + i;
        uint64_t byte_index = bit / 8ULL;
        uint8_t bit_mask = (uint8_t)(1u << (bit % 8ULL));
        if (byte_index < sizeof(storage_bitmap)) {
            storage_bitmap[byte_index] &= (uint8_t)~bit_mask;
        }
    }
}

static bool bitmap_test(uint64_t data_sector_index) {
    uint64_t byte_index = data_sector_index / 8ULL;
    uint8_t bit_mask = (uint8_t)(1u << (data_sector_index % 8ULL));
    if (byte_index >= sizeof(storage_bitmap)) return true;
    return (storage_bitmap[byte_index] & bit_mask) != 0;
}

static uint64_t vfs_used_space(void) {
    uint64_t used = 0;
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (vfs_files[i].used && !vfs_files[i].is_dir) used += vfs_files[i].size;
    }
    return used;
}

static int vfs_find_file(const char* path) {
    if (!path || !path[0]) return -1;
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (vfs_files[i].used && strcmp(vfs_files[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

static int vfs_alloc_file(void) {
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used) return i;
    }
    return -1;
}

static uint64_t vfs_required_sectors(uint64_t size) {
    if (size == 0) return 0;
    return (size + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
}

static int vfs_find_free_run(uint64_t needed) {
    if (needed == 0) return -1;
    uint64_t run = 0;
    uint64_t run_start = 0;
    uint64_t data_sectors = STORAGE_DATA_SECTORS;
    for (uint64_t i = 0; i < data_sectors; ++i) {
        if (!bitmap_test(i)) {
            if (run == 0) run_start = i;
            run++;
            if (run >= needed) return (int)(run_start + STORAGE_DATA_START_SECTOR);
        } else {
            run = 0;
        }
    }
    return -1;
}

static void vfs_reset_ram_state(void) {
    memset(vfs_files, 0, sizeof(vfs_files));
    bitmap_clear_all();
    password_present_ram = false;
    memset(password_ram, 0, sizeof(password_ram));
}

static uint64_t entry_checksum(const storage_disk_entry_t* e) {
    return crc32_skip(e, sizeof(*e), offsetof(storage_disk_entry_t, checksum), sizeof(uint64_t));
}

static uint64_t catalog_checksum(const storage_catalog_t* c) {
    return crc32_skip(c, sizeof(*c), offsetof(storage_catalog_t, header) + offsetof(storage_header_t, checksum), sizeof(uint64_t));
}

static void entry_to_disk(const vfs_file_t* src, storage_disk_entry_t* dst) {
    memset(dst, 0, sizeof(*dst));
    dst->used = src->used ? 1u : 0u;
    dst->kind = src->is_dir ? 1u : 0u;
    dst->first_sector = src->first_sector;
    dst->sector_count = src->sector_count;
    dst->size = src->size;
    dst->created_time = src->created_time;
    dst->modified_time = src->modified_time;
    dst->accessed_time = src->accessed_time;
    strncpy(dst->path, src->path, VFS_MAX_NAME_LEN - 1);
    dst->path[VFS_MAX_NAME_LEN - 1] = '\0';
    dst->checksum = entry_checksum(dst);
}

static bool entry_from_disk(const storage_disk_entry_t* src, vfs_file_t* dst) {
    if (!src || !dst) return false;
    if (!src->used) return false;
    if (src->path[0] == '\0') return false;
    if (src->kind > 1u) return false;
    if (src->kind == 0u && src->size > VFS_MAX_FILE_SIZE) return false;

    uint64_t chk = src->checksum;
    storage_disk_entry_t tmp;
    memcpy(&tmp, src, sizeof(tmp));
    tmp.checksum = 0;
    if (chk != entry_checksum(&tmp)) return false;

    memset(dst, 0, sizeof(*dst));
    dst->used = true;
    dst->is_dir = (src->kind != 0u);
    strncpy(dst->path, src->path, VFS_MAX_NAME_LEN - 1);
    dst->path[VFS_MAX_NAME_LEN - 1] = '\0';
    dst->first_sector = src->first_sector;
    dst->sector_count = src->sector_count;
    dst->size = src->size;
    dst->created_time = src->created_time;
    dst->modified_time = src->modified_time;
    dst->accessed_time = src->accessed_time;
    return true;
}

static bool persist_catalog(void);
static bool load_catalog(void);

static bool write_file_to_disk_with_data(const vfs_file_t* file, const void* data) {
    if (!storage_disk_present || !file || !data) return true;
    if (file->is_dir) return true;
    if (file->size == 0) return true;
    return storage_disk_io_write(file->first_sector, (const uint8_t*)data, (size_t)file->size);
}

static bool write_file_to_disk(const vfs_file_t* file) {
    if (!storage_disk_present || !file) return false;
    if (file->is_dir || file->size == 0) return true;

    int idx = vfs_find_file(file->path);
    if (idx < 0) return false;

    const vfs_file_t* src = &vfs_files[idx];
    if (src->is_dir || src->size == 0) return true;

    uint8_t* tmp = (uint8_t*)kmalloc((size_t)src->size);
    if (!tmp) return false;

    bool ok = storage_disk_io_read(src->first_sector, tmp, (size_t)src->size);
    if (ok) {
        ok = storage_disk_io_write(file->first_sector, tmp, (size_t)src->size);
    }

    kfree(tmp);
    return ok;
}

static bool vfs_store_file_payload(vfs_file_t* file) {
    if (!file) return false;
    if (file->is_dir) return true;

    uint64_t sectors = vfs_required_sectors(file->size);
    if (sectors == 0) {
        file->first_sector = 0;
        file->sector_count = 0;
        return true;
    }

    int first = vfs_find_free_run(sectors);
    if (first < 0) {
        /* Compact the filesystem and retry by repacking every live file. */
        if (!storage_disk_present) return false;

        uint64_t new_first[VFS_MAX_FILES];
        uint64_t new_count[VFS_MAX_FILES];
        memset(new_first, 0, sizeof(new_first));
        memset(new_count, 0, sizeof(new_count));

        uint64_t next_sector = STORAGE_DATA_START_SECTOR;
        for (int i = 0; i < VFS_MAX_FILES; ++i) {
            if (!vfs_files[i].used || vfs_files[i].is_dir) continue;
            uint64_t need = vfs_required_sectors(vfs_files[i].size);
            if (need == 0) continue;

            vfs_file_t temp = vfs_files[i];
            temp.first_sector = next_sector;
            temp.sector_count = need;
            if (!write_file_to_disk(&temp)) return false;

            new_first[i] = next_sector;
            new_count[i] = need;
            next_sector += need;
        }

        bitmap_clear_all();
        for (int i = 0; i < VFS_MAX_FILES; ++i) {
            if (!vfs_files[i].used || vfs_files[i].is_dir) continue;
            vfs_files[i].first_sector = new_first[i];
            vfs_files[i].sector_count = new_count[i];
            if (new_count[i] > 0) bitmap_mark_range(new_first[i], new_count[i]);
        }

        return true;
    }

    file->first_sector = (uint64_t)first;
    file->sector_count = sectors;
    bitmap_mark_range(file->first_sector, file->sector_count);
    return write_file_to_disk(file);
}

static bool rebuild_bitmap_from_files(void) {
    bitmap_clear_all();
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (vfs_files[i].used && !vfs_files[i].is_dir && vfs_files[i].sector_count > 0) {
            if (vfs_files[i].first_sector < STORAGE_DATA_START_SECTOR) return false;
            uint64_t rel = vfs_files[i].first_sector - STORAGE_DATA_START_SECTOR;
            if (rel + vfs_files[i].sector_count > STORAGE_DATA_SECTORS) return false;
            bitmap_mark_range(vfs_files[i].first_sector, vfs_files[i].sector_count);
        }
    }
    return true;
}

static bool load_catalog_from_sector(uint64_t sector, storage_catalog_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!storage_disk_io_read(sector, (uint8_t*)out, sizeof(*out))) return false;
    if (out->header.magic != STORAGE_MAGIC) return false;
    if (out->header.version != STORAGE_VERSION) return false;
    if (out->header.total_sectors != STORAGE_DISK_SECTORS) return false;
    if (out->header.catalog_sectors != STORAGE_CATALOG_SECTORS) return false;
    if (out->header.data_start_sector != STORAGE_DATA_START_SECTOR) return false;
    if (out->header.max_files != VFS_MAX_FILES) return false;

    uint64_t saved = out->header.checksum;
    out->header.checksum = 0;
    if (saved != catalog_checksum(out)) return false;
    out->header.checksum = saved;
    return true;
}

static bool persist_catalog_copy(uint64_t sector, const storage_catalog_t* catalog) {
    if (!storage_disk_io_write(sector, (const uint8_t*)catalog, sizeof(*catalog))) {
        serial_puts("[VFS] persist_catalog_copy: write to sector ");
        serial_putdec(sector);
        serial_puts(" failed\n");
        return false;
    }

    storage_catalog_t verify;
    memset(&verify, 0, sizeof(verify));
    if (!storage_disk_io_read(sector, (uint8_t*)&verify, sizeof(verify))) {
        serial_puts("[VFS] persist_catalog_copy: read-back from sector ");
        serial_putdec(sector);
        serial_puts(" failed\n");
        return false;
    }
    if (verify.header.magic != catalog->header.magic ||
        verify.header.version != catalog->header.version ||
        verify.header.total_sectors != catalog->header.total_sectors ||
        verify.header.catalog_sectors != catalog->header.catalog_sectors ||
        verify.header.data_start_sector != catalog->header.data_start_sector ||
        verify.header.max_files != catalog->header.max_files) {
        serial_puts("[VFS] persist_catalog_copy: read-back from sector ");
        serial_putdec(sector);
        serial_puts(" does not match what was written (fields differ) - write likely silently corrupted\n");
        return false;
    }

    uint64_t saved = verify.header.checksum;
    verify.header.checksum = 0;
    if (saved != catalog_checksum(&verify)) {
        serial_puts("[VFS] persist_catalog_copy: checksum mismatch on read-back from sector ");
        serial_putdec(sector);
        serial_puts("\n");
        return false;
    }
    return true;
}

static void refresh_catalog_header(void) {
    memset(&storage_catalog.header, 0, sizeof(storage_catalog.header));
    memset(storage_catalog.entries, 0, sizeof(storage_catalog.entries));
    storage_catalog.header.magic = STORAGE_MAGIC;
    storage_catalog.header.version = STORAGE_VERSION;
    storage_catalog.header.total_sectors = STORAGE_DISK_SECTORS;
    storage_catalog.header.catalog_sectors = STORAGE_CATALOG_SECTORS;
    storage_catalog.header.data_start_sector = STORAGE_DATA_START_SECTOR;
    storage_catalog.header.max_files = VFS_MAX_FILES;
    storage_catalog.header.used_files = 0;
    storage_catalog.header.used_bytes = 0;

    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used) continue;
        storage_catalog.header.used_files++;
        if (!vfs_files[i].is_dir) storage_catalog.header.used_bytes += vfs_files[i].size;
        entry_to_disk(&vfs_files[i], &storage_catalog.entries[i]);
    }

    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        storage_catalog.entries[i].checksum = entry_checksum(&storage_catalog.entries[i]);
    }
    storage_catalog.header.checksum = 0;
    storage_catalog.header.checksum = catalog_checksum(&storage_catalog);
}

static bool persist_catalog(void) {
    if (!storage_disk_present) return true;
    refresh_catalog_header();

    /* Write backup first, then primary. */
    if (!persist_catalog_copy(STORAGE_CATALOG_BACKUP, &storage_catalog)) return false;
    if (!persist_catalog_copy(STORAGE_CATALOG_SECTOR, &storage_catalog)) return false;
    if (!storage_meta_write_snapshot()) return false;
    return true;
}

static bool load_catalog(void) {
    storage_meta_payload_t payload;
    bool have_meta = storage_meta_load_latest(&payload, &storage_meta_generation);

    storage_catalog_t primary;
    storage_catalog_t backup;
    bool have_primary = load_catalog_from_sector(STORAGE_CATALOG_SECTOR, &primary);
    bool have_backup = load_catalog_from_sector(STORAGE_CATALOG_BACKUP, &backup);

    if (have_meta) {
        storage_catalog = payload.catalog;
        storage_cache_password_record(&payload.password);
    } else if (have_primary) {
        storage_catalog = primary;
    } else if (have_backup) {
        storage_catalog = backup;
    } else {
        return false;
    }

    vfs_reset_ram_state();

    /* Convert catalog entries to RAM cache; invalid/overlapping entries are dropped. */
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        storage_disk_entry_t* src = &storage_catalog.entries[i];
        if (!src->used) continue;
        vfs_file_t temp;
        if (!entry_from_disk(src, &temp)) {
            memset(src, 0, sizeof(*src));
            continue;
        }
        if (!temp.is_dir && temp.sector_count > 0) {
            if (temp.first_sector < STORAGE_DATA_START_SECTOR) {
                memset(src, 0, sizeof(*src));
                continue;
            }
            uint64_t rel = temp.first_sector - STORAGE_DATA_START_SECTOR;
            if (rel + temp.sector_count > STORAGE_DATA_SECTORS) {
                memset(src, 0, sizeof(*src));
                continue;
            }
            bool overlap = false;
            for (uint64_t s = 0; s < temp.sector_count; ++s) {
                if (bitmap_test(rel + s)) {
                    overlap = true;
                    break;
                }
            }
            if (overlap) {
                memset(src, 0, sizeof(*src));
                continue;
            }
            bitmap_mark_range(temp.first_sector, temp.sector_count);
            // No longer loading data into RAM at startup
            // Data will be read from disk on demand
        }
        vfs_files[i] = temp;
    }

    rebuild_bitmap_from_files();

    if (have_meta && storage_disk_present) {
        (void)password_save_disk_verified(&payload.password);
    }

    return true;
}

static bool storage_format_internal(void) {
    vfs_reset_ram_state();
    storage_meta_generation = 0;
    storage_cache_password_record(NULL);

    if (storage_disk_present) {
        memset(&storage_catalog, 0, sizeof(storage_catalog));
        storage_catalog.header.magic = STORAGE_MAGIC;
        storage_catalog.header.version = STORAGE_VERSION;
        storage_catalog.header.total_sectors = STORAGE_DISK_SECTORS;
        storage_catalog.header.catalog_sectors = STORAGE_CATALOG_SECTORS;
        storage_catalog.header.data_start_sector = STORAGE_DATA_START_SECTOR;
        storage_catalog.header.max_files = VFS_MAX_FILES;
        storage_catalog.header.used_files = 0;
        storage_catalog.header.used_bytes = 0;
        storage_catalog.header.checksum = catalog_checksum(&storage_catalog);
        if (!persist_catalog_copy(STORAGE_CATALOG_SECTOR, &storage_catalog)) {
            serial_puts("[VFS] format step FAILED: writing primary catalog copy at sector ");
            serial_putdec((uint64_t)STORAGE_CATALOG_SECTOR);
            serial_puts(" (see [ATA] WRITE SECTOR log above for the underlying reason)\n");
            return false;
        }
        if (!persist_catalog_copy(STORAGE_CATALOG_BACKUP, &storage_catalog)) {
            serial_puts("[VFS] format step FAILED: writing backup catalog copy at sector ");
            serial_putdec((uint64_t)STORAGE_CATALOG_BACKUP);
            serial_puts(" (see [ATA] WRITE SECTOR log above for the underlying reason)\n");
            return false;
        }

        if (!storage_write_default_password_disk()) {
            serial_puts("[VFS] WARNING: No default password is provisioned\n");
        }
        if (!storage_meta_write_snapshot()) {
            serial_puts("[VFS] format step FAILED: writing metadata ring snapshot"
                        " (see [ATA] WRITE SECTOR log above for the underlying reason)\n");
            return false;
        }
    } else {
        storage_set_default_password_ram();
    }

    return true;
}

/* Public API --------------------------------------------------------------- */

bool storage_init(void) {
    if (storage_initialized) return true;

    /* Probe the primary ATA drive. This was previously never called, which
     * left storage_disk_present permanently false and made every disk read
     * (storage_read_file) fail and every disk write silently no-op - so
     * nothing ever actually persisted across reboots. */
    uint64_t real_sectors = 0;
    storage_disk_present = ata_drive_present(&real_sectors);
    g_real_disk_sectors = real_sectors;

    if (storage_disk_present) {
        /* The on-disk layout (catalog, password sector, and especially
         * the metadata ring, which is placed near the very end of the
         * STORAGE_DISK_SECTORS range) assumes a disk at least that big.
         * Previously storage_disk_total_sectors was hard-coded to that
         * same constant regardless of the real attached disk's size, so
         * a smaller disk would have every write near the top of that
         * range silently rejected by the drive (LBA out of range) and
         * the whole persistence layer would fall back to RAM-only with
         * no indication of why. Check the real capacity up front and
         * say so plainly instead. */
        if (real_sectors < STORAGE_DISK_SECTORS) {
            serial_puts("[VFS] WARNING: attached disk is smaller than required (");
            serial_putdec(real_sectors);
            serial_puts(" sectors, need at least ");
            serial_putdec((uint64_t)STORAGE_DISK_SECTORS);
            serial_puts("); persistent storage needs a disk of at least ");
            serial_putdec((uint64_t)(STORAGE_DISK_SECTORS / 2048ULL));
            serial_puts(" MiB. Falling back to RAM-only.\n");
            storage_disk_present = false;
        } else {
            storage_disk_total_sectors = STORAGE_DISK_SECTORS;
        }
    }

    if (storage_disk_present) {
        if (load_catalog()) {
            serial_puts("[VFS] Persistent storage catalog loaded\n");
        } else {
            /* First boot on a blank/unformatted disk image - no valid
             * catalog yet, so create one now instead of staying read-only. */
            serial_puts("[VFS] No existing catalog found; formatting persistent storage\n");
            if (!storage_format_internal()) {
                serial_puts("[VFS] WARNING: failed to initialize persistent storage; falling back to RAM-only\n");
                storage_disk_present = false;
            }
        }
    } else {
        serial_puts("[VFS] No ATA disk detected; running storage in RAM-only mode\n");
    }

    storage_initialized = true;
    return true;
}

bool storage_format(void) {
    if (!storage_initialized) {
        if (!storage_init()) return false;
    }

    serial_puts("[VFS] Formatting storage backend\n");
    return storage_format_internal();
}

/* Exposed for the FatFs partition (see src/kernel/fatfs_diskio.c), which
 * lives in a separate region of the same physical disk, past the end of
 * this file's own 512MiB catalog+data+metadata-ring scheme. These go
 * straight to the ATA layer with a bounds check against the drive's real
 * reported capacity (ata_real_disk_sectors) rather than
 * storage_disk_total_sectors, which is fixed at this scheme's own
 * (smaller, original) size and would incorrectly reject any sector in
 * the FatFs partition. */
bool storage_raw_sector_read(uint64_t lba, void* buf) {
    if (lba >= g_real_disk_sectors) return false;
    return ata_read_sector(lba, buf);
}

bool storage_raw_sector_write(uint64_t lba, const void* buf) {
    if (lba >= g_real_disk_sectors) return false;
    return ata_write_sector(lba, buf);
}

uint64_t storage_get_real_disk_sectors(void) {
    return g_real_disk_sectors;
}

bool storage_write_file(const char* filename, const void* data, uint64_t size) {
    if (!storage_initialized) {
        if (!storage_init()) return false;
    }
    if (!filename || !filename[0]) return false;
    if (!data && size > 0) return false;
    if (size > VFS_MAX_FILE_SIZE) return false;
    if (!storage_disk_present) return false;

    storage_ensure_mutex();
    mutex_lock(&storage_mutex);

    int idx = vfs_find_file(filename);
    bool is_new = false;
    if (idx < 0) {
        idx = vfs_alloc_file();
        if (idx < 0) { mutex_unlock(&storage_mutex); return false; }
        is_new = true;
        memset(&vfs_files[idx], 0, sizeof(vfs_file_t));
        vfs_files[idx].used = true;
        vfs_files[idx].is_dir = false;
        strncpy(vfs_files[idx].path, filename, VFS_MAX_NAME_LEN - 1);
        vfs_files[idx].path[VFS_MAX_NAME_LEN - 1] = '\0';
        vfs_files[idx].created_time = rtc_get_time();
    }

    vfs_files[idx].size = size;
    vfs_files[idx].modified_time = rtc_get_time();
    vfs_files[idx].accessed_time = vfs_files[idx].modified_time;

    if (!is_new && vfs_files[idx].sector_count > 0) {
        bitmap_clear_range(vfs_files[idx].first_sector, vfs_files[idx].sector_count);
        vfs_files[idx].first_sector = 0;
        vfs_files[idx].sector_count = 0;
    }

    if (size == 0) {
        bool ok = persist_catalog();
        mutex_unlock(&storage_mutex);
        return ok;
    }

    uint64_t sectors = vfs_required_sectors(size);
    int first = vfs_find_free_run(sectors);
    if (first < 0) { mutex_unlock(&storage_mutex); return false; }

    vfs_files[idx].first_sector = (uint64_t)first;
    vfs_files[idx].sector_count = sectors;
    bitmap_mark_range(vfs_files[idx].first_sector, vfs_files[idx].sector_count);

    if (!write_file_to_disk_with_data(&vfs_files[idx], data)) {
        mutex_unlock(&storage_mutex);
        return false;
    }

    bool ok = persist_catalog();
    mutex_unlock(&storage_mutex);
    return ok;
}

bool storage_read_file(const char* filename, void* buffer, uint64_t buffer_size, uint64_t* out_size) {
    if (!storage_initialized) {
        if (!storage_init()) return false;
    }
    if (!filename || !buffer) return false;

    storage_ensure_mutex();
    mutex_lock(&storage_mutex);

    int idx = vfs_find_file(filename);
    if (idx < 0) { mutex_unlock(&storage_mutex); return false; }

    uint64_t to_read = vfs_files[idx].size;
    if (to_read > buffer_size) to_read = buffer_size;

    if (to_read > 0) {
        if (storage_disk_present) {
            if (!storage_disk_io_read(vfs_files[idx].first_sector, (uint8_t*)buffer, (size_t)to_read)) {
                mutex_unlock(&storage_mutex);
                return false;
            }
        } else {
            mutex_unlock(&storage_mutex);
            return false;
        }
    }

    vfs_files[idx].accessed_time = rtc_get_time();
    if (out_size) *out_size = to_read;
    mutex_unlock(&storage_mutex);
    return true;
}

bool storage_delete_file(const char* filename) {
    if (!storage_initialized) {
        if (!storage_init()) return false;
    }
    if (!filename || !filename[0]) return false;

    storage_ensure_mutex();
    mutex_lock(&storage_mutex);

    int idx = vfs_find_file(filename);
    if (idx < 0) { mutex_unlock(&storage_mutex); return false; }

    if (storage_disk_present && vfs_files[idx].sector_count > 0) {
        bitmap_clear_range(vfs_files[idx].first_sector, vfs_files[idx].sector_count);
    }

    memset(&vfs_files[idx], 0, sizeof(vfs_file_t));
    bool ok = persist_catalog();
    mutex_unlock(&storage_mutex);
    return ok;
}

bool storage_file_exists(const char* filename) {
    if (!storage_initialized) {
        if (!storage_init()) return false;
    }
    if (!filename) return false;
    storage_ensure_mutex();
    mutex_lock(&storage_mutex);
    bool exists = vfs_find_file(filename) >= 0;
    mutex_unlock(&storage_mutex);
    return exists;
}

uint64_t storage_list_files(char* filenames, uint64_t max_files, uint64_t max_name_len) {
    if (!storage_initialized) {
        if (!storage_init()) return 0;
    }
    if (!filenames || max_files == 0 || max_name_len == 0) return 0;

    storage_ensure_mutex();
    mutex_lock(&storage_mutex);

    uint64_t count = 0;
    for (int i = 0; i < VFS_MAX_FILES && count < max_files; ++i) {
        if (!vfs_files[i].used) continue;
        char* slot = filenames + (count * max_name_len);
        memset(slot, 0, (size_t)max_name_len);
        strncpy(slot, vfs_files[i].path, (size_t)max_name_len - 1);
        slot[max_name_len - 1] = '\0';
        count++;
    }
    mutex_unlock(&storage_mutex);
    return count;
}

uint64_t storage_get_free_space(void) {
    uint64_t used = vfs_used_space();
    return (used >= VFS_TOTAL_SPACE) ? 0 : (VFS_TOTAL_SPACE - used);
}

uint64_t storage_get_used_space(void) {
    return vfs_used_space();
}

uint64_t storage_get_total_space(void) {
    return VFS_TOTAL_SPACE;
}

/* Password / settings persistence ----------------------------------------- */
static bool password_load_disk(password_record_t* pwd) {
    if (!pwd) return false;
    if (!storage_disk_present) return false;
    if (!storage_disk_io_read(STORAGE_PASSWORD_SECTOR, (uint8_t*)pwd, sizeof(*pwd))) return false;
    if (pwd->signature != STORAGE_MAGIC) return false;

    uint64_t saved = pwd->checksum;
    pwd->checksum = 0;
    uint64_t calc = crc32_skip(pwd, sizeof(*pwd), offsetof(password_record_t, checksum), sizeof(uint64_t));
    pwd->checksum = saved;
    if (saved != calc) return false;
    storage_cache_password_record(pwd);
    return true;
}

static bool password_save_disk(const password_record_t* pwd) {
    if (!pwd) return false;
    if (!storage_disk_present) return false;
    if (!storage_disk_io_write(STORAGE_PASSWORD_SECTOR, (const uint8_t*)pwd, sizeof(*pwd))) return false;
    return true;
}

/* When there is no persistent ATA disk attached (RAM-only mode, e.g. no
 * disk image given to the emulator), password state cannot be read from or
 * written to disk. Previously storage_has_password()/storage_verify_password()/
 * storage_set_password() all bailed out with `if (!storage_disk_present)
 * return false;`, which meant:
 *   - storage_has_password() always reported "no password"
 *   - password_screen_enhanced_show() then tried storage_set_password("1234")
 *     to provision a default password, which ALSO always failed
 *   - the login screen therefore could never actually authenticate anyone
 *     and boot always fell through the "did not complete" fallback path
 * even though a perfectly good in-RAM cache (storage_password_cache /
 * storage_password_cache_valid) already existed and is populated by
 * storage_cache_password_record(). We now read/write that RAM cache when
 * there is no disk, so login (and the demo default password) works the
 * same way whether or not a persistent disk is attached. */
bool storage_has_password(void) {
    if (!storage_disk_present) {
        if (!storage_password_cache_valid) return false;
        for (int i = 0; i < 32; ++i) {
            if (storage_password_cache.password_hash[i] != 0) return true;
        }
        return false;
    }
    password_record_t pwd;
    if (!password_load_disk(&pwd)) return false;
    for (int i = 0; i < 32; ++i) {
        if (pwd.password_hash[i] != 0) return true;
    }
    return false;
}

bool storage_verify_password(const char* password) {
    if (!password) return false;

    if (!storage_disk_present) {
        if (!storage_has_password()) {
            return password[0] == '\0';
        }
        uint8_t computed_hash[32];
        password_hash_compute(password, storage_password_cache.salt, computed_hash);
        for (int i = 0; i < 32; ++i) {
            if (computed_hash[i] != storage_password_cache.password_hash[i]) {
                return false;
            }
        }
        return true;
    }

    if (!storage_has_password()) {
        return password[0] == '\0';
    }

    password_record_t pwd;
    if (!password_load_disk(&pwd)) return false;

    uint8_t computed_hash[32];
    password_hash_compute(password, pwd.salt, computed_hash);
    for (int i = 0; i < 32; ++i) {
        if (computed_hash[i] != pwd.password_hash[i]) {
            pwd.attempts++;
            pwd.checksum = 0;
            pwd.checksum = crc32_skip(&pwd, sizeof(pwd), offsetof(password_record_t, checksum), sizeof(uint64_t));
            (void)password_save_disk(&pwd);
            return false;
        }
    }

    pwd.checksum = 0;
    pwd.checksum = crc32_skip(&pwd, sizeof(pwd), offsetof(password_record_t, checksum), sizeof(uint64_t));
    (void)password_save_disk(&pwd);
    return true;
}

bool storage_set_password(const char* password) {
    if (!password) return false;

    password_record_t pwd;
    memset(&pwd, 0, sizeof(pwd));
    pwd.signature = STORAGE_MAGIC;
    pwd.attempts = 0;
    pwd.locked = 0;

    uint8_t salt_seed = (uint8_t)(rtc_get_time() & 0xFFu);
    for (int i = 0; i < 16; ++i) {
        pwd.salt[i] = (uint8_t)(salt_seed ^ (uint8_t)i ^ (uint8_t)(i * 17));
    }

    password_hash_compute(password, pwd.salt, pwd.password_hash);
    pwd.checksum = 0;
    pwd.checksum = crc32_skip(&pwd, sizeof(pwd), offsetof(password_record_t, checksum), sizeof(uint64_t));

    if (!storage_disk_present) {
        /* No disk to persist to - keep the record in the RAM cache so
         * has/verify_password still work for the rest of this boot. */
        storage_cache_password_record(&pwd);
        return true;
    }

    if (!password_save_disk_verified(&pwd)) return false;
    if (!storage_meta_write_snapshot()) return false;
    return true;
}

bool storage_clear_password(void) {
    if (!storage_disk_present) return false;

    password_record_t pwd;
    memset(&pwd, 0, sizeof(pwd));
    pwd.signature = STORAGE_MAGIC;
    pwd.checksum = crc32_skip(&pwd, sizeof(pwd), offsetof(password_record_t, checksum), sizeof(uint64_t));
    if (!password_save_disk_verified(&pwd)) return false;
    if (!storage_meta_write_snapshot()) return false;
    return true;
}
