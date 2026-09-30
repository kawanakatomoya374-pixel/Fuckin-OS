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
 * we are called from a context where interrupts were already off).
 *
 * Cross-CPU safety: sync_irq_save()/sync_irq_restore() are used all over
 * this kernel (the heap allocator, the page-table code, the scheduler,
 * mutex_t/semaphore_t's original implementation, ...) as if disabling
 * interrupts were sufficient mutual exclusion - true only when C-OS was
 * single-CPU. cli/sti affect only the current core; a second physical
 * CPU (src/kernel/smp.c does boot one on real -smp 2 hardware) keeps
 * running regardless, and can be inside the exact same "protected"
 * section at the same time. That produced reproducible corruption
 * hunting down real hardware: page faults at garbage addresses, "leaf
 * already present" from two CPUs racing to map the same page, and
 * thread_create() failing outright - all while simply opening ordinary
 * .c-os programs.
 *
 * Rather than retrofit a separate real lock into every one of those call
 * sites (large, risky surgery under time pressure, on code this
 * central), sync_irq_save()/restore() themselves now ARE a real
 * cross-CPU spinlock (g_kernel_lock below) - every caller's existing
 * cli-based critical section becomes correctly cross-CPU-exclusive for
 * free. It must be recursive: plenty of existing code calls
 * sync_irq_save() while another sync_irq_save() taken earlier by the
 * very same call stack (on the very same CPU) is still held - e.g.
 * mutex_lock() holds one across its call into scheduler_block(), which
 * takes its own. A plain non-recursive spinlock would have the first
 * CPU deadlock against itself the moment any such nested call ran.
 * Recursion is detected by APIC ID (smp.c's own way of naming a CPU),
 * not by thread, because the property being protected is "no other
 * *core* is concurrently inside a cli-protected section" - a context
 * switch to a different thread on the SAME core cannot itself race
 * with the section this same core is already in the middle of. */
static volatile int      g_kernel_lock = 0;
static volatile uint32_t g_kernel_lock_owner = 0xFFFFFFFFu;   /* APIC ID, valid only while locked */
static volatile uint32_t g_kernel_lock_depth = 0;

static inline uint32_t sync_apic_id(void) {
    uint32_t eax = 1, ebx, ecx = 0, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return (ebx >> 24) & 0xFFu;
}

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

    uint32_t me = sync_apic_id();
    if (g_kernel_lock && g_kernel_lock_owner == me) {
        /* Recursive: this exact core already holds it. Aligned 32-bit
         * reads/writes are atomic on x86, and only the actual owning core
         * can ever make this comparison true, so no further ordering is
         * needed to trust it. */
        ++g_kernel_lock_depth;
        return flags;
    }
    while (__sync_lock_test_and_set(&g_kernel_lock, 1)) {
        while (g_kernel_lock) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    g_kernel_lock_owner = me;
    g_kernel_lock_depth = 1;
    return flags;
}

void sync_irq_restore(uint64_t flags) {
    if (g_kernel_lock_depth > 1) {
        --g_kernel_lock_depth;
    } else {
        g_kernel_lock_depth = 0;
        g_kernel_lock_owner = 0xFFFFFFFFu;
        __sync_lock_release(&g_kernel_lock);
    }

    __asm__ volatile(
        "push %0\n\t"
        "popfq"
        :
        : "r"(flags)
        : "memory", "cc"
    );
}

/* =====================================================================
 * Spinlock - the real cross-CPU primitive; see sync.h's header comment
 * for why sync_irq_save() alone was never sufficient once smp.c started
 * booting a second CPU.
 * ===================================================================== */
void spinlock_init(spinlock_t* l) {
    if (!l) return;
    l->locked = 0;
}

uint64_t spin_lock_irqsave(spinlock_t* l) {
    uint64_t flags = sync_irq_save();
    if (!l) return flags;
    /* __sync_lock_test_and_set compiles to a LOCK-prefixed XCHG on x86:
     * atomic across every CPU sharing this memory, not just this core.
     * The pause instruction is the standard spin-wait hint (reduces
     * power/bus traffic and is friendlier to the other logical thread
     * on the same core while we wait). */
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    return flags;
}

void spin_unlock_irqrestore(spinlock_t* l, uint64_t flags) {
    if (l) {
        __sync_lock_release(&l->locked);
    }
    sync_irq_restore(flags);
}

bool spin_trylock_irqsave(spinlock_t* l, uint64_t* out_flags) {
    uint64_t flags = sync_irq_save();
    if (!l) { if (out_flags) *out_flags = flags; return true; }
    if (__sync_lock_test_and_set(&l->locked, 1)) {
        sync_irq_restore(flags);
        return false;
    }
    if (out_flags) *out_flags = flags;
    return true;
}

/* For scheduler_do_context_switch() only - see its own comment and the
 * one on thread_t.kernel_lock_depth_saved. Must be called with interrupts
 * already disabled on this core (true for every call site: it always
 * runs deep inside a sync_irq_save()'d section). */
uint32_t sync_kernel_lock_release_for_switch(void) {
    uint32_t saved = g_kernel_lock_depth;
    g_kernel_lock_depth = 0;
    g_kernel_lock_owner = 0xFFFFFFFFu;
    __sync_lock_release(&g_kernel_lock);
    return saved;
}
void sync_kernel_lock_reacquire_after_switch(uint32_t saved_depth) {
    if (saved_depth == 0) return;
    uint32_t me = sync_apic_id();
    while (__sync_lock_test_and_set(&g_kernel_lock, 1)) {
        while (g_kernel_lock) {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    g_kernel_lock_owner = me;
    g_kernel_lock_depth = saved_depth;
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
