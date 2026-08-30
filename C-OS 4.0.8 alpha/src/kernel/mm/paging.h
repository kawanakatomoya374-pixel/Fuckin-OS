/**
 * paging.h - x86 Paging System
 * 
 * Virtual memory management with page tables and directories.
 * Supports 4KB pages with full 4GB address space mapping.
 */

#ifndef PAGING_H
#define PAGING_H

#include "types.h"

/* Page size and alignment */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096
#endif
#ifndef PAGE_MASK
#define PAGE_MASK       0xFFFFF000
#endif
#define PAGE_OFFSET     0x00000FFF

/* Convert a physical address to the kernel-accessible virtual address
 * that maps it. paging_map_kernel_higher() identity-maps the first
 * 4GB of physical RAM starting at this offset (see paging.c), so any
 * code that needs to read/write through a *physical* address (as
 * opposed to a process's own virtual address space) must go through
 * here rather than casting the physical address straight to a
 * pointer - physical addresses are not valid virtual addresses once
 * paging is enabled and will fault (or silently hit whatever happens
 * to be mapped at that same low address in the *current* page
 * directory) otherwise. Only valid for the first 4GB of physical RAM,
 * matching the range paging_map_kernel_higher() actually maps. */
#define PHYS_TO_VIRT(phys) ((phys) + 0xFFFF800000000000ULL)
#define VIRT_TO_PHYS(virt) ((virt) - 0xFFFF800000000000ULL)

/* Page table/directory entries count */
#define PAGES_PER_TABLE 1024
#define TABLES_PER_DIR  1024
#define TOTAL_PAGES     (PAGES_PER_TABLE * TABLES_PER_DIR)

/* Page flags - Lower 12 bits of page entry */
#define PAGE_PRESENT    0x001   // Page is present in memory
#define PAGE_RW         0x002   // Read/Write (0=read-only)
#define PAGE_USER       0x004   // User/Supervisor (0=supervisor only)
#define PAGE_WRITETHRU  0x008   // Write-through caching
#define PAGE_NOCACHE    0x010   // Disable caching
#define PAGE_ACCESSED   0x020   // Page was accessed
#define PAGE_DIRTY      0x040   // Page was written to
#define PAGE_GLOBAL     0x100   // Global page (TLB don't flush)
#define PAGE_FRAME      0xFFFFF000  // Physical address mask

/* Page directory/table types */
typedef uint64_t page_entry_t;
typedef uint64_t page_table_t[PAGES_PER_TABLE];
typedef uint64_t page_directory_t[TABLES_PER_DIR];

/* Page allocation structure */
typedef struct page_frame {
    uint64_t physical_addr;
    uint64_t virtual_addr;
    uint64_t flags;
    struct page_frame* next;
    struct page_frame* prev;
} page_frame_t;

/* Page pool statistics */
typedef struct {
    uint64_t total_pages;
    uint64_t used_pages;
    uint64_t free_pages;
    uint64_t kernel_pages;
    uint64_t user_pages;
} page_stats_t;

/* Kernel heap allocation tracking */
typedef struct heap_region {
    uint64_t start;
    uint64_t end;
    uint64_t used;
    struct heap_region* next;
} heap_region_t;

/* Function prototypes */

/* Initialization */
void paging_init(void);
void paging_enable(void);
void paging_disable(void);
bool paging_is_enabled(void);

/* Page directory management */
page_directory_t* paging_create_directory(void);
void paging_destroy_directory(page_directory_t* dir);
void paging_switch_directory(page_directory_t* dir);
page_directory_t* paging_get_current_directory(void);

/* Page table management */
page_table_t* paging_create_table(void);
void paging_destroy_table(page_table_t* table);
bool paging_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);
void paging_unmap_page(uint64_t virtual_addr);
bool paging_map_range(uint64_t virt_start, uint64_t phys_start, uint64_t size, uint64_t flags);
void paging_unmap_range(uint64_t virt_start, uint64_t size);

/* Page allocation */
uint64_t paging_alloc_page(uint64_t flags);
void paging_free_page(uint64_t virtual_addr);
uint64_t paging_alloc_pages(uint64_t count, uint64_t flags);
void paging_free_pages(uint64_t virtual_addr, uint64_t count);

/* Address translation */
uint64_t paging_virt_to_phys(uint64_t virtual_addr);
bool paging_is_present(uint64_t virtual_addr);
bool paging_is_writable(uint64_t virtual_addr);
bool paging_is_user(uint64_t virtual_addr);

/* Page faults */
typedef enum {
    PF_PRESENT      = 0x01,  // Page was present
    PF_WRITE        = 0x02,  // Operation was write
    PF_USER         = 0x04,  // User mode access
    PF_RESERVED     = 0x08,  // Reserved bit set
    PF_INSTR_FETCH  = 0x10,  // Instruction fetch
} page_fault_flags_t;

typedef void (*page_fault_handler_t)(uint64_t fault_addr, uint64_t error_code);
void paging_register_fault_handler(page_fault_handler_t handler);
void paging_page_fault_handler(uint64_t error_code);

/* TLB (Translation Lookaside Buffer) */
void paging_flush_tlb(void);
void paging_flush_tlb_entry(uint64_t virtual_addr);
void paging_invalidate_page(uint64_t virtual_addr);

/* Kernel heap with paging */
void* paging_kmalloc(size_t size);
void* paging_kmalloc_aligned(size_t size, uint64_t alignment);
void paging_kfree(void* ptr);
void* paging_krealloc(void* ptr, size_t new_size);

/* User / process helpers */
bool paging_clone_user_range(page_directory_t* dst, page_directory_t* src, uint64_t start, uint64_t size);
bool paging_setup_user_stack(page_directory_t* dir, uint64_t top, uint64_t pages);
bool paging_setup_user_code(page_directory_t* dir, uint64_t base, const uint8_t* code, uint64_t len);
void paging_mark_user_directory(page_directory_t* dir);

/* Memory regions */
bool paging_protect_range(uint64_t start, uint64_t size, uint64_t new_flags);
bool paging_set_readonly(uint64_t start, uint64_t size);
bool paging_set_writable(uint64_t start, uint64_t size);
bool paging_set_user(uint64_t start, uint64_t size);
bool paging_set_supervisor(uint64_t start, uint64_t size);

/* Copy-on-Write */
bool paging_mark_cow(uint64_t start, uint64_t size);
bool paging_handle_cow(uint64_t fault_addr);

/* Statistics and debugging */
void paging_get_stats(page_stats_t* stats);
void paging_print_stats(void);
void paging_dump_directory(page_directory_t* dir);
void paging_dump_page(uint64_t virtual_addr);
void paging_dump_range(uint64_t start, uint64_t size);

/* Higher half kernel support */
void paging_map_kernel_higher(void);
bool paging_is_higher_half(uint64_t virtual_addr);

/* Write-Combining support (see paging.c) - used to accelerate MMIO
 * regions like the linear framebuffer after they're already mapped
 * by paging_map_kernel_higher(). */
void paging_configure_pat_wc(void);
uint64_t paging_mark_region_wc(uint64_t phys_start, uint64_t size);

/* Physical memory manager interface */
uint64_t paging_alloc_physical(void);
void paging_free_physical(uint64_t phys_addr);
uint64_t paging_get_free_memory(void);
uint64_t paging_get_total_memory(void);

#endif /* PAGING_H */
