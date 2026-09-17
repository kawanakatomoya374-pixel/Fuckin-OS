/**
 * sync.c - Mutexes and counting semaphores.
 *
 * Built directly on scheduler_block()/wait_queue_wake_one(), so waiting
 * threads are truly descheduled rather than busy-spinning.
 */

#include "sync.h"
#include "scheduler.h"
#include "task.h"
#include "memory.h"
#include "serial.h"

/* Returns the wait_queue_t behind an opaque pointer, lazily allocating
 * it on first use. Centralized here so mutex_init()/sem_init() (which
 * may run before a heap is fully ready in some exotic init order) and
 * every blocking operation share one allocation path. */
static wait_queue_t* get_or_alloc_waitqueue(void** slot) {
    if (!*slot) {
        wait_queue_t* wq = (wait_queue_t*)kmalloc(sizeof(wait_queue_t));
        if (wq) {
            wait_queue_init(wq);
        }
        *slot = (void*)wq;
    }
    return (wait_queue_t*)*slot;
}

/* Save RFLAGS and disable interrupts; returns the saved flags so the
 * exact previous interrupt-enable state can be restored (safe even if
 * we are called from a context where interrupts were already off). */
uint64_t sync_irq_save(void) {
    uint64_t flags;
    __asm__ volatile(
        "pushfq\n\t"
        "pop %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    return flags;
}

void sync_irq_restore(uint64_t flags) {
    __asm__ volatile(
        "push %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

/* =====================================================================
 * Mutex
 * ===================================================================== */

void mutex_init(mutex_t* m) {
    if (!m) return;
    m->locked = 0;
    m->owner = NULL;
    m->waiters = NULL;
    get_or_alloc_waitqueue(&m->waiters);
}

void mutex_lock(mutex_t* m) {
    if (!m) return;

    for (;;) {
        uint64_t flags = sync_irq_save();

        if (!m->locked) {
            m->locked = 1;
            m->owner = thread_get_current();
            sync_irq_restore(flags);
            return;
        }

        /* Someone else holds it - block on the waiters queue.
         * scheduler_block() moves the current thread off the run queue,
         * marks it TASK_BLOCKED and immediately reschedules, so by the
         * time control returns here we've genuinely been asleep and were
         * woken by mutex_unlock()'s wait_queue_wake_one() call. We still
         * loop back and re-check m->locked (rather than assuming we now
         * own it) because more than one thread can be waiting and only
         * one of them gets to actually take the lock. */
        thread_t* self = thread_get_current();
        if (!scheduler_block(self, get_or_alloc_waitqueue(&m->waiters))) {
            sync_irq_restore(flags);
            serial_puts("[SYNC] mutex_lock() cannot block before scheduler is running\n");
            __builtin_trap();
        }

        sync_irq_restore(flags);
    }
}

int mutex_trylock(mutex_t* m) {
    if (!m) return 0;

    uint64_t flags = sync_irq_save();
    int acquired = 0;
    if (!m->locked) {
        m->locked = 1;
        m->owner = thread_get_current();
        acquired = 1;
    }
    sync_irq_restore(flags);
    return acquired;
}

void mutex_unlock(mutex_t* m) {
    if (!m) return;

    uint64_t flags = sync_irq_save();
    thread_t* self = thread_get_current();

    if (m->locked && m->owner && self && m->owner != self) {
        sync_irq_restore(flags);
        return;
    }

    m->locked = 0;
    m->owner = NULL;

    /* Hand off to one waiter if any are queued. We deliberately leave
     * m->locked at 0 rather than transferring ownership directly - the
     * woken thread races (fairly, FIFO via the wait queue) with any
     * other caller of mutex_lock()/mutex_trylock() to actually set
     * m->locked back to 1 for itself. */
    wait_queue_wake_one(get_or_alloc_waitqueue(&m->waiters));

    sync_irq_restore(flags);
}

bool mutex_is_locked(mutex_t* m) {
    if (!m) return false;
    return m->locked != 0;
}

/* =====================================================================
 * Semaphore
 * ===================================================================== */

void sem_init(semaphore_t* s, int64_t initial_count) {
    if (!s) return;
    s->count = initial_count;
    s->waiters = NULL;
    get_or_alloc_waitqueue(&s->waiters);
}

void sem_wait(semaphore_t* s) {
    if (!s) return;

    for (;;) {
        uint64_t flags = sync_irq_save();

        if (s->count > 0) {
            s->count--;
            sync_irq_restore(flags);
            return;
        }

        thread_t* self = thread_get_current();
        if (!scheduler_block(self, get_or_alloc_waitqueue(&s->waiters))) {
            sync_irq_restore(flags);
            serial_puts("[SYNC] sem_wait() cannot block before scheduler is running\n");
            __builtin_trap();
        }

        sync_irq_restore(flags);
    }
}

int sem_trywait(semaphore_t* s) {
    if (!s) return 0;

    uint64_t flags = sync_irq_save();
    int acquired = 0;
    if (s->count > 0) {
        s->count--;
        acquired = 1;
    }
    sync_irq_restore(flags);
    return acquired;
}

void sem_post(semaphore_t* s) {
    if (!s) return;

    uint64_t flags = sync_irq_save();

    s->count++;
    /* Wake at most one waiter - it will re-check/decrement count itself
     * (there is no path where count is claimed on the waiter's behalf
     * here), which keeps this correct even if sem_post() races with a
     * fresh sem_wait()/sem_trywait() call from another thread. */
    wait_queue_wake_one(get_or_alloc_waitqueue(&s->waiters));

    sync_irq_restore(flags);
}

int64_t sem_get_count(semaphore_t* s) {
    if (!s) return 0;
    return s->count;
}
