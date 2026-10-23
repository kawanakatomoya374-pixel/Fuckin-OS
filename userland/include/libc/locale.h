#ifndef _COS_LOCALE_H
#define _COS_LOCALE_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#define LC_ALL 6
#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
char *setlocale(int cat, const char *loc);
_COS_END
#endif
