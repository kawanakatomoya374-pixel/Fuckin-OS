/**
 * file_manager.h - File Management System for GUI
 * C-OS 4.0.8 alpha
 * 
 * Separated file operations from GUI core
 */

#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "../include/types.h"

// File operations
typedef enum {
    FILE_OP_OPEN,
    FILE_OP_SAVE,
    FILE_OP_DELETE,
    FILE_OP_COPY,
    FILE_OP_MOVE,
    FILE_OP_CREATE_DIR
} file_operation_t;

// File info structure
typedef struct {
    char name[256];
    uint64_t size;
    uint64_t attributes;
    bool is_directory;
    uint64_t created_time;
    uint64_t modified_time;
} file_info_t;

// File dialog types
typedef enum {
    FILE_DIALOG_OPEN,
    FILE_DIALOG_SAVE,
    FILE_DIALOG_SELECT_FOLDER
} file_dialog_type_t;

// File manager functions
void file_manager_init(void);
int file_dialog_show(file_dialog_type_t type, const char* title, char* path, size_t path_size);
bool file_exists(const char* path);
int file_get_info(const char* path, file_info_t* info);
int file_list_directory(const char* path, file_info_t* files, int max_files);
int file_create(const char* path, bool is_directory);
int file_delete(const char* path);
int file_copy(const char* src, const char* dst);
int file_move(const char* src, const char* dst);

// Recent files management
void file_add_recent(const char* path);
const char* file_get_recent(int index);
int file_get_recent_count(void);
void file_clear_recent(void);

#endif // FILE_MANAGER_H
