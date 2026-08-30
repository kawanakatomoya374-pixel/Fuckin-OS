/**
 * sys/time.h - minimal shim providing struct timeval/gettimeofday()
 * and struct timespec/clock_gettime() (the latter is conventionally
 * declared in <time.h> on real systems, but every caller in this
 * tree that wants it also wants gettimeofday(), so both live here
 * together rather than splitting one real need across two files).
 * See time.h's file header for why this exists as a real shim now
 * instead of silently falling through to the host sandbox's real
 * system header.
 */
#ifndef COS_SYS_TIME_H
#define COS_SYS_TIME_H

/* Include the kernel's own time.h shim which defines time_t.
 * Use <time.h> (angle brackets) so the include path resolves to
 * src/include/time.h via -Isrc/include, not a relative path. */
#include <time.h>

struct timeval {
    time_t tv_sec;
    long   tv_usec;
};

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* Real: tv_sec from kernel/rtc.c, tv_usec derived from
 * hal_timer_get_ms()'s sub-second component (millisecond
 * resolution, not true microsecond - documented, not silently
 * imprecise). `tz` is accepted for signature compatibility and
 * always ignored, matching every modern POSIX implementation
 * (timezone-via-gettimeofday has been unsupported/undefined
 * behaviour there for decades too). */
int gettimeofday(struct timeval *tv, void *tz);

/* Real for both clocks: CLOCK_MONOTONIC from hal_timer_get_ms() (the
 * same counter the scheduler uses), CLOCK_REALTIME from the same
 * source as time()/gettimeofday() above. */
int clock_gettime(int clk_id, struct timespec *tp);

#endif /* COS_SYS_TIME_H */
