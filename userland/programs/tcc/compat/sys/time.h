#ifndef _COS_SYS_TIME_H
#define _COS_SYS_TIME_H
#include <time.h>
struct timeval { long tv_sec; long tv_usec; };
int gettimeofday(struct timeval *tv, void *tz);
#endif
