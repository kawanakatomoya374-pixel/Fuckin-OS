/*
 * smp_user.c - ring3 compute jobs on application processors
 *
 * See include/smp_user.h for the contract. Mechanism:
 *
 *  - The job is queued as an ordinary AP work item. The first time an AP runs
 *    one it builds a private GDT (a copy of the BSP's plus its own TSS), loads
 *    a private IDT and enables its Local APIC. The BSP's TSS and IDT are never
 *    shared: a TSS cannot be loaded on two CPUs, and the BSP's handlers assume
 *    BSP-only scheduler state.
 *  - apu_enter() switches to the process's CR3, installs its FS base and
 *    iretq's to the job's entry with a return address pointing at the user's
 *    `int $0x82` stub. The saved kernel context lives in the per-AP area.
 *  - Every way out lands in apu_common: the job returning (0x82), an
 *    exception or forbidden system call, or the kill IPI (0xFD). It records
 *    why, restores the kernel CR3 and returns from apu_enter() as if that
 *    were an ordinary function.
 *  - The per-AP area is a 64KiB-aligned block whose top is TSS.RSP0, so any
 *    ring3->ring0 entry finds its context with `rsp & ~0xFFFF`.
 */
#include "smp_user.h"
#include "smp.h"
#include "percpu.h"
#include "serial.h"
#include "string.h"
#include "arch/gdt.h"
#include "mm/paging.h"

#define APU_AREA_SIZE   0x10000u
#define APU_VEC_DONE    0x82u
#define APU_VEC_KILL    0xFDu
#define APU_VEC_SPUR    0xFFu

typedef struct {
    uint64_t saved_rsp;   /*  0 */
    uint64_t kernel_cr3;  /*  8 */
    smp_user_job_t *job;  /* 16 */
    uint64_t exit_vec;    /* 24 */
    uint64_t exit_err;    /* 32 */
    uint64_t exit_rip;    /* 40 */
    uint64_t exit_cr2;    /* 48 */
    volatile uint32_t *lapic; /* 56 */
    bool prepared;
} apu_ctx_t;

static uint8_t s_apu_area[SMP_MAX_CPUS][APU_AREA_SIZE] __attribute__((aligned(APU_AREA_SIZE)));

typedef struct __attribute__((packed)) {
    uint16_t off_lo; uint16_t sel; uint8_t ist; uint8_t attr;
    uint16_t off_mid; uint32_t off_hi; uint32_t zero;
} apu_idt_gate_t;
static apu_idt_gate_t s_apu_idt[256] __attribute__((aligned(16)));
static volatile uint32_t s_idt_ready;

static smp_user_job_t s_jobs[SMP_USER_MAX_JOBS];
static volatile uint32_t s_cpu_busy;   /* bit per logical CPU */

extern void apu_enter(apu_ctx_t *ctx, smp_user_job_t *job);

__asm__(
".text\n"
".global apu_enter\n"
"apu_enter:\n"
"    pushq %rbx\n pushq %rbp\n pushq %r12\n pushq %r13\n pushq %r14\n pushq %r15\n"
"    movq %rsp, 0(%rdi)\n"
"    movq %cr3, %rax\n"
"    movq %rax, 8(%rdi)\n"
"    movq 0(%rsi), %rax\n"
"    movq %rax, %cr3\n"
"    movl $0xC0000100, %ecx\n"          /* IA32_FS_BASE */
"    movq 40(%rsi), %rax\n"
"    movq %rax, %rdx\n shrq $32, %rdx\n wrmsr\n"
"    movq 24(%rsi), %r8\n"
"    subq $8, %r8\n"
"    movq 32(%rsi), %rax\n"
"    movq %cr4, %rcx\n"
"    testl $0x200000, %ecx\n"           /* CR4.SMAP: only then may (and must) AC be set; stac/clac are #UD on a CPU without SMAP */
"    jz apu_nosmap_a\n"
"    .byte 0x0f, 0x01, 0xcb\n"          /* stac: with SMAP on, the kernel may not store to the user stack without AC */
"apu_nosmap_a:\n"
"    movq %rax, (%r8)\n"                 /* return address = exit stub */
"    testl $0x200000, %ecx\n"
"    jz apu_nosmap_b\n"
"    .byte 0x0f, 0x01, 0xca\n"          /* clac */
"apu_nosmap_b:\n"
"    movq 8(%rsi), %r9\n"
"    movq 16(%rsi), %rdi\n"
"    movw $0x23, %ax\n movw %ax, %ds\n movw %ax, %es\n"
"    pushq $0x23\n"                       /* SS */
"    pushq %r8\n"                         /* RSP */
"    pushq $0x202\n"                      /* RFLAGS: IF=1 so the kill IPI lands */
"    pushq $0x1B\n"                       /* CS */
"    pushq %r9\n"                         /* RIP */
"    xorl %eax,%eax\n xorl %ebx,%ebx\n xorl %ecx,%ecx\n xorl %edx,%edx\n xorl %esi,%esi\n"
"    xorl %ebp,%ebp\n xorl %r8d,%r8d\n xorl %r9d,%r9d\n xorl %r10d,%r10d\n xorl %r11d,%r11d\n"
"    xorl %r12d,%r12d\n xorl %r13d,%r13d\n xorl %r14d,%r14d\n xorl %r15d,%r15d\n"
"    iretq\n"
/* Common landing for every vector. Stack: vec, err, rip, cs, rflags, rsp, ss. */
"apu_common:\n"
"    testb $3, 24(%rsp)\n"               /* CS.RPL == 0: a kernel-mode fault; no way back */
"    jnz 1f\n"
"    movw $0xE9, %dx\n movb $'!', %al\n outb %al, %dx\n"
"2:  cli\n hlt\n jmp 2b\n"
"1:  movq %rsp, %rax\n"
"    andq $-0x10000, %rax\n"             /* rax = ctx */
"    movq 0(%rsp), %rdx\n"               /* vector */
"    cmpq $0xD0, %rdx\n je 5f\n"        /* the scheduler tick, kick and TLB IPIs of the SMP scheduler (0xD0, 0xE0, 0xF1) can arrive just as a job starts; */
"    cmpq $0xE0, %rdx\n je 5f\n"        /* the job does not care: acknowledge and resume it (leaving one in service would stop this CPU's timer for good) */
"    cmpq $0xF1, %rdx\n jne 6f\n"
"5:  movq 56(%rax), %rcx\n"
"    movl $0, 0xB0(%rcx)\n"
"    addq $16, %rsp\n"
"    iretq\n"
"6:  cmpq $0xFD, %rdx\n"
"    jne 3f\n"
"    movq 16(%rax), %rcx\n"              /* job */
"    testq %rcx, %rcx\n jz 4f\n"
"    cmpl $0, 48(%rcx)\n je 4f\n"        /* kill IPI with no kill request: ignore */
"    jmp 3f\n"
"4:  movq 56(%rax), %rcx\n"
"    movl $0, 0xB0(%rcx)\n"              /* EOI, then resume the interrupted job */
"    addq $16, %rsp\n"
"    iretq\n"
"3:  movq %rdx, 24(%rax)\n"
"    movq 8(%rsp), %rcx\n movq %rcx, 32(%rax)\n"
"    movq 16(%rsp), %rcx\n movq %rcx, 40(%rax)\n"
"    movq %cr2, %rcx\n movq %rcx, 48(%rax)\n"
"    movq 8(%rax), %rcx\n movq %rcx, %cr3\n"   /* back on the kernel's page tables */
"    movq 0(%rax), %rsp\n"
"    popq %r15\n popq %r14\n popq %r13\n popq %r12\n popq %rbp\n popq %rbx\n"
"    ret\n"
"apu_spurious:\n"
"    iretq\n"
);

/* One stub per vector: push a dummy error code where the CPU does not, then
 * the vector number, and fall into apu_common. */
#define APU_STUB_ERR(v) __asm__(".text\napu_stub_" #v ":\n pushq $" #v "\n jmp apu_common\n");
#define APU_STUB_NOERR(v) __asm__(".text\napu_stub_" #v ":\n pushq $0\n pushq $" #v "\n jmp apu_common\n");
APU_STUB_NOERR(0) APU_STUB_NOERR(1) APU_STUB_NOERR(2) APU_STUB_NOERR(3)
APU_STUB_NOERR(4) APU_STUB_NOERR(5) APU_STUB_NOERR(6) APU_STUB_NOERR(7)
APU_STUB_ERR(8) APU_STUB_NOERR(9) APU_STUB_ERR(10) APU_STUB_ERR(11)
APU_STUB_ERR(12) APU_STUB_ERR(13) APU_STUB_ERR(14) APU_STUB_NOERR(15)
APU_STUB_NOERR(16) APU_STUB_ERR(17) APU_STUB_NOERR(18) APU_STUB_NOERR(19)
APU_STUB_NOERR(20) APU_STUB_ERR(21) APU_STUB_NOERR(22) APU_STUB_NOERR(23)
APU_STUB_NOERR(24) APU_STUB_NOERR(25) APU_STUB_NOERR(26) APU_STUB_NOERR(27)
APU_STUB_NOERR(28) APU_STUB_ERR(29) APU_STUB_ERR(30) APU_STUB_NOERR(31)
APU_STUB_NOERR(208) APU_STUB_NOERR(224) APU_STUB_NOERR(241)
APU_STUB_NOERR(128) APU_STUB_NOERR(130) APU_STUB_NOERR(253)
extern void apu_stub_0(void), apu_stub_1(void), apu_stub_2(void), apu_stub_3(void),
    apu_stub_4(void), apu_stub_5(void), apu_stub_6(void), apu_stub_7(void),
    apu_stub_8(void), apu_stub_9(void), apu_stub_10(void), apu_stub_11(void),
    apu_stub_12(void), apu_stub_13(void), apu_stub_14(void), apu_stub_15(void),
    apu_stub_16(void), apu_stub_17(void), apu_stub_18(void), apu_stub_19(void),
    apu_stub_20(void), apu_stub_21(void), apu_stub_22(void), apu_stub_23(void),
    apu_stub_24(void), apu_stub_25(void), apu_stub_26(void), apu_stub_27(void),
    apu_stub_28(void), apu_stub_29(void), apu_stub_30(void), apu_stub_31(void),
    apu_stub_208(void), apu_stub_224(void), apu_stub_241(void),
    apu_stub_128(void), apu_stub_130(void), apu_stub_253(void), apu_spurious(void);

static void apu_set_gate(uint32_t v, void (*fn)(void), uint8_t attr) {
    uint64_t a = (uint64_t)(uintptr_t)fn;
    s_apu_idt[v].off_lo = (uint16_t)a;
    s_apu_idt[v].sel = GDT_KERNEL_CODE;
    s_apu_idt[v].ist = 0;
    s_apu_idt[v].attr = attr;
    s_apu_idt[v].off_mid = (uint16_t)(a >> 16);
    s_apu_idt[v].off_hi = (uint32_t)(a >> 32);
    s_apu_idt[v].zero = 0;
}

static void apu_build_idt_once(void) {
    if (__atomic_load_n(&s_idt_ready, __ATOMIC_ACQUIRE)) return;
    static void (*const exc[32])(void) = {
        apu_stub_0, apu_stub_1, apu_stub_2, apu_stub_3, apu_stub_4, apu_stub_5, apu_stub_6, apu_stub_7,
        apu_stub_8, apu_stub_9, apu_stub_10, apu_stub_11, apu_stub_12, apu_stub_13, apu_stub_14, apu_stub_15,
        apu_stub_16, apu_stub_17, apu_stub_18, apu_stub_19, apu_stub_20, apu_stub_21, apu_stub_22, apu_stub_23,
        apu_stub_24, apu_stub_25, apu_stub_26, apu_stub_27, apu_stub_28, apu_stub_29, apu_stub_30, apu_stub_31 };
    /* Identical contents from every caller, so a benign race is harmless. */
    for (uint32_t v = 0; v < 32; ++v) apu_set_gate(v, exc[v], 0x8E);
    apu_set_gate(SMP_VEC_TIMER, apu_stub_208, 0x8E);
    apu_set_gate(SMP_VEC_KICK, apu_stub_224, 0x8E);
    apu_set_gate(SMP_VEC_TLB, apu_stub_241, 0x8E);
    apu_set_gate(128, apu_stub_128, 0x8E);      /* int $0x80: forbidden, reported as a fault */
    apu_set_gate(APU_VEC_DONE, apu_stub_130, 0xEE); /* DPL 3: the exit stub */
    apu_set_gate(APU_VEC_KILL, apu_stub_253, 0x8E);
    apu_set_gate(APU_VEC_SPUR, apu_spurious, 0x8E);
    __atomic_store_n(&s_idt_ready, 1u, __ATOMIC_RELEASE);
}

static void apu_prepare(uint32_t id) {
    apu_ctx_t *ctx = (apu_ctx_t *)s_apu_area[id];
    if (ctx->prepared) return;
    apu_build_idt_once();

    /* The shared GDT with this CPU's own TSS; ring3 -> ring0 entries during a job land on the top of the per-AP area. */
    gdt_cpu_init(id);
    *tss_esp0_ptr(id) = (uint64_t)(uintptr_t)(s_apu_area[id] + APU_AREA_SIZE);
    ctx->lapic = smp_lapic_registers();
    if (ctx->lapic) ctx->lapic[0x0F0 / 4] = 0x1FFu;           /* software-enable this CPU's LAPIC */
    ctx->prepared = true;
}

static void apu_job_fn(void *p) {
    smp_user_job_t *job = (smp_user_job_t *)p;
    uint32_t id = smp_current_logical_id();
    if (id == 0 || id >= SMP_MAX_CPUS) {
        job->state = SMP_USER_JOB_FAULTED;
        __atomic_fetch_and(&s_cpu_busy, ~(1u << job->claim), __ATOMIC_ACQ_REL);
        return;
    }
    apu_ctx_t *ctx = (apu_ctx_t *)s_apu_area[id];
    apu_prepare(id);
    if (job->kill_req) { __atomic_store_n(&job->state, SMP_USER_JOB_KILLED, __ATOMIC_RELEASE); goto out; }
    ctx->job = job;
    job->cpu = id;
    smp_cpu_set_user_busy(id, true);
    __atomic_store_n(&job->state, SMP_USER_JOB_RUNNING, __ATOMIC_RELEASE);
    ctx->exit_vec = 0;
    /* The job runs on this CPU's private IDT (its exits are the only vectors it knows), so the scheduler tick is stopped meanwhile and the kernel
     * IDT is back afterwards. */
    if (ctx->lapic) ctx->lapic[0x320 / 4] = 0x10000u;
    {   struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr =
            { (uint16_t)(sizeof(s_apu_idt) - 1), (uint64_t)(uintptr_t)s_apu_idt };
        __asm__ volatile("lidt %0" : : "m"(idtr)); }
    apu_enter(ctx, job);
    { extern void idt_load_this_cpu(void); idt_load_this_cpu(); }
    if (ctx->lapic && g_cpu_local[id].sched_enabled) {
        extern uint32_t smp_lapic_timer_period(void);
        ctx->lapic[0x320 / 4] = 0x20000u | SMP_VEC_TIMER;
        ctx->lapic[0x380 / 4] = smp_lapic_timer_period();
    }
    ctx->job = NULL;
    smp_cpu_set_user_busy(id, false);
    uint64_t v = ctx->exit_vec;
    if (v == APU_VEC_KILL && ctx->lapic) ctx->lapic[0xB0 / 4] = 0;   /* EOI for the aborted interrupt */
    job->fault_vec = v; job->fault_err = ctx->exit_err;
    job->fault_rip = ctx->exit_rip; job->fault_cr2 = ctx->exit_cr2;
    __atomic_store_n(&job->state,
                     v == APU_VEC_DONE ? SMP_USER_JOB_DONE :
                     v == APU_VEC_KILL ? SMP_USER_JOB_KILLED : SMP_USER_JOB_FAULTED,
                     __ATOMIC_RELEASE);
out:
    __atomic_fetch_and(&s_cpu_busy, ~(1u << job->claim), __ATOMIC_ACQ_REL);
}

uint32_t smp_user_cpu_count(void) {
    uint32_t n = smp_online_cpu_count();
    return n > 1 ? n - 1 : 0;
}

int smp_user_start(void *owner, uint64_t cr3, uint64_t entry, uint64_t arg,
                   uint64_t user_rsp, uint64_t exit_stub, uint64_t fs_base) {
    int id = -1;
    for (int i = 0; i < SMP_USER_MAX_JOBS; ++i) {
        uint32_t expect = SMP_USER_JOB_FREE;
        if (__atomic_compare_exchange_n(&s_jobs[i].state, &expect, SMP_USER_JOB_QUEUED, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { id = i; break; }
    }
    if (id < 0) return -12;
    smp_user_job_t *j = &s_jobs[id];
    j->cr3 = cr3; j->entry = entry; j->arg = arg; j->user_rsp = user_rsp & ~0xFull;
    j->exit_stub = exit_stub; j->fs_base = fs_base; j->kill_req = 0; j->owner = owner;
    j->fault_vec = j->fault_err = j->fault_rip = j->fault_cr2 = 0;
    /* Claim an idle AP, then hand it the job. */
    uint32_t cpus = smp_online_cpu_count();
    for (uint32_t c = 1; c < cpus && c < SMP_MAX_CPUS; ++c) {
        uint32_t bit = 1u << c;
        if (__atomic_fetch_or(&s_cpu_busy, bit, __ATOMIC_ACQ_REL) & bit) continue;
        j->claim = c;
        if (smp_submit_pinned_work_to_cpu(c, apu_job_fn, j)) return id;
        __atomic_fetch_and(&s_cpu_busy, ~bit, __ATOMIC_ACQ_REL);
    }
    __atomic_store_n(&j->state, SMP_USER_JOB_FREE, __ATOMIC_RELEASE);
    return -16;
}

static smp_user_job_t *job_of(void *owner, int id) {
    if (id < 0 || id >= SMP_USER_MAX_JOBS) return NULL;
    smp_user_job_t *j = &s_jobs[id];
    return (j->state != SMP_USER_JOB_FREE && j->owner == owner) ? j : NULL;
}

int smp_user_poll(void *owner, int id, smp_user_job_t *out) {
    smp_user_job_t *j = job_of(owner, id);
    if (!j) return -22;
    int st = (int)__atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (out) *out = *j;
    return st;
}

int smp_user_stop(void *owner, int id) {
    smp_user_job_t *j = job_of(owner, id);
    if (!j) return -22;
    __atomic_store_n(&j->kill_req, 1u, __ATOMIC_RELEASE);
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == SMP_USER_JOB_RUNNING)
        smp_send_fixed_ipi(j->cpu, APU_VEC_KILL);
    return 0;
}

int smp_user_free(void *owner, int id) {
    smp_user_job_t *j = job_of(owner, id);
    if (!j) return -22;
    uint32_t st = j->state;
    if (st == SMP_USER_JOB_QUEUED || st == SMP_USER_JOB_RUNNING) return -16;
    __atomic_store_n(&j->state, SMP_USER_JOB_FREE, __ATOMIC_RELEASE);
    return 0;
}

void smp_user_kill_owner(void *owner) {
    for (int i = 0; i < SMP_USER_MAX_JOBS; ++i) {
        smp_user_job_t *j = &s_jobs[i];
        if (j->owner != owner) continue;
        uint32_t st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
        if (st != SMP_USER_JOB_QUEUED && st != SMP_USER_JOB_RUNNING) {
            if (st != SMP_USER_JOB_FREE) __atomic_store_n(&j->state, SMP_USER_JOB_FREE, __ATOMIC_RELEASE);
            continue;
        }
        __atomic_store_n(&j->kill_req, 1u, __ATOMIC_RELEASE);
        for (uint32_t spin = 0; spin < 50000000u; ++spin) {
            st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
            if (st != SMP_USER_JOB_QUEUED && st != SMP_USER_JOB_RUNNING) break;
            if (st == SMP_USER_JOB_RUNNING && (spin & 0xFFFFu) == 0) smp_send_fixed_ipi(j->cpu, APU_VEC_KILL);
            __asm__ volatile("pause");
        }
        __atomic_store_n(&j->state, SMP_USER_JOB_FREE, __ATOMIC_RELEASE);
        serial_puts("[SMPU] stopped a compute job left running at process exit\n");
    }
}
