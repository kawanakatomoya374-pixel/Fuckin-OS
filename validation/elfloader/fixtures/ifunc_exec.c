/* Fixture: R_X86_64_IRELATIVE. The resolver runs at LOAD time and its
 * return value is the function's real address - which is why a kernel
 * must not run it: it is ring3 code chosen by the file. */
static long impl_fast(void) { return 0xFA57; }
static void *resolve(void) { return (void *)impl_fast; }
long dispatched(void) __attribute__((ifunc("resolve")));

__attribute__((used)) long (*g_fp)(void) = dispatched;
void _start(void) { g_fp(); for (;;) { } }
