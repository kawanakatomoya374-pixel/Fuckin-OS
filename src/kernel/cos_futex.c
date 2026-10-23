/**
 * cos_futex.c - the kernel half of userland thread synchronisation.
 *
 * WHY A FUTEX RATHER THAN A KERNEL MUTEX OBJECT
 * ---------------------------------------------
 * .c-os programs had threads (`cos_thread_create`) and no way to
 * synchronise them: no mutex, no condition variable, no semaphore, not
 * even a documented atomic. A program with two threads touching the same
 * data had no correct way to be written.
 *
 * The obvious fix is a set of kernel objects - `SYS_MUTEX_CREATE`,
 * `SYS_MUTEX_LOCK` and so on. That is worse than it looks: every
 * lock and unlock becomes a ring transition even when uncontended, which
 * is the overwhelmingly common case, and the kernel ends up owning a
 * table of objects whose lifetime it cannot actually see (a program that
 * exits holding a lock, or frees the memory a lock lives in).
 *
 * A futex inverts that. The lock word lives in the program's own memory
 * and is manipulated with ordinary atomic instructions; the kernel is
 * only involved when a thread actually has to WAIT. An uncontended lock
 * costs one `lock cmpxchg` and no syscall at all. The kernel holds no
 * per-lock state between calls, so there is nothing to leak when a
 * program dies holding one.
 *
 * THE RACE THIS MUST NOT HAVE
 * ---------------------------
 * The classic futex bug is:
 *
 *      thread A: sees the word says "contended", decides to sleep
 *      thread B: releases the lock, wakes waiters - there are none yet
 *      thread A: sleeps, forever
 *
 * FUTEX_WAIT closes it by re-reading the word and comparing it against
 * the value the caller expected, with interrupts disabled, and only
 * blocking if they still match. If B changed the word in the window, the
 * comparison fails and the caller returns immediately to retry. The
 * compare and the block have to be atomic with respect to each other,
 * which is what the irq_save around both provides on a single CPU - and
 * on SMP, what the queue lock provides.
 */
#include "cos_futex.h"
#include "scheduler.h"
#include "task.h"
#include "serial.h"
#include "string.h"
#include "sync.h"
#include "mm/paging.h"
#include "timer.h"

/* Monotonic milliseconds, from the same source SYS_TIME_MS reports.
 * TIMER_TICKS_PER_SEC is 1000, so ticks already are milliseconds - the
 * conversion is written out anyway so this stays correct if that rate
 * is ever changed. */
static uint64_t futex_now_ms(void)
{
    return (get_timer_ticks() * 1000ULL) / TIMER_TICKS_PER_SEC;
}

/* Buckets are fixed and hashed, not allocated per address: a futex has
 * no kernel-side identity and no creation call, so there is nothing to
 * allocate at. Several distinct futexes sharing a bucket is harmless -
 * a waiter that is woken spuriously re-checks its word and goes back to
 * sleep, which every futex user must handle anyway. */
#define COS_FUTEX_BUCKETS 64

typedef struct {
    wait_queue_t queue;
    /* Waiters are matched on (pid, address) at wake time rather than by
     * having one queue per key, so a hash collision costs a spurious
     * wakeup instead of a missed one. */
    bool initialised;
} futex_bucket_t;

static futex_bucket_t g_buckets[COS_FUTEX_BUCKETS];

static unsigned futex_hash(uint64_t pid, uint64_t uaddr)
{
    uint64_t k = (uaddr >> 2) ^ (pid * 0x9E3779B97F4A7C15ULL);
    k ^= k >> 29;
    k *= 0xBF58476D1CE4E5B9ULL;
    k ^= k >> 32;
    return (unsigned)(k % COS_FUTEX_BUCKETS);
}

static futex_bucket_t *bucket_for(uint64_t pid, uint64_t uaddr)
{
    futex_bucket_t *b = &g_buckets[futex_hash(pid, uaddr)];
    if (!b->initialised) {
        wait_queue_init(&b->queue);
        b->initialised = true;
    }
    return b;
}

/* Reads the 32-bit word at a user address, having first checked that it
 * is genuinely a mapped, writable user page.
 *
 * Writable and not merely readable: a futex word is something the
 * program will store to, and accepting a read-only address here would
 * let a caller park threads on a page that can never change - a
 * guaranteed permanent block, arranged through a syscall. */
static bool read_futex_word(uint64_t uaddr, uint32_t *out)
{
    if (uaddr & 3u) return false;                    /* must be aligned */
    if (uaddr == 0 || uaddr >= COS_FUTEX_USER_LIMIT) return false;
    if (!paging_user_range_ok(uaddr, 4, true)) return false;

    uint64_t phys = paging_virt_to_phys(uaddr);
    if (!phys) return false;
    *out = *(volatile uint32_t *)(uintptr_t)(phys + 0xFFFF800000000000ULL);
    return true;
}

int64_t cos_futex_wait(uint64_t uaddr, uint32_t expected, uint64_t timeout_ms)
{
    thread_t *self = thread_get_current();
    process_t *proc = process_get_current();
    if (!self || !proc) return COS_FUTEX_EINVAL;

    uint64_t flags = sync_irq_save();

    uint32_t actual = 0;
    if (!read_futex_word(uaddr, &actual)) {
        sync_irq_restore(flags);
        return COS_FUTEX_EFAULT;
    }

    /* The whole point. If the word no longer holds what the caller saw
     * before deciding to sleep, another thread has already changed it -
     * possibly including the wake that would have been this thread's -
     * so return and let the caller re-evaluate rather than sleep through
     * it. */
    if (actual != expected) {
        sync_irq_restore(flags);
        return COS_FUTEX_EAGAIN;
    }

    futex_bucket_t *b = bucket_for(proc->pid, uaddr);

    /* Recorded so a wake can tell this waiter apart from others sharing
     * the bucket. Stored on the thread rather than in a per-futex
     * structure precisely because there is no per-futex structure. */
    self->futex_addr = uaddr;
    self->futex_woken = false;

    if (timeout_ms) {
        /* A timed wait is expressed as a deadline rather than a
         * countdown: the thread may be woken spuriously and re-block,
         * and a countdown would restart each time. */
        self->futex_deadline = futex_now_ms() + timeout_ms;
    } else {
        self->futex_deadline = 0;
    }

    scheduler_block(self, &b->queue);
    sync_irq_restore(flags);

    /* Back here after a wake. Which kind it was matters to the caller:
     * a timeout is a real outcome for cos_mutex_timedlock(), while a
     * spurious wake just means "re-check and possibly wait again". */
    if (!self->futex_woken && self->futex_deadline &&
        futex_now_ms() >= self->futex_deadline) {
        self->futex_addr = 0;
        return COS_FUTEX_ETIMEDOUT;
    }
    self->futex_addr = 0;
    return 0;
}

int64_t cos_futex_wake(uint64_t uaddr, uint32_t count)
{
    process_t *proc = process_get_current();
    if (!proc) return COS_FUTEX_EINVAL;
    if (uaddr & 3u) return COS_FUTEX_EINVAL;
    if (uaddr == 0 || uaddr >= COS_FUTEX_USER_LIMIT) return COS_FUTEX_EINVAL;

    uint64_t flags = sync_irq_save();
    futex_bucket_t *b = bucket_for(proc->pid, uaddr);

    int64_t woken = 0;

    /* Walks the queue looking for waiters on THIS address. The queue can
     * hold waiters for other futexes that hashed to the same bucket, and
     * waking those would be merely wasteful - but skipping a waiter on
     * the right address would be a lost wakeup, which is a hang. So the
     * walk is exhaustive rather than stopping at the first match when
     * more are requested. */
    thread_t *t = b->queue.head;
    while (t && (uint32_t)woken < count) {
        thread_t *next = t->next;
        if (t->futex_addr == uaddr) {
            t->futex_woken = true;
            wait_queue_remove(&b->queue, t);
            scheduler_unblock(t);
            woken++;
        }
        t = next;
    }

    sync_irq_restore(flags);
    return woken;
}

/* Called from the timer tick. A timed waiter that has passed its
 * deadline is released here; without this, cos_mutex_timedlock() with a
 * timeout and no contender on the other side would never return. */
void cos_futex_expire_timeouts(void)
{
    uint64_t now = futex_now_ms();
    uint64_t flags = sync_irq_save();

    for (unsigned i = 0; i < COS_FUTEX_BUCKETS; ++i) {
        futex_bucket_t *b = &g_buckets[i];
        if (!b->initialised) continue;
        thread_t *t = b->queue.head;
        while (t) {
            thread_t *next = t->next;
            if (t->futex_deadline && now >= t->futex_deadline) {
                t->futex_woken = false;      /* signals a timeout, not a wake */
                wait_queue_remove(&b->queue, t);
                scheduler_unblock(t);
            }
            t = next;
        }
    }
    sync_irq_restore(flags);
}

/* Releases any waiters belonging to a dying process.
 *
 * Without this, a thread blocked in FUTEX_WAIT when its process is
 * killed stays on a kernel queue that nothing will ever wake, holding
 * its thread_t and kernel stack forever. Called from process teardown. */
void cos_futex_release_pid(uint64_t pid)
{
    uint64_t flags = sync_irq_save();
    for (unsigned i = 0; i < COS_FUTEX_BUCKETS; ++i) {
        futex_bucket_t *b = &g_buckets[i];
        if (!b->initialised) continue;
        thread_t *t = b->queue.head;
        while (t) {
            thread_t *next = t->next;
            if (t->pid == pid) {
                t->futex_woken = true;
                wait_queue_remove(&b->queue, t);
                scheduler_unblock(t);
            }
            t = next;
        }
    }
    sync_irq_restore(flags);
}
