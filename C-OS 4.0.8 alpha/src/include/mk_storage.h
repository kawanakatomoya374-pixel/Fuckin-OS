#ifndef MK_STORAGE_H
#define MK_STORAGE_H

#include "types.h"
#include "mk_core.h"
#include "mk_ipc.h"

// Storage constants
#define MK_STORAGE_MAGIC 0x53544F5F  // "STO_"
#define MK_STORAGE_TOTAL_SIZE (512ULL * 1024 * 1024)   // 512MB
#define MK_STORAGE_BLOCK_SIZE 4096
#define MK_STORAGE_TOTAL_BLOCKS (MK_STORAGE_TOTAL_SIZE / MK_STORAGE_BLOCK_SIZE)
#define MK_STORAGE_BITMAP_SIZE (MK_STORAGE_TOTAL_BLOCKS / 8)
#define MK_MAX_OPEN_FILES 256
#define MK_MAX_BLOCKS_PER_FILE 1024
#define MK_STORAGE_SERVER_PID 11

// File permissions
#define MK_STORAGE_PERM_READ    0x01
#define MK_STORAGE_PERM_WRITE   0x02
#define MK_STORAGE_PERM_EXECUTE  0x04
#define MK_STORAGE_PERM_OWNER   0x08
#define MK_STORAGE_PERM_GROUP   0x10
#define MK_STORAGE_PERM_OTHER   0x20

// Directory permissions
#define MK_STORAGE_DIR_READ    0x01
#define MK_STORAGE_DIR_WRITE   0x02
#define MK_STORAGE_DIR_EXECUTE  0x04
#define MK_STORAGE_DIR_OWNER   0x08
#define MK_STORAGE_DIR_GROUP   0x10
#define MK_STORAGE_DIR_OTHER   0x20

// Message types for storage server
#define MK_STORAGE_MSG_CREATE_FILE 1
#define MK_STORAGE_MSG_READ_FILE 2
#define MK_STORAGE_MSG_WRITE_FILE 3
#define MK_STORAGE_MSG_DELETE_FILE 4
#define MK_STORAGE_MSG_CREATE_DIR 5
#define MK_STORAGE_MSG_DELETE_DIR 6
#define MK_STORAGE_MSG_LIST_DIR 7
#define MK_STORAGE_MSG_GET_STATS 8
#define MK_STORAGE_MSG_RESPONSE 9
#define MK_STORAGE_MSG_DATA 10
#define MK_STORAGE_MSG_STATS 11

// Forward declarations
typedef struct mk_storage_block mk_storage_block_t;
typedef struct mk_storage_file mk_storage_file_t;
typedef struct mk_storage_directory mk_storage_directory_t;
typedef struct mk_storage_state mk_storage_state_t;
typedef struct mk_storage_request mk_storage_request_t;
typedef struct mk_storage_response mk_storage_response_t;

// Storage block structure
struct mk_storage_block {
    uint64_t id;
    uint64_t size;
    bool used;
    uint64_t checksum;
    uint64_t creation_time;
    uint64_t access_count;
};

// Storage file structure
struct mk_storage_file {
    uint64_t id;
    char name[256];
    char extension[16];
    uint64_t directory_id;
    uint64_t size;
    uint64_t permissions;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t access_time;
    uint64_t block_count;
    uint64_t blocks[MK_MAX_BLOCKS_PER_FILE];
    uint64_t reference_count;
};

// Storage directory structure
struct mk_storage_directory {
    uint64_t id;
    char name[256];
    uint64_t parent_id;
    uint64_t permissions;
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t access_time;
    uint64_t file_count;
    uint64_t directory_count;
    uint64_t size;
};

// Storage state structure
struct mk_storage_state {
    uint64_t magic;
    uint64_t total_size;
    uint64_t free_size;
    uint64_t used_size;
    uint64_t total_blocks;
    uint64_t free_blocks;
    uint64_t used_blocks;
    uint64_t file_count;
    uint64_t directory_count;
    uint64_t read_operations;
    uint64_t write_operations;
    uint64_t delete_operations;
    uint64_t create_operations;
};

// Storage request structure
struct mk_storage_request {
    char filename[256];
    uint64_t directory_id;
    uint64_t file_id;
    uint64_t size;
    uint64_t permissions;
    uint64_t offset;
    void* buffer;
    uint64_t flags;
};

// Storage response structure
struct mk_storage_response {
    bool success;
    uint64_t file_id;
    uint64_t bytes_transferred;
    uint64_t error_code;
    uint64_t free_space;
    uint64_t total_space;
};

// Storage functions
void mk_storage_init(void);
mk_storage_file_t* mk_storage_create_file(const char* name, uint64_t directory_id, uint64_t size, uint64_t permissions);
mk_storage_directory_t* mk_storage_create_directory(const char* name, uint64_t parent_id, uint64_t permissions);
int mk_storage_read_file(mk_storage_file_t* file, void* buffer, uint64_t offset, uint64_t size);
int mk_storage_write_file(mk_storage_file_t* file, const void* buffer, uint64_t offset, uint64_t size);
int mk_storage_delete_file(mk_storage_file_t* file);
mk_storage_file_t* mk_storage_open_file(const char* filename);
int mk_storage_close_file(mk_storage_file_t* file);
int mk_storage_seek_file(mk_storage_file_t* file, uint64_t offset);
mk_storage_directory_t* mk_storage_find_directory(uint64_t directory_id);
mk_storage_file_t* mk_storage_find_file(uint64_t file_id);

// Storage block functions
uint64_t mk_storage_allocate_block(void);
void mk_storage_free_block(uint64_t block_id);
mk_storage_block_t* mk_storage_get_block(uint64_t block_id);

// Storage statistics
mk_storage_state_t* mk_storage_get_state(void);
uint64_t mk_storage_get_free_space(void);
uint64_t mk_storage_get_total_space(void);
uint64_t mk_storage_get_file_count(void);
uint64_t mk_storage_get_directory_count(void);

// Storage server functions
void mk_storage_server_main(void);

// Storage maintenance
void mk_storage_defragment(void);
void mk_storage_check_integrity(void);
void mk_storage_optimize_layout(void);

// Storage format support
void mk_storage_init_fat32(void);
void mk_storage_init_ext4(void);
void mk_storage_init_ntfs(void);
int mk_storage_format_fat32(void);
int mk_storage_format_ext4(void);
int mk_storage_format_ntfs(void);

// Storage caching
typedef struct {
    uint64_t file_id;
    void* cache_data;
    uint64_t cache_size;
    uint64_t access_count;
    uint64_t last_access;
    bool dirty;
} mk_storage_cache_entry_t;

void mk_storage_cache_init(void);
void* mk_storage_cache_get(uint64_t file_id);
void mk_storage_cache_put(uint64_t file_id, void* data, uint64_t size);
void mk_storage_cache_invalidate(uint64_t file_id);
void mk_storage_cache_flush(void);

// Storage security
int mk_storage_check_permissions(mk_storage_file_t* file, uint64_t required_permissions);
int mk_storage_set_permissions(mk_storage_file_t* file, uint64_t permissions);
int mk_storage_encrypt_file(mk_storage_file_t* file, const char* password);
int mk_storage_decrypt_file(mk_storage_file_t* file, const char* password);

// Storage backup and restore
int mk_storage_backup_create(const char* backup_path);
int mk_storage_backup_restore(const char* backup_path);
int mk_storage_snapshot_create(const char* snapshot_name);
int mk_storage_snapshot_restore(const char* snapshot_name);

// Storage performance monitoring
typedef struct {
    uint64_t read_count;
    uint64_t write_count;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t fragmentation_level;
    uint64_t average_seek_time;
} mk_storage_stats_t;

mk_storage_stats_t* mk_storage_get_performance_stats(void);
void mk_storage_reset_performance_stats(void);

#endif // MK_STORAGE_H
