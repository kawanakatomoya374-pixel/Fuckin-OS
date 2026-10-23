/* hello.c - the smallest C-OS kernel module: it provides system call 0x1000, which returns the sum of its three arguments plus 1000.
 * Build: tools/cos-sys -o hello.c-osys hello.c      (see docs/c-osys.md) */
#include "cosys.h"

static uint32_t g_calls;                                   /* writable data: the module has its own .data/.bss */
static int64_t priv_call(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; return 7; }       /* needs the file-write capability */
static int64_t hello_call(uint64_t a, uint64_t b, uint64_t c) { g_calls++; return (int64_t)(a + b + c) + 1000; }
static int hello_init(void) {
    cosys_log("hello from a kernel module");
    if (cosys_register_syscall(0x1001, priv_call, 0x2 /* COS_CAP_FS_WRITE */) != 0) return -1;      /* a sandboxed program without that capability is turned away */
    return cosys_register_syscall(0x1000, hello_call, 0);  /* caps 0: any program, also a sandboxed one */
}
static void hello_exit(void) { cosys_unregister_syscall(0x1001); cosys_unregister_syscall(0x1000); cosys_log("goodbye"); }
COSYS_MODULE("hello", "2.0", COSYS_LEVEL_SERVICE, hello_init, hello_exit);
