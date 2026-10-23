/**
 * storage_partition_stub.c - Compatibility storage backend
 *
 * This file is kept as a fallback layer, but it no longer behaves like a
 * pure placeholder. It provides a small RAM-backed storage surface and logs
 * when the real partition backend is unavailable.
 */

#include "storage_partition_stub.h"
#include "string.h"
#include "serial.h"

#include <stddef.h>
#include <stdint.h>

#ifndef STORAGE_TEMPLATE_MAX_BYTES
#define STORAGE_TEMPLATE_MAX_BYTES (2u * 1024u * 1024u) /* 2 MiB fallback */
#endif

#ifndef STORAGE_TEMPLATE_SECTOR_SIZE
#define STORAGE_TEMPLATE_SECTOR_SIZE 512u
#endif

#ifndef STORAGE_TEMPLATE_MAX_ENTRIES
#define STORAGE_TEMPLATE_MAX_ENTRIES 64u
#endif

#ifndef STORAGE_TEMPLATE_MAX_PATH
#define STORAGE_TEMPLATE_MAX_PATH 96u
#endif

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

typedef struct {
    uint8_t used;
    uint8_t is_dir;
    char path[STORAGE_TEMPLATE_MAX_PATH];
    uint64_t offset;
    uint64_t size;
    uint64_t capacity;
} storage_template_entry_t;

static storage_template_info_t storage_template_info;
static uint8_t storage_template_ram[STORAGE_TEMPLATE_MAX_BYTES];
static uint8_t storage_template_backup_ram[STORAGE_TEMPLATE_MAX_BYTES];
static storage_template_entry_t storage_template_entries[STORAGE_TEMPLATE_MAX_ENTRIES];
static storage_template_entry_t storage_template_backup_entries[STORAGE_TEMPLATE_MAX_ENTRIES];
static uint64_t storage_template_capacity_bytes = 0;
static uint64_t storage_template_data_end = 0;
static uint64_t storage_template_mount_count = 0;
static uint64_t storage_template_backup_data_end = 0;
static uint64_t storage_template_backup_mount_count = 0;
static int storage_template_warned = 0;
static int storage_template_backup_valid = 0;

int WEAK storage_template_defragment(void);

static void storage_template_log(const char* msg) {
    serial_puts("[STORAGE-FALLBACK] ");
    serial_puts(msg);
    serial_puts("\n");
}

static void storage_template_warn_once(void) {
    if (!storage_template_warned) {
        storage_template_warned = 1;
        storage_template_log("real storage backend unavailable; using RAM fallback");
    }
}

static void storage_template_zero(void* ptr, size_t len) {
    uint8_t* p = (uint8_t*)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static void storage_template_copy(void* dst, const void* src, size_t len) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (len--) {
        *d++ = *s++;
    }
}

static size_t storage_template_strlen(const char* s) {
    size_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n]) {
        ++n;
    }
    return n;
}

static int storage_template_streq(const char* a, const char* b) {
    if (!a) a = "";
    if (!b) b = "";
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int storage_template_path_is_child(const char* parent, const char* candidate) {
    size_t plen;
    if (!parent || !candidate) {
        return 0;
    }
    if (storage_template_streq(parent, "/") == 0) {
        return 1;
    }
    plen = storage_template_strlen(parent);
    if (storage_template_streq(parent, candidate) == 0) {
        return 1;
    }
    return (candidate[0] != '\0' && strncmp(candidate, parent, plen) == 0 && candidate[plen] == '/');
}

static void storage_template_copy_path(char* dst, const char* src) {
    size_t i = 0;
    if (!dst) {
        return;
    }
    if (!src || !src[0]) {
        dst[0] = '/';
        dst[1] = '\0';
        return;
    }
    while (src[i] && i + 1 < STORAGE_TEMPLATE_MAX_PATH) {
        char c = src[i];
        if (c == '\\') c = '/';
        dst[i] = c;
        ++i;
    }
    dst[i] = '\0';
}

static void storage_template_join_path(char* out, const char* path, const char* name) {
    size_t p = 0;
    if (!out) {
        return;
    }
    out[0] = '\0';

    if (path && path[0]) {
        while (path[p] && p + 1 < STORAGE_TEMPLATE_MAX_PATH) {
            char c = path[p];
            if (c == '\\') c = '/';
            out[p] = c;
            ++p;
        }
    } else {
        out[p++] = '/';
    }

    if (p > 1 && out[p - 1] != '/') {
        if (p + 1 < STORAGE_TEMPLATE_MAX_PATH) {
            out[p++] = '/';
        }
    }

    if (name && name[0]) {
        size_t i = 0;
        while (name[i] && p + 1 < STORAGE_TEMPLATE_MAX_PATH) {
            char c = name[i];
            if (c == '\\') c = '/';
            out[p++] = c;
            ++i;
        }
    }

    if (p == 0) {
        out[p++] = '/';
    }
    out[p] = '\0';
}

static void storage_template_parent_path(const char* path, char* out) {
    size_t len;
    if (!out) {
        return;
    }
    if (!path || !path[0]) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    storage_template_copy_path(out, path);
    len = storage_template_strlen(out);
    while (len > 1 && out[len - 1] == '/') {
        out[--len] = '\0';
    }
    while (len > 1 && out[len - 1] != '/') {
        out[--len] = '\0';
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    if (len > 1 && out[len - 1] == '/') {
        out[len - 1] = '\0';
    }
}

static uint64_t storage_template_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static uint64_t storage_template_bytes_for_sectors(uint64_t sector_count) {
    if (sector_count == 0) {
        return STORAGE_TEMPLATE_MAX_BYTES;
    }

    uint64_t requested = sector_count * (uint64_t)STORAGE_TEMPLATE_SECTOR_SIZE;
    return storage_template_min_u64(requested, (uint64_t)STORAGE_TEMPLATE_MAX_BYTES);
}

static int storage_template_bounds_check(uint64_t sector, size_t* offset_out) {
    uint64_t offset = sector * (uint64_t)STORAGE_TEMPLATE_SECTOR_SIZE;
    uint64_t end = offset + (uint64_t)STORAGE_TEMPLATE_SECTOR_SIZE;

    if (end > storage_template_capacity_bytes) {
        return -1;
    }

    if (offset_out) {
        *offset_out = (size_t)offset;
    }
    return 0;
}

static storage_template_entry_t* storage_template_find_entry(const char* path) {
    char normalized[STORAGE_TEMPLATE_MAX_PATH];
    int i;

    storage_template_copy_path(normalized, path);
    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (storage_template_entries[i].used && storage_template_streq(storage_template_entries[i].path, normalized) == 0) {
            return &storage_template_entries[i];
        }
    }
    return NULL;
}

static storage_template_entry_t* storage_template_find_free_entry(void) {
    int i;
    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (!storage_template_entries[i].used) {
            return &storage_template_entries[i];
        }
    }
    return NULL;
}

static int storage_template_ensure_dir(const char* path) {
    char normalized[STORAGE_TEMPLATE_MAX_PATH];
    storage_template_entry_t* entry;
    storage_template_copy_path(normalized, path);

    if (normalized[0] == '\0') {
        normalized[0] = '/';
        normalized[1] = '\0';
    }

    entry = storage_template_find_entry(normalized);
    if (!entry) {
        entry = storage_template_find_free_entry();
        if (!entry) {
            return -1;
        }
        storage_template_zero(entry, sizeof(*entry));
        entry->used = 1;
        entry->is_dir = 1;
        storage_template_copy_path(entry->path, normalized);
    } else {
        entry->is_dir = 1;
    }

    return 0;
}

static int storage_template_ensure_parent_dirs(const char* path) {
    char parent[STORAGE_TEMPLATE_MAX_PATH];
    char current[STORAGE_TEMPLATE_MAX_PATH];
    size_t len;

    if (!path || !path[0]) {
        return 0;
    }

    storage_template_copy_path(current, path);
    len = storage_template_strlen(current);
    while (len > 1 && current[len - 1] == '/') {
        current[--len] = '\0';
    }

    storage_template_parent_path(current, parent);
    while (parent[0] && storage_template_streq(parent, "/") != 0) {
        if (storage_template_ensure_dir(parent) != 0) {
            return -1;
        }
        storage_template_parent_path(parent, current);
        storage_template_copy_path(parent, current);
        if (storage_template_streq(parent, "/") == 0) {
            break;
        }
    }

    return storage_template_ensure_dir("/");
}

static int storage_template_alloc_region(uint64_t len, uint64_t* offset_out) {
    if (len == 0) {
        if (offset_out) {
            *offset_out = storage_template_data_end;
        }
        return 0;
    }

    if (len > storage_template_capacity_bytes) {
        return -1;
    }

    if (storage_template_data_end + len > storage_template_capacity_bytes) {
        if (storage_template_defragment() != 0) {
            return -1;
        }
    }

    if (storage_template_data_end + len > storage_template_capacity_bytes) {
        return -1;
    }

    if (offset_out) {
        *offset_out = storage_template_data_end;
    }
    storage_template_data_end += len;
    return 0;
}

static void storage_template_recompute_used(void) {
    uint64_t used = 0;
    int i;
    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (storage_template_entries[i].used && !storage_template_entries[i].is_dir) {
            used += storage_template_entries[i].size;
        }
    }
    storage_template_info.used = used;
}

int WEAK storage_template_init(const char* name, uint64_t base_sector, uint64_t sector_count) {
    (void)base_sector;

    storage_template_zero(&storage_template_info, sizeof(storage_template_info));
    storage_template_zero(storage_template_entries, sizeof(storage_template_entries));

    if (name && name[0]) {
        strncpy(storage_template_info.label, name, sizeof(storage_template_info.label) - 1);
        storage_template_info.label[sizeof(storage_template_info.label) - 1] = '\0';
    } else {
        strncpy(storage_template_info.label, "RAM Fallback", sizeof(storage_template_info.label) - 1);
        storage_template_info.label[sizeof(storage_template_info.label) - 1] = '\0';
    }

    storage_template_capacity_bytes = storage_template_bytes_for_sectors(sector_count);
    storage_template_info.size = storage_template_capacity_bytes;
    storage_template_info.used = 0;
    storage_template_data_end = 0;
    storage_template_warn_once();
    storage_template_log("initialized");
    return 0;
}

int WEAK storage_template_read(uint64_t sector, void* buffer) {
    size_t offset = 0;
    if (!buffer) {
        storage_template_warn_once();
        storage_template_log("read rejected: null buffer");
        return -1;
    }
    if (storage_template_bounds_check(sector, &offset) < 0) {
        storage_template_warn_once();
        storage_template_log("read rejected: out of range sector");
        storage_template_zero(buffer, STORAGE_TEMPLATE_SECTOR_SIZE);
        return -1;
    }

    storage_template_copy(buffer, &storage_template_ram[offset], STORAGE_TEMPLATE_SECTOR_SIZE);
    return 0;
}

int WEAK storage_template_write(uint64_t sector, const void* buffer) {
    size_t offset = 0;
    if (!buffer) {
        storage_template_warn_once();
        storage_template_log("write rejected: null buffer");
        return -1;
    }
    if (storage_template_bounds_check(sector, &offset) < 0) {
        storage_template_warn_once();
        storage_template_log("write rejected: out of range sector");
        return -1;
    }

    storage_template_copy(&storage_template_ram[offset], buffer, STORAGE_TEMPLATE_SECTOR_SIZE);
    if (storage_template_info.used < (offset + STORAGE_TEMPLATE_SECTOR_SIZE)) {
        storage_template_info.used = offset + STORAGE_TEMPLATE_SECTOR_SIZE;
    }
    if (storage_template_data_end < (offset + STORAGE_TEMPLATE_SECTOR_SIZE)) {
        storage_template_data_end = offset + STORAGE_TEMPLATE_SECTOR_SIZE;
    }
    return 0;
}

int WEAK storage_template_mount(void) {
    storage_template_warn_once();
    storage_template_mount_count++;
    storage_template_log("mounted");
    return 0;
}

int WEAK storage_template_unmount(void) {
    storage_template_log("unmounted");
    return 0;
}

storage_template_info_t* WEAK storage_template_get_info(void) {
    return &storage_template_info;
}

int WEAK storage_template_format(uint64_t base_sector, uint64_t sector_count) {
    (void)base_sector;
    storage_template_warn_once();
    storage_template_capacity_bytes = storage_template_bytes_for_sectors(sector_count);
    storage_template_zero(storage_template_ram, (size_t)storage_template_capacity_bytes);
    storage_template_zero(storage_template_entries, sizeof(storage_template_entries));
    storage_template_data_end = 0;
    storage_template_info.size = storage_template_capacity_bytes;
    storage_template_info.used = 0;
    storage_template_log("formatted");
    return 0;
}

int WEAK storage_template_create_file(const char* path, const char* name) {
    char full_path[STORAGE_TEMPLATE_MAX_PATH];
    storage_template_entry_t* entry;

    storage_template_warn_once();
    storage_template_join_path(full_path, path, name);

    if (storage_template_ensure_parent_dirs(full_path) != 0) {
        storage_template_log("create_file failed: parent dir missing");
        return -1;
    }

    entry = storage_template_find_entry(full_path);
    if (entry) {
        entry->is_dir = 0;
        return 0;
    }

    entry = storage_template_find_free_entry();
    if (!entry) {
        storage_template_log("create_file failed: directory table full");
        return -1;
    }

    storage_template_zero(entry, sizeof(*entry));
    entry->used = 1;
    entry->is_dir = 0;
    storage_template_copy_path(entry->path, full_path);
    entry->offset = storage_template_data_end;
    entry->size = 0;
    entry->capacity = 0;
    storage_template_log("file created");
    return 0;
}

int WEAK storage_template_delete_file(const char* path) {
    storage_template_entry_t* entry;

    storage_template_warn_once();
    entry = storage_template_find_entry(path);
    if (!entry || entry->is_dir) {
        storage_template_log("delete_file failed: not found");
        return -1;
    }

    storage_template_zero(entry, sizeof(*entry));
    storage_template_recompute_used();
    storage_template_log("file deleted");
    return 0;
}

int WEAK storage_template_read_file(const char* path, void* buf, size_t len) {
    storage_template_entry_t* entry;
    size_t copy_len;

    storage_template_warn_once();
    if (!buf) {
        storage_template_log("read_file rejected: null buffer");
        return -1;
    }

    entry = storage_template_find_entry(path);
    if (!entry || entry->is_dir) {
        storage_template_log("read_file failed: file not found");
        storage_template_zero(buf, len);
        return -1;
    }

    if (entry->offset + entry->size > storage_template_capacity_bytes) {
        storage_template_log("read_file failed: corrupt metadata");
        return -1;
    }

    copy_len = (size_t)storage_template_min_u64((uint64_t)len, entry->size);
    if (copy_len) {
        storage_template_copy(buf, &storage_template_ram[(size_t)entry->offset], copy_len);
    }
    if (copy_len < len) {
        storage_template_zero((uint8_t*)buf + copy_len, len - copy_len);
    }
    return 0;
}

int WEAK storage_template_write_file(const char* path, void* buf, size_t len) {
    storage_template_entry_t* entry;
    uint64_t new_offset;
    size_t data_len = len;

    storage_template_warn_once();
    if (!buf && len) {
        storage_template_log("write_file rejected: null buffer");
        return -1;
    }

    entry = storage_template_find_entry(path);
    if (!entry || entry->is_dir) {
        storage_template_log("write_file failed: file not found");
        return -1;
    }

    if (entry->capacity >= data_len && entry->offset + data_len <= storage_template_capacity_bytes) {
        if (data_len) {
            storage_template_copy(&storage_template_ram[(size_t)entry->offset], buf, data_len);
        }
        entry->size = data_len;
        storage_template_recompute_used();
        storage_template_log("file overwritten in place");
        return 0;
    }

    if (storage_template_alloc_region((uint64_t)data_len, &new_offset) != 0) {
        storage_template_log("write_file failed: no space");
        return -1;
    }

    if (data_len) {
        storage_template_copy(&storage_template_ram[(size_t)new_offset], buf, data_len);
    }
    entry->offset = new_offset;
    entry->size = data_len;
    entry->capacity = data_len;
    storage_template_recompute_used();
    storage_template_log("file written");
    return 0;
}

int WEAK storage_template_create_dir(const char* path) {
    storage_template_entry_t* entry;
    char normalized[STORAGE_TEMPLATE_MAX_PATH];

    storage_template_warn_once();
    storage_template_copy_path(normalized, path);
    if (storage_template_ensure_parent_dirs(normalized) != 0) {
        return -1;
    }

    entry = storage_template_find_entry(normalized);
    if (entry) {
        entry->is_dir = 1;
        return 0;
    }

    entry = storage_template_find_free_entry();
    if (!entry) {
        storage_template_log("create_dir failed: directory table full");
        return -1;
    }

    storage_template_zero(entry, sizeof(*entry));
    entry->used = 1;
    entry->is_dir = 1;
    storage_template_copy_path(entry->path, normalized);
    storage_template_log("directory created");
    return 0;
}

int WEAK storage_template_delete_dir(const char* path) {
    char normalized[STORAGE_TEMPLATE_MAX_PATH];
    int i;
    int removed = 0;

    storage_template_warn_once();
    storage_template_copy_path(normalized, path);

    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (!storage_template_entries[i].used) {
            continue;
        }
        if (storage_template_path_is_child(normalized, storage_template_entries[i].path)) {
            storage_template_zero(&storage_template_entries[i], sizeof(storage_template_entries[i]));
            removed++;
        }
    }

    if (!removed) {
        storage_template_log("delete_dir failed: not found");
        return -1;
    }

    storage_template_recompute_used();
    storage_template_log("directory deleted");
    return 0;
}

void WEAK storage_template_list_contents(void) {
    int i;
    storage_template_warn_once();
    storage_template_log("listing fallback backend contents");
    serial_puts("[STORAGE-FALLBACK] label=");
    serial_puts(storage_template_info.label);
    serial_puts(" size=");
    serial_putdec(storage_template_info.size);
    serial_puts(" used=");
    serial_putdec(storage_template_info.used);
    serial_puts(" mounts=");
    serial_putdec(storage_template_mount_count);
    serial_puts("\n");
    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (!storage_template_entries[i].used) {
            continue;
        }
        serial_puts("  ");
        serial_puts(storage_template_entries[i].is_dir ? "[DIR] " : "[FILE] ");
        serial_puts(storage_template_entries[i].path);
        if (!storage_template_entries[i].is_dir) {
            serial_puts(" size=");
            serial_putdec(storage_template_entries[i].size);
        }
        serial_puts("\n");
    }
}

int WEAK storage_template_check_integrity(void) {
    int i, j;
    storage_template_warn_once();
    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (!storage_template_entries[i].used || storage_template_entries[i].is_dir) {
            continue;
        }
        if (storage_template_entries[i].offset + storage_template_entries[i].size > storage_template_capacity_bytes) {
            storage_template_log("integrity check failed: file out of bounds");
            return -1;
        }
        for (j = i + 1; j < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++j) {
            uint64_t a0, a1, b0, b1;
            if (!storage_template_entries[j].used || storage_template_entries[j].is_dir) {
                continue;
            }
            a0 = storage_template_entries[i].offset;
            a1 = a0 + storage_template_entries[i].size;
            b0 = storage_template_entries[j].offset;
            b1 = b0 + storage_template_entries[j].size;
            if (!(a1 <= b0 || b1 <= a0)) {
                storage_template_log("integrity check failed: overlapping files");
                return -1;
            }
        }
    }
    storage_template_log("check_integrity -> ok (fallback)");
    return 0;
}

int WEAK storage_template_defragment(void) {
    storage_template_entry_t* files[STORAGE_TEMPLATE_MAX_ENTRIES];
    int count = 0;
    int i, j;

    storage_template_warn_once();

    for (i = 0; i < (int)STORAGE_TEMPLATE_MAX_ENTRIES; ++i) {
        if (storage_template_entries[i].used && !storage_template_entries[i].is_dir) {
            files[count++] = &storage_template_entries[i];
        }
    }

    for (i = 0; i < count - 1; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (files[j]->offset < files[i]->offset) {
                storage_template_entry_t* tmp = files[i];
                files[i] = files[j];
                files[j] = tmp;
            }
        }
    }

    {
        uint64_t new_offset = 0;
        for (i = 0; i < count; ++i) {
            storage_template_entry_t* file = files[i];
            if (file->size == 0) {
                file->offset = new_offset;
                file->capacity = 0;
                continue;
            }
            if (file->offset != new_offset) {
                storage_template_copy(&storage_template_ram[(size_t)new_offset],
                                      &storage_template_ram[(size_t)file->offset],
                                      (size_t)file->size);
            }
            file->offset = new_offset;
            file->capacity = file->size;
            new_offset += file->size;
        }
        storage_template_data_end = new_offset;
    }

    storage_template_log("defragment -> compacted");
    return 0;
}

int WEAK storage_template_backup(void) {
    storage_template_warn_once();
    storage_template_copy(storage_template_backup_ram, storage_template_ram, sizeof(storage_template_ram));
    storage_template_copy(storage_template_backup_entries, storage_template_entries, sizeof(storage_template_entries));
    storage_template_backup_data_end = storage_template_data_end;
    storage_template_backup_mount_count = storage_template_mount_count;
    storage_template_backup_valid = 1;
    storage_template_log("backup -> snapshot stored");
    return 0;
}

int WEAK storage_template_restore(void) {
    if (!storage_template_backup_valid) {
        storage_template_warn_once();
        storage_template_log("restore -> no backup available");
        return -1;
    }

    storage_template_copy(storage_template_ram, storage_template_backup_ram, sizeof(storage_template_ram));
    storage_template_copy(storage_template_entries, storage_template_backup_entries, sizeof(storage_template_entries));
    storage_template_data_end = storage_template_backup_data_end;
    storage_template_mount_count = storage_template_backup_mount_count;
    storage_template_recompute_used();
    storage_template_log("restore -> snapshot restored");
    return 0;
}
