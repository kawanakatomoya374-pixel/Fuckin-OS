/**
 * fs_api.c - File System API Layer Implementation
 * C-OS 4.0.8 alpha
 * 
 * This layer wraps the existing fs.c implementation to provide a clean
 * interface between GUI/Applications and the file system.
 */

#include "fs_api.h"
#include "../../fs/fs.h"
#include "../../include/memory.h"
#include "../../include/string.h"
#include <string.h>

static bool g_fs_api_initialized = false;

static void fs_api_split_path(const char* path, char* parent, size_t parent_size, char* name, size_t name_size) {
    if (!parent || !name || parent_size == 0 || name_size == 0) return;
    parent[0] = '\0';
    name[0] = '\0';
    if (!path || !path[0]) {
        strncpy(parent, "/", parent_size - 1);
        parent[parent_size - 1] = '\0';
        return;
    }

    const char* last_slash = strrchr(path, '/');
    if (!last_slash) {
        strncpy(parent, "/", parent_size - 1);
        parent[parent_size - 1] = '\0';
        strncpy(name, path, name_size - 1);
        name[name_size - 1] = '\0';
        return;
    }

    if (last_slash == path) {
        strncpy(parent, "/", parent_size - 1);
        parent[parent_size - 1] = '\0';
        strncpy(name, path + 1, name_size - 1);
        name[name_size - 1] = '\0';
        return;
    }

    size_t parent_len = (size_t)(last_slash - path);
    if (parent_len >= parent_size) parent_len = parent_size - 1;
    memcpy(parent, path, parent_len);
    parent[parent_len] = '\0';
    strncpy(name, last_slash + 1, name_size - 1);
    name[name_size - 1] = '\0';
}

void fs_api_init(void) {
    if (!g_fs_api_initialized) {
        fs_init();
        g_fs_api_initialized = true;
    }
}

bool fs_api_read_file(const char* path, void* buffer, uint64_t buffer_size, uint64_t* out_size) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path || !buffer || buffer_size == 0) return false;

    char parent[FS_MAX_PATH];
    char name[FS_MAX_NAME];
    fs_api_split_path(path, parent, sizeof(parent), name, sizeof(name));

    fs_entry_t* entries = fs_list_dir(parent);
    if (!entries) return false;

    int count = fs_entry_count_for_path(parent);
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0 && !entries[i].is_dir) {
            uint64_t copy_size = entries[i].size;
            if (copy_size > buffer_size) copy_size = buffer_size;
            memcpy(buffer, entries[i].data, copy_size);
            if (out_size) *out_size = copy_size;
            return true;
        }
    }

    return false;
}

bool fs_api_write_file(const char* path, const void* data, uint64_t size) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path || !data) return false;

    char parent[FS_MAX_PATH];
    char name[FS_MAX_NAME];
    fs_api_split_path(path, parent, sizeof(parent), name, sizeof(name));

    return fs_write_file_at(parent, name, (const char*)data, size);
}

bool fs_api_delete(const char* path) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path) return false;

    fs_file_info_t info;
    if (fs_api_get_file_info(path, &info)) {
        return info.is_directory ? fs_delete_dir(path) : fs_delete_file(path);
    }
    return fs_delete_file(path);
}

bool fs_api_exists(const char* path) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path) return false;

    fs_file_info_t info;
    return fs_api_get_file_info(path, &info);
}

bool fs_api_get_file_info(const char* path, fs_file_info_t* info) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path || !info) return false;

    char parent[FS_MAX_PATH];
    char name[FS_MAX_NAME];
    fs_api_split_path(path, parent, sizeof(parent), name, sizeof(name));

    fs_entry_t* entries = fs_list_dir(parent);
    if (!entries) return false;

    int count = fs_entry_count_for_path(parent);
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            strncpy(info->name, entries[i].name, sizeof(info->name) - 1);
            info->name[sizeof(info->name) - 1] = '\0';
            info->size = entries[i].size;
            info->is_directory = entries[i].is_dir;
            info->is_readonly = (entries[i].permissions != 0xFF);
            info->modification_time = entries[i].modified_time;
            return true;
        }
    }

    return false;
}

bool fs_api_list_directory(const char* path, fs_directory_t* dir) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path || !dir) return false;

    fs_entry_t* entries = fs_list_dir(path);
    if (!entries) return false;

    int count = fs_entry_count_for_path(path);
    if (count <= 0) {
        dir->files = NULL;
        dir->count = 0;
        dir->capacity = 0;
        return true;
    }

    dir->files = (fs_file_info_t*)kmalloc(sizeof(fs_file_info_t) * count);
    if (!dir->files) return false;

    dir->count = count;
    dir->capacity = count;

    for (int i = 0; i < count; i++) {
        strncpy(dir->files[i].name, entries[i].name, sizeof(dir->files[i].name) - 1);
        dir->files[i].name[sizeof(dir->files[i].name) - 1] = '\0';
        dir->files[i].size = entries[i].size;
        dir->files[i].is_directory = entries[i].is_dir;
        dir->files[i].is_readonly = (entries[i].permissions != 0xFF);
        dir->files[i].modification_time = entries[i].modified_time;
    }

    return true;
}

void fs_api_free_directory(fs_directory_t* dir) {
    if (dir && dir->files) {
        kfree(dir->files);
        dir->files = NULL;
        dir->count = 0;
        dir->capacity = 0;
    }
}

bool fs_api_create_directory(const char* path) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path) return false;

    char parent[FS_MAX_PATH];
    char name[FS_MAX_NAME];
    fs_api_split_path(path, parent, sizeof(parent), name, sizeof(name));

    return fs_create_dir_at(parent, name);
}

bool fs_api_copy_file(const char* src, const char* dst) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!src || !dst) return false;

    return fs_copy_file(src, dst);
}

bool fs_api_move(const char* src, const char* dst) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!src || !dst) return false;

    return fs_move_path(src, dst);
}

const char* fs_api_get_extension(const char* path) {
    if (!path) return NULL;
    
    const char* last_dot = strrchr(path, '.');
    if (!last_dot || last_dot == path) return NULL;
    
    // Check if there's a slash after the dot (directory with dot in name)
    const char* last_slash = strrchr(path, '/');
    if (last_slash && last_slash > last_dot) return NULL;
    
    return last_dot;
}

bool fs_api_is_directory(const char* path) {
    if (!g_fs_api_initialized) fs_api_init();
    if (!path) return false;
    
    fs_file_info_t info;
    if (!fs_api_get_file_info(path, &info)) return false;
    
    return info.is_directory;
}
