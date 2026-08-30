#ifndef TASK_H
#define TASK_H

#include "types.h"

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
typedef struct process {
    uint64_t pid;
    char name[32];
    task_type_t type;
    task_state_t state;
    uint64_t parent_pid;

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

#endif // TASK_H
