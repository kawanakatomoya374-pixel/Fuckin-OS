#define PHYS_TO_VIRT(phys) ((phys) + 0xFFFF800000000000ULL)
/* paging_free_pages / paging_virt_to_phys / paging_user_range_ok come
 * from mm/paging.h, included below.
 *
 * They were re-declared here by hand as `unsigned long` and `int` where
 * the real signatures are uint64_t and bool. That happens to be
 * call-compatible on x86-64 - which is why it went unnoticed - but it is
 * a genuine ODR violation, and a compiler is free to warn on or
 * mis-optimise the mismatch. Deleted in favour of the header now that it
 * declares them. */
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
#include "mm/paging.h"
#include "scheduler.h"
#include "cos_app_window.h"
#include "cos_elf_link.h"
#include "cos_futex.h"
#include "cos_fd.h"
#include "timer.h"
#include "rtc.h"
#include "api/net_api.h"
#include "memory.h"
#include "string.h"

extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);
extern int cos_fs_write_file(const char* path, const void* data, uint64_t size);

#define SYS_WRITE      0
#define SYS_EXIT       1
/* GUI calls for ring3 .c-os programs. These do not draw: they queue
 * commands that the GUI owner thread replays (see cos_app_window.c). */
#define SYS_WIN_CREATE 2
#define SYS_WIN_FILL   3
#define SYS_WIN_CLEAR  4
/* Memory. Without these a .c-os program cannot allocate at all - no
 * malloc, no dynamic data structures - which is the single biggest thing
 * standing between "runs hand-written assembly" and "runs a real
 * program". */
#define SYS_BRK        5
#define SYS_SBRK       6
/* Whole-file I/O. Deliberately NOT a POSIX open/read/write/close file
 * descriptor set: that needs a per-process fd table, seek offsets, and
 * lifetime management across fork/exit, none of which exists yet, and a
 * half-built fd layer would be worse than an honest whole-file one.
 * These two cover what a small program actually needs (load a file, save
 * a file) and can be kept as the fast path if a real fd layer is added
 * later. */
#define SYS_READ_FILE  7
#define SYS_WRITE_FILE 8
/* Scheduling. A ring3 program had no way to wait: the only option was a
 * busy loop, which is preemptively scheduled and therefore burns a full
 * CPU share forever. That is not hypothetical - the first GUI demo
 * program in this tree ended in `jmp $` to stay resident and measurably
 * starved the rest of the system until it was changed to exit. Without
 * these, any program that wants to stay alive has that same problem. */
#define SYS_YIELD      9
#define SYS_SLEEP_MS   10
#define SYS_GETPID     11
/* Process creation. Together with the loader this is what lets a program
 * start another program - the practical equivalent of fork+exec here.
 * Implemented as spawn rather than fork because fork needs copy-on-write
 * address-space duplication, which does not exist, and a fork that
 * silently shared or eagerly copied everything would be a trap. */
#define SYS_SPAWN      12
/* Wait for a spawned child and collect its exit status. Genuinely blocks
 * the calling thread on a real wait queue (process_t::child_wait_queue),
 * woken by process_exit() the moment a child becomes a zombie - see the
 * implementation below for the full design note. An earlier version of
 * this polled process_try_reap_child() between 10ms sleeps; replaced once
 * the wait-queue primitives it originally said did not exist turned out
 * to already be present in scheduler.h (wait_queue_t/scheduler_sleep_on/
 * wait_queue_wake_all), just not yet wired up for this case. */
#define SYS_WAITPID    13
/* Shared libraries (.c-osll). Deliberately a kernel-mediated
 * load-and-resolve rather than a userspace dynamic linker: there is no
 * libc, no PLT/GOT setup in these freestanding programs, and no ld.so to
 * host one. The kernel maps the object into the calling process, applies
 * its relocations, and hands back the address of a named export - which
 * is what dlopen()/dlsym() give a caller, without needing the rest of a
 * dynamic linker to exist first. */
#define SYS_DLOPEN     14
#define SYS_DLSYM      15
/* Streaming file I/O: a persistent handle with a seek position, on top
 * of the earlier whole-file SYS_READ_FILE/SYS_WRITE_FILE. Needed for
 * anything that reads a file larger than it wants to buffer at once,
 * appends to a log, writes incrementally, or lists a directory. */
#define SYS_FD_OPEN     16
#define SYS_FD_OPENDIR  17
#define SYS_FD_READ     18
#define SYS_FD_WRITE    19
#define SYS_FD_LSEEK    20
#define SYS_FD_READDIR  21
#define SYS_FD_CLOSE    22
/* Signals.
 *
 * process_t already had pending_signals/blocked_signals/signal_handlers[]
 * fields (task.c), and signal_send()/signal_set_handler() correctly just
 * flip bits and store a pointer - those are reused as-is below. What did
 * NOT exist safely was delivery: task.c's signal_process_pending() (dead
 * code - zero callers anywhere) called `handler(sig)` as a DIRECT C
 * function call from kernel code. A registered handler is a RING3
 * function pointer; calling it that way does not cross any privilege
 * boundary - no ring transition, no stack switch, no CS change - so had
 * anything ever invoked that function, the handler would have executed
 * WITH RING0 PRIVILEGES on the kernel's own stack. That is a real
 * security-severity bug, currently inert only because nothing calls it.
 * It is not reused; a correct trampoline-based delivery replaces it,
 * implemented here where the syscall return path already has direct
 * access to the register frame that controls what CS/RIP/RSP the CPU
 * resumes with. */
#define SYS_SIGACTION  23
#define SYS_KILL       24
#define SYS_SIGRETURN  25
/* Anonymous private memory, independent of the heap. sbrk() can only grow
 * and shrink from a single moving edge; this is for a program that wants
 * an independently-addressed, independently-freeable block - the other
 * major gap versus a real malloc-capable environment now that heap/file/
 * process/signal syscalls all exist. File-backed and shared (MAP_SHARED)
 * mappings are NOT implemented - see the design note above
 * cos_mmap_alloc() for why anonymous-private was chosen as the correctly-
 * scoped first version rather than attempting all of mmap's modes at
 * once. */
#define SYS_MMAP       26
#define SYS_MUNMAP     27
/* Text rendering and keyboard input for a .c-os GUI window, extending
 * the SYS_WIN_* family (2-4) added earlier. Without these, an app window
 * could only ever be filled rectangles - no readable text, no way for
 * the user to type anything into it - which is not "a GUI app", just a
 * colored box. Reuses the SAME command-queue architecture as
 * SYS_WIN_FILL: syscalls only queue, the GUI owner thread does the
 * actual drawing, exactly as documented in cos_app_window.c. */
#define SYS_WIN_DRAW_TEXT 28
#define SYS_WIN_POLL_KEY  29

/* ---- Threads -------------------------------------------------------------
 * The kernel already had thread_create()/thread_exit() and a scheduler
 * that runs multiple threads per process - none of it was reachable from
 * ring3. A .c-os program was single-threaded not by design but because
 * nothing exposed the capability. */
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT   31
#define SYS_THREAD_JOIN   32

/* ---- Time ----------------------------------------------------------------
 * Without these a program cannot measure elapsed time, animate, time out,
 * or seed anything - it can only sleep for a fixed duration and hope. */
#define SYS_TIME_MS       33   /* monotonic milliseconds since boot */
#define SYS_TIME_UNIX     34   /* wall-clock seconds from the RTC */

/* ---- Filesystem mutation --------------------------------------------------
 * Reading and writing file CONTENT was possible; changing the filesystem
 * itself was not. A program could not create a directory, delete a file it
 * had written, or rename anything. */
#define SYS_MKDIR         35
#define SYS_UNLINK        36
#define SYS_RENAME        37
#define SYS_STAT          38

/* ---- Networking ----------------------------------------------------------
 * The kernel already had a TCP/IP stack, DNS resolver and HTTP client
 * (src/kernel/api/net_api.*, used by the bundled browser) - none of it
 * reachable from ring3. A .c-os program could not make a network request
 * of any kind.
 *
 * Exposed at the HTTP/DNS level rather than as BSD sockets: net_api is
 * what actually exists and is exercised by the browser, whereas a socket
 * layer would be a new abstraction written on top of it with nothing yet
 * proving it works. This is the honest surface - it makes real network
 * requests possible now, and does not pretend to be POSIX sockets. */
#define SYS_NET_AVAILABLE 39
#define SYS_NET_RESOLVE   40   /* hostname -> 4-byte IPv4 */
#define SYS_NET_HTTP_GET  41   /* url -> response body */

/* ---- Thread-local storage and the dynamic link map -----------------------
 *
 * A program that wants `__thread` variables needs the CPU's %fs base to
 * point at its TLS block, and only ring0 can write that MSR. The kernel
 * sets it up for the main thread at exec time; SYS_SET_TLS is how a
 * thread the program created itself installs its own block, which is the
 * whole point of thread-LOCAL storage.
 *
 * SYS_DL_INFO hands back the address of the read-only link map the
 * loader published (see cos_elf_linkmap_t). It duplicates what the
 * auxiliary vector already carries in COS_AT_COS_LINKMAP, and exists for
 * the case the auxv cannot cover: code running long after startup, in a
 * library, that never saw the initial stack. */
#define SYS_SET_TLS       42   /* rdi = thread pointer                  */
#define SYS_GET_TLS       43
#define SYS_DL_INFO       44   /* -> address of the cos_elf_linkmap_t   */
/* dlopen with flags (RTLD_GLOBAL/LOCAL), and dlsym against the whole
 * global scope rather than a single handle - neither expressible in the
 * original two-argument SYS_DLOPEN/SYS_DLSYM, which are kept so existing
 * programs keep working. */
#define SYS_DLOPEN2       45   /* rdi = path, rsi = flags               */
#define SYS_DLCLOSE       46

/* ---- thread synchronisation and process arguments -----------------------
 *
 * SYS_FUTEX_WAIT/WAKE are the primitive every userland lock is built on.
 * Before these, .c-os had threads and nothing to synchronise them with;
 * see src/kernel/cos_futex.c for why a futex rather than kernel mutex
 * objects.
 *
 * SYS_SPAWN2 exists because SYS_SPAWN takes only a path - there was NO
 * way for a program to pass arguments or an environment to a child it
 * started. The kernel could already build a full argv/envp stack; the
 * syscall simply had no parameters to fill it from. */
#define SYS_FUTEX_WAIT    47   /* rdi=addr, rsi=expected, rdx=timeout_ms */
#define SYS_FUTEX_WAKE    48   /* rdi=addr, rsi=count                    */
#define SYS_GETTID        49
#define SYS_SPAWN2        50   /* rdi=path, rsi=argv, rdx=envp           */

#define COS_PROT_READ  1u
#define COS_PROT_WRITE 2u

/* Dedicated PML4 slot (PML4[5]) for mmap, matching the SAME pattern
 * already used for the program image (PML4[1]), stack (PML4[2]), heap
 * (PML4[3]) and libraries (PML4[4]) - a fixed, non-overlapping region per
 * purpose, rather than trying to carve mmap space out of any of those. */
#define COS_MMAP_BASE      0x0000028000000000ULL
#define COS_MMAP_MAX_TOTAL (1ULL * 1024ULL * 1024ULL * 1024ULL)   /* 1 GiB cap per process */

/**
 * cos_mmap_alloc - bump-allocate a fresh anonymous region for `proc`.
 *
 * DESIGN: anonymous-private only, bump allocation, no reuse of freed
 * virtual address ranges.
 *
 * Anonymous-private (no file, no MAP_SHARED) was chosen as the correctly-
 * scoped first version: file-backed mmap needs per-mapping fd+offset
 * metadata and lazy page-in from the filesystem inside the page-fault
 * path, and MAP_SHARED needs cross-process physical-page-sharing
 * tracking that the physical allocator does not have today. Both are
 * real future work, not silently promised by this syscall's shape.
 *
 * Bump allocation (mmap_next_offset only ever increases) rather than a
 * general first-fit/best-fit allocator with merge/split on munmap: this
 * keeps region tracking a fixed 16-entry array with no fragmentation
 * bookkeeping. The real cost is honest and stated - munmap() frees the
 * PHYSICAL pages backing a region but never reclaims the VIRTUAL range
 * for a future mmap - bounded by COS_MMAP_MAX_TOTAL (1 GiB) per process,
 * which is generous for a program's total lifetime mmap usage without
 * risking silent, unbounded growth.
 *
 * Demand-paged exactly like the heap: task_handle_page_fault() (task.c)
 * checks proc->mmap_regions[] the same way it already checks
 * heap_start/heap_end, so nothing is eagerly mapped here - only recorded.
 */
static int64_t cos_mmap_alloc(process_t *proc, uint64_t length, bool writable)
{
    if (length == 0) return -1;
    uint64_t rounded = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (proc->mmap_next_offset + rounded < proc->mmap_next_offset) return -1;   /* overflow */
    if (proc->mmap_next_offset + rounded > COS_MMAP_MAX_TOTAL) return -1;       /* cap */

    int slot = -1;
    for (int i = 0; i < COS_MMAP_MAX_REGIONS; ++i) {
        if (!proc->mmap_regions[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;   /* table full */

    uint64_t start = COS_MMAP_BASE + proc->mmap_next_offset;
    proc->mmap_regions[slot].start = start;
    proc->mmap_regions[slot].end = start + rounded;
    proc->mmap_regions[slot].used = true;
    proc->mmap_regions[slot].writable = writable;
    proc->mmap_next_offset += rounded;
    return (int64_t)start;
}

static bool cos_mmap_free(process_t *proc, uint64_t addr, uint64_t length)
{
    uint64_t rounded = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (int i = 0; i < COS_MMAP_MAX_REGIONS; ++i) {
        cos_mmap_region_t *r = &proc->mmap_regions[i];
        if (!r->used) continue;
        if (r->start != addr || (r->end - r->start) != rounded) continue;
        /* Exact-match only: a real munmap() supports partially unmapping
         * or splitting a region. Requiring an exact match to a
         * previously-returned mmap() region is a stated, deliberate
         * simplification - see the header note - rather than
         * implementing region splitting for this pass. */
        uint64_t pages = (r->end - r->start) / PAGE_SIZE;
        paging_free_pages(r->start, pages);
        r->used = false;
        return true;
    }
    return false;
}

#define SYSCALL_TITLE_MAX 64

/* Upper bound on how far SYS_BRK/SYS_SBRK will let one process grow its
 * heap. Policy, not an architectural limit: it bounds how much a single
 * runaway .c-os program can take from the system, and it is checked
 * independently of the heap-vs-stack collision check below (a program
 * could otherwise sit under the cap and still collide, or vice versa). */
#define COS_USER_HEAP_MAX (64ULL * 1024ULL * 1024ULL)

/* True if growing the heap to `target` cannot run into the stack region.
 *
 * This must NOT assume the heap sits below the stack. In this address-space
 * layout it does not: the stack region is at PML4[2] (0x100_0000_0000) and
 * the heap at PML4[3] (0x180_0000_0000), so the heap starts ABOVE the
 * stack. A naive "target >= stack_start" test - which is the usual check
 * when the heap grows up toward a stack growing down - is therefore true
 * for every possible heap address here, and rejected every single
 * allocation. That was a real bug caught by the first ring3 test of this
 * syscall (MEMFILE_FAIL sbrk failed), not by reading the code.
 *
 * The correct test is whether the heap's resulting span would overlap the
 * stack's span at all, in either direction. */
static bool cos_heap_growth_is_safe(const process_t *proc, uint64_t target)
{
    if (!proc->stack_start || !proc->stack_end) return true;   /* no stack to hit */
    if (target <= proc->heap_start) return true;               /* not growing */
    /* Heap occupies [heap_start, target); stack occupies
     * [stack_start, stack_end). Disjoint iff one ends at or before the
     * other begins. */
    if (target <= proc->stack_start) return true;
    if (proc->heap_start >= proc->stack_end) return true;
    return false;
}

#define COS_SYSCALL_PATH_MAX 256
#define COS_SYSCALL_FILE_MAX (8ULL * 1024ULL * 1024ULL)
#define COS_SYSCALL_SLEEP_MAX_MS (60ULL * 1000ULL)   /* still used by SYS_SLEEP_MS */

/* Defined in userspace_demo.c alongside the other launch paths. Returns
 * the new pid, or -1. */
extern int64_t cos_spawn_elf_path(const char *path);
extern int64_t cos_dlopen_path(const char *path);
extern bool cos_fs_mkdir(const char *path);
extern bool cos_fs_unlink(const char *path);
extern bool cos_fs_rename(const char *oldp, const char *newp);
extern bool cos_fs_stat(const char *path, uint64_t *out_size, uint8_t *out_is_dir);

/* Mirrors cos_stat_t in userland/include/cos.h - kept in sync by hand
 * because the kernel cannot include the userland header. */
typedef struct { uint64_t size; uint8_t is_dir; } cos_stat_t;
extern uint64_t cos_dlsym_handle(int64_t handle, const char *name);

/* Copies a NUL-terminated path out of user memory into a kernel buffer.
 *
 * Validates page by page as it walks, rather than validating a fixed
 * length up front: the length is not known until the NUL is found, and
 * assuming a maximum would reject legitimate short paths that sit near
 * the end of a mapping. Rejects anything unterminated within the buffer,
 * so the kernel never ends up with a non-terminated string. */
/**
 * cos_syscall_touch_user_range - demand-page in any not-yet-mapped pages
 * in [uaddr, uaddr+len) that legitimately belong to the calling process,
 * BEFORE validating the range with paging_user_range_ok().
 *
 * Why this exists (a real bug, found by testing the ordinary case rather
 * than by reading the code): heap memory returned by SYS_SBRK is not
 * actually mapped until first touched - that is the whole point of the
 * demand paging in task.c. paging_user_range_ok() only accepts PAGES THAT
 * ARE ALREADY PRESENT, so a syscall receiving a freshly-grown, never-yet-
 * written heap buffer as a destination rejected it outright, even though
 * it is a completely ordinary, legitimate pointer inside
 * [heap_start, heap_end). "sbrk a buffer, then read() into it" is not an
 * edge case - it is the normal way a buffer gets used - so this was not a
 * corner case bug, it was a "the common path does not work" bug.
 *
 * This calls the EXACT SAME function the real page-fault handler calls
 * (task_handle_page_fault(), see task.c) with a synthesized error code
 * matching what the CPU would have generated for a real access of this
 * kind (user-mode, present-bit clear, write bit set iff `need_write`).
 * That means a syscall-validated buffer is faulted in with IDENTICAL
 * semantics to the process touching the memory itself: heap pages inside
 * the process's own heap window get mapped, stack pages just below the
 * current bottom grow the stack, and anything else is correctly left
 * unmapped for paging_user_range_ok() to then reject. This does not
 * relax validation - it only pre-does the mapping step a real touch would
 * have done anyway, so a genuinely invalid pointer is not made to look
 * valid by this call.
 */
static void cos_syscall_touch_user_range(uint64_t uaddr, uint64_t len, bool need_write)
{
    if (len == 0) return;
    uint64_t start = uaddr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = uaddr + len;
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (paging_virt_to_phys(page) != 0) continue;   /* already mapped */
        uint64_t synthetic_error_code = 0x4ULL | (need_write ? 0x2ULL : 0ULL);
        (void)task_handle_page_fault(page, synthetic_error_code);
        /* Ignored on purpose: if this could not fault the page in (a
         * genuinely invalid address), the page is simply still not
         * present, and paging_user_range_ok() right after this call is
         * what actually enforces that and rejects the syscall. */
    }
}

static bool cos_syscall_copy_path(char *dst, uint64_t dst_size, uint64_t uptr)
{
    if (!dst || dst_size == 0 || uptr == 0) return false;
    for (uint64_t i = 0; i < dst_size; ++i) {
        cos_syscall_touch_user_range(uptr + i, 1, false);
        if (!paging_user_range_ok(uptr + i, 1, false)) {
            serial_puts("[SYSCALL] path rejected: not in a valid user mapping\n");
            return false;
        }
        char c = *(const char *)(uintptr_t)(uptr + i);
        dst[i] = c;
        if (c == '\0') return true;
    }
    serial_puts("[SYSCALL] path rejected: unterminated or too long\n");
    return false;
}

/* Caps on a spawn's argument and environment vectors. These bound a
 * kernel-side copy whose size is chosen by the caller, so they are
 * policy rather than an ABI limit; the initial stack builder has its own
 * 64 KiB budget on top. */
#define COS_SPAWN_MAX_ARGS   32
#define COS_SPAWN_MAX_ARGLEN 256

/* Copies a NULL-terminated array of user string pointers into kernel
 * storage, returning the count or -1.
 *
 * Every pointer AND every string is validated, one page-checked byte at
 * a time, exactly as cos_syscall_copy_path does. The vector is a user
 * pointer to an array of user pointers, so there are two levels of
 * untrusted indirection here and both have to be checked - reading the
 * array itself without validating it is the easier half to forget.
 *
 * Copying happens before the caller switches address spaces: these
 * pointers are only meaningful in the PARENT's address space, and
 * dereferencing one after the switch would silently read the child's
 * memory instead. */
static int cos_syscall_copy_strvec(uint64_t uvec,
                                   char storage[][COS_SPAWN_MAX_ARGLEN],
                                   const char *out[], int max)
{
    if (uvec == 0) { out[0] = NULL; return 0; }
    if (uvec & 7u) return -1;                    /* pointer array must align */

    int n = 0;
    for (; n < max; ++n) {
        uint64_t slot = uvec + (uint64_t)n * 8u;
        cos_syscall_touch_user_range(slot, 8, false);
        if (!paging_user_range_ok(slot, 8, false)) return -1;

        uint64_t sp = *(const uint64_t *)(uintptr_t)slot;
        if (sp == 0) break;                       /* end of vector */
        if (!cos_syscall_copy_path(storage[n], COS_SPAWN_MAX_ARGLEN, sp)) return -1;
        out[n] = storage[n];
    }
    if (n == max) {
        serial_puts("[SYSCALL] argument vector rejected: too many entries\n");
        return -1;
    }
    out[n] = NULL;
    return n;
}


/* Identity of the ring3 process that issued the current syscall. Used to
 * enforce window ownership, so one .c-os program cannot draw into
 * another's window by guessing a small handle value. */
static uint32_t cos_syscall_caller_pid(void)
{
    process_t *p = process_get_current();
    return p ? (uint32_t)p->pid : 0u;
}

/* Bound how much a single SYS_WRITE can dump, so a buggy or hostile user
 * program can't wedge the kernel into an unbounded serial write loop. */
#define SYSCALL_WRITE_MAX 4096

/* ---- Signal delivery -----------------------------------------------------
 *
 * DESIGN: a real trampoline on the process's own stack, not a direct call.
 *
 * When a signal is deliverable, this rewrites the interrupt return frame
 * (`r`) so that the CPU's own `iretq` - the SAME instruction that would
 * otherwise resume the process where it made its syscall - instead
 * resumes at the handler, with CS/SS/CPL unchanged (still ring3) and a
 * genuine ring transition already having happened via the ORIGINAL
 * int 0x80. That is what makes this safe: the handler runs at ring3
 * because the frame it is resumed from IS a ring3 frame, not because of
 * anything this code does to elevate or lower privilege.
 *
 * Stack layout built below (low to high address):
 *   new_rsp             -> [8 bytes] return address = trampoline_addr
 *   trampoline_addr     -> [7 bytes] trampoline machine code
 *   sigframe_addr       -> [saved register state, for SYS_SIGRETURN]
 *
 * The handler is entered with RSP = new_rsp, RDI = signum. When the
 * handler's own compiler-generated `ret` executes, it pops the 8-byte
 * return address (= trampoline_addr) into RIP and continues there. The
 * trampoline is `mov eax, SYS_SIGRETURN; int 0x80` - a normal, real
 * syscall, entered the normal way, which is what makes SYS_SIGRETURN's
 * own handler able to find its way back to the ORIGINAL sigframe: at
 * that point RSP is exactly trampoline_addr (the trampoline pushed
 * nothing of its own before trapping), so sigframe_addr is a fixed,
 * known offset from the RSP the SIGRETURN syscall observes.
 */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    /* The process's blocked_signals mask AT THE MOMENT this signal was
     * delivered, restored by SYS_SIGRETURN. Without this, a signal
     * delivered while a DIFFERENT handler is already running would
     * overwrite proc->last_sigframe_addr with its own frame's location,
     * permanently losing the outer handler's return point - the outer
     * handler's own eventual `ret`-into-trampoline would then read the
     * WRONG (inner) frame, or a frame already reused, corrupting
     * resumption of whatever was interrupted before either signal fired.
     * See the design note in cos_deliver_pending_signal() for why this
     * is prevented by blocking further delivery for the duration of any
     * handler, rather than by trying to support a stack of frames. */
    uint64_t saved_blocked_signals;
} cos_sigframe_t;

/* mov eax, 25 ; int 0x80  (25 = SYS_SIGRETURN) */
static const uint8_t COS_SIG_TRAMPOLINE[7] = {
    0xB8, (uint8_t)SYS_SIGRETURN, 0x00, 0x00, 0x00, 0xCD, 0x80
};

#define COS_SIGFRAME_MAX_SIZE 256u   /* trampoline + sigframe, rounded up */

/* Local user-memory write, matching the pattern already used in cos_elf.c
 * (kept file-local here rather than sharing that file's static helpers
 * across translation units for a handful of small writes). Callers are
 * responsible for pre-touching the range with cos_syscall_touch_user_range()
 * so this cannot silently write into a not-yet-demand-paged page - see
 * the bug this exact class of mistake caused in SYS_READ_FILE/WRITE_FILE
 * before it was fixed. */
static bool cos_sig_write_user(uint64_t uaddr, const void *src, uint64_t len)
{
    const uint8_t *in = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; ++i) {
        uint64_t v = uaddr + i;
        uint64_t phys = paging_virt_to_phys(v & ~(uint64_t)(PAGE_SIZE - 1));
        if (!phys) return false;
        uint8_t *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        dst[v & (PAGE_SIZE - 1)] = in[i];
    }
    return true;
}

static bool cos_sig_read_user(uint64_t uaddr, void *dst, uint64_t len)
{
    uint8_t *out = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; ++i) {
        uint64_t v = uaddr + i;
        uint64_t phys = paging_virt_to_phys(v & ~(uint64_t)(PAGE_SIZE - 1));
        if (!phys) return false;
        const uint8_t *src = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        out[i] = src[v & (PAGE_SIZE - 1)];
    }
    return true;
}

/* Called once at the tail of every syscall, before returning to ring3.
 * This is the ONLY place signals are delivered - see the header/design
 * note above SYS_FD_CLOSE for why the old code path was unsafe and is not
 * reused. Honest limitation stated once here rather than scattered: a
 * process that never makes a syscall (a pure ring3 compute loop with no
 * yield/sleep/read/write/etc.) will not see a pending signal delivered
 * until it eventually does. Every syscall added in this codebase so far
 * is a natural, frequent checkpoint, but this is not a full preemptive
 * signal model. */
static void cos_deliver_pending_signal(struct regs *r)
{
    process_t *proc = process_get_current();
    if (!proc) return;

    cos_sigset_t deliverable = proc->pending_signals & ~proc->blocked_signals;
    if (deliverable == 0) return;

    int signum = -1;
    for (int i = 0; i < MAX_SIGNALS; ++i) {
        if (deliverable & (1ULL << i)) { signum = i; break; }
    }
    if (signum < 0) return;
    proc->pending_signals &= ~(1ULL << (uint64_t)signum);

    signal_handler_t handler = proc->signal_handlers[signum];

    /* Signal 9 cannot be caught or ignored, matching real SIGKILL - a
     * process must not be able to make itself unkillable by registering a
     * handler (or SIG_ACTION_IGNORE) for it. */
    bool force_terminate = (signum == 9) ||
                           (handler == SIG_ACTION_DEFAULT);
    if (handler == SIG_ACTION_IGNORE && signum != 9) return;   /* discarded */

    if (force_terminate) {
        serial_puts("[SIGNAL] pid=");
        serial_putdec(proc->pid);
        serial_puts(" terminated by signal ");
        serial_putdec((uint64_t)signum);
        serial_puts("\n");
        process_exit(proc, -(1000 + signum));
        /* process_exit() does not return for the current process (see the
         * exiting_self path in task.c) - if it somehow did, fail safe
         * rather than falling through to deliver a signal to a process
         * that should already be gone. */
        for (;;) { __asm__ volatile("hlt"); }
    }

    /* Real handler: build the trampoline + sigframe on the process's own
     * stack, just below its current RSP - the same region a real call
     * or interrupt would use, so it must be pre-touched exactly like any
     * other syscall-visible user buffer. */
    uint64_t frame_base = (r->rsp - COS_SIGFRAME_MAX_SIZE) & ~0xFULL;
    cos_syscall_touch_user_range(frame_base, COS_SIGFRAME_MAX_SIZE, true);

    uint64_t trampoline_addr = frame_base + 8;
    uint64_t sigframe_addr = trampoline_addr + sizeof(COS_SIG_TRAMPOLINE);
    sigframe_addr = (sigframe_addr + 7ULL) & ~7ULL;   /* keep 8-byte aligned */

    cos_sigframe_t frame;
    frame.r15 = r->r15; frame.r14 = r->r14; frame.r13 = r->r13; frame.r12 = r->r12;
    frame.r11 = r->r11; frame.r10 = r->r10; frame.r9  = r->r9;  frame.r8  = r->r8;
    frame.rbp = r->rbp; frame.rdi = r->rdi; frame.rsi = r->rsi; frame.rdx = r->rdx;
    frame.rcx = r->rcx; frame.rbx = r->rbx; frame.rax = r->rax;
    frame.rip = r->rip; frame.rflags = r->rflags; frame.rsp = r->rsp;
    frame.saved_blocked_signals = proc->blocked_signals;

    bool ok = cos_sig_write_user(frame_base, &trampoline_addr, sizeof(uint64_t));
    ok = ok && cos_sig_write_user(trampoline_addr, COS_SIG_TRAMPOLINE, sizeof(COS_SIG_TRAMPOLINE));
    ok = ok && cos_sig_write_user(sigframe_addr, &frame, sizeof(frame));
    if (!ok) {
        /* Could not build the frame (should only happen under real
         * memory pressure, since the range was just touched above).
         * Re-queue the signal rather than silently dropping it, and
         * leave the process to resume normally this one time. */
        proc->pending_signals |= (1ULL << (uint64_t)signum);
        serial_puts("[SIGNAL] failed to deliver signal (could not build stack frame)\n");
        return;
    }

    /* sigframe_addr must be recoverable from RSP alone at SIGRETURN time
     * (see the design note above): stash it in a per-process field rather
     * than trying to derive it purely from arithmetic on the trampoline's
     * fixed size, so a future change to trampoline padding cannot silently
     * desynchronize the two sides. */
    proc->last_sigframe_addr = sigframe_addr;

    /* Block further signal delivery for the duration of this handler -
     * ALL signals, not just this one, and deliberately NOT signal 9
     * (real SIGKILL is never blockable, and this architecture should not
     * make it MORE blockable than it has to be just because a different
     * handler happens to be running).
     *
     * Without this, cos_deliver_pending_signal() would run again at the
     * end of ANY syscall the handler itself makes (writing to serial,
     * sleeping, anything) and - if a second signal is pending - would
     * overwrite last_sigframe_addr with a frame nested inside THIS
     * handler's own stack usage, permanently losing the way back to
     * whatever this handler itself needs to return to. Real Unix
     * sigaction() defaults to at least blocking the same signal during
     * its own handler (SA_NODEFER opts out); blocking everything else
     * too is a stricter, simpler, and here deliberately conservative
     * policy given there is no per-handler sa_mask tracked. */
    proc->blocked_signals = ~(1ULL << 9ULL);

    r->rdi = (uint64_t)signum;
    r->rsp = frame_base;
    r->rip = (uint64_t)handler;
    /* CS/SS/CPL are untouched - the process resumes at ring3 because the
     * frame it resumes from already was one, exactly as it would for an
     * ordinary syscall return. */
}

static void syscall_handler(struct regs* r) {
    switch (r->rax) {
        case SYS_WRITE: {
            const char* buf = (const char*)(uintptr_t)r->rdi;
            uint64_t len = r->rsi;
            if (!buf) { r->rax = (uint64_t)-1; break; }
            if (len > SYSCALL_WRITE_MAX) len = SYSCALL_WRITE_MAX;

            /* Validate BEFORE dereferencing. r->rdi is fully attacker-
             * controlled: this handler runs at ring0 but on the caller's
             * address space, so without this check a ring3 program could
             * pass a kernel address and have the kernel dump kernel memory
             * to the serial console, or pass an unmapped address and take
             * a page fault inside an interrupt handler. The check covers
             * the clamped length actually used, not the requested one. */
            cos_syscall_touch_user_range((uint64_t)(uintptr_t)buf, len, false);
            if (!paging_user_range_ok((uint64_t)(uintptr_t)buf, len, false)) {
                serial_puts("[SYSCALL] SYS_WRITE rejected: buffer not in a "
                            "valid user mapping\n");
                r->rax = (uint64_t)-1;
                break;
            }

            uint64_t written = 0;
            for (uint64_t i = 0; i < len && buf[i]; ++i) {
                serial_putc(buf[i]);
                ++written;
            }
            /* Report bytes actually written, not the clamped request:
             * the old code returned `len` even when it stopped early at a
             * NUL, which would mislead any caller that used the result to
             * advance a buffer. */
            r->rax = written;
            break;
        }
        case SYS_WIN_CREATE: {
            /* rdi = title ptr (user), rsi = title len, rdx = w, r10 = h */
            const char *utitle = (const char *)(uintptr_t)r->rdi;
            uint64_t tlen = r->rsi;
            char title[SYSCALL_TITLE_MAX];
            title[0] = '\0';

            if (utitle && tlen) {
                if (tlen > SYSCALL_TITLE_MAX - 1) tlen = SYSCALL_TITLE_MAX - 1;
                /* Same rule as SYS_WRITE: validate before dereferencing.
                 * The title is copied into kernel memory immediately so
                 * the GUI never holds a pointer into a user address space
                 * that may be torn down or remapped underneath it. */
                cos_syscall_touch_user_range((uint64_t)(uintptr_t)utitle, tlen, false);
                if (!paging_user_range_ok((uint64_t)(uintptr_t)utitle, tlen, false)) {
                    serial_puts("[SYSCALL] SYS_WIN_CREATE rejected: bad title pointer\n");
                    r->rax = (uint64_t)-1;
                    break;
                }
                for (uint64_t i = 0; i < tlen; ++i) {
                    char c = utitle[i];
                    /* Strip control characters: this string goes straight
                     * into a title bar renderer. */
                    title[i] = (c >= 32 && c < 127) ? c : '?';
                }
                title[tlen] = '\0';
            }

            uint32_t pid = cos_syscall_caller_pid();
            r->rax = (uint64_t)cos_app_window_create(title,
                        (int32_t)(int64_t)r->rdx, (int32_t)(int64_t)r->r10, pid);
            break;
        }

        case SYS_WIN_FILL: {
            /* rdi = handle, rsi = x, rdx = y, r10 = w, r8 = h, r9 = colour.
             * All coordinates are clamped against the window's client area
             * inside cos_app_window_draw(), not trusted here. */
            uint32_t pid = cos_syscall_caller_pid();
            bool ok = cos_app_window_fill_rect((int64_t)r->rdi,
                        (int32_t)(int64_t)r->rsi, (int32_t)(int64_t)r->rdx,
                        (int32_t)(int64_t)r->r10, (int32_t)(int64_t)r->r8,
                        (uint32_t)r->r9, pid);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_WIN_CLEAR: {
            uint32_t pid = cos_syscall_caller_pid();
            cos_app_window_clear((int64_t)r->rdi, pid);
            r->rax = 0;
            break;
        }

        case SYS_BRK: {
            /* rdi = requested new break, or 0 to query the current one.
             * Returns the resulting break, or (uint64_t)-1 on failure -
             * note this deliberately does NOT follow Linux's "return the
             * old break on failure" convention, because that silently
             * looks like success to a caller that does not compare the
             * result against what it asked for. An explicit error value
             * is harder to misuse.
             *
             * Pages are not mapped here: the heap region already has
             * demand paging (task.c's fault handler maps any fault inside
             * [heap_start, heap_end) on first touch), so moving the break
             * is purely a bookkeeping change and an unused allocation
             * costs no physical memory. */
            process_t *proc = process_get_current();
            if (!proc || proc->heap_start == 0) { r->rax = (uint64_t)-1; break; }

            uint64_t requested = r->rdi;
            if (requested == 0) { r->rax = proc->heap_end; break; }

            /* Never shrink below the start, never grow past the cap, and
             * never let the heap run into the stack region. */
            if (requested < proc->heap_start) { r->rax = (uint64_t)-1; break; }
            uint64_t max_end = proc->heap_start + COS_USER_HEAP_MAX;
            if (requested > max_end) { r->rax = (uint64_t)-1; break; }
            if (!cos_heap_growth_is_safe(proc, requested)) {
                r->rax = (uint64_t)-1; break;
            }

            uint64_t new_end = (requested + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            /* Shrinking: release the physical pages that are no longer in
             * the heap, or a program that repeatedly grows and shrinks
             * would leak every page it ever touched. */
            if (new_end < proc->heap_end) {
                uint64_t pages = (proc->heap_end - new_end) / PAGE_SIZE;
                if (pages) paging_free_pages(new_end, pages);
            }
            proc->heap_end = new_end;
            r->rax = new_end;
            break;
        }

        case SYS_SBRK: {
            /* rdi = signed increment. Returns the PREVIOUS break (the
             * start of the newly-allocated region), which is what malloc
             * implementations expect, or (uint64_t)-1 on failure. */
            process_t *proc = process_get_current();
            if (!proc || proc->heap_start == 0) { r->rax = (uint64_t)-1; break; }

            int64_t delta = (int64_t)r->rdi;
            uint64_t old_end = proc->heap_end;
            if (delta == 0) { r->rax = old_end; break; }

            uint64_t target;
            if (delta > 0) {
                /* Overflow check before adding. */
                if (old_end + (uint64_t)delta < old_end) { r->rax = (uint64_t)-1; break; }
                target = old_end + (uint64_t)delta;
            } else {
                uint64_t shrink = (uint64_t)(-delta);
                if (shrink > old_end - proc->heap_start) { r->rax = (uint64_t)-1; break; }
                target = old_end - shrink;
            }

            uint64_t max_end = proc->heap_start + COS_USER_HEAP_MAX;
            if (target > max_end) { r->rax = (uint64_t)-1; break; }
            if (!cos_heap_growth_is_safe(proc, target)) {
                r->rax = (uint64_t)-1; break;
            }

            uint64_t new_end = (target + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            if (new_end < proc->heap_end) {
                uint64_t pages = (proc->heap_end - new_end) / PAGE_SIZE;
                if (pages) paging_free_pages(new_end, pages);
            }
            proc->heap_end = new_end;
            r->rax = old_end;
            break;
        }

        case SYS_READ_FILE: {
            /* rdi = path (user), rsi = buffer (user), rdx = buffer size.
             * Returns bytes read, or (uint64_t)-1. */
            uint64_t upath = r->rdi, ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }

            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), upath)) {
                r->rax = (uint64_t)-1; break;
            }
            /* The destination must be writable user memory for its whole
             * length, checked before the filesystem writes a single byte
             * into it - otherwise a bad pointer would be discovered only
             * partway through, after the kernel had already scribbled on
             * whatever was there. */
            cos_syscall_touch_user_range(ubuf, ulen, true);
            if (!paging_user_range_ok(ubuf, ulen, true)) {
                serial_puts("[SYSCALL] SYS_READ_FILE rejected: bad destination buffer\n");
                r->rax = (uint64_t)-1; break;
            }

            /* Read into a kernel bounce buffer rather than straight into
             * user memory: FatFs runs with the filesystem lock held and
             * can block, and the user mapping must not be relied on to
             * stay put across that. The copy out afterwards is a plain
             * memcpy into memory already validated above. */
            void *bounce = kmalloc((size_t)ulen);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            int got = cos_fs_read_file(path, bounce, ulen);
            if (got < 0) { kfree(bounce); r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)ubuf, bounce, (size_t)got);
            kfree(bounce);
            r->rax = (uint64_t)got;
            break;
        }

        case SYS_WRITE_FILE: {
            /* rdi = path (user), rsi = data (user), rdx = length.
             * Returns bytes written, or (uint64_t)-1. */
            uint64_t upath = r->rdi, ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }

            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), upath)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(ubuf, ulen, false);
            if (!paging_user_range_ok(ubuf, ulen, false)) {
                serial_puts("[SYSCALL] SYS_WRITE_FILE rejected: bad source buffer\n");
                r->rax = (uint64_t)-1; break;
            }

            void *bounce = kmalloc((size_t)ulen);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            memcpy(bounce, (const void *)(uintptr_t)ubuf, (size_t)ulen);
            int written = cos_fs_write_file(path, bounce, ulen);
            kfree(bounce);
            r->rax = (written < 0) ? (uint64_t)-1 : (uint64_t)written;
            break;
        }

        case SYS_YIELD: {
            scheduler_yield();
            r->rax = 0;
            break;
        }

        case SYS_SLEEP_MS: {
            /* rdi = milliseconds. Capped so a bad value cannot park a
             * process effectively forever with no way back. */
            uint64_t ms = r->rdi;
            if (ms > COS_SYSCALL_SLEEP_MAX_MS) ms = COS_SYSCALL_SLEEP_MAX_MS;
            if (ms == 0) {
                scheduler_yield();
            } else {
                scheduler_sleep(ms);
            }
            r->rax = 0;
            break;
        }

        case SYS_GETPID: {
            process_t *proc = process_get_current();
            r->rax = proc ? (uint64_t)proc->pid : (uint64_t)-1;
            break;
        }

        case SYS_SPAWN: {
            /* rdi = path to a .c-os file (user pointer).
             * Returns the new pid, or (uint64_t)-1.
             *
             * The image is read and launched entirely inside the kernel
             * via the same path the file manager's double-click uses, so
             * every validation the loader performs (ELF sanity, segment
             * overlap, user-half addresses, W^X) applies identically to a
             * program started by another program. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = (uint64_t)cos_spawn_elf_path(path);
            break;
        }

        case SYS_WAITPID: {
            /* rdi = pid to wait for (<=0 means any child),
             * rsi = optional user pointer to receive the exit status,
             * rdx = 0 means WNOHANG (check once, do not block), nonzero
             * means block until a matching child exits.
             *
             * REPLACES an earlier polling implementation that woke up
             * every COS_WAIT_POLL_MS (10ms) via scheduler_sleep() to
             * re-check, whether or not anything had changed - burning
             * scheduler cycles the whole time a child was running and
             * adding up to 10ms of pure latency after it actually exited.
             * This blocks the calling thread properly: it is taken off
             * the run queue entirely (scheduler_sleep_on(), a genuine
             * wait_queue_t block, not a timer) and is only put back on
             * the run queue by process_exit()'s explicit wake of this
             * process's child_wait_queue at the moment a child actually
             * becomes a zombie - see the field comment on
             * process_t::child_wait_queue. No polling interval, no
             * latency floor.
             *
             * This also drops the arbitrary timeout the polling version
             * had (COS_SYSCALL_SLEEP_MAX_MS) in favour of matching real
             * POSIX waitpid, which has no timeout parameter either - a
             * caller wanting a bounded wait uses a signal (already
             * implemented this session) or WNOHANG plus its own retry
             * policy, not a kernel-enforced deadline baked into the
             * syscall itself. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }

            uint64_t status_ptr = r->rsi;
            if (status_ptr != 0) cos_syscall_touch_user_range(status_ptr, sizeof(int32_t), true);
            if (status_ptr != 0 &&
                !paging_user_range_ok(status_ptr, sizeof(int32_t), true)) {
                serial_puts("[SYSCALL] SYS_WAITPID rejected: bad status pointer\n");
                r->rax = (uint64_t)-1; break;
            }

            bool wnohang = (r->rdx == 0);
            int64_t got = -1;
            int status = 0;
            for (;;) {
                got = process_try_reap_child(proc->pid, (int64_t)r->rdi, &status);
                if (got > 0) break;              /* collected */
                if (got < 0) break;              /* no such child, ever */
                if (wnohang) { got = -1; break; } /* not ready, caller asked not to block */
                /* Blocks here - does not return to this point until
                 * process_exit() (on some future child) wakes
                 * proc->child_wait_queue and the scheduler reselects this
                 * thread. Loops back to re-check afterward rather than
                 * assuming the wake was for the exact child being waited
                 * on: with rdi<=0 (any child) the first exit to arrive is
                 * always right, but with a specific rdi a DIFFERENT
                 * sibling's exit can spuriously wake this thread too
                 * (wait_queue_wake_all() wakes every waiter on the
                 * parent's queue, not just ones whose target already
                 * matches), and re-checking is what makes that safe
                 * rather than returning the wrong child or a stale
                 * status. */
                scheduler_sleep_on(&proc->child_wait_queue);
            }

            if (got > 0 && status_ptr != 0) {
                *(int32_t *)(uintptr_t)status_ptr = (int32_t)status;
            }
            r->rax = (got > 0) ? (uint64_t)got : (uint64_t)-1;
            break;
        }

        case SYS_DLOPEN: {
            /* rdi = path to a .c-osll. Returns a handle (>0) or -1. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            process_t *proc = process_get_current();
            /* RTLD_GLOBAL to preserve the old behaviour exactly: the
             * previous implementation had no concept of a local scope, so
             * every library it loaded was globally visible. Changing that
             * silently would break a program that relied on one library
             * seeing another's symbols. */
            r->rax = (uint64_t)(proc ? cos_link_dlopen(proc, path, COS_RTLD_GLOBAL)
                                     : -1);
            break;
        }

        case SYS_DLSYM: {
            /* rdi = handle from SYS_DLOPEN, rsi = symbol name (user).
             * Returns the symbol's address, or 0 (not -1: an address of 0
             * is already the natural "no such symbol" value, and callers
             * test pointers against 0). */
            char name[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(name, sizeof(name), r->rsi)) {
                r->rax = 0; break;
            }
            process_t *proc = process_get_current();
            r->rax = proc ? cos_link_dlsym(proc, (int64_t)r->rdi, name) : 0;
            break;
        }

        case SYS_DLOPEN2: {
            /* rdi = path, rsi = COS_RTLD_* flags. The flags version: the
             * original SYS_DLOPEN cannot say RTLD_LOCAL, so every library
             * necessarily joined the process-wide symbol scope and could
             * interpose on names it had no business capturing. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            process_t *proc = process_get_current();
            r->rax = (uint64_t)(proc ? cos_link_dlopen(proc, path, (uint32_t)r->rsi)
                                     : -1);
            break;
        }

        case SYS_FUTEX_WAIT:
            /* The address is validated inside cos_futex_wait(), which
             * must re-read the word with interrupts disabled anyway -
             * checking it here as well would be a TOCTOU window, not
             * extra safety. */
            r->rax = (uint64_t)cos_futex_wait(r->rdi, (uint32_t)r->rsi, r->rdx);
            break;

        case SYS_FUTEX_WAKE:
            r->rax = (uint64_t)cos_futex_wake(r->rdi, (uint32_t)r->rsi);
            break;

        case SYS_GETTID: {
            thread_t *th = thread_get_current();
            r->rax = th ? th->tid : 0;
            break;
        }

        case SYS_SPAWN2: {
            /* rdi = path, rsi = argv (NULL-terminated array of user
             * pointers), rdx = envp (same, may be NULL).
             *
             * Both vectors are copied into the kernel BEFORE the child's
             * address space is built. They live in the PARENT's address
             * space, and the launcher switches to the child's to write
             * the stack - so a pointer read after that switch would
             * resolve against the wrong address space entirely. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }

            static char argbuf[COS_SPAWN_MAX_ARGS][COS_SPAWN_MAX_ARGLEN];
            static char envbuf[COS_SPAWN_MAX_ARGS][COS_SPAWN_MAX_ARGLEN];
            const char *argv[COS_SPAWN_MAX_ARGS + 1];
            const char *envp[COS_SPAWN_MAX_ARGS + 1];

            int argc = cos_syscall_copy_strvec(r->rdi ? r->rsi : 0,
                                               argbuf, argv, COS_SPAWN_MAX_ARGS);
            int envc = cos_syscall_copy_strvec(r->rdx, envbuf, envp,
                                               COS_SPAWN_MAX_ARGS);
            if (argc < 0 || envc < 0) {
                serial_puts("[SYSCALL] SYS_SPAWN2 rejected: bad argv/envp\n");
                r->rax = (uint64_t)-1; break;
            }
            /* argv[0] defaults to the program path, which is what a
             * program reads to identify itself. */
            if (argc == 0) { argv[0] = path; argc = 1; }

            r->rax = (uint64_t)cos_spawn_elf_path_args(path, argv, argc,
                                                       envc ? envp : NULL, envc);
            break;
        }

        case SYS_DLCLOSE: {
            process_t *proc = process_get_current();
            r->rax = (uint64_t)(int64_t)(proc ? cos_link_dlclose(proc, (int64_t)r->rdi)
                                              : -1);
            break;
        }

        case SYS_DL_INFO: {
            /* Address of the read-only link map, or 0 if this process was
             * not started through the dynamic linker. */
            process_t *proc = process_get_current();
            const cos_link_ns_t *ns = proc ? cos_link_ns_peek(proc->pid) : NULL;
            r->rax = ns ? ns->linkmap_addr : 0;
            break;
        }

        case SYS_SET_TLS: {
            /* rdi = the thread pointer to install as this thread's %fs
             * base.
             *
             * Validated, not trusted: %fs.base is the origin every
             * `%fs:offset` access in ring3 is relative to, so a base in
             * the kernel half would let a ring3 instruction address
             * kernel memory directly, with no syscall involved and
             * nothing to check it. It must be a mapped, writable USER
             * address - and the ABI requires *(void**)tp == tp, so the
             * first word has to be readable too. */
            uint64_t tp = r->rdi;
            thread_t *th = thread_get_current();
            if (!th) { r->rax = (uint64_t)-1; break; }
            if (tp == 0) {
                /* Clearing is legitimate: a thread that is done with TLS
                 * says so, and the switch path then skips it. */
                scheduler_set_thread_tls(th, 0);
                r->rax = 0; break;
            }
            if (tp >= COS_ELF_USER_LIMIT || (tp & 7u) != 0) {
                serial_puts("[SYSCALL] SYS_SET_TLS rejected: thread pointer is not "
                            "an aligned user address\n");
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(tp, 8, true);
            if (!paging_user_range_ok(tp, 8, true)) {
                serial_puts("[SYSCALL] SYS_SET_TLS rejected: thread pointer is not "
                            "mapped writable\n");
                r->rax = (uint64_t)-1; break;
            }
            scheduler_set_thread_tls(th, tp);
            r->rax = 0;
            break;
        }

        case SYS_GET_TLS: {
            thread_t *th = thread_get_current();
            r->rax = th ? th->fs_base : 0;
            break;
        }

        case SYS_FD_OPEN: {
            /* rdi = path (user), rsi = COS_O_* flags. Returns fd, or -1. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = (uint64_t)cos_fd_open(cos_syscall_caller_pid(), path, (uint32_t)r->rsi);
            break;
        }

        case SYS_FD_OPENDIR: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = (uint64_t)cos_fd_opendir(cos_syscall_caller_pid(), path);
            break;
        }

        case SYS_FD_READ: {
            /* rdi = fd, rsi = user buffer, rdx = length. Returns bytes
             * read (0 at EOF), or -1. Bounded to COS_FD_IO_CHUNK_MAX
             * inside cos_fd_read() itself - a short count is a normal,
             * expected outcome here, exactly like a real read(). */
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0) { r->rax = 0; break; }
            uint64_t chunk = ulen > COS_SYSCALL_FILE_MAX ? COS_SYSCALL_FILE_MAX : ulen;
            cos_syscall_touch_user_range(ubuf, chunk, true);
            if (!paging_user_range_ok(ubuf, chunk, true)) {
                serial_puts("[SYSCALL] SYS_FD_READ rejected: bad destination buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            void *bounce = kmalloc((size_t)chunk);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            int64_t got = cos_fd_read(cos_syscall_caller_pid(), (int64_t)r->rdi, bounce, chunk);
            if (got > 0) memcpy((void *)(uintptr_t)ubuf, bounce, (size_t)got);
            kfree(bounce);
            r->rax = (uint64_t)got;
            break;
        }

        case SYS_FD_WRITE: {
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0) { r->rax = 0; break; }
            uint64_t chunk = ulen > COS_SYSCALL_FILE_MAX ? COS_SYSCALL_FILE_MAX : ulen;
            cos_syscall_touch_user_range(ubuf, chunk, false);
            if (!paging_user_range_ok(ubuf, chunk, false)) {
                serial_puts("[SYSCALL] SYS_FD_WRITE rejected: bad source buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            void *bounce = kmalloc((size_t)chunk);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            memcpy(bounce, (const void *)(uintptr_t)ubuf, (size_t)chunk);
            int64_t written = cos_fd_write(cos_syscall_caller_pid(), (int64_t)r->rdi, bounce, chunk);
            kfree(bounce);
            r->rax = (uint64_t)written;
            break;
        }

        case SYS_FD_LSEEK: {
            /* rdi = fd, rsi = absolute offset (no SEEK_CUR/SEEK_END yet -
             * see the header note). Returns the new offset, or -1. */
            r->rax = (uint64_t)cos_fd_lseek(cos_syscall_caller_pid(), (int64_t)r->rdi,
                                            (int64_t)r->rsi);
            break;
        }

        case SYS_FD_READDIR: {
            /* rdi = fd, rsi = user pointer to a cos_dirent_t.
             * Returns 1 (entry filled), 0 (end of directory), or -1. */
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_dirent_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_dirent_t), true)) {
                serial_puts("[SYSCALL] SYS_FD_READDIR rejected: bad dirent buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            cos_dirent_t entry;
            int64_t rc = cos_fd_readdir(cos_syscall_caller_pid(), (int64_t)r->rdi, &entry);
            if (rc == 1) memcpy((void *)(uintptr_t)r->rsi, &entry, sizeof(entry));
            r->rax = (uint64_t)rc;
            break;
        }

        case SYS_FD_CLOSE: {
            bool ok = cos_fd_close(cos_syscall_caller_pid(), (int64_t)r->rdi);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_SIGACTION: {
            /* rdi = signal number (1..31; 0 reserved/invalid, matching
             * real Unix convention that signal 0 is not a real signal),
             * rsi = handler address, or 0 for SIG_ACTION_DEFAULT, or 1
             * for SIG_ACTION_IGNORE (both sentinels already defined in
             * task.h, reused here so the meaning is defined in exactly
             * one place). Signal 9 is accepted here (a program is allowed
             * to WANT to catch it) but is still force-terminated at
             * delivery time in cos_deliver_pending_signal() - real Unix
             * SIGKILL also permits sigaction() to "succeed" while the
             * kernel ignores the registration when it matters. */
            process_t *proc = process_get_current();
            if (!proc || r->rdi == 0 || r->rdi >= MAX_SIGNALS) {
                r->rax = (uint64_t)-1; break;
            }
            signal_set_handler(proc, (int)r->rdi, (signal_handler_t)(uintptr_t)r->rsi);
            r->rax = 0;
            break;
        }

        case SYS_KILL: {
            /* rdi = target pid, rsi = signal number. This OS is
             * single-user, so no permission check beyond "does this pid
             * exist" is applied - any process may signal any other. */
            if (r->rsi == 0 || r->rsi >= MAX_SIGNALS) { r->rax = (uint64_t)-1; break; }
            process_t *target = process_get_by_pid(r->rdi);
            if (!target) { r->rax = (uint64_t)-1; break; }
            r->rax = (signal_send(target, (int)r->rsi) == 0) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_SIGRETURN: {
            /* No rdi/rsi arguments: this is only ever reached via the
             * trampoline this same kernel wrote, immediately after the
             * signal handler's `ret` landed on it - see the design note
             * above cos_deliver_pending_signal(). RSP at this exact point
             * is trampoline_addr, and last_sigframe_addr (stashed at
             * delivery time) is where the original register state lives. */
            process_t *proc = process_get_current();
            cos_sigframe_t frame;
            if (!proc || proc->last_sigframe_addr == 0 ||
                !cos_sig_read_user(proc->last_sigframe_addr, &frame, sizeof(frame))) {
                serial_puts("[SIGNAL] SYS_SIGRETURN with no valid pending frame - "
                            "terminating process\n");
                if (proc) process_exit(proc, -1);
                for (;;) { __asm__ volatile("hlt"); }
            }
            proc->last_sigframe_addr = 0;
            /* Restore the mask that was in effect BEFORE this signal was
             * delivered, undoing the "block everything but 9" applied at
             * delivery time - see the design note there. This is what
             * lets a signal that arrived and was correctly deferred while
             * this handler ran become deliverable again right away: the
             * fall-through to cos_deliver_pending_signal() at the end of
             * this same syscall (every syscall gets that check,
             * including this one) will now see it as no longer blocked. */
            proc->blocked_signals = frame.saved_blocked_signals;
            r->r15 = frame.r15; r->r14 = frame.r14; r->r13 = frame.r13; r->r12 = frame.r12;
            r->r11 = frame.r11; r->r10 = frame.r10; r->r9  = frame.r9;  r->r8  = frame.r8;
            r->rbp = frame.rbp; r->rdi = frame.rdi; r->rsi = frame.rsi; r->rdx = frame.rdx;
            r->rcx = frame.rcx; r->rbx = frame.rbx; r->rax = frame.rax;
            r->rip = frame.rip; r->rflags = frame.rflags; r->rsp = frame.rsp;
            /* Falls through to the normal end-of-syscall signal check
             * below with the ORIGINAL context restored, exactly as if the
             * interrupted syscall were the one returning - a second
             * signal that arrived while the first was being handled is
             * correctly picked up here rather than lost. */
            break;
        }

        case SYS_MMAP: {
            /* rdi = length, rsi = COS_PROT_* flags (READ implied always;
             * WRITE controls whether the mapping is genuinely writable -
             * see task_alloc_page()'s flag handling, fixed this session
             * to actually respect this rather than forcing every
             * demand-paged page writable regardless of what was asked).
             * Returns the mapped address, or (uint64_t)-1. No addr hint,
             * no fd/offset - anonymous-private only, see the design note
             * above cos_mmap_alloc(). */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            bool writable = (r->rsi & COS_PROT_WRITE) != 0;
            int64_t addr = cos_mmap_alloc(proc, r->rdi, writable);
            r->rax = (addr < 0) ? (uint64_t)-1 : (uint64_t)addr;
            break;
        }

        case SYS_MUNMAP: {
            /* rdi = addr, rsi = length. Must exactly match a previously
             * returned mmap() region (see cos_mmap_free()'s exact-match
             * note). Returns 0 or -1. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            bool ok = cos_mmap_free(proc, r->rdi, r->rsi);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_WIN_DRAW_TEXT: {
            /* rdi = handle, rsi = x, rdx = y, r10 = fg color, r8 = bg
             * color, r9 = user pointer to a UTF-8-ish byte string
             * (rendered byte-for-byte via vga_draw_string_len, which
             * decodes UTF-8 continuation bytes; plain ASCII is the
             * common case and needs no special handling). Length is
             * read as a NUL-terminated string up to COS_APP_MAX_TEXT_LEN
             * - there is no separate length argument, matching how a
             * small C program most naturally has a string to hand this. */
            char text[64];
            if (!cos_syscall_copy_path(text, sizeof(text), r->r9)) {
                r->rax = (uint64_t)-1; break;
            }
            uint32_t pid = cos_syscall_caller_pid();
            bool ok = cos_app_window_draw_text((int64_t)r->rdi,
                        (int32_t)(int64_t)r->rsi, (int32_t)(int64_t)r->rdx,
                        (uint32_t)r->r10, (uint32_t)r->r8, text,
                        (uint32_t)strlen(text), pid);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_WIN_POLL_KEY: {
            /* rdi = handle, rsi = user pointer to a cos_app_key_event_t
             * (2 bytes: char ascii, uint8_t special). Returns 1 if a key
             * was popped, 0 if the queue is empty (not an error - just
             * nothing typed yet), or (uint64_t)-1 for a bad handle/
             * pointer. */
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_app_key_event_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_app_key_event_t), true)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_app_key_event_t ev;
            bool got = cos_app_window_poll_key((int64_t)r->rdi, cos_syscall_caller_pid(), &ev);
            if (got) {
                memcpy((void *)(uintptr_t)r->rsi, &ev, sizeof(ev));
                r->rax = 1;
            } else {
                r->rax = 0;
            }
            break;
        }

        case SYS_THREAD_CREATE: {
            /* rdi = ring3 entry point, rsi = argument passed in RDI to
             * that entry. Returns a thread id (>0) or -1.
             *
             * The entry point is validated as executable USER memory
             * before use: it comes from ring3, and thread_create() would
             * otherwise happily start a thread at a kernel address, which
             * is the same class of bug as the old signal handler path
             * that called ring3 pointers from ring0. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            if (!paging_user_range_ok(r->rdi, 1, false)) {
                serial_puts("[SYSCALL] SYS_THREAD_CREATE rejected: entry not user memory\n");
                r->rax = (uint64_t)-1; break;
            }
            thread_t *th = thread_create(proc, (void *)(uintptr_t)r->rdi,
                                         (void *)(uintptr_t)r->rsi);
            r->rax = th ? (uint64_t)th->tid : (uint64_t)-1;
            break;
        }

        case SYS_THREAD_EXIT: {
            thread_t *cur = scheduler_get_current_thread();
            if (cur) thread_exit(cur, (int)r->rdi);
            /* thread_exit() does not return for the current thread. */
            for (;;) { __asm__ volatile("hlt"); }
        }

        case SYS_THREAD_JOIN: {
            /* rdi = thread id, rsi = timeout ms (0 = poll once).
             * Returns 0 when the thread has finished, -1 on timeout or a
             * bad id.
             *
             * Polls rather than blocking on a wait queue: unlike
             * SYS_WAITPID (which the process exit path can wake, because
             * process_exit() runs at a single well-defined point), thread
             * teardown has no equivalent single hook to wake a waiter
             * from, and adding one touches the scheduler's thread
             * lifecycle. Polling is the honest interim - stated here
             * rather than implied - and bounded by the caller's timeout
             * so it cannot spin forever. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            uint64_t timeout = r->rsi;
            if (timeout > COS_SYSCALL_SLEEP_MAX_MS) timeout = COS_SYSCALL_SLEEP_MAX_MS;
            uint64_t waited = 0;
            for (;;) {
                bool alive = false;
                for (thread_t *t = proc->thread_list; t; t = t->next) {
                    if (t->tid == (uint64_t)r->rdi &&
                        t->state != TASK_ZOMBIE && t->state != TASK_UNUSED) {
                        alive = true;
                        break;
                    }
                }
                if (!alive) { r->rax = 0; break; }
                if (waited >= timeout) { r->rax = (uint64_t)-1; break; }
                uint64_t step = (timeout - waited > 5) ? 5 : (timeout - waited);
                if (step == 0) step = 1;
                scheduler_sleep(step);
                waited += step;
            }
            break;
        }

        case SYS_TIME_MS: {
            /* TIMER_TICKS_PER_SEC is 1000 (timer.h), so ticks are already
             * milliseconds - converted explicitly anyway so this stays
             * correct if that rate is ever changed. */
            r->rax = (get_timer_ticks() * 1000ULL) / TIMER_TICKS_PER_SEC;
            break;
        }

        case SYS_TIME_UNIX: {
            r->rax = rtc_get_time();
            break;
        }

        case SYS_MKDIR: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = cos_fs_mkdir(path) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_UNLINK: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = cos_fs_unlink(path) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_RENAME: {
            char oldp[COS_SYSCALL_PATH_MAX], newp[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(oldp, sizeof(oldp), r->rdi) ||
                !cos_syscall_copy_path(newp, sizeof(newp), r->rsi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = cos_fs_rename(oldp, newp) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_STAT: {
            /* rdi = path, rsi = user pointer to a cos_stat_t.
             * Returns 0, or -1 if the path does not exist. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_stat_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_stat_t), true)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_stat_t st;
            if (!cos_fs_stat(path, &st.size, &st.is_dir)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rsi, &st, sizeof(st));
            r->rax = 0;
            break;
        }

        case SYS_NET_AVAILABLE: {
            r->rax = net_api_is_available() ? 1 : 0;
            break;
        }

        case SYS_NET_RESOLVE: {
            /* rdi = hostname (user), rsi = user pointer to 4 bytes.
             * Returns 0 on success, -1 on failure. */
            char host[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(host, sizeof(host), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(r->rsi, 4, true);
            if (!paging_user_range_ok(r->rsi, 4, true)) {
                r->rax = (uint64_t)-1; break;
            }
            uint8_t ip[4];
            if (!net_api_dns_resolve(host, ip)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rsi, ip, 4);
            r->rax = 0;
            break;
        }

        case SYS_NET_HTTP_GET: {
            /* rdi = url (user), rsi = destination buffer (user),
             * rdx = buffer size. Returns bytes received, or -1.
             *
             * Goes through a kernel bounce buffer for the same reason the
             * file syscalls do: the request blocks for a potentially long
             * time inside the network stack, and the user mapping must not
             * be relied on to stay put across that. */
            char url[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(url, sizeof(url), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(ubuf, ulen, true);
            if (!paging_user_range_ok(ubuf, ulen, true)) {
                serial_puts("[SYSCALL] SYS_NET_HTTP_GET rejected: bad destination buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            void *bounce = kmalloc((size_t)ulen);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            uint64_t got = 0;
            bool ok = net_api_http_get(url, (char *)bounce, ulen, &got);
            if (ok && got > 0) {
                if (got > ulen) got = ulen;
                memcpy((void *)(uintptr_t)ubuf, bounce, (size_t)got);
            }
            kfree(bounce);
            r->rax = ok ? got : (uint64_t)-1;
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

    /* Checked on every syscall return, including SYS_SIGRETURN's own -
     * see the comment there for why a second signal arriving during
     * handling must not be lost. */
    cos_deliver_pending_signal(r);
}

void syscall_init(void) {
    register_interrupt_handler(128, syscall_handler);
    serial_puts("[SYSCALL] int 0x80 syscall gate registered (SYS_WRITE, SYS_EXIT, SYS_WIN_*)\n");
}
