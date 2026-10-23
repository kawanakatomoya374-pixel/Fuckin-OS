#ifndef _COS_SYS_STAT_H
#define _COS_SYS_STAT_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include "types.h"
typedef unsigned mode_t;
struct stat { mode_t st_mode; off_t st_size; long st_mtime; long st_atime; long st_ctime; int st_nlink; int st_uid; int st_gid; long st_blksize; long st_blocks; };
#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, mode_t mode);
int chmod(const char *path, mode_t mode);
_COS_END
#endif
