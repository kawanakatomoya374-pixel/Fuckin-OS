/**
 * fs.c - C-OS user-facing filesystem, backed by real FAT32 (FatFs)
 *
 * This replaces the previous design (a fixed FS_MAX_ENTRIES-slot array
 * with each file's content embedded inline as a FS_MAX_DATA-byte buffer,
 * persisted by re-serializing the *entire* filesystem into one blob on
 * every change) with real per-file operations against a FAT32 partition
 * on the ATA disk (see src/third_party/fatfs/diskio.c for where that
 * partition lives). Every fs_* entry point below keeps its original
 * signature so the ~40+ call sites across the GUI (file manager, text
 * editor, image viewer, mp3 player, settings, ...) don't need to change.
 *
 * fs_entry_t.data is no longer populated by fs_list_dir() - directory
 * listings are metadata only (name/size/is_dir/timestamps), matching
 * how every real filesystem's readdir works. Callers that need file
 * content call fs_read_file_at()/cos_fs_read_file() explicitly, which
 * now stream from disk with no 32KB cap.
 */
#include "fs.h"
#include "../third_party/fatfs/ff.h"
#include "memory.h"
#include "serial.h"
#include "sync.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>

extern const unsigned char desktop_featured_png[];
extern const unsigned int desktop_featured_png_len;
extern const unsigned char sample_icon_bmp[];
extern const unsigned int sample_icon_bmp_len;
extern const unsigned char sample_beep_mp3[];
extern const unsigned int sample_beep_mp3_len;
extern const unsigned char sample_photo_jpg[];
extern const unsigned int sample_photo_jpg_len;
extern const unsigned char sample_beep_wav[];
extern const unsigned int sample_beep_wav_len;

extern void fatfs_diskio_probe(void);

void* memset(void* s, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);
size_t strlen(const char* s);

static FATFS g_fatfs;
static bool g_fatfs_mounted = false;
static bool g_initialized = false;
static uint64_t g_revision = 0; /* bumped on any mutation, for fs_monitor_t change detection */
static mutex_t g_fs_mutex;
static bool g_fs_mutex_ready = false;

static void fs_lock(void) {
    if (!g_fs_mutex_ready) { mutex_init(&g_fs_mutex); g_fs_mutex_ready = true; }
    mutex_lock(&g_fs_mutex);
}
static void fs_unlock(void) {
    mutex_unlock(&g_fs_mutex);
}

/* ============================================================================
   Path helpers
   ========================================================================== */
static bool path_is_root(const char* p) {
    return !p || !p[0] || (p[0] == '/' && p[1] == '\0');
}

static void join_path(char* dst, size_t dstsz, const char* parent, const char* name) {
    if (!dst || dstsz == 0) return;
    if (!name) name = "";
    if (name[0] == '/') {
        strncpy(dst, name, dstsz - 1);
        dst[dstsz - 1] = '\0';
        return;
    }
    if (path_is_root(parent)) {
        dst[0] = '/';
        strncpy(dst + 1, name, dstsz - 2);
        dst[dstsz - 1] = '\0';
    } else {
        size_t plen = strlen(parent);
        if (plen >= dstsz) plen = dstsz - 1;
        memcpy(dst, parent, plen);
        dst[plen] = '\0';
        if (plen > 0 && dst[plen - 1] != '/' && plen + 1 < dstsz) {
            dst[plen++] = '/';
            dst[plen] = '\0';
        }
        strncat(dst, name, dstsz - strlen(dst) - 1);
    }
}

static uint8_t classify_file_type(const char* name, bool is_dir) {
    if (is_dir) return FS_FILE_TYPE_DIR;
    const char* dot = strrchr(name, '.');
    if (!dot) return FS_FILE_TYPE_TEXT;
    dot++;
    if (!strcmp(dot, "png") || !strcmp(dot, "bmp") || !strcmp(dot, "jpg") || !strcmp(dot, "jpeg")) return FS_FILE_TYPE_MEDIA;
    if (!strcmp(dot, "mp3") || !strcmp(dot, "wav") || !strcmp(dot, "ogg")) return FS_FILE_TYPE_AUDIO;
    if (!strcmp(dot, "txt") || !strcmp(dot, "md") || !strcmp(dot, "c") || !strcmp(dot, "h") ||
        !strcmp(dot, "json") || !strcmp(dot, "cfg") || !strcmp(dot, "log")) return FS_FILE_TYPE_TEXT;
    return FS_FILE_TYPE_BINARY;
}

static void fill_entry_from_filinfo(fs_entry_t* e, const char* parent_path, const FILINFO* fi) {
    memset(e, 0, sizeof(*e));
    strncpy(e->name, fi->fname, sizeof(e->name) - 1);
    strncpy(e->path, parent_path, sizeof(e->path) - 1);
    e->is_dir = (fi->fattrib & AM_DIR) != 0;
    e->size = e->is_dir ? 0 : (uint64_t)fi->fsize;
    e->is_hidden = (fi->fattrib & AM_HID) != 0 || fi->fname[0] == '.';
    e->is_system = (fi->fattrib & AM_SYS) != 0;
    e->permissions = (fi->fattrib & AM_RDO) ? 0555 : 0755;
    e->file_type = classify_file_type(fi->fname, e->is_dir);
    e->created_time = e->modified_time = e->accessed_time = g_revision;
}

/* ============================================================================
   Directory listing
   ========================================================================== */
static fs_entry_t g_dir_entries[FS_MAX_ENTRIES];
static int g_dir_entry_count = 0;

fs_entry_t* fs_list_dir(const char* path) {
    fs_lock();
    if (!path || !path[0]) path = "/";
    DIR dir;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) {
        g_dir_entry_count = 0;
        fs_unlock();
        return g_dir_entries;
    }

    int n = 0;
    FILINFO fi;
    while (n < FS_MAX_ENTRIES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        fill_entry_from_filinfo(&g_dir_entries[n], path, &fi);
        n++;
    }
    f_closedir(&dir);
    g_dir_entry_count = n;
    fs_unlock();
    return g_dir_entries;
}

int fs_entry_count(void) {
    return g_dir_entry_count;
}

int fs_entry_count_for_path(const char* path) {
    fs_lock();
    if (!path || !path[0]) path = "/";
    DIR dir;
    FRESULT fr = f_opendir(&dir, path);
    if (fr != FR_OK) { fs_unlock(); return 0; }
    int n = 0;
    FILINFO fi;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) n++;
    f_closedir(&dir);
    fs_unlock();
    return n;
}

fs_entry_t* fs_find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_dir_entry_count; ++i) {
        if (!strcmp(g_dir_entries[i].name, name)) return &g_dir_entries[i];
    }
    return NULL;
}

/* ============================================================================
   Create / write
   ========================================================================== */
bool fs_create_dir_at(const char* path, const char* name) {
    char full[FS_MAX_PATH];
    join_path(full, sizeof(full), path, name);
    fs_lock();
    FRESULT fr = f_mkdir(full);
    bool ok = (fr == FR_OK || fr == FR_EXIST);
    if (ok) g_revision++;
    fs_unlock();
    return ok;
}

bool fs_create_file_at(const char* path, const char* name) {
    char full[FS_MAX_PATH];
    join_path(full, sizeof(full), path, name);
    fs_lock();
    FIL fp;
    FRESULT fr = f_open(&fp, full, FA_CREATE_NEW | FA_WRITE);
    bool ok = (fr == FR_OK || fr == FR_EXIST);
    if (fr == FR_OK) f_close(&fp);
    if (ok) g_revision++;
    fs_unlock();
    return ok;
}

bool fs_write_file_at(const char* path, const char* name, const char* data, uint64_t size) {
    if (!name || !data) return false;
    char full[FS_MAX_PATH];
    join_path(full, sizeof(full), path, name);

    fs_lock();
    FIL fp;
    FRESULT fr = f_open(&fp, full, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        serial_puts("[FS] f_open write failed (FR=");
        serial_putdec((uint64_t)fr);
        serial_puts("): ");
        serial_puts(full);
        serial_puts("\n");
        fs_unlock();
        return false;
    }

    bool ok = true;
    UINT written = 0;
    if (size > 0) {
        fr = f_write(&fp, data, (UINT)size, &written);
        if (fr != FR_OK || written != (UINT)size) {
            serial_puts("[FS] f_write failed (FR=");
            serial_putdec((uint64_t)fr);
            serial_puts(", wrote=");
            serial_putdec((uint64_t)written);
            serial_puts(" of ");
            serial_putdec(size);
            serial_puts("): ");
            serial_puts(full);
            serial_puts("\n");
            ok = false;
        }
    }
    f_close(&fp);
    if (ok) g_revision++;
    fs_unlock();
    return ok;
}

/* ============================================================================
   Read
   ========================================================================== */
static char* g_read_buf = NULL;
static uint64_t g_read_buf_cap = 0;

const char* fs_read_file_at(const char* path, const char* name) {
    if (!name) return NULL;
    char full[FS_MAX_PATH];
    join_path(full, sizeof(full), path, name);

    fs_lock();
    FIL fp;
    FRESULT fr = f_open(&fp, full, FA_READ);
    if (fr != FR_OK) { fs_unlock(); return NULL; }

    uint64_t fsize = (uint64_t)f_size(&fp);
    if (fsize + 1 > g_read_buf_cap) {
        char* grown = (char*)kmalloc((size_t)(fsize + 1));
        if (!grown) { f_close(&fp); fs_unlock(); return NULL; }
        if (g_read_buf) kfree(g_read_buf);
        g_read_buf = grown;
        g_read_buf_cap = fsize + 1;
    }

    UINT got = 0;
    fr = f_read(&fp, g_read_buf, (UINT)fsize, &got);
    f_close(&fp);
    if (fr != FR_OK) { fs_unlock(); return NULL; }
    g_read_buf[got] = '\0';
    fs_unlock();
    return g_read_buf;
}

const char* fs_read_file(const char* name) {
    return fs_read_file_at("/", name);
}

int cos_fs_read_file(const char* path, void* buffer, uint64_t size) {
    if (!path || !buffer) return -1;
    fs_lock();
    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_READ);
    if (fr != FR_OK) { fs_unlock(); return -1; }
    UINT got = 0;
    fr = f_read(&fp, buffer, (UINT)size, &got);
    f_close(&fp);
    fs_unlock();
    if (fr != FR_OK) return -1;
    return (int)got;
}

int cos_fs_write_file(const char* path, const void* data, uint64_t size) {
    if (!path || !data) return -1;
    fs_lock();
    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) { fs_unlock(); return -1; }
    UINT written = 0;
    fr = f_write(&fp, data, (UINT)size, &written);
    f_close(&fp);
    if (fr == FR_OK) g_revision++;
    fs_unlock();
    if (fr != FR_OK || written != (UINT)size) return -1;
    return (int)written;
}

/* ============================================================================
   Delete / move / copy
   ========================================================================== */
bool fs_delete_file(const char* path) {
    if (!path) return false;
    fs_lock();
    FRESULT fr = f_unlink(path);
    bool ok = (fr == FR_OK);
    if (ok) g_revision++;
    fs_unlock();
    return ok;
}

bool fs_delete_dir(const char* name) {
    return fs_delete_file(name); /* f_unlink handles empty dirs too */
}

bool fs_delete(const char* name) {
    return fs_delete_file(name);
}

bool fs_move_path(const char* src_full_path, const char* dst_full_path) {
    if (!src_full_path || !dst_full_path) return false;
    fs_lock();
    f_unlink(dst_full_path); /* FatFs f_rename fails if destination exists */
    FRESULT fr = f_rename(src_full_path, dst_full_path);
    bool ok = (fr == FR_OK);
    if (ok) g_revision++;
    fs_unlock();
    return ok;
}

bool fs_copy_file(const char* src_full_path, const char* dst_full_path) {
    if (!src_full_path || !dst_full_path) return false;
    fs_lock();
    FIL src, dst;
    if (f_open(&src, src_full_path, FA_READ) != FR_OK) { fs_unlock(); return false; }
    if (f_open(&dst, dst_full_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        f_close(&src);
        fs_unlock();
        return false;
    }
    static uint8_t chunk[4096];
    bool ok = true;
    for (;;) {
        UINT got = 0;
        if (f_read(&src, chunk, sizeof(chunk), &got) != FR_OK) { ok = false; break; }
        if (got == 0) break;
        UINT put = 0;
        if (f_write(&dst, chunk, got, &put) != FR_OK || put != got) { ok = false; break; }
    }
    f_close(&src);
    f_close(&dst);
    if (ok) g_revision++;
    fs_unlock();
    return ok;
}

bool fs_copy_path(const char* src_full_path, const char* dst_full_path) {
    return fs_copy_file(src_full_path, dst_full_path);
}

/* ============================================================================
   Legacy root-relative wrappers
   ========================================================================== */
bool fs_create_file(const char* name) { return fs_create_file_at("/", name); }
bool fs_create_dir(const char* name) { return fs_create_dir_at("/", name); }
bool fs_write_file(const char* name, const char* data, uint64_t size) { return fs_write_file_at("/", name, data, size); }
int fs_rename(const char* old_name, const char* new_name) { return fs_move_path(old_name, new_name) ? 0 : -1; }

/* ============================================================================
   Misc / metadata helpers
   ========================================================================== */
bool fs_is_text_file(const fs_entry_t* entry) {
    if (!entry || entry->is_dir) return false;
    return entry->file_type == FS_FILE_TYPE_TEXT;
}

bool fs_looks_like_text(const char* data, uint64_t size) {
    if (!data || size == 0) return false;
    uint64_t sample = size < 512 ? size : 512;
    uint64_t printable = 0;
    for (uint64_t i = 0; i < sample; ++i) {
        uint8_t c = (uint8_t)data[i];
        if (c == '\t' || c == '\n' || c == '\r' || (c >= 0x20 && c < 0x7F)) printable++;
    }
    return printable * 100 >= sample * 90;
}

bool fs_is_desktop_file(const fs_entry_t* entry) {
    return entry && !strncmp(entry->path, "/desktop", 8);
}

void fs_update_metadata(fs_entry_t* entry) {
    if (!entry) return;
    entry->accessed_time = g_revision;
}

uint64_t fs_get_current_time(void) {
    return g_revision;
}

bool fs_is_disk_persist_healthy(void) {
    return g_fatfs_mounted;
}

/* ============================================================================
   Monitors (simplified: global revision counter, not per-directory)
   ========================================================================== */
bool fs_start_monitor(const char* path, fs_monitor_t* monitor) {
    if (!monitor) return false;
    memset(monitor, 0, sizeof(*monitor));
    if (path) strncpy(monitor->path, path, sizeof(monitor->path) - 1);
    monitor->active = true;
    monitor->last_check_time = g_revision;
    return true;
}

bool fs_stop_monitor(fs_monitor_t* monitor) {
    if (!monitor) return false;
    monitor->active = false;
    return true;
}

bool fs_check_changes(fs_monitor_t* monitor) {
    if (!monitor || !monitor->active) return false;
    bool changed = monitor->last_check_time != g_revision;
    monitor->last_check_time = g_revision;
    return changed;
}

bool fs_has_changes(const char* path) {
    (void)path;
    static uint64_t last_seen = 0;
    bool changed = last_seen != g_revision;
    last_seen = g_revision;
    return changed;
}

/* ============================================================================
   Bootstrap
   ========================================================================== */
static void fs_bootstrap_write_asset(const char* filename, const char* data, uint64_t size) {
    if (!fs_write_file_at("/", filename, data, size)) {
        serial_puts("[FS] Bootstrap asset write failed: ");
        serial_puts(filename);
        serial_puts("\n");
    }
}

static void fs_bootstrap_defaults(void) {
    f_mkdir("/home");
    f_mkdir("/etc");
    f_mkdir("/bin");
    f_mkdir("/tmp");
    f_mkdir("/desktop");

    static const char welcome_msg[] = "Welcome to C-OS 4.0.8 alpha!\n"
        "This filesystem is now real FAT32 (via FatFs) - files are no"
        " longer limited to 32KB.\n";
    fs_bootstrap_write_asset("/desktop/welcome.txt", welcome_msg, (uint64_t)strlen(welcome_msg));
    fs_bootstrap_write_asset("/desktop/featured.png", (const char*)desktop_featured_png, (uint64_t)desktop_featured_png_len);
    /* Media assets keep stable short aliases for interoperability.  LFN is
     * enabled separately, so add an explicit long-name fixture below. */
    fs_bootstrap_write_asset("/desktop/icon.bmp", (const char*)sample_icon_bmp, (uint64_t)sample_icon_bmp_len);
    fs_bootstrap_write_asset("/desktop/photo.jpg", (const char*)sample_photo_jpg, (uint64_t)sample_photo_jpg_len);
    fs_bootstrap_write_asset("/desktop/beep.mp3", (const char*)sample_beep_mp3, (uint64_t)sample_beep_mp3_len);
    fs_bootstrap_write_asset("/desktop/beep.wav", (const char*)sample_beep_wav, (uint64_t)sample_beep_wav_len);

    static const char lfn_msg[] = "FatFS long file name support is enabled.\n";
    static const char jp_lfn_msg[] = "UTF-8 Japanese file name support is enabled.\n";
    fs_bootstrap_write_asset("/desktop/C-OS 4.0.8 Long File Name Verification.txt",
                             lfn_msg, (uint64_t)strlen(lfn_msg));
    fs_bootstrap_write_asset("/desktop/日本語ファイル名の確認.txt",
                             jp_lfn_msg, (uint64_t)strlen(jp_lfn_msg));
}

void fs_init(void) {
    if (g_initialized) return;
    g_initialized = true;

    fatfs_diskio_probe();

    FRESULT fr = f_mount(&g_fatfs, "", 1);
    if (fr == FR_NO_FILESYSTEM) {
        serial_puts("[FS] No FAT32 filesystem found on partition; formatting...\n");
        static uint8_t work[FF_MAX_SS];
        MKFS_PARM opt;
        memset(&opt, 0, sizeof(opt));
        opt.fmt = FM_FAT32;
        fr = f_mkfs("", &opt, work, sizeof(work));
        if (fr != FR_OK) {
            serial_puts("[FS] ERROR: f_mkfs failed, filesystem unavailable\n");
            g_fatfs_mounted = false;
            return;
        }
        fr = f_mount(&g_fatfs, "", 1);
    }

    if (fr != FR_OK) {
        serial_puts("[FS] ERROR: failed to mount FAT32 partition\n");
        g_fatfs_mounted = false;
        return;
    }

    g_fatfs_mounted = true;
    serial_puts("[FS] FAT32 filesystem mounted\n");

    DIR dir;
    bool has_entries = false;
    if (f_opendir(&dir, "/") == FR_OK) {
        FILINFO fi;
        if (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) has_entries = true;
        f_closedir(&dir);
    }
    if (!has_entries) {
        serial_puts("[FS] Empty filesystem; creating default desktop layout\n");
        fs_bootstrap_defaults();
    } else {
        /* Preserve user data, but install the non-destructive LFN fixture so
         * an upgraded persistent volume exercises the new code path too. */
        static const char lfn_msg[] = "FatFS long file name support is enabled.\n";
        static const char jp_lfn_msg[] = "UTF-8 Japanese file name support is enabled.\n";
        serial_puts("[FS] Existing files found; preserving data and adding LFN fixture\n");
        fs_bootstrap_write_asset("/desktop/C-OS 4.0.8 Long File Name Verification.txt",
                                 lfn_msg, (uint64_t)strlen(lfn_msg));
        fs_bootstrap_write_asset("/desktop/日本語ファイル名の確認.txt",
                                 jp_lfn_msg, (uint64_t)strlen(jp_lfn_msg));
    }
}
