/**
 * scheduler.h - Task Scheduler
 * 
 * Priority-based preemptive scheduler with round-robin for same priority.
 * Supports multi-level feedback queue and real-time tasks.
 */

#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"
#include "task.h"

struct regs;

/* Scheduling policies */
typedef enum {
    SCHED_FIFO = 0,      // First In First Out (real-time)
    SCHED_RR,            // Round Robin
    SCHED_OTHER,         // Normal (dynamic priority)
    SCHED_IDLE,          // Lowest priority
} sched_policy_t;

/* Priority levels */
#define SCHED_PRIO_MIN          1
#define SCHED_PRIO_MAX          99
#define SCHED_PRIO_DEFAULT      20
#define SCHED_PRIO_IDLE         1
#define SCHED_PRIO_LOW          10
#define SCHED_PRIO_NORMAL       20
#define SCHED_PRIO_HIGH         40
#define SCHED_PRIO_REALTIME     80
#define SCHED_PRIO_MAX_REALTIME 99

/* Number of priority queues */
#define SCHED_NUM_QUEUES        40

/* Time slices (in timer ticks; timer.c runs at 1000Hz, so 1 tick == 1 ms) */
#define SCHED_TIMESLICE_DEFAULT     10  // 10ms
#define SCHED_TIMESLICE_HIGH        20  // 20ms
#define SCHED_TIMESLICE_LOW         5   // 5ms
#define SCHED_TIMESLICE_REALTIME    100 // 100ms

/* Scheduler statistics */
typedef struct {
    uint64_t context_switches;
    uint64_t schedule_calls;
    uint64_t tasks_created;
    uint64_t tasks_destroyed;
    uint64_t current_load;
    uint64_t avg_load_1m;
    uint64_t avg_load_5m;
    uint64_t avg_load_15m;
    uint64_t idle_time;
    uint64_t busy_time;
} sched_stats_t;

/* Run queue structure */
typedef struct run_queue {
    thread_t* head;
    thread_t* tail;
    int count;
} run_queue_t;

/* Scheduler configuration */
typedef struct {
    int enable_preemption;
    int enable_priority_boost;
    uint64_t boost_interval;      // Priority boost interval (ticks)
    uint64_t quantum_min;           // Minimum time quantum
    uint64_t quantum_max;           // Maximum time quantum
} sched_config_t;

/* Function prototypes */

/* Initialization */
void scheduler_init(void);
void scheduler_start(void);
void scheduler_stop(void);

/* Cooperative vs preemptive mode.
 * C-OS defaults to COOPERATIVE scheduling: scheduler_tick() still fires
 * on every timer interrupt (so sleep timers and priority boosting keep
 * working), but it will never yank a task off the CPU mid-timeslice.
 * A task only gives up the CPU when it voluntarily calls
 * scheduler_yield()/thread_yield(), or when it blocks/sleeps on its own
 * (mutex, semaphore, sleep, wait queue, etc). Call
 * scheduler_set_preemption(1) to switch back to the old preemptive
 * behavior (forced switch when a thread's time_slice reaches 0). */
void scheduler_set_preemption(int enable);
int  scheduler_get_preemption(void);

/* Task management */
void scheduler_add_task(thread_t* thread);
void scheduler_remove_task(thread_t* thread);
void scheduler_add_process(process_t* proc);
void scheduler_remove_process(process_t* proc);
void scheduler_add_sleep(thread_t* thread, uint64_t ms);
void scheduler_wake_thread(thread_t* thread);

/* Main scheduling */
void scheduler_tick(void);              // Called by timer interrupt
void scheduler_capture_interrupt_context(struct regs* r);
void scheduler_end_interrupt_tick(void);
void scheduler_schedule(void);          // Main schedule function
void scheduler_yield(void);              // Yield current task
void scheduler_preempt(void);            // Force preemption

/* Context switching */
void scheduler_switch_task(thread_t* next);
void scheduler_switch_next(void);       // Switch to next ready task

/* Priority management */
void scheduler_set_priority(thread_t* thread, int priority);
int scheduler_get_priority(thread_t* thread);
void scheduler_boost_priority(thread_t* thread);
void scheduler_penalize_priority(thread_t* thread);

/* Policy management */
void scheduler_set_policy(thread_t* thread, sched_policy_t policy);
sched_policy_t scheduler_get_policy(thread_t* thread);

/* Time quantum management */
void scheduler_set_timeslice(thread_t* thread, uint64_t ticks);
uint64_t scheduler_get_timeslice(thread_t* thread);
void scheduler_reset_timeslice(thread_t* thread);

/* Wait queues */
typedef struct wait_queue {
    thread_t* head;
    thread_t* tail;
    int count;
} wait_queue_t;

void wait_queue_init(wait_queue_t* queue);
void wait_queue_add(wait_queue_t* queue, thread_t* thread);
void wait_queue_remove(wait_queue_t* queue, thread_t* thread);
thread_t* wait_queue_wake_one(wait_queue_t* queue);
void wait_queue_wake_all(wait_queue_t* queue);
void wait_queue_print(wait_queue_t* queue);

/* Block/wait operations */
bool scheduler_block(thread_t* thread, wait_queue_t* queue);
void scheduler_unblock(thread_t* thread);
void scheduler_sleep_on(wait_queue_t* queue);
void scheduler_sleep(uint64_t ms);

/* Load balancing (for SMP, stub here) */
void scheduler_rebalance(void);
int scheduler_get_cpu_load(int cpu);
void scheduler_migrate_task(thread_t* thread, int target_cpu);

void scheduler_note_task_created(void);
void scheduler_note_task_destroyed(void);

/* Statistics */
void scheduler_get_stats(sched_stats_t* stats);
void scheduler_print_stats(void);
uint64_t scheduler_get_uptime(void);
float scheduler_get_cpu_usage(void);

/* Debugging */
void scheduler_dump_queues(void);
void scheduler_dump_state(void);
void scheduler_verify_queues(void);

/* Idle task */
void scheduler_idle_task(void);
bool scheduler_is_idle(void);

/* External timer integration */
void scheduler_set_timer_callback(void (*callback)(void));

thread_t* scheduler_get_current_thread(void);
void scheduler_set_idle(thread_t* thread);

#endif /* SCHEDULER_H */
