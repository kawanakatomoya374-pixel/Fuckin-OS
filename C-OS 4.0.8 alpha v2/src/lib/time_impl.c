/**
 * time_impl.c - real implementations for time.h/sys/time.h, backed by
 * kernel/rtc.c (wall-clock seconds since the Unix epoch) and
 * hal_timer_get_ms() (monotonic milliseconds, the same counter the
 * scheduler already uses - see hal_api.h). Needed for real by
 * QuickJS's Date object support and content/llcache.c's cache-expiry
 * timestamps (see time.h's file header for the full story of why
 * this didn't exist before).
 */
#include "time.h"
#include "sys/time.h"
#include "hal_api.h"
#include "rtc.h"

time_t time(time_t *tloc)
{
    time_t now = (time_t)rtc_get_seconds_since_epoch();
    if (tloc != NULL) {
        *tloc = now;
    }
    return now;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (tv == NULL) {
        return -1;
    }
    uint64_t ms = hal_timer_get_ms();
    tv->tv_sec = (time_t)rtc_get_seconds_since_epoch();
    tv->tv_usec = (long)((ms % 1000) * 1000);
    return 0;
}

int clock_gettime(int clk_id, struct timespec *tp)
{
    if (tp == NULL) {
        return -1;
    }
    if (clk_id == CLOCK_MONOTONIC) {
        uint64_t ms = hal_timer_get_ms();
        tp->tv_sec = (time_t)(ms / 1000);
        tp->tv_nsec = (long)((ms % 1000) * 1000000L);
    } else {
        uint64_t ms = hal_timer_get_ms();
        tp->tv_sec = (time_t)rtc_get_seconds_since_epoch();
        tp->tv_nsec = (long)((ms % 1000) * 1000000L);
    }
    return 0;
}

/* Days since the epoch (1970-01-01) -> proleptic Gregorian
 * year/month/day. Standard, well-known technique (Howard Hinnant's
 * "chrono-Compatible Low-Level Date Algorithms" - a public algorithm,
 * not copied from any specific codebase); correct for the entire
 * range any real Unix timestamp can represent. */
static void cos_civil_from_days(long z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);          /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; /* [0, 399] */
    long year = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365] */
    unsigned mp = (5 * doy + 2) / 153;                      /* [0, 11] */
    *d = doy - (153 * mp + 2) / 5 + 1;                      /* [1, 31] */
    *m = mp < 10 ? mp + 3 : mp - 9;                         /* [1, 12] */
    *y = (int)(year + (*m <= 2 ? 1 : 0));
}

static struct tm *cos_time_to_tm(const time_t *timer, struct tm *result)
{
    long t = (long)*timer;
    long days = t / 86400;
    long secs_of_day = t % 86400;
    if (secs_of_day < 0) {
        secs_of_day += 86400;
        days -= 1;
    }

    int y;
    unsigned m, d;
    cos_civil_from_days(days, &y, &m, &d);

    result->tm_year = y - 1900;
    result->tm_mon = (int)m - 1;
    result->tm_mday = (int)d;
    result->tm_hour = (int)(secs_of_day / 3600);
    result->tm_min = (int)((secs_of_day % 3600) / 60);
    result->tm_sec = (int)(secs_of_day % 60);
    /* 1970-01-01 was a Thursday (wday 4). */
    long wday = (days % 7 + 4 + 7) % 7;
    result->tm_wday = (int)wday;
    result->tm_yday = 0; /* not computed - nothing in this tree reads it */
    result->tm_isdst = 0;
    return result;
}

struct tm *gmtime_r(const time_t *timer, struct tm *result)
{
    return cos_time_to_tm(timer, result);
}

/* See time.h's doc comment: no timezone database, so local == UTC. */
struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    return cos_time_to_tm(timer, result);
}

/* mktime() - convert struct tm to time_t.
 * C-OS 4.0.8 alpha: added for QuickJS's getTimezoneOffset() which uses
 * mktime() + difftime() in the NO_TM_GMTOFF path.
 * Since C-OS has no timezone database, local time == UTC, so mktime()
 * is the inverse of gmtime_r(): convert the broken-down UTC time back
 * to a Unix timestamp. */
time_t mktime(struct tm *tm)
{
    if (!tm) return (time_t)-1;

    /* Normalise month/year first (handle values outside [0,11]/[0,...]). */
    int year  = tm->tm_year + 1900;
    int month = tm->tm_mon;      /* 0-based */
    while (month < 0)  { month += 12; year--; }
    while (month > 11) { month -= 12; year++; }

    /* Days since epoch for the start of the given year. */
    long days = 0;
    for (int y = 1970; y < year; y++) {
        days += ((y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365);
    }

    /* Days in each month (non-leap). */
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (int m = 0; m < month; m++) {
        days += mdays[m];
        if (m == 1 && leap) days++;
    }
    days += tm->tm_mday - 1;

    time_t t = (time_t)days * 86400
             + (time_t)tm->tm_hour * 3600
             + (time_t)tm->tm_min  * 60
             + (time_t)tm->tm_sec;

    /* Update the struct to reflect the normalised values. */
    cos_time_to_tm(&t, tm);
    return t;
}

/* difftime() - difference between two time_t values in seconds.
 * Simple subtraction is correct for Unix timestamps. */
double difftime(time_t time1, time_t time0)
{
    return (double)(time1 - time0);
}
