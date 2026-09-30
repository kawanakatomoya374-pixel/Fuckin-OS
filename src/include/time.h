/**
 * time.h - minimal <time.h> shim.
 *
 * There was no time.h in this libc shim before (grep the tree - it
 * genuinely didn't exist). Any ported code that did `#include
 * <time.h>` was silently falling through to the *host sandbox's*
 * real system time.h (this environment doesn't build with
 * -nostdinc), which supplied plausible-looking declarations that
 * happily compiled against - but nothing backing them at link time,
 * since this kernel obviously isn't linked against host glibc. That
 * combination (compiles fine, fails only at final link, often much
 * later than the file that pulled the header in) is exactly the kind
 * of confusing failure worth writing down: if something *else* later
 * fails to compile with time-related errors that don't make sense
 * given this file, it's worth checking whether the include path
 * order changed and it's newly seeing the host header again instead
 * of this one.
 *
 * Scope: exactly what QuickJS's Date support and content/llcache.c's
 * cache-expiry timestamps need (see lib/time_impl.c for the real
 * implementations, backed by kernel/rtc.c + hal_timer_get_ms()) -
 * not a complete <time.h>.
 */
#ifndef COS_TIME_H
#define COS_TIME_H

#include <stdint.h>

typedef long time_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/* Real: backed by kernel/rtc.c's rtc_get_seconds_since_epoch(). */
time_t time(time_t *tloc);

/* Honest limitation, not a stub: there's no timezone database here,
 * so "local" time is treated as UTC. Correct for any system whose
 * local timezone genuinely is UTC, and a documented simplification
 * rather than a silent wrong answer everywhere else. */
struct tm *localtime_r(const time_t *timer, struct tm *result);
struct tm *gmtime_r(const time_t *timer, struct tm *result);

/* C-OS 4.0.8 alpha: added for QuickJS Date support (getTimezoneOffset).
 * mktime() converts a broken-down UTC struct tm back to a time_t.
 * difftime() returns the difference between two time_t values in seconds. */
time_t mktime(struct tm *tm);
double difftime(time_t time1, time_t time0);

#endif /* COS_TIME_H */
