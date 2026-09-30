/* cos_fm_support.c - see cos_fm_support.h. */
#include "cos_fm_support.h"
#include "serial.h"
#include "string.h"
#include "../third_party/fatfs/ff.h"

extern void fs_lock_for_external_use(void);
extern void fs_unlock_for_external_use(void);
extern bool fs_get_space(uint64_t *total, uint64_t *free_b);
extern void gui_open_file_in_app(const char *path, int file_type);
extern int  gui_dark_mode;
extern bool gui_is_japanese(void);

/* FAT date/time -> Unix seconds (0 when the entry has no date) */
static uint64_t fat_time(WORD fdate, WORD ftime) {
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
static uint8_t attr_of(BYTE fa) {
    uint8_t a = 0;
    if (fa & AM_RDO) a |= COS_ATTR_RDONLY;
    if (fa & AM_HID) a |= COS_ATTR_HIDDEN;
    if (fa & AM_SYS) a |= COS_ATTR_SYSTEM;
    if (fa & AM_ARC) a |= COS_ATTR_ARCHIVE;
    return a;
}
static void fill(cos_dirent_ex_t *e, const FILINFO *fi) {
    memset(e, 0, sizeof(*e));
    size_t i = 0;
    for (; fi->fname[i] && i < sizeof(e->name) - 1; ++i) e->name[i] = fi->fname[i];
    e->name[i] = 0;
    e->is_dir = (fi->fattrib & AM_DIR) ? 1 : 0;
    e->size = e->is_dir ? 0 : (uint64_t)fi->fsize;
    e->mtime = fat_time(fi->fdate, fi->ftime);
    e->attr = attr_of(fi->fattrib);
}

bool cos_fm_stat_ex(const char *path, cos_stat_ex_t *out) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
    if (path[0] == '/' && path[1] == 0) { out->is_dir = 1; return true; }   /* the root has no FILINFO */
    FILINFO fi;
    fs_lock_for_external_use();
    FRESULT fr = f_stat(path, &fi);
    fs_unlock_for_external_use();
    if (fr != FR_OK) return false;
    cos_dirent_ex_t e; fill(&e, &fi);
    out->size = e.size; out->mtime = e.mtime; out->is_dir = e.is_dir; out->attr = e.attr;
    return true;
}

int cos_fm_listdir_ex(const char *path, cos_dirent_ex_t *out, int max) {
    if (!path || !out || max <= 0) return -1;
    if (max > COS_LISTDIR_MAX) max = COS_LISTDIR_MAX;
    DIR dir;
    FILINFO fi;
    int n = 0;
    fs_lock_for_external_use();
    FRESULT fr = f_opendir(&dir, path);
    if (fr == FR_OK) {
        while (n < max) {
            fr = f_readdir(&dir, &fi);
            if (fr != FR_OK || fi.fname[0] == 0) break;
            fill(&out[n++], &fi);
        }
        f_closedir(&dir);
    }
    fs_unlock_for_external_use();
    return fr == FR_OK || n > 0 ? n : -1;
}

bool cos_fm_space(uint64_t out[2]) {
    uint64_t t = 0, f = 0;
    if (!fs_get_space(&t, &f)) return false;
    out[0] = t; out[1] = f;
    return true;
}

/* ---- "open in default app": ring 3 queues, the GUI thread executes ---- */
#define QN 8
static char s_q[QN][256];
static volatile int s_qh, s_qt, s_qlock;
static void qlock(void) { while (__sync_lock_test_and_set(&s_qlock, 1)) { } }
static void qunlock(void) { __sync_lock_release(&s_qlock); }

bool cos_fm_queue_open(const char *path) {
    if (!path || !path[0]) return false;
    bool ok = false;
    qlock();
    int next = (s_qt + 1) % QN;
    if (next != s_qh) {
        size_t i = 0;
        for (; path[i] && i < 255; ++i) s_q[s_qt][i] = path[i];
        s_q[s_qt][i] = 0;
        s_qt = next; ok = true;
    }
    qunlock();
    return ok;
}
void cos_fm_pump(void) {
    for (int guard = 0; guard < 4; ++guard) {
        char p[256];
        bool have = false;
        qlock();
        if (s_qh != s_qt) { memcpy(p, s_q[s_qh], sizeof p); s_qh = (s_qh + 1) % QN; have = true; }
        qunlock();
        if (!have) return;
        cos_stat_ex_t st;
        if (cos_fm_stat_ex(p, &st) && st.is_dir) cos_files_open(p);      /* a folder opens in the file manager */
        else gui_open_file_in_app(p, 0);
    }
}

uint32_t cos_fm_ui_prefs(void) {
    return (gui_dark_mode ? COS_UI_DARK : 0u) | (gui_is_japanese() ? COS_UI_JAPANESE : 0u);
}

/* ---- one clipboard for the whole system ---- */
static uint8_t s_clip[COS_CLIP_MAX];
static volatile uint64_t s_clip_len;
static volatile int s_clip_lock;
int64_t cos_fm_clip_set(const void *data, uint64_t len) {
    if (len > COS_CLIP_MAX) return -1;
    while (__sync_lock_test_and_set(&s_clip_lock, 1)) { }
    if (len) memcpy(s_clip, data, (size_t)len);
    s_clip_len = len;
    __sync_lock_release(&s_clip_lock);
    return (int64_t)len;
}
int64_t cos_fm_clip_get(void *out, uint64_t cap) {
    while (__sync_lock_test_and_set(&s_clip_lock, 1)) { }
    uint64_t n = s_clip_len < cap ? s_clip_len : cap;
    if (out && n) memcpy(out, s_clip, (size_t)n);
    uint64_t total = s_clip_len;
    __sync_lock_release(&s_clip_lock);
    return out ? (int64_t)n : (int64_t)total;       /* out == NULL: just the length */
}
