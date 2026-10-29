/**
 * file_manager.h - File Management System for GUI
 * C-OS 4.1.0
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

// Recent files management

#endif // FILE_MANAGER_H
