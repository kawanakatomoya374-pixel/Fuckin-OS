/**
 * storage_manager.c - ストレージ永続化管理実装
 * C-OS 5.0.0 - fs_unified API 移行版
 */
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "storage_manager.h"
#include "../../include/cos_version.h"
#include "../../fs/fs_unified.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef NULL
#define NULL ((void*)0)
#endif

extern void serial_puts(const char* s);
extern void serial_puthex(u64 val);
extern void serial_putdec(u32 val);
extern uint64_t get_timer_ticks(void);
extern void* kmalloc(size_t size);
extern void kfree(void* ptr);

/* Forward declarations */
int storage_save_state(void);
int storage_load_state(void);
static u32 storage_entry_checksum(const storage_entry_t* entry);

/* ストレージ状態（不揮発性） */
static storage_state_t g_storage_state;
static bool g_storage_initialized = false;
static bool g_is_dirty = false;

#define STORAGE_STATE_PRIMARY_PATH "/system/storage_meta.cos"
#define STORAGE_STATE_BACKUP_PATH  "/system/storage_meta.cos.bak"
#define STORAGE_STATE_TEMP_PATH    "/system/storage_meta.cos.tmp"

static void storage_copy_state_path(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static u32 storage_checksum_bytes(const void* data, size_t size) {
    const u8* bytes = (const u8*)data;
    u32 hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static u32 storage_state_checksum(const storage_state_t* state) {
    if (!state) return 0;
    storage_state_t temp;
    memset(&temp, 0, sizeof(temp));
    memcpy(&temp, state, sizeof(temp));
    temp.checksum = 0;
    return storage_checksum_bytes(&temp, sizeof(temp));
}

static int storage_validate_loaded_state(void) {
    if (g_storage_state.magic != STORAGE_MAGIC) return -1;
    if (g_storage_state.total_entries > STORAGE_MAX_FILES) return -1;
    if (g_storage_state.password_count > MAX_PASSWORDS) return -1;
    if (g_storage_state.checksum != storage_state_checksum(&g_storage_state)) return -1;

    for (u32 i = 0; i < g_storage_state.total_entries; ++i) {
        storage_entry_t* entry = &g_storage_state.entries[i];
        if (entry->name[0] == '\0' || entry->path[0] == '\0') return -1;
        if (entry->size > (u64)SIZE_MAX) return -1;
        if (entry->checksum != storage_entry_checksum(entry)) return -1;
    }

    return 0;
}

/* ヘルパー: ファイルへの一括書き込み */
static int write_entire_file(const char* path, const void* data, size_t size) {
    int fd = fs_unified_open(path, FS_UNIFIED_O_WRONLY | FS_UNIFIED_O_CREAT | FS_UNIFIED_O_TRUNC);
    if (fd < 0) return -1;
    int written = fs_unified_write(fd, data, size);
    fs_unified_close(fd);
    return (written == (int)size) ? 0 : -1;
}

/* ヘルパー: ファイルからの一括読み込み */
static int read_entire_file(const char* path, void* data, size_t* size) {
    int fd = fs_unified_open(path, FS_UNIFIED_O_RDONLY);
    if (fd < 0) return -1;
    int read = fs_unified_read(fd, data, *size);
    fs_unified_close(fd);
    if (read >= 0) {
        *size = (size_t)read;
        return 0;
    }
    return -1;
}

int storage_manager_init(void) {
    if (g_storage_initialized) return 0;
    
    serial_puts("[STORAGE] Initializing storage manager...\n");
    memset(&g_storage_state, 0, sizeof(storage_state_t));
    
    if (storage_load_state() != 0) {
        serial_puts("[STORAGE] No valid state found, creating new one\n");
        g_storage_state.magic = STORAGE_MAGIC;
        g_storage_state.version = 1;
        g_storage_state.total_entries = 0;
        g_is_dirty = true;
    }
    
    g_storage_initialized = true;
    return 0;
}

int storage_save_state(void) {
    if (!g_storage_initialized) return -1;

    /* チェックサム更新 */
    for (u32 i = 0; i < g_storage_state.total_entries; ++i) {
        g_storage_state.entries[i].checksum = storage_entry_checksum(&g_storage_state.entries[i]);
    }
    g_storage_state.checksum = storage_state_checksum(&g_storage_state);

    char temp_path[128];
    char primary_path[128];
    char backup_path[128];
    storage_copy_state_path(temp_path, sizeof(temp_path), STORAGE_STATE_TEMP_PATH);
    storage_copy_state_path(primary_path, sizeof(primary_path), STORAGE_STATE_PRIMARY_PATH);
    storage_copy_state_path(backup_path, sizeof(backup_path), STORAGE_STATE_BACKUP_PATH);

    if (write_entire_file(temp_path, &g_storage_state, sizeof(storage_state_t)) != 0) {
        serial_puts("[STORAGE] WARNING: temp state save failed\n");
        return -1;
    }

    if (write_entire_file(primary_path, &g_storage_state, sizeof(storage_state_t)) != 0) {
        serial_puts("[STORAGE] WARNING: primary state save failed\n");
        return -1;
    }

    if (write_entire_file(backup_path, &g_storage_state, sizeof(storage_state_t)) != 0) {
        serial_puts("[STORAGE] WARNING: backup state save failed\n");
        return -1;
    }

    g_is_dirty = false;
    serial_puts("[STORAGE] State saved successfully\n");
    return 0;
}

int storage_load_state(void) {
    serial_puts("[STORAGE] Loading storage state...\n");

    const char* candidates[3] = { STORAGE_STATE_PRIMARY_PATH, STORAGE_STATE_BACKUP_PATH, STORAGE_STATE_TEMP_PATH };
    for (int i = 0; i < 3; i++) {
        size_t size = sizeof(storage_state_t);
        if (read_entire_file(candidates[i], &g_storage_state, &size) == 0 && size == sizeof(storage_state_t)) {
            if (storage_validate_loaded_state() == 0) {
                serial_puts("[STORAGE] State loaded from ");
                serial_puts(candidates[i]);
                serial_puts("\n");
                g_is_dirty = false;
                return 0;
            }
        }
    }
    return -1;
}

// NOTE: storage_read_file / storage_write_file are intentionally NOT defined here.
// The canonical, VFS-backed implementations live in src/drivers/disk/storage.c
// and provide persistent file IO for the rest of the kernel.

int storage_sync(void) {
    /* Flush dirty storage state to disk. Delegates to the existing save
     * pipeline; returns 0 when nothing needs to be flushed. */
    if (!g_storage_initialized) return -1;
    if (!g_is_dirty) return 0;
    return storage_save_state();
}

bool storage_is_dirty(void) { return g_is_dirty; }

static u32 storage_entry_checksum(const storage_entry_t* entry) {
    if (!entry) return 0;
    storage_entry_t temp;
    memset(&temp, 0, sizeof(temp));
    memcpy(&temp, entry, sizeof(temp));
    temp.checksum = 0;
    return storage_checksum_bytes(&temp, sizeof(temp));
}
