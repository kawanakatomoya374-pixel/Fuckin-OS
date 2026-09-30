/* A host implementation of the futex syscalls, so cos_sync.c can be
 * exercised with REAL concurrent threads.
 *
 * Linux has a futex with the same semantics, so the stub is a thin
 * forward rather than a simulation. That matters: a simulated futex
 * would be written by the same person who wrote the locks and would
 * share their misconceptions. Forwarding to the kernel's means the locks
 * are tested against genuine preemption, genuine memory ordering and
 * genuine lost-wakeup windows. */
#define _GNU_SOURCE
#include <stdint.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

int64_t cos_futex_wait(volatile uint32_t *addr, uint32_t expected,
                       uint64_t timeout_ms)
{
    struct timespec ts, *tp = NULL;
    if (timeout_ms) {
        ts.tv_sec = (time_t)(timeout_ms / 1000);
        ts.tv_nsec = (long)((timeout_ms % 1000) * 1000000L);
        tp = &ts;
    }
    long r = syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAIT_PRIVATE,
                     expected, tp, NULL, 0);
    if (r == 0) return 0;
    if (errno == EAGAIN) return -11;
    if (errno == ETIMEDOUT) return -110;
    return 0;                      /* EINTR and friends: "look again" */
}

int64_t cos_futex_wake(volatile uint32_t *addr, uint32_t count)
{
    return syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAKE_PRIVATE, count,
                   NULL, NULL, 0);
}

int64_t cos_gettid(void) { return (int64_t)syscall(SYS_gettid); }

uint64_t cos_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}
