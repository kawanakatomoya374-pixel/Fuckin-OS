#ifndef COS_SMP_H
#define COS_SMP_H

#include "types.h"
#include <stdbool.h>

/*
 * SMP platform layer. C-OS supports up to 64 logical CPUs in this release.
 * ACPI MADT supplies real hardware APIC IDs and the BSP starts each AP
 * through INIT/SIPI. Secondary CPUs enter a lock-protected kernel
 * work loop after startup, so real AP execution is available even before the
 * general thread scheduler is fully converted to per-CPU run queues.
 */
#define SMP_MAX_CPUS 64

/* IPI / timer vectors of the SMP scheduler. Outside 0x40-0x5F (vectors the kernel modules allocate for MSI and I/O APIC routes) and in three different priority
 * classes, so that a vector left "in service" by a bug or a lost EOI cannot block the others. */
#define SMP_VEC_TIMER 0xD0u     /* per-CPU scheduler tick (Local APIC timer) */
#define SMP_VEC_KICK  0xE0u     /* reschedule now */
#define SMP_VEC_TLB   0xF1u     /* TLB shootdown */

typedef struct {
    uint32_t logical_id;
    uint32_t apic_id;
    bool present;
    bool online;
} smp_cpu_info_t;

typedef void (*smp_work_fn_t)(void *arg);

typedef enum {
    SMP_WORK_PRIORITY_LOW = 0,
    SMP_WORK_PRIORITY_NORMAL = 1,
    SMP_WORK_PRIORITY_HIGH = 2
} smp_work_priority_t;

typedef enum {
    SMP_BACKGROUND_JOB_IDLE = 0,
    SMP_BACKGROUND_JOB_QUEUED = 1,
    SMP_BACKGROUND_JOB_RUNNING = 2,
    SMP_BACKGROUND_JOB_COMPLETE = 3,
    SMP_BACKGROUND_JOB_CANCELLED = 4
} smp_background_job_state_t;

/* The caller owns this object until it reaches COMPLETE or CANCELLED. Its
 * function and argument must remain immutable while queued/running. `frame_id`
 * allows GUI owners to reject stale off-screen tile/decode output, while an
 * optional absolute timer-tick deadline prevents a presentation frame from
 * waiting indefinitely for AP work. */
typedef struct {
    smp_work_fn_t fn;
    void *arg;
    uint64_t frame_id;
    uint64_t deadline_ticks;
    smp_work_priority_t priority;
    volatile uint32_t state;
    volatile uint32_t assigned_cpu;
} smp_background_job_t;

void smp_init(void);
/* Starts AP workers deferred during early boot on hypervisors whose firmware
 * exposes a valid MADT but is sensitive to INIT/SIPI before first display. */
void smp_start_deferred_workers(void);
bool smp_workers_deferred(void);
uint32_t smp_possible_cpu_count(void);
uint32_t smp_online_cpu_count(void);
bool smp_apic_available(void);
bool smp_secondary_startup_ready(void);
const smp_cpu_info_t* smp_cpu_info(uint32_t logical_id);

/* Queue a short, non-blocking kernel work item for execution by an online AP.
 * The function must not use BSP-only scheduler state or sleep while holding
 * external locks. */
bool smp_submit_work(smp_work_fn_t fn, void *arg);
/* Queue work with a latency class. APs take local high-priority work first;
 * an idle AP steals only from a victim queue's tail, preserving its newest
 * latency-sensitive work where possible. */
bool smp_submit_work_priority(smp_work_fn_t fn, void *arg, smp_work_priority_t priority);
/* Queue work for a specified online AP. This is used for CPU-local services
 * and deterministic SMP validation; CPU 0 is BSP-only in this stage. */
bool smp_submit_work_to_cpu(uint32_t logical_id, smp_work_fn_t fn, void *arg);
/* Same, but the item can only run on that CPU (it is never stolen). */
bool smp_submit_pinned_work_to_cpu(uint32_t logical_id, smp_work_fn_t fn, void *arg);
bool smp_submit_work_to_cpu_priority(uint32_t logical_id, smp_work_fn_t fn,
                                     void *arg, smp_work_priority_t priority);
uint64_t smp_cpu_completed_work(uint32_t logical_id);
uint64_t smp_cpu_stolen_work(uint32_t logical_id);

void smp_background_job_init(smp_background_job_t *job, smp_work_fn_t fn,
                             void *arg, uint64_t frame_id,
                             uint64_t deadline_ticks,
                             smp_work_priority_t priority);
bool smp_submit_background_job(smp_background_job_t *job);
bool smp_background_job_is_done(const smp_background_job_t *job);
bool smp_cancel_background_job(smp_background_job_t *job);
/* Waits for the job; takes it back and runs it on the calling CPU if no AP picks it up promptly. */
void smp_background_job_wait(smp_background_job_t *job);

/* Runs fn(ctx, tile) for tile = 0..ntiles-1 across the idle cores, pulling tiles from a shared counter so load
 * balances itself. The calling CPU takes owner_weight_pct/100 of one helper's share while helpers are making
 * progress (use < 100 for the BSP, which also serves interrupts) and everything if they are not. Returns when
 * every tile has finished. Tiles must be independent. */
typedef void (*smp_tile_fn_t)(void *ctx, uint32_t tile);
void smp_parallel_tiles(smp_tile_fn_t fn, void *ctx, uint32_t ntiles, uint32_t owner_weight_pct);

/* Used by smp_user.c: identify the executing CPU, reach its Local APIC, and
 * interrupt another CPU with a fixed vector. */
uint32_t smp_current_logical_id(void);
volatile uint32_t *smp_lapic_registers(void);
/* Interrupts of devices: enable and map the Local APIC (also on a single-CPU machine), end of interrupt, the boot CPU's APIC id, the first I/O APIC (NULL when absent). */
bool smp_lapic_ensure(void);
void smp_lapic_eoi(void);
uint32_t smp_bsp_apic_id(void);
volatile uint32_t *smp_ioapic(uint32_t *gsi_base);
void smp_send_fixed_ipi(uint32_t logical_id, uint8_t vector);
/* Marks a CPU as running a ring3 compute job: it is then skipped when kernel work is dispatched. */
void smp_cpu_set_user_busy(uint32_t logical_id, bool busy);

#endif
