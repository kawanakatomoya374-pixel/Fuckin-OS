/*
 * cos_crt0.S - C runtime startup for .c-os programs.
 *
 * The kernel enters a .c-os program at its ELF entry point with a full
 * System V initial stack already built by cos_elf_setup_stack_ex()
 * (src/kernel/cos_elf.c):
 *
 *   RSP -> [ argc ][ argv[0..argc-1] ][ NULL ]
 *          [ envp[0..envc-1] ][ NULL ]
 *          [ auxv: (tag, value) pairs ][ AT_NULL, 0 ]
 *
 * WHAT CHANGED, AND ONE THING THAT WAS ALREADY WRONG
 * --------------------------------------------------
 * This file used to read argc/argv straight off the stack and
 * `call main`. That matched the stack the kernel built at the time -
 * argc, argv and two NULLs - and is no longer enough: the stack now also
 * carries the environment and an auxiliary vector, and the auxiliary
 * vector is how a program finds the link map describing its loaded
 * objects, its ifunc relocations and its constructors.
 *
 * Separately, the previous version was written in NASM syntax in a file
 * named `.S`. By convention - and by what every compiler driver actually
 * does - a capital-S file is preprocessed and handed to GAS, so
 * `gcc cos_crt0.S` could never have assembled it; it was buildable only
 * by invoking nasm by hand, which nothing in the Makefile did. It is GAS
 * syntax now, so the ordinary driver builds it and tools/cos-cc needs no
 * assembler beyond the compiler itself.
 *
 * Rather than parse the auxiliary vector in assembly, this stub does the
 * one thing that genuinely needs assembly - capture RSP before anything
 * perturbs it - and hands it to __cos_rt_start() in cos_rtld.c.
 *
 * The System V ABI requires RSP to be 16-byte aligned immediately before
 * a `call`. The kernel guarantees that at entry, so the `and` below is a
 * no-op on a correct entry and a rescue on a malformed one: one
 * instruction to remove a class of "works until the first SSE spill"
 * failure.
 */

    .text
    .globl _start
    .type  _start, @function
_start:
    /* Clear the frame pointer so a debugger or unwinder walking the
     * chain stops here rather than following whatever the kernel left
     * in RBP. */
    xorq    %rbp, %rbp

    movq    %rsp, %rdi          /* the whole initial stack block */
    andq    $-16, %rsp          /* guarantee the ABI's alignment  */

    call    __cos_rt_start

    /* __cos_rt_start() ends in cos_exit() and does not return. The halt
     * loop exists so a bug making it return stops here instead of
     * executing whatever bytes follow. */
1:  hlt
    jmp     1b
    .size _start, . - _start

/*
 * Mark the stack non-executable. Not merely cosmetic here: the loader
 * reads PT_GNU_STACK and reports an executable-stack request, and on a
 * CPU where cos_elf_nx_enable() succeeded it enforces the difference
 * with the hardware NX bit. Being explicit keeps the program's declared
 * requirements consistent with what the kernel will actually grant it.
 */
    .section .note.GNU-stack,"",@progbits
