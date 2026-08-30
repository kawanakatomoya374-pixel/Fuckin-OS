/*
 * time_cos.c - C-OS implementation of libnsutils' nsu_getmonotonic_ms().
 *
 * Upstream's own src/time.c wraps real POSIX time sources
 * (clock_gettime/CLOCK_MONOTONIC, or a handful of platform-specific
 * fallbacks) - none of which exist in this freestanding kernel. This
 * is a from-scratch replacement (not a port of that file) backed by
 * C-OS's own HAL timer, which already maintains a monotonic
 * millisecond tick count for the scheduler (see kernel/timer.c).
 *
 * Only this one function from libnsutils/include/nsutils/time.h is
 * implemented, since it's the only one anything in this tree calls
 * (content/handlers/html/html.c, for parse-time logging) - see
 * PORTING_NOTES.md for why the rest of libnsutils (time.c/unistd.c as
 * upstream wrote them) isn't part of this build.
 */
#include <nsutils/time.h>
#include "hal_api.h"

nsuerror nsu_getmonotonic_ms(uint64_t *current)
{
    if (current == NULL) {
        return NSUERROR_BAD_PARAMETER;
    }
    *current = hal_timer_get_ms();
    return NSUERROR_OK;
}
