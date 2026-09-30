#ifndef TASK_H
#define TASK_H

#include "types.h"

/* Forward declaration: thread_t's full definition comes later in this
 * file (it is defined after process_t), but wait_queue_t below only
 * needs POINTERS to it, which is legal for an as-yet-incomplete type in
 * C. This lets process_t embed a wait_queue_t by value without
 * reordering the whole file, and without scheduler.h (which includes
 * this header, not the other way around - task.h cannot include
 * scheduler.h back) needing to define wait_queue_t itself. */
typedef struct thread thread_t;

/* Wait queue - moved here (rather than staying only in scheduler.h,
 * where it originally lived) specifically so process_t can embed one by
 * value (child_wait_queue, added for SYS_WAITPID's real blocking
 * implementation - see the field comment there). scheduler.h includes
 * this header and continues to declare the wait_queue_* / scheduler_block
 * / scheduler_sleep_on functions that operate on this type; only the
 * type definition itself moved, not its behaviour. */
typedef struct wait_queue {
    thread_t* head;
    thread_t* tail;
    int count;
} wait_queue_t;

// Task states
typedef enum {
    TASK_UNUSED = 0,
    TASK_CREATED,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_ZOMBIE
} task_state_t;

// Task types
typedef enum {
    TASK_TYPE_KERNEL = 0,
    TASK_TYPE_USER,
    TASK_TYPE_IDLE
} task_type_t;

// Process structure
/* Anonymous mmap regions. A bump allocator within a dedicated PML4 slot
 * (see COS_MMAP_BASE in syscall.c), not a general-purpose address-space
 * allocator: mmap_next_offset only ever increases, so munmap() releases a
 * region's physical pages and marks its slot unused, but never reclaims
 * the virtual range for reuse. That is a real, stated simplification -
 * see the design note in syscall.c - not an oversight; it is what keeps
 * region tracking a fixed array instead of a general allocator with
 * merge/split logic. */
#define COS_MMAP_MAX_REGIONS 16
typedef struct {
    uint64_t start;
    uint64_t end;
    bool     used;
    bool     writable;   /* enforced at fault time in task_handle_page_fault(),
                          * not merely accepted and ignored - a PROT_READ-only
                          * mapping is genuinely mapped without PAGE_RW. */
    bool     executable; /* Set only by SYS_MPROTECT (syscall.c), never by
                          * mmap() itself - see cos_mmap_alloc()'s own
                          * comment for why mmap() cannot create an
                          * executable mapping directly. Explicitly
                          * cleared to false in cos_mmap_alloc() for every
                          * new region, INCLUDING a reused slot - without
                          * that explicit clear, a slot reused after an
                          * earlier mmap()+mprotect(PROT_EXEC)+munmap()
                          * would silently inherit executable=true from
                          * its prior occupant, which is exactly the kind
                          * of residual-permission bug W^X exists to
                          * prevent. A region starts life as
                          * (writable=true, executable=false) or
                          * (writable=false, executable=false); SYS_MPROTECT
                          * is the only path that can ever change
                          * executable to true, and it refuses to do so
                          * while writable is also true. */
} cos_mmap_region_t;

typedef struct process {
    uint64_t pid;
    char name[32];
    task_type_t type;
    task_state_t state;
    uint64_t parent_pid;
    /* Exit status, valid once state == TASK_ZOMBIE. process_exit() used to
     * take a status and discard it ((void)status), so nothing could ever
     * learn how a process finished - waitpid() needs this preserved from
     * the moment the process dies until its parent collects it. */
    int      exit_status;
    bool     exit_status_valid;

    /* Set by cos_deliver_pending_signal() (syscall.c) when it hands a
     * process off to a signal handler, and consumed by SYS_SIGRETURN to
     * find the saved register frame on the process's own stack. Kept
     * per-process rather than derived purely from RSP arithmetic at
     * SIGRETURN time so a future change to the trampoline's layout cannot
     * silently desynchronize delivery from return. */
    uint64_t last_sigframe_addr;

    /* Anonymous mmap regions - see cos_mmap_region_t above process_t for
     * the design note. Consulted directly by task_handle_page_fault()
     * (task.c), the exact same way heap_start/heap_end already are, so an
     * mmap'd page is demand-paged in on first touch identically to heap
     * memory. */
    cos_mmap_region_t mmap_regions[COS_MMAP_MAX_REGIONS];
    uint64_t mmap_next_offset;

    /* Real blocking wait for SYS_WAITPID, replacing an earlier polling
     * implementation (10ms scheduler_sleep() between checks). Owned by
     * the PARENT: a thread calling waitpid() blocks here via
     * scheduler_sleep_on(), and process_exit() wakes THIS queue on the
     * parent it looked up via parent_pid at the moment a child becomes a
     * zombie - no polling interval, no latency floor, no wasted CPU on a
     * child that takes a long time to exit. wait_queue_wake_all() rather
     * than wake_one(): more than one thread could in principle be
     * waiting on different children of the same parent, and a spurious
     * wake is harmless - the woken thread just re-checks
     * process_try_reap_child() and blocks again if its own wait was not
     * the one satisfied. */
    wait_queue_t child_wait_queue;

    // Credentials / security context
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t umask;
    
    // Memory management
    void* page_dir;
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t stack_start;
    uint64_t stack_end;
    
    // Threading
    struct thread* thread_list;
    struct thread* main_thread;
    uint64_t thread_count;
    
    // File management
    void* files[256];
    uint64_t file_count;
    
    // Signal handling
    uint64_t pending_signals;
    uint64_t blocked_signals;
    void* signal_handlers[32];
    
    // Timing
    uint64_t created_time;
    uint64_t cpu_time;
    uint64_t time_slice;
    uint64_t context_switches;
    uint64_t page_faults;
    
    // Linked list
    struct process* next;
    struct process* prev;
} process_t;

struct wait_queue;

// Thread structure
typedef struct thread {
    uint64_t tid;
    uint64_t pid;
    task_state_t state;
    uint64_t priority;
    
    // CPU context (64-bit)
    struct {
        uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
        uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
        uint64_t rip, cs, rflags, rsp, ss;
    } context;
    
    // Stack management
    uint64_t kernel_stack;
    /* Actual size of the kernel_stack allocation. Almost always
     * KERNEL_STACK_SIZE (every thread_create() caller gets that via the
     * default), but a thread started through thread_create_stack_size()/
     * thread_create_kernel_stack_size() (see task.c - gui_main uses this to
     * get real headroom for NetSurf + QuickJS) can have a larger one. This
     * must be freed with task_free_stack(kernel_stack, kernel_stack_size),
     * not the KERNEL_STACK_SIZE constant, or a custom-sized stack would only
     * be partially freed. */
    uint64_t kernel_stack_size;
    uint8_t  fpu_state[512] __attribute__((aligned(16)));

    /* Futex state. A futex has no kernel-side object - that is the whole
     * point (see cos_futex.c) - so what identifies a waiter has to live
     * on the waiter itself.
     *
     * futex_addr is the user address this thread is parked on, used at
     * wake time to tell it apart from other waiters sharing a hash
     * bucket. futex_woken distinguishes a real wake from a timeout,
     * which the caller genuinely needs to know. futex_deadline is
     * absolute rather than a countdown, because a spuriously woken
     * thread re-blocks and a countdown would restart each time. */
    uint64_t futex_addr;
    uint64_t futex_deadline;
    bool     futex_woken;

    /* Thread-local storage pointer: the value of the IA32_FS_BASE MSR
     * this thread runs with.
     *
     * This has to be per-THREAD and switched with the rest of the
     * context, not per-process: every thread has its own TLS block, and
     * that is the entire point of thread-local storage. Leaving it out
     * is why `__thread` and anything built on it (errno in a real libc,
     * a per-thread arena in an allocator) could not work here before -
     * the loader can build a TLS block, but if %fs.base is never
     * restored on a switch, every thread reads whichever block happened
     * to be installed last.
     *
     * Zero means "this thread has no TLS"; the switch path then leaves
     * the MSR alone rather than writing 0 into it, so a kernel thread
     * costs nothing. */
    uint64_t fs_base;
    
    // Scheduling
    uint64_t time_slice;
    
    // Scheduler / wait-queue linkage (separate from process lists)
    struct wait_queue* blocked_on;
    struct thread* next;
    struct thread* prev;
    struct thread* proc_next;
    struct thread* proc_prev;
} thread_t;

// Signal handling
typedef void (*signal_handler_t)(int);
typedef uint64_t cos_sigset_t;

#define MAX_SIGNALS 32
#define MAX_TASKS 256
#define MAX_THREADS 1024
/* One uniform 512KiB kernel stack per C-OS execution context.  This removes
 * the old 8KiB default that was insufficient for browser, TLS and filesystem
 * call depth while retaining page-aligned allocation and precise teardown. */
#define KERNEL_STACK_SIZE (512u * 1024u)

// Signal actions
#define SIG_ACTION_DEFAULT ((signal_handler_t)0)
#define SIG_ACTION_IGNORE ((signal_handler_t)1)

// Task management functions
void task_init(void);
void task_init_idle(void);

process_t* process_create(const char* name, task_type_t type);
void process_exit(process_t* proc, int status);
void process_destroy(process_t* proc);
void task_reap_zombies(void); /* called from the idle loop; see task.c */

/* Non-blocking waitpid(). Returns the collected child's pid (writing its
 * exit status), 0 if a matching child exists but has not exited, or -1 if
 * there is no such child - so "not yet" and "never" are distinguishable. */
int64_t process_try_reap_child(uint64_t parent_pid, int64_t want_pid, int* out_status);
process_t* process_get_current(void);
void process_set_current(process_t* proc);
process_t* process_get_by_pid(uint64_t pid);
int process_get_slot_index(uint64_t pid); /* stable table-slot key, see task.c */
void process_set_state(process_t* proc, task_state_t state);

thread_t* thread_create(process_t* proc, void* entry_point, void* arg);
/* Same as thread_create(), but with an explicit kernel stack size instead of
 * the KERNEL_STACK_SIZE default. For threads that run substantially more
 * native C call depth than a typical kernel worker - see gui_main in
 * kernel.c, which drives NetSurf's HTML/CSS pipeline and QuickJS. */
thread_t* thread_create_stack_size(process_t* proc, void* entry_point, void* arg,
                                   size_t stack_size);
thread_t* thread_create_kernel(const char* name, void* entry, void* arg);
thread_t* thread_create_kernel_stack_size(const char* name, void* entry, void* arg,
                                          size_t stack_size);
void thread_exit(thread_t* thread, int code);
void thread_destroy(thread_t* thread);
thread_t* thread_get_current(void);
thread_t* thread_get_by_tid(uint64_t tid);
void thread_set_state(thread_t* thread, task_state_t state);

uint64_t task_alloc_stack(size_t size);
void task_free_stack(uint64_t stack_base, size_t size);


void thread_yield(void);
void thread_sleep(uint64_t ms);
void thread_wake(thread_t* thread);

int signal_send(process_t* proc, int sig);
int signal_send_thread(thread_t* thread, int sig);
void signal_set_handler(process_t* proc, int sig, signal_handler_t handler);
void signal_block(process_t* proc, int sig);
void signal_unblock(process_t* proc, int sig);
void signal_process_pending(void);

bool task_handle_page_fault(uint64_t fault_addr, uint64_t error_code);
int task_clone_memory(process_t* parent, process_t* child);
bool task_alloc_page(process_t* proc, uint64_t virt_addr, uint64_t flags);
void task_free_page(process_t* proc, uint64_t virt_addr);

int task_get_count(void);
void task_dump(process_t* proc);
void task_dump_all(void);
process_t* task_get_first(void);
process_t* task_get_next(process_t* proc);

/* Starting a program with arguments.
 *
 * cos_spawn_elf_path() and cos_launch_elf_image() are the original
 * argument-less forms, kept working. The _args variants exist because
 * there was previously NO way for a program to pass argv or an
 * environment to a child it started - the kernel could build a full
 * System V stack, but nothing above it had parameters to fill one from,
 * so every spawned process saw argc == 1 and an empty environment. */
int64_t cos_spawn_elf_path_args(const char *path, const char *const *argv,
                                int argc, const char *const *envp, int envc);
int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image,
                                  unsigned int image_len,
                                  const char *const *argv, int argc,
                                  const char *const *envp, int envc);

#endif // TASK_H
