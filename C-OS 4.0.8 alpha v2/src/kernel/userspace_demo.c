#include <string.h>
#include "memory.h"
/**
 * userspace_demo.c - Real ring3 usermode demonstration process
 *
 * This exercises the ring3 execution path end-to-end: a genuinely
 * separate process (its own page directory, its own user stack) running
 * real machine code at CPL3, which calls back into the kernel purely
 * through the int 0x80 syscall gate (see syscall.c) - not by jumping
 * into kernel functions directly, since ring3 code cannot do that.
 *
 * The machine code below is not hand-encoded from memory: it was
 * assembled with nasm from this source (kept here for anyone who wants
 * to regenerate or modify it):
 *
 *   BITS 64
 *   ORG 0x40000000
 *   start:
 *       mov rax, 0              ; SYS_WRITE
 *       mov rdi, msg
 *       mov rsi, msg_len
 *       int 0x80
 *       mov rax, 1              ; SYS_EXIT
 *       mov rdi, 0              ; exit code 0
 *       int 0x80
 *   .hang:
 *       jmp .hang
 *   msg: db "Hello from real Ring3 userspace!", 10, 0
 *   msg_len equ $ - msg - 1
 */
#include "task.h"
#include "serial.h"
#include "types.h"
#include "mm/paging.h"
#include "cos_elf.h"
#include "cos_elf_link.h"
#include "scheduler.h"
#include "cos_hello_elf.h"
#include "cos_paint_elf.h"
#include "cos_crash_elf.h"
#include "cos_argv_elf.h"
#include "cos_memfile_elf.h"
#include "cos_spawner_elf.h"
#include "cos_waiter_elf.h"
#include "cos_childexit_elf.h"
#include "cos_libuser_elf.h"
#include "cos_libmath_so.h"
#include "cos_untouched_elf.h"
#include "cos_fdtest_elf.h"
#include "cos_sigreceiver_elf.h"
#include "cos_sigsender_elf.h"
#include "cos_sigkilltest_elf.h"
#include "cos_libbase_so.h"
#include "cos_libdepend_so.h"
#include "cos_extsymtest_elf.h"
#include "cos_extsymmissing_elf.h"
#include "cos_mmaptest_elf.h"
#include "cos_mmapwritefault_elf.h"
#include "cos_childfast_elf.h"
#include "cos_childslow_elf.h"
#include "cos_multiwaiter_elf.h"
#include "cos_signest_elf.h"
#include "cos_test_app_elf.h"
#include "cos_modtest_elf.h"
#include "cos_ctest_elf.h"
#include "cos_deptest_elf.h"
#include "cos_advtest_elf.h"
#include "cos_libtest_elf.h"
#include "cos_libbase2_so.h"
#include "cos_libdepend2_so.h"
#include "gui.h"

/* 0x10000000 is covered by the kernel's low identity map and cannot be
 * remapped in a copied user PML4. Use a free address in the user half. */
/* User programs live in PML4[1..255], the per-process private region.
 * 0x40000000 is inside PML4[0], which is the SHARED kernel identity map -
 * loading user code there gave every process the same lower page tables
 * (see paging_create_directory() for the full explanation). */
#define USER_DEMO_CODE_BASE 0x0000008000000000ULL

/* Ring3 syscall-boundary security test.
 *
 * Assembled with nasm from validation/ring3_sec.asm (kept in the tree so
 * this can be regenerated rather than trusted as opaque bytes). It runs at
 * CPL3 and deliberately attacks the int 0x80 boundary:
 *   1. a legitimate SYS_WRITE (must succeed)
 *   2. SYS_WRITE pointed at a higher-half KERNEL address
 *   3. SYS_WRITE pointed at an unmapped user address
 *   4. SYS_WRITE with a length that overflows past the top of the address
 *      space
 * and prints RING3SEC_PASS only if the kernel rejected 2 and 3 with -1.
 *
 * Before paging_user_range_ok() existed, case 2 made the kernel read
 * kernel memory out to the serial console and case 3 faulted inside an
 * interrupt handler - so this is a regression test for a real hole, not a
 * hypothetical one. */
static const uint8_t g_user_demo_code[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0xbf, 0xa9, 0x00, 0x00, 0x00, 0x80,
    0x00, 0x00, 0x00, 0xbe, 0x1b, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x49, 0x89,
    0xc4, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0xbf, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x80, 0xff, 0xff, 0xbe, 0x40, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x49,
    0x89, 0xc5, 0xb8, 0x00, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0x30,
    0xbe, 0x20, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x49, 0x89, 0xc6, 0xb8, 0x00,
    0x00, 0x00, 0x00, 0x48, 0xbf, 0xa9, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x48, 0xc7, 0xc6, 0xf0, 0xff, 0xff, 0xff, 0xcd, 0x80, 0x49, 0x89,
    0xc7, 0x49, 0x83, 0xfd, 0xff, 0x75, 0x1e, 0x49, 0x83, 0xfe, 0xff, 0x75,
    0x18, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0xbf, 0xc4, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0xbe, 0x38, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb,
    0x16, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0xbf, 0xfc, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0xbe, 0x36, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8,
    0x01, 0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb,
    0xfe, 0x52, 0x49, 0x4e, 0x47, 0x33, 0x53, 0x45, 0x43, 0x20, 0x62, 0x61,
    0x73, 0x65, 0x6c, 0x69, 0x6e, 0x65, 0x20, 0x77, 0x72, 0x69, 0x74, 0x65,
    0x20, 0x6f, 0x6b, 0x0a, 0x52, 0x49, 0x4e, 0x47, 0x33, 0x53, 0x45, 0x43,
    0x5f, 0x50, 0x41, 0x53, 0x53, 0x20, 0x6b, 0x65, 0x72, 0x6e, 0x65, 0x6c,
    0x20, 0x72, 0x65, 0x6a, 0x65, 0x63, 0x74, 0x65, 0x64, 0x20, 0x61, 0x6c,
    0x6c, 0x20, 0x69, 0x6e, 0x76, 0x61, 0x6c, 0x69, 0x64, 0x20, 0x75, 0x73,
    0x65, 0x72, 0x20, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x73, 0x0a,
    0x52, 0x49, 0x4e, 0x47, 0x33, 0x53, 0x45, 0x43, 0x5f, 0x46, 0x41, 0x49,
    0x4c, 0x20, 0x6b, 0x65, 0x72, 0x6e, 0x65, 0x6c, 0x20, 0x61, 0x63, 0x63,
    0x65, 0x70, 0x74, 0x65, 0x64, 0x20, 0x61, 0x6e, 0x20, 0x69, 0x6e, 0x76,
    0x61, 0x6c, 0x69, 0x64, 0x20, 0x75, 0x73, 0x65, 0x72, 0x20, 0x70, 0x6f,
    0x69, 0x6e, 0x74, 0x65, 0x72, 0x0a,
};

void spawn_ring3_demo_process(void) {
    serial_puts("[USERSPACE] Spawning real ring3 (CPL3) demo process...\n");

    process_t* proc = process_create("ring3-demo", TASK_TYPE_USER);
    if (!proc) {
        serial_puts("[USERSPACE] FAILED: process_create() returned NULL\n");
        return;
    }

    if (!paging_setup_user_code((page_directory_t*)proc->page_dir, USER_DEMO_CODE_BASE,
                                 g_user_demo_code, sizeof(g_user_demo_code))) {
        serial_puts("[USERSPACE] FAILED: could not map user code page\n");
        return;
    }

    thread_t* th = thread_create(proc, (void*)USER_DEMO_CODE_BASE, NULL);
    if (!th) {
        serial_puts("[USERSPACE] FAILED: thread_create() returned NULL\n");
        return;
    }

    serial_puts("[USERSPACE] Ring3 demo process created (pid=");
    serial_putdec((uint64_t)proc->pid);
    serial_puts("), entry=0x");
    serial_puthex(USER_DEMO_CODE_BASE);
    serial_puts(" - running the int 0x80 syscall-boundary security test;"
                " expect RING3SEC_PASS below.\n");
}

/**
 * Loads and runs the embedded ".c-os" ELF executable through the real
 * loader, as opposed to the raw flat blob used by the syscall security
 * test above.
 *
 * The test program is built by the ordinary toolchain (nasm + ld with
 * validation/cos_programs/cos.ld) into three PT_LOAD segments with
 * genuinely different permissions - R+X text, read-only rodata, and R+W
 * data followed by .bss - so this exercises multi-segment loading,
 * per-segment permission handling, and .bss zero-fill rather than a
 * single flat RWX blob that would prove none of those.
 */
/* Shared launcher: loads a .c-os image into a fresh process and starts it.
 * Used by the boot-time demos and by the file manager's double-click
 * handler, so there is exactly one code path that turns an image into a
 * running ring3 process. */
/* Returns the new process's pid, or -1 on failure. Returning the pid
 * rather than a bool is what lets SYS_SPAWN report a usable handle to the
 * calling program; every existing caller simply treats <0 as failure. */
/* Declared before use rather than relying on an implicit declaration:
 * the two disagree about the return type (int vs int64_t), which on
 * x86-64 silently truncates a pid to 32 bits. Prototyped in
 * src/include/task.h for other callers. */
int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image,
                                  unsigned int image_len,
                                  const char *const *argv, int argc,
                                  const char *const *envp, int envc);

int64_t cos_launch_elf_image(const char *name, const unsigned char *image,
                             unsigned int image_len)
{
    return cos_launch_elf_image_args(name, image, image_len, NULL, 0, NULL, 0);
}

/* The real launcher. Everything that turns an image into a running ring3
 * process goes through here, so every loader validation applies
 * identically no matter whether a program was started by the file
 * manager, by boot, or by another program via SYS_SPAWN.
 *
 * TWO REAL BUGS THIS FIXES
 * ------------------------
 * 1. The previous version called cos_elf_load(proc->page_dir, ...)
 *    without ever making that directory active. Every loader write
 *    resolves through paging_virt_to_phys(), which walks the ACTIVE
 *    tables - and paging_create_directory() explicitly zeroes PML4
 *    entries 1..255 on a new directory, so a freshly created process
 *    cannot already be active and cannot inherit the mapping either.
 *    cos_elf.h documented the requirement; this was the one caller in
 *    the tree that did not honour it, while every other cross-address-
 *    space path (paging_setup_user_stack, the fork path in task.c) does
 *    the save/switch/restore dance. cos_link_load_executable() now does
 *    it, once and correctly.
 *
 * 2. thread_create() was called BEFORE the stack was built, and its
 *    context.rsp then patched afterwards. That works only because
 *    nothing can schedule the new thread in between - a race that held
 *    by luck rather than by design. The thread is now created last,
 *    with everything it needs already in place.
 */
int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image,
                                  unsigned int image_len,
                                  const char *const *argv, int argc,
                                  const char *const *envp, int envc)
{
    process_t* proc = process_create(name, TASK_TYPE_USER);
    if (!proc) {
        serial_puts("[USERSPACE] FAILED: process_create() returned NULL\n");
        return -1;
    }

    /* argv[0] defaults to the program's own name, which is what a
     * program reads to identify itself. */
    const char *default_argv[1] = { name };
    if (!argv || argc <= 0) { argv = default_argv; argc = 1; }

    cos_link_result_t res;
    if (!cos_link_load_executable(proc, name, image, (uint64_t)image_len,
                                  argv, argc, envp, envc, &res)) {
        /* The loader's contract is explicit: on failure the caller must
         * discard the address space, because segments may have been
         * partially mapped. The ORIGINAL code did not, so every rejected
         * image - a corrupt file, a wrong architecture, a file that
         * simply is not a program - leaked a process_t, its page
         * directory and any mapped pages, permanently. */
        serial_puts("[USERSPACE] FAILED: image rejected by the loader/linker\n");
        process_destroy(proc);
        return -1;
    }

    thread_t* th = thread_create(proc, (void*)(uintptr_t)res.entry, NULL);
    if (!th) {
        serial_puts("[USERSPACE] FAILED: thread_create() returned NULL\n");
        process_destroy(proc);
        return -1;
    }
    th->context.rsp = res.rsp;

    /* Install the thread pointer the linker built. Without this every
     * `__thread` access in the program reads from whatever %fs base the
     * previously running thread left behind. */
    if (res.tp) scheduler_set_thread_tls(th, res.tp);

    serial_puts("[USERSPACE] launched ");
    serial_puts(name);
    serial_puts(" pid=");
    serial_putdec((uint64_t)proc->pid);
    serial_puts(" entry=0x");
    serial_puthex(res.entry);
    serial_puts("\n");
    return (int64_t)proc->pid;
}

/* Launches the bundled GUI demo - the one a double-click on
 * paint.c-os in the file manager runs. */
/* Validation hook: open the file manager from the owner loop so storage
 * I/O happens under normal GUI interaction, which is the scenario where
 * the desktop was reported to freeze. */
void cos_validation_open_file_manager(void) {
    extern window_t* gui_open_window(int kind, const char* title, int x, int y, int w, int h);
    /* Matches the REAL user-facing default (gui_menus.c's Start-menu/
     * desktop-icon open path), not an arbitrary validation-only size -
     * the file manager should be tested at the size users actually get
     * by default, with narrower sizes (a user's own resize) covered
     * separately by the responsive column-collapse behaviour itself. */
    (void)gui_open_window(WIN_FILE_MGR, "Files", 72, 56, 1120, 760);
}

/* Exercises the window-close/minimize/maximize save path directly, so the
 * reported "moving windows / opening apps freezes everything" symptom can
 * be measured against a real, discrete user action rather than only
 * against the boot-time catalog load this session already fixed. */
void cos_validation_close_file_manager(void) {
    extern int gui_find_window(int kind);
    extern void gui_close_window(int idx);
    int idx = gui_find_window(WIN_FILE_MGR);
    if (idx >= 0) {
        serial_puts("[VALIDATION] closing file manager to measure the save path\n");
        gui_close_window(idx);
    }
}

void spawn_cos_paint_process(void) {
    serial_puts("[USERSPACE] Loading .c-os GUI program (black/white window)...\n");
    (void)cos_launch_elf_image("paint.c-os", cos_paint_elf, cos_paint_elf_len);
}

void spawn_cos_elf_process(void) {
    /* Routed through the shared launcher rather than duplicating it.
     * The open-coded copy this replaces had the same missing
     * paging_switch_directory() as the launcher did, plus two leaks its
     * own error paths never cleaned up (a rejected image and a failed
     * thread_create both returned with the process still allocated). One
     * launcher means one place for those to be right. */
    serial_puts("[USERSPACE] Loading embedded .c-os ELF executable "
                "- expect COSELF_PASS below.\n");
    (void)cos_launch_elf_image("hello.c-os", cos_hello_elf, cos_hello_elf_len);
}

/* Deliberately crashes a ring3 process (invalid opcode) to prove the
 * kernel survives it and keeps running everything else - the fix for
 * "any bug in any .c-os program used to halt the entire kernel". A
 * well-behaved process is launched immediately after, on the SAME boot,
 * so survival is proven by that second process actually running to
 * completion rather than merely by the serial log not going silent. */
void spawn_cos_crash_test(void) {
    serial_puts("[USERSPACE] Loading deliberately-crashing .c-os program...\n");
    (void)cos_launch_elf_image("crash.c-os", cos_crash_elf, cos_crash_elf_len);
}

/* Validation: seeds a real .c-os file onto the FAT32 volume and then
 * simulates a double-click open through the SAME dispatch path the file
 * manager uses (gui_open_file_in_app), rather than calling
 * cos_launch_elf_image() directly - this is specifically to prove the
 * disk-read path (fs_find/fs_read_file_at) works, since that is the part
 * that was missing and the part an earlier draft of this feature got
 * wrong (read from the wrong storage backend, see the comment in
 * gui_apps_common.c). */
void cos_validation_disk_launch(void) {
    extern void fs_init(void);
    extern bool fs_write_file_at(const char* path, const char* name,
                                 const char* data, uint64_t size);
    extern void gui_open_file_in_app(const char* path, int file_type);

    bool wrote = fs_write_file_at("/", "diskhello.c-os",
                                  (const char*)cos_hello_elf, cos_hello_elf_len);
    serial_puts("[VALIDATION] seeded /diskhello.c-os on FAT32: ");
    serial_puts(wrote ? "ok" : "FAILED");
    serial_puts("\n");
    if (!wrote) return;

    /* fs_find() queries an in-RAM directory cache (g_dir_entries in fs.c)
     * that is only populated by fs_list_dir() - writing a file through
     * FatFs does not update it automatically. In real usage this is never
     * an issue: a user cannot double-click a file without first seeing it
     * in a folder view, and seeing it means fs_list_dir() already ran.
     * This validation hook skips that step (it writes and immediately
     * tries to open, with no GUI navigation in between), so it has to do
     * explicitly what navigating to "/" in the file manager would have
     * done implicitly - confirmed by first observing this exact failure
     * ("Not an executable file", i.e. fs_find() returned NULL) with this
     * line absent. */
    extern void* fs_list_dir(const char* path);
    (void)fs_list_dir("/");

    serial_puts("[VALIDATION] simulating double-click open of /diskhello.c-os\n");
    gui_open_file_in_app("/diskhello.c-os", 0);
}

/* Verifies the System V initial stack: argc, a readable argv[0] matching
 * the program's name, and a NULL argv[1] terminator. */
void spawn_cos_argv_test(void) {
    serial_puts("[USERSPACE] Loading argv ABI test .c-os program...\n");
    (void)cos_launch_elf_image("argvtest.c-os", cos_argv_elf, cos_argv_elf_len);
}

/* Verifies the memory and file syscalls end to end from ring3: grow the
 * heap with sbrk, actually touch the new pages (proving demand paging
 * maps them), write a file, read it back INTO the freshly-allocated heap
 * memory, and compare contents. Reading into heap memory specifically
 * exercises paging_user_range_ok() against demand-paged pages, not just
 * the statically-mapped image. */
void spawn_cos_memfile_test(void) {
    serial_puts("[USERSPACE] Loading heap+file syscall test .c-os program...\n");
    (void)cos_launch_elf_image("memfile.c-os", cos_memfile_elf, cos_memfile_elf_len);
}

/**
 * cos_spawn_elf_path - load a .c-os file from disk and start it as a new
 * process. Returns the new pid, or -1.
 *
 * Backs SYS_SPAWN. Kept next to the other launch paths so there is one
 * place that turns "a path" into "a running process", and so every
 * loader validation applies identically no matter whether a program was
 * started by the file manager, by boot, or by another program.
 *
 * Reads with cos_fs_read_file() (the FatFs path) rather than
 * fs_read_file_at(): that returns a pointer into a single shared buffer
 * which the next fs_read_* call would invalidate, and a spawning program
 * has no control over what else runs in between.
 */
int64_t cos_spawn_elf_path(const char *path)
{
    /* argv[0] defaults to the path, which is what a program reads to
     * identify itself. */
    const char *argv[1] = { path };
    return cos_spawn_elf_path_args(path, argv, 1, NULL, 0);
}

int64_t cos_spawn_elf_path_args(const char *path, const char *const *argv,
                                int argc, const char *const *envp, int envc)
{
    if (!path || !path[0]) return -1;

    extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);

    /* Cap the read independently of the loader's own internal caps: this
     * bounds the kernel allocation made before the loader has parsed
     * anything at all. */
    const uint64_t max_image = 8u * 1024u * 1024u;
    uint8_t *buffer = (uint8_t *)kmalloc((size_t)max_image);
    if (!buffer) return -1;

    int got = cos_fs_read_file(path, buffer, max_image);
    if (got <= 0) {
        kfree(buffer);
        serial_puts("[SPAWN] failed to read image: ");
        serial_puts(path);
        serial_puts("\n");
        return -1;
    }

    const char *base = path;
    for (const char *p = path; *p; ++p) {
        if (*p == '/') base = p + 1;
    }

    int64_t pid = cos_launch_elf_image_args(base, buffer, (unsigned int)got,
                                            argv, argc, envp, envc);
    kfree(buffer);
    return pid;
}

/* Verifies getpid/yield/sleep/spawn. Must run after the disk-launch
 * validation, because it spawns /diskhello.c-os - proving a ring3 program
 * can start another program that lives on the filesystem, not just one
 * the kernel had embedded. */
void spawn_cos_spawner_test(void) {
    serial_puts("[USERSPACE] Loading spawn/sched syscall test .c-os program...\n");
    (void)cos_launch_elf_image("spawner.c-os", cos_spawner_elf, cos_spawner_elf_len);
}

/* waitpid test. The child is seeded onto the FAT32 volume first because
 * SYS_SPAWN takes a path - the parent spawns it by name, exactly as a
 * real program would, rather than the kernel handing it a blob. */
void spawn_cos_waitpid_test(void) {
    extern bool fs_write_file_at(const char* path, const char* name,
                                 const char* data, uint64_t size);
    extern void* fs_list_dir(const char* path);

    bool wrote = fs_write_file_at("/", "childexit.c-os",
                                  (const char*)cos_childexit_elf,
                                  cos_childexit_elf_len);
    serial_puts("[VALIDATION] seeded /childexit.c-os: ");
    serial_puts(wrote ? "ok" : "FAILED");
    serial_puts("\n");
    if (!wrote) return;
    /* Refresh the directory cache fs_find() consults - see the note in
     * cos_validation_disk_launch(). */
    (void)fs_list_dir("/");

    serial_puts("[USERSPACE] Loading waitpid test .c-os program...\n");
    (void)cos_launch_elf_image("waiter.c-os", cos_waiter_elf, cos_waiter_elf_len);
}

/* ---- .c-osll runtime support -------------------------------------------
 *
 * All of it now lives in cos_elf_link.c. What was here was a single
 * GLOBAL four-entry array named COS_LIB_MAX_PER_PROC - shared by every
 * process in the system, so two programs each loading two libraries
 * exhausted it - with library bases assigned by slot index times a fixed
 * 256 MiB stride, which silently overlapped for any library larger than
 * the stride. These wrappers keep the old entry points working while the
 * real work happens in the linker.
 */

int64_t cos_dlopen_path(const char *path)
{
    process_t *proc = process_get_current();
    if (!proc) return -1;
    /* RTLD_GLOBAL matches the old behaviour, which had no concept of a
     * local scope: every library it loaded was visible to every other. */
    return cos_link_dlopen(proc, path, COS_RTLD_GLOBAL);
}

uint64_t cos_dlsym_handle(int64_t handle, const char *name)
{
    process_t *proc = process_get_current();
    if (!proc) return 0;
    return cos_link_dlsym(proc, handle, name);
}

/* Releases a process's link namespace. Called from process teardown so
 * the table does not fill with entries for processes that no longer
 * exist - the same job the old cos_dlclose_for_pid() did, for a
 * per-process structure instead of a shared array. */
void cos_dlclose_for_pid(uint64_t pid)
{
    cos_link_release_pid(pid);
}

/* .c-osll test: seeds the shared library onto the FAT32 volume, then runs
 * a program that dlopens it BY PATH - the same way a real program would -
 * resolves exports, and calls into them. */
void spawn_cos_library_test(void) {
    extern bool fs_write_file_at(const char* path, const char* name,
                                 const char* data, uint64_t size);
    extern void* fs_list_dir(const char* path);

    bool wrote = fs_write_file_at("/", "libmath.c-osll",
                                  (const char*)cos_libmath_so, cos_libmath_so_len);
    serial_puts("[VALIDATION] seeded /libmath.c-osll: ");
    serial_puts(wrote ? "ok" : "FAILED");
    serial_puts("\n");
    if (!wrote) return;
    (void)fs_list_dir("/");

    serial_puts("[USERSPACE] Loading .c-osll test program...\n");
    (void)cos_launch_elf_image("libuser.c-os", cos_libuser_elf, cos_libuser_elf_len);
}

/* Regression test for a real bug found during audit: syscalls validated
 * user buffers with paging_user_range_ok(), which only accepts PHYSICALLY
 * PRESENT pages. Heap memory returned by sbrk() is not actually mapped
 * until first touched (demand paging) - so a syscall receiving a freshly
 * allocated, never-yet-written buffer as a destination would reject it,
 * even though it is a completely legitimate pointer inside
 * [heap_start, heap_end). This is the ordinary "malloc a buffer, then
 * read() into it" pattern, which does not pre-touch the buffer. */
void spawn_cos_untouched_test(void) {
    serial_puts("[USERSPACE] Loading untouched-heap-buffer regression test...\n");
    (void)cos_launch_elf_image("untouched.c-os", cos_untouched_elf, cos_untouched_elf_len);
}

/* Verifies the streaming fd layer: open/write/close, then open/read in
 * two short chunks (proving the seek position persists between calls on
 * the same fd - a whole-file API cannot express this at all), lseek back
 * to the start and re-read, a read past EOF returning 0 rather than an
 * error, and a real directory listing via opendir/readdir/closedir. */
void spawn_cos_fd_test(void) {
    serial_puts("[USERSPACE] Loading file descriptor test .c-os program...\n");
    (void)cos_launch_elf_image("fdtest.c-os", cos_fdtest_elf, cos_fdtest_elf_len);
}

/* Cross-process signal test: the receiver registers a handler and waits
 * (via ordinary SYS_SLEEP_MS calls - the checkpoint at which a pending
 * signal is actually delivered), the sender reads the receiver's pid off
 * a marker file and calls SYS_KILL, and the receiver verifies its handler
 * ran with the correct signal number AND that normal execution resumed
 * correctly afterward (via SYS_SIGRETURN). */
void spawn_cos_signal_test(void) {
    serial_puts("[USERSPACE] Loading signal test .c-os programs (receiver + sender)...\n");
    (void)cos_launch_elf_image("sigreceiver.c-os", cos_sigreceiver_elf, cos_sigreceiver_elf_len);
    (void)cos_launch_elf_image("sigsender.c-os", cos_sigsender_elf, cos_sigsender_elf_len);
}

/* Proves signal 9 cannot be caught or ignored: a process registers a
 * handler for it anyway, sends it to itself, and must be force-
 * terminated at the next syscall checkpoint - the registered handler
 * must never run, and the process must never see the sleep return. */
void spawn_cos_sigkill_test(void) {
    serial_puts("[USERSPACE] Loading uncatchable-signal-9 test .c-os program...\n");
    (void)cos_launch_elf_image("sigkilltest.c-os", cos_sigkilltest_elf, cos_sigkilltest_elf_len);
}

/* External-symbol resolution test: seeds two real libraries with a
 * genuine cross-library dependency (libdepend calls libbase via a
 * JUMP_SLOT relocation, confirmed with readelf while building this),
 * then runs two SEPARATE processes:
 *   - one that loads them in the CORRECT order and calls through the
 *     resolved relocation
 *   - one that loads ONLY libdepend, which must be REJECTED since
 *     nothing can resolve its external symbol
 * Two separate processes rather than one, so the "missing dependency"
 * case genuinely has no libbase loaded anywhere in its own process. */
void spawn_cos_extsym_test(void) {
    extern bool fs_write_file_at(const char* path, const char* name,
                                 const char* data, uint64_t size);
    extern void* fs_list_dir(const char* path);

    bool ok1 = fs_write_file_at("/", "libbase.c-osll",
                                (const char*)cos_libbase_so, cos_libbase_so_len);
    bool ok2 = fs_write_file_at("/", "libdepend.c-osll",
                                (const char*)cos_libdepend_so, cos_libdepend_so_len);
    serial_puts("[VALIDATION] seeded libbase.c-osll: ");
    serial_puts(ok1 ? "ok" : "FAILED");
    serial_puts(", libdepend.c-osll: ");
    serial_puts(ok2 ? "ok" : "FAILED");
    serial_puts("\n");
    if (!ok1 || !ok2) return;
    (void)fs_list_dir("/");

    serial_puts("[USERSPACE] Loading external-symbol resolution test programs...\n");
    (void)cos_launch_elf_image("extsymtest.c-os", cos_extsymtest_elf, cos_extsymtest_elf_len);
    (void)cos_launch_elf_image("extsymmissing.c-os", cos_extsymmissing_elf,
                               cos_extsymmissing_elf_len);
}

/* Verifies SYS_MMAP/SYS_MUNMAP: two independently-addressed regions that
 * do not overlap, demand paging works across a WHOLE requested region
 * (not just its first page), a PROT_READ-only region is genuinely
 * non-writable (this session found and fixed task_alloc_page() forcing
 * every demand-paged page writable regardless of requested flags - this
 * test exercises the fix, though it only checks the read side directly;
 * writing to it is covered by the separate mmap_write_fault crash test),
 * munmap actually releases a region, and a mismatched-length munmap is
 * correctly rejected rather than partially unmapping. */
void spawn_cos_mmap_test(void) {
    serial_puts("[USERSPACE] Loading mmap/munmap test .c-os program...\n");
    (void)cos_launch_elf_image("mmaptest.c-os", cos_mmaptest_elf, cos_mmaptest_elf_len);
}

/* Proves PROT_READ-only mmap is enforced by the HARDWARE, not merely
 * accepted and ignored: deliberately writes to a read-only mapping,
 * which must genuinely fault (page present, PAGE_RW clear -> a real CPU
 * protection violation) and terminate the process - followed by a normal
 * program launch to prove the kernel survived it, matching the same
 * proof-of-survival pattern used for the earlier ud2 crash test. */
void spawn_cos_mmap_write_fault_test(void) {
    serial_puts("[USERSPACE] Loading mmap-write-to-readonly fault test...\n");
    (void)cos_launch_elf_image("mmapwritefault.c-os", cos_mmapwritefault_elf,
                               cos_mmapwritefault_elf_len);
}

/* Verifies the real wait-queue waitpid against the specific bug class its
 * design has to handle correctly: wait_queue_wake_all() wakes EVERY
 * blocked waiter in the parent's queue, not just ones whose target
 * already matches, so a parent waiting for a SPECIFIC child must
 * correctly ignore a spurious wake caused by a DIFFERENT sibling exiting
 * first, re-check, and go back to sleep rather than mis-returning the
 * wrong child's pid or status. Seeds both children onto the FAT32 volume
 * since SYS_SPAWN takes a path. */
void spawn_cos_multiwaiter_test(void) {
    extern bool fs_write_file_at(const char* path, const char* name,
                                 const char* data, uint64_t size);
    extern void* fs_list_dir(const char* path);

    bool ok1 = fs_write_file_at("/", "childfast.c-os",
                                (const char*)cos_childfast_elf, cos_childfast_elf_len);
    bool ok2 = fs_write_file_at("/", "childslow.c-os",
                                (const char*)cos_childslow_elf, cos_childslow_elf_len);
    serial_puts("[VALIDATION] seeded childfast.c-os: ");
    serial_puts(ok1 ? "ok" : "FAILED");
    serial_puts(", childslow.c-os: ");
    serial_puts(ok2 ? "ok" : "FAILED");
    serial_puts("\n");
    if (!ok1 || !ok2) return;
    (void)fs_list_dir("/");

    serial_puts("[USERSPACE] Loading multi-waiter targeted-wait test...\n");
    (void)cos_launch_elf_image("multiwaiter.c-os", cos_multiwaiter_elf,
                               cos_multiwaiter_elf_len);
}

/* Verifies the signal-nesting fix: a handler that itself sends a
 * DIFFERENT signal to itself before returning must not have that second
 * signal delivered nested inside the first (which would clobber
 * last_sigframe_addr and corrupt the outer handler's own return point).
 * It must instead be deferred until the first handler fully returns, then
 * delivered against the freshly-resumed context - checked by requiring
 * strict A-then-B ordering AND a real computation after both complete. */
void spawn_cos_signest_test(void) {
    serial_puts("[USERSPACE] Loading nested-signal correctness test...\n");
    (void)cos_launch_elf_image("signest.c-os", cos_signest_elf, cos_signest_elf_len);
}

/* Seeds Test.c-os onto the FAT32 root, so it appears in the file manager
 * exactly like any file a user created themselves - proving the
 * double-click launch path (fm_is_cos_executable() ->
 * cos_launch_elf_file() in gui_apps_common.c, wired up in an earlier
 * session) works for a real, complex GUI application, not only for the
 * simple embedded test programs used to validate individual syscalls. */
/* Test.c-os now ships as a REAL default file, written by
 * fs_bootstrap_defaults() (fs.c) from src/assets/test_c_os.c on every
 * boot - the same asset pipeline every other default /desktop file uses
 * - so it exists whether or not this validation code runs at all. This
 * file's own cos_test_app_elf/_len (the SAME bytes, embedded a second
 * time via build/cos_test_app_elf.h purely for this validation hook) is
 * kept only so cos_open_test_app() below has a name to launch through
 * the SAME embedded-image path other boot-time demo launches already
 * use, without depending on fs_bootstrap_defaults() having run first in
 * whatever order these hooks execute.
 *
 * Opens Test.c-os through gui_open_file_in_app() - the SAME function the
 * file manager's real double-click handler calls (efm_input.c's
 * efm_handle_double_click(), verified in an earlier session) - so this
 * exercises the genuine dispatch path (fm_is_cos_executable() check,
 * disk read via fs_read_file_at(), cos_launch_elf_image()), not a
 * shortcut around it. What this hook adds beyond the double-click event
 * itself is simulating the KEYBOARD interaction afterward, which a
 * bare double-click cannot demonstrate on its own. */
void cos_open_test_app(void) {
    extern void gui_open_file_in_app(const char* path, int file_type);
    serial_puts("[VALIDATION] double-clicking /Test.c-os via the real file manager dispatch\n");
    gui_open_file_in_app("/Test.c-os", 0);
}

/* Verifies COS_MOD_* modifier reporting on SYS_WIN_POLL_KEY events. */
void spawn_cos_modtest(void) {
    serial_puts("[USERSPACE] Loading keyboard modifier test .c-os program...\n");
    (void)cos_launch_elf_image("modtest.c-os", cos_modtest_elf, cos_modtest_elf_len);
}

/* Verifies the C runtime (userland/lib) end to end: crt0's argc/argv
 * setup, printf/snprintf conversions, the string functions, the malloc
 * free-list allocator (alloc/read-back/free/reuse/calloc-zeroing/realloc-
 * preserving), and file I/O into heap memory - all from a program written
 * in C rather than hand-written assembly, which was impossible before the
 * runtime existed. */
void spawn_cos_ctest(void) {
    serial_puts("[USERSPACE] Loading C-runtime test program (built from C source)...\n");
    (void)cos_launch_elf_image("ctest.c-os", cos_ctest_elf, cos_ctest_elf_len);
}

/* Verifies DT_NEEDED automatic dependency loading. Seeds a library that
 * DECLARES a dependency (libdepend2 -> libbase) and a program that opens
 * ONLY the dependent one - the loader must find and load libbase itself
 * for the cross-library relocation to resolve. */
void spawn_cos_deptest(void) {
    extern bool fs_write_file_at(const char* path, const char* name,
                                 const char* data, uint64_t size);
    extern void* fs_list_dir(const char* path);

    bool a = fs_write_file_at("/", "libbase.c-osll",
                              (const char*)cos_libbase2_so, cos_libbase2_so_len);
    bool b = fs_write_file_at("/", "libdepend2.c-osll",
                              (const char*)cos_libdepend2_so, cos_libdepend2_so_len);
    serial_puts("[VALIDATION] seeded DT_NEEDED test libraries: ");
    serial_puts((a && b) ? "ok" : "FAILED");
    serial_puts("\n");
    if (!a || !b) return;
    (void)fs_list_dir("/");

    serial_puts("[USERSPACE] Loading DT_NEEDED dependency test...\n");
    (void)cos_launch_elf_image("deptest.c-os", cos_deptest_elf, cos_deptest_elf_len);
}

/* Exercises the advanced capabilities newly exposed to ring3: real
 * threads, monotonic/wall-clock time, filesystem mutation
 * (mkdir/rename/unlink/stat), and setjmp/longjmp. */
void spawn_cos_advtest(void) {
    serial_puts("[USERSPACE] Loading advanced-capability test...\n");
    (void)cos_launch_elf_image("advtest.c-os", cos_advtest_elf, cos_advtest_elf_len);
}

/* Exercises the FILE* stdio layer, stdlib staples and the network
 * syscalls - the surface a small external C program would need to be
 * ported to C-OS with modest changes. */
void spawn_cos_libtest(void) {
    serial_puts("[USERSPACE] Loading stdio/stdlib/net library test...\n");
    (void)cos_launch_elf_image("libtest.c-os", cos_libtest_elf, cos_libtest_elf_len);
}
