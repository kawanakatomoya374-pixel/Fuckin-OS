#ifndef MK_ADVANCED_FILEMANAGER_H
#define MK_ADVANCED_FILEMANAGER_H

#include <stdbool.h>
#include <stdint.h>

void mk_advanced_filemanager_init(void);
uint64_t mk_filemanager_copy_files(const char* source_path, const char* destination_path, bool background, bool recursive);
uint64_t mk_filemanager_move_files(const char* source_path, const char* destination_path, bool background, bool recursive);
uint64_t mk_filemanager_delete_files(const char* path, bool background, bool recursive, bool permanent);
uint64_t mk_filemanager_search_files(const char* pattern, const char* search_path, uint64_t search_type, bool case_sensitive, bool recursive);
uint64_t mk_filemanager_create_bookmark(const char* name, const char* path, const char* description, uint64_t bookmark_type);
uint64_t mk_filemanager_create_tag(const char* name, const char* color, const char* description);
uint64_t mk_filemanager_create_view(const char* name, const char* path, uint64_t view_type, uint64_t sort_type, uint64_t sort_order);
void mk_filemanager_list_operations(void);
void mk_filemanager_list_search_results(uint64_t search_id);
void mk_filemanager_list_bookmarks(void);
void mk_filemanager_monitor_operations(void);
void mk_filemanager_optimize_performance(void);

#endif
