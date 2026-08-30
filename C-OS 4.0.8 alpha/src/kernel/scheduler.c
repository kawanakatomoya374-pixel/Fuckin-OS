/**
 * scheduler.c - Task Scheduler Implementation
 * 
 * Priority-based preemptive round-robin scheduler.
 */

#include "scheduler.h"
#include "idt.h"
#include "task.h"
#include "sync.h"
#include "hal_api.h"
#include "serial.h"
#include "string.h"
#include "mm/paging.h"

// Function declarations to resolve undefined references
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dst, const void* src, size_t n);
void serial_puts(const char* s);
void serial_putdec(uint64_t n);

static void scheduler_do_context_switch(thread_t* prev, thread_t* next);

#include "../include/gdt.h"

static thread_t* current_thread = NULL;
/* Set by scheduler_capture_interrupt_context() when the *true* register
 * state of current_thread (rip/rsp/rflags/cs/ss and all GPRs) has just
 * been captured from a genuine interrupt frame. See the long comment
 * on scheduler_do_context_switch() for why this matters. */
static bool current_thread_ctx_precaptured = false;
/* current_process is managed by task.c */
static thread_t* sleep_list = NULL;

static inline void scheduler_fpu_init(thread_t* thread) {
    if (!thread) return;
    __asm__ volatile("fninit\n\tfxsave64 %0" : "=m"(thread->fpu_state) :: "memory");
}

static inline void scheduler_fpu_save(thread_t* thread) {
    if (!thread) return;
    __asm__ volatile("fxsave64 %0" : "=m"(thread->fpu_state) :: "memory");
}

static inline void scheduler_fpu_restore(const thread_t* thread) {
    if (!thread) return;
    __asm__ volatile("fxrstor64 %0" :: "m"(thread->fpu_state) : "memory");
}

static bool detach_linked_thread(thread_t** head, thread_t** tail, int* count, thread_t* thread) {
    if (!head || !tail || !count || !thread) {
        return false;
    }
    thread_t* cur = *head;
    while (cur) {
        if (cur == thread) {
            if (cur->prev) {
                cur->prev->next = cur->next;
            } else {
                *head = cur->next;
            }
            if (cur->next) {
                cur->next->prev = cur->prev;
            } else {
                *tail = cur->prev;
            }
            if (*count > 0) {
                (*count)--;
            }
            cur->next = NULL;
            cur->prev = NULL;
            return true;
        }
        cur = cur->next;
    }
    return false;
}

static bool detach_sleep_thread(thread_t* thread) {
    if (!thread) {
        serial_puts("[SCHED] WARNING: scheduler_block() called before the "
                     "first thread switch. The caller will keep polling "
                     "instead of blocking because no schedulable thread "
                     "exists yet.\n");
        return false;
    }
    thread_t* cur = sleep_list;
    while (cur) {
        if (cur == thread) {
            if (cur->prev) {
                cur->prev->next = cur->next;
            } else {
                sleep_list = cur->next;
            }
            if (cur->next) {
                cur->next->prev = cur->prev;
            }
            cur->next = NULL;
            cur->prev = NULL;
            return true;
        }
        cur = cur->next;
    }
    return false;
}


static inline uint64_t scheduler_priority_value(const thread_t* thread) {
    return thread ? (thread->priority & 0xFFFFFFFFULL) : 0ULL;
}

/* Return the currently running thread (used by task.c) */
thread_t* scheduler_get_current_thread(void) {
    return current_thread;
}


#include "io.h"

/* External functions */

/* Run queues - one per priority level */
static run_queue_t run_queues[SCHED_NUM_QUEUES];

/* Current running task */

/* Idle thread */
static thread_t* idle_thread = NULL;

/* Sleeping tasks list */
/* Blocked tasks */
static wait_queue_t blocked_queue;

/* Statistics */
static sched_stats_t stats = {0};

void scheduler_note_task_created(void) {
    uint64_t flags = sync_irq_save();
    stats.tasks_created++;
    sync_irq_restore(flags);
}

void scheduler_note_task_destroyed(void) {
    uint64_t flags = sync_irq_save();
    stats.tasks_destroyed++;
    sync_irq_restore(flags);
}

/* Configuration */
static sched_config_t config = {
    .enable_preemption = 0,  /* cooperative by default - see scheduler.h */
    .enable_priority_boost = 1,
    .boost_interval = 10000,  // 10 seconds at 1000Hz (1 tick == 1 ms)
    .quantum_min = 5,
    .quantum_max = 100
};

/* Scheduling state */
static int scheduler_running = 0;
static uint64_t current_tick = 0;
static uint64_t last_boost_tick = 0;

/**
 * Initialize scheduler
 */
void scheduler_init(void) {
    serial_puts("[SCHED] Initializing scheduler...\n");
    
    /* Clear run queues */
    for (int i = 0; i < SCHED_NUM_QUEUES; i++) {
        run_queues[i].head = NULL;
        run_queues[i].tail = NULL;
        run_queues[i].count = 0;
    }
    
    /* Initialize blocked queue */
    wait_queue_init(&blocked_queue);
    
    sleep_list = NULL;
    current_thread = NULL;
    
    /* Reset statistics */
    memset(&stats, 0, sizeof(stats));
    
    serial_puts("[SCHED] Scheduler initialized.\n");
    serial_puts("[SCHED] Priority levels: ");
    serial_putdec(SCHED_NUM_QUEUES);
    serial_puts("\n");
}

/**
 * Start scheduler
 */
void scheduler_start(void) {
    scheduler_running = 1;
    serial_puts("[SCHED] Scheduler started. Switching to first task...\n");
    
    /* Trigger the first context switch. 
     * scheduler_schedule() will pick the highest priority task (usually 
     * the first kernel thread created or idle) and switch to it. */
    scheduler_schedule();
}

/**
 * Stop scheduler
 */
void scheduler_stop(void) {
    scheduler_running = 0;
    serial_puts("[SCHED] Scheduler stopped.\n");
}

/**
 * Switch between cooperative and preemptive mode at runtime.
 * Going cooperative -> preemptive re-arms a fresh timeslice for the
 * currently running thread so it doesn't inherit a stale/expired
 * value from before preemption was off.
 */
void scheduler_set_preemption(int enable) {
    uint64_t flags = sync_irq_save();
    config.enable_preemption = enable ? 1 : 0;
    if (config.enable_preemption && current_thread) {
        scheduler_reset_timeslice(current_thread);
    }
    serial_puts(config.enable_preemption
        ? "[SCHED] Preemption ENABLED.\n"
        : "[SCHED] Preemption DISABLED (cooperative mode).\n");
    sync_irq_restore(flags);
}

int scheduler_get_preemption(void) {
    return config.enable_preemption;
}

/**
 * Get priority queue index (0 = highest priority)
 */
static inline int priority_to_queue(int priority) {
    /* Higher priority value means higher priority.
     * Map priority 1-99 to queue index 39-0 (0 is highest priority). */
    int p = priority;
    if (p < SCHED_PRIO_MIN) p = SCHED_PRIO_MIN;
    if (p > SCHED_PRIO_MAX) p = SCHED_PRIO_MAX;
    int queue = SCHED_NUM_QUEUES - 1 - ((p - 1) * SCHED_NUM_QUEUES / 99);
    if (queue < 0) queue = 0;
    if (queue >= SCHED_NUM_QUEUES) queue = SCHED_NUM_QUEUES - 1;
    return queue;
}

/**
 * Register the idle thread. It is deliberately NOT placed in a run
 * queue via scheduler_add_task(): pick_next_task() only falls back to
 * it when every real run queue is empty, which is exactly the "nothing
 * to do" idle condition. Before this setter existed, the local
 * idle_thread pointer here was declared but never assigned, so that
 * fallback was always NULL - meaning scheduler_schedule() had nothing
 * to switch to at all once every other thread had, e.g., gone to
 * sleep, and simply did nothing.
 */
void scheduler_set_idle(thread_t* thread) {
    idle_thread = thread;
}

/**
 * Add task to scheduler
 */
void scheduler_add_task(thread_t* thread) {
    if (!thread) return;
    
    uint64_t flags = sync_irq_save();
    
    int queue_idx = priority_to_queue(thread->priority);
    run_queue_t* queue = &run_queues[queue_idx];
    
    /* Add to end of queue (round robin) */
    thread->next = NULL;
    thread->prev = queue->tail;
    
    if (queue->tail) {
        queue->tail->next = thread;
    } else {
        queue->head = thread;
    }
    queue->tail = thread;
    queue->count++;
    
    /* Set time slice based on priority */
    thread->blocked_on = NULL;
    scheduler_reset_timeslice(thread);
    
    
    sync_irq_restore(flags);
}

/**
 * Remove task from scheduler
 */
void scheduler_remove_task(thread_t* thread) {
    if (!thread) return;

    uint64_t flags = sync_irq_save();
    bool removed = false;

    if (thread->blocked_on) {
        removed = detach_linked_thread(&thread->blocked_on->head,
                                       &thread->blocked_on->tail,
                                       &thread->blocked_on->count,
                                       thread);
        thread->blocked_on = NULL;
    }

    if (!removed) {
        removed = detach_sleep_thread(thread);
    }

    if (!removed) {
        int queue_idx = priority_to_queue(thread->priority);
        run_queue_t* queue = &run_queues[queue_idx];
        removed = detach_linked_thread(&queue->head, &queue->tail, &queue->count, thread);
    }

    if (!removed) {
        thread->next = NULL;
        thread->prev = NULL;
    }

    sync_irq_restore(flags);
}

/**
 * Find highest priority ready task
 */
static thread_t* pick_next_task(void) {
    /* Search from highest priority (queue 0) */
    for (int i = 0; i < SCHED_NUM_QUEUES; i++) {
        if (run_queues[i].head != NULL) {
            return run_queues[i].head;
        }
    }
    
    /* No ready tasks - return idle */
    return idle_thread;
}

/**
 * Capture a timer-interrupted thread's full CPU state before any
 * preemption decision. The interrupt stub has already saved the live
 * register frame into struct regs, so this is the only place we can
 * reliably snapshot caller-saved registers on the preemptive path.
 */
void scheduler_capture_interrupt_context(struct regs* r) {
    if (!r || !current_thread) return;

    current_thread->context.r15 = r->r15;
    current_thread->context.r14 = r->r14;
    current_thread->context.r13 = r->r13;
    current_thread->context.r12 = r->r12;
    current_thread->context.r11 = r->r11;
    current_thread->context.r10 = r->r10;
    current_thread->context.r9  = r->r9;
    current_thread->context.r8  = r->r8;
    current_thread->context.rbp = r->rbp;
    current_thread->context.rdi = r->rdi;
    current_thread->context.rsi = r->rsi;
    current_thread->context.rdx = r->rdx;
    current_thread->context.rcx = r->rcx;
    current_thread->context.rbx = r->rbx;
    current_thread->context.rax = r->rax;
    current_thread->context.rip = r->rip;
    current_thread->context.cs = r->cs;
    current_thread->context.rflags = r->rflags;
    current_thread->context.rsp = r->rsp;
    current_thread->context.ss = r->ss;
    current_thread_ctx_precaptured = true;
}

void scheduler_end_interrupt_tick(void) {
    current_thread_ctx_precaptured = false;
}

/**
 * Main scheduler tick - called by timer interrupt
 */
void scheduler_tick(void) {
    if (!scheduler_running) return;

    uint64_t flags = sync_irq_save();

    current_tick++;
    stats.schedule_calls++;

    /* Decrement current task's time slice.
     * Only meaningful in preemptive mode: in cooperative mode (the
     * default) nothing ever forces a switch based on time_slice, so we
     * skip this entirely - otherwise time_slice (a uint64_t) would just
     * underflow tick after tick for as long as a task keeps running,
     * and re-enabling preemption later would inherit a huge bogus
     * value instead of a fresh quantum. */
    if (config.enable_preemption && current_thread && current_thread->state == TASK_RUNNING) {
        if (current_thread->time_slice > 0) {
            current_thread->time_slice--;
        }

        if (current_thread->time_slice == 0) {
            /* Move to end of its queue. */
            scheduler_remove_task(current_thread);
            current_thread->state = TASK_READY;
            scheduler_add_task(current_thread);
            scheduler_preempt();
        }
    } else if (!current_thread) {
        /* current_thread is NULL exactly once: before the very first
         * switch ever happens (the CPU is still executing on the
         * original boot path, which was never registered as a
         * thread_t). We only want to hand control away from that boot
         * path once there is genuinely useful work to run - never just
         * to run the idle thread.
         *
         * task_init() registers idle_thread as pick_next_task()'s
         * fallback (via scheduler_set_idle()) right at the start of
         * boot, long before real threads like gui_main_thread exist.
         * From that point on, pick_next_task() always returns
         * *something* (idle, if nothing else is ready) - so the naive
         * version of this branch called scheduler_schedule()
         * unconditionally on every tick, and the very next 1ms timer
         * interrupt would perform a real hal_context_switch() away
         * from whatever sequential boot-init code (ipc_init(),
         * rtc_init(), gui_init(), ...) happened to be running at that
         * exact instant, into idle_task_entry on a fresh stack.
         */
        bool real_task_ready = false;
        for (int i = 0; i < SCHED_NUM_QUEUES; i++) {
            if (run_queues[i].head != NULL) {
                real_task_ready = true;
                break;
            }
        }
        if (real_task_ready) {
            scheduler_schedule();
        }
    }

    /* Check sleeping tasks while interrupts remain disabled so sleep
     * list mutations cannot race with this timer tick. */
    thread_t* thread = sleep_list;
    while (thread) {
        thread_t* next = thread->next;
        if (thread->time_slice > 0) {
            thread->time_slice--;  /* Use time_slice for sleep countdown */
        }

        if (thread->time_slice == 0) {
            if (thread->prev) thread->prev->next = thread->next;
            if (thread->next) thread->next->prev = thread->prev;
            if (sleep_list == thread) sleep_list = thread->next;

            thread->state = TASK_READY;
            thread->next = NULL;
            thread->prev = NULL;
            scheduler_add_task(thread);
        }

        thread = next;
    }

    /* Priority boost every N ticks. */
    if (config.enable_priority_boost &&
        (current_tick - last_boost_tick) >= config.boost_interval) {
        scheduler_rebalance();
        last_boost_tick = current_tick;
    }

    sync_irq_restore(flags);
}

/**
 * Force preemption
 */
void scheduler_preempt(void) {
    if (!scheduler_running) return;
    
    /* Trigger a software interrupt or use task switching */
    /* For now, just schedule next */
    scheduler_schedule();
}

/**
 * Main schedule function
 */
void scheduler_schedule(void) {
    if (!scheduler_running) return;
    uint64_t flags = sync_irq_save();
    thread_t* next = pick_next_task();
    if (!next || next == current_thread) {
        sync_irq_restore(flags);
        return;  /* No switch needed */
    }

    stats.context_switches++;

    /* scheduler_switch_task handles current_thread update + real context switch */
    scheduler_switch_task(next);
    
    sync_irq_restore(flags);
}

/**
 * Yield current task
 */
void scheduler_yield(void) {
    if (!current_thread || !scheduler_running) return;

    uint64_t flags = sync_irq_save();
    /* Move to end of queue */
    scheduler_remove_task(current_thread);
    current_thread->state = TASK_READY;
    scheduler_add_task(current_thread);

    /* Schedule next */
    scheduler_preempt();
    sync_irq_restore(flags);
}

/**
 * Switch to new task
 */
void scheduler_switch_task(thread_t* next) {
    scheduler_do_context_switch(current_thread, next);
}

/**
 * Set task priority
 */
void scheduler_set_priority(thread_t* thread, int priority) {
    if (!thread) return;
    
    uint64_t flags = sync_irq_save();
    
    if (priority < SCHED_PRIO_MIN) priority = SCHED_PRIO_MIN;
    if (priority > SCHED_PRIO_MAX) priority = SCHED_PRIO_MAX;
    
    /* Remove from old queue */
    scheduler_remove_task(thread);
    
    /* Preserve any policy bits stored in the upper 32 bits. */
    uint64_t policy_bits = thread->priority & 0xFFFFFFFF00000000ULL;
    thread->priority = policy_bits | (uint64_t)priority;
    
    /* Add to new queue if ready */
    if (thread->state == TASK_READY) {
        scheduler_add_task(thread);
    }
    
    sync_irq_restore(flags);
}

/**
 * Get task priority
 */
int scheduler_get_priority(thread_t* thread) {
    return thread ? (int)(thread->priority & 0xFFFFFFFFULL) : 0;
}

/**
 * Set time slice
 */
void scheduler_set_timeslice(thread_t* thread, uint64_t ticks) {
    if (!thread) return;
    
    if (ticks < config.quantum_min) ticks = config.quantum_min;
    if (ticks > config.quantum_max) ticks = config.quantum_max;
    
    thread->time_slice = ticks;
}

/**
 * Reset time slice based on priority
 */
void scheduler_reset_timeslice(thread_t* thread) {
    if (!thread) return;
    
    int priority = (int)(thread->priority & 0xFFFFFFFFULL);
    uint64_t timeslice;
    
    if (priority >= SCHED_PRIO_REALTIME) {
        timeslice = SCHED_TIMESLICE_REALTIME;
    } else if (priority >= SCHED_PRIO_HIGH) {
        timeslice = SCHED_TIMESLICE_HIGH;
    } else if (priority <= SCHED_PRIO_LOW) {
        timeslice = SCHED_TIMESLICE_LOW;
    } else {
        timeslice = SCHED_TIMESLICE_DEFAULT;
    }
    
    thread->time_slice = timeslice;
}

/**
 * Initialize wait queue
 */
void wait_queue_init(wait_queue_t* queue) {
    if (!queue) return;
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
}

/**
 * Add to wait queue
 */
void wait_queue_add(wait_queue_t* queue, thread_t* thread) {
    if (!queue || !thread) return;

    uint64_t flags = sync_irq_save();

    thread->next = NULL;
    thread->prev = queue->tail;

    if (queue->tail) {
        queue->tail->next = thread;
    } else {
        queue->head = thread;
    }
    queue->tail = thread;
    queue->count++;

    thread->blocked_on = queue;
    thread->state = TASK_BLOCKED;

    sync_irq_restore(flags);
}

/**
 * Remove from wait queue
 */
void wait_queue_remove(wait_queue_t* queue, thread_t* thread) {
    if (!queue || !thread) return;

    uint64_t flags = sync_irq_save();

    if (thread->blocked_on != queue) {
        sync_irq_restore(flags);
        return;
    }

    bool removed = detach_linked_thread(&queue->head, &queue->tail, &queue->count, thread);
    if (removed) {
        thread->blocked_on = NULL;
    }

    sync_irq_restore(flags);
}

/**
 * Wake one thread from wait queue
 */
thread_t* wait_queue_wake_one(wait_queue_t* queue) {
    if (!queue) return NULL;

    uint64_t flags = sync_irq_save();
    if (!queue->head) {
        sync_irq_restore(flags);
        return NULL;
    }

    thread_t* thread = queue->head;
    bool removed = detach_linked_thread(&queue->head, &queue->tail, &queue->count, thread);
    if (removed) {
        thread->blocked_on = NULL;
        thread->state = TASK_READY;
    }
    sync_irq_restore(flags);

    if (removed) {
        scheduler_add_task(thread);
        return thread;
    }
    return NULL;
}

/**
 * Wake all threads from wait queue
 */
void wait_queue_wake_all(wait_queue_t* queue) {
    if (!queue) return;
    
    while (queue->head) {
        wait_queue_wake_one(queue);
    }
}

/**
 * Block current thread on wait queue
 */
bool scheduler_block(thread_t* thread, wait_queue_t* queue) {
    if (!queue) return false;

    if (!thread || !scheduler_running) {
        serial_puts("[SCHED] ERROR: blocking requested before the scheduler "
                     "is ready; refusing to spin in a busy loop.\n");
        __builtin_trap();
        return false;
    }
    uint64_t flags = sync_irq_save();
    scheduler_remove_task(thread);
    /* Mark the thread BLOCKED. scheduler_unblock() refuses to wake a
     * thread unless it is in this exact state, so without this the
     * thread would sit on the wait queue forever - any mutex, IPC
     * channel, or blocking I/O wait would deadlock permanently. */
    thread->state = TASK_BLOCKED;
    wait_queue_add(queue, thread);

    scheduler_preempt();
    sync_irq_restore(flags);
    return true;
}

/**
 * Unblock thread
 */
void scheduler_unblock(thread_t* thread) {
    if (!thread || thread->state != TASK_BLOCKED) return;

    uint64_t flags = sync_irq_save();

    if (thread->blocked_on) {
        wait_queue_t* queue = thread->blocked_on;
        if (detach_linked_thread(&queue->head, &queue->tail, &queue->count, thread)) {
            thread->blocked_on = NULL;
        }
    }

    thread->state = TASK_READY;
    scheduler_add_task(thread);

    sync_irq_restore(flags);
}

/**
 * Put thread to sleep
 */
void scheduler_sleep(uint64_t ms) {
    if (!current_thread) return;

    uint64_t flags = sync_irq_save();

    /* timer.c runs at 1000Hz, so 1 tick == 1 ms. */
    uint64_t ticks = ms;
    if (ticks < 1) ticks = 1;

    /* Remove from run queue */
    scheduler_remove_task(current_thread);

    /* Add to sleep list */
    current_thread->state = TASK_SLEEPING;
    current_thread->time_slice = ticks;  // Use as sleep counter
    current_thread->blocked_on = NULL;

    current_thread->next = sleep_list;
    current_thread->prev = NULL;
    if (sleep_list) {
        sleep_list->prev = current_thread;
    }
    sleep_list = current_thread;

    /* Schedule next task */
    scheduler_preempt();

    sync_irq_restore(flags);
}

/**
 * Wake sleeping thread
 */
void scheduler_wake_thread(thread_t* thread) {
    if (!thread || thread->state != TASK_SLEEPING) return;

    uint64_t flags = sync_irq_save();

    if (!detach_sleep_thread(thread)) {
        sync_irq_restore(flags);
        return;
    }

    thread->state = TASK_READY;
    sync_irq_restore(flags);

    scheduler_add_task(thread);
}

/**
 * Priority boost for starving tasks
 */
void scheduler_rebalance(void) {
    uint64_t flags = sync_irq_save();
    
    /* Boost priority of low-priority tasks that have been waiting */
    for (int i = SCHED_NUM_QUEUES - 1; i >= 5; i--) {
        if (run_queues[i].head) {
            thread_t* thread = run_queues[i].head;
            while (thread) {
                thread_t* next = thread->next;
                /* Boost by moving to higher priority queue */
                if (thread->priority > SCHED_PRIO_LOW) {
                    scheduler_set_priority(thread, thread->priority - 5);
                }
                thread = next;
            }
        }
    }
    
    sync_irq_restore(flags);
}

/**
 * Get scheduler statistics
 */
void scheduler_get_stats(sched_stats_t* out_stats) {
    if (out_stats) {
        memcpy(out_stats, &stats, sizeof(sched_stats_t));
    }
}

/**
 * Print scheduler statistics
 */
void scheduler_print_stats(void) {
    serial_puts("=== Scheduler Statistics ===\n");
    serial_puts("Context switches: ");
    serial_putdec(stats.context_switches);
    serial_puts("\nSchedule calls: ");
    serial_putdec(stats.schedule_calls);
    serial_puts("\nTasks created: ");
    serial_putdec(stats.tasks_created);
    serial_puts("\nTasks destroyed: ");
    serial_putdec(stats.tasks_destroyed);
    serial_puts("\nCurrent load: ");
    serial_putdec(stats.current_load);
    serial_puts("\n============================\n");
}

/**
 * Dump scheduler queues
 */
void scheduler_dump_queues(void) {
    serial_puts("=== Scheduler Queues ===\n");
    for (int i = 0; i < SCHED_NUM_QUEUES; i++) {
        if (run_queues[i].count > 0) {
            serial_puts("Queue ");
            serial_putdec(i);
            serial_puts(": ");
            serial_putdec(run_queues[i].count);
            serial_puts(" tasks\n");
        }
    }
    serial_puts("==========================\n");
}

/**
 * Check if scheduler is idle
 */
bool scheduler_is_idle(void) {
    /* Check if any queue has tasks */
    for (int i = 0; i < SCHED_NUM_QUEUES; i++) {
        if (run_queues[i].head != NULL) {
            return FALSE;
        }
    }
    return TRUE;
}


/**
 * Get scheduler uptime in ticks
 */
uint64_t scheduler_get_uptime(void) {
    return current_tick;
}
/**
 * scheduler_enhanced.c - Enhanced Scheduler with Real Context Switching
 * C-OS 4.0.8 alpha
 * 
 * Implements true preemptive multitasking with proper context switching
 */
#include "scheduler.h"
#include "task.h"
#include "serial.h"
#include "hal_api.h"
#include "string.h"
#include "io.h"

/* ============================================================================
   External Assembly Functions
   ========================================================================== */
extern uint64_t get_current_rsp(void);
extern uint64_t get_current_rip(void);
extern void set_kernel_stack(uint64_t rsp0);

/* ============================================================================
   Enhanced Scheduler State
   ========================================================================== */
static volatile uint64_t context_switches_performed = 0;
static volatile uint64_t preemptions_triggered = 0;
static volatile bool scheduler_in_switch = false;

/* ============================================================================
   Context Switch Implementation
   ========================================================================== */
/* Canonical switch path lives in scheduler_switch_task().
 * scheduler_perform_context_switch() remains as a compatibility shim
 * for any older call sites that still pass an explicit prev pointer. */
void scheduler_perform_context_switch(thread_t* prev, thread_t* next);

/* ============================================================================
   Missing implementations – complete the scheduler for full multitasking
   ============================================================================ */

/* Timer callback hook (optional external callback on each tick) */
static void (*timer_hook_callback)(void) = NULL;

void scheduler_set_timer_callback(void (*callback)(void)) {
    timer_hook_callback = callback;
}

/* scheduler_add_process / scheduler_remove_process
   Adds/removes all threads of a process to/from the ready queues. */
void scheduler_add_process(process_t* proc) {
    if (!proc) return;

    uint64_t flags = sync_irq_save();
    thread_t* th = proc->thread_list;
    while (th) {
        thread_t* next = th->proc_next;
        if (th->state == TASK_READY || th->state == TASK_CREATED) {
            scheduler_add_task(th);
        }
        th = next;
    }
    sync_irq_restore(flags);
}

void scheduler_remove_process(process_t* proc) {
    if (!proc) return;

    uint64_t flags = sync_irq_save();
    thread_t* th = proc->thread_list;
    while (th) {
        thread_t* next = th->proc_next;
        if (th->state != TASK_UNUSED) {
            scheduler_remove_task(th);
            th->state = TASK_ZOMBIE;
        }
        th = next;
    }
    sync_irq_restore(flags);
}

/* scheduler_add_sleep – put thread to sleep for ms milliseconds */
void scheduler_add_sleep(thread_t* thread, uint64_t ms) {
    if (!thread) return;
    
    uint64_t flags = sync_irq_save();
    
    scheduler_remove_task(thread);
    uint64_t ticks = ms;
    if (ticks < 1) ticks = 1;
    thread->state = TASK_SLEEPING;
    thread->time_slice = ticks;
    thread->next = sleep_list;
    thread->prev = NULL;
    if (sleep_list) sleep_list->prev = thread;
    sleep_list = thread;
    
    sync_irq_restore(flags);
}

/* scheduler_sleep_on – block current thread on a wait queue */
void scheduler_sleep_on(wait_queue_t* queue) {
    if (!current_thread || !queue) return;
    scheduler_block(current_thread, queue);
}

/* Priority boost / penalise */
void scheduler_boost_priority(thread_t* thread) {
    if (!thread) return;
    int p = thread->priority + 5;
    if (p > SCHED_PRIO_MAX) p = SCHED_PRIO_MAX;
    scheduler_set_priority(thread, p);
}

void scheduler_penalize_priority(thread_t* thread) {
    if (!thread) return;
    int p = thread->priority - 5;
    if (p < SCHED_PRIO_MIN) p = SCHED_PRIO_MIN;
    scheduler_set_priority(thread, p);
}

/* Scheduling policy (stored in unused field; C-OS is single-CPU so it's advisory) */
void scheduler_set_policy(thread_t* thread, sched_policy_t policy) {
    if (!thread) return;
    /* Store policy in high bits of priority field (bits 32-63) */
    thread->priority = (thread->priority & 0xFFFFFFFFULL) | ((uint64_t)policy << 32);
}

sched_policy_t scheduler_get_policy(thread_t* thread) {
    if (!thread) return SCHED_OTHER;
    return (sched_policy_t)((thread->priority >> 32) & 0xF);
}

/* Time slice query */
uint64_t scheduler_get_timeslice(thread_t* thread) {
    return thread ? thread->time_slice : 0;
}

/* switch_next – pick and switch to next ready task */
void scheduler_switch_next(void) {
    scheduler_schedule();
}

/* Idle task entry – just HLT in a loop */
void scheduler_idle_task(void) {
    for (;;) {
        __asm__ volatile("sti; hlt");
        stats.idle_time++;
        if (timer_hook_callback) timer_hook_callback();
    }
}

/* CPU load query (single-CPU stub) */
int scheduler_get_cpu_load(int cpu) {
    (void)cpu;
    /* Simple load = number of runnable threads */
    int load = 0;
    for (int i = 0; i < SCHED_NUM_QUEUES; i++) load += run_queues[i].count;
    stats.current_load = (uint64_t)load;
    return load;
}

/* CPU usage percentage (0-100) */
float scheduler_get_cpu_usage(void) {
    if (stats.schedule_calls == 0) return 0.0f;
    uint64_t total = stats.busy_time + stats.idle_time;
    if (total == 0) return 0.0f;
    return (float)stats.busy_time * 100.0f / (float)total;
}

/* Migrate task to another CPU (stub – single CPU) */
void scheduler_migrate_task(thread_t* thread, int target_cpu) {
    (void)thread;
    (void)target_cpu;
    /* No-op on single-CPU systems */
}

/* wait_queue_print */
void wait_queue_print(wait_queue_t* queue) {
    if (!queue) return;
    serial_puts("[WQ] count=");
    serial_putdec(queue->count);
    serial_puts("\n");
    thread_t* th = queue->head;
    while (th) {
        serial_puts("  tid="); serial_putdec(th->tid); serial_puts("\n");
        th = th->next;
    }
}

/* Dump full scheduler state */
void scheduler_dump_state(void) {
    serial_puts("=== Scheduler State ===\n");
    serial_puts("Running: ");
    if (current_thread) {
        serial_puts("tid="); serial_putdec(current_thread->tid);
    } else {
        serial_puts("(none)");
    }
    serial_puts("\n");
    scheduler_dump_queues();
    scheduler_print_stats();
}

/* Verify queue integrity */
void scheduler_verify_queues(void) {
    for (int i = 0; i < SCHED_NUM_QUEUES; i++) {
        run_queue_t* q = &run_queues[i];
        int count = 0;
        thread_t* th = q->head;
        while (th) {
            count++;
            if (count > 512) {
                serial_puts("[SCHED] ERROR: queue loop detected\n");
                q->head = NULL; q->tail = NULL; q->count = 0;
                return;
            }
            th = th->next;
        }
        if (count != q->count) {
            serial_puts("[SCHED] WARN: queue count mismatch, fixing\n");
            q->count = count;
        }
    }
}

static void scheduler_do_context_switch(thread_t* prev, thread_t* next) {
    if (!next) return;

    /* Update process states and address space before the CPU switch so
     * the outgoing thread does not continue running with stale CR3
     * bookkeeping once it returns. */
    process_t* prev_proc = prev ? process_get_by_pid(prev->pid) : process_get_current();
    process_t* next_proc = process_get_by_pid(next->pid);

    /* The scheduler tick only preempts a TASK_RUNNING thread.  A newly
     * created or requeued thread is TASK_READY, and the old switch path
     * changed only the process state, leaving the thread state READY forever.
     * That silently disabled every timer-driven timeslice expiration while
     * cooperative yield still appeared to work. */
    if (prev && prev != next && prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current_thread = next;

    if (prev_proc && prev_proc->state == TASK_RUNNING) {
        prev_proc->state = TASK_READY;
    }
    if (next_proc) {
        if (next_proc != prev_proc && next_proc->page_dir) {
            paging_switch_directory((page_directory_t*)next_proc->page_dir);
        }
        next_proc->state = TASK_RUNNING;
        process_set_current(next_proc);
    }

    tss_set_kernel_stack(next->kernel_stack);

    static hal_context_t boot_context_scratch;
    static bool boot_context_init = false;
    if (!boot_context_init) {
        memset(&boot_context_scratch, 0, sizeof(boot_context_scratch));
        boot_context_init = true;
    }

    /* If prev's true register state (rip/rsp/rflags/cs/ss and GPRs) was
     * just captured from a genuine interrupt frame by
     * scheduler_capture_interrupt_context(), that data in prev->context
     * is already correct and must not be touched again: hal_context_switch
     * would otherwise "save" over it using its own call-site position
     * (deep inside the scheduler's C call chain), which is not a
     * meaningful place to resume prev later. Route that save into a
     * disposable scratch buffer instead so prev->context is left alone. */
    static hal_context_t discard_scratch;
    bool skip_prev_save = prev && current_thread_ctx_precaptured;
    current_thread_ctx_precaptured = false;

    if (next != prev) {
        hal_context_t* prev_ctx;
        if (!prev) {
            prev_ctx = &boot_context_scratch;
        } else if (skip_prev_save) {
            prev_ctx = &discard_scratch;
        } else {
            prev_ctx = (hal_context_t*)&prev->context;
        }
        stats.busy_time++;
        scheduler_fpu_save(prev);
        scheduler_fpu_restore(next);
        hal_context_switch(prev_ctx, (const hal_context_t*)&next->context);
    }
}

/* Complete the context switch implementation */
void scheduler_perform_context_switch(thread_t* prev, thread_t* next) {
    if (!next) return;
    if (scheduler_in_switch) return;

    uint64_t flags = sync_irq_save();
    scheduler_in_switch = true;
    context_switches_performed++;
    preemptions_triggered++;

    /* Keep the explicit prev parameter for compatibility, but always
     * switch from the scheduler's live current thread. That prevents
     * stale callers from re-entering with a mismatched bookkeeping
     * pointer and corrupting the active thread/process pair. */
    (void)prev;
    scheduler_do_context_switch(current_thread, next);

    scheduler_in_switch = false;
    sync_irq_restore(flags);
}
