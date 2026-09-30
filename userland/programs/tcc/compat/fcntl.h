/* fcntl.h - just enough for TinyCC's open(). The values are the ones
 * cos_open()'s flag translation in tcc_shim.c understands. */
#ifndef _COS_FCNTL_H
#define _COS_FCNTL_H
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_BINARY 0
int open(const char *path, int flags, ...);
#endif
