#ifndef MEMORY_PHYSICAL_H
#define MEMORY_PHYSICAL_H

#include "types.h"

void phys_memory_init(void);
phys_addr_t phys_alloc_page(void);
void phys_free_page(phys_addr_t addr);
bool phys_is_valid_addr(phys_addr_t addr);
void phys_memory_reserve_range(phys_addr_t start, uint64_t size);
void phys_memory_unreserve_range(phys_addr_t start, uint64_t size);
uint64_t phys_get_total_memory(void);

/* Optional statistics structure from memory_physical.c */
typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t allocations;
    uint64_t deallocations;
} phys_mem_stats_t;

phys_mem_stats_t* phys_memory_get_stats(void);

#endif /* MEMORY_PHYSICAL_H */
