/**
 * gui_apps_common.c - C-OS 4.1.0 GUI Apps (分割ファイル)
 * 複数アプリから共有される汎用ユーティリティ関数
 *
 * 元は単一の gui_apps.c (11,638行) に含まれていたコードを、
 * 保守性向上のため機能単位に分割したものの一部。
 */
#include "gui.h"
#include "mk_desktop.h"
#include "system/password_screen.h"
#include "vga.h"
#include "mk_mp3.h"
#include "../../../apps/jpeg_viewer.h"
#include "string.h"
#include "serial.h"
#include "fs.h"
#include "memory.h"
#include "../drivers/disk/storage.h"

#ifndef KEY_PAGEUP
#define KEY_PAGEUP   0x49
#endif
#ifndef KEY_PAGEDOWN
#define KEY_PAGEDOWN 0x51
#endif
#ifndef KEY_HOME
#define KEY_HOME     0x47
#endif
#ifndef KEY_END
#define KEY_END      0x4F
#endif
#include "memory.h"
#include "memory_physical.h"
#include "cos_version.h"
#include "rtc.h"
#include "scheduler.h"
#include "../../bios/bios.h"
#include "../../kernel/drivers/usb.h"
#include "../../kernel/drivers/pci.h"
#include "fs.h"
#include "keyboard.h"
#include "../../drivers/disk/storage.h"
#include "../../drivers/input/mouse_minimal.h"
#include <shell.h>
extern const char* fs_read_file_at(const char* path, const char* name);
extern const char* config_get_string(const char* key);
extern void gui_snapshot_save_desktop(void);
extern bool settings_set_desktop_icon_size(uint32_t size) __attribute__((weak));
extern void gui_normalize_desktop_icons(void);
#include "gui_apps_common.h"



void copy_cstr_local(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_size && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int slen(const char* s) { 
    int n=0; 
    while(s[n]) n++; 
    return n; 
}

void scopy(char* d, const char* s, int m) {
    int i=0; 
    while(i<m-1&&s[i]){d[i]=s[i];i++;} 
    d[i]=0;
}

void scat(char* d, const char* s, int m) {
    int dl=slen(d); 
    int i=0;
    while(dl+i<m-1&&s[i]){d[dl+i]=s[i];i++;} 
    d[dl+i]=0;
}





/* fm_full_path_from_entry() は fm_effective_path()（core専用のstatic関数）に
 * 依存するため gui_apps_core.c 側に定義を置く。 */





bool smatch(const char* s1, const char* s2) {
    while (*s1 && *s2 && *s1 == *s2) { s1++; s2++; }
    return *s1 == *s2;
}





bool has_suffix(const char* str, const char* suffix) {
    int sl = slen(str);
    int pl = slen(suffix);
    if (pl > sl) return FALSE;

    /* FatFs without LFN commonly reports 8.3 names in uppercase.  Match
     * extensions without case sensitivity so ICON.BMP and icon.bmp launch
     * the same application. */
    const char* tail = str + sl - pl;
    for (int i = 0; i < pl; ++i) {
        char a = tail[i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return FALSE;
    }
    return TRUE;
}



bool sstartswith(const char* str, const char* prefix) {
    while (*prefix) {
        if (*str != *prefix) return FALSE;
        str++; prefix++;
    }
    return TRUE;
}

void draw_statusbar(int x, int y, int w, const char* status, const char* path) {
    const char* left = (status && status[0]) ? status : "Ready";
    const char* right = (path && path[0]) ? path : "/";
    vga_fill_rect(x, y, w, C_STATUSBAR_H, rgb(238, 243, 251));
    vga_fill_rect(x, y, w, 1, rgb(206, 218, 236));
    vga_fill_rect(x, y + C_STATUSBAR_H - 1, w, 1, rgb(220, 228, 240));
    vga_fill_rounded_rect(x + 8, y + 3, w / 2 - 18, C_STATUSBAR_H - 6, 6, rgb(249, 251, 255));
    vga_fill_rounded_rect(x + w / 2, y + 3, w / 2 - 10, C_STATUSBAR_H - 6, 6, rgb(245, 248, 254));
    vga_draw_rounded_rect(x + 8, y + 3, w / 2 - 18, C_STATUSBAR_H - 6, 6, rgb(210, 221, 238));
    vga_draw_rounded_rect(x + w / 2, y + 3, w / 2 - 10, C_STATUSBAR_H - 6, 6, rgb(210, 221, 238));
    vga_draw_string(x + 14, y + 4, left, rgb(46, 60, 90), 0xFFFFFFFF);
    vga_draw_string(x + w / 2 + 8, y + 4, right, rgb(96, 110, 138), 0xFFFFFFFF);
}



bool fm_is_music_file(const char* name) {
    return has_suffix(name, ".mp3") || has_suffix(name, ".wav") || has_suffix(name, ".ogg");
}
bool fm_is_video_file(const char* name) {
    return has_suffix(name, ".mp4");
}

/* Source files C-OS Studio owns: double-click opens them in the IDE (single instance). */
bool fm_is_studio_file(const char* name) {
    if (!name) return false;
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return has_suffix(base, ".c") || has_suffix(base, ".h") || !strcmp(base, "Makefile") ||
           !strcmp(base, "makefile") || !strcmp(base, "cosproj.txt");
}

bool fm_is_jpeg_file(const char* name) {
    return has_suffix(name, ".jpg") || has_suffix(name, ".jpeg") || has_suffix(name, ".jpe");
}

bool fm_is_image_file(const char* name) {
    return fm_is_jpeg_file(name) || has_suffix(name, ".png") || has_suffix(name, ".bmp") ||
           has_suffix(name, ".gif") || has_suffix(name, ".webp");
}

bool fm_is_html_file(const char* name) {
    return has_suffix(name, ".html") || has_suffix(name, ".htm") ||
           has_suffix(name, ".xhtml") || has_suffix(name, ".shtml");
}

bool fm_is_cos_executable(const char* name) {
    return has_suffix(name, ".c-os");
}

/* ---- ".app" shortcut: a small text file describing one installed app ----
 *   name=PSXFunkin
 *   exec=/apps/psxfunkin/psxfunkin.c-os
 *   icon=/apps/psxfunkin/icon.png      (PNG, any size up to 128x128)
 * The desktop draws icon= instead of a kernel glyph and opening the file
 * launches exec=. Lines starting with '#' are ignored. */
bool fm_is_app_shortcut(const char* name) {
    return has_suffix(name, ".app");
}

bool gui_app_shortcut_read(const char* path, char* name, size_t name_n,
                           char* exec, size_t exec_n, char* icon, size_t icon_n) {
    if (!path || !path[0]) return false;
    char buf[1024];
    uint64_t got = 0;
    if (!fs_read_path_into(path, buf, sizeof(buf) - 1, &got) || got == 0) return false;
    buf[got] = '\0';
    if (name && name_n) name[0] = '\0';
    if (exec && exec_n) exec[0] = '\0';
    if (icon && icon_n) icon[0] = '\0';
    char* p = buf;
    while (*p) {
        char* e = p;
        while (*e && *e != '\n' && *e != '\r') ++e;
        char save = *e; *e = '\0';
        if (*p != '#') {
            char* v = strchr(p, '=');
            if (v) {
                *v++ = '\0';
                char* dst = NULL; size_t dn = 0;
                if (!strcmp(p, "name")) { dst = name; dn = name_n; }
                else if (!strcmp(p, "exec")) { dst = exec; dn = exec_n; }
                else if (!strcmp(p, "icon")) { dst = icon; dn = icon_n; }
                if (dst && dn) { strncpy(dst, v, dn - 1); dst[dn - 1] = '\0'; }
            }
        }
        *e = save;
        p = e;
        while (*p == '\n' || *p == '\r') ++p;
    }
    return exec && exec[0];
}

/* the args= line of a shortcut (what is put before the program's own arguments, e.g. the permissions of a Sprint app) */
bool gui_app_shortcut_args(const char* path, char* out, size_t n) {
    if (!path || !out || !n) return false;
    out[0] = '\0';
    char buf[1536]; uint64_t got = 0;
    if (!fs_read_path_into(path, buf, sizeof(buf) - 1, &got) || got == 0) return false;
    buf[got] = '\0';
    for (char* p = buf; *p; ) {
        char* e = p; while (*e && *e != '\n' && *e != '\r') ++e;
        char save = *e; *e = '\0';
        if (!strncmp(p, "args=", 5)) { strncpy(out, p + 5, n - 1); out[n - 1] = '\0'; return true; }
        *e = save; p = e; while (*p == '\n' || *p == '\r') ++p;
    }
    return false;
}

/* Reads a .c-os file from disk and launches it as a real ring3 process
 * through the same ELF loader (cos_elf.c) and launcher
 * (cos_launch_elf_image()) already used for the boot-time demo programs.
 * This is the piece that was missing for "double-click a .c-os file" to
 * do anything: cos_launch_elf_image() only ever took an in-memory buffer,
 * and nothing read one from the filesystem.
 *
 * The buffer is read fully into kernel memory, handed to the loader
 * (which copies whatever it needs into the new process's own pages and
 * does not retain the pointer), and freed before returning - the loader
 * never sees anything beyond the single call it needs. */
static void launch_elf_file_args(const char* path, const char* const* argv, int argc);
void cos_launch_elf_file(const char* path) { launch_elf_file_args(path, NULL, 0); }

static void launch_elf_file_args(const char* path, const char* const* argv, int argc) {
    if (!path || !path[0]) return;

    /* fs_find()/the file manager's listing and storage_read_file() are two
     * DIFFERENT backends: fs_find() queries the FAT32 volume (via FatFs,
     * see fs.c), while storage_read_file() queries a separate custom
     * catalog+metadata-ring store (storage.c) used for window-state
     * snapshots, settings, and similar internal data. An earlier version
     * of this function used fs_find() to get the size but
     * storage_read_file() to read the bytes - two unrelated stores, so it
     * would have silently failed to find any real user-visible file. The
     * actual FAT32 read path is fs_read_file_at(), which fs_create_file_at()
     * / fs_write_file_at() (used elsewhere in this file manager) also go
     * through via FatFs. */
    /* Split the incoming path FIRST, because fs_find() matches against
     * fs_entry_t::name - the BARE filename - not the full path. The file
     * manager passes efm_entry_t::full_path here (e.g. "/diskhello.c-os"),
     * so calling fs_find() with it directly never matches and every
     * double-click reported "Not an executable file". Confirmed on a real
     * boot before this fix. */
    char dir[FS_MAX_PATH];
    const char* base = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') base = p + 1;
    }
    size_t dir_len = (size_t)(base - path);
    if (dir_len == 0) {
        dir[0] = '/'; dir[1] = '\0';
    } else {
        if (dir_len >= sizeof(dir)) dir_len = sizeof(dir) - 1;
        memcpy(dir, path, dir_len);
        /* Trim the trailing '/' that fs_read_file_at()'s own join_path()
         * adds back - passing it twice would produce "a//b". */
        if (dir_len > 1 && dir[dir_len - 1] == '/') dir_len--;
        dir[dir_len] = '\0';
    }

    /* Look the file up by its full path. fs_find() only searches the
     * directory listed LAST (the file manager's current folder), so it
     * failed for any launch that did not come from that exact folder -
     * a desktop icon, another app, or a boot-time launch. */
    uint64_t file_size = 0;
    bool is_dir = false;
    if (!fs_stat_path(path, &file_size, &is_dir) || is_dir) {
        gui_notify(gui_text("Not an executable file", "実行可能ファイルではありません"), 2000);
        return;
    }
    fs_entry_t entry_storage;
    memset(&entry_storage, 0, sizeof(entry_storage));
    entry_storage.size = file_size;
    fs_entry_t* entry = &entry_storage;

    /* Bound the read: this is the size of the FILE on disk (headers plus
     * every segment's file-backed bytes), which is normally far smaller
     * than the loaded memory image. A limit here is a sanity check against
     * a corrupt directory entry claiming an absurd size, independent of
     * cos_elf.c's own internal per-segment and total-image caps, which
     * only apply once parsing has actually started. */
    #define COS_ELF_FILE_READ_MAX (16u * 1024u * 1024u)
    if (entry->size == 0 || entry->size > COS_ELF_FILE_READ_MAX) {
        gui_notify(gui_text("File too large or empty", "ファイルサイズが不正です"), 2000);
        return;
    }

    /* fs_read_file_at() returns a pointer into a SINGLE SHARED read buffer
     * (g_read_buf in fs.c), not a fresh allocation - it is only valid until
     * the next call to this or any other fs_read_* function. Copy the
     * exact byte count from entry->size (not strlen: this is arbitrary
     * binary data and may legitimately contain zero bytes) into a private
     * buffer immediately, before anything else can reuse the shared one. */
    const char* shared = fs_read_file_at(dir, base);
    if (!shared) {
        gui_notify(gui_text("Failed to read file", "ファイルの読み込みに失敗しました"), 2000);
        return;
    }

    uint8_t* buffer = (uint8_t*)kmalloc((size_t)entry->size);
    if (!buffer) {
        gui_notify(gui_text("Out of memory", "メモリ不足です"), 2000);
        return;
    }
    memcpy(buffer, shared, (size_t)entry->size);

    extern int64_t cos_launch_elf_image(const char* name, const unsigned char* image,
                                        unsigned int image_len);
    extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                             const char *const *argv, int argc, const char *const *envp, int envc);
    int64_t launched = argc > 0 ? cos_launch_elf_image_args(base, buffer, (unsigned int)entry->size, argv, argc, NULL, 0)
                                : cos_launch_elf_image(base, buffer, (unsigned int)entry->size);
    kfree(buffer);

    if (launched < 0) {
        gui_notify(gui_text("Failed to launch program", "プログラムの起動に失敗しました"), 2500);
    }
}

void fm_open_image_viewer(const char* filename) {
    extern void cos_photo_open(const char *path);      /* the image viewer is the ring-3 Photo program */
    cos_photo_open(filename);
}

void fm_open_text_editor(const char* filename) {
    extern void cos_editor_open(const char* path);
    cos_editor_open(filename);
}

void gui_open_file_in_app(const char* path, int file_type) {
    (void)file_type;
    if (!path || !path[0]) return;
    if (fm_is_app_shortcut(path)) {
        char exec[128];
        if (gui_app_shortcut_read(path, NULL, 0, exec, sizeof(exec), NULL, 0)) {
            char args[512]; static char argbuf[600]; const char* av[24]; int ac = 0;
            (void)gui_app_shortcut_args(path, args, sizeof args);
            if (has_suffix(exec, ".sp")) {                       /* a Sprint app: the interpreter runs it, the shortcut's args= are its permissions */
                av[ac++] = "sprint.c-os"; av[ac++] = "run";
            }
            strncpy(argbuf, args, sizeof argbuf - 1); argbuf[sizeof argbuf - 1] = '\0';
            for (char* t = argbuf; *t && ac < 21; ) { while (*t == ' ') ++t; if (!*t) break; av[ac++] = t; while (*t && *t != ' ') ++t; if (*t) *t++ = '\0'; }
            if (has_suffix(exec, ".sp")) { av[ac++] = exec; launch_elf_file_args("/bin/sprint.c-os", av, ac); }
            else if (ac) { av[ac] = NULL; static const char* lead[1]; (void)lead; const char* full[24]; int fc = 0; full[fc++] = exec; for (int q = 0; q < ac; ++q) full[fc++] = av[q]; launch_elf_file_args(exec, full, fc); }
            else cos_launch_elf_file(exec);
        }
        else gui_notify(gui_text("Broken app shortcut", "アプリのショートカットが壊れています"), 2000);
    } else if (fm_is_cos_executable(path)) {
        /* Checked first, ahead of every other type: a .c-os file must
         * never fall through to the text editor (it would show binary
         * garbage) or be misidentified by any future extension check
         * that happens to overlap. */
        cos_launch_elf_file(path);
    } else if (has_suffix(path, ".sp")) {
        /* a Sprint script: run by the Sprint interpreter, which is an app of C-OS Apps */
        uint64_t sz = 0; bool dir = false;
        if (fs_stat_path("/bin/sprint.c-os", &sz, &dir) && !dir) {
            const char* av[3] = { "sprint.c-os", "run", path };
            launch_elf_file_args("/bin/sprint.c-os", av, 3);
        } else {
            gui_notify(gui_text("Sprint is not installed: get it in C-OS Apps", "Sprint が未インストールです。C-OS Apps から入手してください"), 3500);
        }
    } else if (fm_is_studio_file(path)) {
        extern void cos_studio_open(const char* path);
        cos_studio_open(path);  /* the ring-3 IDE (single instance) */
    } else if (fm_is_video_file(path)) {
        extern void cos_media_open(const char* path);
        cos_media_open(path);
    } else if (fm_is_music_file(path)) {
        extern void cos_music_open(const char* path);
        cos_music_open(path);   /* the ring-3 player (single instance) */
    } else if (fm_is_image_file(path)) {
        fm_open_image_viewer(path);
    } else if (fm_is_html_file(path)) {
        int ex = gui_find_window(WIN_BROWSER);
        if (ex < 0) {
            gui_open_window(WIN_BROWSER, gui_text("NetSurf 3.11", "NetSurf 3.11"), 180, 160, 1040, 700);
            ex = gui_find_window(WIN_BROWSER);
        }
        if (ex >= 0) {
            browser_commit_navigation(&windows[ex], path, true);
            gui_restore_window(ex);
            gui_focus_window(ex);
        }
    } else {
        fm_open_text_editor(path);
    }
}


/* 元々 gui_apps.c 内でブラウザ部分にstaticで定義されていたが、
 * タスクマネージャからも参照されるため共通ファイルへ移動・非static化。 */

