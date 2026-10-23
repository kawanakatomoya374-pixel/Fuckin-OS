/**
 * page_fault.c - Page Fault Handler
 * C-OS 4.0.8 alpha
 *
 * Handles page faults with proper error analysis and simple recovery.
 * This version is wired to the active 64-bit paging implementation.
 */

#include "types.h"
#include "io.h"
#include "serial.h"
#include "mm/paging.h"
#include "task.h"
#include "idt.h"
#include <stdint.h>
#include <string.h>

/* Page fault error codes */
#define PF_PRESENT     (1 << 0)  /* 1 = protection violation, 0 = not-present */
#define PF_WRITE       (1 << 1)  /* Write operation */
#define PF_USER        (1 << 2)  /* User mode */
#define PF_RESERVED    (1 << 3)  /* Reserved bit violation */
#define PF_INSTRUCTION (1 << 4)  /* Instruction fetch */

#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1ULL))

/* Page fault statistics */
typedef struct {
    uint64_t total_faults;
    uint64_t not_present_faults;
    uint64_t protection_faults;
    uint64_t write_faults;
    uint64_t user_faults;
    uint64_t demand_pages;
    uint64_t lazy_allocations;
    uint64_t stack_growths;
} page_fault_stats_t;

static page_fault_stats_t pf_stats = {0};

static void analyze_fault(uint64_t error_code, uint64_t fault_addr) {
    serial_puts("[PF] Page fault analysis:\n");
    serial_puts("  Address: 0x"); serial_puthex(fault_addr); serial_puts("\n");
    serial_puts("  Error code: 0x"); serial_puthex(error_code); serial_puts("\n");

    if (!(error_code & PF_PRESENT)) {
        serial_puts("  Cause: Page not present\n");
        pf_stats.not_present_faults++;
    } else {
        serial_puts("  Cause: Protection violation\n");
        pf_stats.protection_faults++;
    }

    if (error_code & PF_WRITE) {
        serial_puts("  Cause: Write operation\n");
        pf_stats.write_faults++;
    } else {
        serial_puts("  Cause: Read operation\n");
    }

    if (error_code & PF_USER) {
        serial_puts("  Cause: User mode access\n");
        pf_stats.user_faults++;
    } else {
        serial_puts("  Cause: Kernel mode access\n");
    }

    if (error_code & PF_RESERVED) {
        serial_puts("  Cause: Reserved bit violation\n");
    }

    if (error_code & PF_INSTRUCTION) {
        serial_puts("  Cause: Instruction fetch\n");
    }

    pf_stats.total_faults++;
}

static bool is_kernel_stack_area(uint64_t fault_addr) {
    thread_t* th = thread_get_current();
    if (!th || !th->kernel_stack) return false;

    uint64_t stack_top = th->kernel_stack;
    /* Use this thread's actual stack allocation, not the KERNEL_STACK_SIZE
     * default: a thread created via thread_create_stack_size() (e.g.
     * gui_main, see kernel.c) can have a larger real stack, and hardcoding
     * the default here would make this recognize only the bottom
     * KERNEL_STACK_SIZE of it as legitimate, misdiagnosing a fault in the
     * rest of that thread's own, real stack. */
    uint64_t effective_size = th->kernel_stack_size ? th->kernel_stack_size : KERNEL_STACK_SIZE;
    uint64_t stack_bottom = (stack_top > effective_size) ? (stack_top - effective_size) : 0;
    return fault_addr >= stack_bottom && fault_addr < stack_top;
}

static bool map_zeroed_page(uint64_t virt_addr, uint64_t flags) {
    uint64_t paddr = paging_alloc_physical();
    if (!paddr) {
        serial_puts("[PF] Out of physical memory\n");
        return false;
    }

    if (!paging_map_page(PAGE_ALIGN_DOWN(virt_addr), paddr, flags)) {
        paging_free_physical(paddr);
        serial_puts("[PF] Map failed\n");
        return false;
    }

    memset((void *)(uintptr_t)PHYS_TO_VIRT(paddr), 0, PAGE_SIZE);
    return true;
}

static bool handle_demand_paging(uint64_t fault_addr, uint64_t error_code) {
    /* The active task layer already handles legitimate lazy materialization
     * for the user heap and user stack via task_handle_page_fault().  Keep
     * this legacy hook disabled so we do not accidentally map arbitrary
     * user addresses that merely happen to fall inside a broad numeric
     * window. */
    (void)fault_addr;
    (void)error_code;
    return false;
}

static bool handle_lazy_allocation(uint64_t fault_addr, uint64_t error_code) {
    (void)fault_addr;
    (void)error_code;
    return false;
}

static bool handle_stack_growth(uint64_t fault_addr, uint64_t error_code) {
    if (is_kernel_stack_area(fault_addr) && !(error_code & PF_PRESENT) && (error_code & PF_WRITE)) {
        serial_puts("[PF] Kernel stack growth\n");
        if (!map_zeroed_page(fault_addr, PAGE_PRESENT | PAGE_RW)) {
            serial_puts("[PF] Stack growth failed\n");
            return false;
        }
        pf_stats.stack_growths++;
        serial_puts("[PF] Stack growth successful\n");
        return true;
    }
    return false;
}

uint64_t page_fault_rip;   /* RIP of the faulting instruction, set by the #PF dispatcher (idt.c) */
struct regs *page_fault_regs;
uint64_t page_fault_hist[8][2];   /* last faults (rip, address), for the report on an unhandled one */
uint32_t page_fault_hist_n;

void page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    /* Demand-paged codec heaps can touch thousands of fresh pages during
     * decoder start-up.  Do not serialize a full diagnostic report for every
     * recoverable fault: task_handle_page_fault() already keeps a bounded
     * counter and emits a sparse progress record.  Besides flooding the
     * serial device, the old ordering made the diagnostic I/O part of the
     * Media frame-time path and made VP9 appear dramatically slower under
     * QEMU.  Invalid/protection faults still take the full diagnostic path
     * below. */
    if (task_handle_page_fault(fault_addr, error_code)) return;
    if (handle_demand_paging(fault_addr, error_code)) return;
    if (handle_stack_growth(fault_addr, error_code)) return;

    serial_puts("[PF] Page fault occurred\n");
    analyze_fault(error_code, fault_addr);

    if (error_code & PF_USER) {
        process_t* proc = process_get_current();
        { extern void cos_linux_crash_report(process_t *p, uint64_t rip, uint64_t addr); cos_linux_crash_report(proc, page_fault_rip, fault_addr); }
        serial_puts("[PF] Unhandled user page fault - terminating process, rip=0x");
        serial_puthex(page_fault_rip);
        serial_puts("\n");
        if (page_fault_regs) {
            const struct regs *pr = page_fault_regs;
            serial_puts("[PF]   rdi=0x"); serial_puthex(pr->rdi); serial_puts(" rsi=0x"); serial_puthex(pr->rsi);
            serial_puts(" rdx=0x"); serial_puthex(pr->rdx); serial_puts(" rcx=0x"); serial_puthex(pr->rcx);
            serial_puts(" rsp=0x"); serial_puthex(pr->rsp); serial_puts("\n");
        }
        for (uint32_t k = page_fault_hist_n > 8 ? page_fault_hist_n - 8 : 0; k < page_fault_hist_n; ++k) {
            serial_puts("[PF]   recent rip=0x"); serial_puthex(page_fault_hist[k & 7][0]);
            serial_puts(" addr=0x"); serial_puthex(page_fault_hist[k & 7][1]); serial_puts("\n");
        }
        if (proc) {
            serial_puts("[PF]   stack 0x"); serial_puthex(proc->stack_start); serial_puts("..0x"); serial_puthex(proc->stack_end);
            serial_puts(" heap 0x"); serial_puthex(proc->heap_start); serial_puts("..0x"); serial_puthex(proc->heap_end); serial_puts("\n");
        }
        if (proc) {
            process_exit(proc, -1);
            return;
        }
    }

    {
        /* Fatal-path diagnostics: which thread was running, and where its own
         * kernel stack is - so a fault whose address lies nowhere near that
         * stack (a corrupted RSP) can be told apart from a genuine overflow. */
        uint64_t rsp_now;
        __asm__ volatile("mov %%rsp, %0" : "=r"(rsp_now));
        thread_t* th = thread_get_current();
        serial_puts("[PF] fatal: rsp_in_handler=0x"); serial_puthex(rsp_now);
        if (th) {
            serial_puts(" thread pid="); serial_putdec(th->pid);
            serial_puts(" kstack_top=0x"); serial_puthex(th->kernel_stack);
            serial_puts(" kstack_size=0x"); serial_puthex(th->kernel_stack_size);
            serial_puts(" saved_ctx.rsp=0x"); serial_puthex(th->context.rsp);
            serial_puts(" state="); serial_putdec((uint64_t)th->state);
        }
        serial_puts("\n");
    }
    if (page_fault_regs) {
        const struct regs *pr = page_fault_regs;
        serial_puts("[PF]   rip=0x"); serial_puthex(pr->rip);
        serial_puts(" rax=0x"); serial_puthex(pr->rax); serial_puts(" rbx=0x"); serial_puthex(pr->rbx);
        serial_puts(" rcx=0x"); serial_puthex(pr->rcx); serial_puts(" rdx=0x"); serial_puthex(pr->rdx);
        serial_puts(" rsi=0x"); serial_puthex(pr->rsi); serial_puts(" rdi=0x"); serial_puthex(pr->rdi);
        serial_puts(" rbp=0x"); serial_puthex(pr->rbp); serial_puts(" rsp=0x"); serial_puthex(pr->rsp); serial_puts("\n");
        /* words on the faulting stack: return addresses show the call chain */
        const uint64_t *sp = (const uint64_t *)pr->rsp;
        for (int k = 0; k < 48; ++k) {
            uint64_t v = sp[k];
            if (v >= 0x100000 && v < 0x4ca000) { serial_puts("[PF]   stack+"); serial_putdec((uint64_t)k * 8); serial_puts(" text 0x"); serial_puthex(v); serial_puts("\n"); }
        }
    }
    serial_puts("[PF] Unhandled kernel page fault - halting\n");
    __asm__ volatile("cli");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

page_fault_stats_t* page_fault_get_stats(void) {
    return &pf_stats;
}

void page_fault_reset_stats(void) {
    memset(&pf_stats, 0, sizeof(page_fault_stats_t));
}

bool pf_is_valid_user_addr(uint64_t addr) {
    return (addr < 0x000000800000ULL) && (addr >= 0x0000001000ULL);
}

bool pf_is_kernel_heap_addr(uint64_t addr) {
    (void)addr;
    return false;
}

void page_fault_init(void) {
    serial_puts("[PF] Initializing page fault handler\n");
    page_fault_reset_stats();
    serial_puts("[PF] Page fault handler initialized\n");
}
