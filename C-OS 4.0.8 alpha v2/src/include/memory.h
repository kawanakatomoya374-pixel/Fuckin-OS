#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* Shared memory-statistics types — both src/include/memory.h and
 * src/kernel/mm/memory.h need to agree on these typedefs so that
 * kmemory_get_stats() and friends link from any translation unit. */
typedef struct {
    uint64_t total_mb;
    uint64_t used_mb;
    uint64_t free_mb;
    uint64_t total_allocs;
    uint64_t active_allocs;
    uint64_t peak_used;
} mem_stats_t;

/* Old allocator descriptor — kept for ABI compatibility with any consumer
 * that still includes the legacy routine names. */
typedef struct block_header {
    uint64_t magic;
    uint64_t size;
    uint64_t real_size;
    uint8_t  free_;
    uint8_t  zone;
    uint8_t  guard[2];
    struct block_header* next;
    struct block_header* prev;
} block_header_t;

/* Memory management functions */
void  memory_init(void);
void* kmalloc(size_t size);          /* FIX: was void* kmalloc(void) -- missing size argument */
void* kmalloc_aligned(size_t size, size_t align);
void* kmalloc_pages(size_t pages);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);

/* Memory statistics */
uint64_t memory_get_total(void);
uint64_t memory_get_used(void);
uint64_t memory_get_free(void);
void     memory_dump(void);

/* ------------------------------------------------------------------
 * Runtime memory authority — single source of truth that mirrors the
 * amount of RAM actually assigned to the VM/virtualizer by the host
 * (VirtualBox, QEMU, real hardware, etc.).
 *
 * These values are populated from the multiboot2 memory-map tag very
 * early during boot so every subsystem (memory.c, mm/memory.c, the
 * GUI status bar, About, System Info) shows the same number.
 * ------------------------------------------------------------------ */
void     cos_runtime_memory_init(uint64_t physical_total_bytes,
                                 uint64_t available_total_bytes);
void     cos_runtime_memory_set_heap(uint64_t heap_size_bytes);
/* Highest physical end address that must be reachable through PHYS_TO_VIRT.
 * This is RAM plus ACPI firmware extents, not a sum of PCI/MMIO descriptors. */
void     cos_runtime_memory_set_direct_map_extent(uint64_t extent_bytes);
uint64_t cos_runtime_direct_map_extent(void);
uint64_t cos_runtime_total_bytes(void);
uint64_t cos_runtime_available_bytes(void);
uint64_t cos_runtime_heap_bytes(void);
uint64_t cos_runtime_allocated_bytes(void);
uint64_t cos_runtime_peak_bytes(void);

bool     memory_heap_ready(void);

uint64_t memory_get_total(void);
uint64_t memory_get_used(void);
uint64_t memory_get_free(void);

uint64_t memory_get_heap_total(void);
uint64_t memory_get_heap_used(void);
uint64_t memory_get_heap_peak(void);
uint64_t memory_get_heap_free(void);
uint64_t memory_get_apps_used(void);

#endif /* MEMORY_H */
