/**
 * fs_unified.c - Unified File System API Implementation
 * C-OS 4.0.7
 *
 * Drop-in replacement focused on reducing eager heap usage.
 *
 * Main change:
 *  - Do NOT snapshot the whole file at open() time.
 *  - Keep only metadata in the fd table, and lazily load a bounded
 *    in-memory snapshot on the first read()/seek-to-end.
 */

#include "fs_unified.h"
#include "fs.h"
#include "string.h"
#include "memory.h"
#include "sync.h"
#include "../kernel/security/permission_manager.h"
#include "../kernel/api/fs_api.h"

extern void serial_puts(const char* s);
extern void serial_puthex(uint64_t n);
extern void serial_putdec(uint64_t n);

/* 
 * Keep snapshots bounded. This avoids the early-boot OOM behavior that was
 * happening when open() eagerly copied entire files into kernel heap.
 */
#define FS_UNIFIED_SNAPSHOT_CAP_BYTES (8ULL * 1024ULL * 1024ULL) /* 8 MiB */

static mutex_t fs_unified_mutex;
static bool fs_unified_mutex_ready = false;
static bool g_fs_unified_initialized = false;
static int g_inode_count = 0;

static void fs_unified_ensure_mutex(void) {
    if (!fs_unified_mutex_ready) {
        mutex_init(&fs_unified_mutex);
        fs_unified_mutex_ready = true;
    }
}

static void fs_unified_copy_string(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < dst_size && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

static int fs_unified_access_mode(int flags) {
    return flags & FS_UNIFIED_O_ACCMODE;
}

typedef struct {
    int in_use;
    char path[FS_UNIFIED_MAX_PATH];
    int flags;
    int64_t pos;
    void* private_data;
    size_t data_size;      /* Loaded snapshot size, or cached file size hint */
    bool owns_data;
    bool snapshot_loaded;  /* True if private_data/data_size currently represent a loaded snapshot */
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t owner_pid;
    int inode_index;
    uint64_t created_time;
    uint64_t accessed_time;
} fs_unified_fd_t;

typedef struct {
    char path[FS_UNIFIED_MAX_PATH];
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    uint64_t size;
    uint64_t created_time;
    uint64_t modified_time;
    uint64_t accessed_time;
    int ref_count;
} fs_unified_inode_t;

static fs_unified_fd_t g_fd_table[FS_UNIFIED_MAX_FD];
static fs_unified_inode_t g_inode_table[FS_UNIFIED_MAX_FD];

static void fs_unified_release_fd_data(fs_unified_fd_t* f) {
    if (!f) return;
    if (f->owns_data && f->private_data) {
        kfree(f->private_data);
    }
    f->private_data = NULL;
    f->data_size = 0;
    f->owns_data = false;
    f->snapshot_loaded = false;
}

static fs_unified_inode_t* fs_unified_alloc_inode(const char* path) {
    if (g_inode_count >= FS_UNIFIED_MAX_FD) return NULL;

    fs_unified_inode_t* inode = &g_inode_table[g_inode_count++];
    memset(inode, 0, sizeof(*inode));
    fs_unified_copy_string(inode->path, sizeof(inode->path), path);
    inode->uid = permission_get_uid();
    inode->gid = permission_get_gid();
    inode->mode = 0644;
    inode->size = 0;
    inode->ref_count = 0;
    inode->created_time = 0;
    inode->modified_time = 0;
    inode->accessed_time = 0;
    return inode;
}

static fs_unified_inode_t* fs_unified_find_inode(const char* path) {
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < g_inode_count; i++) {
        if (strcmp(g_inode_table[i].path, path) == 0) {
            return &g_inode_table[i];
        }
    }
    return NULL;
}

static int fs_unified_inode_index(const fs_unified_inode_t* inode) {
    if (!inode) return -1;
    return (int)(inode - g_inode_table);
}

static fs_unified_inode_t* fs_unified_inode_from_index(int idx) {
    if (idx < 0 || idx >= g_inode_count) return NULL;
    return &g_inode_table[idx];
}

static uint64_t fs_unified_current_size(const fs_unified_fd_t* f) {
    if (!f) return 0;
    if (f->snapshot_loaded) return (uint64_t)f->data_size;

    if (f->inode_index >= 0) {
        const fs_unified_inode_t* inode = fs_unified_inode_from_index(f->inode_index);
        if (inode) return inode->size;
    }

    if (f->path[0]) {
        fs_file_info_t info;
        memset(&info, 0, sizeof(info));
        if (fs_api_get_file_info(f->path, &info)) {
            return info.size;
        }
    }
    return (uint64_t)f->data_size;
}

static bool fs_unified_ensure_snapshot(fs_unified_fd_t* f) {
    if (!f || !f->path[0]) return false;
    if (f->snapshot_loaded) return true;

    uint64_t size_hint = 0;
    if (f->inode_index >= 0) {
        fs_unified_inode_t* inode = fs_unified_inode_from_index(f->inode_index);
        if (inode && inode->size > 0) size_hint = inode->size;
    }
    if (size_hint == 0 && f->data_size > 0) {
        size_hint = (uint64_t)f->data_size;
    }
    if (size_hint == 0) {
        fs_file_info_t info;
        memset(&info, 0, sizeof(info));
        if (fs_api_get_file_info(f->path, &info)) {
            size_hint = info.size;
        }
    }

    if (size_hint == 0) {
        f->snapshot_loaded = true;
        f->data_size = 0;
        return true;
    }

    if (size_hint > FS_UNIFIED_SNAPSHOT_CAP_BYTES) {
        serial_puts("[FS_UNIFIED] Snapshot too large; refusing eager load (size=");
        serial_putdec(size_hint);
        serial_puts(")\n");
        return false;
    }

    void* snapshot = kmalloc((size_t)size_hint);
    if (!snapshot) {
        serial_puts("[FS_UNIFIED] Out of memory while loading snapshot\n");
        return false;
    }

    uint64_t copied = 0;
    if (!fs_api_read_file(f->path, snapshot, (size_t)size_hint, &copied)) {
        kfree(snapshot);
        return false;
    }

    if (copied > size_hint) copied = size_hint;
    f->private_data = snapshot;
    f->data_size = (size_t)copied;
    f->owns_data = true;
    f->snapshot_loaded = true;
    return true;
}

static int fs_unified_check_permission(const fs_unified_inode_t* inode, int flags) {
    if (!inode) return -1;

    if (permission_get_euid() == 0) return 0;

    uint32_t uid = permission_get_uid();
    uint32_t gid = permission_get_gid();
    uint32_t mode = inode->mode;
    int can_read = 0, can_write = 0, can_exec = 0;

    if (inode->uid == uid) {
        can_read = (mode & 0400) != 0;
        can_write = (mode & 0200) != 0;
        can_exec = (mode & 0100) != 0;
    } else if (inode->gid == gid) {
        can_read = (mode & 0040) != 0;
        can_write = (mode & 0020) != 0;
        can_exec = (mode & 0010) != 0;
    } else {
        can_read = (mode & 0004) != 0;
        can_write = (mode & 0002) != 0;
        can_exec = (mode & 0001) != 0;
    }

    (void)can_exec;

    switch (fs_unified_access_mode(flags)) {
        case FS_UNIFIED_O_WRONLY: return can_write ? 0 : -1;
        case FS_UNIFIED_O_RDWR:    return (can_read && can_write) ? 0 : -1;
        case FS_UNIFIED_O_RDONLY:
        default:                   return can_read ? 0 : -1;
    }
}

void fs_unified_close_process_files(uint64_t owner_pid) {
    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);
    for (int i = 0; i < FS_UNIFIED_MAX_FD; ++i) {
        if (g_fd_table[i].in_use && g_fd_table[i].owner_pid == owner_pid) {
            fs_unified_fd_t* f = &g_fd_table[i];
            fs_unified_inode_t* inode = fs_unified_inode_from_index(f->inode_index);
            if (inode && inode->ref_count > 0) {
                inode->ref_count--;
            } else if (f->path[0]) {
                inode = fs_unified_find_inode(f->path);
                if (inode && inode->ref_count > 0) inode->ref_count--;
            }
            fs_unified_release_fd_data(f);
            f->in_use = 0;
            f->path[0] = '\0';
            f->pos = 0;
            f->owner_pid = 0;
            f->inode_index = -1;
        }
    }
    mutex_unlock(&fs_unified_mutex);
}

int fs_unified_init(void) {
    if (g_fs_unified_initialized) return 0;

    serial_puts("[FS_UNIFIED] Initializing unified file system...\n");

    fs_init();
    /* NOTE: fat32_vfs_mount() used to be called here, but nothing in the
     * codebase actually reads or writes through the FAT32 API (fat32.c's
     * own internals are its only caller) - it was dead integration code.
     * Worse, it mounts starting at sector 0 of the SAME disk image that
     * storage.c's VFS catalog (the persistence layer fs_save_snapshot()
     * actually uses) also keeps its header at. When the FAT32 boot-sector
     * check failed, it called fat32_format(), which wrote a fresh FAT32
     * boot sector over storage.c's just-written catalog header - so the
     * real filesystem's on-disk catalog got corrupted on every single
     * boot and never survived a restart. Leaving it unmounted fixes
     * persistence without removing any feature actually in use. */

    memset(g_fd_table, 0, sizeof(g_fd_table));
    memset(g_inode_table, 0, sizeof(g_inode_table));
    g_inode_count = 0;

    fs_unified_inode_t* root = fs_unified_alloc_inode("/");
    if (root) {
        root->uid = 0;
        root->gid = 0;
        root->mode = 0755;
    }

    g_fs_unified_initialized = true;
    serial_puts("[FS_UNIFIED] Initialization complete\n");
    return 0;
}

int fs_unified_shutdown(void) {
    if (!g_fs_unified_initialized) return 0;

    serial_puts("[FS_UNIFIED] Shutting down...\n");
    for (int i = 0; i < FS_UNIFIED_MAX_FD; i++) {
        if (g_fd_table[i].in_use) {
            fs_unified_close(i);
        }
    }
    /* fat32_vfs_unmount() used to be called here, but fat32.c (the
     * hand-rolled FAT32 driver this unmounted) has been removed - see
     * the comment in fs_unified_init() above for why it was never
     * safe to mount in the first place. Nothing here needs it: real
     * FAT32 access goes through fs.c/FatFs (fs_init(), called
     * above), which is unaffected by this. */
    g_fs_unified_initialized = false;
    g_inode_count = 0;
    return 0;
}

static int fs_unified_alloc_fd(void) {
    for (int i = 0; i < FS_UNIFIED_MAX_FD; i++) {
        if (!g_fd_table[i].in_use) {
            g_fd_table[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static fs_unified_fd_t* fs_unified_get_fd(int fd) {
    if (fd < 0 || fd >= FS_UNIFIED_MAX_FD) return NULL;
    if (!g_fd_table[fd].in_use) return NULL;
    return &g_fd_table[fd];
}

int fs_unified_open(const char* path, int flags) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !path[0]) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    fs_file_info_t info;
    memset(&info, 0, sizeof(info));
    bool exists = fs_api_get_file_info(path, &info);

    if (!inode) {
        if (!exists && !(flags & FS_UNIFIED_O_CREAT)) {
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }

        if (!exists) {
            if (!fs_create_file(path)) {
                mutex_unlock(&fs_unified_mutex);
                return -1;
            }
            exists = true;
        }

        inode = fs_unified_alloc_inode(path);
        if (!inode) {
            if (!fs_api_exists(path)) {
                fs_delete_file(path);
            }
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }
        if (exists) {
            inode->size = info.size;
            inode->mode = info.is_directory ? 0755 : 0644;
        }
    } else if (!exists && !(flags & FS_UNIFIED_O_CREAT)) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    if (fs_unified_check_permission(inode, flags) != 0) {
        serial_puts("[FS_UNIFIED] Permission denied\n");
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    int fd = fs_unified_alloc_fd();
    if (fd < 0) {
        serial_puts("[FS_UNIFIED] No free file descriptors\n");
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    fs_unified_fd_t* f = &g_fd_table[fd];
    memset(f, 0, sizeof(*f));
    f->in_use = 1;
    fs_unified_copy_string(f->path, sizeof(f->path), path);
    f->flags = flags;
    f->pos = 0;
    f->private_data = NULL;
    f->data_size = (size_t)inode->size;
    f->owns_data = false;
    f->snapshot_loaded = (inode->size == 0);
    process_t* current = process_get_current();
    f->owner_pid = current ? current->pid : 0;
    f->inode_index = fs_unified_inode_index(inode);
    f->uid = inode->uid;
    f->gid = inode->gid;
    f->mode = inode->mode;
    inode->accessed_time = 0;
    inode->ref_count++;

    if (flags & FS_UNIFIED_O_TRUNC) {
        if (!fs_write_file(path, "", 0)) {
            fs_unified_release_fd_data(f);
            f->in_use = 0;
            f->path[0] = '\0';
            f->pos = 0;
            f->owner_pid = 0;
            f->inode_index = -1;
            if (inode && inode->ref_count > 0) inode->ref_count--;
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }
        inode->size = 0;
        f->data_size = 0;
        f->snapshot_loaded = true;
    }

    mutex_unlock(&fs_unified_mutex);
    return fd;
}

int fs_unified_close(int fd) {
    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);
    fs_unified_fd_t* f = fs_unified_get_fd(fd);
    if (!f) { mutex_unlock(&fs_unified_mutex); return -1; }

    fs_unified_inode_t* inode = fs_unified_inode_from_index(f->inode_index);
    if (!inode && f->path[0]) {
        inode = fs_unified_find_inode(f->path);
    }
    if (inode && inode->ref_count > 0) {
        inode->ref_count--;
    }

    fs_unified_release_fd_data(f);
    f->in_use = 0;
    f->path[0] = '\0';
    f->pos = 0;
    f->owner_pid = 0;
    f->inode_index = -1;

    mutex_unlock(&fs_unified_mutex);
    return 0;
}

int fs_unified_read(int fd, void* buf, size_t count) {
    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);
    fs_unified_fd_t* f = fs_unified_get_fd(fd);
    if (!f) { mutex_unlock(&fs_unified_mutex); return -1; }
    if (!buf) { mutex_unlock(&fs_unified_mutex); return -1; }

    int access = fs_unified_access_mode(f->flags);
    if (access == FS_UNIFIED_O_WRONLY) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    if (count == 0) {
        mutex_unlock(&fs_unified_mutex);
        return 0;
    }

    if (!f->snapshot_loaded) {
        if (!fs_unified_ensure_snapshot(f)) {
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }
    }

    const uint8_t* data = (const uint8_t*)f->private_data;
    size_t data_len = f->data_size;
    if (!data || data_len == 0 || f->pos >= (int64_t)data_len) {
        mutex_unlock(&fs_unified_mutex);
        return 0;
    }

    size_t avail = data_len - (size_t)f->pos;
    if (count > avail) count = avail;

    memcpy(buf, data + f->pos, count);
    f->pos += (int64_t)count;

    fs_unified_inode_t* inode = fs_unified_find_inode(f->path);
    if (inode) inode->accessed_time = 0;

    mutex_unlock(&fs_unified_mutex);
    return (int)count;
}

int fs_unified_write(int fd, const void* buf, size_t count) {
    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);
    fs_unified_fd_t* f = fs_unified_get_fd(fd);
    if (!f) { mutex_unlock(&fs_unified_mutex); return -1; }
    if (!buf && count > 0) { mutex_unlock(&fs_unified_mutex); return -1; }

    int access = fs_unified_access_mode(f->flags);
    if (access == FS_UNIFIED_O_RDONLY) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    if (fs_write_file(f->path, (const char*)buf, count)) {
        /* We intentionally do not keep a full-file snapshot here; that was a
         * major source of early-boot heap pressure. The next read() will
         * lazily fetch a bounded snapshot if needed. */
        fs_unified_release_fd_data(f);
        f->data_size = count;
        f->snapshot_loaded = (count == 0);
        f->pos += (int64_t)count;

        fs_unified_inode_t* inode = fs_unified_find_inode(f->path);
        if (inode) {
            inode->modified_time = 0;
            if ((uint64_t)f->pos > inode->size) {
                inode->size = (uint64_t)f->pos;
            }
        }

        mutex_unlock(&fs_unified_mutex);
        return (int)count;
    }

    mutex_unlock(&fs_unified_mutex);
    return -1;
}

int64_t fs_unified_seek(int fd, int64_t offset, int whence) {
    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);
    fs_unified_fd_t* f = fs_unified_get_fd(fd);
    if (!f) { mutex_unlock(&fs_unified_mutex); return -1; }

    uint64_t size_hint = fs_unified_current_size(f);
    int64_t new_pos = f->pos;

    switch (whence) {
        case 0: new_pos = offset; break;            /* SEEK_SET */
        case 1: new_pos = f->pos + offset; break;   /* SEEK_CUR */
        case 2: new_pos = (int64_t)size_hint + offset; break; /* SEEK_END */
        default:
            mutex_unlock(&fs_unified_mutex);
            return -1;
    }

    if (new_pos < 0) new_pos = 0;
    if (new_pos > (int64_t)size_hint) new_pos = (int64_t)size_hint;
    f->pos = new_pos;

    mutex_unlock(&fs_unified_mutex);
    return new_pos;
}

int fs_unified_mkdir(const char* path) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !path[0]) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    if (fs_api_exists(path)) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    int created = fs_create_dir(path) ? 0 : -1;
    if (created == 0) {
        fs_unified_inode_t* inode = fs_unified_alloc_inode(path);
        if (!inode) {
            fs_delete_dir(path);
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }
        inode->mode = 0755;
    }

    mutex_unlock(&fs_unified_mutex);
    return created;
}

int fs_unified_rmdir(const char* path) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !path[0]) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    if (!inode) { mutex_unlock(&fs_unified_mutex); return -1; }
    if (fs_unified_check_permission(inode, FS_UNIFIED_O_WRONLY) != 0) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    mutex_unlock(&fs_unified_mutex);
    return -1; /* native FS doesn't support rmdir yet */
}

int fs_unified_readdir(const char* path, fs_unified_dirent_t* entries, int max_entries) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !entries || max_entries <= 0) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    if (inode && fs_unified_check_permission(inode, FS_UNIFIED_O_RDONLY) != 0) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    fs_entry_t* fs_entries = fs_list_dir(path);
    if (!fs_entries) { mutex_unlock(&fs_unified_mutex); return 0; }

    int count = 0;
    for (int i = 0; i < max_entries && fs_entries[i].name[0]; i++) {
        fs_unified_copy_string(entries[i].name, sizeof(entries[i].name), fs_entries[i].name);
        entries[i].type = fs_entries[i].is_dir ? FS_UNIFIED_TYPE_DIR : FS_UNIFIED_TYPE_FILE;
        entries[i].size = fs_entries[i].size;
        entries[i].mtime = fs_entries[i].modified_time;
        count++;
    }

    mutex_unlock(&fs_unified_mutex);
    return count;
}

int fs_unified_unlink(const char* path) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !path[0]) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    if (inode && fs_unified_check_permission(inode, FS_UNIFIED_O_WRONLY) != 0) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    int ok = fs_delete_file(path) ? 0 : -1;
    mutex_unlock(&fs_unified_mutex);
    return ok;
}

int fs_unified_rename(const char* oldpath, const char* newpath) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!oldpath || !newpath) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(oldpath);
    if (inode && fs_unified_check_permission(inode, FS_UNIFIED_O_WRONLY) != 0) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    int ok = fs_rename(oldpath, newpath);
    mutex_unlock(&fs_unified_mutex);
    return ok;
}

int fs_unified_stat(const char* path, fs_unified_stat_t* st) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !st) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    if (inode) {
        st->uid = inode->uid;
        st->gid = inode->gid;
        st->mode = inode->mode;
        st->size = inode->size;
        st->atime = inode->accessed_time;
        st->mtime = inode->modified_time;
        st->ctime = inode->created_time;
        st->type = FS_UNIFIED_TYPE_FILE;
        mutex_unlock(&fs_unified_mutex);
        return 0;
    }

    fs_entry_t* entries = fs_list_dir(path);
    if (!entries) { mutex_unlock(&fs_unified_mutex); return -1; }

    if (entries[0].name[0] != '\0') {
        st->type = entries[0].is_dir ? FS_UNIFIED_TYPE_DIR : FS_UNIFIED_TYPE_FILE;
        st->size = entries[0].size;
        st->mtime = entries[0].modified_time;
        st->atime = entries[0].accessed_time;
        st->ctime = entries[0].created_time;
        st->mode = 0644;
        st->uid = 0;
        st->gid = 0;
        mutex_unlock(&fs_unified_mutex);
        return 0;
    }

    mutex_unlock(&fs_unified_mutex);
    return -1;
}

bool fs_unified_exists(const char* path) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return false;
    }
    if (!path || !path[0]) return false;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);
    bool exists = fs_api_exists(path);
    mutex_unlock(&fs_unified_mutex);
    return exists;
}

int fs_unified_chmod(const char* path, uint32_t mode) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !path[0]) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    if (!inode) {
        if (!fs_api_exists(path)) {
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }
        inode = fs_unified_alloc_inode(path);
        if (!inode) { mutex_unlock(&fs_unified_mutex); return -1; }
    }

    if (permission_get_euid() != 0 && permission_get_uid() != inode->uid) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    inode->mode = mode;
    mutex_unlock(&fs_unified_mutex);
    return 0;
}

int fs_unified_chown(const char* path, uint32_t uid, uint32_t gid) {
    if (!g_fs_unified_initialized) {
        if (fs_unified_init() != 0) return -1;
    }
    if (!path || !path[0]) return -1;

    fs_unified_ensure_mutex();
    mutex_lock(&fs_unified_mutex);

    fs_unified_inode_t* inode = fs_unified_find_inode(path);
    if (!inode) {
        if (!fs_api_exists(path)) {
            mutex_unlock(&fs_unified_mutex);
            return -1;
        }
        inode = fs_unified_alloc_inode(path);
        if (!inode) { mutex_unlock(&fs_unified_mutex); return -1; }
    }

    if (permission_get_euid() != 0) {
        mutex_unlock(&fs_unified_mutex);
        return -1;
    }

    inode->uid = uid;
    inode->gid = gid;
    mutex_unlock(&fs_unified_mutex);
    return 0;
}

int fs_unified_setuid(uint32_t uid) { return permission_set_uid(uid); }
int fs_unified_setgid(uint32_t gid) { return permission_set_gid(gid); }
uint32_t fs_unified_getuid(void) { return permission_get_uid(); }
uint32_t fs_unified_getgid(void) { return permission_get_gid(); }
