#ifndef _COS_SYS_UTSNAME_H
#define _COS_SYS_UTSNAME_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
struct utsname { char sysname[65]; char nodename[65]; char release[65]; char version[65]; char machine[65]; };
int uname(struct utsname *u);
_COS_END
#endif
