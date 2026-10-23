#ifndef _COS_DIRENT_H
#define _COS_DIRENT_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
typedef struct __cos_DIR DIR;
struct dirent { char d_name[256]; unsigned char d_type; unsigned long d_ino; };
#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
void rewinddir(DIR *d);
_COS_END
#endif
