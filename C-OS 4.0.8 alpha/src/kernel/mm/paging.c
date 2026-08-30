/**
 * paging.c - Virtual Memory / Paging Implementation
 * C-OS 4.0.8 alpha
 *
 * Uses the x86-64 4-level hierarchy: PML4 -> PDPT -> PD -> PT (4KB pages).
 * The header (paging.h) exports a "page_directory_t" / "page_table_t" API
 * that was written for a flat 32-bit design.  We keep those typedefs as
 * aliases so callers compile cleanly, but internally we work with the full
 * 64-bit walk.
 */

#include "paging.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "sync.h"

/* Physical memory manager is optional at early boot. */
extern phys_addr_t phys_alloc_page(void);
extern void phys_free_page(phys_addr_t addr);
extern bool phys_is_valid_addr(phys_addr_t addr);
extern uint64_t phys_get_total_memory(void);
extern uint8_t _kernel_end;

/* =========================================================
 * Internal 64-bit page-table level type (512 entries)
 * ========================================================= */
typedef struct { uint64_t e[512]; } pt_level_t;

/* =========================================================
 * Flags
 * ========================================================= */
#define F_PRESENT   PAGE_PRESENT        /* 0x001 */
#define F_RW        PAGE_RW             /* 0x002 */
#define F_USER      PAGE_USER           /* 0x004 */
#define F_PS        0x080               /* Page size (2 MiB) */
#define F_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define F_HUGE_ADDR_MASK 0x000FFFFFFFE00000ULL

/* =========================================================
 * Globals
 * ========================================================= */
static pt_level_t        *kernel_pml4       = NULL;
static page_directory_t  *current_directory = NULL;
static page_fault_handler_t g_fault_handler = NULL;
static page_stats_t       stats;

uint64_t paging_virt_to_phys(uint64_t v);

static inline pt_level_t *paging_root(void) {
    return current_directory ? (pt_level_t *)current_directory : kernel_pml4;
}

static inline uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + a - 1ULL) & ~(a - 1ULL);
}

/*
 * Early boot paging code runs before the HHDM is established.
 * Page-table pages are allocated from the identity-mapped low kernel heap,
 * so we must treat those table pointers as direct addresses here.
 */
static uint64_t table_ptr_to_phys(const void* ptr) {
    if (!ptr) return 0;
    return (uint64_t)(uintptr_t)ptr;
}

static inline pt_level_t *phys_to_table_ptr(uint64_t phys) {
    if (!phys) return NULL;
    return (pt_level_t *)(uintptr_t)(phys & F_ADDR_MASK);
}

/* Kernel virtual allocation cursor for page-backed mappings. */
static uint64_t kernel_valloc_next = 0;

/* Free virtual ranges so paging_alloc_pages() can reuse released slots. */
typedef struct {
    uint64_t base;
    uint64_t count;
} virt_range_t;

#define VIRT_FREE_LIST_CAP 256
static bool add_overflow_u64(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out) return true;
    if (a > UINT64_MAX - b) return true;
    *out = a + b;
    return false;
}

static bool pages_to_bytes_safe(uint64_t pages, uint64_t* bytes_out) {
    if (!bytes_out) return false;
    if (pages > UINT64_MAX / PAGE_SIZE) return false;
    *bytes_out = pages * PAGE_SIZE;
    return true;
}

static virt_range_t virt_free_list[VIRT_FREE_LIST_CAP];
static size_t virt_free_list_len = 0;

static void virt_free_range_add(uint64_t base, uint64_t count) {
    if (!base || !count) return;
    base = align_up_u64(base, PAGE_SIZE);
    if (!base) return;

    bool merged;
    do {
        merged = false;
        uint64_t range_bytes = 0;
        if (!pages_to_bytes_safe(count, &range_bytes)) return;
        uint64_t end = 0;
        if (add_overflow_u64(base, range_bytes, &end)) return;
        for (size_t i = 0; i < virt_free_list_len; ++i) {
            uint64_t cur_base = virt_free_list[i].base;
            uint64_t cur_bytes = 0;
            uint64_t cur_end = 0;
            if (!pages_to_bytes_safe(virt_free_list[i].count, &cur_bytes)) return;
            if (add_overflow_u64(cur_base, cur_bytes, &cur_end)) return;
            if (cur_end == base) {
                base = cur_base;
                count += virt_free_list[i].count;
                virt_free_list[i] = virt_free_list[virt_free_list_len - 1];
                virt_free_list_len--;
                merged = true;
                break;
            }
            if (end == cur_base) {
                count += virt_free_list[i].count;
                virt_free_list[i] = virt_free_list[virt_free_list_len - 1];
                virt_free_list_len--;
                merged = true;
                break;
            }
        }
    } while (merged);

    if (virt_free_list_len < VIRT_FREE_LIST_CAP) {
        virt_free_list[virt_free_list_len].base = base;
        virt_free_list[virt_free_list_len].count = count;
        virt_free_list_len++;
    }
}

static bool virt_free_range_take(uint64_t count, uint64_t* base_out) {
    if (!count || !base_out) return false;
    for (size_t i = 0; i < virt_free_list_len; ++i) {
        if (virt_free_list[i].count >= count) {
            uint64_t base = virt_free_list[i].base;
            *base_out = base;
            if (virt_free_list[i].count == count) {
                virt_free_list[i] = virt_free_list[virt_free_list_len - 1];
                virt_free_list_len--;
            } else {
                virt_free_list[i].base += count * PAGE_SIZE;
                virt_free_list[i].count -= count;
            }
            return true;
        }
    }
    return false;
}

static void free_page_table_recursive(pt_level_t *level, int depth) {
    if (!level) return;

    for (size_t i = 0; i < 512; ++i) {
        uint64_t entry = level->e[i];
        if (!(entry & F_PRESENT)) {
            continue;
        }

        /* Kernel mappings live in the higher-half PML4 entries and are
         * shared across process directories. Only tear down the user half
         * here; the kernel half is copied by reference and must survive. */
        if (depth == 4 && i >= 256) {
            continue;
        }

        if (depth > 1) {
            pt_level_t *child = phys_to_table_ptr(entry);
            free_page_table_recursive(child, depth - 1);
        }

        level->e[i] = 0;
    }

    kfree(level);
}

/* =========================================================
 * Physical page allocator
 * ========================================================= */
#define FREE_LIST_CAP 256
static uint64_t free_list[FREE_LIST_CAP];
static int      free_list_len = 0;

uint64_t paging_alloc_physical(void) {
    /* free_list_len and stats are shared mutable state with
     * no other protection. Under real preemption (as opposed to the
     * old always-runs-to-completion-on-one-stack boot path) this can
     * be interrupted between any two lines by a timer tick that
     * switches to another thread also inside this function - two
     * threads could then hand out the same physical frame, or corrupt
     * free_list_len into a state where paging_free_physical() writes
     * past the end of free_list[]. Single-core, so a plain
     * irq-disable critical section (matching what kmalloc/kfree
     * already do in memory.c) is sufficient here - no spinlock needed. */
    uint64_t irq_flags = sync_irq_save();

    phys_addr_t phys = phys_alloc_page();
    if (phys) {
        stats.used_pages++;
        if (stats.free_pages) stats.free_pages--;
        sync_irq_restore(irq_flags);
        return (uint64_t)phys;
    }

    if (free_list_len > 0) {
        uint64_t p = free_list[--free_list_len];
        if (stats.free_pages) stats.free_pages--;
        stats.used_pages++;
        sync_irq_restore(irq_flags);
        return p;
    }
    sync_irq_restore(irq_flags);
    return 0;
}

void paging_free_physical(uint64_t phys) {
    uint64_t irq_flags = sync_irq_save();
    bool released = false;
    if (phys_is_valid_addr((phys_addr_t)phys)) {
        phys_free_page((phys_addr_t)phys);
        released = true;
    } else if (phys != 0 && free_list_len < FREE_LIST_CAP) {
        free_list[free_list_len++] = phys & F_ADDR_MASK;
        released = true;
    }
    if (released) {
        if (stats.used_pages) stats.used_pages--;
        stats.free_pages++;
    }
    sync_irq_restore(irq_flags);
}

/* =========================================================
 * Alloc one zeroed page-aligned table level
 * ========================================================= */
static pt_level_t *alloc_level(void) {
    pt_level_t *t = (pt_level_t *)kmalloc_pages(1);

    serial_puts("[PAGING] alloc_level -> 0x");
    serial_puthex((uint64_t)(uintptr_t)t);
    serial_puts("\n");

    if (t) {
        memset(t, 0, PAGE_SIZE);
        serial_puts("[PAGING] alloc_level memset ok\n");
    } else {
        serial_puts("[PAGING] alloc_level FAILED\n");
    }

    return t;
}

/* =========================================================
 * TLB helpers
 * ========================================================= */
void paging_flush_tlb(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3,%0":"=r"(cr3));
    __asm__ volatile("mov %0,%%cr3"::"r"(cr3):"memory");
}
void paging_flush_tlb_entry(uint64_t v) {
    __asm__ volatile("invlpg (%0)"::"r"(v):"memory");
}
void paging_invalidate_page(uint64_t v) { paging_flush_tlb_entry(v); }

/* =========================================================
 * CR0 paging enable / disable
 * ========================================================= */
void paging_enable(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    cr0 |= (uint64_t)0x80000000UL;
    __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
}

void paging_disable(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    cr0 &= ~(uint64_t)0x80000000UL;
    __asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
}
bool paging_is_enabled(void) {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0,%0":"=r"(cr0));
    return (cr0 >> 31) & 1;
}

/* The physical allocator begins issuing frames immediately after the
 * statically linked kernel image.  A handful of legacy kernel services and
 * third-party ports still temporarily use those frames by their low physical
 * address while constructing DOM/CSS state.  Map a bounded low-memory runway
 * after the image as an identity mapping so those accesses remain valid until
 * every caller exclusively uses the high-half direct map. */
#define KERNEL_LOW_IDENTITY_DYNAMIC_SLACK (64ULL * 1024ULL * 1024ULL)

static uint64_t paging_initial_identity_limit(void)
{
    uint64_t limit = align_up_u64((uint64_t)(uintptr_t)&_kernel_end, PAGE_SIZE);
    uint64_t total = phys_get_total_memory();
    uint64_t slack_end = limit + KERNEL_LOW_IDENTITY_DYNAMIC_SLACK;

    if (slack_end < limit) {
        slack_end = limit;
    }
    if (total != 0 && slack_end > total) {
        slack_end = total;
    }
    return align_up_u64(slack_end, PAGE_SIZE);
}

/* =========================================================
 * Initialise paging
 * ========================================================= */
void paging_init(void) {
    serial_puts("[PAGING] init\n");

    memset(&stats, 0, sizeof(stats));
    serial_puts("[PAGING] memset ok\n");

    stats.total_pages = phys_get_total_memory() / PAGE_SIZE;
    stats.free_pages  = stats.total_pages;
    kernel_valloc_next = align_up_u64((uint64_t)(uintptr_t)&_kernel_end, PAGE_SIZE);

    serial_puts("[PAGING] before alloc_level\n");
    kernel_pml4 = alloc_level();
    serial_puts("[PAGING] after alloc_level\n");

    if (!kernel_pml4) {
        serial_puts("[PAGING] FATAL no PML4\n");
        return;
    }

    serial_puts("[PAGING] before map\n");
    uint64_t kernel_identity_end =
        align_up_u64((uint64_t)(uintptr_t)&_kernel_end, PAGE_SIZE);
    uint64_t identity_limit = paging_initial_identity_limit();
    /* Keep the historical [0, kernel-image) mapping unchanged.  Mapping the
     * whole interval through the dynamic runway would pre-map low virtual
     * addresses reserved for user stacks (around 8 MiB), making task stack
     * allocation fail.  Only the physical frames immediately after the image
     * are required as compatibility identity mappings. */
    paging_map_range(
        0,
        0,
        kernel_identity_end,
        PAGE_PRESENT|PAGE_RW
    );
    if (identity_limit > kernel_identity_end) {
        serial_puts("[PAGING] dynamic identity runway 0x");
        serial_puthex(kernel_identity_end);
        serial_puts("..0x");
        serial_puthex(identity_limit);
        serial_puts("\n");
        paging_map_range(
            kernel_identity_end,
            kernel_identity_end,
            identity_limit - kernel_identity_end,
            PAGE_PRESENT|PAGE_RW
        );
    }
    /* paging_alloc_pages() must not reuse the identity-mapped runway: the
     * 4 KiB mapper correctly refuses to overwrite an existing leaf entry.
     * Start all kernel stacks and future virtual allocations after it. */
    kernel_valloc_next = identity_limit;
    serial_puts("[PAGING] after map\n");

    serial_puts("[PAGING] before higher\n");
    paging_map_kernel_higher();
    serial_puts("[PAGING] after higher\n");

    serial_puts("[PAGING] before cr3\n");
    {
        uint64_t cr3_phys = table_ptr_to_phys(kernel_pml4);
        __asm__ volatile("mov %0,%%cr3"::"r"(cr3_phys):"memory");
    }
    current_directory = (page_directory_t*)kernel_pml4;
    serial_puts("[PAGING] after cr3\n");

    serial_puts("[PAGING] ready\n");
}

/* =========================================================
 * 4-level walk index helpers
 * ========================================================= */
static inline uint64_t idx4(uint64_t v){ return (v>>39)&0x1FF; }
static inline uint64_t idx3(uint64_t v){ return (v>>30)&0x1FF; }
static inline uint64_t idx2(uint64_t v){ return (v>>21)&0x1FF; }
static inline uint64_t idx1(uint64_t v){ return (v>>12)&0x1FF; }

static uint64_t paging_virt_to_phys_in_root(pt_level_t *root, uint64_t v) {
    if (!root) return 0;
    uint64_t e4 = root->e[idx4(v)]; if (!(e4&F_PRESENT)) return 0;
    pt_level_t *pdpt = phys_to_table_ptr(e4);
    if (!pdpt) return 0;
    uint64_t e3 = pdpt->e[idx3(v)]; if (!(e3&F_PRESENT)) return 0;
    pt_level_t *pd = phys_to_table_ptr(e3);
    if (!pd) return 0;
    uint64_t e2 = pd->e[idx2(v)]; if (!(e2&F_PRESENT)) return 0;
    if (e2 & F_PS) {
        return (e2 & F_HUGE_ADDR_MASK) | (v & ((1ULL << 21) - 1ULL));
    }
    pt_level_t *pt = phys_to_table_ptr(e2);
    if (!pt) return 0;
    uint64_t e1 = pt->e[idx1(v)]; if (!(e1&F_PRESENT)) return 0;
    return (e1&F_ADDR_MASK)|(v&(PAGE_SIZE-1));
}

static pt_level_t *walk_get_or_alloc(pt_level_t *parent, uint64_t i, uint64_t flags, bool *created) {
    if (created) {
        *created = false;
    }
    if (!(parent->e[i] & F_PRESENT)) {
        pt_level_t *child = alloc_level();
        if (!child) return NULL;
        parent->e[i] = (table_ptr_to_phys(child) & F_ADDR_MASK) | (flags | F_PRESENT | F_RW);
        if (created) {
            *created = true;
        }
    } else {
        /* Preserve existing mapping, only widen permissions. */
        parent->e[i] |= (flags & (F_USER | F_RW));
    }

    pt_level_t *ret = phys_to_table_ptr(parent->e[i]);
    if (!ret) {
        serial_puts("[PAGING] walk_get_or_alloc: NULL return\n");
    }
    return ret;
}

static bool free_if_empty_child(pt_level_t *parent, uint64_t index);

/* Map one 2MiB page through a page-directory entry.  The early identity
 * mapping is contiguous and aligned, so using large pages avoids tens of
 * thousands of allocator calls and per-page TLB invalidations during boot. */
static bool paging_map_huge_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    pt_level_t *root = paging_root();
    if (!root || (virt & ((1ULL << 21) - 1ULL)) ||
        (phys & ((1ULL << 21) - 1ULL))) return FALSE;

    bool created_pdpt = false;
    bool created_pd = false;
    pt_level_t *pdpt = walk_get_or_alloc(root, idx4(virt), flags, &created_pdpt);
    if (!pdpt) return FALSE;
    pt_level_t *pd = walk_get_or_alloc(pdpt, idx3(virt), flags, &created_pd);
    if (!pd) {
        if (created_pdpt) free_if_empty_child(root, idx4(virt));
        return FALSE;
    }

    uint64_t *entry = &pd->e[idx2(virt)];
    if (*entry & F_PRESENT) {
        if (created_pd) free_if_empty_child(pdpt, idx3(virt));
        if (created_pdpt) free_if_empty_child(root, idx4(virt));
        return FALSE;
    }
    *entry = (phys & F_HUGE_ADDR_MASK) | (flags | F_PRESENT | F_PS);
    return TRUE;
}

static bool free_if_empty_child(pt_level_t *parent, uint64_t index) {
    if (!parent) return false;
    uint64_t entry = parent->e[index];
    if (!(entry & F_PRESENT)) return false;
    pt_level_t *child = phys_to_table_ptr(entry);
    if (!child) return false;
    for (size_t i = 0; i < 512; ++i) {
        if (child->e[i]) {
            return false;
        }
    }
    parent->e[index] = 0;
    kfree(child);
    return true;
}

static bool level_is_empty(pt_level_t *level) {
    if (!level) return true;
    for (size_t i = 0; i < 512; ++i) {
        if (level->e[i]) return false;
    }
    return true;
}

static void prune_empty_page_tables(pt_level_t *root, uint64_t virt) {
    if (!root) return;

    uint64_t i4 = (virt >> 39) & 0x1FFULL;
    uint64_t e4 = root->e[i4];
    if (!(e4 & F_PRESENT)) return;

    pt_level_t *pdpt = phys_to_table_ptr(e4);
    if (!pdpt) return;

    uint64_t i3 = (virt >> 30) & 0x1FFULL;
    uint64_t e3 = pdpt->e[i3];
    if (!(e3 & F_PRESENT)) return;

    pt_level_t *pd = phys_to_table_ptr(e3);
    if (!pd) return;

    uint64_t i2 = (virt >> 21) & 0x1FFULL;
    uint64_t e2 = pd->e[i2];
    if (!(e2 & F_PRESENT)) return;

    pt_level_t *pt = phys_to_table_ptr(e2);
    if (!pt || !level_is_empty(pt)) return;

    pd->e[i2] = 0;
    kfree(pt);

    if (!level_is_empty(pd)) return;

    pdpt->e[i3] = 0;
    kfree(pd);

    if (!level_is_empty(pdpt)) return;

    root->e[i4] = 0;
    kfree(pdpt);
}

/* =========================================================
 * Map / unmap single page
 * ========================================================= */
bool paging_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t irq_flags = sync_irq_save();
    pt_level_t *root = paging_root();
    if (!root) {
        serial_puts("[PAGING] map_page: no root\n");
        sync_irq_restore(irq_flags);
        return FALSE;
    }
    virt &= ~(uint64_t)(PAGE_SIZE-1);
    phys &= ~(uint64_t)(PAGE_SIZE-1);

    bool created_pdpt = false;
    bool created_pd = false;
    bool created_pt = false;

    pt_level_t *pdpt = walk_get_or_alloc(root, idx4(virt), flags, &created_pdpt);
    if (!pdpt) {
        serial_puts("[PAGING] map_page: PDPT alloc/lookup failed\n");
        sync_irq_restore(irq_flags);
        return FALSE;
    }
    pt_level_t *pd = walk_get_or_alloc(pdpt, idx3(virt), flags, &created_pd);
    if (!pd) {
        serial_puts("[PAGING] map_page: PD alloc/lookup failed\n");
        if (created_pdpt) {
            free_if_empty_child(root, idx4(virt));
        }
        sync_irq_restore(irq_flags);
        return FALSE;
    }
    /* A 2MiB leaf cannot be descended into as a page table. */
    if (pd->e[idx2(virt)] & F_PS) {
        sync_irq_restore(irq_flags);
        return FALSE;
    }
    pt_level_t *pt = walk_get_or_alloc(pd, idx2(virt), flags, &created_pt);
    if (!pt) {
        serial_puts("[PAGING] map_page: PT alloc/lookup failed\n");
        if (created_pd) {
            free_if_empty_child(pdpt, idx3(virt));
        }
        if (created_pdpt) {
            free_if_empty_child(root, idx4(virt));
        }
        sync_irq_restore(irq_flags);
        return FALSE;
    }

    uint64_t *leaf = &pt->e[idx1(virt)];
    if (*leaf & F_PRESENT) {
        serial_puts("[PAGING] map_page: leaf already present\n");
        if (created_pt) {
            free_if_empty_child(pd, idx2(virt));
        }
        if (created_pd) {
            free_if_empty_child(pdpt, idx3(virt));
        }
        if (created_pdpt) {
            free_if_empty_child(root, idx4(virt));
        }
        sync_irq_restore(irq_flags);
        return FALSE;
    }

    *leaf = (phys & F_ADDR_MASK) | (flags | F_PRESENT);
    /* NOTE: this used to serial_puts() one line per mapped 4KB page. During
     * paging_init()'s identity map of the whole kernel image that is tens of
     * thousands of lines (e.g. ~20,000+ for an ~80MB kernel image), which
     * made boot extremely slow over a real/slow serial port and buried the
     * handful of log lines that actually matter under noise. Mapping
     * failures below are still logged since those are rare and actionable. */
    paging_flush_tlb_entry(virt);
    sync_irq_restore(irq_flags);
    return TRUE;
}

void paging_unmap_page(uint64_t virt) {
    uint64_t irq_flags = sync_irq_save();
    pt_level_t *root = paging_root();
    if (!root) { sync_irq_restore(irq_flags); return; }
    virt &= ~(uint64_t)(PAGE_SIZE-1);
    uint64_t e4 = root->e[idx4(virt)]; if (!(e4&F_PRESENT)) { sync_irq_restore(irq_flags); return; }
    pt_level_t *pdpt = phys_to_table_ptr(e4);
    if (!pdpt) { sync_irq_restore(irq_flags); return; }
    uint64_t e3 = pdpt->e[idx3(virt)];        if (!(e3&F_PRESENT)) { sync_irq_restore(irq_flags); return; }
    pt_level_t *pd = phys_to_table_ptr(e3);
    if (!pd) { sync_irq_restore(irq_flags); return; }
    uint64_t e2 = pd->e[idx2(virt)];          if (!(e2&F_PRESENT)) { sync_irq_restore(irq_flags); return; }
    pt_level_t *pt = phys_to_table_ptr(e2);
    if (!pt) { sync_irq_restore(irq_flags); return; }
    pt->e[idx1(virt)] = 0;

    /* If the leaf table becomes empty, tear down empty ancestors so we
     * do not leak PT/PD/PDPT pages when mappings are created and removed
     * repeatedly. This is intentionally bottom-up: freeing the leaf first
     * avoids walking dangling pointers. */
    prune_empty_page_tables(root, virt);

    paging_flush_tlb_entry(virt);
    sync_irq_restore(irq_flags);
}

/* =========================================================
 * Map / unmap ranges
 * ========================================================= */
bool paging_map_range(uint64_t vs, uint64_t ps_arg, uint64_t size, uint64_t flags) {
    uint64_t pg = PAGE_SIZE;
    serial_puts("[PAGING] map_range start virt=0x");
    serial_puthex(vs);
    serial_puts(" phys=0x");
    serial_puthex(ps_arg);
    serial_puts(" size=0x");
    serial_puthex(size);
    serial_puts(" flags=0x");
    serial_puthex(flags);
    serial_puts("\n");
    vs    = (vs    / pg) * pg;
    ps_arg= (ps_arg/ pg) * pg;
    size  = ((size + pg - 1) / pg) * pg;
    
    uint64_t mapped = 0;
    const uint64_t huge = 1ULL << 21;
    while (mapped + huge <= size &&
           ((vs + mapped) % huge) == 0 &&
           ((ps_arg + mapped) % huge) == 0) {
        if (!paging_map_huge_page(vs + mapped, ps_arg + mapped, flags)) {
            serial_puts("[PAGING] huge map failed at virt=0x");
            serial_puthex(vs + mapped);
            serial_puts("\\n");
            return FALSE;
        }
        mapped += huge;
    }
    for (; mapped < size; mapped += pg) {
        if (!paging_map_page(vs + mapped, ps_arg + mapped, flags)) {
            serial_puts("[PAGING] map_range FAILED at virt=0x");
            serial_puthex(vs + mapped);
            serial_puts(" phys=0x");
            serial_puthex(ps_arg + mapped);
            serial_puts("\n");
            for (uint64_t undo = 0; undo < mapped; undo += pg) {
                paging_unmap_page(vs + undo);
            }
            return FALSE;
        }
    }
    paging_flush_tlb();
    return TRUE;
}

void paging_unmap_range(uint64_t vs, uint64_t size) {
    uint64_t pg = PAGE_SIZE;
    vs   = (vs  / pg) * pg;
    size = ((size+pg-1)/pg)*pg;
    for (uint64_t off=0; off<size; off+=pg) paging_unmap_page(vs+off);
}

/* =========================================================
 * Address translation
 * ========================================================= */
uint64_t paging_virt_to_phys(uint64_t v) {
    pt_level_t *root = paging_root();
    if (!root) return 0;
    uint64_t e4 = root->e[idx4(v)]; if (!(e4 & F_PRESENT)) return 0;
    pt_level_t *pdpt = phys_to_table_ptr(e4);
    if (!pdpt) return 0;
    uint64_t e3 = pdpt->e[idx3(v)]; if (!(e3 & F_PRESENT)) return 0;
    pt_level_t *pd = phys_to_table_ptr(e3);
    if (!pd) return 0;
    uint64_t e2 = pd->e[idx2(v)]; if (!(e2 & F_PRESENT)) return 0;
    if (e2 & F_PS) {
        return (e2 & F_HUGE_ADDR_MASK) | (v & ((1ULL << 21) - 1ULL));
    }
    pt_level_t *pt = phys_to_table_ptr(e2);
    if (!pt) return 0;
    uint64_t e1 = pt->e[idx1(v)]; if (!(e1 & F_PRESENT)) return 0;
    return (e1 & F_ADDR_MASK) | (v & (PAGE_SIZE - 1));
}
bool paging_is_present(uint64_t v)  { return paging_virt_to_phys(v) != 0; }

static uint64_t leaf_entry(uint64_t v) {
    pt_level_t *root = paging_root();
    if (!root) return 0;
    uint64_t e4=root->e[idx4(v)]; if(!(e4&F_PRESENT)) return 0;
    pt_level_t *pdpt = phys_to_table_ptr(e4);
    if (!pdpt) return 0;
    uint64_t e3=pdpt->e[idx3(v)]; if(!(e3&F_PRESENT)) return 0;
    pt_level_t *pd = phys_to_table_ptr(e3);
    if (!pd) return 0;
    uint64_t e2=pd->e[idx2(v)]; if(!(e2&F_PRESENT)) return 0;
    if (e2 & F_PS) return e2;
    pt_level_t *pt = phys_to_table_ptr(e2);
    if (!pt) return 0;
    return pt->e[idx1(v)];
}
bool paging_is_writable(uint64_t v) { uint64_t e=leaf_entry(v); return (e&F_PRESENT)&&(e&F_RW); }
bool paging_is_user(uint64_t v)     { uint64_t e=leaf_entry(v); return (e&F_PRESENT)&&(e&F_USER); }

/* =========================================================
 * Page directory API (wrappers used by task.c)
 * ========================================================= */
page_directory_t *paging_create_directory(void) {
    pt_level_t *pml4 = alloc_level();
    if (!pml4) return NULL;
    if (kernel_pml4) memcpy(pml4, kernel_pml4, sizeof(pt_level_t));
    return (page_directory_t *)pml4;
}
void paging_destroy_directory(page_directory_t *dir) {
    if (!dir || (pt_level_t *)dir == kernel_pml4) return;
    free_page_table_recursive((pt_level_t *)dir, 4);
}
void paging_switch_directory(page_directory_t *dir) {
    if (!dir) return;
    uint64_t irq_flags = sync_irq_save();
    current_directory = dir;
    {
        uint64_t cr3_phys = table_ptr_to_phys(dir);
        __asm__ volatile("mov %0,%%cr3"::"r"(cr3_phys):"memory");
    }
    sync_irq_restore(irq_flags);
}
page_directory_t *paging_get_current_directory(void) { return current_directory; }
page_table_t     *paging_create_table(void)  { return (page_table_t *)alloc_level(); }
void              paging_destroy_table(page_table_t *t) { kfree(t); }

/* =========================================================
 * Per-page alloc / free
 * ========================================================= */
uint64_t paging_alloc_page(uint64_t flags) {
    uint64_t phys = paging_alloc_physical();
    if (!phys) return 0;
    if (!paging_map_page(phys, phys, flags | F_PRESENT)) {
        paging_free_physical(phys);
        return 0;
    }
    memset((void*)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    return phys;
}
void paging_free_page(uint64_t virt) {
    uint64_t phys = paging_virt_to_phys(virt);
    paging_unmap_page(virt);
    if (phys) paging_free_physical(phys);
}
uint64_t paging_alloc_pages(uint64_t count, uint64_t flags) {
    if (!count) return 0;

    uint64_t irq_flags = sync_irq_save();
    if (!kernel_valloc_next) {
        kernel_valloc_next = paging_initial_identity_limit();
    }

    uint64_t base = 0;
    bool from_free_list = virt_free_range_take(count, &base);
    if (!from_free_list) {
        base = kernel_valloc_next;
    }

    uint64_t mapped = 0;
    for (; mapped < count; ++mapped) {
        uint64_t phys = paging_alloc_physical();
        if (!phys) {
            break;
        }
        uint64_t virt = 0;
        if (add_overflow_u64(base, mapped * PAGE_SIZE, &virt)) {
            paging_free_physical(phys);
            break;
        }
        if (!paging_map_page(virt, phys, flags | F_PRESENT)) {
            paging_free_physical(phys);
            break;
        }
        memset((void*)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
    }

    if (mapped != count) {
        for (uint64_t undo = 0; undo < mapped; ++undo) {
            uint64_t virt = 0;
            uint64_t phys = 0;
            if (add_overflow_u64(base, undo * PAGE_SIZE, &virt)) {
                continue;
            }
            phys = paging_virt_to_phys(virt);
            if (phys) {
                paging_unmap_page(virt);
                paging_free_physical(phys);
            }
        }
        virt_free_range_add(base, count);
        sync_irq_restore(irq_flags);
        return 0;
    }

    if (!from_free_list) {
        kernel_valloc_next = base + (count * PAGE_SIZE);
    }

    sync_irq_restore(irq_flags);
    return base;
}
void paging_free_pages(uint64_t virt, uint64_t count) {
    if (!virt || !count) return;

    uint64_t irq_flags = sync_irq_save();
    uint64_t freed = 0;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t v = 0;
        if (add_overflow_u64(virt, i * PAGE_SIZE, &v)) {
            break;
        }
        uint64_t p = paging_virt_to_phys(v);
        paging_unmap_page(v);
        if (p) paging_free_physical(p);
        ++freed;
    }
    virt_free_range_add(virt, freed);
    sync_irq_restore(irq_flags);
}

/* =========================================================
 * Page-fault handler
 * ========================================================= */
void paging_register_fault_handler(page_fault_handler_t h) { g_fault_handler = h; }
void paging_page_fault_handler(uint64_t error_code) {
    uint64_t fault_addr;
    __asm__ volatile("mov %%cr2,%0":"=r"(fault_addr));
    serial_puts("[PAGING] fault addr="); serial_puthex(fault_addr);
    serial_puts(" err="); serial_puthex(error_code); serial_puts("\n");
    if (g_fault_handler) g_fault_handler(fault_addr, error_code);
}

/* =========================================================
 * Memory protection
 * ========================================================= */
static bool set_flags(uint64_t s, uint64_t sz, uint64_t set, uint64_t clr) {
    uint64_t pg=PAGE_SIZE;
    s  =(s /pg)*pg; sz=((sz+pg-1)/pg)*pg;
    pt_level_t *root = paging_root();
    if (!root) return FALSE;
    for (uint64_t off=0; off<sz; off+=pg) {
        uint64_t v=s+off;
        uint64_t e4=root->e[idx4(v)]; if(!(e4&F_PRESENT)) continue;
        pt_level_t *pdpt = phys_to_table_ptr(e4);
        if (!pdpt) continue;
        uint64_t e3=pdpt->e[idx3(v)]; if(!(e3&F_PRESENT)) continue;
        pt_level_t *pd = phys_to_table_ptr(e3);
        if (!pd) continue;
        uint64_t e2=pd->e[idx2(v)]; if(!(e2&F_PRESENT)) continue;
        pt_level_t *pt = phys_to_table_ptr(e2);
        if (!pt) continue;
        uint64_t *e=&pt->e[idx1(v)];
        *e=(*e|set)&~clr;
        paging_flush_tlb_entry(v);
    }
    return TRUE;
}
bool paging_protect_range(uint64_t s,uint64_t sz,uint64_t f){ return set_flags(s,sz,f&0xFFF,(~f)&0xFFF&~(uint64_t)F_PRESENT); }
bool paging_set_readonly(uint64_t s,uint64_t sz)  { return set_flags(s,sz,0,F_RW); }
bool paging_set_writable(uint64_t s,uint64_t sz)  { return set_flags(s,sz,F_RW,0); }
bool paging_set_user(uint64_t s,uint64_t sz)      { return set_flags(s,sz,F_USER,0); }
bool paging_set_supervisor(uint64_t s,uint64_t sz){ return set_flags(s,sz,0,F_USER); }

/* =========================================================
 * Copy-on-Write (minimal)
 * ========================================================= */
bool paging_mark_cow(uint64_t s,uint64_t sz){ return paging_set_readonly(s,sz); }
bool paging_handle_cow(uint64_t fa) {
    uint64_t v=fa&~(uint64_t)(PAGE_SIZE-1);
    uint64_t po=paging_virt_to_phys(v); if(!po) return FALSE;
    uint64_t pn=paging_alloc_physical(); if(!pn) return FALSE;
    memcpy((void*)(uintptr_t)PHYS_TO_VIRT(pn),(void*)(uintptr_t)PHYS_TO_VIRT(po),PAGE_SIZE);
    if (!paging_map_page(v,pn,F_PRESENT|F_RW)) {
        paging_free_physical(pn);
        return FALSE;
    }
    return TRUE;
}

/* =========================================================
 * Higher-half helpers
 * ========================================================= */
static bool paging_map_huge_page_2m(pt_level_t *root, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!root) return false;
    uint64_t pg2m = 1ULL << 21;
    virt &= ~(pg2m - 1ULL);
    phys &= ~(pg2m - 1ULL);

    bool created_pdpt = false;
    bool created_pd = false;
    pt_level_t *pdpt = walk_get_or_alloc(root, idx4(virt), flags, &created_pdpt);
    if (!pdpt) return false;
    pt_level_t *pd = walk_get_or_alloc(pdpt, idx3(virt), flags, &created_pd);
    if (!pd) {
        if (created_pdpt) free_if_empty_child(root, idx4(virt));
        return false;
    }

    uint64_t *entry = &pd->e[idx2(virt)];
    if (*entry & F_PRESENT) {
        if (created_pd) free_if_empty_child(pdpt, idx3(virt));
        if (created_pdpt) free_if_empty_child(root, idx4(virt));
        return false;
    }

    *entry = (phys & F_HUGE_ADDR_MASK) | (flags | F_PRESENT | F_RW | F_PS);
    paging_flush_tlb_entry(virt);
    return true;
}

void paging_map_kernel_higher(void) {
    /* High-half direct map for physical memory access.
     *
     * The physical allocator intentionally manages only Multiboot type-1
     * (available) memory, and phys_get_total_memory() consequently returns
     * that allocator limit. It is not a safe direct-map bound: on UEFI,
     * ACPI reclaimable/NVS tables commonly sit immediately above the final
     * available span. SMP discovery dereferences those tables through
     * PHYS_TO_VIRT(), so mapping only the allocator limit faults on valid
     * firmware data.
     *
     * cos_runtime_direct_map_extent() is the highest RAM/ACPI firmware
     * endpoint, deliberately excluding large reserved PCI/MMIO apertures
     * which q35 can place far above RAM. Mapping the contiguous interval does
     * not make holes allocatable; it only makes valid RAM/ACPI references
     * addressable. Use 2 MiB leaves so the extra UEFI firmware extent has a
     * modest page-table cost. */
    uint64_t phys_extent = cos_runtime_direct_map_extent();
    if (!phys_extent) phys_extent = phys_get_total_memory();
    if (!phys_extent) phys_extent = 4ULL * 1024 * 1024 * 1024;
    uint64_t pg2m = 1ULL << 21;
    uint64_t map_size = align_up_u64(phys_extent, pg2m);
    serial_puts("[PAGING] direct-map extent=0x");
    serial_puthex(phys_extent);
    serial_puts(" map-size=0x");
    serial_puthex(map_size);
    serial_puts("\n");
    for (uint64_t off = 0; off < map_size; off += pg2m) {
        if (!paging_map_huge_page_2m(kernel_pml4,
                                     0xFFFF800000000000ULL + off,
                                     off,
                                     PAGE_PRESENT | PAGE_RW)) {
            serial_puts("[PAGING] FATAL: higher-half map failed at phys=0x");
            serial_puthex(off);
            serial_puts("\n");
            break;
        }
    }
}
bool paging_is_higher_half(uint64_t v){ return v>=0xFFFF800000000000ULL; }

/* =========================================================
 * Write-Combining support (PAT) for MMIO regions - namely the linear
 * framebuffer. paging_map_kernel_higher() above maps all of physical
 * memory (RAM *and* MMIO alike) as plain cacheable 2MB pages, which
 * is correct for RAM but leaves the framebuffer without the
 * write-combining behaviour that makes large sequential writes to a
 * linear framebuffer (i.e. every vga_flip()) fast. The functions
 * below let a caller upgrade an already-mapped 2MB region to WC
 * *after* paging_map_kernel_higher() has run, without disturbing any
 * other mapping.
 * ========================================================= */
#define PAT_MSR             0x277u
#define PAT_SLOT4_WC        0x01ULL /* PAT memory-type encoding: 1 = Write-Combining */
#define PAGE_PAT_2M         (1ULL << 12) /* PAT bit for a 2MB (PS=1) page table entry */

static bool g_pat_wc_configured = false;

static inline uint64_t rdmsr64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void wrmsr64(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile("wrmsr" :: "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/* Repurpose PAT slot 4 (selected by a PTE/PDE with PAT=1, PCD=0,
 * PWT=0) as Write-Combining. The CPU's other 7 reset-default slots -
 * including whichever ones every existing mapping in the system
 * already uses (all with PAT=0) - are left untouched, so this cannot
 * change the behaviour of any mapping that doesn't explicitly opt in
 * via the PAT bit. Idempotent; safe to call more than once. */
void paging_configure_pat_wc(void) {
    if (g_pat_wc_configured) return;
    uint64_t pat = rdmsr64(PAT_MSR);
    uint64_t byte4_mask = 0xFFULL << 32;
    pat = (pat & ~byte4_mask) | (PAT_SLOT4_WC << 32);
    wrmsr64(PAT_MSR, pat);
    g_pat_wc_configured = true;
    serial_puts("[PAGING] PAT slot 4 configured for Write-Combining\n");
}

/* Flip an *already-present* 2MB mapping to PAT slot 4 (WC). Unlike
 * paging_map_huge_page_2m() (which deliberately refuses to touch an
 * already-present entry - it's for establishing brand new mappings
 * only), this is specifically for adjusting a mapping that
 * paging_map_kernel_higher() already created. Returns false without
 * changing anything if the virtual address isn't currently backed by
 * a present 2MB page (not mapped yet, mapped as 4KB pages, or part of
 * a 1GB page) - callers rely on this to safely "try" a region instead
 * of duplicating the walk just to check first. */
static bool paging_set_2m_entry_wc(uint64_t virt) {
    if (!kernel_pml4) return false;
    virt &= ~((1ULL << 21) - 1ULL);

    uint64_t e4 = kernel_pml4->e[idx4(virt)];
    if (!(e4 & F_PRESENT)) return false;
    pt_level_t *pdpt = (pt_level_t *)(uintptr_t)PHYS_TO_VIRT(e4 & F_ADDR_MASK);

    uint64_t e3 = pdpt->e[idx3(virt)];
    if (!(e3 & F_PRESENT)) return false;
    if (e3 & F_PS) return false; /* 1GB page here - different case, don't guess */
    pt_level_t *pd = (pt_level_t *)(uintptr_t)PHYS_TO_VIRT(e3 & F_ADDR_MASK);

    uint64_t *entry = &pd->e[idx2(virt)];
    if (!(*entry & F_PRESENT) || !(*entry & F_PS)) return false;

    /* Physical address, present/RW/global bits etc. are all preserved -
     * only the PAT bit changes, which (with PCD/PWT already 0 on every
     * normal kernel mapping) moves this entry from slot 0 (WB) to
     * slot 4 (now WC). */
    *entry |= PAGE_PAT_2M;
    paging_flush_tlb_entry(virt);
    return true;
}

/* Mark the physical range [phys_start, phys_start+size) Write-
 * Combining, rounding outward to whole 2MB pages. Only touches pages
 * that are already present 2MB mappings - exactly what
 * paging_map_kernel_higher()'s direct map produces for RAM/MMIO in
 * the range it covers. Anything that doesn't hold for a given 2MB
 * chunk (not mapped, or mapped some other way) is safely left with
 * its existing correct-but-not-WC attributes rather than risk an
 * incorrect partial remap. Returns how many 2MB pages were actually
 * converted, so a caller can tell full success from a partial/no-op -
 * e.g. if the framebuffer sits above the range paging_map_kernel_higher()
 * mapped, this returns 0 and the caller keeps running at the default
 * (correct, just not accelerated) cacheability. */
uint64_t paging_mark_region_wc(uint64_t phys_start, uint64_t size) {
    if (size == 0) return 0;
    paging_configure_pat_wc();

    uint64_t pg2m = 1ULL << 21;
    uint64_t start = phys_start & ~(pg2m - 1ULL);
    uint64_t end = align_up_u64(phys_start + size, pg2m);
    uint64_t converted = 0;
    uint64_t total = (end - start) / pg2m;

    for (uint64_t p = start; p < end; p += pg2m) {
        if (paging_set_2m_entry_wc(PHYS_TO_VIRT(p))) converted++;
    }

    serial_puts("[PAGING] paging_mark_region_wc: converted ");
    serial_putdec(converted);
    serial_puts(" of ");
    serial_putdec(total);
    serial_puts(" 2MB page(s) to Write-Combining\n");
    return converted;
}

/* =========================================================
 * Kernel heap wrappers
 * ========================================================= */
void *paging_kmalloc(size_t sz)                        { return kmalloc(sz); }
void *paging_kmalloc_aligned(size_t sz,uint64_t align) { return kmalloc_aligned(sz,align); }
void  paging_kfree(void *p)                            { kfree(p); }
void *paging_krealloc(void *p,size_t sz)               { return krealloc(p, sz); }

/* =========================================================
 * User-space helpers
 * ========================================================= */
bool paging_clone_user_range(page_directory_t *dst, page_directory_t *src, uint64_t start, uint64_t size) {
    if (!dst || !src) return FALSE;
    uint64_t irq_flags = sync_irq_save();
    pt_level_t *old_root = paging_root();
    uint64_t pg = PAGE_SIZE;
    start = (start / pg) * pg;
    size = ((size + pg - 1) / pg) * pg;

    paging_switch_directory(dst);

    bool ok = TRUE;
    uint64_t mapped = 0;
    for (uint64_t off = 0; off < size; off += pg) {
        uint64_t v = start + off;
        uint64_t phys = paging_virt_to_phys_in_root((pt_level_t*)src, v);
        if (!phys) continue;

        uint64_t new_phys = paging_alloc_physical();
        if (!new_phys) { ok = FALSE; break; }
        memcpy((void*)(uintptr_t)PHYS_TO_VIRT(new_phys), (void*)(uintptr_t)PHYS_TO_VIRT(phys), PAGE_SIZE);

        if (!paging_map_page(v, new_phys, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            paging_free_physical(new_phys);
            ok = FALSE;
            break;
        }
        mapped++;
    }

    if (!ok) {
        for (uint64_t off = 0; off < mapped; ++off) {
            uint64_t v = start + off * pg;
            uint64_t phys = paging_virt_to_phys(v);
            if (phys) {
                paging_unmap_page(v);
                paging_free_physical(phys);
            }
        }
    }

    if (old_root) {
        paging_switch_directory((page_directory_t*)old_root);
    }
    sync_irq_restore(irq_flags);
    return ok;
}

bool paging_setup_user_stack(page_directory_t *dir, uint64_t top, uint64_t pages) {
    if (!dir || pages == 0) return FALSE;
    if (top < pages * PAGE_SIZE) return FALSE;
    uint64_t irq_flags = sync_irq_save();
    page_directory_t *old = current_directory;
    paging_switch_directory(dir);
    bool ok = TRUE;
    uint64_t mapped = 0;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t phys = paging_alloc_physical();
        if (!phys) { ok = FALSE; break; }
        uint64_t virt = (top - (i + 1) * PAGE_SIZE);
        if (!paging_map_page(virt, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            paging_free_physical(phys);
            ok = FALSE;
            break;
        }
        memset((void*)(uintptr_t)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        mapped++;
    }
    if (!ok) {
        for (uint64_t i = 0; i < mapped; ++i) {
            uint64_t virt = top - (i + 1) * PAGE_SIZE;
            uint64_t phys = paging_virt_to_phys(virt);
            if (phys) {
                paging_unmap_page(virt);
                paging_free_physical(phys);
            }
        }
    }
    if (old) {
        paging_switch_directory(old);
    }
    sync_irq_restore(irq_flags);
    return ok;
}

/* Maps enough PAGE_SIZE-aligned pages starting at `base` (a ring3-usable
 * virtual address) to hold `len` bytes, then copies `code`/`len` into
 * them. Pages are PRESENT|RW|USER - this kernel doesn't use the NX bit,
 * so no separate "executable" flag is needed for the CPU to fetch
 * instructions from them. This is the real-userspace equivalent of what
 * an ELF loader's PT_LOAD segment mapping would do, just for a single
 * flat blob instead of a parsed executable format. */
bool paging_setup_user_code(page_directory_t *dir, uint64_t base, const uint8_t *code, uint64_t len) {
    if (!dir || !code || len == 0) return FALSE;
    if (base % PAGE_SIZE != 0) return FALSE;

    uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t irq_flags = sync_irq_save();
    page_directory_t *old = current_directory;
    paging_switch_directory(dir);

    bool ok = TRUE;
    uint64_t mapped = 0;
    for (uint64_t i = 0; i < pages; ++i) {
        uint64_t phys = paging_alloc_physical();
        if (!phys) { ok = FALSE; break; }
        uint64_t virt = base + i * PAGE_SIZE;
        if (!paging_map_page(virt, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            paging_free_physical(phys);
            ok = FALSE;
            break;
        }
        uint8_t *dst = (uint8_t*)(uintptr_t)PHYS_TO_VIRT(phys);
        memset(dst, 0, PAGE_SIZE);
        uint64_t remaining = len - i * PAGE_SIZE;
        uint64_t chunk = remaining < PAGE_SIZE ? remaining : PAGE_SIZE;
        memcpy(dst, code + i * PAGE_SIZE, (size_t)chunk);
        mapped++;
    }
    if (!ok) {
        for (uint64_t i = 0; i < mapped; ++i) {
            uint64_t virt = base + i * PAGE_SIZE;
            uint64_t phys = paging_virt_to_phys(virt);
            if (phys) {
                paging_unmap_page(virt);
                paging_free_physical(phys);
            }
        }
    }
    if (old) {
        paging_switch_directory(old);
    }
    sync_irq_restore(irq_flags);
    return ok;
}

void paging_mark_user_directory(page_directory_t *dir) {
    if (!dir) return;
    paging_switch_directory(dir);
}

/* =========================================================
 * Statistics
 * ========================================================= */
void paging_get_stats(page_stats_t *out){ if(out) memcpy(out,&stats,sizeof(stats)); }
void paging_print_stats(void){
    serial_puts("[PAGING] total="); serial_putdec(stats.total_pages);
    serial_puts(" used=");          serial_putdec(stats.used_pages);
    serial_puts(" free=");          serial_putdec(stats.free_pages);
    serial_puts("\n");
}
void paging_dump_directory(page_directory_t *d){ (void)d; }
void paging_dump_page(uint64_t v)              { (void)v; }
void paging_dump_range(uint64_t s,uint64_t sz) { (void)s;(void)sz; }
uint64_t paging_get_free_memory(void) { return stats.free_pages  * PAGE_SIZE; }
uint64_t paging_get_total_memory(void){ return stats.total_pages * PAGE_SIZE; }
