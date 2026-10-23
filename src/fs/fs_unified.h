/**
 * fs_unified.h - Unified File System API
 * 
 * C-OS 5.0.0 統一ファイルシステムAPI
 * VFS廃止後の標準インターフェース
 * 
 * 対応ファイルシステム:
 * - C-OS Native FS (fs.c)
 * - FAT32 (fat32.c)
 * - ext2 (ext2.c)
 * - RAM FS (ramfs.c)
 */

#ifndef FS_UNIFIED_H
#define FS_UNIFIED_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* File descriptor limits */
#define FS_UNIFIED_MAX_FD           256
#define FS_UNIFIED_MAX_MOUNTS       16
#define FS_UNIFIED_MAX_PATH         4096
#define FS_UNIFIED_MAX_NAME         256

/* Open flags */
#define FS_UNIFIED_O_RDONLY         0x0000
#define FS_UNIFIED_O_WRONLY         0x0001
#define FS_UNIFIED_O_RDWR           0x0002
#define FS_UNIFIED_O_ACCMODE        0x0003
#define FS_UNIFIED_O_CREAT          0x0040
#define FS_UNIFIED_O_EXCL           0x0080
#define FS_UNIFIED_O_TRUNC          0x0200
#define FS_UNIFIED_O_APPEND         0x0400

/* File types */
typedef enum {
    FS_UNIFIED_TYPE_UNKNOWN = 0,
    FS_UNIFIED_TYPE_FILE,
    FS_UNIFIED_TYPE_DIR,
    FS_UNIFIED_TYPE_SYMLINK,
    FS_UNIFIED_TYPE_BLOCKDEV,
    FS_UNIFIED_TYPE_CHARDEV,
} fs_unified_type_t;

/* File statistics */
typedef struct {
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    fs_unified_type_t type;
} fs_unified_stat_t;

/* Directory entry */
typedef struct {
    char name[FS_UNIFIED_MAX_NAME];
    fs_unified_type_t type;
    uint64_t size;
    uint64_t mtime;
} fs_unified_dirent_t;

/* ============================================================
 * Core File Operations
 * ============================================================ */

/**
 * Open a file
 * @param path Absolute path to file
 * @param flags Open flags (FS_UNIFIED_O_*)
 * @return File descriptor (>= 0) on success, -1 on error
 */
int fs_unified_open(const char* path, int flags);

/**
 * Close a file
 * @param fd File descriptor
 * @return 0 on success, -1 on error
 */
int fs_unified_close(int fd);

/**
 * Read from file
 * @param fd File descriptor
 * @param buf Buffer to read into
 * @param count Number of bytes to read
 * @return Number of bytes read, -1 on error
 */
int fs_unified_read(int fd, void* buf, size_t count);

/**
 * Write to file
 * @param fd File descriptor
 * @param buf Buffer to write from
 * @param count Number of bytes to write
 * @return Number of bytes written, -1 on error
 */
int fs_unified_write(int fd, const void* buf, size_t count);

/**
 * Seek in file
 * @param fd File descriptor
 * @param offset Offset
 * @param whence SEEK_SET, SEEK_CUR, or SEEK_END
 * @return New file position, -1 on error
 */
int64_t fs_unified_seek(int fd, int64_t offset, int whence);

/* ============================================================
 * Directory Operations
 * ============================================================ */

/**
 * Create a directory
 * @param path Absolute path to directory
 * @return 0 on success, -1 on error
 */
int fs_unified_mkdir(const char* path);

/**
 * Remove a directory
 * @param path Absolute path to directory
 * @return 0 on success, -1 on error
 */
int fs_unified_rmdir(const char* path);

/**
 * List directory contents
 * @param path Absolute path to directory
 * @param entries Output array of directory entries
 * @param max_entries Maximum number of entries to return
 * @return Number of entries, -1 on error
 */
int fs_unified_readdir(const char* path, fs_unified_dirent_t* entries, int max_entries);

/* ============================================================
 * File Operations
 * ============================================================ */

/**
 * Delete a file
 * @param path Absolute path to file
 * @return 0 on success, -1 on error
 */
int fs_unified_unlink(const char* path);

/**
 * Rename a file
 * @param oldpath Old path
 * @param newpath New path
 * @return 0 on success, -1 on error
 */
int fs_unified_rename(const char* oldpath, const char* newpath);

/**
 * Get file statistics
 * @param path Absolute path to file
 * @param st Output statistics
 * @return 0 on success, -1 on error
 */
int fs_unified_stat(const char* path, fs_unified_stat_t* st);

/**
 * Check if file exists
 * @param path Absolute path
 * @return true if exists, false otherwise
 */
bool fs_unified_exists(const char* path);

/* ============================================================
 * Initialization
 * ============================================================ */

/**
 * Initialize the unified file system
 * @return 0 on success, -1 on error
 */
int fs_unified_init(void);

/**
 * Shutdown the unified file system
 * @return 0 on success, -1 on error
 */
int fs_unified_shutdown(void);

/* ============================================================
 * Permission Management
 * ============================================================ */

/**
 * Change file mode (permissions)
 * @param path Absolute path to file
 * @param mode New file mode (permissions)
 * @return 0 on success, -1 on error
 */
int fs_unified_chmod(const char* path, uint32_t mode);

/**
 * Change file owner and group
 * @param path Absolute path to file
 * @param uid New owner UID
 * @param gid New owner GID
 * @return 0 on success, -1 on error
 */
int fs_unified_chown(const char* path, uint32_t uid, uint32_t gid);

/**
 * Close every file descriptor owned by the given process.
 * Used by task teardown so per-process open files do not leak.
 */
void fs_unified_close_process_files(uint64_t owner_pid);

/**
 * Set current process UID
 * @param uid New UID
 * @return 0 on success, -1 on error
 */
int fs_unified_setuid(uint32_t uid);

/**
 * Set current process GID
 * @param gid New GID
 * @return 0 on success, -1 on error
 */
int fs_unified_setgid(uint32_t gid);

/**
 * Get current process UID
 * @return Current UID
 */
uint32_t fs_unified_getuid(void);

/**
 * Get current process GID
 * @return Current GID
 */
uint32_t fs_unified_getgid(void);

/* ============================================================
 * Compatibility Layer (for legacy code)
 * ============================================================ */

#define vfs_open(path, flags, mode) fs_unified_open(path, flags)
#define vfs_close(fd) fs_unified_close(fd)
#define vfs_read(fd, buf, count) fs_unified_read(fd, buf, count)
#define vfs_write(fd, buf, count) fs_unified_write(fd, buf, count)
#define vfs_seek(fd, offset, whence) fs_unified_seek(fd, offset, whence)
#define vfs_mkdir(path, mode) fs_unified_mkdir(path)
#define vfs_rmdir(path) fs_unified_rmdir(path)
#define vfs_unlink(path) fs_unified_unlink(path)
#define vfs_rename(old, new) fs_unified_rename(old, new)
#define vfs_stat(path, st) fs_unified_stat(path, (fs_unified_stat_t*)(st))

#endif /* FS_UNIFIED_H */
