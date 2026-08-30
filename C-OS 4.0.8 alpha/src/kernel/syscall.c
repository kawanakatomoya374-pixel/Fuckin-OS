/**
 * syscall.c - Real ring3 -> ring0 syscall entry point (int 0x80)
 *
 * This is the piece that was missing to make C-OS's existing ring3
 * scaffolding (GDT user segments, TSS.rsp0 switching on every context
 * switch, per-process page directories, PAGE_USER page mapping) into an
 * actually-usable userspace: without a syscall gate, a ring3 thread had
 * no way to ask the kernel for anything and no clean way to end itself.
 *
 * Convention (deliberately tiny - just enough for a real, verifiable
 * ring3 milestone, not a full ABI):
 *   rax = syscall number
 *   rdi, rsi = arg1, arg2
 *   SYS_WRITE (0): rdi = pointer (in the *caller's* mapped address space,
 *                  which is what's active when this handler runs, since
 *                  interrupts don't change CR3), rsi = length. Writes to
 *                  the serial console.
 *   SYS_EXIT  (1): rdi = exit code. Terminates the calling thread/process.
 */
#include "idt.h"
#include "serial.h"
#include "task.h"
#include "scheduler.h"

#define SYS_WRITE 0
#define SYS_EXIT  1

/* Bound how much a single SYS_WRITE can dump, so a buggy or hostile user
 * program can't wedge the kernel into an unbounded serial write loop. */
#define SYSCALL_WRITE_MAX 4096

static void syscall_handler(struct regs* r) {
    switch (r->rax) {
        case SYS_WRITE: {
            const char* buf = (const char*)(uintptr_t)r->rdi;
            uint64_t len = r->rsi;
            if (!buf) { r->rax = (uint64_t)-1; break; }
            if (len > SYSCALL_WRITE_MAX) len = SYSCALL_WRITE_MAX;
            for (uint64_t i = 0; i < len && buf[i]; ++i) {
                serial_putc(buf[i]);
            }
            r->rax = len;
            break;
        }
        case SYS_EXIT: {
            int code = (int)r->rdi;
            serial_puts("[SYSCALL] ring3 thread exiting via SYS_EXIT, code=");
            serial_putdec((uint64_t)(int64_t)code);
            serial_puts("\n");
            thread_exit(scheduler_get_current_thread(), code);
            /* thread_exit() does not return for the current thread - it
             * preempts into another task. If we ever do get here, the
             * process/thread bookkeeping is in an unexpected state; fail
             * safe rather than falling through to arbitrary ring3 code. */
            for (;;) { __asm__ volatile("hlt"); }
        }
        default:
            serial_puts("[SYSCALL] unknown syscall number=");
            serial_putdec(r->rax);
            serial_puts("\n");
            r->rax = (uint64_t)-1;
            break;
    }
}

void syscall_init(void) {
    register_interrupt_handler(128, syscall_handler);
    serial_puts("[SYSCALL] int 0x80 syscall gate registered (SYS_WRITE, SYS_EXIT)\n");
}
