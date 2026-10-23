#ifndef _COS_STRINGS_H
#define _COS_STRINGS_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
_COS_END
#endif
