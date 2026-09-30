#ifndef _COS_CTYPE_H
#define _COS_CTYPE_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
int isalpha(int c); int isdigit(int c); int isalnum(int c); int isspace(int c);
int isupper(int c); int islower(int c); int isxdigit(int c); int ispunct(int c);
int isprint(int c); int iscntrl(int c); int isgraph(int c); int isblank(int c);
int toupper(int c); int tolower(int c);
_COS_END
#endif
