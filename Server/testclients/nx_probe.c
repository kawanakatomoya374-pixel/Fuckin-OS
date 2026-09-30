/**
 * nx_probe.c - deliberately tries to execute code from heap memory, to
 * PROVE the NX fix in task_handle_page_fault() (task.c) actually works,
 * rather than merely observing that nothing else broke.
 *
 * Writes a single x86-64 `ret` instruction (0xC3) into a heap buffer,
 * then calls it as a function. Before this session's fix, heap pages
 * carried no NX bit at all, so this call would have executed the `ret`
 * successfully and the program would print "UNEXPECTED: call succeeded"
 * and exit 1 - a clean, silent proof that code execution from writable
 * heap memory was possible. With the fix, the CPU's own execute-disable
 * hardware refuses the fetch, the process takes a page fault, and -
 * matching this codebase's existing ring3 crash-isolation policy - is
 * terminated by the kernel rather than being allowed to continue. From
 * this program's own point of view that looks like never returning from
 * the call at all: the "UNEXPECTED" line is never reached, and the
 * process simply does not survive to print anything further. The kernel
 * serial log (not this program's own output) is therefore where the
 * actual pass/fail signal comes from - see the boot-time spawn hook that
 * launches this for exactly what it checks for.
 */
#include "cos.h"

int main(void)
{
    cos_puts("[nx_probe] starting\n");

    volatile uint8_t *buf = (volatile uint8_t *)cos_mmap(4096, COS_PROT_READ | COS_PROT_WRITE);
    if (!buf) {
        cos_puts("[nx_probe] mmap failed\n");
        return 1;
    }

    /* 0xC3 = `ret`. The smallest possible "prove you executed me"
     * instruction - it does not need anywhere to jump back to behave
     * correctly if actually reached, unlike almost anything else. */
    buf[0] = 0xC3;

    cos_puts("[nx_probe] about to call into heap memory - if NX is "
             "working this line is the last one this process ever "
             "logs\n");

    void (*heap_fn)(void) = (void (*)(void))(uintptr_t)buf;
    heap_fn();

    /* Reached only if NX failed to stop the call above. */
    cos_puts("[nx_probe] UNEXPECTED: call into heap memory succeeded - "
             "NX is NOT being enforced\n");
    return 1;
}
