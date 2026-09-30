#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAX_ENTRIES 128
#define FS_MAX_NAME    64
#define FS_MAX_DATA    (32 * 1024)  /* 64KB per file - increased for text editing */
#define FS_MAX_PATH    256

/* File type classification */
typedef enum {
    FS_FILE_TYPE_UNKNOWN = 0,
    FS_FILE_TYPE_TEXT    = 1,
    FS_FILE_TYPE_BINARY  = 2,
    FS_FILE_TYPE_DIR     = 3,
    FS_FILE_TYPE_MEDIA   = 4,
    FS_FILE_TYPE_AUDIO   = 5
} fs_file_type_t;

/* Enhanced file metadata */
typedef struct {
    char     name[FS_MAX_NAME];
    char     path[FS_MAX_PATH];          /* Full path including parent dirs */
    bool     is_dir;
    uint64_t size;
    char     data[FS_MAX_DATA];
    
    /* Enhanced metadata */
    uint64_t created_time;               /* Creation timestamp */
    uint64_t modified_time;              /* Last modification timestamp */
    uint64_t accessed_time;              /* Last access timestamp */
    uint8_t  permissions;               /* File permissions (rwx for owner/group/others) */
    bool     is_hidden;                 /* Hidden file flag */
    bool     is_system;                 /* System file flag */
    uint8_t  file_type;                /* File type identifier */
    uint8_t  attributes;               /* raw FAT attributes (AM_RDO/HID/SYS/DIR/ARC) */
} fs_entry_t;

/* Directory monitoring structure */
typedef struct {
    char     path[FS_MAX_PATH];
    bool     active;
    uint64_t last_check_time;
    uint64_t entry_count;
    fs_entry_t entries[FS_MAX_ENTRIES];
} fs_monitor_t;

void        fs_init(void);
fs_entry_t* fs_list_dir(const char* path);
int         fs_entry_count(void);
int         fs_entry_count_for_path(const char* path);
fs_entry_t* fs_find(const char* name);
bool fs_stat_path(const char* path, uint64_t* out_size, bool* out_is_dir);
bool fs_get_space(uint64_t* total_bytes, uint64_t* free_bytes);
bool fs_read_path_into(const char* path, void* buf, uint64_t buf_size, uint64_t* out_read);
typedef struct fs_stream fs_stream_t;
fs_stream_t* fs_stream_open(const char* path);
int64_t      fs_stream_read_at(fs_stream_t* st, uint64_t offset, void* buf, uint64_t n);
uint64_t     fs_stream_size(fs_stream_t* st);
void         fs_stream_close(fs_stream_t* st);

/* Path-based operations (new) */
bool        fs_create_file_at(const char* path, const char* name);
bool        fs_create_dir_at(const char* path, const char* name);
bool        fs_write_file_at(const char* path, const char* name, const char* data, uint64_t size);
bool        fs_copy_path(const char* src_full_path, const char* dst_full_path);
bool        fs_copy_file(const char* src_full_path, const char* dst_full_path);
bool        fs_move_path(const char* src_full_path, const char* dst_full_path);
bool        fs_is_text_file(const fs_entry_t* entry);
bool        fs_looks_like_text(const char* data, uint64_t size);

/* Enhanced file system functions */
bool        fs_start_monitor(const char* path, fs_monitor_t* monitor);
bool        fs_stop_monitor(fs_monitor_t* monitor);
bool        fs_check_changes(fs_monitor_t* monitor);
bool        fs_has_changes(const char* path);
uint64_t    fs_get_current_time(void);
void        fs_update_metadata(fs_entry_t* entry);
bool        fs_is_desktop_file(const fs_entry_t* entry);

/* Legacy root-based operations */
bool        fs_create_file(const char* name);
bool        fs_create_dir(const char* name);
bool        fs_write_file(const char* name, const char* data, uint64_t size);
bool        fs_delete(const char* name);
bool        fs_delete_file(const char* name);
bool        fs_delete_dir(const char* name);
int         fs_rename(const char* old_name, const char* new_name);
const char* fs_read_file(const char* name);

#endif
