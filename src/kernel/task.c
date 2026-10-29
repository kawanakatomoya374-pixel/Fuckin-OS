/**
 * task.c - Process and Thread Management
 *
 * Minimal but functional process / thread model for the kernel.
 */

#include "memory_physical.h"
void task_reap_note(void);
void task_reap_idle(void);
#include "cos_kevent.h"
#include "smp_user.h"
#include "task.h"
#include "cos_sysinfo.h"
#include "scheduler.h"
#include "memory.h"
#include "mm/paging.h"
#include "gdt.h"
#include "serial.h"
#include "timer.h"
#include "sync.h"
#include "cos_elf.h"
#include "security/cos_security.h"

void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t num);
char* strncpy(char* dest, const char* src, size_t n);
void serial_puthex(uint64_t n);
void serial_putdec(uint64_t n);
uint64_t paging_alloc_pages(uint64_t count, uint64_t flags);
void paging_free_pages(uint64_t virtual_addr, uint64_t count);
void paging_release_user_range(uint64_t virtual_addr, uint64_t count);
uint64_t paging_alloc_physical(void);
void paging_free_physical(uint64_t phys_addr);
bool paging_setup_user_stack(page_directory_t *dir, uint64_t top, uint64_t pages);
void task_entry_wrapper(void);
void fs_unified_close_process_files(uint64_t owner_pid);
void ipc_release_process_resources(uint64_t owner_pid);

static process_t process_table[MAX_TASKS];
thread_t thread_table[MAX_THREADS];     /* scheduler.c reads it for its diagnostic dump */
static process_t* process_list = NULL;
static process_t* zombie_list = NULL;
static process_t* idle_process = NULL;
static thread_t* idle_thread = NULL;
#include "percpu.h"
#define current_process (cpu_local()->proc)       /* the process whose address space this CPU has loaded */
static uint64_t next_pid = 1;
static uint64_t next_tid = 1;

#define TASK_ROOT_UID 0u
#define TASK_ROOT_GID 0u
#define TASK_ROOT_UMASK 0022u

/* Keep user mappings outside the low identity-mapped kernel image and
 * NetSurf compatibility runway. The former 64–128 MiB defaults could collide
 * with dynamically mapped physical frames during real HTML/CSS processing. */
/* User stack/heap must live in the PER-PROCESS PML4 region (entries
 * 1..255), not in PML4[0].
 *
 * PML4[0] is the kernel's low identity map and is shared by reference with
 * every process directory (see paging_create_directory()). These constants
 * used to place the user stack at 0x40000000 and the heap at 0x10000000,
 * both inside PML4[0] - so the FIRST user process's stack mapping was
 * visible in every subsequent process's address space, and creating a
 * second user process failed outright with "map_page: leaf already
 * present" because the address was already taken. That made
 * process_create() return NULL for any user process after the first, which
 * is why only one ring3 program could ever exist.
 *
 * Program image now sits at 0x80_0000_0000 (PML4[1]); stack and heap are
 * placed in PML4[2] and PML4[3] so a large program image can never grow
 * into them. */
#define USER_STACK_TOP_DEFAULT   0x0000010000000000ULL
#define USER_STACK_PAGES_DEFAULT 8ULL
/* Upper bound on how far task_handle_page_fault() will grow a user
 * stack downward across its lifetime (see the comment there). 512
 * pages (2 MiB) comfortably covers any real call chain - including
 * large single-stack-frame functions like dav1d's wedge-mask table
 * builder (~24 KiB in one frame) - while still catching a genuinely
 * wild fault address instead of mapping pages for it forever. */
#define USER_STACK_MAX_PAGES     512ULL
#define USER_HEAP_START_DEFAULT  0x0000018000000000ULL
/* Virtual window only (pages are mapped on first touch and freed with the process). 64 MiB was too small for software
 * HEVC at 1080p when it decodes on the main thread (no spare cores): the reference-picture pool alone reaches it, and
 * the next frame allocation fails. The window is ~3 TiB clear of the next region (the zero-copy video buffers). */
#define USER_HEAP_SIZE_DEFAULT   0x0000000020000000ULL

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
        proc->caps = 0xFFFFFFFFu;
        proc->jail[0] = '\0';
        return;
    }

    proc->uid = parent->uid;
    proc->gid = parent->gid;
    proc->euid = parent->euid;
    proc->egid = parent->egid;
    proc->umask = parent->umask;
    proc->caps = parent->caps;                  /* a sandbox is inherited: a child can never hold more than its parent */
    memcpy(proc->jail, parent->jail, sizeof(proc->jail));
}

static void release_user_process_pages(process_t* proc) {
    if (!proc || proc->type != TASK_TYPE_USER || !proc->page_dir) return;
    uint64_t free_before = phys_memory_get_stats()->free_pages;

    uint64_t flags = sync_irq_save();
    page_directory_t* saved = paging_get_current_directory();
    paging_switch_directory((page_directory_t*)proc->page_dir);

    {   /* the program image, its libraries, TLS and link map (their frames were never given back before) */
        extern void cos_link_unmap_pid(uint64_t pid);
        extern void cos_link_release_pid(uint64_t pid);
        cos_link_unmap_pid(proc->pid);
        cos_link_release_pid(proc->pid);
    }
    if (proc->linux_ctx) { extern void cos_linux_release_pages(process_t *p); cos_linux_release_pages(proc); }       /* a Linux program's program, mmap and brk pages (cos_linux.c) */
    if (proc->heap_end > proc->heap_start) {
        uint64_t heap_pages = (proc->heap_end - proc->heap_start + PAGE_SIZE - 1) / PAGE_SIZE;
        paging_release_user_range(proc->heap_start, heap_pages);
    }
    if (proc->stack_end > proc->stack_start) {
        uint64_t stack_pages = (proc->stack_end - proc->stack_start + PAGE_SIZE - 1) / PAGE_SIZE;
        paging_release_user_range(proc->stack_start, stack_pages);
    }

    if (saved) paging_switch_directory(saved);
    sync_irq_restore(flags);
    if (proc->linux_ctx) {
        static uint32_t logged; if (logged++ < 40) { serial_puts("[TASK] pages of pid "); serial_putdec(proc->pid); serial_puts(" released: free frames "); serial_putdec(free_before >> 8); serial_puts(" -> "); serial_putdec(phys_memory_get_stats()->free_pages >> 8); serial_puts(" MiB\n"); }
        extern void cos_linux_release_final(process_t *p); cos_linux_release_final(proc);       /* the table of ranges was needed until now */
    }
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
        task_reap_idle();
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

    /* ASLR: every user process gets its own heap base (up to 16 MiB above the default) and stack top (up to 1 MiB below
     * it), so an attacker cannot rely on addresses learned from another run or another process. */
    uint64_t heap_base = USER_HEAP_START_DEFAULT, stack_top = USER_STACK_TOP_DEFAULT;
    if (type == TASK_TYPE_USER) {
        heap_base += (cos_random_u64() % 4096ull) * PAGE_SIZE;
        stack_top -= (cos_random_u64() % 256ull) * PAGE_SIZE;
    }
    proc->heap_start = (type == TASK_TYPE_USER) ? heap_base : 0;
    /* heap_end is the current brk, not the reserved window.  Initializing it
     * to the window ceiling made the first sbrk() return an address at the
     * far end of the heap and caused every Media malloc to fault repeatedly. */
    proc->heap_end = (type == TASK_TYPE_USER) ? heap_base : 0;
    proc->heap_limit = (type == TASK_TYPE_USER) ? (heap_base + USER_HEAP_SIZE_DEFAULT) : 0;
    proc->stack_end = (type == TASK_TYPE_USER) ? stack_top : 0;
    proc->stack_start = (type == TASK_TYPE_USER) ? (stack_top - USER_STACK_PAGES_DEFAULT * PAGE_SIZE) : 0;
    proc->mmap_next_offset = (type == TASK_TYPE_USER) ? (cos_random_u64() % 16384ull) * PAGE_SIZE : 0;   /* mmap base: up to 64 MiB of slide */
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
    /* Real blocking wait for SYS_WAITPID - see the field comment on
     * process_t::child_wait_queue. Must be initialized before this
     * process can possibly become a PARENT (i.e. before it returns to
     * whoever is about to call process_create() again on its behalf),
     * since process_exit() looks up and wakes a parent's queue
     * unconditionally by pid with no NULL/uninitialized check beyond
     * "does this pid still exist". */
    wait_queue_init(&proc->child_wait_queue);
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

    cos_kmod_event(COSYS_EV_PROC_START, proc->pid, proc->parent_pid, 0);                 /* kernel modules (.c-osys) that subscribed */
    return proc;
}

void process_exit(process_t* proc, int status) {
    if (!proc || proc->state == TASK_UNUSED) return;

    /* Record the status BEFORE anything else can observe the zombie
     * state: waitpid() looks for TASK_ZOMBIE and then reads these, so
     * setting the state first would leave a window where a waiter sees a
     * zombie with a stale/garbage status. */
    proc->exit_status = status;
    proc->exit_status_valid = true;
    proc->state = TASK_ZOMBIE;
    task_reap_note();
    cos_kmod_event(COSYS_EV_PROC_EXIT, proc->pid, (uint64_t)(int64_t)status, 0);

    /* Compute jobs this process left running on other cores execute under
     * its page tables; stop them before anything below tears those down. */
    smp_user_kill_owner(proc);

    /* Per-process device resources. cos_app_window_release_for_pid()
     * existed but was never called, so an exited program's ring-3 window
     * slots (only COS_APP_MAX_WINDOWS of them) stayed taken forever. */
    {
        extern void cos_app_window_release_for_pid(uint32_t pid);
        extern void cos_audio_release_for_pid(uint32_t pid);
        cos_app_window_release_for_pid((uint32_t)proc->pid);
        cos_audio_release_for_pid((uint32_t)proc->pid);
    }

    /* Wake any thread of the PARENT blocked in SYS_WAITPID, replacing
     * the earlier polling implementation (a 10ms scheduler_sleep()
     * between checks). Looked up by parent_pid rather than a stored
     * pointer, since the parent's own process_t slot address is not
     * retained anywhere else and pid is already the stable identifier
     * used throughout this codebase for cross-process references.
     * A NULL result (parent already exited, or pid 0 for a
     * kernel-spawned process with no real parent) means there is no one
     * to wake - not an error. wait_queue_wake_all() rather than
     * wake_one(): more than one thread could in principle be waiting on
     * different children of the same parent, and a spurious wake is
     * harmless (the woken thread just re-checks and blocks again if its
     * own wait was not the one satisfied). */
    process_t *parent = process_get_by_pid(proc->parent_pid);
    if (parent) {
        wait_queue_wake_all(&parent->child_wait_queue);
    }

    /* Free any .c-osll slots this process held; its address space is about
     * to go away, so the recorded library bases are meaningless. */
    extern void cos_dlclose_for_pid(uint64_t pid);
    if (!proc->page_dir) cos_dlclose_for_pid(proc->pid);     /* otherwise release_user_process_pages() unmaps the objects first, then drops the namespace */

    /* Close any files/directories this process still had open. Without
     * this, a process that exits without closing its own descriptors
     * would leak the FatFs FIL/DIR object forever - and worse, the fd
     * table slot itself, since nothing else would ever release it. */
    extern void cos_fd_close_all_for_pid(uint64_t pid);
    cos_fd_close_all_for_pid(proc->pid);
    { extern void cos_linux_release(process_t *p); cos_linux_release(proc); }
    extern void cos_sock_close_all_for_pid(uint32_t pid);
    cos_sock_close_all_for_pid((uint32_t)proc->pid);

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
        task_reap_note();
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
    /* A thread of this process may be running on another CPU, in user mode under these very page tables. Nothing is freed until every such thread is off
     * its CPU (the reaper checks on_cpu); the CPU is told to look at the run queues now instead of at its next tick. */
    bool running_elsewhere = false;
    for (thread_t* t = proc->thread_list; t; t = t->proc_next) {
        if (t != self && t->on_cpu) {
            running_elsewhere = true;
            if (t->last_cpu > 0) { extern void smp_kick_cpu(uint32_t cpu); smp_kick_cpu((uint32_t)t->last_cpu); }
        }
    }
    if (proc->page_dir && !exiting_self && !running_elsewhere) {
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
    /* Release any threads of this process parked in FUTEX_WAIT, and any
     * link-namespace bookkeeping. A thread blocked on a futex sits on a
     * kernel wait queue that nothing will ever wake once its process is
     * gone, holding its thread_t and kernel stack forever. */
    extern void cos_futex_release_pid(uint64_t pid);
    extern void cos_link_release_pid(uint64_t pid);
    if (proc) {
        cos_futex_release_pid(proc->pid);
        /* A process with an address space keeps its link namespace until release_user_process_pages() has unmapped the image and libraries it describes. */
        if (!proc->page_dir) cos_link_release_pid(proc->pid);
    }

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
/**
 * process_try_reap_child - non-blocking waitpid().
 *
 * Looks for an exited child of `parent_pid` whose status has not been
 * collected. If `want_pid` is > 0 only that pid matches; otherwise any
 * child does.
 *
 * Returns the child's pid and writes its status, or 0 if the child (or
 * children) exist but none has exited yet, or -1 if there is no such
 * child at all - so a caller can distinguish "not yet" from "never",
 * which a single failure value would conflate.
 *
 * Collecting clears exit_status_valid, which is what allows the reaper
 * above to finally recycle the slot.
 */
int64_t process_try_reap_child_ex(uint64_t parent_pid, int64_t want_pid, int* out_status, bool consume);
int64_t process_try_reap_child(uint64_t parent_pid, int64_t want_pid, int* out_status) { return process_try_reap_child_ex(parent_pid, want_pid, out_status, true); }
int64_t process_try_reap_child_ex(uint64_t parent_pid, int64_t want_pid, int* out_status, bool consume) {
    uint64_t flags = sync_irq_save();
    bool saw_child = false;
    int64_t result = -1;

    for (int i = 0; i < MAX_TASKS; ++i) {
        process_t* p = &process_table[i];
        if (p->state == TASK_UNUSED) continue;
        if (p->parent_pid != parent_pid) continue;
        if (want_pid > 0 && p->pid != (uint64_t)want_pid) continue;

        saw_child = true;
        if (p->state == TASK_ZOMBIE && p->exit_status_valid) {
            if (out_status) *out_status = p->exit_status;
            if (consume) { p->exit_status_valid = false; task_reap_note(); }   /* collected; reaper may recycle */
            result = (int64_t)p->pid;
            break;
        }
    }

    sync_irq_restore(flags);
    if (result >= 0) return result;
    return saw_child ? 0 : -1;
}

/* A thread that ended while its process lives on (pthread_exit, a returned thread function) stays a ZOMBIE until this gives its slot and its kernel stack back:
 * without it a program that creates and ends threads all day (a browser) runs out of the 1024 thread slots. */
/* The idle loops (every CPU, at every interrupt) used to call the reaper each time, and the reaper takes the kernel lock twice and scans the thread table: with several idle CPUs that was a
 * large part of the lock's traffic for nothing. Something now says when there is anything to reap: a thread or a process that ended, a child whose exit status was collected. The idle
 * loop calls the reaper only then, and until a pass leaves nothing behind (a zombie whose thread is still on a CPU has to be tried again). */
static volatile uint32_t g_reap_hint, g_reap_done;
static volatile bool g_reap_incomplete;
void task_reap_note(void) { __atomic_add_fetch(&g_reap_hint, 1u, __ATOMIC_RELEASE); }
void task_reap_idle(void) {
    uint32_t seen = __atomic_load_n(&g_reap_hint, __ATOMIC_ACQUIRE);
    if (seen == __atomic_load_n(&g_reap_done, __ATOMIC_ACQUIRE)) return;
    g_reap_incomplete = false;
    task_reap_zombies();
    if (!g_reap_incomplete) __atomic_store_n(&g_reap_done, seen, __ATOMIC_RELEASE);
}
void task_reap_dead_threads(void) {
    uint64_t flags = sync_irq_save();
    thread_t *cur = scheduler_get_current_thread();
    for (size_t i = 0; i < MAX_THREADS; ++i) {
        thread_t *t = &thread_table[i];
        if (t->state == TASK_ZOMBIE && (t->on_cpu || t == cur)) g_reap_incomplete = true;
        if (t->state != TASK_ZOMBIE || t->on_cpu || t == cur) continue;
        process_t *p = process_get_by_pid(t->pid);
        if (!p || p->state == TASK_ZOMBIE || p->state == TASK_UNUSED) continue;            /* a dead process is reaped whole below */
        thread_destroy(t);
    }
    sync_irq_restore(flags);
}

void task_reap_zombies(void) {
    task_reap_dead_threads();
    uint64_t flags = sync_irq_save();       /* the kernel lock, not just cli: the process tables and page allocator are shared by every CPU */

    process_t* proc = zombie_list;
    while (proc) {
        process_t* next = proc->next;

        /* Another CPU may still be running (or switching away from) a thread of this process: its kernel stack and address space stay until it is off. */
        proc->reap_defer = 0;
        for (size_t i = 0; i < MAX_THREADS; ++i)
            if (thread_table[i].state != TASK_UNUSED && thread_table[i].pid == proc->pid && thread_table[i].on_cpu) { proc->reap_defer = 1; break; }
        if (proc->reap_defer) {
            g_reap_incomplete = true;
            static uint32_t defer_logs;
            if (defer_logs++ < 12) { serial_puts("[TASK] reap deferred: pid="); serial_putdec(proc->pid); serial_puts("\n"); }
            proc = next; continue;
        }

        destroy_threads_for_process(proc);

        if (proc->page_dir) {
            release_user_process_pages(proc);
            paging_destroy_directory(proc->page_dir);
            proc->page_dir = NULL;
        }

        proc = next;
    }

    /* Recycle slots - except a zombie whose exit status nobody has
     * collected yet AND whose parent is still around to collect it.
     *
     * The heavy resources (threads, page directory) were already released
     * above and still are: what is retained here is only the process_t
     * slot carrying pid/parent_pid/exit_status, which is what waitpid()
     * reads. Without this the reaper - which runs from the idle loop and
     * therefore very often - would recycle a child before its parent ever
     * got a chance to see it had exited, making waitpid() inherently
     * racy rather than merely slow.
     *
     * A dead or absent parent means nobody will ever collect, so those are
     * recycled immediately; that is what stops uncollected zombies from
     * accumulating when a parent exits without waiting. */
    process_t* keep_head = NULL;
    while (zombie_list) {
        process_t* p = zombie_list;
        zombie_list = p->next;

        if (p->reap_defer) {                      /* not released yet: stays on the list */
            g_reap_incomplete = true;
            p->next = keep_head;
            keep_head = p;
            continue;
        }

        bool parent_alive = false;
        if (p->exit_status_valid && p->parent_pid != 0) {
            for (int i = 0; i < MAX_TASKS; ++i) {
                if (process_table[i].state != TASK_UNUSED &&
                    process_table[i].state != TASK_ZOMBIE &&
                    process_table[i].pid == p->parent_pid) {
                    parent_alive = true;
                    break;
                }
            }
        }

        if (parent_alive) {
            /* Stay a zombie, stay on the list, wait to be collected. */
            p->next = keep_head;
            keep_head = p;
            continue;
        }

        p->state = TASK_UNUSED;
        p->exit_status_valid = false;
        p->thread_list = NULL;
        p->main_thread = NULL;
        p->next = NULL;
        p->prev = NULL;
    }
    zombie_list = keep_head;

    sync_irq_restore(flags);
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

int process_snapshot(cos_procinfo_t *out, int max) {
    int n = 0;
    for (size_t i = 0; i < MAX_TASKS && n < max; ++i) {
        const process_t *p = &process_table[i];
        if (p->state == TASK_UNUSED) continue;
        cos_procinfo_t *o = &out[n++];
        memset(o, 0, sizeof(*o));
        o->pid = p->pid;
        o->parent_pid = p->parent_pid;
        o->cpu_time = p->cpu_time;
        o->ctx_switches = p->context_switches;
        o->page_faults = p->page_faults;
        o->heap_bytes = p->heap_end > p->heap_start ? p->heap_end - p->heap_start : 0;
        o->state = (uint32_t)p->state;
        o->is_user = p->type == TASK_TYPE_USER;
        o->threads = (uint32_t)p->thread_count;
        o->caps = p->caps;
        memcpy(o->name, p->name, sizeof(o->name) - 1);
    }
    return n;
}

int process_count_children(uint64_t pid) {
    int n = 0;
    for (size_t i = 0; i < MAX_TASKS; ++i)
        if (process_table[i].state != TASK_UNUSED && process_table[i].parent_pid == pid) n++;
    return n;
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

static thread_t* thread_create_impl(process_t* proc, void* entry_point, void* arg,
                                    size_t stack_size, uint64_t user_sp, bool cpu_idle);

thread_t* thread_create_stack_size(process_t* proc, void* entry_point, void* arg,
                                   size_t stack_size) {
    return thread_create_user_sp(proc, entry_point, arg, stack_size, 0);
}

/* user_sp: the initial ring-3 stack pointer of a user thread, or 0 for the process stack top.
 * Threads that all start on the process stack top overwrite each other's frames, so a program that runs more than one thread gives each its own stack. */
thread_t* thread_create_user_sp(process_t* proc, void* entry_point, void* arg,
                                size_t stack_size, uint64_t user_sp) {
    return thread_create_impl(proc, entry_point, arg, stack_size, user_sp, false);
}

/* The idle thread of an application processor: a thread of the idle process that no run queue and no scheduler_set_idle() knows about (the CPU keeps it in
 * its cpu_local_t). */
thread_t* thread_create_cpu_idle(void* entry_point, void* arg) {
    return thread_create_impl(idle_process, entry_point, arg, KERNEL_STACK_SIZE, 0, true);
}

static thread_t* thread_create_impl(process_t* proc, void* entry_point, void* arg,
                                    size_t stack_size, uint64_t user_sp, bool cpu_idle) {
    if (!proc) return NULL;

    thread_t* th = alloc_thread_slot();
    if (!th) return NULL;

    th->pid = proc->pid;
    th->state = TASK_CREATED;
    th->smp_ok = (proc->type == TASK_TYPE_USER) ? 1 : 0;
    th->last_cpu = -1;
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
    cos_fpu_init_area(cos_fpu_area(th));
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
        th->context.rsp = user_sp ? ((user_sp & ~15ULL) - 8ULL) : align_up_u64(proc->stack_end, 16ULL) - 8ULL;
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
    th->state = cpu_idle ? TASK_RUNNING : TASK_READY;
    if (cpu_idle) th->on_cpu = 1;

    /* Register with the scheduler. Without this, a thread could be
     * fully constructed and marked TASK_READY yet sit in no run queue
     * at all - scheduler_add_task()/pick_next_task() only look at the
     * run queues, not at thread_table, so nothing would ever actually
     * pick this thread up. The idle thread is the one exception: it is
     * wired in via scheduler_set_idle() as pick_next_task()'s fallback
     * for "nothing else is ready", not queued alongside normal work. */
    if (cpu_idle) {
        /* kept by its CPU only */
    } else if (proc->type == TASK_TYPE_IDLE) {
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
    task_reap_note();

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

/* Bounded, deliberate tradeoff: the guard page below a stack (see
 * task_alloc_stack()) is unmapped and its physical frame freed
 * immediately, but its VIRTUAL address is never returned to
 * virt_free_range_add() - so it can never be handed to a later
 * allocation and silently stop being a guard. This costs one page of
 * virtual address space, permanently, per thread ever created. Kernel
 * virtual address space here is a 64-bit range several orders of
 * magnitude larger than any realistic number of threads a machine like
 * this will create in its lifetime, so the tradeoff is one-sided: a
 * guard that provably stays a guard forever, for an amount of address
 * space that will never be missed. */
uint64_t task_alloc_stack(size_t size) {
    size = align_up_u64(size, PAGE_SIZE);
    uint64_t pages = size / PAGE_SIZE;

    /* One extra page, reserved contiguously with the real stack so the
     * guard sits immediately below it with nothing else able to land in
     * between. Requested from the SAME allocator/SAME call as the real
     * stack pages specifically so this is true: two separate
     * allocations have no guaranteed adjacency at all, and a stack
     * without a guard directly beneath it is not guarded. */
    uint64_t base = paging_alloc_pages(pages + 1, PAGE_PRESENT | PAGE_RW);
    if (!base) return 0;

    /* The bottom page of that block becomes the guard: unmapped, its
     * frame freed back to the physical allocator immediately (nothing
     * is ever stored there, so holding the frame would only waste it).
     * paging_virt_to_phys() must be read BEFORE unmapping - once the
     * PTE's present bit is cleared, paging_virt_to_phys() itself will no
     * longer report an address for it, by design (see paging.c), and
     * this is the one and only place that address is needed. */
    uint64_t guard_phys = paging_virt_to_phys(base);
    paging_unmap_page(base);
    if (guard_phys) paging_free_physical(guard_phys);

    /* The real, usable stack begins one page above the guard. Returning
     * `real_base + size` keeps this function's contract identical to
     * before the guard existed: the caller gets a stack TOP, sized
     * exactly as requested, with the guard now an implementation detail
     * it never needs to know about. */
    return base + PAGE_SIZE + size;
}

void task_free_stack(uint64_t stack_base, size_t size) {
    if (!stack_base) return;
    size = align_up_u64(size, PAGE_SIZE);
    uint64_t virt_base = stack_base - size;
    paging_free_pages(virt_base, size / PAGE_SIZE);
    /* The guard page one page below was already unmapped and its frame
     * already freed at allocation time (see task_alloc_stack()) - there
     * is nothing left to release for it here. Its virtual address is
     * deliberately not returned to the allocator; see the comment above
     * task_alloc_stack() for why that is safe and intentional rather
     * than an oversight. */
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

const char *page_fault_kind;   /* how the last #PF was resolved ("heap", "mmap", "stack", "stack-grow"), NULL when it was not; read by the #PF dispatcher (idt.c) */

bool task_handle_page_fault(uint64_t fault_addr, uint64_t error_code) {
    process_t* proc = process_get_current();
    if (!proc) return FALSE;

    proc->page_faults++;
    if (proc->page_faults == 1 || (proc->page_faults & 0xFFu) == 0) {
        serial_puts("[TASK] demand page @ 0x");
        serial_puthex(fault_addr);
        serial_puts(" pid=");
        serial_putdec(proc->pid);
        serial_puts(" count=");
        serial_putdec(proc->page_faults);
        serial_puts("\n");
    }

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
    if (proc->type == TASK_TYPE_USER && proc->linux_ctx && (error_code & 0x7ULL) == 0x7ULL) {     /* a write to a page shared by fork() */
        extern bool cos_linux_cow_fault(process_t *p, uint64_t pg);
        return cos_linux_cow_fault(proc, fault_addr & ~(PAGE_SIZE - 1ULL));
    }
    if (proc->type != TASK_TYPE_USER || (error_code & 0x1ULL) || !(error_code & 0x4ULL)) {
        return FALSE;
    }

    uint64_t page = fault_addr & ~(PAGE_SIZE - 1ULL);

    if (proc->linux_ctx) {                                      /* a Linux program: its mmap and brk ranges (cos_linux.c) */
        extern bool cos_linux_page_fault(process_t *p, uint64_t pg);
        page_fault_kind = "linux";
        return cos_linux_page_fault(proc, page);
    }

    if (proc->heap_start && proc->heap_end && page >= proc->heap_start && page < proc->heap_end) {
        /* Heap data is never code. Was PAGE_PRESENT | PAGE_RW | PAGE_USER
         * with no NX bit at all - meaning the CPU's own execute-protect
         * hardware was never engaged for heap memory, on any build where
         * the CPU supports it. cos_elf.c already does this correctly for
         * a program's static PT_LOAD segments (COS_PAGE_NX set per the
         * segment's own PF_X bit - see cos_elf_nx_available() there);
         * this closed the same gap for the memory demand-paged in
         * afterward, which is exactly where a real exploit's payload
         * would have to land, since it is the only writable memory a
         * process has once its static image is mapped read-only/
         * executable-only per segment. */
        uint64_t heap_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
        if (cos_elf_nx_available()) heap_flags |= PAGE_NX;
        page_fault_kind = "heap";
        {   /* fault-around: a heap that is filled in order takes one fault per page otherwise (an IME dictionary load took 17 000) */
            uint64_t left = (proc->heap_end - page) / PAGE_SIZE; unsigned n = left > 8 ? 8 : (unsigned)left;
            return task_alloc_page_run(proc, page, n ? n : 1, heap_flags);
        }
    }

    /* Anonymous mmap regions: same demand-paging treatment as the heap.
     * A linear scan over COS_MMAP_MAX_REGIONS (16) is fine here - this
     * runs once per fault, not once per byte, and 16 is a hard, small
     * cap (see the design note on cos_mmap_region_t in task.h). */
    for (int i = 0; i < COS_MMAP_MAX_REGIONS; ++i) {
        if (!proc->mmap_regions[i].used) continue;
        if (page >= proc->mmap_regions[i].start && page < proc->mmap_regions[i].end) {
            uint64_t flags = PAGE_PRESENT | PAGE_USER;
            if (proc->mmap_regions[i].writable) flags |= PAGE_RW;
            /* Same gap as the heap, closed the same way - except mmap
             * has a legitimate reason to ever be executable (JIT), so
             * this defers to the region's own flag rather than always
             * forcing NX. SYS_MPROTECT (syscall.c) is the only way that
             * flag becomes true, and it refuses to set it alongside
             * writable=true in the same call - W^X is enforced at the
             * point permissions are requested, not left to this fault
             * path to second-guess. */
            if (!proc->mmap_regions[i].executable && cos_elf_nx_available()) {
                flags |= PAGE_NX;
            }
            page_fault_kind = "mmap";
            return task_alloc_page(proc, page, flags);
        }
    }

    if (proc->stack_start && proc->stack_end) {
        /* A single function prologue can drop rsp by far more than one
         * page in one instruction (e.g. a large local array - dav1d's
         * one-time wedge-mask table builder allocates ~24 KiB in a
         * single stack frame), and then write near the new rsp before
         * ever touching the intervening pages. That lands the fault
         * more than one page below stack_start, which the previous
         * single-page-only growth check rejected outright, crashing
         * the process instead of growing the stack. Grow by however
         * many pages are needed to cover the fault, not just one, as
         * long as the result stays within USER_STACK_MAX_PAGES - a
         * fault further out than that is treated as a real wild
         * pointer, not a deep stack frame, and still crashes loudly. */
        uint64_t stack_floor = (proc->stack_end > USER_STACK_MAX_PAGES * PAGE_SIZE)
                                ? (proc->stack_end - USER_STACK_MAX_PAGES * PAGE_SIZE) : 0;
        if (page < proc->stack_start && page >= stack_floor) {
            /* Same NX gap as the heap, and arguably the more important
             * of the two to close: the stack is the classic landing spot
             * for a stack-smashing exploit's injected shellcode, and
             * with no NX bit ever set here, the CPU's own hardware
             * execute-protection was never actually engaged for it. */
            uint64_t stack_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
            if (cos_elf_nx_available()) stack_flags |= PAGE_NX;
            for (uint64_t p = page; p < proc->stack_start; p += PAGE_SIZE) {
                if (!task_alloc_page(proc, p, stack_flags)) return FALSE;
            }
            proc->stack_start = page;
            page_fault_kind = "stack-grow";
            return TRUE;
        } else if (page >= proc->stack_start && page < proc->stack_end) {
            uint64_t stack_flags = PAGE_PRESENT | PAGE_RW | PAGE_USER;
            if (cos_elf_nx_available()) stack_flags |= PAGE_NX;
            page_fault_kind = "stack";
            return task_alloc_page(proc, page, stack_flags);
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
        child->heap_end   = parent->heap_end ? parent->heap_end : USER_HEAP_START_DEFAULT;
        child->heap_limit = parent->heap_limit ? parent->heap_limit : (USER_HEAP_START_DEFAULT + USER_HEAP_SIZE_DEFAULT);
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
    if (saved != (page_directory_t*)proc->page_dir) paging_switch_directory((page_directory_t*)proc->page_dir);     /* (a switch reloads CR3: the page is usually in the running process) */
    else saved = NULL;

    if (paging_virt_to_phys(virt_addr & PAGE_MASK)) {
        if (saved) paging_switch_directory(saved);
        sync_irq_restore(dir_flags);
        return TRUE;
    }
    uint64_t phys = paging_alloc_physical();
    if (!phys) {
        if (saved) paging_switch_directory(saved);
        sync_irq_restore(dir_flags);
        return FALSE;
    }

    /* Was `flags | PAGE_PRESENT | PAGE_RW` - unconditionally forcing every
     * demand-paged page writable regardless of what the caller asked for.
     * Every existing caller (heap, stack growth) already passes PAGE_RW
     * explicitly, so removing the forced OR does not change their
     * behaviour - but it is what makes the mmap PROT_READ-only case below
     * (added this session) actually enforceable rather than silently
     * ignored: a region recorded as non-writable now really is mapped
     * without PAGE_RW, not "read-only" in name only. Found while wiring
     * up mmap's writable flag and discovering it had no effect until this
     * line changed. */
    uint64_t map_flags = flags | PAGE_PRESENT;
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
    if (virt_addr < proc->stack_start && !proc->linux_ctx) {          /* (a Linux program's mappings below the stack are not stack) */
        proc->stack_start = virt_addr & PAGE_MASK;
    }
    /* Keep heap bounds stable here; callers that grow the heap should
       update the explicit heap window, not shrink its lower bound. */

    if (saved) paging_switch_directory(saved);
    sync_irq_restore(dir_flags);
    return TRUE;
}

/* Demand paging with "fault-around": maps `n` consecutive zeroed pages from `virt_addr` in one go (one interrupt-off section, one address-space switch). Pages that are already mapped are skipped.
 * The first page must succeed; if memory runs out for a later one the run just ends there (it will fault again when touched). A program that touches memory in order (an allocator carving a
 * fresh arena, a buffer being filled) takes a fault per page otherwise: 56 000 of them in one Firefox process before it shows a window. */
bool task_alloc_page_run(process_t* proc, uint64_t virt_addr, unsigned n, uint64_t flags) {
    if (!proc || !proc->page_dir || !n) return FALSE;
    uint64_t dir_flags = sync_irq_save();
    page_directory_t* saved = paging_get_current_directory();
    if (saved != (page_directory_t*)proc->page_dir) paging_switch_directory((page_directory_t*)proc->page_dir);
    else saved = NULL;
    uint64_t map_flags = flags | PAGE_PRESENT;
    if (proc->type == TASK_TYPE_USER) map_flags |= PAGE_USER;
    bool ok = TRUE;
    for (unsigned i = 0; i < n; ++i) {
        uint64_t va = (virt_addr & PAGE_MASK) + (uint64_t)i * PAGE_SIZE;
        if (paging_virt_to_phys(va)) continue;
        uint64_t phys = paging_alloc_physical();
        if (!phys) { if (i == 0) ok = FALSE; break; }
        if (!paging_map_page(va, phys, map_flags)) { paging_free_physical(phys); if (i == 0) ok = FALSE; break; }
        memset((void*)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    }
    if (saved) paging_switch_directory(saved);
    sync_irq_restore(dir_flags);
    return ok;
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
