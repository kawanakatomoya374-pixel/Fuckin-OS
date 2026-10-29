/*
 * smp.c - Bounded x86-64 SMP discovery and AP bootstrap for C-OS
 *
 * Secondary CPUs are enumerated from ACPI MADT and brought online through
 * INIT/SIPI one by one.  They receive independent bootstrap stacks and enter
 * a controlled HLT state after acknowledging startup.  This is deliberate:
 * the existing scheduler/task lifetime machinery has one global run queue,
 * so executing normal threads concurrently would corrupt its UP-only state.
 * The platform layer nevertheless performs genuine hardware AP startup and
 * exposes up to twelve online processors for the next scheduler stage.
 */
#include "smp.h"
#include "idt.h"
#include "percpu.h"
#include "arch/gdt.h"
#include "serial.h"
#include "string.h"
#include "mm/paging.h"
#include "timer.h"
#include "task.h"
#include "cos_elf.h"

#define SMP_LAPIC_MSR              0x1Bu
#define SMP_LAPIC_ENABLE            (1ULL << 11)
#define SMP_LAPIC_ICR_LOW           0x300u
#define SMP_LAPIC_ICR_HIGH          0x310u
#define SMP_LAPIC_SVR               0x0F0u
#define SMP_LAPIC_ICR_DELIVERY      (1u << 12)
#define SMP_TRAMPOLINE_PHYS         0x9000u
#define SMP_TRAMPOLINE_VECTOR       (SMP_TRAMPOLINE_PHYS >> 12)
#define SMP_MAILBOX_PHYS            0x8000u
/* AP workers execute real C code (decode/tile jobs as well as bootstrap
 * probes), so their private stacks must match the 512KiB kernel/GUI runtime
 * stack policy rather than the old trampoline-era 8KiB allocation. */
#define SMP_AP_STACK_SIZE            (512u * 1024u)

/* The deliberate queue-imbalance benchmark is useful under a debugger, but
 * never belongs on the interactive boot path. Real GUI/image jobs exercise the
 * same API after the desktop is responsive. */
#ifndef COS_SMP_BOOT_STEAL_DIAGNOSTIC
#define COS_SMP_BOOT_STEAL_DIAGNOSTIC 0
#endif

/* Minimal packed ACPI types used only for MADT CPU enumeration. */
typedef struct __attribute__((packed)) {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_phys;
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} smp_acpi_rsdp_t;

typedef struct __attribute__((packed)) {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} smp_acpi_sdt_t;

typedef struct __attribute__((packed)) {
    smp_acpi_sdt_t header;
    uint32_t lapic_phys;
    uint32_t flags;
} smp_acpi_madt_t;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;
} smp_madt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} smp_gdtr_t;

typedef struct __attribute__((packed)) {
    uint64_t cr3;
    smp_gdtr_t gdtr;
    uint8_t reserved0[6];
    uint64_t stack;
    uint64_t entry;
} smp_ap_mailbox_t;

extern uint64_t cos_mb2_get_acpi_rsdp(void);
extern uint8_t smp_ap_trampoline_start[];
extern uint8_t smp_ap_trampoline_end[];

void smp_kick_cpu(uint32_t cpu);
static smp_cpu_info_t s_cpus[SMP_MAX_CPUS];
static uint8_t s_ap_stacks[SMP_MAX_CPUS][SMP_AP_STACK_SIZE]
    __attribute__((aligned(16)));
static volatile uint32_t s_possible_cpus = 1;
/* CPUs currently running a ring3 compute job (smp_user.c): they take no kernel work until it ends. */
static volatile uint32_t s_user_busy_mask;
void smp_cpu_set_user_busy(uint32_t logical_id, bool busy) {
    if (logical_id >= 32u) return;
    if (busy) __atomic_fetch_or(&s_user_busy_mask, 1u << logical_id, __ATOMIC_ACQ_REL);
    else __atomic_fetch_and(&s_user_busy_mask, ~(1u << logical_id), __ATOMIC_ACQ_REL);
}
static volatile uint32_t s_online_cpus = 1;
static bool s_apic = false;
static bool s_ap_bootstrap_ready = false;
static bool s_workers_deferred = false;
static bool s_workers_started = false;
static volatile uint32_t* s_lapic = NULL;

/* The general task scheduler still owns one BSP-centric run queue. This
 * bounded queue is a safe first SMP execution domain: it dispatches short,
 * non-blocking kernel work onto genuine APs without exposing scheduler-global
 * lists to concurrent mutation. */
#define SMP_WORK_QUEUE_CAPACITY 64u
typedef struct {
    smp_work_fn_t fn;
    void *arg;
    smp_work_priority_t priority;
    smp_background_job_t *background_job;
    bool pinned;       /* must run on the CPU it was queued to; never stolen (ring3 jobs) */
} smp_work_item_t;

/* One queue and lock per CPU.  An AP never contends for the list belonging to
 * another AP, and the BSP is the only normal producer during this bootstrap
 * stage.  This is the same ownership boundary a full per-CPU task scheduler
 * will use later. */
static smp_work_item_t s_work_queue[SMP_MAX_CPUS][SMP_WORK_QUEUE_CAPACITY];
static volatile uint32_t s_work_head[SMP_MAX_CPUS];
static volatile uint32_t s_work_tail[SMP_MAX_CPUS];
static volatile uint32_t s_work_count[SMP_MAX_CPUS];
/* Fast, advisory count of queued AP-only jobs. It avoids O(CPU) stealing
 * scans while all queues are empty; queue locks remain the authority. */
static volatile uint32_t s_pending_work = 0u;
static volatile uint32_t s_work_lock[SMP_MAX_CPUS];
static volatile uint64_t s_completed_work[SMP_MAX_CPUS];
static volatile uint64_t s_stolen_work[SMP_MAX_CPUS];
static volatile uint32_t s_boot_probe_done[SMP_MAX_CPUS];
static volatile uint64_t s_boot_probe_checksum[SMP_MAX_CPUS];
#if COS_SMP_BOOT_STEAL_DIAGNOSTIC
static volatile uint32_t s_steal_probe_done = 0u;
static volatile uint64_t s_steal_probe_checksum = 0u;
#endif
static uint32_t s_next_work_cpu = 1u;

static void smp_work_lock_acquire(uint32_t logical_id) {
    while (__atomic_exchange_n(&s_work_lock[logical_id], 1u, __ATOMIC_ACQUIRE) != 0u) {
        while (__atomic_load_n(&s_work_lock[logical_id], __ATOMIC_RELAXED) != 0u) {
            __asm__ volatile("pause");
        }
    }
}

static void smp_work_lock_release(uint32_t logical_id) {
    __atomic_store_n(&s_work_lock[logical_id], 0u, __ATOMIC_RELEASE);
}

static smp_work_priority_t smp_sanitize_priority(smp_work_priority_t priority) {
    if (priority > SMP_WORK_PRIORITY_HIGH) return SMP_WORK_PRIORITY_NORMAL;
    return priority;
}

/* Caller holds the owning per-CPU queue lock. The queue remains compact and
 * bounded, so selecting the highest priority item costs at most 64 entries and
 * avoids introducing a second shared scheduling structure before the general
 * scheduler has per-CPU ownership. */
static bool smp_pop_best_locked(uint32_t logical_id, smp_work_item_t *out, bool steal) {
    uint32_t count = s_work_count[logical_id];
    if (!out || count == 0u) return false;

    uint32_t best_offset = 0u;
    smp_work_priority_t best_priority = SMP_WORK_PRIORITY_LOW;
    bool found = false;
    for (uint32_t offset = 0; offset < count; ++offset) {
        uint32_t index = (s_work_head[logical_id] + offset) % SMP_WORK_QUEUE_CAPACITY;
        if (steal && s_work_queue[logical_id][index].pinned) continue;
        smp_work_priority_t priority = s_work_queue[logical_id][index].priority;
        if (!found || priority > best_priority) {
            best_offset = offset;
            best_priority = priority;
            found = true;
        }
    }
    if (!found) return false;

    uint32_t best_index = (s_work_head[logical_id] + best_offset) % SMP_WORK_QUEUE_CAPACITY;
    *out = s_work_queue[logical_id][best_index];
    for (uint32_t offset = best_offset; offset + 1u < count; ++offset) {
        uint32_t dst = (s_work_head[logical_id] + offset) % SMP_WORK_QUEUE_CAPACITY;
        uint32_t src = (s_work_head[logical_id] + offset + 1u) % SMP_WORK_QUEUE_CAPACITY;
        s_work_queue[logical_id][dst] = s_work_queue[logical_id][src];
    }
    s_work_tail[logical_id] = (s_work_tail[logical_id] + SMP_WORK_QUEUE_CAPACITY - 1u) % SMP_WORK_QUEUE_CAPACITY;
    --s_work_count[logical_id];
    __atomic_fetch_sub(&s_pending_work, 1u, __ATOMIC_RELAXED);
    return true;
}

static bool smp_take_work(uint32_t logical_id, smp_work_item_t *out) {
    if (!out || logical_id == 0 || logical_id >= SMP_MAX_CPUS) return false;
    if (__atomic_load_n(&s_work_count[logical_id], __ATOMIC_ACQUIRE) == 0u) return false;
    smp_work_lock_acquire(logical_id);
    bool found = smp_pop_best_locked(logical_id, out, false);
    smp_work_lock_release(logical_id);
    return found;
}

/* Idle APs are permitted to take short, non-blocking tasks from the most
 * loaded peer queue. This is deliberately constrained to the AP work domain;
 * it never touches the scheduler's global run queue or GUI/VRAM state. */
static bool smp_steal_work(uint32_t thief_logical_id, smp_work_item_t *out) {
    if (!out || thief_logical_id == 0u || thief_logical_id >= s_possible_cpus) return false;
    if (__atomic_load_n(&s_pending_work, __ATOMIC_ACQUIRE) == 0u) return false;

    uint32_t victim = 0u;
    uint32_t largest = 0u;
    for (uint32_t cpu = 1u; cpu < s_possible_cpus; ++cpu) {
        if (cpu == thief_logical_id ||
            !__atomic_load_n(&s_cpus[cpu].online, __ATOMIC_ACQUIRE)) continue;
        uint32_t count = __atomic_load_n(&s_work_count[cpu], __ATOMIC_RELAXED);
        if (count > largest) {
            largest = count;
            victim = cpu;
        }
    }
    if (victim == 0u || largest == 0u) return false;

    smp_work_lock_acquire(victim);
    bool stolen = smp_pop_best_locked(victim, out, true);
    smp_work_lock_release(victim);
    if (stolen) {
        __atomic_fetch_add(&s_stolen_work[thief_logical_id], 1u, __ATOMIC_RELAXED);
    }
    return stolen;
}

static void smp_execute_work(uint32_t logical_id, const smp_work_item_t *item) {
    if (!item || item->fn == NULL) return;
    smp_background_job_t *job = item->background_job;
    if (job != NULL) {
        uint32_t expected = SMP_BACKGROUND_JOB_QUEUED;
        if (!__atomic_compare_exchange_n(&job->state, &expected,
                                         SMP_BACKGROUND_JOB_RUNNING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return; /* cancelled or invalidated before this AP acquired it */
        }
        __atomic_store_n(&job->assigned_cpu, logical_id, __ATOMIC_RELEASE);
        if (job->deadline_ticks != 0u && get_timer_ticks() > job->deadline_ticks) {
            __atomic_store_n(&job->state, SMP_BACKGROUND_JOB_CANCELLED, __ATOMIC_RELEASE);
            return;
        }
        item->fn(item->arg);
        __atomic_store_n(&job->state, SMP_BACKGROUND_JOB_COMPLETE, __ATOMIC_RELEASE);
    } else {
        item->fn(item->arg);
    }
    __atomic_fetch_add(&s_completed_work[logical_id], 1u, __ATOMIC_RELAXED);
}

static void smp_boot_probe(void *arg) {
    uint32_t logical_id = (uint32_t)(uintptr_t)arg;
    if (logical_id >= SMP_MAX_CPUS) return;

    /* A deterministic, side-effecting integer workload. It is intentionally
     * small enough for boot, yet substantial enough to prove execution was on
     * the AP's private stack rather than a BSP-side enqueue/dequeue shortcut. */
    uint64_t value = 0x9E3779B97F4A7C15ULL ^ (uint64_t)logical_id;
    for (uint32_t n = 0; n < 250000u; ++n) {
        value ^= value << 7;
        value ^= value >> 9;
        value += 0xD1B54A32D192ED03ULL + (uint64_t)n;
    }
    __atomic_store_n(&s_boot_probe_checksum[logical_id], value, __ATOMIC_RELEASE);
    __atomic_store_n(&s_boot_probe_done[logical_id], 1u, __ATOMIC_RELEASE);
}

#if COS_SMP_BOOT_STEAL_DIAGNOSTIC
/* This intentionally has no GUI, allocator, TCP, or scheduler-global side
 * effects. It is a bounded stand-in for an image decode/tile conversion job and
 * lets a debug boot validate that an idle AP steals from an overloaded peer. */
static void smp_steal_probe(void *arg) {
    uint64_t value = 0xA0761D6478BD642FULL ^ (uint64_t)(uintptr_t)arg;
    for (uint32_t n = 0; n < 350000u; ++n) {
        value ^= value >> 11;
        value *= 0x9E3779B185EBCA87ULL;
        value ^= value << 13;
    }
    __atomic_fetch_xor(&s_steal_probe_checksum, value, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_steal_probe_done, 1u, __ATOMIC_RELEASE);
}
#endif

static inline void smp_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t* eax, uint32_t* ebx,
                             uint32_t* ecx, uint32_t* edx) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(leaf), "c"(subleaf));
    if (eax) *eax = a;
    if (ebx) *ebx = b;
    if (ecx) *ecx = c;
    if (edx) *edx = d;
}

static bool smp_is_virtualbox(void) {
    uint32_t max_hypervisor = 0, ebx = 0, ecx = 0, edx = 0;
    smp_cpuid(1, 0, NULL, NULL, &ecx, NULL);
    if ((ecx & (1u << 31)) == 0) return false;
    smp_cpuid(0x40000000u, 0, &max_hypervisor, &ebx, &ecx, &edx);
    if (max_hypervisor < 0x40000000u) return false;
    char vendor[13];
    memcpy(vendor + 0, &ebx, 4);
    memcpy(vendor + 4, &ecx, 4);
    memcpy(vendor + 8, &edx, 4);
    vendor[12] = '\0';
    return memcmp(vendor, "VBoxVBoxVBox", 12) == 0;
}

static inline uint64_t smp_read_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3,%0" : "=r"(value));
    return value;
}

static inline uint64_t smp_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void smp_wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static void smp_delay(uint32_t cycles) {
    for (volatile uint32_t i = 0; i < cycles; ++i) {
        __asm__ volatile("pause");
    }
}

static uint32_t smp_detect_logical_cpus(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    smp_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_basic = eax;
    uint32_t count = 1;

    if (max_basic >= 0x0Bu) {
        for (uint32_t level = 0; level < 8; ++level) {
            uint32_t lb = 0, lc = 0;
            smp_cpuid(0x0Bu, level, NULL, &lb, &lc, NULL);
            uint32_t level_type = (lc >> 8) & 0xFFu;
            if (level_type == 0 || lb == 0) break;
            if (lb > count) count = lb;
        }
    }
    if (count == 1 && max_basic >= 1) {
        smp_cpuid(1, 0, NULL, &ebx, NULL, NULL);
        uint32_t legacy = (ebx >> 16) & 0xFFu;
        if (legacy) count = legacy;
    }
    if (count == 0) count = 1;
    if (count > SMP_MAX_CPUS) count = SMP_MAX_CPUS;
    return count;
}

static bool smp_sum_is_zero(const uint8_t* bytes, uint32_t length) {
    if (bytes == NULL || length < sizeof(smp_acpi_sdt_t) || length > 0x100000u) return false;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static const smp_acpi_sdt_t* smp_phys_sdt(uint64_t phys) {
    if (phys == 0 || phys >= (1ULL << 32)) return NULL;
    return (const smp_acpi_sdt_t*)(uintptr_t)PHYS_TO_VIRT(phys);
}

static const smp_acpi_madt_t* smp_find_madt(void) {
    uint64_t rsdp_addr = cos_mb2_get_acpi_rsdp();
    if (rsdp_addr == 0) return NULL;
    const smp_acpi_rsdp_t* rsdp = (const smp_acpi_rsdp_t*)(uintptr_t)rsdp_addr;
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) return NULL;

    const smp_acpi_sdt_t* root = NULL;
    uint32_t entry_bytes = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_phys != 0) {
        root = smp_phys_sdt(rsdp->xsdt_phys);
        entry_bytes = 8;
        if (!root || memcmp(root->signature, "XSDT", 4) != 0) root = NULL;
    }
    if (root == NULL && rsdp->rsdt_phys != 0) {
        root = smp_phys_sdt(rsdp->rsdt_phys);
        entry_bytes = 4;
        if (!root || memcmp(root->signature, "RSDT", 4) != 0) root = NULL;
    }
    if (!root || root->length < sizeof(smp_acpi_sdt_t) || root->length > 0x100000u ||
        !smp_sum_is_zero((const uint8_t*)root, root->length)) return NULL;

    uint32_t entries = (root->length - sizeof(smp_acpi_sdt_t)) / entry_bytes;
    const uint8_t* table_entries = (const uint8_t*)root + sizeof(smp_acpi_sdt_t);
    for (uint32_t i = 0; i < entries; ++i) {
        uint64_t phys = 0;
        if (entry_bytes == 8) memcpy(&phys, table_entries + (uint64_t)i * 8u, sizeof(phys));
        else {
            uint32_t phys32 = 0;
            memcpy(&phys32, table_entries + (uint64_t)i * 4u, sizeof(phys32));
            phys = phys32;
        }
        const smp_acpi_sdt_t* candidate = smp_phys_sdt(phys);
        if (candidate && memcmp(candidate->signature, "APIC", 4) == 0 &&
            candidate->length >= sizeof(smp_acpi_madt_t) && candidate->length <= 0x100000u &&
            smp_sum_is_zero((const uint8_t*)candidate, candidate->length)) {
            return (const smp_acpi_madt_t*)candidate;
        }
    }
    return NULL;
}

static void smp_add_cpu(uint32_t apic_id, uint32_t bsp_apic_id) {
    if (s_possible_cpus >= SMP_MAX_CPUS) return;
    if (apic_id == bsp_apic_id) return;
    for (uint32_t i = 0; i < s_possible_cpus; ++i) {
        if (s_cpus[i].present && s_cpus[i].apic_id == apic_id) return;
    }
    uint32_t index = s_possible_cpus++;
    s_cpus[index].logical_id = index;
    s_cpus[index].apic_id = apic_id;
    s_cpus[index].present = true;
    s_cpus[index].online = false;
}

static bool smp_enumerate_madt(uint32_t bsp_apic_id, uint32_t detected_count) {
    const smp_acpi_madt_t* madt = smp_find_madt();
    if (madt == NULL) return false;

    s_possible_cpus = 1;
    uint32_t offset = sizeof(smp_acpi_madt_t);
    while (offset + sizeof(smp_madt_entry_t) <= madt->header.length &&
           s_possible_cpus < SMP_MAX_CPUS) {
        const smp_madt_entry_t* entry = (const smp_madt_entry_t*)((const uint8_t*)madt + offset);
        if (entry->length < sizeof(smp_madt_entry_t) || offset + entry->length > madt->header.length) break;
        if (entry->type == 0 && entry->length >= 8) {
            const uint8_t* data = (const uint8_t*)entry;
            uint32_t flags = 0;
            memcpy(&flags, data + 4, sizeof(flags));
            if ((flags & 1u) != 0) smp_add_cpu(data[3], bsp_apic_id);
        } else if (entry->type == 9 && entry->length >= 16) {
            const uint8_t* data = (const uint8_t*)entry;
            uint32_t apic_id = 0, flags = 0;
            memcpy(&apic_id, data + 4, sizeof(apic_id));
            memcpy(&flags, data + 8, sizeof(flags));
            if ((flags & 1u) != 0) smp_add_cpu(apic_id, bsp_apic_id);
        }
        offset += entry->length;
    }
    /* The enabled processor entries of the MADT are the firmware's list of the CPUs that exist. `detected_count` (CPUID leaf 0xB / leaf 1) only says how many
     * logical processors one PACKAGE has: QEMU's -smp N (N sockets of one core, the default of older QEMU and of some Windows builds) and multi-socket machines
     * report 1 there, and capping the list by it left the system with CPU 0 alone. An AP that does not start is dropped by its own start-up timeout. */
    (void)detected_count;
    return s_possible_cpus >= 1;
}

static void smp_lapic_wait_idle(void) {
    if (s_lapic == NULL) return;
    /* Firmware occasionally leaves the delivery-status bit asserted while
     * an INIT is being retired.  Do not stall BSP startup indefinitely: the
     * following architecturally required delays still separate INIT/SIPI. */
    for (uint32_t i = 0; i < 1000u; ++i) {
        if ((s_lapic[SMP_LAPIC_ICR_LOW / 4u] & SMP_LAPIC_ICR_DELIVERY) == 0) return;
        __asm__ volatile("pause");
    }
}

static void smp_lapic_send_ipi(uint32_t apic_id, uint32_t low) {
    s_lapic[SMP_LAPIC_ICR_HIGH / 4u] = apic_id << 24;
    s_lapic[SMP_LAPIC_ICR_LOW / 4u] = low;
    smp_lapic_wait_idle();
}

/* ---- helpers for smp_user.c (ring3 compute jobs on APs) ---------------- */
cpu_local_t g_cpu_local[SMP_MAX_CPUS];

uint32_t smp_current_logical_id(void) { return cpu_id(); }       /* the TR selector, see percpu.h (an AP loads its TSS first thing in smp_ap_entry) */

volatile uint32_t *smp_lapic_registers(void) { return s_lapic; }

/* Stops every other CPU with an NMI (destination shorthand "all excluding self"): their NMI handler (idt.c) parks them when cos_panic_active is set. Used by the
 * panic screen so that nothing else draws over it or goes on writing to the disk. */
void smp_halt_other_cpus(void) {
    if (s_lapic == NULL || s_online_cpus <= 1) return;
    s_lapic[0x300 / 4] = 0x000C0400u;                               /* all excluding self, NMI, assert */
    for (uint32_t i = 0; i < 100000 && (s_lapic[0x300 / 4] & 0x1000u); ++i) __asm__ volatile("pause");
}

void smp_send_fixed_ipi(uint32_t logical_id, uint8_t vector) {
    if (s_lapic == NULL || logical_id >= s_possible_cpus || !s_cpus[logical_id].present) return;
    smp_lapic_send_ipi(s_cpus[logical_id].apic_id, 0x00004000u | vector); /* fixed, assert */
}

/* ---- SMP scheduler: Local APIC timer, IPIs, idle threads ---------------------------------------------------------------------------- */
static volatile uint32_t s_sched_go;            /* set by the boot CPU: the APs may start scheduling */
static volatile uint32_t s_sched_ready;         /* how many APs have entered their idle thread */
static uint32_t s_lapic_ticks_per_ms;
static bool s_no_ap_timer;                      /* /etc/smp_notimer: leave the APs' own timers off (test of the watchdog; also a diagnostic switch) */
static volatile uint64_t s_tlb_addr;            /* 0: flush everything */
static volatile uint64_t s_tlb_pending;         /* CPUs that still have to flush (bit per logical CPU) */
extern void scheduler_cpu_enter(struct thread *idle);
extern void scheduler_cpu_tick(bool kicked);
extern void scheduler_capture_interrupt_context(struct regs *r);
extern void scheduler_end_interrupt_tick(void);
extern struct thread *thread_create_cpu_idle(void *entry, void *arg);
extern void idt_load_this_cpu(void);
extern void idt_set_handler(uint8_t vec, void (*fn)(struct regs *));

/* Flush what another CPU asked for. Called from the IPI and from the spin loops that wait for the kernel lock with interrupts off. */
void smp_service_requests(void) {
    uint32_t me = cpu_id();
    if (!(s_tlb_pending & (1ull << me))) return;
    uint64_t a = s_tlb_addr;
    if (a) __asm__ volatile("invlpg (%0)" :: "r"(a) : "memory");
    else { uint64_t cr3; __asm__ volatile("mov %%cr3,%0" : "=r"(cr3)); __asm__ volatile("mov %0,%%cr3" :: "r"(cr3) : "memory"); }
    __atomic_fetch_and(&s_tlb_pending, ~(1ull << me), __ATOMIC_ACQ_REL);
}

/* The page tables of the current address space (or the kernel's) changed in a way another CPU may have cached: make every scheduling CPU drop it.
 * The caller holds the kernel lock, so there is one request at a time. */
void smp_tlb_shootdown(uint64_t addr) {
    if (!__atomic_load_n(&s_sched_ready, __ATOMIC_ACQUIRE)) return;
    extern uint64_t sync_irq_save(void);
    extern void sync_irq_restore(uint64_t);
    uint64_t lock_flags = sync_irq_save();                       /* one request at a time */
    uint32_t me = cpu_id();
    uint64_t mask = 0;
    for (uint32_t c = 0; c < s_possible_cpus && c < 64; ++c) {
        if (c == me) continue;
        if (c != 0 && !g_cpu_local[c].sched_enabled) continue;
        if (c < 32u && (__atomic_load_n(&s_user_busy_mask, __ATOMIC_ACQUIRE) & (1u << c))) continue;   /* in a compute job: flushed when the job ends */
        mask |= 1ull << c;
    }
    if (!mask) { sync_irq_restore(lock_flags); return; }
    s_tlb_addr = addr;
    __atomic_store_n(&s_tlb_pending, mask, __ATOMIC_RELEASE);
    for (uint32_t c = 0; c < 64; ++c)
        if (mask & (1ull << c)) smp_send_fixed_ipi(c, (uint8_t)SMP_VEC_TLB);
    for (uint32_t spin = 0; __atomic_load_n(&s_tlb_pending, __ATOMIC_ACQUIRE); ++spin) {
        __asm__ volatile("pause");
        if (spin > 200000000u) { serial_puts("[SMP] TLB shootdown timed out\n"); s_tlb_pending = 0; break; }
    }
    sync_irq_restore(lock_flags);
}

/* Make CPU `cpu` look at the run queues now (a thread of an exited process, a signal, queued kernel work, a thread that became ready). mode 1: reschedule
 * the running thread; mode 2: only account a tick (the watchdog below, when the CPU's own timer interrupts do not arrive). */
static void smp_ipi_cpu(uint32_t cpu, uint32_t mode) {
    if (cpu == 0 || cpu >= s_possible_cpus || !g_cpu_local[cpu].sched_enabled) return;
    if (cpu < 32u && (__atomic_load_n(&s_user_busy_mask, __ATOMIC_ACQUIRE) & (1u << cpu))) return;
    g_cpu_local[cpu].kick = mode;
    smp_send_fixed_ipi(cpu, (uint8_t)SMP_VEC_KICK);
}
void smp_kick_cpu(uint32_t cpu) { smp_ipi_cpu(cpu, 1); }
void smp_tick_cpu(uint32_t cpu) { smp_ipi_cpu(cpu, 2); }

/* An idle AP does not need a tick every millisecond: before it halts its LAPIC timer is slowed to one tick per 16 ms (a deadman, in case a wake-up IPI is ever missing), and the first
 * interrupt it takes (the tick, a kick IPI for a ready thread or queued work, a TLB request) puts the 1 ms tick back. On a hypervisor with the interrupt controller in user space
 * (QEMU WHPX with kernel-irqchip=off) every tick of every idle CPU costs a trip to QEMU. */
#define SMP_IDLE_SLOW_TICK_MS 16u
static volatile uint8_t s_slow_tick[SMP_MAX_CPUS];
static inline void smp_tick_fast(void) {
    uint32_t id = cpu_id();
    if (id < SMP_MAX_CPUS && s_slow_tick[id]) { s_slow_tick[id] = 0; if (s_lapic) s_lapic[0x380 / 4] = s_lapic_ticks_per_ms; }
}
static void smp_timer_irq(struct regs *r) {
    smp_tick_fast();
    { extern void cos_linux_prof_sample(struct regs *); cos_linux_prof_sample(r); }
    scheduler_capture_interrupt_context(r);
    scheduler_cpu_tick(false);
    scheduler_end_interrupt_tick();
}
static void smp_kick_irq(struct regs *r) {
    smp_tick_fast();
    { extern void cos_linux_prof_sample(struct regs *); cos_linux_prof_sample(r); }
    scheduler_capture_interrupt_context(r);
    uint32_t mode = g_cpu_local[cpu_id()].kick;
    g_cpu_local[cpu_id()].kick = 0;
    scheduler_cpu_tick(mode != 2u);
    scheduler_end_interrupt_tick();
}
static void smp_tlb_irq(struct regs *r) { (void)r; smp_tick_fast(); smp_service_requests(); }

/* Does CPU `cpu` have kernel work or a compute job waiting? Then its idle thread must run (scheduler.c: pick_next_task). */
bool smp_cpu_has_pending_work(uint32_t cpu) {
    return cpu < SMP_MAX_CPUS && __atomic_load_n(&s_work_count[cpu], __ATOMIC_ACQUIRE) != 0u;
}

uint32_t smp_lapic_timer_period(void) { return s_lapic_ticks_per_ms; }

static void smp_lapic_timer_calibrate(void) {
    if (!s_lapic || s_lapic_ticks_per_ms) return;
    extern uint64_t get_timer_ticks(void);
    s_lapic[0x3E0 / 4] = 0x3;                  /* divide by 16 */
    s_lapic[0x320 / 4] = 0x10000;              /* one-shot, masked */
    s_lapic[0x380 / 4] = 0xFFFFFFFFu;
    uint64_t t0 = get_timer_ticks();
    while (get_timer_ticks() - t0 < 20) __asm__ volatile("pause");
    uint32_t left = s_lapic[0x390 / 4];
    s_lapic[0x380 / 4] = 0;
    uint32_t per_ms = (0xFFFFFFFFu - left) / 20u;
    s_lapic_ticks_per_ms = per_ms >= 1000u ? per_ms : 1000u;
    serial_puts("[SMP] LAPIC timer: ticks per ms (div 16) = "); serial_putdec(s_lapic_ticks_per_ms); serial_puts("\n");
}

/* The AP's idle thread: queued kernel work first (interrupts off while it runs, as before), then a short window with interrupts on so the tick can hand
 * this CPU a user thread. */
static void smp_ap_idle_thread(void *arg) {
    (void)arg;
    uint32_t id = cpu_id();
    uint32_t idle_spins = 0;
    for (;;) {
        smp_work_item_t item;
        __asm__ volatile("cli");
        if (smp_take_work(id, &item) || smp_steal_work(id, &item)) {
            smp_execute_work(id, &item);
            idle_spins = 0;
            continue;
        }
        if (idle_spins < 6u) {                       /* work came a moment ago: keep polling briefly */
            __asm__ volatile("sti");
            uint32_t backoff = 1u << idle_spins;
            for (uint32_t i = 0; i < backoff; ++i) __asm__ volatile("pause");
            ++idle_spins;
        } else {
            /* Nothing to do: halt. The tick (1 ms), the kick IPI sent when work is queued for this CPU and a TLB request all wake it. sti;hlt is atomic, no wake-up
             * can fall between them. */
            if (s_lapic && s_lapic_ticks_per_ms && id < SMP_MAX_CPUS) { s_slow_tick[id] = 1; s_lapic[0x380 / 4] = s_lapic_ticks_per_ms * SMP_IDLE_SLOW_TICK_MS; }
            __asm__ volatile("sti; hlt");
        }
    }
}

static void smp_ap_enter_scheduler(uint32_t id) {
    idt_load_this_cpu();
    { extern void paging_configure_pat_this_cpu(void); paging_configure_pat_this_cpu(); }    /* the boot CPU has set the PAT by now */
    if (s_lapic) s_lapic[0x0F0 / 4] = 0x1FFu;                 /* software-enable this CPU's Local APIC (the timer needs it) */
    struct thread *idle = thread_create_cpu_idle((void *)smp_ap_idle_thread, NULL);
    if (!idle) { serial_puts("[SMP] AP idle thread create failed\n"); for (;;) { __asm__ volatile("cli; hlt"); } }
    g_cpu_local[id].id = id;
    g_cpu_local[id].sched_enabled = 1;
    if (s_lapic && !s_no_ap_timer) {                           /* periodic tick, 1 ms */
        s_lapic[0x3E0 / 4] = 0x3;
        s_lapic[0x320 / 4] = 0x20000u | SMP_VEC_TIMER;
        s_lapic[0x380 / 4] = s_lapic_ticks_per_ms;
    }
    __atomic_fetch_add(&s_sched_ready, 1u, __ATOMIC_ACQ_REL);
    scheduler_cpu_enter(idle);
    for (;;) { __asm__ volatile("cli; hlt"); }                  /* not reached */
}

/* The boot CPU, once the desktop is up and the PIT tick runs: let the APs schedule threads. */
void smp_sched_enable(void) {
    if (s_sched_go || s_possible_cpus <= 1) return;
    {   /* The switch back: a file /etc/smp_off keeps the application processors on queued kernel work only, as in earlier releases. */
        extern bool cos_fs_stat(const char *path, uint64_t *out_size, uint8_t *out_is_dir);
        uint64_t sz = 0; uint8_t dir = 0;
        if (cos_fs_stat("/etc/smp_off", &sz, &dir)) { serial_puts("[SMP] /etc/smp_off present: the application processors do not run threads\n"); return; }
    }
    {
        extern bool cos_fs_stat(const char *path, uint64_t *out_size, uint8_t *out_is_dir);
        uint64_t sz = 0; uint8_t dir = 0;
        if (cos_fs_stat("/etc/smp_notimer", &sz, &dir)) { s_no_ap_timer = true; serial_puts("[SMP] /etc/smp_notimer present: no Local APIC timer on the application processors\n"); }
    }
    smp_lapic_timer_calibrate();
    idt_set_handler((uint8_t)SMP_VEC_TIMER, smp_timer_irq);
    idt_set_handler((uint8_t)SMP_VEC_KICK, smp_kick_irq);
    idt_set_handler((uint8_t)SMP_VEC_TLB, smp_tlb_irq);
    __atomic_store_n(&s_sched_go, 1u, __ATOMIC_RELEASE);
    serial_puts("[SMP] scheduler enabled on the application processors\n");
}

void smp_ap_entry(void) {
    { extern void cos_xsave_init_cpu(void); cos_xsave_init_cpu(); }   /* before anything can interrupt this CPU: the stubs save state with XSAVE once the boot CPU has switched it on */
    uint32_t ebx = 0;
    smp_cpuid(1, 0, NULL, &ebx, NULL, NULL);
    uint32_t apic_id = (ebx >> 24) & 0xFFu;
    uint32_t logical_id = 0;
    for (uint32_t i = 1; i < s_possible_cpus; ++i) {
        if (s_cpus[i].present && s_cpus[i].apic_id == apic_id) {
            logical_id = i;
            gdt_cpu_init(i);                       /* from here on cpu_id() names this CPU */
            __atomic_store_n(&s_cpus[i].online, true, __ATOMIC_RELEASE);
            __atomic_fetch_add(&s_online_cpus, 1u, __ATOMIC_ACQ_REL);
            break;
        }
    }

    /* An AP leaves reset with CR0.CD and CR0.NW set: caching disabled, every memory access uncached (QEMU's emulation ignores it, real hardware and
     * hypervisors do not: code on such a CPU runs many times slower). Make CR0 what the boot CPU's is (caches on, MP set). */
    {
        uint64_t cr0;
        __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~((1ULL << 30) | (1ULL << 29));
        cr0 |= (1ULL << 1);
        __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
        { extern void paging_configure_pat_this_cpu(void); paging_configure_pat_this_cpu(); }
    }

    /* EFER.NXE is a per-CPU MSR, not a shared/global setting - the BSP
     * enabling it (kernel.c, right after paging_init()) has no effect on
     * this core at all. Without this, an AP that ever walked or built a
     * page table carrying a PAGE_NX bit would either fault on a
     * reserved-bit violation (if this core treats bit 63 as reserved
     * rather than NX) or simply not enforce it, depending on exactly
     * where that page table access happened - an inconsistency across
     * cores that would be far harder to track down than a single,
     * consistently-wrong or consistently-right BSP.
     *
     * This kernel does not yet run general per-process scheduling on
     * APs - see this function's own comment below on why they only pull
     * discrete work items from a queue for now - so nothing today
     * actually exercises an AP touching a user process's NX-bit page
     * table. Enabled anyway, on the same principle as clearing this
     * session's other latent SMP gaps as they are found rather than
     * leaving them for whenever per-CPU scheduling eventually lands: a
     * correctness property that is trivially cheap to establish now
     * (one CPUID+RDMSR+WRMSR sequence, already written and tested via
     * the BSP path) is worth establishing before something depends on
     * it, not after. */
    cos_elf_nx_enable();
    { extern void cos_security_harden_cpu(void); cos_security_harden_cpu(); }
    { extern void cos_syscall_init_cpu(void); cos_syscall_init_cpu(); }          /* the SYSCALL instruction of the Linux layer (gdt.c) */

    /* Never return to the trampoline's legacy HLT park. APs execute real
     * kernel work queued by the BSP. The pause idle path deliberately leaves
     * timer/scheduler IRQ handling BSP-only until its full per-CPU conversion. */
    /* Idle backoff.
     *
     * This loop previously retried smp_take_work()/smp_steal_work() on every
     * single iteration with only one `pause` between attempts. Both of those
     * take the per-CPU queue lock with an atomic exchange, so N idle APs
     * produced N continuous streams of atomic read-modify-writes on shared
     * cache lines - pure contention, since by definition there was no work.
     *
     * Measured consequence, booting with -smp 8 under QEMU: the BSP, polling
     * the ATA status port to bring up storage, advanced its 2,000,000-iteration
     * wait loop at roughly 60 iterations per second (loop counter sampled via
     * QMP 5s apart: 0x1e38ab -> 0x1e3779). At that rate the poll would need
     * about nine hours, so boot never got past storage init and -smp 8 looked
     * like a hang. -smp 1/2/4 booted normally; the cost scales with the number
     * of idle APs, which is the signature of contention rather than a logic bug.
     *
     * Backing off exponentially (capped) keeps the polling design and its
     * wakeup latency for the first few attempts, but drops the steady-state
     * atomic traffic of a fully idle AP by ~4096x. APs still cannot halt here:
     * they run with interrupts disabled and no IPI is sent on work submission,
     * so `hlt` would never wake. Making idle APs sleep properly needs an IDT
     * and a wakeup IPI on submit, which is a separate change. */
    /* Until the boot CPU has the desktop up (smp_sched_enable) the AP only runs queued kernel work, with interrupts off, as before. After that it
     * becomes a scheduling CPU: it enters its idle thread, whose loop is the same work loop with interrupts allowed between polls. */
    uint32_t idle_spins = 0;
    for (;;) {
        smp_work_item_t item;
        if (smp_take_work(logical_id, &item) ||
            smp_steal_work(logical_id, &item)) {
            smp_execute_work(logical_id, &item);
            idle_spins = 0;
        } else {
            if (__atomic_load_n(&s_sched_go, __ATOMIC_ACQUIRE)) smp_ap_enter_scheduler(logical_id);   /* does not return */
            uint32_t backoff = (idle_spins < 6u) ? (1u << idle_spins) : 64u;
            for (uint32_t i = 0; i < backoff; ++i) {
                __asm__ volatile("pause");
            }
            if (idle_spins < 6u) ++idle_spins;
        }
    }
}

static bool smp_prepare_ap_bootstrap(void) {
    uint64_t image_size = (uint64_t)(smp_ap_trampoline_end - smp_ap_trampoline_start);
    if (image_size == 0 || image_size > 4096u) return false;
    memcpy((void*)(uintptr_t)SMP_TRAMPOLINE_PHYS, smp_ap_trampoline_start, (size_t)image_size);

    smp_ap_mailbox_t* mailbox = (smp_ap_mailbox_t*)(uintptr_t)SMP_MAILBOX_PHYS;
    memset(mailbox, 0, sizeof(*mailbox));
    mailbox->cr3 = smp_read_cr3();
    __asm__ volatile("sgdt %0" : "=m"(mailbox->gdtr));
    mailbox->entry = (uint64_t)(uintptr_t)smp_ap_entry;
    return mailbox->cr3 != 0 && mailbox->gdtr.base != 0;
}

/* ---- Local APIC and I/O APIC for interrupts of devices (MSI, IOAPIC routes of kernel modules) ---- */
static uint32_t s_bsp_apic_id;
static volatile uint32_t* smp_map_mmio_page(uint64_t phys) {
    if (phys == 0 || phys >= (1ULL << 32)) return NULL;
    uint64_t virt = PHYS_TO_VIRT(phys);
    bool mapped = paging_is_present(virt);
    if (mapped && paging_virt_to_phys(virt) != phys) return NULL;
    if (!mapped && !paging_map_range(virt, phys, 4096u, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE)) return NULL;
    return (volatile uint32_t*)(uintptr_t)virt;
}
bool smp_lapic_ensure(void) {
    if (s_lapic) return true;
    if (!s_apic) return false;
    uint64_t apic_base = smp_rdmsr(SMP_LAPIC_MSR);
    apic_base |= SMP_LAPIC_ENABLE;
    smp_wrmsr(SMP_LAPIC_MSR, apic_base);
    volatile uint32_t* l = smp_map_mmio_page(apic_base & 0xFFFFF000ULL);
    if (!l) return false;
    l[SMP_LAPIC_SVR / 4u] = 0x1FFu;                         /* enabled, spurious vector 0xFF */
    s_lapic = l;
    return true;
}
uint32_t smp_bsp_apic_id(void) { return s_bsp_apic_id; }
void smp_lapic_eoi(void) { if (s_lapic) s_lapic[0xB0 / 4u] = 0; }
/* The first I/O APIC: from the MADT (type 1), else the architectural default. gsi_base is the first global system interrupt it serves. */
volatile uint32_t* smp_ioapic(uint32_t* gsi_base) {
    static volatile uint32_t* io; static uint32_t base; static bool done;
    if (!done) {
        done = true;
        uint64_t phys = 0xFEC00000ULL;
        const smp_acpi_madt_t* madt = smp_find_madt();
        if (madt) {
            uint32_t off = sizeof(smp_acpi_madt_t);
            while (off + sizeof(smp_madt_entry_t) <= madt->header.length) {
                const smp_madt_entry_t* e = (const smp_madt_entry_t*)((const uint8_t*)madt + off);
                if (e->length < sizeof(smp_madt_entry_t) || off + e->length > madt->header.length) break;
                if (e->type == 1 && e->length >= 12) {
                    uint32_t a, g; memcpy(&a, (const uint8_t*)e + 4, 4); memcpy(&g, (const uint8_t*)e + 8, 4);
                    phys = a; base = g; break;
                }
                off += e->length;
            }
        }
        io = smp_map_mmio_page(phys);
    }
    if (gsi_base) *gsi_base = base;
    return io;
}

static void smp_start_secondary_processors(void) {
    if (s_workers_started || !s_apic || s_possible_cpus <= 1) return;
    s_workers_started = true;

    uint64_t apic_base = smp_rdmsr(SMP_LAPIC_MSR);
    apic_base |= SMP_LAPIC_ENABLE;
    smp_wrmsr(SMP_LAPIC_MSR, apic_base);
    uint64_t lapic_phys = apic_base & 0xFFFFF000ULL;
    if (lapic_phys == 0 || lapic_phys >= (1ULL << 32)) return;
    /* The physical-RAM direct map may already cover LAPIC MMIO on large-memory
     * boots. Reuse a matching mapping; only create an explicit uncached map
     * when the virtual aperture is genuinely absent. */
    uint64_t lapic_virt = PHYS_TO_VIRT(lapic_phys);
    bool lapic_mapped = paging_is_present(lapic_virt);
    if (lapic_mapped && paging_virt_to_phys(lapic_virt) != lapic_phys) {
        serial_puts("[SMP] LAPIC virtual mapping conflicts with physical address\n");
        return;
    }
    if (!lapic_mapped && !paging_map_range(lapic_virt, lapic_phys, 4096u,
                                            PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE)) {
        serial_puts("[SMP] Unable to map Local APIC MMIO\n");
        return;
    }
    if (lapic_mapped) serial_puts("[SMP] LAPIC MMIO reused from direct map\n");
    serial_puts("[SMP] LAPIC MMIO mapped\n");
    s_lapic = (volatile uint32_t*)(uintptr_t)PHYS_TO_VIRT(lapic_phys);
    serial_puts("[SMP] LAPIC SVR write\n");
    s_lapic[SMP_LAPIC_SVR / 4u] = 0x1FFu;
    smp_lapic_wait_idle();
    serial_puts("[SMP] LAPIC ready; preparing AP trampoline\n");

    if (!smp_prepare_ap_bootstrap()) {
        serial_puts("[SMP] AP trampoline preparation failed\n");
        return;
    }
    serial_puts("[SMP] AP trampoline prepared\n");
    smp_ap_mailbox_t* mailbox = (smp_ap_mailbox_t*)(uintptr_t)SMP_MAILBOX_PHYS;

    for (uint32_t i = 1; i < s_possible_cpus; ++i) {
        if (!s_cpus[i].present) continue;
        mailbox->stack = (uint64_t)(uintptr_t)&s_ap_stacks[i][SMP_AP_STACK_SIZE];
        __atomic_thread_fence(__ATOMIC_RELEASE);
        serial_puts("[SMP] Starting AP ");
        serial_putdec(i);
        serial_puts("\n");

        /* INIT assertion/deassertion followed by two startup IPIs is accepted
         * by both legacy xAPIC firmware and QEMU/OVMF. */
        smp_lapic_send_ipi(s_cpus[i].apic_id, 0x0000C500u);
        smp_delay(1000u);
        smp_lapic_send_ipi(s_cpus[i].apic_id, 0x00008500u);
        smp_delay(1000u);
        smp_lapic_send_ipi(s_cpus[i].apic_id, 0x00000600u | SMP_TRAMPOLINE_VECTOR);
        smp_delay(2000u);
        smp_lapic_send_ipi(s_cpus[i].apic_id, 0x00000600u | SMP_TRAMPOLINE_VECTOR);

        bool online = false;
        for (uint32_t wait = 0; wait < 50000u; ++wait) {
            if (__atomic_load_n(&s_cpus[i].online, __ATOMIC_ACQUIRE)) {
                online = true;
                break;
            }
            __asm__ volatile("pause");
        }
        if (!online) {
            serial_puts("[SMP] AP startup timeout for APIC ID ");
            serial_putdec(s_cpus[i].apic_id);
            serial_puts("\n");
        }
    }
    s_ap_bootstrap_ready = s_online_cpus > 1;
    if (!s_ap_bootstrap_ready) return;

    /* Submit one CPU-targeted no-op probe to every online AP and wait for the
     * completion bit. This proves the secondary CPU executed C kernel work,
     * rather than merely acknowledging INIT/SIPI then parking in assembly. */
    for (uint32_t i = 1; i < s_possible_cpus; ++i) {
        if (!__atomic_load_n(&s_cpus[i].online, __ATOMIC_ACQUIRE)) continue;
        if (!smp_submit_work_to_cpu(i, smp_boot_probe, (void *)(uintptr_t)i)) {
            serial_puts("[SMP] AP work-queue submit failed for CPU ");
            serial_putdec(i);
            serial_puts("\n");
            continue;
        }
        bool completed = false;
        for (uint32_t wait = 0; wait < 500000u; ++wait) {
            if (__atomic_load_n(&s_boot_probe_done[i], __ATOMIC_ACQUIRE) != 0u) {
                completed = true;
                break;
            }
            __asm__ volatile("pause");
        }
        if (completed && __atomic_load_n(&s_boot_probe_checksum[i], __ATOMIC_ACQUIRE) != 0u) {
            serial_puts("[SMP] CPU parallel workload complete: ");
        } else {
            serial_puts("[SMP] CPU kernel worker timeout: ");
        }
        serial_putdec(i);
        serial_puts("\n");
    }

#if COS_SMP_BOOT_STEAL_DIAGNOSTIC
    /* Never enable this synthetic benchmark in an interactive image: it is
     * intentionally queue-heavy and belongs only to a controlled debug boot. */
    uint32_t jobs = (s_online_cpus > 2u) ? (s_online_cpus - 1u) * 3u : 0u;
    if (jobs != 0u) {
        __atomic_store_n(&s_steal_probe_done, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&s_steal_probe_checksum, 0u, __ATOMIC_RELEASE);
        for (uint32_t job = 0u; job < jobs; ++job) {
            if (!smp_submit_work_to_cpu_priority(1u, smp_steal_probe,
                                                 (void *)(uintptr_t)job,
                                                 SMP_WORK_PRIORITY_LOW)) {
                jobs = job;
                break;
            }
        }
        for (uint32_t wait = 0u; wait < 4000000u; ++wait) {
            if (__atomic_load_n(&s_steal_probe_done, __ATOMIC_ACQUIRE) >= jobs) break;
            __asm__ volatile("pause");
        }
    }
#endif
}

void smp_init(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    memset(s_cpus, 0, sizeof(s_cpus));
    memset(s_work_queue, 0, sizeof(s_work_queue));
    memset((void *)s_work_head, 0, sizeof(s_work_head));
    memset((void *)s_work_tail, 0, sizeof(s_work_tail));
    memset((void *)s_work_count, 0, sizeof(s_work_count));
    s_pending_work = 0u;
    memset((void *)s_work_lock, 0, sizeof(s_work_lock));
    memset((void *)s_completed_work, 0, sizeof(s_completed_work));
    memset((void *)s_stolen_work, 0, sizeof(s_stolen_work));
    memset((void *)s_boot_probe_done, 0, sizeof(s_boot_probe_done));
    memset((void *)s_boot_probe_checksum, 0, sizeof(s_boot_probe_checksum));
#if COS_SMP_BOOT_STEAL_DIAGNOSTIC
    s_steal_probe_done = 0u;
    s_steal_probe_checksum = 0u;
#endif
    s_next_work_cpu = 1u;
    s_workers_deferred = false;
    s_workers_started = false;
    smp_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
    (void)eax; (void)ecx;
    s_apic = (edx & (1u << 9)) != 0;
    uint32_t detected_count = smp_detect_logical_cpus();
    uint32_t bsp_apic_id = (ebx >> 24) & 0xFFu;
    s_bsp_apic_id = bsp_apic_id;

    s_possible_cpus = 1;
    s_online_cpus = 1;
    s_cpus[0].logical_id = 0;
    s_cpus[0].apic_id = bsp_apic_id;
    s_cpus[0].present = true;
    s_cpus[0].online = true;
    bool madt_ok = smp_enumerate_madt(bsp_apic_id, detected_count);
    if (!madt_ok) {
        /* Do not invent APIC IDs on a real machine; remain safely BSP-only
         * when firmware topology cannot be validated. */
        s_possible_cpus = 1;
    }

    serial_puts("[SMP] ACPI/CPUID topology: possible CPUs=");
    serial_putdec(s_possible_cpus);
    serial_puts(", BSP APIC ID=");
    serial_putdec(bsp_apic_id);
    serial_puts(s_apic ? ", local APIC=yes\n" : ", local APIC=no\n");

    /* VirtualBox 7 with EFI and several vCPUs has a valid MADT but can stall
     * display hand-off when INIT/SIPI is issued before its first framebuffer
     * presentation. Defer only that hypervisor's AP start; bare metal and
     * QEMU/KVM retain immediate SMP bring-up. */
    if (smp_is_virtualbox() && s_possible_cpus > 1u) {
        s_workers_deferred = true;
        serial_puts("[SMP] VirtualBox detected; AP workers deferred until desktop frame\n");
    } else {
        smp_start_secondary_processors();
    }
    serial_puts("[SMP] Online CPUs=");
    serial_putdec(s_online_cpus);
    serial_puts(" (AP per-CPU kernel work loops active)\n");
}

void smp_start_deferred_workers(void) {
    if (!s_workers_deferred) return;
    s_workers_deferred = false;
    serial_puts("[SMP] Starting deferred VirtualBox AP workers after desktop frame\n");
    smp_start_secondary_processors();
}

bool smp_workers_deferred(void) { return s_workers_deferred; }

uint32_t smp_possible_cpu_count(void) { return s_possible_cpus; }
uint32_t smp_online_cpu_count(void) { return s_online_cpus; }
bool smp_apic_available(void) { return s_apic; }

static bool s_enqueue_pinned;   /* set around an enqueue by smp_submit_pinned_work_to_cpu (under no lock: caller is single) */
static bool smp_enqueue_work_to_cpu(uint32_t logical_id, smp_work_fn_t fn,
                                    void *arg, smp_work_priority_t priority,
                                    smp_background_job_t *background_job) {
    if (fn == NULL || logical_id == 0u || logical_id >= s_possible_cpus ||
        !__atomic_load_n(&s_cpus[logical_id].online, __ATOMIC_ACQUIRE)) {
        return false;
    }
    /* A CPU busy with a ring3 job will not get to this for as long as that job runs. Callers treat
     * a refusal as "do it yourself" (the tiled BitBlt runs the tile inline). */
    if (logical_id < 32u && (__atomic_load_n(&s_user_busy_mask, __ATOMIC_ACQUIRE) & (1u << logical_id))) return false;
    bool queued = false;
    smp_work_lock_acquire(logical_id);
    if (s_work_count[logical_id] < SMP_WORK_QUEUE_CAPACITY) {
        smp_work_item_t *slot = &s_work_queue[logical_id][s_work_tail[logical_id]];
        slot->fn = fn;
        slot->arg = arg;
        slot->priority = smp_sanitize_priority(priority);
        slot->background_job = background_job;
        slot->pinned = s_enqueue_pinned;
        s_work_tail[logical_id] = (s_work_tail[logical_id] + 1u) % SMP_WORK_QUEUE_CAPACITY;
        ++s_work_count[logical_id];
        __atomic_fetch_add(&s_pending_work, 1u, __ATOMIC_RELEASE);
        queued = true;
    }
    smp_work_lock_release(logical_id);
    if (queued) smp_kick_cpu(logical_id);           /* a CPU running a user thread must give its idle thread a turn */
    return queued;
}

/* Queue work that only the named CPU may run (it is never stolen). Used for ring3 compute jobs, whose
 * bookkeeping assumes the job runs where it was placed. */
bool smp_submit_pinned_work_to_cpu(uint32_t logical_id, smp_work_fn_t fn, void *arg) {
    static volatile uint32_t lock;
    while (__atomic_exchange_n(&lock, 1u, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    s_enqueue_pinned = true;
    bool ok = smp_enqueue_work_to_cpu(logical_id, fn, arg, SMP_WORK_PRIORITY_NORMAL, NULL);
    s_enqueue_pinned = false;
    __atomic_store_n(&lock, 0u, __ATOMIC_RELEASE);
    return ok;
}

bool smp_submit_work_to_cpu_priority(uint32_t logical_id, smp_work_fn_t fn,
                                     void *arg, smp_work_priority_t priority) {
    return smp_enqueue_work_to_cpu(logical_id, fn, arg, priority, NULL);
}

bool smp_submit_work_to_cpu(uint32_t logical_id, smp_work_fn_t fn, void *arg) {
    return smp_submit_work_to_cpu_priority(logical_id, fn, arg,
                                           SMP_WORK_PRIORITY_NORMAL);
}

bool smp_submit_work_priority(smp_work_fn_t fn, void *arg, smp_work_priority_t priority) {
    if (fn == NULL || __atomic_load_n(&s_online_cpus, __ATOMIC_ACQUIRE) <= 1u) {
        return false;
    }

    /* Start rotating ties from the prior target, but always choose the least
     * loaded online AP. This prevents the previous round-robin design from
     * leaving a busy AP with a decode backlog while another core is idle. */
    uint32_t best_cpu = 0u;
    uint32_t best_depth = SMP_WORK_QUEUE_CAPACITY + 1u;
    uint32_t start = s_next_work_cpu;
    for (uint32_t offset = 0u; offset + 1u < s_possible_cpus; ++offset) {
        uint32_t cpu = start + offset;
        if (cpu >= s_possible_cpus) cpu = 1u + (cpu - s_possible_cpus);
        if (!__atomic_load_n(&s_cpus[cpu].online, __ATOMIC_ACQUIRE)) continue;
        if (cpu < 32u && (__atomic_load_n(&s_user_busy_mask, __ATOMIC_ACQUIRE) & (1u << cpu))) continue;
        uint32_t depth = __atomic_load_n(&s_work_count[cpu], __ATOMIC_RELAXED);
        if (depth < best_depth) {
            best_depth = depth;
            best_cpu = cpu;
            if (depth == 0u) break;
        }
    }
    if (best_cpu == 0u) return false;
    s_next_work_cpu = best_cpu + 1u;
    if (s_next_work_cpu >= s_possible_cpus) s_next_work_cpu = 1u;
    return smp_enqueue_work_to_cpu(best_cpu, fn, arg, priority, NULL);
}

bool smp_submit_work(smp_work_fn_t fn, void *arg) {
    return smp_submit_work_priority(fn, arg, SMP_WORK_PRIORITY_NORMAL);
}

uint64_t smp_cpu_completed_work(uint32_t logical_id) {
    if (logical_id >= SMP_MAX_CPUS) return 0;
    return __atomic_load_n(&s_completed_work[logical_id], __ATOMIC_ACQUIRE);
}

uint64_t smp_cpu_stolen_work(uint32_t logical_id) {
    if (logical_id >= SMP_MAX_CPUS) return 0;
    return __atomic_load_n(&s_stolen_work[logical_id], __ATOMIC_ACQUIRE);
}

void smp_background_job_init(smp_background_job_t *job, smp_work_fn_t fn,
                             void *arg, uint64_t frame_id,
                             uint64_t deadline_ticks,
                             smp_work_priority_t priority) {
    if (job == NULL) return;
    job->fn = fn;
    job->arg = arg;
    job->frame_id = frame_id;
    job->deadline_ticks = deadline_ticks;
    job->priority = smp_sanitize_priority(priority);
    __atomic_store_n(&job->assigned_cpu, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&job->state, SMP_BACKGROUND_JOB_IDLE, __ATOMIC_RELEASE);
}

bool smp_submit_background_job(smp_background_job_t *job) {
    if (job == NULL || job->fn == NULL ||
        __atomic_load_n(&s_online_cpus, __ATOMIC_ACQUIRE) <= 1u) return false;
    if (job->deadline_ticks != 0u && get_timer_ticks() > job->deadline_ticks) {
        __atomic_store_n(&job->state, SMP_BACKGROUND_JOB_CANCELLED, __ATOMIC_RELEASE);
        return false;
    }
    uint32_t expected = SMP_BACKGROUND_JOB_IDLE;
    if (!__atomic_compare_exchange_n(&job->state, &expected,
                                     SMP_BACKGROUND_JOB_QUEUED, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return false;
    }

    uint32_t best_cpu = 0u;
    uint32_t best_depth = SMP_WORK_QUEUE_CAPACITY + 1u;
    for (uint32_t cpu = 1u; cpu < s_possible_cpus; ++cpu) {
        if (!__atomic_load_n(&s_cpus[cpu].online, __ATOMIC_ACQUIRE)) continue;
        uint32_t depth = __atomic_load_n(&s_work_count[cpu], __ATOMIC_RELAXED);
        if (depth < best_depth) {
            best_depth = depth;
            best_cpu = cpu;
        }
    }
    if (best_cpu != 0u && smp_enqueue_work_to_cpu(best_cpu, job->fn, job->arg,
                                                   job->priority, job)) {
        return true;
    }
    __atomic_store_n(&job->state, SMP_BACKGROUND_JOB_IDLE, __ATOMIC_RELEASE);
    return false;
}

bool smp_background_job_is_done(const smp_background_job_t *job) {
    if (job == NULL) return true;
    uint32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    return state == SMP_BACKGROUND_JOB_COMPLETE ||
           state == SMP_BACKGROUND_JOB_CANCELLED;
}

/* Waits for a background job. An AP normally picks one up within microseconds, but APs can be fully
 * occupied (ring3 compute jobs, see smp_user.c, run for as long as their process likes); a job still
 * queued after a few thousand polls is taken back and run right here instead of waiting for a core
 * that may never come free. A job found cancelled was never run by anyone, so it is run here too. */
void smp_background_job_wait(smp_background_job_t *job) {
    if (job == NULL) return;
    /* Counted in polls, not timer ticks: this is exactly the situation (every AP busy) in which
     * the caller may be spinning with interrupts held off, so ticks cannot be trusted to advance. */
    uint32_t polls = 0;
    for (;;) {
        uint32_t st = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
        if (st == SMP_BACKGROUND_JOB_COMPLETE) return;
        if (st == SMP_BACKGROUND_JOB_CANCELLED ||
            (st == SMP_BACKGROUND_JOB_QUEUED && ++polls >= 4000u &&
             smp_cancel_background_job(job))) {
            if (job->fn) job->fn(job->arg);
            return;
        }
        thread_yield();
    }
}

bool smp_cancel_background_job(smp_background_job_t *job) {
    if (job == NULL) return false;
    uint32_t expected = SMP_BACKGROUND_JOB_QUEUED;
    if (__atomic_compare_exchange_n(&job->state, &expected,
                                    SMP_BACKGROUND_JOB_CANCELLED, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return true;
    }
    return __atomic_load_n(&job->state, __ATOMIC_ACQUIRE) ==
           SMP_BACKGROUND_JOB_CANCELLED;
}


/* ---- dynamic tile scheduler ---------------------------------------------------------------------------
 * A frame's pixel work is cut into many small tiles that every idle core pulls from one shared counter, so a
 * core that is slow, interrupted or busy simply takes fewer tiles and the others make up the difference - no
 * core is assigned a fixed share. The calling CPU (the BSP, which also fields interrupts and runs the GUI) takes
 * only a weighted share while helpers are making progress, and everything if they are not. APs that are running a
 * ring3 compute job are not recruited. */
#define SMP_TILE_SETS 32u
#define SMP_TILE_HELPERS 15u
typedef struct {
    smp_tile_fn_t fn;
    void *ctx;
    uint32_t ntiles;
    volatile uint32_t done;
    /* Generation (high 32 bits, bumped each time the slot is armed and when it is released) and the next tile to hand out (low 32 bits) in ONE word:
     * a tile is claimed with a compare-and-swap that only succeeds while the generation is the caller's. With two separate words a late helper of an
     * earlier call could pass the generation check, then take a tile number of the next call that re-armed the slot meanwhile, be counted in `done`
     * and let the owner return (and free its stack context) while tiles were still running. */
    volatile uint64_t claim;
    volatile uint32_t in_use;
    smp_background_job_t jobs[SMP_TILE_HELPERS];
} smp_tile_set_t;
/* Statically allocated: a queued helper entry may outlive the call that posted it (an AP can be busy for a
 * while), so it must never point into a stack frame. It carries (slot, generation) and ignores itself when the
 * slot has moved on. */
static smp_tile_set_t s_tile_sets[SMP_TILE_SETS];

static bool smp_tile_run_one(smp_tile_set_t *ts, uint32_t gen) {
    for (;;) {
        uint64_t cur = __atomic_load_n(&ts->claim, __ATOMIC_ACQUIRE);
        if ((uint32_t)(cur >> 32) != gen) return false;             /* the slot has moved on */
        uint32_t t = (uint32_t)cur;
        if (t >= ts->ntiles) return false;                          /* (if ntiles is of a newer call, the CAS below fails: the generation differs) */
        if (!__atomic_compare_exchange_n(&ts->claim, &cur, cur + 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        /* Claimed tile t of generation `gen`: the owner cannot retire or re-arm the slot until `done` reaches ntiles, which needs this tile. */
        ts->fn(ts->ctx, t);
        __atomic_fetch_add(&ts->done, 1u, __ATOMIC_ACQ_REL);
        return true;
    }
}
static void smp_tile_helper(void *arg) {
    uint64_t v = (uint64_t)(uintptr_t)arg;
    smp_tile_set_t *ts = &s_tile_sets[(uint32_t)(v >> 32) % SMP_TILE_SETS];
    uint32_t gen = (uint32_t)v;
    while (smp_tile_run_one(ts, gen)) {}
}

void smp_parallel_tiles(smp_tile_fn_t fn, void *ctx, uint32_t ntiles, uint32_t owner_weight_pct) {
    if (!fn || ntiles == 0) return;
    uint32_t online = __atomic_load_n(&s_online_cpus, __ATOMIC_ACQUIRE);
    smp_tile_set_t *ts = NULL; uint32_t slot = 0;
    if (online > 1u && ntiles > 1u) {
        for (uint32_t i = 0; i < SMP_TILE_SETS; ++i) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&s_tile_sets[i].in_use, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { ts = &s_tile_sets[i]; slot = i; break; }
        }
    }
    if (!ts) { for (uint32_t t = 0; t < ntiles; ++t) fn(ctx, t); return; }
    ts->fn = fn; ts->ctx = ctx; ts->ntiles = ntiles; ts->done = 0;
    uint32_t gen = (uint32_t)(__atomic_load_n(&ts->claim, __ATOMIC_ACQUIRE) >> 32) + 1u;
    __atomic_store_n(&ts->claim, (uint64_t)gen << 32, __ATOMIC_RELEASE);            /* publishes fn/ctx/ntiles and opens the claim counter at 0 */
    uint32_t njobs = 0;
    uint32_t busy = __atomic_load_n(&s_user_busy_mask, __ATOMIC_ACQUIRE);
    uint32_t want = 0;
    for (uint32_t c = 1; c < online && c < 32u; ++c) if (!(busy & (1u << c))) ++want;
    if (want > ntiles - 1u) want = ntiles - 1u;
    if (want > SMP_TILE_HELPERS) want = SMP_TILE_HELPERS;
    for (uint32_t i = 0; i < want; ++i) {
        smp_background_job_init(&ts->jobs[njobs], smp_tile_helper, (void *)(uintptr_t)(((uint64_t)slot << 32) | gen), 0, 0, SMP_WORK_PRIORITY_NORMAL);
        if (smp_submit_background_job(&ts->jobs[njobs])) ++njobs;
    }
    /* The owner's share while helpers are alive: weight / (weight + 100 per helper). */
    uint32_t quota = ntiles;
    if (njobs) {
        if (owner_weight_pct == 0) owner_weight_pct = 100;
        quota = (ntiles * owner_weight_pct) / (owner_weight_pct + 100u * njobs);
        if (quota < 1u) quota = 1u;
    }
    for (uint32_t i = 0; i < quota; ++i) if (!smp_tile_run_one(ts, gen)) break;
    /* Wait for the rest. Counted in polls (interrupts may be off, ticks unreliable). If helpers stop making
     * progress - none got scheduled, or all are stuck behind other work - the owner takes tiles itself. */
    uint32_t last = __atomic_load_n(&ts->done, __ATOMIC_ACQUIRE), stall = 0;
    while (last < ntiles) {
        uint32_t d = __atomic_load_n(&ts->done, __ATOMIC_ACQUIRE);
        if (d != last) { last = d; stall = 0; }
        else if (++stall >= 2000u) {
            if (smp_tile_run_one(ts, gen)) { last = __atomic_load_n(&ts->done, __ATOMIC_ACQUIRE); stall = 0; }
            else stall = 1000u;                    /* everything is claimed; in-flight tiles will finish */
        }
        __builtin_ia32_pause();
    }
    /* Retire the slot: late helpers see a new generation and do nothing; unstarted helper entries are cancelled. */
    __atomic_store_n(&ts->claim, ((uint64_t)(gen + 1u) << 32) | 0xFFFFFFFFu, __ATOMIC_RELEASE);   /* new generation, nothing left to claim */
    for (uint32_t i = 0; i < njobs; ++i) smp_cancel_background_job(&ts->jobs[i]);
    /* A helper that is mid-tile has already been counted in `done`, so no tile is still running. */
    __atomic_store_n(&ts->in_use, 0u, __ATOMIC_RELEASE);
}

bool smp_secondary_startup_ready(void) {
    return s_ap_bootstrap_ready;
}

const smp_cpu_info_t* smp_cpu_info(uint32_t logical_id) {
    if (logical_id >= s_possible_cpus) return NULL;
    return &s_cpus[logical_id];
}
