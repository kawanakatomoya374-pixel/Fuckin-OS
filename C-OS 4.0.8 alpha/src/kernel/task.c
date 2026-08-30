/**
 * task.c - Process and Thread Management
 *
 * Minimal but functional process / thread model for the kernel.
 */

#include "task.h"
#include "scheduler.h"
#include "memory.h"
#include "mm/paging.h"
#include "gdt.h"
#include "serial.h"
#include "timer.h"
#include "sync.h"

void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t num);
char* strncpy(char* dest, const char* src, size_t n);
void serial_puthex(uint64_t n);
void serial_putdec(uint64_t n);
uint64_t paging_alloc_pages(uint64_t count, uint64_t flags);
void paging_free_pages(uint64_t virtual_addr, uint64_t count);
uint64_t paging_alloc_physical(void);
void paging_free_physical(uint64_t phys_addr);
bool paging_setup_user_stack(page_directory_t *dir, uint64_t top, uint64_t pages);
void task_entry_wrapper(void);
void fs_unified_close_process_files(uint64_t owner_pid);
void ipc_release_process_resources(uint64_t owner_pid);

static process_t process_table[MAX_TASKS];
static thread_t thread_table[MAX_THREADS];
static process_t* process_list = NULL;
static process_t* zombie_list = NULL;
static process_t* idle_process = NULL;
static thread_t* idle_thread = NULL;
static process_t* current_process = NULL;
static uint64_t next_pid = 1;
static uint64_t next_tid = 1;

#define TASK_ROOT_UID 0u
#define TASK_ROOT_GID 0u
#define TASK_ROOT_UMASK 0022u

/* Keep user mappings outside the low identity-mapped kernel image and
 * NetSurf compatibility runway. The former 64–128 MiB defaults could collide
 * with dynamically mapped physical frames during real HTML/CSS processing. */
#define USER_STACK_TOP_DEFAULT   0x0000000040000000ULL
#define USER_STACK_PAGES_DEFAULT 8ULL
#define USER_HEAP_START_DEFAULT  0x0000000010000000ULL
#define USER_HEAP_SIZE_DEFAULT   0x0000000000100000ULL

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

static void process_init_credentials(process_t* proc, task_type_t type) {
    if (!proc) return;

    process_t* parent = current_process;
    if (type == TASK_TYPE_KERNEL || type == TASK_TYPE_IDLE || !parent) {
        proc->uid = TASK_ROOT_UID;
        proc->gid = TASK_ROOT_GID;
        proc->euid = TASK_ROOT_UID;
        proc->egid = TASK_ROOT_GID;
        proc->umask = TASK_ROOT_UMASK;
        return;
    }

    proc->uid = parent->uid;
    proc->gid = parent->gid;
    proc->euid = parent->euid;
    proc->egid = parent->egid;
    proc->umask = parent->umask;
}

static void release_user_process_pages(process_t* proc) {
    if (!proc || proc->type != TASK_TYPE_USER || !proc->page_dir) return;

    uint64_t flags = sync_irq_save();
    page_directory_t* saved = paging_get_current_directory();
    paging_switch_directory((page_directory_t*)proc->page_dir);

    if (proc->heap_end > proc->heap_start) {
        uint64_t heap_pages = (proc->heap_end - proc->heap_start + PAGE_SIZE - 1) / PAGE_SIZE;
        paging_free_pages(proc->heap_start, heap_pages);
    }
    if (proc->stack_end > proc->stack_start) {
        uint64_t stack_pages = (proc->stack_end - proc->stack_start + PAGE_SIZE - 1) / PAGE_SIZE;
        paging_free_pages(proc->stack_start, stack_pages);
    }

    if (saved) paging_switch_directory(saved);
    sync_irq_restore(flags);
}

static void destroy_threads_for_process(process_t* proc) {
    if (!proc) return;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (thread_table[i].state != TASK_UNUSED && thread_table[i].pid == proc->pid) {
            thread_destroy(&thread_table[i]);
        }
    }
}

static bool process_has_live_threads_except(process_t* proc, thread_t* except) {
    if (!proc) return false;
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        thread_t* th = &thread_table[i];
        if (th->state == TASK_UNUSED || th->state == TASK_ZOMBIE) continue;
        if (th->pid != proc->pid) continue;
        if (th == except) continue;
        return true;
    }
    return false;
}

static void detach_thread_from_process(process_t* proc, thread_t* thread) {
    if (!proc || !thread) return;
    if (thread->proc_prev) thread->proc_prev->proc_next = thread->proc_next;
    else if (proc->thread_list == thread) proc->thread_list = thread->proc_next;
    if (thread->proc_next) thread->proc_next->proc_prev = thread->proc_prev;
    if (proc->main_thread == thread) {
        proc->main_thread = proc->thread_list;
    }
    if (proc->thread_count > 0) {
        proc->thread_count--;
    }
    thread->proc_next = NULL;
    thread->proc_prev = NULL;
}

static const char* task_state_name(task_state_t state) {
    switch (state) {
        case TASK_UNUSED:   return "unused";
        case TASK_CREATED:  return "created";
        case TASK_READY:    return "ready";
        case TASK_RUNNING:  return "running";
        case TASK_BLOCKED:  return "blocked";
        case TASK_SLEEPING: return "sleeping";
        case TASK_ZOMBIE:   return "zombie";
        default:            return "unknown";
    }
}

static process_t* alloc_process_slot(void) {
    /* Scanning for a free slot and claiming it (marking it non-UNUSED
     * via the memset below) has to happen as one atomic step. Without
     * this, two threads calling process_create()/thread_create() back
     * to back - entirely possible now that the scheduler can actually
     * switch between them cooperatively or preemptively instead of
     * everything running strictly sequentially on the boot stack -
     * could both see the same TASK_UNUSED slot free and both start
     * writing into it, corrupting whichever process loses the race. */
    uint64_t flags = sync_irq_save();
    for (size_t i = 0; i < MAX_TASKS; ++i) {
        if (process_table[i].state == TASK_UNUSED) {
            memset(&process_table[i], 0, sizeof(process_table[i]));
            process_table[i].pid = next_pid++;
            process_table[i].state = TASK_CREATED;
            sync_irq_restore(flags);
            return &process_table[i];
        }
    }
    sync_irq_restore(flags);
    return NULL;
}

static thread_t* alloc_thread_slot(void) {
    uint64_t flags = sync_irq_save();
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (thread_table[i].state == TASK_UNUSED) {
            memset(&thread_table[i], 0, sizeof(thread_table[i]));
            thread_table[i].tid = next_tid++;
            thread_table[i].state = TASK_CREATED;
            sync_irq_restore(flags);
            return &thread_table[i];
        }
    }
    sync_irq_restore(flags);
    return NULL;
}

static void idle_task_entry(void* arg) {
    (void)arg;
    for (;;) {
        /* Safe reap point: see task_reap_zombies()'s comment for why
         * idle is exactly the right place to do this. */
        task_reap_zombies();
        __asm__ volatile("sti; hlt");
    }
}

void task_init_idle(void);

void task_init(void) {
    serial_puts("[TASK] init\n");
    /* process_table/thread_table live in the ELF BSS, which the UEFI GRUB
     * loader has already zeroed before entering the kernel. Re-clearing the
     * large tables here was both redundant and, on the signed standalone GRUB
     * path, exposed an early page-table fault before the first task existed. */
    process_list = NULL;
    zombie_list = NULL;
    idle_process = NULL;
    idle_thread = NULL;
    current_process = NULL;
    next_pid = 1;
    next_tid = 1;
    task_init_idle();
}
void task_init_idle(void) {
    if (idle_process) return;

    idle_process = process_create("[idle]", TASK_TYPE_IDLE);
    if (!idle_process) {
        serial_puts("[TASK] idle process create failed\n");
        return;
    }

    /* Keep the idle thread inside the idle process, not a separate process. */
    idle_thread = thread_create(idle_process, (void*)idle_task_entry, NULL);
    if (!idle_thread) {
        serial_puts("[TASK] idle thread create failed\n");

        /* Roll back the partially constructed idle process so boot can
         * still continue in a well-defined failure mode. */
        if (process_list == idle_process) {
            process_list = idle_process->next;
        }
        if (idle_process->prev) {
            idle_process->prev->next = idle_process->next;
        }
        if (idle_process->next) {
            idle_process->next->prev = idle_process->prev;
        }
        if (idle_process->page_dir) {
            paging_destroy_directory(idle_process->page_dir);
            idle_process->page_dir = NULL;
        }
        memset(idle_process, 0, sizeof(*idle_process));
        idle_process = NULL;
        return;
    }

    idle_process->main_thread = idle_thread;
    idle_thread->state = TASK_READY;
    if (!current_process) {
        current_process = idle_process;
    }
}

process_t* process_create(const char* name, task_type_t type) {
    process_t* proc = alloc_process_slot();
    if (!proc) return NULL;

    strncpy(proc->name, name ? name : "unnamed", sizeof(proc->name) - 1);
    proc->name[sizeof(proc->name) - 1] = '\0';
    proc->type = type;
    proc->state = TASK_CREATED;
    proc->parent_pid = current_process ? current_process->pid : 0;
    process_init_credentials(proc, type);

    proc->page_dir = (type == TASK_TYPE_KERNEL) ? NULL : paging_create_directory();
    if (type != TASK_TYPE_KERNEL && !proc->page_dir) {
        proc->state = TASK_UNUSED;
        return NULL;
    }

    proc->heap_start = (type == TASK_TYPE_USER) ? USER_HEAP_START_DEFAULT : 0;
    proc->heap_end = (type == TASK_TYPE_USER) ? (USER_HEAP_START_DEFAULT + USER_HEAP_SIZE_DEFAULT) : 0;
    proc->stack_end = (type == TASK_TYPE_USER) ? USER_STACK_TOP_DEFAULT : 0;
    proc->stack_start = (type == TASK_TYPE_USER) ? (USER_STACK_TOP_DEFAULT - USER_STACK_PAGES_DEFAULT * PAGE_SIZE) : 0;
    proc->thread_list = NULL;
    proc->main_thread = NULL;
    proc->thread_count = 0;
    memset(proc->files, 0, sizeof(proc->files));
    proc->file_count = 0;
    proc->pending_signals = 0;
    proc->blocked_signals = 0;
    for (size_t i = 0; i < MAX_SIGNALS; ++i) {
        proc->signal_handlers[i] = SIG_ACTION_DEFAULT;
    }
    proc->created_time = get_timer_ticks();
    proc->cpu_time = 0;
    proc->time_slice = 0;
    proc->context_switches = 0;
    proc->page_faults = 0;

    if (proc->type == TASK_TYPE_USER && proc->page_dir) {
        /* current_directory is a single global that paging_switch_directory()
         * writes straight into CR3 - it's not per-thread state, it's
         * literally "which address space the CPU is running under right
         * now", for every piece of code including interrupt handlers.
         * If a timer tick preempts us between the switch-in and
         * switch-back below, whatever runs next (another thread, an
         * IRQ handler) does so under *this* process's address space
         * instead of its own until we get scheduled back - wrong data,
         * wrong mappings, or an instant fault. Has to be atomic. */
        uint64_t dir_flags = sync_irq_save();
        page_directory_t* saved = paging_get_current_directory();
        paging_switch_directory((page_directory_t*)proc->page_dir);
        if (!paging_setup_user_stack((page_directory_t*)proc->page_dir, proc->stack_end, USER_STACK_PAGES_DEFAULT)) {
            if (saved) { paging_switch_directory(saved); }
            sync_irq_restore(dir_flags);
            release_user_process_pages(proc);
            paging_destroy_directory(proc->page_dir);
            proc->page_dir = NULL;
            proc->state = TASK_UNUSED;
            return NULL;
        }
        if (saved) { paging_switch_directory(saved); }
        sync_irq_restore(dir_flags);
    }

    uint64_t list_flags = sync_irq_save();
    proc->next = process_list;
    proc->prev = NULL;
    if (process_list) {
        process_list->prev = proc;
    }
    process_list = proc;

    if (!current_process) {
        current_process = proc;
    }
    sync_irq_restore(list_flags);

    return proc;
}

void process_exit(process_t* proc, int status) {
    (void)status;
    if (!proc || proc->state == TASK_UNUSED) return;

    proc->state = TASK_ZOMBIE;

    uint64_t list_flags = sync_irq_save();

    /* Detach threads; the scheduler may still be running one of them.
     * Marking a thread ZOMBIE is not enough on its own: pick_next_task()
     * and scheduler_switch_task() never look at thread->state, they just
     * pull whatever is at the head of its run queue. Any thread left in
     * a run queue keeps getting scheduled even after being marked dead,
     * which - combined with freeing the page directory below - means a
     * zombie thread can resume execution inside memory that no longer
     * has valid page-table mappings. Explicitly pull every thread out of
     * the scheduler first. */
    thread_t* th = proc->thread_list;
    thread_t* self = scheduler_get_current_thread();
    bool exiting_self = false;
    while (th) {
        thread_t* next_th = th->proc_next;
        th->state = TASK_ZOMBIE;
        if (th == self) {
            /* Can't fully remove/destroy the thread we are currently
             * running as - just take it out of the run queue so it will
             * never be picked again once we switch away. */
            exiting_self = true;
        }
        scheduler_remove_task(th);
        th = next_th;
    }

    /* Only free the address space immediately if we are not currently
     * executing inside it. Freeing page_dir while a thread of this very
     * process is the one running would yank the page tables out from
     * under our own instruction/stack fetches. If this process is
     * exiting itself, defer the free: the thread is already removed
     * from every run queue above, so it can never be scheduled again,
     * and task_reap_zombies() (called from the idle loop, see task.c)
     * will free proc->page_dir the next time the system goes idle -
     * which is guaranteed to happen before this process could ever run
     * again, since it has no threads left in any run queue. */
    if (proc->page_dir && !exiting_self) {
        release_user_process_pages(proc);
        paging_destroy_directory(proc->page_dir);
        proc->page_dir = NULL;
    }

    if (proc->prev) proc->prev->next = proc->next;
    else process_list = proc->next;
    if (proc->next) proc->next->prev = proc->prev;

    proc->next = zombie_list;
    proc->prev = NULL;
    zombie_list = proc;

    if (current_process == proc) {
        current_process = idle_process;
    }

    /* Release per-process resources while the task is still known to be
     * exiting. These helpers operate on owner pid / per-process indices
     * rather than the current CPU context, so they remain valid even if
     * this is the last live thread. */
    fs_unified_close_process_files(proc->pid);
    ipc_release_process_resources(proc->pid);

    sync_irq_restore(list_flags);

    if (exiting_self) {
        /* Never return to a thread that has already been detached from
         * every run queue. If the scheduler has not switched us away by
         * the time this returns, force a reschedule and then trap as a
         * last-resort safety net instead of parking the CPU forever. */
        scheduler_preempt();
        __builtin_trap();
    }
}

void process_destroy(process_t* proc) {
    if (!proc) return;
    process_exit(proc, 0);
    task_reap_zombies();
}

/* Reap every process on the zombie list: free any page directory that
 * process_exit() had to leave behind (the "exiting_self" deferred-free
 * case - see the comment there), then recycle the process_table slot
 * by marking it TASK_UNUSED so alloc_process_slot() can reuse it.
 *
 * This is only safe to call from a context where NOTHING is currently
 * executing inside any zombie process's address space. The idle
 * thread is exactly such a context: pick_next_task() only ever falls
 * back to it when every run queue is empty, and every zombie's
 * threads were already pulled out of the run queues back in
 * process_exit() - so by the time idle runs, none of them can
 * possibly be "the thing we're currently running as" anymore,
 * regardless of which zombie deferred its own cleanup. */
void task_reap_zombies(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");

    process_t* proc = zombie_list;
    while (proc) {
        process_t* next = proc->next;

        destroy_threads_for_process(proc);

        if (proc->page_dir) {
            release_user_process_pages(proc);
            paging_destroy_directory(proc->page_dir);
            proc->page_dir = NULL;
        }

        proc = next;
    }

    /* All reaped - the whole list is now free to recycle. */
    while (zombie_list) {
        process_t* p = zombie_list;
        zombie_list = p->next;
        p->state = TASK_UNUSED;
        p->thread_list = NULL;
        p->main_thread = NULL;
        p->next = NULL;
        p->prev = NULL;
    }

    __asm__ volatile("push %0\n\tpopfq" :: "r"(flags) : "memory", "cc");
}

process_t* process_get_current(void) {
    thread_t* th = scheduler_get_current_thread();
    if (th) {
        process_t* p = process_get_by_pid(th->pid);
        if (p) return p;
    }
    return current_process;
}

void process_set_current(process_t* proc) {
    current_process = proc;
}

process_t* process_get_by_pid(uint64_t pid) {
    for (size_t i = 0; i < MAX_TASKS; ++i) {
        if (process_table[i].state != TASK_UNUSED && process_table[i].pid == pid) {
            return &process_table[i];
        }
    }
    return NULL;
}

/* Returns the stable process_table slot index (0..MAX_TASKS-1) for a
 * live pid, or -1 if not found. Unlike the pid itself (which only ever
 * increases and is never reused - see next_pid in alloc_process_slot),
 * this index is a small, table-bounded key that's safe to use directly
 * as an array index (e.g. ipc.c's per-process mailbox table) without
 * needing a hash or risking collisions between two live processes. */
int process_get_slot_index(uint64_t pid) {
    for (int i = 0; i < MAX_TASKS; ++i) {
        if (process_table[i].state != TASK_UNUSED && process_table[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

void process_set_state(process_t* proc, task_state_t state) {
    if (proc) proc->state = state;
}

thread_t* thread_create(process_t* proc, void* entry_point, void* arg) {
    return thread_create_stack_size(proc, entry_point, arg, KERNEL_STACK_SIZE);
}

thread_t* thread_create_stack_size(process_t* proc, void* entry_point, void* arg,
                                   size_t stack_size) {
    if (!proc) return NULL;

    thread_t* th = alloc_thread_slot();
    if (!th) return NULL;

    th->pid = proc->pid;
    th->state = TASK_CREATED;
    th->priority = (proc->type == TASK_TYPE_IDLE) ? SCHED_PRIO_IDLE : SCHED_PRIO_DEFAULT;
    th->kernel_stack = task_alloc_stack(stack_size);
    if (!th->kernel_stack) {
        th->state = TASK_UNUSED;
        return NULL;
    }
    /* task_alloc_stack() rounds up to a whole number of pages; remember
     * the size it actually committed (not the raw request) so the matching
     * task_free_stack() call releases exactly what was allocated. */
    th->kernel_stack_size = align_up_u64(stack_size, PAGE_SIZE);

    memset(&th->context, 0, sizeof(th->context));
    memset(th->fpu_state, 0, sizeof(th->fpu_state));
    __asm__ volatile("fninit\n\tfxsave64 %0" : "=m"(th->fpu_state) :: "memory");
    th->context.rdi = (uint64_t)arg;
    th->context.rax = (uint64_t)entry_point;
    th->context.rip = (proc->type == TASK_TYPE_USER)
        ? (uint64_t)entry_point
        : (uint64_t)task_entry_wrapper;
    /* User threads must return through iretq with ring-3 selectors and
     * a user stack; kernel threads remain on the kernel stack and return
     * normally via retq. */
    if (proc->type == TASK_TYPE_USER) {
        th->context.cs = GDT_USER_CODE;
        th->context.ss = GDT_USER_DATA;
        th->context.rsp = align_up_u64(proc->stack_end, 16ULL) - 8ULL;
    } else {
        th->context.cs = GDT_KERNEL_CODE;
        th->context.ss = GDT_KERNEL_DATA;
        th->context.rsp = th->kernel_stack;
    }
    th->context.rflags = 0x202;

    uint64_t list_flags = sync_irq_save();
    th->proc_next = proc->thread_list;
    th->proc_prev = NULL;
    if (proc->thread_list) {
        proc->thread_list->proc_prev = th;
    }
    proc->thread_list = th;
    proc->thread_count++;
    if (!proc->main_thread) {
        proc->main_thread = th;
    }
    sync_irq_restore(list_flags);

    th->time_slice = SCHED_TIMESLICE_DEFAULT;
    th->state = TASK_READY;

    /* Register with the scheduler. Without this, a thread could be
     * fully constructed and marked TASK_READY yet sit in no run queue
     * at all - scheduler_add_task()/pick_next_task() only look at the
     * run queues, not at thread_table, so nothing would ever actually
     * pick this thread up. The idle thread is the one exception: it is
     * wired in via scheduler_set_idle() as pick_next_task()'s fallback
     * for "nothing else is ready", not queued alongside normal work. */
    if (proc->type == TASK_TYPE_IDLE) {
        scheduler_set_idle(th);
    } else {
        scheduler_add_task(th);
    }

    scheduler_note_task_created();

    return th;
}

thread_t* thread_create_kernel(const char* name, void* entry, void* arg) {
    process_t* proc = process_create(name, TASK_TYPE_KERNEL);
    if (!proc) return NULL;

    thread_t* thread = thread_create(proc, entry, arg);
    if (!thread) {
        process_destroy(proc);
        task_reap_zombies();
        return NULL;
    }
    return thread;
}

thread_t* thread_create_kernel_stack_size(const char* name, void* entry, void* arg,
                                          size_t stack_size) {
    process_t* proc = process_create(name, TASK_TYPE_KERNEL);
    if (!proc) return NULL;

    thread_t* thread = thread_create_stack_size(proc, entry, arg, stack_size);
    if (!thread) {
        process_destroy(proc);
        task_reap_zombies();
        return NULL;
    }
    return thread;
}

thread_t* thread_get_current(void) {
    return scheduler_get_current_thread();
}

thread_t* thread_get_by_tid(uint64_t tid) {
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        if (thread_table[i].state != TASK_UNUSED && thread_table[i].tid == tid) {
            return &thread_table[i];
        }
    }
    return NULL;
}

void thread_set_state(thread_t* thread, task_state_t state) {
    if (thread) thread->state = state;
}

void thread_exit(thread_t* thread, int code) {
    if (!thread) return;

    process_t* proc = process_get_by_pid(thread->pid);

    /* If this is the last live thread in the process, tear the whole
     * process down instead of leaving a zombie process with no runnable
     * threads. This is especially important for the task entry wrapper:
     * when the entry function returns, the thread should exit cleanly,
     * and a one-thread process must become a zombie process so the idle
     * reaper can reclaim it. */
    if (proc && !process_has_live_threads_except(proc, thread)) {
        process_exit(proc, code);
        if (thread == scheduler_get_current_thread()) {
            /* process_exit marks the current thread/process dead but does not
             * itself select the next runnable task.  Trapping here left the
             * CPU in the terminated ring3 context, starving gui_main and the
             * browser event loop.  Force the normal scheduler handoff; the
             * call should not return, but keep a safe halt fallback for a
             * pathological empty run queue. */
            scheduler_preempt();
            for (;;) { __asm__ volatile("sti; hlt"); }
        }
        return;
    }

    uint64_t flags = sync_irq_save();

    scheduler_remove_task(thread);
    thread->state = TASK_ZOMBIE;

    if (proc) {
        detach_thread_from_process(proc, thread);
    }

    sync_irq_restore(flags);

    if (thread == scheduler_get_current_thread()) {
        scheduler_preempt();
        __builtin_trap();
    }
}

void thread_destroy(thread_t* thread) {
    if (!thread) return;
    scheduler_remove_task(thread);
    uint64_t list_flags = sync_irq_save();
    process_t* proc = process_get_by_pid(thread->pid);
    if (proc) {
        bool linked = (proc->thread_list == thread) || thread->proc_prev || thread->proc_next;
        if (linked) {
            if (thread->proc_prev) thread->proc_prev->proc_next = thread->proc_next;
            else proc->thread_list = thread->proc_next;
            if (thread->proc_next) thread->proc_next->proc_prev = thread->proc_prev;
            if (proc->main_thread == thread) {
                proc->main_thread = proc->thread_list;
            }
            if (proc->thread_count) {
                proc->thread_count--;
            }
        }
    }
    sync_irq_restore(list_flags);

    if (thread->kernel_stack) {
        uint64_t dir_flags = sync_irq_save();
        page_directory_t* saved = paging_get_current_directory();
        if (proc && proc->page_dir) {
            paging_switch_directory((page_directory_t*)proc->page_dir);
        }
        task_free_stack(thread->kernel_stack, thread->kernel_stack_size);
        if (saved) {
            paging_switch_directory(saved);
        }
        sync_irq_restore(dir_flags);
        thread->kernel_stack = 0;
    }
    thread->blocked_on = NULL;
    thread->proc_next = NULL;
    thread->proc_prev = NULL;
    thread->state = TASK_UNUSED;

    scheduler_note_task_destroyed();
}

uint64_t task_alloc_stack(size_t size) {
    size = align_up_u64(size, PAGE_SIZE);
    uint64_t pages = size / PAGE_SIZE;
    uint64_t base = paging_alloc_pages(pages, PAGE_PRESENT | PAGE_RW);
    return base ? (base + size) : 0;
}

void task_free_stack(uint64_t stack_base, size_t size) {
    if (!stack_base) return;
    size = align_up_u64(size, PAGE_SIZE);
    uint64_t virt_base = stack_base - size;
    paging_free_pages(virt_base, size / PAGE_SIZE);
}



void thread_yield(void) {
    scheduler_yield();
}

void thread_sleep(uint64_t ms) {
    scheduler_sleep(ms);
}

void thread_wake(thread_t* thread) {
    if (thread && thread->state == TASK_SLEEPING) {
        scheduler_wake_thread(thread);
    }
}

int signal_send(process_t* proc, int sig) {
    if (!proc || sig < 0 || sig >= MAX_SIGNALS) return -1;
    proc->pending_signals |= (1ULL << sig);
    return 0;
}

int signal_send_thread(thread_t* thread, int sig) {
    if (!thread) return -1;
    process_t* proc = process_get_by_pid(thread->pid);
    return proc ? signal_send(proc, sig) : -1;
}

void signal_set_handler(process_t* proc, int sig, signal_handler_t handler) {
    if (!proc || sig < 0 || sig >= MAX_SIGNALS) return;
    proc->signal_handlers[sig] = handler;
}

void signal_block(process_t* proc, int sig) {
    if (!proc || sig < 0 || sig >= MAX_SIGNALS) return;
    proc->blocked_signals |= (1ULL << sig);
}

void signal_unblock(process_t* proc, int sig) {
    if (!proc || sig < 0 || sig >= MAX_SIGNALS) return;
    proc->blocked_signals &= ~(1ULL << sig);
}

static void signal_default_handler(int sig) {
    serial_puts("[TASK] signal ");
    serial_putdec((uint64_t)sig);
    serial_puts("\n");
    process_t* proc = process_get_current();
    if (sig == 9 && proc) {
        process_exit(proc, 1);
    }
}

void signal_process_pending(void) {
    process_t* proc = process_get_current();
    if (!proc) return;

    cos_sigset_t pending = proc->pending_signals & ~proc->blocked_signals;
    for (int sig = 0; sig < MAX_SIGNALS; ++sig) {
        if (pending & (1ULL << sig)) {
            signal_handler_t handler = proc->signal_handlers[sig];
            if (handler == SIG_ACTION_DEFAULT) {
                signal_default_handler(sig);
            } else if (handler != SIG_ACTION_IGNORE) {
                handler(sig);
            }
            proc->pending_signals &= ~(1ULL << sig);
        }
    }
}

bool task_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    process_t* proc = process_get_current();
    if (!proc) return FALSE;

    serial_puts("[TASK] page fault @ 0x");
    serial_puthex(fault_addr);
    serial_puts(" pid=");
    serial_putdec(proc->pid);
    serial_puts("\n");
    proc->page_faults++;

    /* Recover only from not-present faults in user space. Protection
     * violations still need to crash loudly so real bugs are not hidden.
     * We support two small recovery cases here:
     *   1) lazily materialize a page inside the user heap window
     *   2) grow the user stack by one page when the fault is just below
     *      the current mapped bottom
     */
    /* Only recover faults that actually came from user-mode accesses.
     * Kernel-mode faults should still surface so real bugs are not
     * hidden behind demand-paging heuristics. */
    if (proc->type != TASK_TYPE_USER || (error_code & 0x1ULL) || !(error_code & 0x4ULL)) {
        return FALSE;
    }

    uint64_t page = fault_addr & ~(PAGE_SIZE - 1ULL);

    if (proc->heap_start && proc->heap_end && page >= proc->heap_start && page < proc->heap_end) {
        return task_alloc_page(proc, page, PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    if (proc->stack_start && proc->stack_end) {
        uint64_t grow_floor = (proc->stack_start > PAGE_SIZE) ? (proc->stack_start - PAGE_SIZE) : 0;
        if (fault_addr >= grow_floor && fault_addr < proc->stack_end) {
            return task_alloc_page(proc, page, PAGE_PRESENT | PAGE_RW | PAGE_USER);
        }
    }

    return FALSE;
}

int task_clone_memory(process_t* parent, process_t* child) {
    if (!parent || !child) return 0;

    child->page_dir = paging_create_directory();
    if (!child->page_dir) return 0;

    /* Everything below repeatedly flips CR3 between parent's and
     * child's address space via paging_switch_directory(). That's
     * global CPU state, not per-thread state, so this whole dance has
     * to run as one atomic block - see the comment in process_create()
     * above for why a preemption mid-switch is dangerous. This also
     * means paging_alloc_physical()/paging_map_page() below run with
     * interrupts already off; that's fine, they don't block. */
    uint64_t dir_flags = sync_irq_save();

    page_directory_t* saved = paging_get_current_directory();
    if (parent->page_dir) {
        paging_switch_directory((page_directory_t*)parent->page_dir);
    }
    if (child->type == TASK_TYPE_USER) {
        child->heap_start = parent->heap_start ? parent->heap_start : USER_HEAP_START_DEFAULT;
        child->heap_end   = parent->heap_end ? parent->heap_end : (USER_HEAP_START_DEFAULT + USER_HEAP_SIZE_DEFAULT);
        child->stack_start = parent->stack_start ? parent->stack_start : (USER_STACK_TOP_DEFAULT - USER_STACK_PAGES_DEFAULT * PAGE_SIZE);
        child->stack_end   = parent->stack_end ? parent->stack_end : USER_STACK_TOP_DEFAULT;

        uint64_t heap_size = (child->heap_end > child->heap_start) ? (child->heap_end - child->heap_start) : 0;
        uint64_t stack_size = (child->stack_end > child->stack_start) ? (child->stack_end - child->stack_start) : 0;

        if (heap_size && !paging_clone_user_range((page_directory_t*)child->page_dir, parent->page_dir, child->heap_start, heap_size)) {
            if (saved) paging_switch_directory(saved);
            sync_irq_restore(dir_flags);
            release_user_process_pages(child);
            paging_destroy_directory((page_directory_t*)child->page_dir);
            child->page_dir = NULL;
            return 0;
        }
        if (stack_size && !paging_clone_user_range((page_directory_t*)child->page_dir, parent->page_dir, child->stack_start, stack_size)) {
            if (saved) paging_switch_directory(saved);
            sync_irq_restore(dir_flags);
            release_user_process_pages(child);
            paging_destroy_directory((page_directory_t*)child->page_dir);
            child->page_dir = NULL;
            return 0;
        }
    }

    if (saved) {
        paging_switch_directory(saved);
    }
    sync_irq_restore(dir_flags);
    return 1;
}

bool task_alloc_page(process_t* proc, uint64_t virt_addr, uint64_t flags) {
    if (!proc || !proc->page_dir) return FALSE;

    uint64_t dir_flags = sync_irq_save();
    page_directory_t* saved = paging_get_current_directory();
    paging_switch_directory((page_directory_t*)proc->page_dir);

    uint64_t phys = paging_alloc_physical();
    if (!phys) {
        if (saved) paging_switch_directory(saved);
        sync_irq_restore(dir_flags);
        return FALSE;
    }

    uint64_t map_flags = flags | PAGE_PRESENT | PAGE_RW;
    if (proc->type == TASK_TYPE_USER) map_flags |= PAGE_USER;
    if (!paging_map_page(virt_addr, phys, map_flags)) {
        paging_free_physical(phys);
        if (saved) paging_switch_directory(saved);
        sync_irq_restore(dir_flags);
        return FALSE;
    }

    memset((void*)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    /* Stack grows DOWN (toward lower addresses) on x86_64: stack_end is
     * the fixed top, stack_start is the current lowest mapped page. A
     * newly-mapped page extending the stack therefore has an address
     * BELOW the current stack_start, not >= it - the old
     * `virt_addr >= proc->stack_start` check could only ever be true
     * for pages already inside the mapped region (or above it, which
     * isn't stack at all), so stack_start could never actually move
     * and calling this to grow the stack would silently do nothing to
     * the tracked bounds. */
    if (virt_addr < proc->stack_start) {
        proc->stack_start = virt_addr & PAGE_MASK;
    }
    /* Keep heap bounds stable here; callers that grow the heap should
       update the explicit heap window, not shrink its lower bound. */

    if (saved) paging_switch_directory(saved);
    sync_irq_restore(dir_flags);
    return TRUE;
}

void task_free_page(process_t* proc, uint64_t virt_addr) {
    if (!proc || !proc->page_dir) return;
    uint64_t dir_flags = sync_irq_save();
    page_directory_t* saved = paging_get_current_directory();
    paging_switch_directory((page_directory_t*)proc->page_dir);
    uint64_t phys = paging_virt_to_phys(virt_addr);
    if (phys) {
        paging_unmap_page(virt_addr);
        paging_free_physical(phys);
    }
    if (saved) paging_switch_directory(saved);
    sync_irq_restore(dir_flags);
}

int task_get_count(void) {
    int count = 0;
    for (size_t i = 0; i < MAX_TASKS; ++i) {
        if (process_table[i].state != TASK_UNUSED) count++;
    }
    return count;
}

void task_dump(process_t* proc) {
    if (!proc) return;
    serial_puts("PID=");
    serial_putdec(proc->pid);
    serial_puts(" name=");
    serial_puts(proc->name);
    serial_puts(" state=");
    serial_puts(task_state_name(proc->state));
    serial_puts(" threads=");
    serial_putdec(proc->thread_count);
    serial_puts("\n");
}

void task_dump_all(void) {
    for (size_t i = 0; i < MAX_TASKS; ++i) {
        if (process_table[i].state != TASK_UNUSED) {
            task_dump(&process_table[i]);
        }
    }
}

process_t* task_get_first(void) {
    return process_list;
}

process_t* task_get_next(process_t* proc) {
    return proc ? proc->next : NULL;
}
