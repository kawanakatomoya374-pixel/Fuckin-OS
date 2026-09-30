/**
 * cos_fd.h - per-process file descriptor table for ring3 programs.
 *
 * Handles are 1-based (0 is never valid). Every function takes the
 * calling process's pid explicitly and checks ownership internally, so a
 * caller cannot access another process's descriptor by guessing a small
 * integer.
 */
#ifndef COS_FD_H
#define COS_FD_H

#include <stdint.h>
#include <stdbool.h>

/* Small, stable open-mode ABI exposed to ring3 - deliberately NOT FatFs's
 * own FA_* bits or POSIX's O_* bits, both of which are external library/
 * OS conventions that could change independently of what a .c-os program
 * needs to express. */
#define COS_O_RDONLY        1   /* must already exist */
#define COS_O_WRONLY_CREATE 2   /* truncate-or-create, write only */
#define COS_O_WRONLY_APPEND 3   /* create if missing, write at end */
#define COS_O_RDWR_CREATE   4   /* create if missing, read+write, keep contents */

#define COS_DIRENT_NAME_MAX 255

typedef struct {
    char     name[COS_DIRENT_NAME_MAX + 1];
    uint8_t  is_dir;
    uint64_t size;
} cos_dirent_t;

int64_t cos_fd_open(uint64_t pid, const char *path, uint32_t cos_flags);
int64_t cos_fd_opendir(uint64_t pid, const char *path);
int64_t cos_fd_read(uint64_t pid, int64_t fd, void *kernel_dst, uint64_t len);
int64_t cos_fd_write(uint64_t pid, int64_t fd, const void *kernel_src, uint64_t len);
int64_t cos_fd_lseek(uint64_t pid, int64_t fd, int64_t offset);
/* Returns 1 with `out` filled for a real entry, 0 at end of directory
 * (a real, distinct outcome from FatFs - not an error), or -1 on error. */
int64_t cos_fd_readdir(uint64_t pid, int64_t fd, cos_dirent_t *out);
bool    cos_fd_close(uint64_t pid, int64_t fd);

/* Called from process_exit() so a process's open files/directories do not
 * leak past its own lifetime. */
void cos_fd_close_all_for_pid(uint64_t pid);

#endif /* COS_FD_H */
