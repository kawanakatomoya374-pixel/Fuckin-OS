#ifndef STORAGE_H
#define STORAGE_H

#include "types.h"

// Image-backed persistent storage (ATA disk image only; no RAM fallback)
bool storage_init(void);
bool storage_format(void);
bool storage_write_file(const char* filename, const void* data, uint64_t size);

/* Deferred catalog persistence (see the design comment in storage.c above
 * storage_mark_catalog_dirty()). storage_write_file()/storage_delete_file()
 * mark the on-disk catalog dirty instead of committing synchronously;
 * these two functions are how that gets flushed.
 *
 * storage_flush_catalog_if_due(): poll once per GUI frame. Cheap when
 * clean (a single flag check); does the real ~120-170ms commit only once
 * the debounce window has elapsed, batching any writes that happened
 * during it into one flush.
 *
 * storage_flush_catalog_now(): unconditional, for callers that need
 * durability immediately (an explicit user Save action, or a clean
 * shutdown path) rather than waiting out the debounce window. */
bool storage_flush_catalog_if_due(void);
bool storage_flush_catalog_now(void);
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
