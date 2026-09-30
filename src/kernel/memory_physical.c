/**
 * memory_physical.c - Physical Memory Manager
 *
 * This version keeps a real bitmap allocator, reserves low memory, and exposes
 * live statistics for the GUI. It is still a simple allocator, but it is now
 * initialized explicitly and tracks its scan position to avoid always starting
 * from page zero.
 */

#include "memory_physical.h"
#include "serial.h"
#include "string.h"
#include "memory.h"

extern uint8_t _kernel_end;

#ifndef PHYS_MEMORY_BASE
#define PHYS_MEMORY_BASE   0x00100000ULL   /* 1MB - start of usable physical memory */
#endif

#ifndef PHYS_MAX_MEMORY_SIZE
#define PHYS_MAX_MEMORY_SIZE   (64ULL * 1024ULL * 1024ULL * 1024ULL)   /* 64GB bitmap capacity */
#endif

#define PHYS_PAGE_SIZE     4096ULL
#define PHYS_PAGE_COUNT    (PHYS_MAX_MEMORY_SIZE / PHYS_PAGE_SIZE)
#define PHYS_BITMAP_SIZE   ((PHYS_PAGE_COUNT + 7ULL) / 8ULL)
#define PHYS_RESERVED_PAGES (PHYS_MEMORY_BASE / PHYS_PAGE_SIZE)

static uint8_t physical_bitmap[PHYS_BITMAP_SIZE];
static uint64_t phys_memory_limit_bytes = PHYS_MAX_MEMORY_SIZE;
static uint64_t total_physical_pages = PHYS_PAGE_COUNT;
static uint64_t used_physical_pages = 0;
static uint64_t next_free_hint = PHYS_RESERVED_PAGES;
static phys_mem_stats_t phys_stats = {0};

static void update_stats(void);

static inline void set_bit(uint64_t bit) {
    physical_bitmap[bit / 8ULL] |= (uint8_t)(1u << (bit % 8ULL));
}

static inline void clear_bit(uint64_t bit) {
    physical_bitmap[bit / 8ULL] &= (uint8_t)~(1u << (bit % 8ULL));
}

static inline bool test_bit(uint64_t bit) {
    return (physical_bitmap[bit / 8ULL] & (uint8_t)(1u << (bit % 8ULL))) != 0;
}

static void reserve_page(uint64_t page_num) {
    if (page_num >= total_physical_pages) return;
    if (!test_bit(page_num)) {
        set_bit(page_num);
        used_physical_pages++;
    }
}

static void reserve_range(uint64_t start_page, uint64_t page_count) {
    for (uint64_t i = 0; i < page_count; ++i) {
        reserve_page(start_page + i);
    }
}

static bool bounded_align_range(phys_addr_t start, uint64_t size, uint64_t* aligned_start_out, uint64_t* aligned_end_out) {
    if (!aligned_start_out || !aligned_end_out || size == 0) return false;

    uint64_t start_u = (uint64_t)start;
    if (start_u > UINT64_MAX - size) return false;

    uint64_t aligned_start = start_u & ~(PHYS_PAGE_SIZE - 1ULL);
    uint64_t end_unaligned = start_u + size;
    if (end_unaligned > UINT64_MAX - (PHYS_PAGE_SIZE - 1ULL)) return false;
    uint64_t aligned_end = ((end_unaligned + PHYS_PAGE_SIZE - 1ULL) / PHYS_PAGE_SIZE) * PHYS_PAGE_SIZE;
    if (aligned_end <= aligned_start) return false;

    *aligned_start_out = aligned_start;
    *aligned_end_out = aligned_end;
    return true;
}

void phys_memory_reserve_range(phys_addr_t start, uint64_t size) {
    uint64_t aligned_start = 0, aligned_end = 0;
    if (!bounded_align_range(start, size, &aligned_start, &aligned_end)) return;
    if (aligned_start < PHYS_MEMORY_BASE) aligned_start = PHYS_MEMORY_BASE;
    /* A bootloader information block may live entirely below 1 MiB.  Once
     * the allocator's lower bound is applied, that interval contains no
     * managed physical pages; returning here avoids an unsigned page-count
     * underflow that otherwise loops for billions of pages on UEFI GRUB. */
    if (aligned_end <= aligned_start) return;
    if (aligned_start >= PHYS_MEMORY_BASE + phys_memory_limit_bytes) return;
    if (aligned_end > PHYS_MEMORY_BASE + phys_memory_limit_bytes) aligned_end = PHYS_MEMORY_BASE + phys_memory_limit_bytes;
    if (aligned_end <= aligned_start) return;
    uint64_t first_page = (aligned_start - PHYS_MEMORY_BASE) / PHYS_PAGE_SIZE;
    uint64_t page_count = (aligned_end - aligned_start) / PHYS_PAGE_SIZE;
    reserve_range(first_page, page_count);
    update_stats();
}

void phys_memory_unreserve_range(phys_addr_t start, uint64_t size) {
    uint64_t aligned_start = 0, aligned_end = 0;
    if (!bounded_align_range(start, size, &aligned_start, &aligned_end)) return;
    if (aligned_start < PHYS_MEMORY_BASE) aligned_start = PHYS_MEMORY_BASE;
    /* A bootloader information block may live entirely below 1 MiB.  Once
     * the allocator's lower bound is applied, that interval contains no
     * managed physical pages; returning here avoids an unsigned page-count
     * underflow that otherwise loops for billions of pages on UEFI GRUB. */
    if (aligned_end <= aligned_start) return;
    if (aligned_start >= PHYS_MEMORY_BASE + phys_memory_limit_bytes) return;
    if (aligned_end > PHYS_MEMORY_BASE + phys_memory_limit_bytes) aligned_end = PHYS_MEMORY_BASE + phys_memory_limit_bytes;
    if (aligned_end <= aligned_start) return;
    uint64_t first_page = (aligned_start - PHYS_MEMORY_BASE) / PHYS_PAGE_SIZE;
    uint64_t page_count = (aligned_end - aligned_start) / PHYS_PAGE_SIZE;
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t page = first_page + i;
        if (page >= total_physical_pages) break;
        if (test_bit(page)) {
            clear_bit(page);
            if (used_physical_pages > 0) used_physical_pages--;
            if (page < next_free_hint) {
                next_free_hint = page;
            }
        }
    }
    update_stats();
}

static void update_stats(void) {
    phys_stats.total_pages = total_physical_pages;
    phys_stats.used_pages = used_physical_pages;
    phys_stats.free_pages = (used_physical_pages <= total_physical_pages)
        ? (total_physical_pages - used_physical_pages)
        : 0;
}

void phys_memory_init(void) {
    serial_puts("[PHYS] Initializing physical memory manager\n");

    memset(physical_bitmap, 0, sizeof(physical_bitmap));
    used_physical_pages = 0;
    next_free_hint = PHYS_RESERVED_PAGES;
    memset(&phys_stats, 0, sizeof(phys_stats));

    /* Use *available* memory (Multiboot type=1 regions only), not *total*
     * (which sums every region including Reserved/MMIO - e.g. PCI holes
     * can be tens of GB and would otherwise make the allocator believe
     * physical pages exist far past the end of real RAM; see the
     * [MMAP] log at boot for the underlying per-region breakdown). */
    uint64_t runtime_total = cos_runtime_available_bytes();
    if (runtime_total == 0) {
        runtime_total = PHYS_MAX_MEMORY_SIZE;
    }
    if (runtime_total > PHYS_MAX_MEMORY_SIZE) {
        runtime_total = PHYS_MAX_MEMORY_SIZE;
    }
    if (runtime_total < PHYS_MEMORY_BASE + PHYS_PAGE_SIZE) {
        runtime_total = PHYS_MEMORY_BASE + PHYS_PAGE_SIZE;
    }

    phys_memory_limit_bytes = runtime_total;
    total_physical_pages = phys_memory_limit_bytes / PHYS_PAGE_SIZE;
    if (total_physical_pages == 0) {
        total_physical_pages = 1;
    }
    if (next_free_hint >= total_physical_pages) {
        next_free_hint = (total_physical_pages > PHYS_RESERVED_PAGES) ? PHYS_RESERVED_PAGES : 0;
    }

    /* Reserve low memory for firmware, the bootloader and the kernel image.
     * The kernel itself is loaded at 1 MiB and the heap/page-table backing
     * lives inside the image's BSS, so the entire [PHYS_MEMORY_BASE, _kernel_end)
     * span must stay out of the physical page allocator. */
    reserve_range(0, PHYS_RESERVED_PAGES);

    uint64_t kernel_end = (uint64_t)(uintptr_t)&_kernel_end;
    if (kernel_end > PHYS_MEMORY_BASE) {
        uint64_t end_page = (kernel_end - PHYS_MEMORY_BASE + PHYS_PAGE_SIZE - 1ULL) / PHYS_PAGE_SIZE;
        if (end_page > PHYS_RESERVED_PAGES) {
            reserve_range(PHYS_RESERVED_PAGES, end_page - PHYS_RESERVED_PAGES);
        }
    }
    update_stats();

    serial_puts("[PHYS] Physical memory initialized: ");
    serial_putdec(phys_stats.total_pages);
    serial_puts(" pages (");
    serial_putdec((phys_stats.total_pages * PHYS_PAGE_SIZE) / 1024ULL / 1024ULL);
    serial_puts(" MB managed range)\n");
}

phys_addr_t phys_alloc_page(void) {
    if (used_physical_pages >= total_physical_pages) {
        serial_puts("[PHYS] Out of physical memory!\n");
        return 0;
    }

    uint64_t start = (next_free_hint < total_physical_pages) ? next_free_hint : PHYS_RESERVED_PAGES;
    for (uint64_t pass = 0; pass < 2; ++pass) {
        uint64_t begin = (pass == 0) ? start : PHYS_RESERVED_PAGES;
        for (uint64_t page = begin; page < total_physical_pages; ++page) {
            if (!test_bit(page)) {
                set_bit(page);
                used_physical_pages++;
                next_free_hint = page + 1;
                if (next_free_hint < PHYS_RESERVED_PAGES) {
                    next_free_hint = PHYS_RESERVED_PAGES;
                }
                update_stats();
                return (phys_addr_t)(PHYS_MEMORY_BASE + (page * PHYS_PAGE_SIZE));
            }
        }
    }

    serial_puts("[PHYS] Out of physical memory!\n");
    return 0;
}

void phys_free_page(phys_addr_t addr) {
    if (((uint64_t)addr & (PHYS_PAGE_SIZE - 1ULL)) != 0) {
        serial_puts("[PHYS] Attempt to free unaligned physical page\n");
        return;
    }

    if ((uint64_t)addr < PHYS_MEMORY_BASE) {
        serial_puts("[PHYS] Attempt to free below physical base\n");
        return;
    }

    uint64_t page_num = ((uint64_t)addr - PHYS_MEMORY_BASE) / PHYS_PAGE_SIZE;
    if (page_num >= total_physical_pages) {
        serial_puts("[PHYS] Invalid physical address to free\n");
        return;
    }

    if (page_num < PHYS_RESERVED_PAGES) {
        serial_puts("[PHYS] Attempt to free reserved page\n");
        return;
    }

    if (!test_bit(page_num)) {
        serial_puts("[PHYS] Double free of physical page\n");
        return;
    }

    clear_bit(page_num);
    if (used_physical_pages > 0) {
        used_physical_pages--;
    }
    if (page_num < next_free_hint) {
        next_free_hint = page_num;
    }
    update_stats();
}

phys_mem_stats_t* phys_memory_get_stats(void) {
    update_stats();
    return &phys_stats;
}

bool phys_is_valid_addr(phys_addr_t addr) {
    return (addr >= PHYS_MEMORY_BASE) &&
           ((uint64_t)addr < (PHYS_MEMORY_BASE + phys_memory_limit_bytes)) &&
           (((uint64_t)addr & (PHYS_PAGE_SIZE - 1ULL)) == 0);
}

uint64_t phys_get_total_memory(void) {
    return phys_memory_limit_bytes;
}
