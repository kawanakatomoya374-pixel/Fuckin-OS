/* kuser.c - a module that needs another module: system call 0x1002 returns kbase_add(a, b) * 10. Link with: cos-sys -l kbase.so */
#include "cosys.h"
COSYS_EXPORT int kbase_add(int a, int b);                  /* resolved from the kbase module at load time */
static int64_t kuser_call(uint64_t a, uint64_t b, uint64_t c) { (void)c; return (int64_t)kbase_add((int)a, (int)b) * 10; }
static int kuser_init(void) { return cosys_register_syscall(0x1002, kuser_call, 0); }
static void kuser_exit(void) { cosys_unregister_syscall(0x1002); }
COSYS_MODULE("kuser", "1.0", COSYS_LEVEL_SERVICE, kuser_init, kuser_exit);
