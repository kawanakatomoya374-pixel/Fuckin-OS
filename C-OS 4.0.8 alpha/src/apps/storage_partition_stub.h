#ifndef STORAGE_PARTITION_STUB_H
#define STORAGE_PARTITION_STUB_H

#include "types.h"

typedef struct {
    char label[32];
    uint64_t size;
    uint64_t used;
} storage_template_info_t;

int storage_template_init(const char* name, uint64_t base_sector, uint64_t sector_count);
int storage_template_read(uint64_t sector, void* buffer);
int storage_template_write(uint64_t sector, const void* buffer);
int storage_template_format(uint64_t base_sector, uint64_t sector_count);
int storage_template_mount(void);
int storage_template_unmount(void);
storage_template_info_t* storage_template_get_info(void);
int storage_template_create_file(const char* path, const char* name);
int storage_template_delete_file(const char* path);
int storage_template_read_file(const char* path, void* buf, size_t len);
int storage_template_write_file(const char* path, void* buf, size_t len);
int storage_template_create_dir(const char* path);
int storage_template_delete_dir(const char* path);
void storage_template_list_contents(void);
int storage_template_check_integrity(void);
int storage_template_defragment(void);
int storage_template_backup(void);
int storage_template_restore(void);

#endif
