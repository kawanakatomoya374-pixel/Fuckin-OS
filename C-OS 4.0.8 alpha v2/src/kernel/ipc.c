/**
 * ipc.c - Inter-Process Communication
 *
 * Implements the API declared in ipc.h on top of the multitasking
 * primitives in scheduler.c/task.c/sync.c:
 *
 *   - Each process gets a lazily-allocated mailbox: a small fixed-depth
 *     ring buffer of ipc_message_t, guarded by a mutex_t and signalled
 *     by a semaphore_t counting "messages available".
 *   - ipc_send()/ipc_notify()/ipc_respond() enqueue into the target
 *     process's mailbox and post its semaphore.
 *   - ipc_receive() dequeues from the CALLING process's own mailbox,
 *     blocking (via sem_wait) if it is empty and the caller asked to
 *     wait forever, or polling with 1ms sleeps against a deadline if a
 *     finite timeout was given.
 *
 * Mailboxes are keyed by the target process's stable process_table
 * slot index (see process_get_slot_index() in task.c), not by pid
 * directly - pids in this kernel only ever increase and are never
 * reused, so a naive pid-based key (e.g. pid % IPC_MAX_PROCESSES)
 * could alias two simultaneously-alive processes on a long-running
 * system. The slot index is bounded and unique among currently-alive
 * processes, so no such collision is possible.
 */

#include "ipc.h"
#include "mk_ipc.h"
#include "sync.h"
#include "task.h"
#include "scheduler.h"
#include "timer.h"
#include "serial.h"
#include "string.h"
#include "memory.h"
#include "memory_physical.h"

/* Optional paging support for ipc_request_shared_memory(). If the
 * memory-management headers aren't available in a given build, shared
 * memory requests simply fail gracefully (see IPC_HAVE_SHM below). */
#include "mm/paging.h"
#define IPC_HAVE_SHM 1

typedef struct {
    ipc_message_t entries[IPC_QUEUE_SIZE];
    uint64_t head;
    uint64_t tail;
    uint64_t count;
    uint64_t owner_pid;
    mutex_t  lock;
    semaphore_t items;      /* signalled once per enqueued message */
    uint64_t total_enqueued;
    uint64_t total_dequeued;
} mailbox_t;

static mailbox_t* mailboxes[IPC_MAX_PROCESSES];
static mutex_t mailbox_table_lock;

static uint64_t next_msg_id = 1;

static bool ipc_initialized = false;
static ipc_stats_t g_stats;

/* ---------------------------------------------------------------------
 * Shared memory (simple fixed-window allocator)
 * ------------------------------------------------------------------- */
#if IPC_HAVE_SHM
#define SHM_MAX_REGIONS      MK_MAX_SHARED_MEMORY
#define SHM_SLOT_SIZE        0x400000ULL           /* 4MB per region, max */
#define SHM_VA_BASE          0x0000000020000000ULL /* clear of default heap/stack */

#define SHM_MAX_ATTACH        8   /* max processes that can attach to one region (incl. owner) */

typedef struct {
    bool     in_use;
    uint64_t id;
    uint64_t size;         /* requested size, rounded up to pages */
    uint64_t pages;
    uint64_t virt_addr;    /* address used for this region's mapping, same in every attached process */
    uint64_t owner_pid;
    uint64_t flags;
    uint64_t* phys_pages;  /* kmalloc'd array of `pages` physical frame addresses */
    uint64_t attached_pids[SHM_MAX_ATTACH]; /* pids currently mapped into (owner included) */
    uint64_t attached_count;
} shm_region_t;

static shm_region_t shm_regions[SHM_MAX_REGIONS];
static uint64_t next_shm_id = 1;
static mutex_t shm_lock;
#endif
static bool shm_range_is_clear(uint64_t virt, uint64_t pages) {
#if IPC_HAVE_SHM
    if (!pages) return false;
    for (uint64_t p = 0; p < pages; ++p) {
        uint64_t page_va = virt + p * PAGE_SIZE;
        if (page_va < virt) return false;
        if (paging_is_present(page_va)) return false;
    }
#endif
    return true;
}

static void shm_unmap_range(uint64_t virt, uint64_t pages) {
#if IPC_HAVE_SHM
    for (uint64_t p = 0; p < pages; ++p) {
        uint64_t page_va = virt + p * PAGE_SIZE;
        if (page_va < virt) break;
        paging_unmap_page(page_va);
    }
#else
    (void)virt; (void)pages;
#endif
}

static void shm_release_owner(uint64_t pid) {
#if IPC_HAVE_SHM
    if (!ipc_initialized || !pid) return;
    mutex_lock(&shm_lock);
    for (uint64_t i = 0; i < SHM_MAX_REGIONS; ++i) {
        shm_region_t* region = &shm_regions[i];
        if (!region->in_use) continue;

        bool removed = false;
        for (uint64_t a = 0; a < region->attached_count; ++a) {
            if (region->attached_pids[a] != pid) continue;
            region->attached_pids[a] = region->attached_pids[region->attached_count - 1];
            region->attached_pids[region->attached_count - 1] = 0;
            region->attached_count--;
            removed = true;
            break;
        }
        if (!removed) continue;

        /* If the last attacher leaves, the region is no longer needed. */
        if (region->attached_count == 0) {
            if (region->phys_pages) {
                for (uint64_t p = 0; p < region->pages; ++p) {
                    if (region->phys_pages[p]) {
                        paging_free_physical(region->phys_pages[p]);
                    }
                }
                kfree(region->phys_pages);
            }
            memset(region, 0, sizeof(*region));
        }
    }
    mutex_unlock(&shm_lock);
#else
    (void)pid;
#endif
}


/* ---------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------- */

static inline int mailbox_slot(uint64_t pid) {
    /* Keyed by the process's stable process_table slot index rather
     * than pid itself. PIDs only ever increase (task.c's next_pid is
     * never reused), so pid % IPC_MAX_PROCESSES could alias two
     * distinct, simultaneously-alive processes whose pids happen to
     * differ by exactly IPC_MAX_PROCESSES on a long-running system.
     * The table slot index is bounded (0..MAX_TASKS-1, and
     * MAX_TASKS == IPC_MAX_PROCESSES) and unique among currently-alive
     * processes, so this can't collide. */
    return process_get_slot_index(pid);
}

/* Get (creating if necessary) the mailbox for a given pid. */
static mailbox_t* mailbox_get_or_create(uint64_t pid) {
    int slot = mailbox_slot(pid);
    if (slot < 0) return NULL; /* no such live process */

    mutex_lock(&mailbox_table_lock);

    mailbox_t* mb = mailboxes[slot];
    if (mb && mb->owner_pid != pid) {
        /* The process that previously occupied this table slot has
         * exited and the slot was reused by a newer process with a
         * different pid - reclaim the stale mailbox. */
        kfree(mb);
        mb = NULL;
        mailboxes[slot] = NULL;
    }

    if (!mb) {
        mb = (mailbox_t*)kmalloc(sizeof(mailbox_t));
        if (!mb) {
            mutex_unlock(&mailbox_table_lock);
            return NULL;
        }
        memset(mb, 0, sizeof(mailbox_t));
        mb->owner_pid = pid;
        mutex_init(&mb->lock);
        sem_init(&mb->items, 0);
        mailboxes[slot] = mb;
    }

    mutex_unlock(&mailbox_table_lock);
    return mb;
}

static void mailbox_release_owner(uint64_t pid) {
    if (!ipc_initialized || !pid) return;
    int slot = mailbox_slot(pid);
    if (slot < 0) return;
    mutex_lock(&mailbox_table_lock);
    mailbox_t* mb = mailboxes[slot];
    if (mb && mb->owner_pid == pid) {
        mailboxes[slot] = NULL;
        kfree(mb);
    }
    mutex_unlock(&mailbox_table_lock);
}

static uint64_t ms_to_ticks(uint64_t ms) {
    /* timer.c runs the PIT at TIMER_FREQ = 1000Hz, i.e. 1 tick == 1ms. */
    uint64_t ticks = ms;
    if (ticks < 1) ticks = 1;
    return ticks;
}

/* Enqueue a message into dst_pid's mailbox. Used by ipc_send(),
 * ipc_respond() and ipc_notify(), which only differ in how they fill
 * in msg_type/msg_id. */
static int mailbox_enqueue(uint64_t dst_pid, uint64_t msg_type, uint64_t msg_id,
                            const void* data, uint64_t data_size) {
    if (!ipc_initialized) return IPC_ERROR_NOT_INITIALIZED;
    if (data_size > IPC_MAX_DATA_SIZE) return IPC_ERROR_DATA_TOO_LARGE;
    if (dst_pid == 0) return IPC_ERROR_INVALID_PID;

    mailbox_t* mb = mailbox_get_or_create(dst_pid);
    if (!mb) return IPC_ERROR_NOT_INITIALIZED;

    mutex_lock(&mb->lock);

    if (mb->count >= IPC_QUEUE_SIZE) {
        mutex_unlock(&mb->lock);
        return IPC_ERROR_QUEUE_FULL;
    }

    ipc_message_t* slot = &mb->entries[mb->tail];
    process_t* self = process_get_current();

    slot->src_pid = self ? self->pid : 0;
    slot->dst_pid = dst_pid;
    slot->msg_type = msg_type;
    slot->msg_id = msg_id;
    slot->data_size = data_size;
    if (data && data_size) {
        memcpy(slot->data, data, data_size);
    }

    mb->tail = (mb->tail + 1) % IPC_QUEUE_SIZE;
    mb->count++;
    mb->total_enqueued++;

    mutex_unlock(&mb->lock);

    /* Wake a blocked receiver, if any, AFTER releasing the mailbox lock
     * so the woken thread doesn't immediately contend on a lock we are
     * still holding. */
    sem_post(&mb->items);

    g_stats.total_messages++;
    return IPC_SUCCESS;
}

/* ---------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------- */

void ipc_init(void) {
    memset(mailboxes, 0, sizeof(mailboxes));
    mutex_init(&mailbox_table_lock);
    memset(&g_stats, 0, sizeof(g_stats));
    next_msg_id = 1;

#if IPC_HAVE_SHM
    memset(shm_regions, 0, sizeof(shm_regions));
    mutex_init(&shm_lock);
    next_shm_id = 1;
#endif

    ipc_initialized = true;
    serial_puts("[IPC] Initialized (mailbox-based message passing)\n");
}

bool ipc_is_initialized(void) {
    return ipc_initialized;
}

int ipc_send(uint64_t dst_pid, uint64_t msg_type, const void* data, uint64_t data_size) {
    uint64_t id = next_msg_id++;
    return mailbox_enqueue(dst_pid, msg_type, id, data, data_size);
}

int ipc_receive(uint64_t* src_pid, uint64_t* msg_type, void* data, uint64_t* data_size, uint64_t timeout) {
    if (!ipc_initialized) return IPC_ERROR_NOT_INITIALIZED;

    process_t* self = process_get_current();
    if (!self) return IPC_ERROR_INVALID_PID;

    mailbox_t* mb = mailbox_get_or_create(self->pid);
    if (!mb) return IPC_ERROR_NOT_INITIALIZED;

    if (timeout == IPC_WAIT_FOREVER) {
        sem_wait(&mb->items);
    } else {
        uint64_t deadline = get_timer_ticks() + ms_to_ticks(timeout);
        while (!sem_trywait(&mb->items)) {
            if (get_timer_ticks() >= deadline) {
                return IPC_ERROR_TIMEOUT;
            }
            thread_sleep(1);
        }
    }

    mutex_lock(&mb->lock);

    if (mb->count == 0) {
        /* Shouldn't happen (the semaphore count tracks mb->count 1:1),
         * but guard against it rather than reading a stale/garbage
         * queue slot. */
        mutex_unlock(&mb->lock);
        return IPC_ERROR_TIMEOUT;
    }

    ipc_message_t* slot = &mb->entries[mb->head];

    if (src_pid) *src_pid = slot->src_pid;
    if (msg_type) *msg_type = slot->msg_type;
    if (data_size) {
        uint64_t n = slot->data_size;
        if (data && n) memcpy(data, slot->data, n);
        *data_size = n;
    } else if (data && slot->data_size) {
        memcpy(data, slot->data, slot->data_size);
    }

    mb->head = (mb->head + 1) % IPC_QUEUE_SIZE;
    mb->count--;
    mb->total_dequeued++;

    mutex_unlock(&mb->lock);

    return IPC_SUCCESS;
}

int ipc_respond(uint64_t dst_pid, uint64_t original_msg_id, uint64_t status, const void* data, uint64_t data_size) {
    /* Wire format for a response payload: [uint64_t status][caller data...]
     * msg_id is set to original_msg_id so the original requester can
     * correlate the response with its request. */
    if (data_size > IPC_MAX_DATA_SIZE - sizeof(uint64_t)) {
        return IPC_ERROR_DATA_TOO_LARGE;
    }

    uint8_t buf[IPC_MAX_DATA_SIZE];
    memcpy(buf, &status, sizeof(uint64_t));
    if (data && data_size) {
        memcpy(buf + sizeof(uint64_t), data, data_size);
    }

    return mailbox_enqueue(dst_pid, IPC_MSG_RESPONSE, original_msg_id, buf, data_size + sizeof(uint64_t));
}

int ipc_notify(uint64_t dst_pid, uint64_t event_type, const void* data, uint64_t data_size) {
    /* Fire-and-forget event: msg_id carries the event_type so the
     * receiver can distinguish which event fired even though msg_type
     * is uniformly IPC_MSG_EVENT. */
    return mailbox_enqueue(dst_pid, IPC_MSG_EVENT, event_type, data, data_size);
}

int ipc_request_shared_memory(uint64_t size, uint64_t* shm_id) {
#if IPC_HAVE_SHM
    if (!shm_id) return IPC_ERROR_INVALID_PID;
    if (size == 0 || size > SHM_SLOT_SIZE) return IPC_ERROR_DATA_TOO_LARGE;

    process_t* self = process_get_current();
    if (!self || !self->page_dir) return IPC_ERROR_PERMISSION_DENIED;

    mutex_lock(&shm_lock);

    shm_region_t* region = NULL;
    uint64_t index = 0;
    for (uint64_t i = 0; i < SHM_MAX_REGIONS; i++) {
        if (!shm_regions[i].in_use) {
            region = &shm_regions[i];
            index = i;
            break;
        }
    }

    if (!region) {
        mutex_unlock(&shm_lock);
        return IPC_ERROR_QUEUE_FULL;
    }

    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t virt = SHM_VA_BASE + index * SHM_SLOT_SIZE;

    if (!shm_range_is_clear(virt, pages)) {
        mutex_unlock(&shm_lock);
        return IPC_ERROR_PERMISSION_DENIED;
    }

    uint64_t* phys_pages = (uint64_t*)kmalloc(pages * sizeof(uint64_t));
    if (!phys_pages) {
        mutex_unlock(&shm_lock);
        return IPC_ERROR_NOT_INITIALIZED;
    }
    memset(phys_pages, 0, pages * sizeof(uint64_t));

    /* We are already running in the caller's own address space (the
     * scheduler switches CR3 to the current process before any user
     * code, including this syscall path, runs), so we can map straight
     * into "current" here without an explicit directory switch. */
    for (uint64_t p = 0; p < pages; p++) {
        uint64_t page_va = virt + p * PAGE_SIZE;
        uint64_t phys = paging_alloc_page(PAGE_PRESENT | PAGE_RW | PAGE_USER);
        if (!phys) {
            /* Roll back any pages already mapped for this request. */
            shm_unmap_range(virt, p);
            for (uint64_t q = 0; q < p; q++) {
                if (phys_pages[q]) {
                    phys_free_page((phys_addr_t)phys_pages[q]);
                }
            }
            kfree(phys_pages);
            mutex_unlock(&shm_lock);
            return IPC_ERROR_NOT_INITIALIZED;
        }
        if (!paging_map_page(page_va, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            paging_free_page(phys);
            /* Roll back any pages already mapped for this request. */
            shm_unmap_range(virt, p);
            for (uint64_t q = 0; q < p; q++) {
                if (phys_pages[q]) {
                    phys_free_page((phys_addr_t)phys_pages[q]);
                }
            }
            kfree(phys_pages);
            mutex_unlock(&shm_lock);
            return IPC_ERROR_NOT_INITIALIZED;
        }
        /* paging_alloc_page() also identity-maps the frame at
         * virt==phys as a side effect; we only want it reachable via
         * the dedicated shm window, so drop that incidental alias
         * (the physical frame itself stays allocated/mapped at page_va). */
        if (phys != page_va) {
            paging_unmap_page(phys);
        }
        phys_pages[p] = phys;
    }

    region->in_use = true;
    region->id = next_shm_id++;
    region->size = size;
    region->pages = pages;
    region->virt_addr = virt;
    region->owner_pid = self->pid;
    region->flags = MK_SHM_READ | MK_SHM_WRITE;
    region->phys_pages = phys_pages;
    region->attached_count = 1;
    region->attached_pids[0] = self->pid;

    *shm_id = region->id;

    mutex_unlock(&shm_lock);
    return IPC_SUCCESS;
#else
    (void)size;
    if (shm_id) *shm_id = 0;
    return IPC_ERROR_NOT_INITIALIZED;
#endif
}

#if IPC_HAVE_SHM
/* Extra helper (not part of the original ipc.h contract): resolves a
 * shared memory id to the virtual address it is mapped at for a
 * process that has already created or attached it. See
 * ipc_attach_shared_memory() below for mapping a region created by a
 * *different* process into the caller's own address space. */
uint64_t ipc_shm_get_address(uint64_t shm_id) {
    uint64_t addr = 0;
    mutex_lock(&shm_lock);
    for (uint64_t i = 0; i < SHM_MAX_REGIONS; i++) {
        if (shm_regions[i].in_use && shm_regions[i].id == shm_id) {
            addr = shm_regions[i].virt_addr;
            break;
        }
    }
    mutex_unlock(&shm_lock);
    return addr;
}

/* Extension beyond the original API: attach an existing shared memory
 * region (created by another process via ipc_request_shared_memory)
 * into the CALLING process's own address space, returning the virtual
 * address it was mapped at.
 *
 * All processes use the same virtual address for a given region (the
 * region's fixed slot in the shm window), which is simplest and avoids
 * needing to track a different address per attacher - the region's
 * physical frames are what's actually being shared, the fact that
 * every attacher sees them at the same VA is just a convenience of
 * this allocator, not a requirement of the sharing mechanism itself.
 */
int ipc_attach_shared_memory(uint64_t shm_id, uint64_t* out_addr) {
    if (!out_addr) return IPC_ERROR_INVALID_PID;

    process_t* self = process_get_current();
    if (!self || !self->page_dir) return IPC_ERROR_PERMISSION_DENIED;

    mutex_lock(&shm_lock);

    shm_region_t* region = NULL;
    for (uint64_t i = 0; i < SHM_MAX_REGIONS; i++) {
        if (shm_regions[i].in_use && shm_regions[i].id == shm_id) {
            region = &shm_regions[i];
            break;
        }
    }

    if (!region) {
        mutex_unlock(&shm_lock);
        return IPC_ERROR_INVALID_PID;
    }

    /* Already attached (e.g. the owner calling this on its own
     * region, or a repeat attach) - just hand back the address. */
    for (uint64_t a = 0; a < region->attached_count; a++) {
        if (region->attached_pids[a] == self->pid) {
            mutex_unlock(&shm_lock);
            *out_addr = region->virt_addr;
            return IPC_SUCCESS;
        }
    }

    if (region->attached_count >= SHM_MAX_ATTACH) {
        mutex_unlock(&shm_lock);
        return IPC_ERROR_QUEUE_FULL;
    }

    if (!shm_range_is_clear(region->virt_addr, region->pages)) {
        mutex_unlock(&shm_lock);
        return IPC_ERROR_PERMISSION_DENIED;
    }

    /* Map each of the region's already-allocated physical frames into
     * our own (the caller's, i.e. "current") address space at the
     * same virtual address the owner uses. We're already running in
     * our own directory (the scheduler switches CR3 before any of
     * this code runs), so no directory switch is needed here either. */
    for (uint64_t p = 0; p < region->pages; p++) {
        uint64_t page_va = region->virt_addr + p * PAGE_SIZE;
        if (!paging_map_page(page_va, region->phys_pages[p], PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            /* Roll back whatever this attach attempt mapped so far.
             * Note: this only unmaps our own (the attacher's) entries -
             * the owner's and any other already-attached process's
             * mappings of the same physical frames are untouched. */
            shm_unmap_range(region->virt_addr, p);
            mutex_unlock(&shm_lock);
            return IPC_ERROR_NOT_INITIALIZED;
        }
    }

    region->attached_pids[region->attached_count++] = self->pid;
    *out_addr = region->virt_addr;

    mutex_unlock(&shm_lock);
    return IPC_SUCCESS;
}
#endif

void ipc_release_process_resources(uint64_t owner_pid) {
    if (!owner_pid || !ipc_initialized) return;
    mailbox_release_owner(owner_pid);
    shm_release_owner(owner_pid);
}

void ipc_process_messages(void) {
    /* No background/deferred work is required by this implementation:
     * ipc_send()/ipc_notify()/ipc_respond() deliver synchronously into
     * the destination mailbox and post its semaphore immediately, so
     * there is nothing queued "waiting to be processed" at the IPC
     * layer itself. Kept as a no-op entry point for callers that
     * expect to poll it periodically (e.g. a message pump in a main
     * loop) without needing to special-case this implementation. */
}

void ipc_get_stats(ipc_stats_t* stats) {
    if (!stats) return;

    g_stats.active_processes = 0;
    g_stats.blocked_processes = 0;
    g_stats.total_queue_size = 0;

    mutex_lock(&mailbox_table_lock);
    for (uint64_t i = 0; i < IPC_MAX_PROCESSES; i++) {
        mailbox_t* mb = mailboxes[i];
        if (mb) {
            g_stats.active_processes++;
            g_stats.total_queue_size += mb->count;
        }
    }
    mutex_unlock(&mailbox_table_lock);

    *stats = g_stats;
}
