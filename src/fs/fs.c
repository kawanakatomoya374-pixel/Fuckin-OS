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
/* Test.c-os: a real, complete .c-os GUI application (text-to-binary
 * converter) shipped as a default file rather than only ever existing
 * for validation. Ships through the SAME asset pipeline as every other
 * default /desktop file (src/assets/*.c, generated once from the built
 * binary and compiled in normally - see src/assets/test_c_os.c) so a
 * user booting their own built ISO finds it in the file manager and can
 * double-click it themselves, exactly like any file they created - not
 * through a validation-only hook that also auto-launched it on every
 * boot, which is what an earlier version of this integration did. */
extern const unsigned char cos_asset_test_c_os[];
extern const unsigned int cos_asset_test_c_os_len;
extern const unsigned int sample_icon_bmp_len;
extern const unsigned char sample_beep_mp3[];
extern const unsigned int sample_beep_mp3_len;
extern const unsigned char sample_photo_jpg[];
extern const unsigned int sample_photo_jpg_len;
extern const unsigned char sample_beep_wav[];
extern const unsigned int sample_beep_wav_len;
extern const unsigned char sample_test_webp[];
extern const unsigned int sample_test_webp_len;

#ifndef COS_BROWSER_FILE_SMOKE
#define COS_BROWSER_FILE_SMOKE 0
#endif

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

/* Called once from fs_init(), which runs early and single-threaded during
 * boot, before any AP or ring3 thread could possibly call fs_lock(). This
 * replaces a lazy check-then-act init ("if (!ready) { init(); ready =
 * true; }") that was a genuine SMP race: that check and that write are
 * two separate, non-atomic steps, so two CPUs calling fs_lock() for the
 * very first time at the same moment could both see "not ready" and both
 * call mutex_init() concurrently - corrupting the mutex's internal state,
 * or leaving both callers believing they hold exclusive access
 * simultaneously, which defeats the entire point of the lock.
 *
 * This was latent rather than new - fs_lock() has always looked like
 * this - but it went from theoretical to actually reachable once ring3
 * programs could call file syscalls (SYS_READ_FILE/SYS_WRITE_FILE) from
 * threads that can genuinely run on different cores, where before, every
 * filesystem call came from the single GUI owner thread and the race
 * could never trigger. */
static void fs_lock_subsystem_init(void) {
    if (!g_fs_mutex_ready) {
        mutex_init(&g_fs_mutex);
        g_fs_mutex_ready = true;
    }
}

static void fs_lock(void) {
    /* Defensive fallback only, not the real fix: if something ever calls
     * fs_lock() before fs_init() has run, this keeps behaviour identical
     * to before rather than dereferencing an uninitialised mutex. The
     * actual race is closed by fs_lock_subsystem_init() running eagerly,
     * single-threaded, from fs_init() below. */
    if (!g_fs_mutex_ready) { mutex_init(&g_fs_mutex); g_fs_mutex_ready = true; }
    mutex_lock(&g_fs_mutex);
}
static void fs_unlock(void) {
    mutex_unlock(&g_fs_mutex);
}

/**
 * fs_lock()/fs_unlock() (above) are the ONLY synchronization around every
 * FatFs call anywhere in this tree - FatFs is not reentrant, and a
 * second, independent mutex guarding some OTHER caller's FatFs calls
 * would not actually protect anything: it would just let that caller
 * race with everyone using fs_lock() directly, on the exact same global
 * filesystem state. cos_fd.c (the per-process file descriptor layer)
 * needs to hold this SAME lock around its own f_open()/f_read()/
 * f_write()/f_opendir()/f_readdir()/f_close() calls, so it is exposed
 * here under names that say plainly that they are for exactly that - not
 * for casual use elsewhere.
 */
void fs_lock_for_external_use(void) { fs_lock(); }
void fs_unlock_for_external_use(void) { fs_unlock(); }

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

/* FAT date/time -> Unix seconds. 0 if the entry has no date. */
static uint64_t fat_to_unix(WORD fdate, WORD ftime) {
    if (fdate == 0) return 0;
    int year = 1980 + ((fdate >> 9) & 0x7F), mon = (fdate >> 5) & 0x0F, day = fdate & 0x1F;
    int hour = (ftime >> 11) & 0x1F, min = (ftime >> 5) & 0x3F, sec = (ftime & 0x1F) * 2;
    if (mon < 1 || mon > 12 || day < 1) return 0;
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    uint64_t days = 0;
    for (int y = 1970; y < year; ++y) days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    days += (uint64_t)cum[mon - 1] + (uint64_t)(day - 1);
    if (mon > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days += 1;
    return days * 86400ull + (uint64_t)hour * 3600ull + (uint64_t)min * 60ull + (uint64_t)sec;
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
    /* Real timestamps from the directory entry (FAT: date = year-1980:7 |
     * month:4 | day:5, time = hour:5 | min:6 | sec/2:5). This used to be
     * g_revision - a change counter, not a time - so every file showed as
     * 1970-01-01 00:00. Converted to Unix seconds (local time == UTC). */
    e->created_time = e->modified_time = e->accessed_time = fat_to_unix(fi->fdate, fi->ftime);
    e->attributes = fi->fattrib;
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

/* Size / directory flag for an absolute path, straight from FatFs -
 * independent of g_dir_entries (which only ever holds the directory that
 * was listed last, e.g. by the file manager). */
bool fs_stat_path(const char* path, uint64_t* out_size, bool* out_is_dir) {
    if (!path || !path[0] || !g_fatfs_mounted) return false;
    FILINFO fi;
    fs_lock();
    FRESULT fr = f_stat(path, &fi);
    fs_unlock();
    if (fr != FR_OK) return false;
    if (out_size) *out_size = (uint64_t)fi.fsize;
    if (out_is_dir) *out_is_dir = (fi.fattrib & AM_DIR) != 0;
    return true;
}

/* Volume size and free space in bytes (FatFs f_getfree). */
bool fs_get_space(uint64_t* total_bytes, uint64_t* free_bytes) {
    if (!g_fatfs_mounted) return false;
    fs_lock();
    DWORD free_clst = 0;
    FATFS* fsp = NULL;
    FRESULT fr = f_getfree("", &free_clst, &fsp);
    fs_unlock();
    if (fr != FR_OK || !fsp) return false;
    uint64_t csz = (uint64_t)fsp->csize * 512u;
    if (total_bytes) *total_bytes = (uint64_t)(fsp->n_fatent - 2) * csz;
    if (free_bytes) *free_bytes = (uint64_t)free_clst * csz;
    return true;
}

/* ---- read-only streaming handles ------------------------------------
 * Positioned reads of arbitrarily large files without loading them into
 * memory - for media (a WAV is ~10 MB per minute) and anything else that
 * reads sequentially. Each handle owns a FatFs FIL; every call takes the
 * filesystem lock only for its own duration. */
struct fs_stream { bool used; FIL fp; uint64_t size; };
#define FS_STREAM_MAX 8
static struct fs_stream g_streams[FS_STREAM_MAX];

fs_stream_t* fs_stream_open(const char* path) {
    if (!path || !g_fatfs_mounted) return NULL;
    fs_lock();
    struct fs_stream* st = NULL;
    for (int i = 0; i < FS_STREAM_MAX; ++i) if (!g_streams[i].used) { st = &g_streams[i]; break; }
    if (!st || f_open(&st->fp, path, FA_READ) != FR_OK) { fs_unlock(); return NULL; }
    st->used = true;
    st->size = (uint64_t)f_size(&st->fp);
    fs_unlock();
    return st;
}
int64_t fs_stream_read_at(fs_stream_t* st, uint64_t offset, void* buf, uint64_t n) {
    if (!st || !st->used || !buf) return -1;
    if (offset >= st->size || n == 0) return 0;
    if (n > st->size - offset) n = st->size - offset;
    fs_lock();
    UINT got = 0;
    FRESULT fr = f_lseek(&st->fp, (FSIZE_t)offset);
    if (fr == FR_OK) fr = f_read(&st->fp, buf, (UINT)n, &got);
    fs_unlock();
    return fr == FR_OK ? (int64_t)got : -1;
}
uint64_t fs_stream_size(fs_stream_t* st) { return (st && st->used) ? st->size : 0; }
void fs_stream_close(fs_stream_t* st) {
    if (!st || !st->used) return;
    fs_lock();
    f_close(&st->fp);
    st->used = false;
    fs_unlock();
}

/* Reads up to buf_size bytes of the file at `path` straight from FatFs
 * into the caller's buffer (no shared buffer, no directory cache). */
bool fs_read_path_into(const char* path, void* buf, uint64_t buf_size, uint64_t* out_read) {
    if (out_read) *out_read = 0;
    if (!path || !buf || !g_fatfs_mounted) return false;
    fs_lock();
    FIL fp;
    if (f_open(&fp, path, FA_READ) != FR_OK) { fs_unlock(); return false; }
    uint64_t total = 0;
    bool ok = true;
    while (total < buf_size) {
        UINT chunk = (UINT)((buf_size - total) > 65536u ? 65536u : (buf_size - total));
        UINT got = 0;
        if (f_read(&fp, (uint8_t*)buf + total, chunk, &got) != FR_OK) { ok = false; break; }
        total += got;
        if (got < chunk) break;   /* end of file */
    }
    f_close(&fp);
    fs_unlock();
    if (out_read) *out_read = total;
    return ok;
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

/* Validation-only: seed the browser self-test document so BROWSER_FILE_SMOKE
 * builds can load it through the ordinary file:// -> NetSurf -> QuickJS
 * pipeline. Production builds compile this to an empty function and write
 * nothing, so the shipped filesystem layout is unchanged. */
#if COS_BROWSER_FILE_SMOKE
#include "browser_smoke_fixture.h"
#endif
static void fs_bootstrap_browser_fixture(void) {
#if COS_BROWSER_FILE_SMOKE
    f_mkdir("/browser");
    fs_bootstrap_write_asset("/browser/index.html", browser_smoke_fixture_html,
                             (uint64_t)browser_smoke_fixture_html_len);
    serial_puts("[FS] validation: seeded /browser/index.html self-test\n");
#endif
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
#ifndef COS_VALIDATION_OPEN_IMAGE_VIEWER
#define COS_VALIDATION_OPEN_IMAGE_VIEWER 0
#endif
#if COS_VALIDATION_OPEN_IMAGE_VIEWER
    /* Validation-only WebP fixture (see src/third_party/ffmpeg_webp) -
     * not part of the standard desktop, so it is not written on a
     * normal build. */
    fs_bootstrap_write_asset("/desktop/test.webp", (const char*)sample_test_webp, (uint64_t)sample_test_webp_len);
#endif
    fs_bootstrap_write_asset("/Test.c-os", (const char*)cos_asset_test_c_os,
                             (uint64_t)cos_asset_test_c_os_len);

    static const char lfn_msg[] = "FatFS long file name support is enabled.\n";
    static const char jp_lfn_msg[] = "UTF-8 Japanese file name support is enabled.\n";
    fs_bootstrap_write_asset("/desktop/C-OS 4.0.8 Long File Name Verification.txt",
                             lfn_msg, (uint64_t)strlen(lfn_msg));
    fs_bootstrap_write_asset("/desktop/日本語ファイル名の確認.txt",
                             jp_lfn_msg, (uint64_t)strlen(jp_lfn_msg));
    fs_bootstrap_browser_fixture();
}

void fs_init(void) {
    if (g_initialized) return;
    g_initialized = true;

    /* Single-threaded at this point in boot: safe to initialize the
     * mutex here rather than racing to do it lazily on first use. */
    fs_lock_subsystem_init();

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
        { extern void cos_system_layout_ensure(void); cos_system_layout_ensure(); }
    } else {
        /* Preserve user data, but install the non-destructive LFN fixture so
         * an upgraded persistent volume exercises the new code path too. */
        static const char lfn_msg[] = "FatFS long file name support is enabled.\n";
        static const char jp_lfn_msg[] = "UTF-8 Japanese file name support is enabled.\n";
        serial_puts("[FS] Existing files found; preserving data and adding LFN fixture\n");
        { extern void cos_system_layout_ensure(void); cos_system_layout_ensure(); }
        /* Default programs that a volume created by an older build may
         * lack: install only if absent, never overwrite. */
        {
            FILINFO probe;
            /* The desktop folder itself: if a previous first boot failed
             * part-way, a volume can have files but no /desktop, and every
             * later write there (downloads, saved files, fixtures) fails
             * with FR_NO_PATH. */
            if (f_stat("/desktop", &probe) != FR_OK) {
                serial_puts("[FS] /desktop missing on existing volume - recreating\n");
                (void)f_mkdir("/desktop");
            }
            if (f_stat("/Test.c-os", &probe) != FR_OK) {
                serial_puts("[FS] /Test.c-os missing on existing volume - installing default copy\n");
                fs_bootstrap_write_asset("/Test.c-os", (const char*)cos_asset_test_c_os,
                                         (uint64_t)cos_asset_test_c_os_len);
            }
        }
        fs_bootstrap_write_asset("/desktop/C-OS 4.0.8 Long File Name Verification.txt",
                                 lfn_msg, (uint64_t)strlen(lfn_msg));
        fs_bootstrap_write_asset("/desktop/日本語ファイル名の確認.txt",
                                 jp_lfn_msg, (uint64_t)strlen(jp_lfn_msg));
        fs_bootstrap_browser_fixture();
    }
}
