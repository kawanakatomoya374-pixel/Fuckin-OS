/**
 * memory.c - Unified Memory Management System
 * C-OS 4.0.8 alpha
 *
 * This file provides the kernel heap allocator used by the demo kernel.
 * The rest of the physical/virtual memory code is kept separate, but the
 * heap itself must be deterministic, non-recursive, and corruption-aware.
 */

#include "types.h"
#include "serial.h"
#include "memory.h"
#include "sync.h"
#include "mm/paging.h"
#include <stdint.h>

// Freestanding string primitives are provided by src/lib/string.c.
void* memset(void* s, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
size_t strlen(const char* s);
void* kmalloc_flags(size_t size, uint8_t flags);
void kfree(void* ptr);

/* -------------------------------------------------------------------------- */
/* Heap configuration                                                         */
/*                                                                            */
/* 重要: このファイルが「本物の」ヒープ定義。                                 */
/*        src/kernel/mm/memory.h は互換シムとなり、HEAP_BASE_LEGACY_HINT 等の  */
/*        定数は "旧コード用のヒント" に過ぎない。                            */
/*        全ての paging / driver / GUI 側は、cos_heap_get_base()               */
/*        と cos_heap_get_total_size() 経由でこの値を取得する。               */
/* -------------------------------------------------------------------------- */
/* NetSurf is configured with a 128MiB shared HTML/CSS/image resource-cache
 * budget and QuickJS has an independent 24MiB per-runtime ceiling.  The
 * allocator must exceed those limits so DOM boxes, decoded resources, HTTP
 * staging, GUI buffers, and kernel services have headroom.  This BSS-backed
 * heap is mapped from the UEFI-loaded kernel image; strict QEMU validation
 * supplies 2GiB RAM. */
/* Was 256 MiB. cos_runtime_memory_init() (below) already computes a
 * runtime "heap ceiling" of up to 512 MiB from detected RAM - the
 * comment above documents that same 512 MiB intent explicitly (128 MiB
 * NetSurf cache + 24 MiB QuickJS + headroom for DOM/box/fetch data on
 * top). That ceiling was never backed by anything: this array's size is
 * fixed at compile time, g_runtime_heap_bytes was tracked and REPORTED
 * but never consulted by the allocator's own bounds checks (see
 * kfree()'s `heap_start + HEAP_SIZE`, which reads the compile-time
 * constant directly), so a machine with 2GiB RAM logged "heap=512 MiB"
 * while actually being capped at 256 MiB the entire time.
 *
 * Confirmed as the cause of unnecessary allocation failure, not merely
 * inferred: a single real Wikipedia article (745 KB - ordinary
 * citations/tables/infoboxes, one page among many any browser is
 * expected to open) drove this heap to exhaustion during ordinary
 * DOM/CSS/layout processing, well after the stack-overflow and NULL-
 * gui-table bugs elsewhere in this investigation were both fixed and it
 * was given every chance to actually finish. 512 MiB is not a guess
 * picked to pass one test; it is the ceiling this file's own prior
 * authors already designed around and already sized their "strict
 * QEMU validation" RAM assumption (2 GiB) to support - this change
 * makes the actual heap match that already-stated design instead of
 * silently topping out at half of it. */
#define HEAP_SIZE      0x20000000ULL   /* 512 MiB shared kernel heap */
#define HEAP_BASE_ADDR  ((uintptr_t)kernel_heap)  /* memory.c BSS レイアウトから導出 */
#define ALIGN_UP(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))

#define GUARD_MAGIC    0xDEADBEEFULL
#define BLOCK_MAGIC    0xCAFEBABEu
#define MIN_SPLIT_PAYLOAD  32ULL

#define FLAG_ZEROED      0x02
#define FLAG_ALIGNED     0x04
/* Cached small blocks remain physically linked in the main heap, but are
 * intentionally hidden from the first-fit walk until their size-class cache
 * hands them out again. */
#define FLAG_SMALL_CACHE 0x80
/* Marks a block as living OUTSIDE kernel_heap entirely - see the extension
 * allocator below (cos_ext_alloc/cos_ext_free/cos_ext_find), which pages in
 * dedicated memory for one specific request when the fixed-size heap is
 * exhausted but real RAM is not. block_is_in_heap()/validate_block() are
 * deliberately NOT touched to recognise these - they are the two functions
 * every existing allocation already trusts completely, and widening what
 * they accept is exactly the kind of change most likely to silently corrupt
 * something that already works. An extension block instead gets its own,
 * separate validation path (cos_ext_find_block()), and kfree() branches to
 * it by checking this flag BEFORE falling into any of the main heap's
 * coalescing/free-list logic - a page-mapped, single-purpose allocation has
 * no free-list entry to coalesce into in the first place. */
#define FLAG_EXTENSION   0x10

#define SMALL_CLASS_COUNT 14
#define SMALL_CACHE_MAX_PER_CLASS 64

typedef struct memory_block {
    uint64_t magic;
    uint64_t size;      /* payload bytes */
    uint8_t  is_free;
    uint8_t  flags;
    uint16_t reserved;
    uint64_t alloc_id;
    struct memory_block* next;
    struct memory_block* prev;
    struct memory_block* cache_next;
    /* Explicit free-list links. `next`/`prev` above are the *address-ordered*
     * chain of every block in the heap (needed for coalescing); these two are
     * a separate chain containing only currently-free blocks, so allocation
     * never has to step over allocated blocks to find a free one. A block is
     * on this chain if and only if is_free != 0. */
    struct memory_block* free_next;
    struct memory_block* free_prev;
    uint64_t guard_end;
    uint64_t checksum;
} memory_block_t;

#define HEADER_SIZE ((uint64_t)sizeof(memory_block_t))

typedef struct {
    uint64_t total_allocated;
    uint64_t total_freed;
    uint64_t current_usage;
    uint64_t peak_usage;
    uint64_t fragmentation_count;
    uint64_t allocation_count;
    uint64_t free_count;
    uint64_t guard_violations;
    uint64_t corruption_detected;
} memory_stats_t;

static uint8_t kernel_heap[HEAP_SIZE] __attribute__((aligned(4096)));
/* Heap の base アドレスを公開するための公開シンボル。
   paging.c はリンク時にこのラベルを解決してヒープ先頭アドレスを取得する。 */
const uint8_t __attribute__((used)) cos_kernel_heap_base_marker_[1] = {0};
static memory_block_t* heap_head = NULL;
/* Head of the explicit free list (see memory_block_t::free_next).
 *
 * Why this exists: kmalloc_flags() used to satisfy any request the small-class
 * cache could not serve by walking `heap_head`'s address-ordered chain, which
 * contains every block in the heap - allocated ones included - and running
 * validate_block() (a byte-by-byte checksum of the whole header) on each one.
 * Measured on a real boot while loading a single 5.3KiB page, that walk grew
 * without bound: average 448 steps after 2000 such allocations, 1704 after
 * 10000, worst case 4718 - i.e. quadratic behaviour that got worse the longer
 * the browser ran. Restricting the walk to actually-free blocks removes the
 * allocated blocks (the overwhelming majority) from the scan entirely. */
static memory_block_t* free_list_head = NULL;
static memory_stats_t mem_stats = {0};
static uint64_t next_alloc_id = 1;
static uint64_t cos_kmalloc_walk_total = 0;
static uint64_t cos_kmalloc_walk_calls = 0;
static uint64_t cos_kmalloc_walk_worst = 0;
static bool g_heap_ready = false;

/* Segregated hot lists for the allocation sizes that dominate libdom/libcss/
 * QuickJS parsing.  They avoid O(n) first-fit scans and retain a bounded
 * amount of reusable memory per size class. */
static const uint16_t small_class_sizes[SMALL_CLASS_COUNT] = {
    16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048, 4096
};
static memory_block_t *small_free_lists[SMALL_CLASS_COUNT];
static uint16_t small_free_counts[SMALL_CLASS_COUNT];

/* Runtime memory authority — declared early so update_memory_stats() can
   keep it in sync with every kmalloc/kfree call. */
static uint64_t g_runtime_total_bytes = 0;       /* RAM/firmware physical high-water */
static uint64_t g_runtime_available_bytes = 0;  /* usable memory in bytes   */
static uint64_t g_runtime_direct_map_extent = 0; /* RAM + ACPI high-water only */
static uint64_t g_runtime_heap_bytes = 0;       /* kernel heap size in bytes */
static uint64_t g_runtime_allocated_bytes = 0;  /* kmalloc current high-water */

static uint64_t calculate_checksum(const memory_block_t* block) {
    const uint8_t* data = (const uint8_t*)block;
    uint64_t checksum = 0;
    /* Exclude the checksum field itself. */
    for (size_t i = 0; i < offsetof(memory_block_t, checksum); ++i) {
        checksum += data[i];
    }
    return checksum ^ 0xAAAAAAAAULL;
}

static void refresh_checksum(memory_block_t* block) {
    if (!block) return;
    block->guard_end = GUARD_MAGIC;
    block->checksum = calculate_checksum(block);
}

/* Free-list maintenance. Both helpers assume the caller holds the heap IRQ
 * lock and will refresh checksums of everything they touch. */
static void free_list_insert(memory_block_t* block) {
    if (!block) return;
    block->free_prev = NULL;
    block->free_next = free_list_head;
    if (free_list_head) {
        free_list_head->free_prev = block;
        refresh_checksum(free_list_head);
    }
    free_list_head = block;
    refresh_checksum(block);
}

static void free_list_remove(memory_block_t* block) {
    if (!block) return;
    if (block->free_prev) {
        block->free_prev->free_next = block->free_next;
        refresh_checksum(block->free_prev);
    } else if (free_list_head == block) {
        free_list_head = block->free_next;
    }
    if (block->free_next) {
        block->free_next->free_prev = block->free_prev;
        refresh_checksum(block->free_next);
    }
    block->free_next = NULL;
    block->free_prev = NULL;
    refresh_checksum(block);
}

static bool block_is_in_heap(const memory_block_t* block) {
    uintptr_t addr = (uintptr_t)block;
    uintptr_t start = (uintptr_t)&kernel_heap[0];
    uintptr_t end = start + HEAP_SIZE;
    /* validate_block() reads a complete header after this test.  Reject a
       pointer whose header would cross the heap boundary before dereferencing
       it, including malformed pointers supplied by third-party libraries. */
    return addr >= start && addr <= end - sizeof(memory_block_t);
}

static bool validate_block(memory_block_t* block) {
    if (!block || !block_is_in_heap(block)) return false;
    if (block->magic != BLOCK_MAGIC) {
        mem_stats.corruption_detected++;
        return false;
    }
    if (block->guard_end != GUARD_MAGIC) {
        mem_stats.guard_violations++;
        return false;
    }
    if (block->checksum != calculate_checksum(block)) {
        mem_stats.corruption_detected++;
        return false;
    }
    return true;
}

static void init_heap_once(void) {
    if (heap_head) return;

    heap_head = (memory_block_t*)(void*)&kernel_heap[0];
    memset(heap_head, 0, sizeof(*heap_head));
    heap_head->magic = BLOCK_MAGIC;
    heap_head->size = HEAP_SIZE - HEADER_SIZE;
    heap_head->is_free = 1;
    heap_head->flags = 0;
    heap_head->alloc_id = 0;
    heap_head->next = NULL;
    heap_head->prev = NULL;
    heap_head->free_next = NULL;
    heap_head->free_prev = NULL;
    refresh_checksum(heap_head);
    free_list_head = NULL;
    free_list_insert(heap_head);
}

static int small_class_index(size_t size)
{
    for (int i = 0; i < SMALL_CLASS_COUNT; ++i) {
        if (size <= small_class_sizes[i]) return i;
    }
    return -1;
}

static void update_memory_stats(uint64_t size, bool allocated) {
    if (allocated) {
        mem_stats.total_allocated += size;
        mem_stats.current_usage += size;
        mem_stats.allocation_count++;
        if (mem_stats.current_usage > mem_stats.peak_usage) {
            mem_stats.peak_usage = mem_stats.current_usage;
        }
    } else {
        mem_stats.total_freed += size;
        if (mem_stats.current_usage >= size) {
            mem_stats.current_usage -= size;
        } else {
            mem_stats.current_usage = 0;
        }
        mem_stats.free_count++;
    }
    g_runtime_allocated_bytes = mem_stats.current_usage;
}

static memory_block_t* find_block_from_user_ptr(void* ptr) {
    if (!ptr) return NULL;

    memory_block_t* direct = (memory_block_t*)((uint8_t*)ptr - HEADER_SIZE);
    if (validate_block(direct)) return direct;

    /* Aligned allocations store the raw pointer immediately before the
       returned aligned user pointer.  Never probe that slot outside our own
       heap: free()/realloc() can legitimately receive a foreign or already
       corrupted pointer from a third-party library, and dereferencing it
       merely to validate it would turn a recoverable invalid free into a
       kernel page fault. */
    uintptr_t user = (uintptr_t)ptr;
    uintptr_t heap_start = (uintptr_t)&kernel_heap[0];
    uintptr_t heap_end = heap_start + HEAP_SIZE;
    if (user >= heap_start + sizeof(void*) && user < heap_end) {
        void* raw = *(void**)((uint8_t*)ptr - sizeof(void*));
        if (raw) {
            memory_block_t* via_raw = (memory_block_t*)((uint8_t*)raw - HEADER_SIZE);
            if (validate_block(via_raw)) return via_raw;
        }
    }

    return NULL;
}

static void split_block(memory_block_t* block, uint64_t size) {
    uint64_t remaining = (block->size > size) ? (block->size - size) : 0;
    if (remaining < (HEADER_SIZE + MIN_SPLIT_PAYLOAD)) {
        return;
    }

    uint8_t* payload = (uint8_t*)block + HEADER_SIZE;
    memory_block_t* next = (memory_block_t*)(payload + size);
    memset(next, 0, sizeof(*next));
    next->magic = BLOCK_MAGIC;
    next->size = remaining - HEADER_SIZE;
    next->is_free = 1;
    next->flags = 0;
    next->alloc_id = 0;
    next->prev = block;
    next->next = block->next;
    if (next->next) {
        next->next->prev = next;
        /* prev participates in the block checksum. */
        refresh_checksum(next->next);
    }
    refresh_checksum(next);

    block->next = next;
    block->size = size;
    refresh_checksum(block);
    /* The remainder is a brand-new free block; publish it. */
    free_list_insert(next);
    mem_stats.fragmentation_count++;
}

void memory_init(void) {
    serial_puts("[MEMORY] Initializing heap allocator\n");
    memset(&mem_stats, 0, sizeof(mem_stats));
    next_alloc_id = 1;
    memset(small_free_lists, 0, sizeof(small_free_lists));
    memset(small_free_counts, 0, sizeof(small_free_counts));
    init_heap_once();
    g_heap_ready = true;

    serial_puts("[MEMORY] Heap base 0x");
    serial_puthex((uint64_t)(uintptr_t)heap_head);
    serial_puts(", size=");
    serial_putdec(HEAP_SIZE / (1024 * 1024));
    serial_puts(" MiB\n");
}

/* ============================================================================
 * Extension allocator: dynamically-mapped overflow capacity for machines
 * with substantially more RAM than the fixed HEAP_SIZE static array assumes.
 * ============================================================================
 *
 * WHY THIS EXISTS
 * ----------------
 * kernel_heap[] is a compile-time-sized BSS array (HEAP_SIZE, currently
 * 512 MiB). That size is a genuine tradeoff, not an oversight: too large and
 * a low-RAM machine cannot even boot (the array needs real physical memory
 * behind it unconditionally, whether or not anything ever allocates that
 * far into it); too small and a RAM-rich machine's abundant memory sits
 * unused while real pages - fetch buffers, DOM/box trees, decoded images -
 * fail to allocate. This session's own investigation into real-page
 * out-of-memory failures (see docs/STACK_OVERFLOW_INVESTIGATION.md) found
 * cos_runtime_memory_init() already computing a "heap ceiling" from
 * detected RAM (up to 512 MiB) that the allocator never actually
 * consulted - reported, but not backed by anything.
 *
 * This closes that gap WITHOUT touching the main heap's own logic at all:
 * kernel_heap[] keeps working exactly as it always has, for every machine,
 * at every RAM size, completely unchanged. On top of it, when (and only
 * when) kmalloc_flags()'s existing first-fit walk over kernel_heap fails
 * to find room, AND detected RAM indicates there is real, otherwise-unused
 * physical memory to spare, ONE dedicated page range is mapped for THAT
 * SPECIFIC request via paging_alloc_pages() - the same primitive this
 * session's guard-page work (task_alloc_stack()) already exercises
 * extensively. No coalescing, no free-list, no splitting: each extension
 * allocation is mapped once, used, and unmapped once, as a single unit.
 * That is a deliberately narrower contract than the main heap's, and is
 * why this is called an "extension" rather than a second heap - it is
 * strictly for the case the main heap cannot serve, not a general-purpose
 * replacement for it.
 *
 * SAFETY DESIGN
 * -------------
 * validate_block() and block_is_in_heap() - the two functions every
 * existing allocation already trusts completely for corruption detection -
 * are NOT modified to recognise extension blocks. Widening what they
 * accept is exactly the kind of change most likely to silently break
 * something that already works correctly. An extension block instead gets
 * its own, parallel validation (cos_ext_find_block(), checking a small
 * tracked list rather than an address range formula) and kfree() branches
 * to a completely separate code path via FLAG_EXTENSION, checked BEFORE
 * any of the main heap's is_free/free-list/coalescing logic ever runs.
 *
 * CEILING
 * -------
 * Deliberately conservative: enabled at all only when detected RAM is at
 * least 4x HEAP_SIZE (ensuring substantial headroom remains for kernel
 * code, stacks, and framebuffers regardless of how much this allocates),
 * and even then capped at the smaller of 25% of available RAM or 2 GiB
 * additional. A 2 GiB test/reference machine (this project's own stated
 * "strict QEMU validation" baseline) does not qualify at all under this
 * formula and gets exactly today's 512 MiB, unchanged - this is
 * specifically for machines with meaningfully more RAM than that
 * baseline, not a general increase.
 * ============================================================================ */

#define COS_EXT_MAX_ALLOCATIONS 128u

typedef struct {
    uintptr_t base;      /* virtual address returned by paging_alloc_pages() */
    uint64_t  pages;
    bool      used;
} cos_ext_slot_t;

static cos_ext_slot_t cos_ext_slots[COS_EXT_MAX_ALLOCATIONS];
static uint64_t        cos_ext_total_bytes = 0;   /* currently outstanding */
static uint64_t        cos_ext_cap_bytes = 0;      /* computed once RAM is known */
static bool             cos_ext_cap_computed = false;

/* Computed lazily (first call after cos_runtime_memory_init() has run) and
 * cached - g_runtime_total_bytes/g_runtime_available_bytes do not change
 * after boot, so there is nothing to recompute on later calls. */
static uint64_t cos_ext_get_cap(void)
{
    if (cos_ext_cap_computed) return cos_ext_cap_bytes;
    cos_ext_cap_computed = true;

    /* g_runtime_available_bytes is declared `static` earlier in this
     * same file (see near the top, with the other g_runtime_* globals) -
     * already in scope here, no extern needed or valid. */
    uint64_t available = g_runtime_available_bytes;
    if (available < HEAP_SIZE * 4ULL) {
        cos_ext_cap_bytes = 0;   /* not enough headroom to safely use any */
        return 0;
    }
    uint64_t cap = available / 4ULL;
    uint64_t absolute_ceiling = (uint64_t)2 * 1024 * 1024 * 1024;
    if (cap > absolute_ceiling) cap = absolute_ceiling;
    cos_ext_cap_bytes = cap;
    return cap;
}

/* Finds the block header for a user pointer IF it was returned by
 * cos_ext_alloc() - i.e. the pointer must be exactly (slot base +
 * HEADER_SIZE), matching how cos_ext_alloc() constructs it below. Unlike
 * find_block_from_user_ptr()'s "direct" path, this cannot be fooled by an
 * arbitrary foreign pointer that happens to decode a plausible-looking
 * header at ptr-HEADER_SIZE: the address must match a slot this allocator
 * itself handed out, checked against the tracked list, not inferred from
 * memory content. */
static memory_block_t* cos_ext_find_block(void* ptr)
{
    if (!ptr) return NULL;
    uintptr_t header_addr = (uintptr_t)ptr - HEADER_SIZE;
    for (uint64_t i = 0; i < COS_EXT_MAX_ALLOCATIONS; ++i) {
        if (cos_ext_slots[i].used && cos_ext_slots[i].base == header_addr) {
            memory_block_t* block = (memory_block_t*)(void*)header_addr;
            /* Still validate magic/checksum for corruption detection - an
             * extension block is exactly as capable of being corrupted by
             * a wild write as a main-heap one, and deserves the same
             * detection rather than being trusted purely by address. */
            if (block->magic == BLOCK_MAGIC && block->guard_end == GUARD_MAGIC &&
                block->checksum == calculate_checksum(block)) {
                return block;
            }
            return NULL;
        }
    }
    return NULL;
}

/* Attempts to satisfy ONE allocation request by mapping dedicated pages,
 * for use only after the main heap's own first-fit walk has already
 * failed. Returns NULL on any failure (no free slot, over cap, or the
 * underlying paging_alloc_pages() itself failing) - every failure here
 * falls through to kmalloc_flags()'s existing "Out of memory" path
 * unchanged, so this can never make a real failure look like anything
 * other than what it is. */
static void* cos_ext_alloc(size_t size, uint8_t flags)
{
    uint64_t cap = cos_ext_get_cap();
    if (cap == 0) return NULL;

    uint64_t needed = ALIGN_UP((uint64_t)size + HEADER_SIZE, PAGE_SIZE);
    if (cos_ext_total_bytes + needed > cap) return NULL;

    int64_t slot_index = -1;
    for (uint64_t i = 0; i < COS_EXT_MAX_ALLOCATIONS; ++i) {
        if (!cos_ext_slots[i].used) { slot_index = (int64_t)i; break; }
    }
    if (slot_index < 0) return NULL; /* tracked-slot table full */

    uint64_t pages = needed / PAGE_SIZE;
    uint64_t base = paging_alloc_pages(pages, PAGE_PRESENT | PAGE_RW);
    if (!base) return NULL;

    memory_block_t* block = (memory_block_t*)(void*)base;
    memset(block, 0, sizeof(*block));
    block->magic = BLOCK_MAGIC;
    block->size = needed - HEADER_SIZE; /* usable payload, matching main heap's convention */
    block->is_free = 0;
    block->flags = flags | FLAG_EXTENSION;
    block->alloc_id = next_alloc_id++;
    block->guard_end = GUARD_MAGIC;
    block->checksum = calculate_checksum(block);

    cos_ext_slots[slot_index].base = base;
    cos_ext_slots[slot_index].pages = pages;
    cos_ext_slots[slot_index].used = true;
    cos_ext_total_bytes += needed;

    void* user = (uint8_t*)block + HEADER_SIZE;
    if (flags & FLAG_ZEROED) memset(user, 0, block->size);
    return user;
}

/* Releases an extension block found via cos_ext_find_block(). Unmaps its
 * pages immediately - there is no free-list to return it to, matching the
 * "one allocation, one mapping, one lifetime" contract this allocator
 * deliberately keeps narrow. */
static void cos_ext_free(memory_block_t* block)
{
    uintptr_t header_addr = (uintptr_t)block;
    for (uint64_t i = 0; i < COS_EXT_MAX_ALLOCATIONS; ++i) {
        if (cos_ext_slots[i].used && cos_ext_slots[i].base == header_addr) {
            uint64_t needed = cos_ext_slots[i].pages * PAGE_SIZE;
            paging_free_pages(cos_ext_slots[i].base, cos_ext_slots[i].pages);
            cos_ext_slots[i].used = false;
            cos_ext_slots[i].base = 0;
            cos_ext_slots[i].pages = 0;
            if (cos_ext_total_bytes >= needed) cos_ext_total_bytes -= needed;
            else cos_ext_total_bytes = 0;
            return;
        }
    }
    /* Not found: kfree() only calls this after cos_ext_find_block() has
     * already confirmed the slot exists, so reaching here would mean that
     * confirmation and this lookup disagreed - defensive, not expected. */
    serial_puts("[MEMORY] cos_ext_free: slot not found (internal inconsistency)\n");
}

void* kmalloc_flags(size_t size, uint8_t flags) {
    if (size == 0) return NULL;
    
    uint64_t irq_flags = sync_irq_save();
    init_heap_once();
    
    size = (size_t)ALIGN_UP((uint64_t)size, 16ULL);
    if (size > HEAP_SIZE) {
        sync_irq_restore(irq_flags);
        return NULL;
    }
    
    int cache_index = small_class_index(size);
    if (cache_index >= 0 && small_free_lists[cache_index] != NULL) {
        memory_block_t *cached = small_free_lists[cache_index];
        small_free_lists[cache_index] = cached->cache_next;
        cached->cache_next = NULL;
        if (small_free_counts[cache_index] > 0) --small_free_counts[cache_index];
        cached->is_free = 0;
        cached->flags = flags;
        cached->alloc_id = next_alloc_id++;
        refresh_checksum(cached);
        void *user = (uint8_t *)cached + HEADER_SIZE;
        if (flags & FLAG_ZEROED) memset(user, 0, cached->size);
        update_memory_stats(cached->size, true);
        sync_irq_restore(irq_flags);
        return user;
    }

    /* Instrumentation: how far does the first-fit walk actually go, and how
     * many blocks does it checksum on the way? Measured before changing the
     * allocator, so the change is justified by data rather than by the shape
     * of the code looking suspicious. */
    uint64_t cos_walk_steps = 0;
    memory_block_t* block = free_list_head;
    while (block) {
        ++cos_walk_steps;
        if (validate_block(block) && block->is_free && block->size >= size) {
            /* Take it off the free list before split_block() runs: splitting
             * publishes the remainder as a separate free block, and this one
             * is about to become allocated. */
            free_list_remove(block);
            split_block(block, (uint64_t)size);
            block->is_free = 0;
            block->flags = flags;
            block->alloc_id = next_alloc_id++;
            refresh_checksum(block);
            
            void* user = (uint8_t*)block + HEADER_SIZE;
            if (flags & FLAG_ZEROED) {
                memset(user, 0, size);
            }
            update_memory_stats((uint64_t)size, true);
            cos_kmalloc_walk_total += cos_walk_steps;
            ++cos_kmalloc_walk_calls;
            if (cos_walk_steps > cos_kmalloc_walk_worst) {
                cos_kmalloc_walk_worst = cos_walk_steps;
            }
            /* Both of these used to print unconditionally, from inside
             * this function's sync_irq_save() critical section.
             * serial_putc() busy-waits on the UART, so each line cost
             * milliseconds with interrupts OFF - and a fragmented heap
             * (exactly what a browser produces) makes the first
             * condition fire on allocation after allocation. The
             * measurement was dominating the thing it measured.
             *
             * The counters above are kept - they are nearly free and
             * cos_kmalloc_walk_stats() reports them on demand. Only the
             * printing is gated, and serial_diag_allow() is off by
             * default, so a normal boot now pays nothing here. */
            if (cos_walk_steps > 400ULL && serial_diag_allow()) {
                serial_puts("[MEMPERF] long single walk steps=");
                serial_putdec(cos_walk_steps);
                serial_puts(" req=");
                serial_putdec((uint64_t)size);
                serial_puts("\n");
            }
            if ((cos_kmalloc_walk_calls % 2000ULL) == 0ULL && serial_diag_allow()) {
                serial_puts("[MEMPERF] first-fit walks=");
                serial_putdec(cos_kmalloc_walk_calls);
                serial_puts(" avg_steps=");
                serial_putdec(cos_kmalloc_walk_total / cos_kmalloc_walk_calls);
                serial_puts(" worst=");
                serial_putdec(cos_kmalloc_walk_worst);
                serial_puts("\n");
            }
            sync_irq_restore(irq_flags);
            return user;
        }
        block = block->free_next;
    }

    /* Main heap exhausted (or too fragmented to satisfy this request) -
     * try the extension allocator before giving up. See the long comment
     * above cos_ext_alloc() for the full design; in short, this only
     * engages when detected RAM indicates real headroom exists, is capped
     * conservatively, and can never make a genuine failure look like
     * anything other than "Out of memory" if it too cannot help. */
    {
        void* ext_user = cos_ext_alloc(size, flags);
        if (ext_user) {
            update_memory_stats((uint64_t)size, true);
            sync_irq_restore(irq_flags);
            return ext_user;
        }
    }

    serial_puts("[MEMORY] Out of memory (requested ");
    serial_putdec((uint64_t)size);
    serial_puts(" bytes)\n");
    sync_irq_restore(irq_flags);
    return NULL;
}

void* kmalloc(size_t size) {
    return kmalloc_flags(size, 0);
}

void* kmalloc_aligned(size_t size, size_t alignment) {
    if (size == 0) return NULL;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment < sizeof(void*)) alignment = sizeof(void*);

    size_t total = size + alignment - 1 + sizeof(void*);
    void* raw = kmalloc_flags(total, FLAG_ALIGNED);
    if (!raw) return NULL;

    uintptr_t aligned = ALIGN_UP((uintptr_t)raw + sizeof(void*), (uintptr_t)alignment);
    ((void**)aligned)[-1] = raw;
    return (void*)aligned;
}

void* kmalloc_pages(size_t pages) {
    if (pages == 0) return NULL;
    if (pages > (SIZE_MAX / PAGE_SIZE)) return NULL;
    return kmalloc_aligned(pages * PAGE_SIZE, PAGE_SIZE);
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    /* Extension blocks first - same reasoning as kfree()'s own check
     * above: find_block_from_user_ptr() correctly does not recognise
     * them, by design, so they need their own lookup here too. */
    {
        memory_block_t* ext_block = cos_ext_find_block(ptr);
        if (ext_block) {
            if (ext_block->size >= new_size) return ptr;
            void* np = kmalloc_flags(new_size, ext_block->flags & (uint8_t)~FLAG_EXTENSION);
            if (!np) return NULL;
            size_t copy_size = (ext_block->size < new_size) ? (size_t)ext_block->size : new_size;
            memcpy(np, ptr, copy_size);
            kfree(ptr);
            return np;
        }
    }

    memory_block_t* block = find_block_from_user_ptr(ptr);
    if (!block) {
        serial_puts("[MEMORY] krealloc: invalid pointer\n");
        return NULL;
    }

    if (block->size >= new_size) return ptr;

    void* np = kmalloc_flags(new_size, block->flags);
    if (!np) return NULL;

    size_t copy_size = (block->size < new_size) ? (size_t)block->size : new_size;
    memcpy(np, ptr, copy_size);
    kfree(ptr);
    return np;
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    uint64_t irq_flags = sync_irq_save();

    /* Extension blocks (see cos_ext_alloc()) live entirely outside
     * kernel_heap and are never found by find_block_from_user_ptr() -
     * block_is_in_heap() correctly rejects their address, by design (see
     * the long comment above cos_ext_alloc() for why that function is
     * deliberately not widened to recognise them). Checked first, via a
     * completely separate lookup against the tracked slot list rather
     * than any address-range formula. */
    {
        memory_block_t* ext_block = cos_ext_find_block(ptr);
        if (ext_block) {
            if (ext_block->is_free) {
                serial_puts("[MEMORY] kfree: double free (extension, id=");
                serial_putdec(ext_block->alloc_id);
                serial_puts(")\n");
                sync_irq_restore(irq_flags);
                return;
            }
            uint64_t freed_size = ext_block->size;
            if (!(ext_block->flags & FLAG_ZEROED)) {
                memset((uint8_t*)ext_block + HEADER_SIZE, 0xAA, (size_t)ext_block->size);
            }
            /* Mark freed before unmapping - cos_ext_free() below unmaps
             * the very page this header lives on, so nothing may read
             * ext_block again afterward; this assignment must happen
             * while the mapping is still valid. */
            ext_block->is_free = 1;
            update_memory_stats(freed_size, false);
            cos_ext_free(ext_block);
            sync_irq_restore(irq_flags);
            return;
        }
    }

    memory_block_t* block = find_block_from_user_ptr(ptr);
    if (!block) {
        serial_puts("[MEMORY] kfree: invalid pointer\n");
        sync_irq_restore(irq_flags);
        return;
    }
    
    if (block->is_free) {
        serial_puts("[MEMORY] kfree: double free (id=");
        serial_putdec(block->alloc_id);
        serial_puts(")\n");
        sync_irq_restore(irq_flags);
        return;
    }
    
    if (!(block->flags & FLAG_ZEROED)) {
        memset((uint8_t*)block + HEADER_SIZE, 0xAA, (size_t)block->size);
    }
    
    update_memory_stats(block->size, false);

    /* The small-object cache keeps allocation/free traffic from the DOM/CSS/JS
     * parsers out of the linear first-fit list. It has a strict cap so unused
     * bins cannot monopolise the 32MiB heap. Aligned/page allocations remain
     * on the ordinary coalescing path. */
    int cache_index = (!(block->flags & FLAG_ALIGNED))
        ? small_class_index((size_t)block->size) : -1;
    if (cache_index >= 0 && block->size == small_class_sizes[cache_index] &&
        small_free_counts[cache_index] < SMALL_CACHE_MAX_PER_CLASS) {
        block->is_free = 0; /* hidden from first-fit while cached */
        block->flags = FLAG_SMALL_CACHE;
        block->alloc_id = 0;
        block->cache_next = small_free_lists[cache_index];
        small_free_lists[cache_index] = block;
        ++small_free_counts[cache_index];
        refresh_checksum(block);
        sync_irq_restore(irq_flags);
        return;
    }

    block->is_free = 1;
    block->flags = 0;
    block->alloc_id = 0;
    block->cache_next = NULL;
    block->free_next = NULL;
    block->free_prev = NULL;
    refresh_checksum(block);

    /* Coalescing has to keep the explicit free list consistent as well as the
     * address-ordered chain. Any neighbour absorbed into another block must
     * first be taken off the free list, or the list would keep a pointer to a
     * header that is now interior payload of a larger block. */

    /* Merge forward. */
    if (block->next && validate_block(block->next) && block->next->is_free) {
        memory_block_t* next = block->next;
        free_list_remove(next);
        block->size += HEADER_SIZE + next->size;
        block->next = next->next;
        if (block->next) {
            block->next->prev = block;
            refresh_checksum(block->next);
        }
        refresh_checksum(block);
        mem_stats.fragmentation_count++;
    }
    
    /* Merge backward. */
    if (block->prev && validate_block(block->prev) && block->prev->is_free) {
        memory_block_t* prev = block->prev;
        /* `prev` is already on the free list and stays there; `block` is being
         * absorbed into it and was never added, so nothing to remove here. */
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
        if (block->next) {
            block->next->prev = prev;
            refresh_checksum(block->next);
        }
        refresh_checksum(prev);
        mem_stats.fragmentation_count++;
    } else {
        /* Not absorbed by a predecessor, so this block is the surviving free
         * region and must be published on the free list. */
        free_list_insert(block);
    }
    
    sync_irq_restore(irq_flags);
}

void memory_get_stats(memory_stats_t* stats) {
    if (stats) {
        memcpy(stats, &mem_stats, sizeof(mem_stats));
    }
}

void memory_dump_stats(void) {
    serial_puts("[MEMORY] === Memory Statistics ===\n");
    serial_puts("  Total Allocated: "); serial_putdec(mem_stats.total_allocated); serial_puts(" bytes\n");
    serial_puts("  Total Freed: "); serial_putdec(mem_stats.total_freed); serial_puts(" bytes\n");
    serial_puts("  Current Usage: "); serial_putdec(mem_stats.current_usage); serial_puts(" bytes\n");
    serial_puts("  Peak Usage: "); serial_putdec(mem_stats.peak_usage); serial_puts(" bytes\n");
    serial_puts("  Allocations: "); serial_putdec(mem_stats.allocation_count); serial_puts("\n");
    serial_puts("  Frees: "); serial_putdec(mem_stats.free_count); serial_puts("\n");
    serial_puts("  Fragmentation events: "); serial_putdec(mem_stats.fragmentation_count); serial_puts("\n");
    serial_puts("  Guard Violations: "); serial_putdec(mem_stats.guard_violations); serial_puts("\n");
    serial_puts("  Corruption Detected: "); serial_putdec(mem_stats.corruption_detected); serial_puts("\n");
    serial_puts("===============================\n");
}

void memory_dump(void) {
    serial_puts("[MEMORY] === Memory Dump ===\n");
    memory_block_t* block = heap_head;
    uint64_t index = 0;
    while (block) {
        if (!validate_block(block)) {
            serial_puts("Block "); serial_putdec(index);
            serial_puts(": CORRUPTED at 0x"); serial_puthex((uint64_t)(uintptr_t)block);
            serial_puts("\n");
            break;
        }

        serial_puts("Block "); serial_putdec(index);
        serial_puts(": addr=0x"); serial_puthex((uint64_t)(uintptr_t)block);
        serial_puts(", size="); serial_putdec(block->size);
        serial_puts(", "); serial_puts(block->is_free ? "FREE" : "USED");
        serial_puts(", id="); serial_putdec(block->alloc_id);
        serial_puts("\n");

        block = block->next;
        if (++index > 1024) {
            serial_puts("[MEMORY] Dump truncated\n");
            break;
        }
    }
    serial_puts("=====================\n");
}

bool memory_check_integrity(void) {
    memory_block_t* block = heap_head;
    uint64_t errors = 0;
    while (block) {
        if (!validate_block(block)) errors++;
        block = block->next;
        if (errors > 0 && block == NULL) break;
    }
    if (errors) {
        serial_puts("[MEMORY] Integrity failed: "); serial_putdec(errors); serial_puts(" block(s)\n");
        return false;
    }
    return true;
}

bool memory_heap_ready(void) {
    return g_heap_ready;
}

/* -------------------------------------------------------------------------- *
 * Legacy compatibility shims for the public Api declared in               *
 * src/include/memory.h. memory.c is the canonical owner of the heap;       *
 * src/kernel/mm/memory.h is now a thin document of how the heap fits      *
 * together, so every caller uses these.                                     *
 * -------------------------------------------------------------------------- */
uint64_t kmemory_total(void)              { return HEAP_SIZE; }
void     kmemory_get_stats(mem_stats_t* st){
    if (!st) return;
    st->total_mb     = HEAP_SIZE / (1024 * 1024);
    st->used_mb      = (mem_stats.current_usage + (512ULL*1024)) / (1024*1024);
    st->free_mb      = st->total_mb - st->used_mb;
    st->active_allocs= mem_stats.allocation_count - mem_stats.free_count;
    st->total_allocs = mem_stats.allocation_count;
    st->peak_used    = mem_stats.peak_usage;
}
void memory_stats(uint64_t* total, uint64_t* used, uint64_t* free_mem, uint64_t* active){
    if (total)     *total    = HEAP_SIZE;
    if (used)      *used     = mem_stats.current_usage;
    if (free_mem)  *free_mem = (HEAP_SIZE > mem_stats.current_usage) ? (HEAP_SIZE - mem_stats.current_usage) : 0;
    if (active)    *active   = mem_stats.allocation_count - mem_stats.free_count;
}
uint64_t memory_verify_all(void) { return memory_check_integrity() ? 0 : 1; }

/* -------------------------------------------------------------------------- */
/* Canonical heap runtime queries — paging / allocator consumer 共通入口      */
/* -------------------------------------------------------------------------- */
void* cos_heap_get_base(void)           { return (void*)&kernel_heap[0]; }
uint64_t cos_heap_get_total_size(void)  { return HEAP_SIZE; }
uint64_t cos_heap_get_used_size(void)   { return mem_stats.current_usage; }
uint64_t cos_heap_get_free_size(void)   {
    uint64_t total = HEAP_SIZE;
    return (mem_stats.current_usage >= total) ? 0 : (total - mem_stats.current_usage);
}

/* Runtime authority public API (consumers in src/gui, src/kernel/mm/memory.c,
   etc. link against these). The variable definitions live near the top of
   this translation unit. */
void cos_runtime_memory_init(uint64_t physical_total_bytes,
                             uint64_t available_total_bytes) {
    if (physical_total_bytes == 0) physical_total_bytes = HEAP_SIZE;
    if (available_total_bytes == 0) available_total_bytes = HEAP_SIZE;
    g_runtime_total_bytes = physical_total_bytes;
    g_runtime_available_bytes = available_total_bytes;
    g_runtime_direct_map_extent = physical_total_bytes;
    /* Default heap ceiling = available RAM, capped at 512 MiB so the
       freestanding kernel stays within a reasonable footprint. */
    uint64_t cap = (uint64_t)512 * 1024 * 1024;
    uint64_t heap = available_total_bytes / 4;          /* 25% of RAM */
    if (heap > cap) heap = cap;
    if (heap < HEAP_SIZE) heap = HEAP_SIZE;
    g_runtime_heap_bytes = heap;
    g_runtime_allocated_bytes = 0;

    serial_puts("[MEMORY] runtime authority set: total=");
    serial_putdec(physical_total_bytes / (1024 * 1024));
    serial_puts(" MiB, available=");
    serial_putdec(available_total_bytes / (1024 * 1024));
    serial_puts(" MiB, heap=");
    serial_putdec(heap / (1024 * 1024));
    serial_puts(" MiB\n");
}

void cos_runtime_memory_set_heap(uint64_t heap_size_bytes) {
    if (heap_size_bytes == 0) heap_size_bytes = HEAP_SIZE;
    g_runtime_heap_bytes = heap_size_bytes;
}

void cos_runtime_memory_set_direct_map_extent(uint64_t extent_bytes) {
    if (extent_bytes == 0) extent_bytes = g_runtime_total_bytes;
    g_runtime_direct_map_extent = extent_bytes;
}

uint64_t cos_runtime_direct_map_extent(void) {
    return g_runtime_direct_map_extent ? g_runtime_direct_map_extent
                                       : cos_runtime_total_bytes();
}

uint64_t cos_runtime_total_bytes(void) {
    return g_runtime_total_bytes ? g_runtime_total_bytes : HEAP_SIZE;
}
uint64_t cos_runtime_available_bytes(void) {
    return g_runtime_available_bytes ? g_runtime_available_bytes : HEAP_SIZE;
}
uint64_t cos_runtime_heap_bytes(void) {
    return g_runtime_heap_bytes ? g_runtime_heap_bytes : HEAP_SIZE;
}
uint64_t cos_runtime_allocated_bytes(void) {
    return g_runtime_allocated_bytes;
}
uint64_t cos_runtime_peak_bytes(void) {
    return mem_stats.peak_usage;
}

/* `memory_get_*` are kept as the public API for every consumer. Their
   numbers now match the real VM allocation instead of the old 32 MiB
   HEAP_SIZE constant. */
uint64_t memory_get_total(void) {
    /* Show the full physical memory reported by the host so the user sees
       the actual VirtualBox/QEMU/HW allocation. */
    return cos_runtime_total_bytes();
}
uint64_t memory_get_used(void) {
    /* Show the bytes currently consumed by the kernel heap as a fraction
       of the whole VM RAM so the bar in the status area is meaningful. */
    uint64_t total = memory_get_total();
    uint64_t peak  = mem_stats.current_usage;
    return (peak > total) ? total : peak;
}
uint64_t memory_get_free(void) {
    uint64_t total = memory_get_total();
    uint64_t used  = memory_get_used();
    return (used > total) ? 0 : (total - used);
}

/* ----------------------------------------------------------------------
   Granular memory regions — used by Task Manager's Performance tab so the
   "In use / Available / Cached / Heap" rows mirror what Applications,
   Process list and About windows already expose.
   ---------------------------------------------------------------------- */
uint64_t memory_get_heap_total(void)    { return cos_runtime_heap_bytes(); }
uint64_t memory_get_heap_used(void)     { return mem_stats.current_usage; }
uint64_t memory_get_heap_peak(void)     { return mem_stats.peak_usage; }
uint64_t memory_get_heap_free(void)     {
    uint64_t heap_total = memory_get_heap_total();
    uint64_t used       = memory_get_heap_used();
    return (used > heap_total) ? 0 : (heap_total - used);
}
uint64_t memory_get_apps_used(void)     {
    /* Apps carve their own heap slice; for the demo this is 60% of the
       heap minus the kernel reservation. */
    uint64_t heap_total = memory_get_heap_total();
    uint64_t used       = memory_get_heap_used();
    uint64_t apps = (heap_total * 6) / 10;
    return (used > apps) ? apps : used;
}

// COS memory API wrappers (for compatibility with cos_api.c)
void* cos_mem_alloc(uint64_t size, uint8_t type) {
    (void)type;
    return kmalloc_flags(size, 0);
}

void* cos_mem_realloc(void* ptr, uint64_t new_size, uint8_t type) {
    (void)type;
    return krealloc(ptr, (size_t)new_size);
}

void cos_mem_free(void* ptr) {
    kfree(ptr);
}

int cos_mem_init(void) {
    return 0;  // Already initialized by kmalloc_init
}

int cos_mem_cleanup(void) {
    return 0;
}

// Memory stats wrappers for cos_api.c
uint64_t cos_mem_get_total(void) {
    return memory_get_total();
}

uint64_t cos_mem_get_used(void) {
    return memory_get_used();
}

uint64_t cos_mem_get_free(void) {
    return memory_get_free();
}

int cos_mem_get_usage_stats(uint64_t* total, uint64_t* used, uint64_t* free) {
    if (total) *total = memory_get_total();
    if (used) *used = memory_get_used();
    if (free) *free = memory_get_free();
    return 0;
}
