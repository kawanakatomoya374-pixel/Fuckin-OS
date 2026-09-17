/**
 * paging_fixed.h - C-OS 4.0.8 alpha Clean Paging System
 * Fixed 64-bit address conversion constants
 */

#ifndef PAGING_FIXED_H
#define PAGING_FIXED_H

#include "types.h"

/* 64-bit Page Table Entry Structure */
typedef struct {
    uint64_t global    : 1;
    uint64_t avail0    : 3;
    uint64_t frame     : 40;   /* Physical address */
    uint64_t avail1    : 11;
    uint64_t nx        : 1;    /* No Execute */
} __attribute__((packed)) pt_entry_struct_t;

/* 64-bit Page Table Structures */
typedef struct {
    pte_t entries[512];
} __attribute__((aligned(PAGE_SIZE))) page_table_t;

typedef struct {
    pte_t entries[512];
} __attribute__((aligned(PAGE_SIZE))) page_directory_t;

typedef struct {
    pte_t entries[512];
} __attribute__((aligned(PAGE_SIZE))) pdpt_t;

typedef struct {
    pte_t entries[512];
} __attribute__((aligned(PAGE_SIZE))) pml4_t;

/* Page Table Pointers */
extern pml4_t* kernel_pml4;

/* Memory Map Flags - Fixed 64-bit constants */
#ifndef MEM_PRESENT
#define MEM_PRESENT      (1ULL << 0)
#endif
#ifndef MEM_WRITE
#define MEM_WRITE        (1ULL << 1)
#endif
#ifndef MEM_USER
#define MEM_USER         (1ULL << 2)
#endif
#ifndef MEM_WRITETHROUGH
#define MEM_WRITETHROUGH (1ULL << 3)
#endif
#ifndef MEM_NOCACHE
#define MEM_NOCACHE      (1ULL << 4)
#endif
#ifndef MEM_GLOBAL
#define MEM_GLOBAL       (1ULL << 8)
#endif
#ifndef MEM_NX
#define MEM_NX           (1ULL << 63)
#endif

/* Page Alignment Macros */
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGNED(addr) (((addr) & (PAGE_SIZE - 1)) == 0)

/* Physical to Virtual Address Conversion - CORRECTED */
#define PHYS_TO_VIRT(phys) ((phys) + 0xFFFF800000000000ULL)
#define VIRT_TO_PHYS(virt) ((virt) - 0xFFFF800000000000ULL)

/* Paging Functions */
void paging_init(void);
pml4_t* paging_get_current_pml4(void);
void paging_switch_pml4(pml4_t* pml4);
page_table_t* paging_alloc_page_table(void);
bool paging_map_page(virt_addr_t virt_addr, phys_addr_t phys_addr, uint64_t flags);
bool paging_unmap_page(virt_addr_t virt_addr);
phys_addr_t paging_get_phys_addr(virt_addr_t virt_addr);
void paging_handle_fault(uint64_t error_code, virt_addr_t fault_addr);

/* Memory Management Functions */
void* paging_create_directory(void);
void paging_destroy_directory(void* dir);
void paging_flush_tlb(virt_addr_t addr);

/* Memory Management Functions */
void* kmalloc(size_t size);
void kfree(void* ptr);
void* kmalloc_aligned(size_t size, size_t alignment);
void* kmalloc_pages(size_t pages);

#endif // PAGING_FIXED_H
