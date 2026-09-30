#ifndef _COS_SYS_STAT_H
#define _COS_SYS_STAT_H
#include <sys/types.h>
struct stat { unsigned long st_size; unsigned int st_mode; };
#define S_IFDIR 0040000
#define S_ISDIR(m) (((m) & S_IFDIR) != 0)
int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int mkdir(const char *path, unsigned mode);
int chmod(const char *path, unsigned mode);
#endif
