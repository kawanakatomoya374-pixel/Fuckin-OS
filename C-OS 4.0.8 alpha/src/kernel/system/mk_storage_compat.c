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

typedef struct mk_storage_handle {
    mk_storage_file_t file;
    int fd;
    bool in_use;
} mk_storage_handle_t;

static mk_storage_handle_t g_handles[MK_MAX_OPEN_FILES];

void mk_storage_init(void) {
    memset(g_handles, 0, sizeof(g_handles));
    serial_puts("[MKSTORAGE] fs_unified compatibility layer initialized\n");
}

mk_storage_file_t* mk_storage_open_file(const char* filename) {
    int fd = fs_unified_open(filename, FS_UNIFIED_O_RDONLY);
    if (fd < 0) return NULL;

    for (int i = 0; i < MK_MAX_OPEN_FILES; i++) {
        if (!g_handles[i].in_use) {
            g_handles[i].in_use = true;
            g_handles[i].fd = fd;
            g_handles[i].file.id = (uint64_t)fd;
            
            fs_unified_stat_t st;
            if (fs_unified_stat(filename, &st) == 0) {
                g_handles[i].file.size = st.size;
            }
            
            strncpy(g_handles[i].file.name, filename, sizeof(g_handles[i].file.name) - 1);
            return &g_handles[i].file;
        }
    }
    
    fs_unified_close(fd);
    return NULL;
}

int mk_storage_close_file(mk_storage_file_t* file) {
    if (!file) return -1;
    mk_storage_handle_t* h = (mk_storage_handle_t*)file;
    if (h->in_use) {
        fs_unified_close(h->fd);
        h->in_use = false;
        return 0;
    }
    return -1;
}

int mk_storage_read_file(mk_storage_file_t* file, void* buffer, uint64_t offset, uint64_t size) {
    if (!file || !buffer) return -1;
    mk_storage_handle_t* h = (mk_storage_handle_t*)file;

    fs_unified_seek(h->fd, (int64_t)offset, 0);
    return fs_unified_read(h->fd, buffer, (size_t)size);
}

int mk_storage_seek_file(mk_storage_file_t* file, uint64_t offset) {
    /* Seek within an open mk_storage file. Required by mk_mp3_backend for
     * stream rewinding and resume support. Forwarded to fs_unified_seek
     * using the in-kernel compatibility handle. */
    if (!file) return -1;
    mk_storage_handle_t* h = (mk_storage_handle_t*)file;
    if (!h->in_use) return -1;
    int64_t new_pos = fs_unified_seek(h->fd, (int64_t)offset, 0 /* SEEK_SET */);
    return (new_pos < 0) ? -1 : 0;
}

/* 他のスタブ関数は省略または最小限の実装 */
int mk_storage_write_file(mk_storage_file_t* file, const void* buffer, uint64_t offset, uint64_t size) { (void)file; (void)buffer; (void)offset; (void)size; return -1; }
int mk_storage_delete_file(mk_storage_file_t* file) { (void)file; return -1; }
mk_storage_state_t* mk_storage_get_state(void) { return NULL; }
uint64_t mk_storage_get_free_space(void) { return 0; }
uint64_t mk_storage_get_total_space(void) { return 0; }
void mk_storage_server_main(void) {}
