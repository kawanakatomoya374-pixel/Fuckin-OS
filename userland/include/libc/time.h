#ifndef _COS_TIME_H
#define _COS_TIME_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
typedef long long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000L
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst; };
struct timespec { time_t tv_sec; long tv_nsec; };
time_t     time(time_t *t);
clock_t    clock(void);
double     difftime(time_t a, time_t b);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t     mktime(struct tm *tm);
size_t     strftime(char *buf, size_t max, const char *fmt, const struct tm *tm);
char      *asctime(const struct tm *tm);
char      *ctime(const time_t *t);
_COS_END
#endif
