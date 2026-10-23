#ifndef COS_PERCPU_H
#define COS_PERCPU_H
/*
 * percpu.h - per-CPU kernel state.
 *
 * Each CPU loads its own TSS (a TSS cannot be busy on two CPUs), and the selectors differ: CPU n uses GDT_TSS + 16*n in the one GDT all CPUs share.
 * `str` is unprivileged work for the kernel, is not intercepted by a hypervisor (cpuid is: a VM exit each time) and needs no MSR or swapgs,
 * so the TR selector is the CPU's number.
 */
#include "types.h"
#include <stdbool.h>
#include "smp.h"

struct thread;
struct process;

typedef struct cpu_local {
    uint32_t id;
    struct thread           *current;       /* the thread running here (NULL on the boot path before the first switch) */
    struct thread           *idle;          /* what this CPU runs when nothing is ready */
    struct process          *proc;          /* the process whose address space is loaded (task.c) */
    bool                     ctx_precaptured; /* scheduler: the preempted thread's registers were taken from the interrupt frame */
    struct thread           *switch_prev;   /* the thread being switched away from: its on_cpu is cleared once this CPU has left its stack (hal_context_switch) */
    void                    *dir;           /* page_directory_t loaded in CR3 on this CPU (paging.c) */
    volatile uint64_t        ticks;         /* timer ticks seen on this CPU */
    volatile uint32_t        sched_enabled; /* this CPU takes part in scheduling threads (SMP scheduler) */
    volatile uint32_t        kick;          /* reschedule requested by another CPU (process exit, signal) */
    uint64_t                 spin_flags[8]; /* saved RFLAGS of the cos_spin_lock() sections this CPU is in (sync.c) */
    uint32_t                 spin_depth;
    uint32_t                 acc_busy, acc_total;   /* ticks spent running something other than idle / all ticks, in the current 500-tick window */
    volatile uint32_t        load_pct;              /* the window before: percent busy */
} cpu_local_t;

extern cpu_local_t g_cpu_local[SMP_MAX_CPUS];

static inline uint32_t cpu_id(void) {
    uint16_t tr;
    __asm__ volatile("str %0" : "=r"(tr));
    return tr >= 0x40 ? (uint32_t)((tr - 0x40) >> 4) : 0u;      /* 0: the TSS is not loaded yet (early boot) */
}
static inline cpu_local_t *cpu_local(void) { return &g_cpu_local[cpu_id()]; }

#endif
