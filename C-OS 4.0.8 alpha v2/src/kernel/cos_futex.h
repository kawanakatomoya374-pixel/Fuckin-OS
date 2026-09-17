/**
 * cos_futex.h - the kernel wait/wake primitive userland locks are built on.
 *
 * See cos_futex.c for why this shape rather than kernel mutex objects.
 * In short: the lock word lives in the program's memory and the kernel
 * is only involved when a thread must actually sleep, so an uncontended
 * lock costs one atomic instruction and no ring transition, and the
 * kernel holds no per-lock state that could leak when a program dies.
 */
#ifndef COS_FUTEX_H
#define COS_FUTEX_H

#include <stdint.h>
#include <stdbool.h>

#define COS_FUTEX_USER_LIMIT 0x0000800000000000ULL

/* Negative returns, chosen to match the errno values a caller would
 * expect so a future POSIX shim does not have to translate them. */
#define COS_FUTEX_EAGAIN    (-11)  /* the word no longer held `expected` */
#define COS_FUTEX_EFAULT    (-14)  /* not a mapped, writable user word   */
#define COS_FUTEX_EINVAL    (-22)
#define COS_FUTEX_ETIMEDOUT (-110)

/* Sleeps until woken, IF the 32-bit word at `uaddr` still equals
 * `expected`. Returns 0 on a wake, COS_FUTEX_EAGAIN if the word had
 * already changed (the caller must re-check and retry), or
 * COS_FUTEX_ETIMEDOUT. A timeout of 0 means wait indefinitely.
 *
 * A return of 0 does NOT mean the condition the caller is waiting for
 * became true - spurious wakeups are possible and every caller must
 * re-check. That is not a defect to work around; it is what lets the
 * wake side stay a single atomic store plus an unconditional syscall. */
int64_t cos_futex_wait(uint64_t uaddr, uint32_t expected, uint64_t timeout_ms);

/* Wakes at most `count` threads waiting on `uaddr`. Returns how many. */
int64_t cos_futex_wake(uint64_t uaddr, uint32_t count);

/* Timer-tick hook: releases waiters whose deadline has passed. */
void cos_futex_expire_timeouts(void);

/* Process-teardown hook: releases waiters belonging to a dying process,
 * which would otherwise sit on a kernel queue forever. */
void cos_futex_release_pid(uint64_t pid);

#endif /* COS_FUTEX_H */
