#ifndef SYNC_H
#define SYNC_H

#include "types.h"
#include "task.h"      /* thread_t */

/*
 * sync.h - Mutexes and counting semaphores.
 *
 * These are the missing synchronization primitives that turn the
 * scheduler's raw block/unblock machinery into something real code can
 * use to coordinate: mutual exclusion around shared data structures
 * (e.g. the IPC mailboxes in ipc.c) and producer/consumer signalling.
 *
 * Both types are safe to use from kernel code and are built entirely on
 * scheduler_block()/wait_queue_wake_one(), so a thread that has to wait
 * is genuinely taken off the CPU (not spin-waiting) and is woken up by
 * exactly the mechanism the scheduler already provides.
 *
 * Because this kernel is single-CPU, the "lock" around each primitive's
 * own internal state is a short interrupts-disabled critical section
 * (see sync_irq_save/sync_irq_restore in sync.c) rather than a spinlock -
 * there is no second CPU that could be spinning on it, the only other
 * party that could touch the state concurrently is the timer interrupt
 * (which could preempt us mid-update), so disabling interrupts for the
 * few instructions it takes to update a counter/queue is sufficient and
 * cheap.
 *
 * NOTE on the opaque `void* waiters`: scheduler.h used to exist as two
 * different, non-identical copies (src/include/scheduler.h and
 * src/kernel/scheduler.h). They have since been unified into the single
 * canonical src/include/scheduler.h (the fuller definition that
 * scheduler.c/task.c actually implement against); the stale
 * src/kernel/scheduler.h copy was removed. This header still keeps
 * mutex_t/semaphore_t's `waiters` as an opaque pointer rather than an
 * embedded wait_queue_t, since sync.h is included from public-facing
 * code that shouldn't need the full scheduler.h layout. sync.c owns the
 * concrete wait_queue_t allocation behind that pointer.
 */

typedef struct {
    volatile int locked;      /* 0 = free, 1 = held */
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

#endif /* SYNC_H */
