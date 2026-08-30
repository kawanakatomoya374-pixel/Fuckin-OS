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
#include "serial.h"
#include "string.h"
#include "mm/paging.h"
#include "timer.h"

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

static smp_cpu_info_t s_cpus[SMP_MAX_CPUS];
static uint8_t s_ap_stacks[SMP_MAX_CPUS][SMP_AP_STACK_SIZE]
    __attribute__((aligned(16)));
static volatile uint32_t s_possible_cpus = 1;
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
static bool smp_pop_best_locked(uint32_t logical_id, smp_work_item_t *out) {
    uint32_t count = s_work_count[logical_id];
    if (!out || count == 0u) return false;

    uint32_t best_offset = 0u;
    smp_work_priority_t best_priority = SMP_WORK_PRIORITY_LOW;
    for (uint32_t offset = 0; offset < count; ++offset) {
        uint32_t index = (s_work_head[logical_id] + offset) % SMP_WORK_QUEUE_CAPACITY;
        smp_work_priority_t priority = s_work_queue[logical_id][index].priority;
        if (offset == 0u || priority > best_priority) {
            best_offset = offset;
            best_priority = priority;
        }
    }

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
    bool found = smp_pop_best_locked(logical_id, out);
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
    bool stolen = smp_pop_best_locked(victim, out);
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
    if (detected_count < s_possible_cpus) s_possible_cpus = detected_count;
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

void smp_ap_entry(void) {
    uint32_t ebx = 0;
    smp_cpuid(1, 0, NULL, &ebx, NULL, NULL);
    uint32_t apic_id = (ebx >> 24) & 0xFFu;
    uint32_t logical_id = 0;
    for (uint32_t i = 1; i < s_possible_cpus; ++i) {
        if (s_cpus[i].present && s_cpus[i].apic_id == apic_id) {
            logical_id = i;
            __atomic_store_n(&s_cpus[i].online, true, __ATOMIC_RELEASE);
            __atomic_fetch_add(&s_online_cpus, 1u, __ATOMIC_ACQ_REL);
            break;
        }
    }

    /* Never return to the trampoline's legacy HLT park. APs execute real
     * kernel work queued by the BSP. The pause idle path deliberately leaves
     * timer/scheduler IRQ handling BSP-only until its full per-CPU conversion. */
    for (;;) {
        smp_work_item_t item;
        if (smp_take_work(logical_id, &item) ||
            smp_steal_work(logical_id, &item)) {
            smp_execute_work(logical_id, &item);
        } else {
            __asm__ volatile("pause");
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

static bool smp_enqueue_work_to_cpu(uint32_t logical_id, smp_work_fn_t fn,
                                    void *arg, smp_work_priority_t priority,
                                    smp_background_job_t *background_job) {
    if (fn == NULL || logical_id == 0u || logical_id >= s_possible_cpus ||
        !__atomic_load_n(&s_cpus[logical_id].online, __ATOMIC_ACQUIRE)) {
        return false;
    }
    bool queued = false;
    smp_work_lock_acquire(logical_id);
    if (s_work_count[logical_id] < SMP_WORK_QUEUE_CAPACITY) {
        smp_work_item_t *slot = &s_work_queue[logical_id][s_work_tail[logical_id]];
        slot->fn = fn;
        slot->arg = arg;
        slot->priority = smp_sanitize_priority(priority);
        slot->background_job = background_job;
        s_work_tail[logical_id] = (s_work_tail[logical_id] + 1u) % SMP_WORK_QUEUE_CAPACITY;
        ++s_work_count[logical_id];
        __atomic_fetch_add(&s_pending_work, 1u, __ATOMIC_RELEASE);
        queued = true;
    }
    smp_work_lock_release(logical_id);
    return queued;
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

bool smp_secondary_startup_ready(void) {
    return s_ap_bootstrap_ready;
}

const smp_cpu_info_t* smp_cpu_info(uint32_t logical_id) {
    if (logical_id >= s_possible_cpus) return NULL;
    return &s_cpus[logical_id];
}
