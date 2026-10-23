/**
 * fs_api.h - File System API Layer
 * C-OS 4.0.8 alpha
 * 
 * This layer provides a clean interface between GUI/Applications and the
 * underlying file system implementation (FAT32, ext2, etc.).
 * This abstracts away low-level details and allows for easier testing and
 * future file system changes.
 */

#ifndef FS_API_H
#define FS_API_H

#include <stdint.h>
#include <stdbool.h>
#include "../../fs/fs.h"


/* File operations */
typedef struct {
    char name[FS_MAX_NAME];
    uint64_t size;
    bool is_directory;
    bool is_readonly;
    uint64_t modification_time;
} fs_file_info_t;

/* Directory listing */
typedef struct {
    fs_file_info_t* files;
    int count;
    int capacity;
} fs_directory_t;

/**
 * Initialize the file system API layer
 */
void fs_api_init(void);

/**
 * Read a file's contents into a buffer
 * @param path Full path to the file
 * @param buffer Output buffer
 * @param buffer_size Size of the buffer
 * @param out_size Actual bytes read (can be NULL)
 * @return true on success, false on failure
 */
bool fs_api_read_file(const char* path, void* buffer, uint64_t buffer_size, uint64_t* out_size);

/**
 * Write data to a file
 * @param path Full path to the file
 * @param data Data to write
 * @param size Number of bytes to write
 * @return true on success, false on failure
 */
bool fs_api_write_file(const char* path, const void* data, uint64_t size);

/**
 * Delete a file or directory
 * @param path Full path to delete
 * @return true on success, false on failure
 */
bool fs_api_delete(const char* path);

/**
 * Check if a file or directory exists
 * @param path Full path to check
 * @return true if exists, false otherwise
 */
bool fs_api_exists(const char* path);

/**
 * Get information about a file
 * @param path Full path to the file
 * @param info Output structure for file information
 * @return true on success, false on failure
 */
bool fs_api_get_file_info(const char* path, fs_file_info_t* info);

/**
 * List directory contents
 * @param path Directory path
 * @param dir Output directory structure (caller must free with fs_api_free_directory)
 * @return true on success, false on failure
 */
bool fs_api_list_directory(const char* path, fs_directory_t* dir);

/**
 * Free directory listing resources
 * @param dir Directory structure to free
 */
void fs_api_free_directory(fs_directory_t* dir);

/**
 * Create a directory
 * @param path Full path of the directory to create
 * @return true on success, false on failure
 */
bool fs_api_create_directory(const char* path);

/**
 * Copy a file
 * @param src Source path
 * @param dst Destination path
 * @return true on success, false on failure
 */
bool fs_api_copy_file(const char* src, const char* dst);

/**
 * Move/rename a file or directory
 * @param src Source path
 * @param dst Destination path
 * @return true on success, false on failure
 */
bool fs_api_move(const char* src, const char* dst);

/**
 * Get file extension
 * @param path File path
 * @return Pointer to extension (including dot), or NULL if no extension
 */
const char* fs_api_get_extension(const char* path);

/**
 * Check if a path is a directory
 * @param path Path to check
 * @return true if directory, false otherwise
 */
bool fs_api_is_directory(const char* path);

#endif /* FS_API_H */
