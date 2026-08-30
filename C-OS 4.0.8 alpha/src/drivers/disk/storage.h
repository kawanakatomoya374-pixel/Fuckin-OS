#ifndef STORAGE_H
#define STORAGE_H

#include "types.h"

// Image-backed persistent storage (ATA disk image only; no RAM fallback)
bool storage_init(void);
bool storage_format(void);
bool storage_write_file(const char* filename, const void* data, uint64_t size);
bool storage_read_file(const char* filename, void* buffer, uint64_t buffer_size, uint64_t* out_size);
bool storage_delete_file(const char* filename);
bool storage_file_exists(const char* filename);
uint64_t storage_list_files(char* filenames, uint64_t max_files, uint64_t max_name_len);
uint64_t storage_get_free_space(void);
uint64_t storage_get_used_space(void);
uint64_t storage_get_total_space(void);

// Password / settings storage used by the boot and settings UI
bool storage_has_password(void);
bool storage_verify_password(const char* password);
bool storage_set_password(const char* password);
bool storage_clear_password(void);

#endif
