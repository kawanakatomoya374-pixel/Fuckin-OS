/*
 * x86-64 freestanding setjmp/longjmp reference implementation.
 *
 * Why this file exists
 * --------------------
 * The runtime implements Python exceptions with setjmp/longjmp, and every
 * generated for-loop relies on StopIteration being catchable. In
 * PYTHON_CODE_TO_C_NO_STDLIB mode the runtime declares its own jmp_buf
 * (include/runtime/python_code_to_c_runtime.h):
 *
 *     typedef struct { void *buf[16]; } jmp_buf[1];   // 128 bytes
 *
 * A hobby OS must provide setjmp/longjmp for *that* layout (libc headers are
 * not included, so the libc jmp_buf type is not visible and must not be used).
 * This file is a compact, dependency-free implementation of the contract for
 * x86-64 System V, used by the Alpha0.6 embedded test-suite and intended as a
 * starting point for kernels:
 *
 *   buf[0] = rip   (resume address)
 *   buf[1] = rsp   (stack pointer after this call returns)
 *   buf[2] = rbp
 *   buf[3] = rbx
 *   buf[4..7] = r12, r13, r14, r15
 *
 * On other architectures build with your own assembly instead (or keep the
 * linear-heap/hosted configuration, which uses the platform setjmp).
 */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__) || defined(__TINYC__))

__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "    movq (%rsp), %rdx\n"      /* return address             */
    "    movq %rdx, 0(%rdi)\n"
    "    leaq 8(%rsp), %rdx\n"     /* stack pointer after ret    */
    "    movq %rdx, 8(%rdi)\n"
    "    movq %rbp, 16(%rdi)\n"
    "    movq %rbx, 24(%rdi)\n"
    "    movq %r12, 32(%rdi)\n"
    "    movq %r13, 40(%rdi)\n"
    "    movq %r14, 48(%rdi)\n"
    "    movq %r15, 56(%rdi)\n"
    "    xorl %eax, %eax\n"
    "    ret\n"
    ".size setjmp,.-setjmp\n"
    "\n"
    ".globl longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "    movq 0(%rdi), %rdx\n"
    "    movq 8(%rdi), %rsp\n"
    "    movq 16(%rdi), %rbp\n"
    "    movq 24(%rdi), %rbx\n"
    "    movq 32(%rdi), %r12\n"
    "    movq 40(%rdi), %r13\n"
    "    movq 48(%rdi), %r14\n"
    "    movq 56(%rdi), %r15\n"
    "    movl %esi, %eax\n"
    "    testl %eax, %eax\n"
    "    jne 1f\n"
    "    movl $1, %eax\n"          /* longjmp(env, 0) behaves as 1 */
    "1:\n"
    "    jmp *%rdx\n"
    ".size longjmp,.-longjmp\n"
);

#else

#error "examples/embed/x86_64_setjmp.c only implements the x86-64 System V ABI; provide your own setjmp/longjmp for this target."

#endif
