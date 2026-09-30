/**
 * memory_virtual.c - Legacy virtual-memory compatibility shim.
 *
 * The old standalone VM implementation has been retired. The kernel now
 * uses mm/paging.c as the single source of truth; this file remains only
 * so older GUI / tooling code can keep calling the legacy symbols without
 * pulling in a second paging implementation.
 */

#include "types.h"
#include "mm/paging.h"
#include "memory.h"
#include "serial.h"
#include "string.h"

typedef uint64_t virt_addr_t;
typedef uint64_t phys_addr_t;

typedef struct {
    uint64_t total_pages;
    uint64_t kernel_pages;
    uint64_t user_pages;
    uint64_t mapped_pages;
    uint64_t page_faults;
    uint64_t tlb_flushes;
} virt_mem_stats_t;

static virt_mem_stats_t s_virt_stats;

static void virt_sync_stats(void) {
    page_stats_t ps = {0};
    paging_get_stats(&ps);
    s_virt_stats.total_pages = ps.total_pages;
    s_virt_stats.kernel_pages = ps.kernel_pages;
    s_virt_stats.user_pages = ps.user_pages;
    s_virt_stats.mapped_pages = ps.used_pages;
    /* The legacy compatibility shim does not maintain its own fault/TLB
     * counters. Keep the fields valid and deterministic. */
    s_virt_stats.page_faults = 0;
    s_virt_stats.tlb_flushes = 0;
}

void virt_memory_init(void) {
    memset(&s_virt_stats, 0, sizeof(s_virt_stats));
    virt_sync_stats();
    serial_puts("[VIRT] Legacy VM disabled; paging.c compatibility layer active\n");
}

void virt_map_page(virt_addr_t vaddr, phys_addr_t paddr, uint64_t flags) {
    (void)vaddr;
    (void)paddr;
    (void)flags;
    /* Intentionally stubbed: the kernel uses paging.c directly now. */
}

void virt_unmap_page(virt_addr_t vaddr) {
    (void)vaddr;
}

phys_addr_t virt_to_phys(virt_addr_t vaddr) {
    return (phys_addr_t)paging_virt_to_phys((uint64_t)vaddr);
}

bool virt_is_mapped(virt_addr_t vaddr) {
    return paging_is_present((uint64_t)vaddr);
}

virt_mem_stats_t* virt_memory_get_stats(void) {
    virt_sync_stats();
    return &s_virt_stats;
}

uint64_t virt_memory_get_total_pages(void) {
    virt_sync_stats();
    return s_virt_stats.total_pages;
}

uint64_t virt_memory_get_kernel_pages(void) {
    virt_sync_stats();
    return s_virt_stats.kernel_pages;
}

uint64_t virt_memory_get_user_pages(void) {
    virt_sync_stats();
    return s_virt_stats.user_pages;
}

uint64_t virt_memory_get_mapped_pages(void) {
    virt_sync_stats();
    return s_virt_stats.mapped_pages;
}

uint64_t virt_memory_get_page_faults(void) {
    virt_sync_stats();
    return s_virt_stats.page_faults;
}

uint64_t virt_memory_get_tlb_flushes(void) {
    virt_sync_stats();
    return s_virt_stats.tlb_flushes;
}

void virt_flush_tlb(void) {
    paging_flush_tlb();
}

void virt_page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    (void)error_code;
    (void)fault_addr;
    serial_puts("[VIRT] legacy page fault handler stub called\n");
}
