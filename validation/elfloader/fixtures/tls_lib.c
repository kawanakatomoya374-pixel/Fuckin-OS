/* Fixture: a shared object with thread-local storage, exported symbols
 * and an ifunc. Exercises PT_TLS, DTPMOD64/DTPOFF64/TPOFF64,
 * R_X86_64_IRELATIVE and DT_GNU_HASH all at once. */
__thread int   tls_zero;            /* .tbss: must initialise to 0    */
__thread int   tls_init = 0x5A5A;   /* .tdata: must carry its value   */
__thread long  tls_big[4] = { 1, 2, 3, 4 };

int lib_global = 0x1234;
const char lib_name[] = "COS_TLS_LIB";

int lib_add(int a, int b) { return a + b; }
int *lib_tls_addr(void) { return &tls_init; }

/* An ifunc: the address of `fast_or_slow` is decided at load time by
 * calling the resolver. The kernel must NOT call this - it is ring3
 * code - so it should come back as deferred work. */
static int impl_a(void) { return 1; }
static void *resolve_impl(void) { return (void *)impl_a; }
int fast_or_slow(void) __attribute__((ifunc("resolve_impl")));

/* Referencing the ifunc through a function pointer is what forces the
 * linker to emit R_X86_64_IRELATIVE - an ifunc that is merely DEFINED
 * and never referenced produces no relocation at all. */
int (*lib_ifunc_ptr)(void) = fast_or_slow;
int lib_call_ifunc(void) { return lib_ifunc_ptr(); }
