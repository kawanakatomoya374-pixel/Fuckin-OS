#ifndef _COS_FCNTL_H
#define _COS_FCNTL_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_ACCMODE  3
#define O_CREAT    0x40
#define O_EXCL     0x80
#define O_TRUNC    0x200
#define O_APPEND   0x400
#define O_NONBLOCK 0x800
#define O_CLOEXEC  0x80000
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
int open(const char *path, int flags, ...);
int creat(const char *path, unsigned mode);
int fcntl(int fd, int cmd, ...);
_COS_END
#endif
