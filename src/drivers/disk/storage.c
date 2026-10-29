/**
 * storage.c - Persistent filesystem storage backend
 * C-OS 4.1.0
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
#include "timer.h"
#include "crc32_common.h"
#include "rtc.h"
#include "io.h"
#include "sync.h"
#include "blk.h"
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
int memcmp(const void* a, const void* b, size_t n);


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
static uint64_t g_real_disk_sectors = 0; /* usable capacity: the C-OS data partition when the disk is partitioned, else the whole drive */
static uint64_t g_abs_disk_sectors = 0;  /* whole-drive capacity from ATA IDENTIFY */
static uint64_t g_part_base = 0;         /* first LBA of the C-OS data partition (0 on a raw, unpartitioned data disk) */
static uint8_t storage_bitmap[STORAGE_BITMAP_BYTES];
/* Extents freed since the last durable catalog commit. The on-disk catalog (and the older metadata-ring generation a
 * torn commit falls back to) may still point at them, so they must not be handed out again - and so overwritten - until a
 * commit that no longer references them is complete. This is what makes rewriting a file crash-safe: the new contents go
 * to a fresh extent, the old extent stays intact until the catalog switches over. */
static uint8_t storage_bitmap_pending[STORAGE_BITMAP_BYTES];
static uint64_t storage_pending_sectors;
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

/* ---- Metadata ring position hint --------------------------------------
 *
 * Why this exists: storage_meta_load_latest() below finds the newest
 * committed slot by reading and checksumming EVERY slot in the ring - all
 * STORAGE_META_SLOT_COUNT of them, STORAGE_META_RING_SECTORS (4096)
 * sectors total - on every single boot, regardless of whether anything
 * changed. Measured on a real boot: 4000 sector reads, 2207ms, entirely
 * before "[VFS] Persistent storage catalog loaded" printed - a multi-
 * second synchronous stall on the GUI/boot thread that reproduced
 * whether or not the disk needed formatting. This is the O(n) ring scan
 * that was chased down as the actual cause of the reported storage-
 * attached freeze.
 *
 * The ring is written append-only (storage_meta_write_snapshot() always
 * writes to generation % SLOT_COUNT and increments), so on any boot after
 * the first, the answer is almost always "the slot the previous session
 * last wrote to" - a single number that can be cached instead of
 * rediscovered by scanning everything.
 *
 * This hint is written to STORAGE_RESERVED_SECTOR, which nothing else in
 * this file uses (confirmed: it exists only in the sector-arithmetic
 * chain that derives STORAGE_DATA_START_SECTOR, with no reads or writes
 * of its own before this). Using an already-reserved sector rather than
 * changing the catalog layout means the on-disk format for the parts
 * other code depends on is untouched.
 *
 * Written on every successful commit (cheap: one extra sector write, and
 * writes happen when the user actually changes something, not per frame).
 * Read once at boot. If the hint is missing, stale, or fails its own
 * checksum - first boot ever, an image from before this existed, or
 * corruption - fall back to the original full scan, so this is a fast
 * path with a correct slow path behind it, never a new failure mode. */
typedef struct {
    uint64_t magic;
    uint64_t slot_index;
    uint64_t generation;
    uint64_t checksum;
} storage_meta_hint_t;

static bool storage_meta_hint_write(uint64_t slot_index, uint64_t generation) {
    storage_meta_hint_t hint;
    hint.magic = STORAGE_MAGIC;
    hint.slot_index = slot_index;
    hint.generation = generation;
    hint.checksum = 0;
    hint.checksum = crc32_calc(&hint, sizeof(hint));

    uint8_t sector_buf[SECTOR_SIZE];
    memset(sector_buf, 0, sizeof(sector_buf));
    memcpy(sector_buf, &hint, sizeof(hint));
    /* Best-effort: if this write fails, boot simply falls back to the full
     * scan next time, so a failure here must never be treated as fatal to
     * the commit that is actually being persisted (the metadata slot
     * write above this call is the one that has to succeed). */
    (void)storage_disk_io_write(STORAGE_RESERVED_SECTOR, sector_buf, sizeof(sector_buf));
    return true;
}

/* Reads and validates the hinted slot directly. Returns true only if the
 * hint pointed at a genuinely valid, committed slot - anything else (bad
 * checksum, wrong magic, slot no longer valid) returns false so the
 * caller falls back to the full scan rather than trusting a stale hint. */
static bool storage_meta_hint_try_load(storage_meta_payload_t* payload, uint64_t* generation_out) {
    uint8_t sector_buf[SECTOR_SIZE];
    if (!storage_disk_io_read(STORAGE_RESERVED_SECTOR, sector_buf, sizeof(sector_buf))) return false;

    storage_meta_hint_t hint;
    memcpy(&hint, sector_buf, sizeof(hint));
    if (hint.magic != STORAGE_MAGIC) return false;
    uint64_t saved_checksum = hint.checksum;
    hint.checksum = 0;
    if (saved_checksum != crc32_calc(&hint, sizeof(hint))) return false;
    if (hint.slot_index >= STORAGE_META_SLOT_COUNT) return false;

    uint64_t sector = STORAGE_META_RING_START_SECTOR + (hint.slot_index * STORAGE_META_SLOT_SECTORS);
    storage_meta_slot_t slot;
    memset(&slot, 0, sizeof(slot));
    if (!storage_disk_io_read(sector, (uint8_t*)&slot, sizeof(slot))) return false;
    if (slot.header.magic != STORAGE_MAGIC) return false;
    if (slot.header.version != STORAGE_VERSION) return false;
    if (slot.header.committed != 1ULL) return false;
    if (slot.header.payload_size != sizeof(storage_meta_payload_t)) return false;
    if (slot.header.generation != hint.generation) return false; /* stale hint */

    uint64_t header_sum = slot.header.header_checksum;
    uint64_t payload_sum = slot.header.payload_checksum;
    slot.header.header_checksum = 0;
    if (header_sum != crc32_skip(&slot.header, sizeof(slot.header),
                                 offsetof(storage_meta_record_header_t, header_checksum),
                                 sizeof(uint64_t))) return false;
    if (payload_sum != crc32_calc(slot.payload, sizeof(storage_meta_payload_t))) return false;

    memcpy(payload, slot.payload, sizeof(storage_meta_payload_t));
    if (generation_out) *generation_out = hint.generation;
    storage_meta_generation = hint.generation;
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
    (void)storage_meta_hint_write(slot_index, generation);
    return true;
}

static bool storage_meta_load_latest(storage_meta_payload_t* payload, uint64_t* generation_out) {
    /* Fast path: trust the hint if it checks out completely. Falls through
     * to the original full ring scan on any failure - missing hint, bad
     * checksum, or a generation mismatch - so correctness never depends
     * on the hint being right, only boot speed does. */
    {
        uint64_t t0 = get_timer_ticks();
        bool ok = storage_meta_hint_try_load(payload, generation_out);
        uint64_t ms = get_timer_ticks() - t0;
        serial_puts("[VFSPERF] hint fast-path ");
        serial_puts(ok ? "HIT" : "MISS");
        serial_puts(" ms="); serial_putdec(ms); serial_puts("\n");
        if (ok) return true;
    }
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
/* The block device everything below sits on: legacy IDE, AHCI (SATA) or NVMe - see blk.h. */
static bool ata_read_sector(uint64_t lba, void *buffer);
static bool ata_write_sector(uint64_t lba, const void *buffer);
static bool ata_read_sectors(uint64_t lba, uint32_t count, void *buffer);
static bool ata_write_sectors(uint64_t lba, uint32_t count, const void *buffer);
static bool ata_flush(void);
void ata_report_cache_stats(void);
static blk_dev_t *g_blk;
static bool blk_read_retry(uint64_t lba, uint32_t count, void *buf);
static bool blk_write_retry(uint64_t lba, uint32_t count, const void *buf);

static bool storage_disk_io_read(uint64_t lba, uint8_t* buffer, size_t bytes) {
    if (!buffer || bytes == 0) return true;
    size_t full = bytes / SECTOR_SIZE;
    if (lba + full + ((bytes % SECTOR_SIZE) ? 1u : 0u) > storage_disk_total_sectors) return false;
    if (full) {
        if (!ata_read_sectors(g_part_base + lba, (uint32_t)full, buffer)) return false;
        lba += full; buffer += full * SECTOR_SIZE; bytes -= full * SECTOR_SIZE;
    }
    if (bytes) {
        uint8_t sector[SECTOR_SIZE];
        if (!ata_read_sector(g_part_base + lba, sector)) return false;
        memcpy(buffer, sector, bytes);
    }
    return true;
}

static bool storage_disk_io_write(uint64_t lba, const uint8_t* buffer, size_t bytes) {
    if (!buffer || bytes == 0) return true;
    size_t full = bytes / SECTOR_SIZE;
    if (lba + full + ((bytes % SECTOR_SIZE) ? 1u : 0u) > storage_disk_total_sectors) return false;
    if (full) {
        if (!ata_write_sectors(g_part_base + lba, (uint32_t)full, buffer)) return false;
        lba += full; buffer += full * SECTOR_SIZE; bytes -= full * SECTOR_SIZE;
    }
    if (bytes) {
        uint8_t sector[SECTOR_SIZE];
        memset(sector, 0, sizeof(sector));
        memcpy(sector, buffer, bytes);
        if (!ata_write_sector(g_part_base + lba, sector)) return false;
    }
    return true;
}

/* Bitmap and catalog helpers ----------------------------------------------- */
static void bitmap_clear_all(void) {
    memset(storage_bitmap, 0, sizeof(storage_bitmap));
}

static void pending_mark_range(uint64_t first_sector, uint64_t sector_count) {
    if (sector_count == 0 || first_sector < STORAGE_DATA_START_SECTOR) return;
    uint64_t start = first_sector - STORAGE_DATA_START_SECTOR;
    for (uint64_t i = 0; i < sector_count; ++i) {
        uint64_t bit = start + i, byte_index = bit / 8ULL;
        if (byte_index < sizeof(storage_bitmap_pending)) storage_bitmap_pending[byte_index] |= (uint8_t)(1u << (bit % 8ULL));
    }
    storage_pending_sectors += sector_count;
}

static void pending_release_all(void) {
    if (storage_pending_sectors) memset(storage_bitmap_pending, 0, sizeof(storage_bitmap_pending));
    storage_pending_sectors = 0;
}

static bool pending_test(uint64_t data_sector_index) {
    uint64_t byte_index = data_sector_index / 8ULL;
    if (byte_index >= sizeof(storage_bitmap_pending)) return false;
    return (storage_bitmap_pending[byte_index] & (uint8_t)(1u << (data_sector_index % 8ULL))) != 0;
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
        if (!bitmap_test(i) && !pending_test(i)) {
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
    pending_release_all();
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

/* ---- Deferred catalog persistence ---------------------------------------
 *
 * Why this exists: persist_catalog() rewrites BOTH the backup and primary
 *64-sector catalog and commits a full 64-sector metadata-ring slot (write
 * uncommitted, write committed, read back to verify) on every single call
 * - roughly 320 sectors of synchronous PIO, measured at 123ms for a real
 * gui_close_window() call in this session, ~170ms when the boot-time hint
 * fast path is not yet warm.
 *
 * storage_write_file() called persist_catalog() synchronously and
 * unconditionally after every write, and gui_save_window_state_snapshot()
 * - which persists a few hundred bytes of window geometry - goes through
 * that exact same generic path on every window close/minimize/maximize/
 * restore/drag-end. So closing an app cost the same ~120-170ms as saving
 * a large file, and that cost landed on the GUI thread: this is the
 * confirmed, measured cause of "the OS freezes whenever you open or close
 * an app with storage attached".
 *
 * The fix is the same one virtually every real filesystem uses for this
 * exact tradeoff: write-back the metadata, not write-through. The file's
 * actual DATA is still written synchronously inside storage_write_file()
 * before this is ever reached, so the content is safely on disk
 * immediately - only the catalog/metadata-ring bookkeeping that makes the
 * write durable against a power loss is deferred and batched. A burst of
 * several saves (e.g. closing three windows in a row) now costs one
 * flush, not three.
 *
 * Bounded staleness, not indefinite deferral: storage_flush_catalog_if_due()
 * is polled from the GUI owner loop every frame (a cheap flag check when
 * clean) and forces a flush once COS_CATALOG_FLUSH_DEBOUNCE_MS has passed
 * since the catalog first became dirty, so the worst case is "at most
 * ~500ms of freshly-written metadata could be lost to a power cut", a
 * tradeoff every write-back cache makes, not "never persisted". */
#define COS_CATALOG_FLUSH_DEBOUNCE_MS 500ULL

static bool g_catalog_dirty = false;
static uint64_t g_catalog_dirty_since_ms = 0;

static void storage_mark_catalog_dirty(void) {
    if (!g_catalog_dirty) {
        g_catalog_dirty = true;
        g_catalog_dirty_since_ms = get_timer_ticks();
    }
}

/* Called from the GUI owner loop. Returns true if a flush actually ran. */
bool storage_flush_catalog_if_due(void) {
    if (!g_catalog_dirty) return false;
    uint64_t age = get_timer_ticks() - g_catalog_dirty_since_ms;
    if (age < COS_CATALOG_FLUSH_DEBOUNCE_MS) return false;
    return storage_flush_catalog_now();
}

/* Unconditional flush for callers that need durability immediately -
 * an explicit user Save action, or a clean shutdown path, should not
 * wait out the debounce window. */
static bool persist_catalog(void);

/* Caller holds storage_mutex. */
static bool flush_catalog_locked(void) {
    if (!g_catalog_dirty) { pending_release_all(); return true; }   /* nothing pending is not a failure */
    g_catalog_dirty = false;
    if (!persist_catalog()) {
        /* Persist failed: re-arm so the next poll retries rather than
         * silently dropping the pending metadata update. */
        storage_mark_catalog_dirty();
        return false;
    }
    pending_release_all();    /* the durable catalog no longer references the extents freed since the last commit */
    return true;
}

bool storage_flush_catalog_now(void) {
    if (!g_catalog_dirty) return true;
    storage_ensure_mutex();
    mutex_lock(&storage_mutex);       /* the catalog is shared with writers: persist a consistent snapshot of it */
    bool ok = flush_catalog_locked();
    mutex_unlock(&storage_mutex);
    return ok;
}

static bool persist_catalog(void) {
    if (!storage_disk_present) return true;
    uint64_t cos_pc_t0 = get_timer_ticks();
    refresh_catalog_header();

    /* Commit order, each step made durable before the next (ATA flush = barrier):
     *   1. file data written earlier is on the medium before any catalog refers to it,
     *   2. backup copy, then primary copy,
     *   3. the metadata-ring snapshot, which is what boot trusts first. */
    (void)ata_flush();
    if (!persist_catalog_copy(STORAGE_CATALOG_BACKUP, &storage_catalog)) return false;
    if (!persist_catalog_copy(STORAGE_CATALOG_SECTOR, &storage_catalog)) return false;
    (void)ata_flush();
    if (!storage_meta_write_snapshot()) return false;
    (void)ata_flush();
    {
        uint64_t cos_pc_ms = get_timer_ticks() - cos_pc_t0;
        serial_puts("[VFSPERF] persist_catalog ms=");
        serial_putdec(cos_pc_ms);
        serial_puts("\n");
    }
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

/* ---- Sector cache -------------------------------------------------------
 *
 * Why this exists: every filesystem operation went straight to PIO, and
 * PIO here costs roughly half a millisecond per sector because each of the
 * 256 inw() transfers plus the status polling is an I/O port access. Boot
 * alone issued 4000 sector reads totalling 2165ms of purely synchronous
 * I/O ([ATAPERF] reads=4000 total_ms=2165 worst_ms=3).
 *
 * That time is spent on whichever thread made the call - which for the
 * file manager, the text editor, or anything touching the filesystem is
 * the GUI owner thread. The whole desktop stops for the duration, which
 * is exactly the reported symptom: the UI freezes whenever storage is
 * attached and you open an app or move a window.
 *
 * FAT32 makes this far worse than the raw numbers suggest, because it
 * re-reads the same few sectors constantly: the FAT itself, the directory
 * cluster being listed, the boot sector. A direct-mapped cache turns those
 * repeats into memory copies.
 *
 * Direct-mapped rather than associative: a single index computation, no
 * search, no replacement policy to get wrong, and the access pattern
 * (a hot FAT region plus a hot directory region) maps well onto distinct
 * lines. Write-through rather than write-back: correctness on an OS that
 * can be powered off at any moment matters more here than write speed,
 * and it keeps the cache trivially coherent - a write updates the line and
 * goes to disk in the same call, so there is no dirty state that a crash
 * could lose or that a second code path could miss.
 */
#define ATA_CACHE_LINES 4096u  /* 4096 * 512B = 2 MiB */

typedef struct {
    uint64_t lba;
    bool     valid;
    uint8_t  data[512];
} ata_cache_line_t;

static ata_cache_line_t g_ata_cache[ATA_CACHE_LINES];
static uint64_t g_ata_cache_hits, g_ata_cache_misses;

static inline ata_cache_line_t *ata_cache_line_for(uint64_t lba) {
    return &g_ata_cache[(uint32_t)(lba % ATA_CACHE_LINES)];
}

/* One owner of the ATA task-file registers at a time. There was no lock
 * here at all, yet sectors are read from several kernel threads (FatFs
 * under fs_lock, the separate storage.c catalog store, music streaming):
 * two threads interleaving drive-select / LBA / command writes corrupt
 * each other's command, which surfaced as sporadic "timeout waiting for
 * DRQ" on sectors inside the FAT32 partition - making directories such as
 * /desktop unreadable. The lock also covers g_ata_cache, which is shared
 * state updated by the same paths. */
static mutex_t g_ata_mutex;
static bool g_ata_mutex_ready = false;
static void ata_lock(void) {
    if (!g_ata_mutex_ready) { mutex_init(&g_ata_mutex); g_ata_mutex_ready = true; }
    mutex_lock(&g_ata_mutex);
}
static void ata_unlock(void) { mutex_unlock(&g_ata_mutex); }

#define ATA_IO_ATTEMPTS 3

static bool ata_read_sector(uint64_t lba, void *buffer) {
    ata_lock();
    ata_cache_line_t *line = ata_cache_line_for(lba);
    if (line->valid && line->lba == lba) {
        memcpy(buffer, line->data, 512);
        ++g_ata_cache_hits;
        ata_unlock();
        return true;
    }
    bool ok = blk_read_retry(lba, 1, buffer);
    if (!ok) {
        /* Do not cache a failed read: the buffer contents are undefined,
         * and caching them would make a transient failure permanent. */
        line->valid = false;
        ata_unlock();
        return false;
    }
    ++g_ata_cache_misses;
    memcpy(line->data, buffer, 512);
    line->lba = lba;
    line->valid = true;
    ata_unlock();
    return true;
}

static bool ata_write_sector(uint64_t lba, const void *buffer) {
    ata_lock();
    ata_cache_line_t *line = ata_cache_line_for(lba);
    /* Invalidate first. If the disk write fails partway, the cache must
     * not keep claiming the new contents are on disk. */
    line->valid = false;
    bool ok = blk_write_retry(lba, 1, buffer);
    if (ok) {
        memcpy(line->data, buffer, 512);
        line->lba = lba;
        line->valid = true;
    }
    ata_unlock();
    return ok;
}

/* Cached read. Missing stretches are fetched with one backend call (the drivers keep many commands in flight inside
 * it). Small misses are rounded up with adaptive read-ahead - it doubles while a reader keeps asking for the next sectors
 * - so sequential readers mostly hit the cache; a stretch of BIG_IO sectors or more bypasses the cache and goes straight
 * into the caller's buffer, which is what lets a drive stream at full speed. */
#define ATA_READAHEAD_MIN 16u
#define ATA_READAHEAD_MAX 256u
#define BIG_IO            128u
#define DIRECT_CALL_MAX   4096u                         /* sectors handed to the driver per call (2 MiB) */
#define RUN_BUF_SECTORS   (BIG_IO + ATA_READAHEAD_MAX)
static uint8_t g_ata_run_buf[RUN_BUF_SECTORS * 512u];
static uint64_t g_ra_next;
static uint32_t g_ra_len = ATA_READAHEAD_MIN;

static bool blk_read_once(uint64_t lba, uint32_t count, void *buf) {
    bool ok = false;
    for (int attempt = 0; attempt < ATA_IO_ATTEMPTS && !ok; ++attempt) ok = g_blk->read(g_blk, lba, count, buf);
    return ok;
}
static bool blk_write_once(uint64_t lba, uint32_t count, const void *buf) {
    bool ok = false;
    for (int attempt = 0; attempt < ATA_IO_ATTEMPTS && !ok; ++attempt) ok = g_blk->write(g_blk, lba, count, buf);
    return ok;
}
/* The drivers hand the buffer to a DMA engine (IDE bus master, AHCI, NVMe): its address has to be at least 2 bytes (IDE/AHCI) or 4 bytes (NVMe) aligned,
 * and a transfer from an odd address is not what the engine writes to the disk. A file written at an odd offset (a zip header of 45 bytes, then the data) reaches
 * here with such a buffer (FatFs passes the user's data on from the byte where it stands), so the sectors go through an aligned bounce buffer. The caller holds
 * the ATA lock, which also protects the bounce buffer. */
#define ALIGN_SECT 128u
static uint8_t *g_align_bounce;
static bool blk_buf_unaligned(const void *buf) { return ((uintptr_t)buf & 3u) != 0; }
static bool align_bounce_ready(void) {
    if (!g_align_bounce) g_align_bounce = (uint8_t *)kmalloc_aligned((size_t)ALIGN_SECT * 512u, 4096);
    return g_align_bounce != NULL;
}
static bool blk_read_retry(uint64_t lba, uint32_t count, void *buf) {
    if (!blk_buf_unaligned(buf)) return blk_read_once(lba, count, buf);
    if (!align_bounce_ready()) return false;
    uint8_t *out = (uint8_t *)buf;
    while (count) {
        uint32_t n = count < ALIGN_SECT ? count : ALIGN_SECT;
        if (!blk_read_once(lba, n, g_align_bounce)) return false;
        memcpy(out, g_align_bounce, (size_t)n * 512u);
        lba += n; out += (size_t)n * 512u; count -= n;
    }
    return true;
}
static bool blk_write_retry(uint64_t lba, uint32_t count, const void *buf) {
    if (!blk_buf_unaligned(buf)) return blk_write_once(lba, count, buf);
    if (!align_bounce_ready()) return false;
    const uint8_t *in = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count < ALIGN_SECT ? count : ALIGN_SECT;
        memcpy(g_align_bounce, in, (size_t)n * 512u);
        if (!blk_write_once(lba, n, g_align_bounce)) return false;
        lba += n; in += (size_t)n * 512u; count -= n;
    }
    return true;
}

static bool ata_read_sectors(uint64_t lba, uint32_t count, void *buffer) {
    uint8_t *out = (uint8_t *)buffer;
    ata_lock();
    if (lba == g_ra_next) { if (g_ra_len < ATA_READAHEAD_MAX) g_ra_len *= 2; } else g_ra_len = ATA_READAHEAD_MIN;
    uint32_t i = 0;
    while (i < count) {
        ata_cache_line_t *line = ata_cache_line_for(lba + i);
        if (line->valid && line->lba == lba + i) {
            memcpy(out + (size_t)i * 512u, line->data, 512);
            ++g_ata_cache_hits;
            ++i;
            continue;
        }
        /* length of the missing stretch */
        uint32_t miss = 1;
        while (i + miss < count) {
            ata_cache_line_t *l2 = ata_cache_line_for(lba + i + miss);
            if (l2->valid && l2->lba == lba + i + miss) break;
            ++miss;
        }
        if (miss >= BIG_IO) {                          /* stream straight into the caller's buffer */
            uint32_t n = miss < DIRECT_CALL_MAX ? miss : DIRECT_CALL_MAX;
            if (!blk_read_retry(lba + i, n, out + (size_t)i * 512u)) { ata_unlock(); return false; }
            g_ata_cache_misses += n;
            i += n;
            continue;
        }
        uint32_t fetch = miss;
        if (i + miss >= count) {                       /* the stretch reaches the end of the request: read ahead */
            fetch = miss + g_ra_len;
            if (lba + i + fetch > g_abs_disk_sectors) fetch = (uint32_t)(g_abs_disk_sectors - (lba + i));
            if (fetch > RUN_BUF_SECTORS) fetch = RUN_BUF_SECTORS;
            if (fetch < miss) fetch = miss;
        }
        if (!blk_read_retry(lba + i, fetch, g_ata_run_buf)) { ata_unlock(); return false; }
        for (uint32_t k = 0; k < fetch; ++k) {
            ata_cache_line_t *cl = ata_cache_line_for(lba + i + k);
            memcpy(cl->data, g_ata_run_buf + (size_t)k * 512u, 512);
            cl->lba = lba + i + k;
            cl->valid = true;
        }
        g_ata_cache_misses += miss;
        memcpy(out + (size_t)i * 512u, g_ata_run_buf, (size_t)miss * 512u);
        i += miss;
    }
    g_ra_next = lba + count;
    ata_unlock();
    return true;
}

/* Write-through: the lines are invalidated first (not on disk until the driver says so), then refreshed - but only for
 * small writes; a big write just leaves them invalid rather than polluting the cache. */
static bool ata_write_sectors(uint64_t lba, uint32_t count, const void *buffer) {
    const uint8_t *in = (const uint8_t *)buffer;
    ata_lock();
    uint32_t i = 0;
    while (i < count) {
        uint32_t n = count - i < DIRECT_CALL_MAX ? count - i : DIRECT_CALL_MAX;
        for (uint32_t k = 0; k < n; ++k) ata_cache_line_for(lba + i + k)->valid = false;
        if (!blk_write_retry(lba + i, n, in + (size_t)i * 512u)) { ata_unlock(); return false; }
        if (n <= BIG_IO) {
            for (uint32_t k = 0; k < n; ++k) {
                ata_cache_line_t *cl = ata_cache_line_for(lba + i + k);
                memcpy(cl->data, in + (size_t)(i + k) * 512u, 512);
                cl->lba = lba + i + k;
                cl->valid = true;
            }
        }
        i += n;
    }
    ata_unlock();
    return true;
}

static bool ata_flush(void) {
    ata_lock();
    bool ok = g_blk->flush ? g_blk->flush(g_blk) : true;
    ata_unlock();
    return ok;
}

void ata_report_cache_stats(void) {
    uint64_t total = g_ata_cache_hits + g_ata_cache_misses;
    if (total == 0) return;
    serial_puts("[ATAPERF] cache hits=");
    serial_putdec(g_ata_cache_hits);
    serial_puts(" misses=");
    serial_putdec(g_ata_cache_misses);
    serial_puts(" hit_rate_pct=");
    serial_putdec((g_ata_cache_hits * 100ULL) / total);
    serial_puts("\n");
}

/* A bootable C-OS disk (tools/make_hdd_image.py) is GPT-partitioned: GRUB + kernel live in the first partitions and all
 * persistent data in a partition of type COS_DATA_GUID. Everything this file stores is addressed relative to that
 * partition, so the boot partitions can never be overwritten. A raw data disk (no GPT, e.g. the plain storage.img)
 * keeps the original layout starting at LBA 0. */
static const uint8_t k_cos_data_guid[16] = { 0x43, 0x4F, 0x53, 0x44, 0x41, 0x54, 0x41, 0x00,
                                             0x80, 0x00, 0xC0, 0x5D, 0xA7, 0xA0, 0x00, 0x01 };

static bool storage_find_data_partition(uint64_t abs_sectors, uint64_t* first, uint64_t* count) {
    uint8_t sec[SECTOR_SIZE];
    if (!ata_read_sector(1, sec)) return false;
    if (memcmp(sec, "EFI PART", 8) != 0) return false;
    uint64_t entries_lba = *(uint64_t*)(sec + 72);
    uint32_t n = *(uint32_t*)(sec + 80), esz = *(uint32_t*)(sec + 84);
    if (esz < 128 || esz > 512 || n == 0 || n > 256 || entries_lba < 2 || entries_lba >= abs_sectors) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t off = (uint64_t)i * esz;
        if (!ata_read_sector(entries_lba + off / SECTOR_SIZE, sec)) return false;
        const uint8_t* e = sec + (off % SECTOR_SIZE);
        if (memcmp(e, k_cos_data_guid, 16) != 0) continue;
        uint64_t f = *(uint64_t*)(e + 32), l = *(uint64_t*)(e + 40);
        if (f < 34 || l < f || l >= abs_sectors) return false;
        *first = f;
        *count = l - f + 1;
        return true;
    }
    return false;
}

bool storage_init(void) {
    if (storage_initialized) return true;

    /* Probe the primary ATA drive. This was previously never called, which
     * left storage_disk_present permanently false and made every disk read
     * (storage_read_file) fail and every disk write silently no-op - so
     * nothing ever actually persisted across reboots. */
    uint64_t real_sectors = 0;
    static blk_dev_t devs[9];                 /* [8]: a USB stick, see below */
    int ndev = 0;
    ndev += ide_enumerate(&devs[ndev], 8 - ndev);
    ndev += ahci_enumerate(&devs[ndev], 8 - ndev);
    if (ndev < 8) ndev += nvme_enumerate(&devs[ndev], 8 - ndev);

    /* Choose the disk: the one that carries a C-OS data partition (a disk we were booted from / installed to); failing
     * that, the first disk found, used as a raw data disk exactly as before. */
    storage_disk_present = ndev > 0;
    g_part_base = 0;
    int pick = -1;
    for (int i = 0; i < ndev && pick < 0; ++i) {
        g_blk = &devs[i];
        memset(g_ata_cache, 0, sizeof(g_ata_cache));
        g_abs_disk_sectors = devs[i].sectors;
        uint64_t pf = 0, pc = 0;
        if (storage_find_data_partition(devs[i].sectors, &pf, &pc)) {
            pick = i;
            g_part_base = pf;
            g_real_disk_sectors = pc;
            serial_puts("[VFS] GPT disk: C-OS data partition at LBA ");
            serial_putdec(pf);
            serial_puts(", ");
            serial_putdec(pc / 2048ULL);
            serial_puts(" MiB on ");
            serial_puts(devs[i].name);
            serial_puts("\n");
        }
    }
    /* Booted from a USB stick: the data partition is on the stick. The stick is only ever used when it carries a C-OS data partition (never as
     * a raw disk - it may be somebody's files), and it is only waited for when no internal disk has one. */
    if (pick < 0 && usb_msc_wait(8000)) {
        static blk_dev_t usbdevs[4];
        int nusb = usb_msc_enumerate(usbdevs, 4);
        for (int i = 0; i < nusb && pick < 0; ++i) {
            g_blk = &usbdevs[i];
            memset(g_ata_cache, 0, sizeof(g_ata_cache));
            g_abs_disk_sectors = usbdevs[i].sectors;
            uint64_t pf = 0, pc = 0;
            if (storage_find_data_partition(usbdevs[i].sectors, &pf, &pc)) {
                devs[8] = usbdevs[i];                 /* keep it where `devs[pick]` below can describe it */
                pick = 8;
                g_blk = &devs[8];
                g_part_base = pf;
                g_real_disk_sectors = pc;
                serial_puts("[VFS] GPT disk: C-OS data partition at LBA ");
                serial_putdec(pf);
                serial_puts(", ");
                serial_putdec(pc / 2048ULL);
                serial_puts(" MiB on ");
                serial_puts(usbdevs[i].name);
                serial_puts("\n");
                storage_disk_present = true;
            }
        }
    }
    if (pick < 0 && ndev > 0) {
        pick = 0;
        g_blk = &devs[0];
        memset(g_ata_cache, 0, sizeof(g_ata_cache));
        g_abs_disk_sectors = devs[0].sectors;
        g_real_disk_sectors = devs[0].sectors;
    }
    if (pick >= 0) {
        real_sectors = g_real_disk_sectors;
        serial_puts("[VFS] storage device: ");
        serial_puts(devs[pick].name);
        if (devs[pick].model[0]) { serial_puts(" ("); serial_puts(devs[pick].model); serial_puts(")"); }
        serial_puts("\n");
    } else {
        g_blk = &devs[0];                 /* keeps the pointer valid; storage_disk_present is false */
    }

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
    return ata_read_sector(g_part_base + lba, buf);
}

bool storage_raw_sector_write(uint64_t lba, const void* buf) {
    if (lba >= g_real_disk_sectors) return false;
    return ata_write_sector(g_part_base + lba, buf);
}

bool storage_raw_sectors_read(uint64_t lba, uint32_t count, void* buf) {
    if (count == 0) return true;
    if (lba + count > g_real_disk_sectors) return false;
    return ata_read_sectors(g_part_base + lba, count, buf);
}

bool storage_raw_sectors_write(uint64_t lba, uint32_t count, const void* buf) {
    if (count == 0) return true;
    if (lba + count > g_real_disk_sectors) return false;
    return ata_write_sectors(g_part_base + lba, count, buf);
}

/* Barrier: everything written so far is on the medium when this returns (ATA FLUSH CACHE). */
/* For the disk benchmark (VALIDATION_DISKBENCH): the active block device and a scratch extent at the end of the data
 * partition, in absolute device LBAs. */
blk_dev_t* storage_active_dev(uint64_t* scratch_lba, uint64_t* scratch_sectors) {
    if (!storage_initialized || !storage_disk_present) return NULL;
    uint64_t n = 262144ull;                                   /* 128 MiB */
    if (g_real_disk_sectors < 2 * n) return NULL;
    if (scratch_lba) *scratch_lba = g_part_base + g_real_disk_sectors - n;
    if (scratch_sectors) *scratch_sectors = n;
    return g_blk;
}

/* Discard blocks the filesystem no longer uses (SSD TRIM / NVMe deallocate). Best effort: a failure is harmless. */
bool storage_raw_trim(uint64_t lba, uint64_t count) {
    if (!g_blk || !g_blk->trim || count == 0 || lba + count > g_real_disk_sectors) return false;
    ata_lock();
    for (uint64_t i = 0; i < count; ++i) { ata_cache_line_t *l = ata_cache_line_for(g_part_base + lba + i); if (l->valid && l->lba == g_part_base + lba + i) l->valid = false; if (i > 8192) break; }
    bool ok = g_blk->trim(g_blk, g_part_base + lba, count);
    ata_unlock();
    return ok;
}

/* ---- the boot partition (EFI System Partition) of the disk the system runs from: used only by the kernel updater (cos_bootupd.c) ---- */
static const uint8_t k_esp_guid[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };   /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
/* The ESP of a GPT disk that carries the C-OS data partition (g_part_base != 0); false on a raw data disk, a live CD, anything else. */
bool storage_find_esp(uint64_t* first, uint64_t* count) {
    if (!storage_disk_present || g_part_base == 0) return false;
    uint8_t sec[SECTOR_SIZE];
    if (!ata_read_sector(1, sec) || memcmp(sec, "EFI PART", 8) != 0) return false;
    uint64_t entries_lba = *(uint64_t*)(sec + 72);
    uint32_t n = *(uint32_t*)(sec + 80), esz = *(uint32_t*)(sec + 84);
    if (esz < 128 || esz > 512 || n == 0 || n > 256 || entries_lba < 2 || entries_lba >= g_abs_disk_sectors) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t off = (uint64_t)i * esz;
        if (!ata_read_sector(entries_lba + off / SECTOR_SIZE, sec)) return false;
        const uint8_t* e = sec + (off % SECTOR_SIZE);
        if (memcmp(e, k_esp_guid, 16) != 0) continue;
        uint64_t f = *(uint64_t*)(e + 32), l = *(uint64_t*)(e + 40);
        if (f < 34 || l < f || l >= g_abs_disk_sectors || l >= g_part_base) return false;       /* the boot partition lies before the data partition */
        *first = f; *count = l - f + 1;
        return true;
    }
    return false;
}
/* Absolute sectors of the disk the system runs from. The caller (the ESP volume in diskio.c) keeps itself inside the ESP. */
bool storage_abs_read(uint64_t lba, uint32_t count, void* buf) { if (!count || lba + count > g_abs_disk_sectors) return false; return ata_read_sectors(lba, count, buf); }
bool storage_abs_write(uint64_t lba, uint32_t count, const void* buf) { if (!count || lba + count > g_abs_disk_sectors) return false; return ata_write_sectors(lba, count, buf); }

bool storage_raw_flush(void) {
    return ata_flush();
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
    bool is_new = idx < 0;
    vfs_file_t nf;
    if (is_new) {
        idx = vfs_alloc_file();
        if (idx < 0) { mutex_unlock(&storage_mutex); return false; }
        memset(&nf, 0, sizeof(nf));
        nf.used = true;
        nf.is_dir = false;
        strncpy(nf.path, filename, VFS_MAX_NAME_LEN - 1);
        nf.path[VFS_MAX_NAME_LEN - 1] = '\0';
        nf.created_time = rtc_get_time();
    } else {
        nf = vfs_files[idx];
    }
    uint64_t old_first = nf.first_sector, old_count = nf.sector_count;
    nf.size = size;
    nf.modified_time = rtc_get_time();
    nf.accessed_time = nf.modified_time;
    nf.first_sector = 0;
    nf.sector_count = 0;

    if (size > 0) {
        /* Copy-on-write: the new contents go to a fresh extent while the old one stays allocated and untouched, so a
         * crash at any point leaves the previous version of the file fully readable through the previous catalog. */
        uint64_t sectors = vfs_required_sectors(size);
        int first = vfs_find_free_run(sectors);
        if (first < 0 && storage_pending_sectors) {
            (void)flush_catalog_locked();            /* releases extents that were only held back for crash safety */
            first = vfs_find_free_run(sectors);
        }
        if (first < 0) { mutex_unlock(&storage_mutex); return false; }
        nf.first_sector = (uint64_t)first;
        nf.sector_count = sectors;
        bitmap_mark_range(nf.first_sector, nf.sector_count);
        if (!write_file_to_disk_with_data(&nf, data)) {
            bitmap_clear_range(nf.first_sector, nf.sector_count);   /* old version untouched */
            mutex_unlock(&storage_mutex);
            return false;
        }
    }

    /* Switch over in RAM; only now does the previous extent become free - but held back until the catalog that stops
     * referencing it is durable (see storage_bitmap_pending). */
    if (!is_new && old_count > 0) {
        bitmap_clear_range(old_first, old_count);
        pending_mark_range(old_first, old_count);
    }
    vfs_files[idx] = nf;
    storage_mark_catalog_dirty();
    mutex_unlock(&storage_mutex);
    return true;
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
        pending_mark_range(vfs_files[idx].first_sector, vfs_files[idx].sector_count);   /* reusable once the catalog commit lands */
    }

    memset(&vfs_files[idx], 0, sizeof(vfs_file_t));
    /* Same deferral as storage_write_file(): the in-RAM catalog is already
     * updated above, so the file is correctly gone as far as every other
     * call in this process is concerned. Only the on-disk bookkeeping is
     * batched with other pending saves. */
    storage_mark_catalog_dirty();
    mutex_unlock(&storage_mutex);
    return true;
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

bool storage_fsck(storage_fsck_t* out) {
    storage_fsck_t r;
    memset(&r, 0, sizeof(r));
    if (!storage_initialized && !storage_init()) return false;
    storage_ensure_mutex();
    mutex_lock(&storage_mutex);
    static uint8_t owned[STORAGE_BITMAP_BYTES];
    memset(owned, 0, sizeof(owned));
    for (int i = 0; i < VFS_MAX_FILES; ++i) {
        if (!vfs_files[i].used) continue;
        if (vfs_files[i].is_dir) { r.dirs++; continue; }
        r.files++;
        const vfs_file_t* f = &vfs_files[i];
        if (f->sector_count != vfs_required_sectors(f->size)) r.bad_size++;
        if (f->sector_count == 0) continue;
        if (f->first_sector < STORAGE_DATA_START_SECTOR || f->first_sector + f->sector_count > STORAGE_DATA_END_SECTOR) {
            r.bad_bounds++;
            continue;
        }
        uint64_t start = f->first_sector - STORAGE_DATA_START_SECTOR;
        for (uint64_t k = 0; k < f->sector_count; ++k) {
            uint64_t bit = start + k;
            uint8_t m = (uint8_t)(1u << (bit % 8ULL));
            if (owned[bit / 8ULL] & m) r.overlaps++;
            owned[bit / 8ULL] |= m;
            r.used_sectors++;
        }
    }
    for (uint64_t b = 0; b < STORAGE_BITMAP_BYTES; ++b) {
        uint8_t diff = (uint8_t)(owned[b] ^ storage_bitmap[b]);
        for (int k = 0; k < 8 && diff; ++k) {
            if (!(diff & (1u << k))) continue;
            if (storage_bitmap[b] & (1u << k)) r.leaked_sectors++; else r.unmarked_sectors++;
        }
    }
    r.pending_sectors = storage_pending_sectors;
    mutex_unlock(&storage_mutex);
    if (out) *out = r;
    return r.bad_bounds == 0 && r.overlaps == 0 && r.bad_size == 0 && r.leaked_sectors == 0 && r.unmarked_sectors == 0;
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
