#ifndef _COS_SYS_TIME_H
#define _COS_SYS_TIME_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
#include "types.h"
#include "../time.h"
struct timeval { time_t tv_sec; long tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };
int gettimeofday(struct timeval *tv, struct timezone *tz);
_COS_END
#endif
