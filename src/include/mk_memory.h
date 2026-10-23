#ifndef MK_MEMORY_H
#define MK_MEMORY_H

#include "types.h"
#include "mk_core.h"
#include "mk_ipc.h"

// Memory management constants
#define MK_MEMORY_MAGIC 0x4D454D5F  // "MEM_"
#define MK_MAX_MEMORY_POOLS 16
#define MK_MAX_MEMORY_REGIONS 32
#define MK_MAX_FREE_BLOCKS 1024
#define MK_MAX_ALLOCATED_BLOCKS 1024
#define MK_MEMORY_SERVER_PID 10

// Block types
#define MK_BLOCK_FREE 0
#define MK_BLOCK_ALLOCATED 1
#define MK_BLOCK_RESERVED 2

// Region types
#define MK_REGION_UNUSED 0
#define MK_REGION_KERNEL 1
#define MK_REGION_USER 2
#define MK_REGION_DEVICE 3
#define MK_REGION_VIDEO 4
#define MK_REGION_SHARED 5

// Protection flags
#define MK_PROTECTION_READ 0x01
#define MK_PROTECTION_WRITE 0x02
#define MK_PROTECTION_EXECUTE 0x04
#define MK_PROTECTION_KERNEL 0x80

// Allocation flags
#define MK_ALLOC_NORMAL 0x00
#define MK_ALLOC_ZEROED 0x01
#define MK_ALLOC_ALIGNED 0x02
#define MK_ALLOC_CONTIGUOUS 0x04
#define MK_ALLOC_DMA 0x08
#define MK_ALLOC_KERNEL 0x80

// Message types for memory server
#define MK_MEMORY_MSG_ALLOCATE 1
#define MK_MEMORY_MSG_FREE 2
#define MK_MEMORY_MSG_GET_STATS 3
#define MK_MEMORY_MSG_RESPONSE 4
#define MK_MEMORY_MSG_STATS 5

// Forward declarations
typedef struct mk_memory_block mk_memory_block_t;
typedef struct mk_memory_pool mk_memory_pool_t;
typedef struct mk_memory_region mk_memory_region_t;
typedef struct mk_memory_state mk_memory_state_t;

// Memory block structure
struct mk_memory_block {
    uint64_t address;
    uint64_t size;
    uint64_t type;
    uint64_t magic;
    uint64_t owner_pid;
    uint64_t creation_time;
    uint64_t access_count;
    uint64_t protection;
    mk_memory_block_t* next;
    mk_memory_block_t* prev;
};

// Memory pool structure
struct mk_memory_pool {
    uint64_t block_size;
    uint64_t free_count;
    uint64_t total_count;
    mk_memory_block_t* free_list;
    uint64_t allocation_count;
    uint64_t deallocation_count;
};

// Memory region structure
struct mk_memory_region {
    uint64_t base;
    uint64_t size;
    uint64_t type;
    uint64_t protection;
    uint64_t ref_count;
    char name[32];
    uint64_t creation_time;
    uint64_t access_count;
};

// Memory state structure
struct mk_memory_state {
    uint64_t magic;
    uint64_t total_memory;
    uint64_t free_memory;
    uint64_t allocated_memory;
    uint64_t fragmented_memory;
    uint64_t largest_free_block;
    uint64_t allocation_count;
    uint64_t deallocation_count;
    uint64_t fragmention_count;
    uint64_t page_fault_count;
    uint64_t protection_violations;
};

// Memory request structure
typedef struct {
    uint64_t size;
    uint64_t flags;
    uint64_t address;
    uint64_t alignment;
} mk_memory_request_t;

// Memory response structure
typedef struct {
    uint64_t address;
    uint64_t size;
    bool success;
    uint64_t error_code;
} mk_memory_response_t;

// Memory management functions
void mk_memory_init(void);
void* mk_allocate_memory(uint64_t size, uint64_t flags);
void mk_free_memory(void* ptr);
void* mk_reallocate_memory(void* ptr, uint64_t new_size);

// Memory pool functions
void mk_init_memory_pools(void);
void* mk_allocate_from_pool(uint64_t size);
void mk_free_to_pool(void* ptr, uint64_t size);

// Memory region functions
mk_memory_region_t* mk_create_memory_region(uint64_t base, uint64_t size, uint64_t type, const char* name);
mk_memory_region_t* mk_find_memory_region(uint64_t address);
int mk_protect_memory_region(uint64_t base, uint64_t size, uint64_t protection);
int mk_unprotect_memory_region(uint64_t base, uint64_t size);

// Memory block functions
mk_memory_block_t* mk_create_memory_block(uint64_t address, uint64_t size, uint64_t type);
void mk_split_block(mk_memory_block_t* block, uint64_t size);
void mk_merge_blocks(mk_memory_block_t* block);

// Memory optimization functions
void mk_defragment_memory(void);
void mk_compact_memory(void);
void mk_garbage_collect(void);

// Memory statistics functions
mk_memory_state_t* mk_get_memory_state(void);
uint64_t mk_get_memory_usage(void);
uint64_t mk_get_fragmentation_level(void);
uint64_t mk_get_largest_free_block(void);

// Memory debugging functions
void mk_dump_memory_map(void);
void mk_dump_memory_pools(void);
void mk_dump_memory_regions(void);
void mk_check_memory_integrity(void);
void mk_detect_memory_leaks(void);

// Advanced memory functions
void* mk_allocate_aligned_memory(uint64_t size, uint64_t alignment);
void* mk_allocate_contiguous_memory(uint64_t size);
void* mk_allocate_dma_memory(uint64_t size);
int mk_lock_memory(void* ptr, uint64_t size);
int mk_unlock_memory(void* ptr, uint64_t size);

// Memory sharing functions
int mk_share_memory(void* ptr, uint64_t size, uint64_t target_pid);
void* mk_attach_shared_memory(uint64_t shared_id);
int mk_detach_shared_memory(uint64_t shared_id);

// Memory protection functions
int mk_set_memory_protection(void* ptr, uint64_t size, uint64_t protection);
uint64_t mk_get_memory_protection(void* ptr);
int mk_check_memory_access(void* ptr, uint64_t size, uint64_t access_type);

// Memory server functions
void mk_memory_server_main(void);

// Performance monitoring
typedef struct {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t realloc_count;
    uint64_t peak_usage;
    uint64_t current_usage;
    uint64_t fragmentation_level;
    uint64_t allocation_time_total;
    uint64_t deallocation_time_total;
} mk_memory_stats_t;

mk_memory_stats_t* mk_get_memory_performance_stats(void);
void mk_reset_memory_performance_stats(void);

// Memory constants
#define MK_BLOCK_MAGIC 0x424C4F43  // "BLOC"
#define MK_MIN_BLOCK_SIZE 32
#define MK_MAX_BLOCK_SIZE (64 * 1024 * 1024)  // 64MB
#define MK_MEMORY_ALIGNMENT 16
#define MK_MEMORY_GUARD_SIZE 16

#endif // MK_MEMORY_H
