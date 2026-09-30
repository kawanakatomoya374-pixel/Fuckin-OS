#ifndef SYNC_H
#define SYNC_H

#include "types.h"
#include "task.h"      /* thread_t */

/*
 * sync.h - Spinlocks, mutexes and counting semaphores.
 *
 * IMPORTANT HISTORY: every primitive below used to be protected only by
 * sync_irq_save()/sync_irq_restore() (cli/sti), on the documented
 * assumption that C-OS was single-CPU - true when this file was written,
 * but no longer true once src/kernel/smp.c learned to boot secondary
 * processors. cli/sti only stops interrupts and rescheduling on the
 * *current* CPU; it does nothing to a second physical CPU core, which
 * keeps executing (and can be inside this exact same function,
 * concurrently) regardless of CPU 0's interrupt flag. Under real -smp 2
 * this let two CPUs both see a lock as free and both take it, corrupting
 * the kernel heap and page tables at once - reported as reproducible
 * crashes (page faults at garbage addresses, "map_page: leaf already
 * present", thread_create() failing) when opening ordinary .c-os
 * programs. cli/sti alone was never a cross-CPU exclusion mechanism.
 *
 * spinlock_t is the fix: a real atomic test-and-set, safe across CPUs,
 * with interrupts also disabled on the current CPU while held (so a
 * timer interrupt on the CPU already holding the lock cannot deadlock by
 * trying to re-enter it). mutex_t and semaphore_t are now built on it
 * instead of bare sync_irq_save(), and the kernel heap allocator and
 * page-table code (memory.c, mm/paging.c) take a spinlock_t of their own
 * for the same reason.
 */

typedef struct {
    volatile int locked;   /* accessed only through the atomic ops below */
} spinlock_t;

#define SPINLOCK_INIT { 0 }
void     spinlock_init(spinlock_t* l);
/* Disables interrupts on the current CPU, then spins (on that CPU) until
 * the lock is acquired. Returns the flags to pass to spin_unlock_irqrestore(). */
uint64_t spin_lock_irqsave(spinlock_t* l);
void     spin_unlock_irqrestore(spinlock_t* l, uint64_t flags);
/* Non-blocking: returns true and disables interrupts (flags out-param) if
 * acquired, false (interrupt state unchanged) if the lock was already held. */
bool     spin_trylock_irqsave(spinlock_t* l, uint64_t* flags);

typedef struct {
    volatile int locked;      /* 0 = free, 1 = held - see sync_irq_save()/restore() in sync.c: they are now the real cross-CPU lock this and semaphore_t rely on */
    thread_t*    owner;       /* thread currently holding the mutex, or NULL */
    void*        waiters;     /* opaque wait_queue_t*, see note above */
} mutex_t;

typedef struct {
    volatile int64_t count;   /* current semaphore value */
    void*             waiters; /* opaque wait_queue_t*, see note above */
} semaphore_t;


/* Mutex API */
void mutex_init(mutex_t* m);
void mutex_lock(mutex_t* m);
int  mutex_trylock(mutex_t* m);   /* returns 1 if acquired, 0 if already held */
void mutex_unlock(mutex_t* m);
bool mutex_is_locked(mutex_t* m);

/* Semaphore API */
void sem_init(semaphore_t* s, int64_t initial_count);
void sem_wait(semaphore_t* s);           /* P() / down(), blocks while count <= 0 */
int  sem_trywait(semaphore_t* s);        /* returns 1 if acquired, 0 if would block */
void sem_post(semaphore_t* s);           /* V() / up(), wakes one waiter if any */
int64_t sem_get_count(semaphore_t* s);

/* Low-level IRQ control */
uint64_t sync_irq_save(void);
void sync_irq_restore(uint64_t flags);

/* For scheduler_do_context_switch() only - see the long comment beside
 * its call to these in scheduler.c. Everywhere else, use
 * sync_irq_save()/sync_irq_restore() (or a mutex/spinlock), never these. */
uint32_t sync_kernel_lock_release_for_switch(void);
void     sync_kernel_lock_reacquire_after_switch(uint32_t saved_depth);

#endif /* SYNC_H */
