/* rt.c - loaded and unloaded at run time by the tests (kmod load / kmod unload).
 *   0x1003 -> 4242
 *   0x1004 (user pointer) -> writes "rt-ok" through the checked copy and returns its result (0, or -1 for a pointer that is not user memory)
 *   0x1005 -> allocates 100 bytes with cosys_alloc, uses them, and returns 100 (or -1) - the block is freed by the kernel when the module unloads */
#include "cosys.h"
static int64_t rt_call(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; return 4242; }
static int64_t rt_copy(uint64_t a, uint64_t b, uint64_t c) { (void)b; (void)c; return cosys_copy_to_user(a, "rt-ok", 6); }
static int64_t rt_alloc(uint64_t a, uint64_t b, uint64_t c) {
    (void)a; (void)b; (void)c; unsigned char *p = cosys_alloc(100); if (!p) return -1;
    for (int i = 0; i < 100; i++) p[i] = (unsigned char)i;
    return p[99] == 99 ? 100 : -1;
}
static int rt_init(void) { return cosys_register_syscall(0x1003, rt_call, 0) || cosys_register_syscall(0x1004, rt_copy, 0) || cosys_register_syscall(0x1005, rt_alloc, 0); }
static void rt_exit(void) { cosys_unregister_syscall(0x1003); cosys_unregister_syscall(0x1004); cosys_unregister_syscall(0x1005); }
COSYS_MODULE("rt", "1.0", COSYS_LEVEL_SERVICE, rt_init, rt_exit);
