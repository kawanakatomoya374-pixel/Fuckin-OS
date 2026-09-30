/**
 * mk_storage_compat.c - freestanding compatibility layer for MP3 backend
 * C-OS 5.0.0 - fs_unified API 移行版
 */

#include "types.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "mk_storage.h"
#include "../../fs/fs_unified.h"
#include "../../fs/fs.h"

/* Streaming handles over fs_stream_* (positioned FatFs reads). This layer
 * used to go through fs_unified, which loads a whole file into memory
 * (capped at 8 MiB, so ordinary MP3s and nearly every WAV were refused)
 * and, via fs_api_read_file, returned garbage instead of file data. */
typedef struct mk_storage_handle {
    mk_storage_file_t file;
    fs_stream_t* st;
    uint64_t pos;
    bool in_use;
} mk_storage_handle_t;

static mk_storage_handle_t g_handles[MK_MAX_OPEN_FILES];

void mk_storage_init(void) {
    memset(g_handles, 0, sizeof(g_handles));
    serial_puts("[MKSTORAGE] fs_unified compatibility layer initialized\n");
}

mk_storage_file_t* mk_storage_open_file(const char* filename) {
    if (!filename) return NULL;
    for (int i = 0; i < MK_MAX_OPEN_FILES; i++) {
        if (!g_handles[i].in_use) {
            fs_stream_t* st = fs_stream_open(filename);
            if (!st) return NULL;
            memset(&g_handles[i], 0, sizeof(g_handles[i]));
            g_handles[i].in_use = true;
            g_handles[i].st = st;
            g_handles[i].pos = 0;
            g_handles[i].file.id = (uint64_t)i;
            g_handles[i].file.size = fs_stream_size(st);
            strncpy(g_handles[i].file.name, filename, sizeof(g_handles[i].file.name) - 1);
            return &g_handles[i].file;
        }
    }
    return NULL;
}

int mk_storage_close_file(mk_storage_file_t* file) {
    if (!file) return -1;
    mk_storage_handle_t* h = (mk_storage_handle_t*)file;
    if (!h->in_use) return -1;
    fs_stream_close(h->st);
    h->in_use = false;
    return 0;
}

int mk_storage_read_file(mk_storage_file_t* file, void* buffer, uint64_t offset, uint64_t size) {
    if (!file || !buffer) return -1;
    mk_storage_handle_t* h = (mk_storage_handle_t*)file;
    if (!h->in_use) return -1;
    int64_t got = fs_stream_read_at(h->st, offset, buffer, size);
    if (got > 0) h->pos = offset + (uint64_t)got;
    return (int)got;
}

int mk_storage_seek_file(mk_storage_file_t* file, uint64_t offset) {
    if (!file) return -1;
    mk_storage_handle_t* h = (mk_storage_handle_t*)file;
    if (!h->in_use) return -1;
    h->pos = offset;
    return 0;
}

/* 他のスタブ関数は省略または最小限の実装 */
int mk_storage_write_file(mk_storage_file_t* file, const void* buffer, uint64_t offset, uint64_t size) { (void)file; (void)buffer; (void)offset; (void)size; return -1; }
int mk_storage_delete_file(mk_storage_file_t* file) { (void)file; return -1; }
mk_storage_state_t* mk_storage_get_state(void) { return NULL; }
uint64_t mk_storage_get_free_space(void) { return 0; }
uint64_t mk_storage_get_total_space(void) { return 0; }
void mk_storage_server_main(void) {}
