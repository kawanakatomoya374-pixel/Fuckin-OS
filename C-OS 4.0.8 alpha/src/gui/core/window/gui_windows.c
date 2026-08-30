/**
 * gui_windows.c - GUI Window Implementations
 * 
 * All window type content drawing (File Manager, Terminal, Calculator, etc.)
 * Maintains original design from gui.c
 */

#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "memory.h"
#include "serial.h"
#include "fs.h"
#include "storage.h"
#include "../drivers/input/mouse_minimal.h"
#include <string.h>
#include <stdio.h>
#include "cosnet_state.h"

const char* gui_text(const char* en, const char* ja);

#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif

static void uitostr(uint64_t num, char* buf) {
    char tmp[32];
    int i = 0, j = 0;
    if (num == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (num > 0 && i < 31) { tmp[i++] = (char)('0' + (num % 10ULL)); num /= 10ULL; }
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

static void scopy_local(char* dst, const char* src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t i = 0;
    if (src) {
        while (i + 1 < max_len && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

static size_t slen_local(const char* s) {
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void scat_local(char* dst, const char* src, size_t max_len);

static void format_size_bytes(uint64_t bytes, char* out, int max) {
    if (!out || max <= 0) return;
    if (bytes >= (1024ULL * 1024ULL * 1024ULL)) {
        uint64_t gb = bytes / (1024ULL * 1024ULL * 1024ULL);
        uint64_t tenths = (bytes % (1024ULL * 1024ULL * 1024ULL)) / (1024ULL * 1024ULL * 1024ULL / 10ULL);
        uitostr(gb, out);
        if (tenths && slen_local(out) + 2 < (size_t)max) { out[slen_local(out)] = '.'; out[slen_local(out)+1] = (char)('0' + (int)tenths); out[slen_local(out)+2] = '\0'; }
        scat_local(out, " GB", (size_t)max);
    } else if (bytes >= (1024ULL * 1024ULL)) {
        uint64_t mb = bytes / (1024ULL * 1024ULL);
        uint64_t tenths = (bytes % (1024ULL * 1024ULL)) / (1024ULL * 1024ULL / 10ULL);
        uitostr(mb, out);
        if (tenths && slen_local(out) + 2 < (size_t)max) { out[slen_local(out)] = '.'; out[slen_local(out)+1] = (char)('0' + (int)tenths); out[slen_local(out)+2] = '\0'; }
        scat_local(out, " MB", (size_t)max);
    } else if (bytes >= 1024ULL) {
        uint64_t kb = bytes / 1024ULL;
        uitostr(kb, out);
        scat_local(out, " KB", (size_t)max);
    } else {
        uitostr(bytes, out);
        scat_local(out, " B", (size_t)max);
    }
}

static void scat_local(char* dst, const char* src, size_t max_len) {
    if (!dst || max_len == 0) return;
    size_t len = 0;
    while (len < max_len && dst[len]) len++;
    size_t i = 0;
    if (src) {
        while (len + i + 1 < max_len && src[i]) { dst[len + i] = src[i]; i++; }
    }
    dst[len + i] = '\0';
}


static const char* fm_effective_path_local(window_t* w) {
    if (!w || !w->fm_path[0]) return "/";
    return w->fm_path;
}

static void fm_parent_path_local(const char* path, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || !path[0] || strcmp(path, "/") == 0) {
        scopy_local(out, "/", out_size);
        return;
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    while (len > 0 && path[len - 1] != '/') len--;
    if (len == 0) {
        scopy_local(out, "/", out_size);
        return;
    }
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
    if (out[0] == '\0') scopy_local(out, "/", out_size);
}

static char fm_tolower_local(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static bool fm_contains_ci_local(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return TRUE;
    if (!hay) return FALSE;
    for (int i = 0; hay[i]; ++i) {
        int j = 0;
        while (hay[i + j] && needle[j] && fm_tolower_local(hay[i + j]) == fm_tolower_local(needle[j])) j++;
        if (!needle[j]) return TRUE;
    }
    return FALSE;
}

static bool fm_search_matches_local(window_t* w, const fs_entry_t* e) {
    if (!w || !e) return FALSE;
    if (!w->fm_search[0]) return TRUE;
    return fm_contains_ci_local(e->name, w->fm_search) || fm_contains_ci_local(e->path, w->fm_search);
}

static int fm_type_rank_local(const fs_entry_t* e) {
    if (!e) return 99;
    if (e->is_dir) return 0;
    switch (e->file_type) {
        case FS_FILE_TYPE_TEXT: return 1;
        case FS_FILE_TYPE_AUDIO: return 2;
        case FS_FILE_TYPE_MEDIA: return 3;
        case FS_FILE_TYPE_BINARY: return 4;
        default: return 5;
    }
}

static int fm_compare_entries_local(const window_t* w, const fs_entry_t* a, const fs_entry_t* b) {
    if (!a && !b) return 0;
    if (!a) return 1;
    if (!b) return -1;

    int ad = a->is_dir ? 0 : 1;
    int bd = b->is_dir ? 0 : 1;
    if (ad != bd) return ad - bd;

    int cmp = 0;
    int sort_by = w ? w->fm_sort_by : 0;
    switch (sort_by) {
        case 1:
            cmp = (a->size < b->size) ? 1 : (a->size > b->size) ? -1 : 0;
            break;
        case 2:
            cmp = (a->modified_time < b->modified_time) ? 1 : (a->modified_time > b->modified_time) ? -1 : 0;
            break;
        case 3:
            cmp = fm_type_rank_local(a) - fm_type_rank_local(b);
            break;
        default:
            cmp = strcmp(a->name, b->name);
            break;
    }
    if (cmp == 0) cmp = strcmp(a->name, b->name);
    if (w && w->fm_sort_reverse) cmp = -cmp;
    return cmp;
}

typedef struct {
    fs_entry_t* entry;
    bool parent;
} fm_row_item_local_t;

static int fm_build_rows_local(window_t* w, fm_row_item_local_t* rows, int max_rows) {
    if (!w || !rows || max_rows <= 0) return 0;

    const char* path = fm_effective_path_local(w);
    fs_entry_t* entries = fs_list_dir(path);
    int total = fs_entry_count_for_path(path);
    int count = 0;

    if (!path_is_root(path) && count < max_rows) {
        rows[count].entry = NULL;
        rows[count].parent = TRUE;
        count++;
    }

    fs_entry_t* filtered[FS_MAX_ENTRIES];
    int filtered_count = 0;
    for (int i = 0; i < total && filtered_count < FS_MAX_ENTRIES; ++i) {
        if (!entries) continue;
        if (!fm_search_matches_local(w, &entries[i])) continue;
        filtered[filtered_count++] = &entries[i];
    }

    for (int i = 1; i < filtered_count; ++i) {
        fs_entry_t* cur = filtered[i];
        int j = i - 1;
        while (j >= 0 && fm_compare_entries_local(w, filtered[j], cur) > 0) {
            filtered[j + 1] = filtered[j];
            j--;
        }
        filtered[j + 1] = cur;
    }

    for (int i = 0; i < filtered_count && count < max_rows; ++i) {
        rows[count].entry = filtered[i];
        rows[count].parent = FALSE;
        count++;
    }

    return count;
}

static int fm_view_row_height_local(window_t* w) {
    switch (w ? w->fm_view_mode : 0) {
        case 1: return 44;
        case 2: return 34;
        default: return 30;
    }
}

static int fm_view_visible_rows_local(window_t* w) {
    if (!w) return 1;
    int row_h = fm_view_row_height_local(w);
    int content_h = w->h - TITLEBAR_H - 286;
    if (content_h < row_h) content_h = row_h;
    int rows = content_h / row_h;
    return (rows < 1) ? 1 : rows;
}

static bool fm_get_row_entry_local(window_t* w, int row, fs_entry_t** out_entry, bool* out_parent) {
    fm_row_item_local_t rows[FS_MAX_ENTRIES + 1];
    int total = fm_build_rows_local(w, rows, FS_MAX_ENTRIES + 1);
    if (out_entry) *out_entry = NULL;
    if (out_parent) *out_parent = FALSE;
    if (!w || row < 0 || row >= total) return FALSE;
    if (out_entry) *out_entry = rows[row].entry;
    if (out_parent) *out_parent = rows[row].parent;
    return TRUE;
}

static int fm_find_row_by_name_local(window_t* w, const char* name) {
    if (!w || !name || !name[0]) return -1;
    fm_row_item_local_t rows[FS_MAX_ENTRIES + 1];
    int total = fm_build_rows_local(w, rows, FS_MAX_ENTRIES + 1);
    for (int i = 0; i < total; ++i) {
        if (rows[i].parent) continue;
        if (rows[i].entry && strcmp(rows[i].entry->name, name) == 0) return i;
    }
    return -1;
}

static void fm_focus_row_local(window_t* w, int row) {
    if (!w) return;
    int total = fm_build_rows_local(w, (fm_row_item_local_t[FS_MAX_ENTRIES + 1]){0}, FS_MAX_ENTRIES + 1);
    int visible_rows = fm_view_visible_rows_local(w);
    if (row < 0 || row >= total) {
        w->fm_selected = -1;
        w->fm_scroll = 0;
        return;
    }
    w->fm_selected = row;
    if (row < w->fm_scroll) w->fm_scroll = row;
    else if (row >= w->fm_scroll + visible_rows) w->fm_scroll = row - visible_rows + 1;
    if (w->fm_scroll < 0) w->fm_scroll = 0;
    if (w->fm_scroll > total - visible_rows) w->fm_scroll = total - visible_rows;
    if (w->fm_scroll < 0) w->fm_scroll = 0;
}

static void fm_refresh_after_mutation_local(window_t* w, const char* preferred_name) {
    if (!w) return;
    if (strcmp(fm_effective_path_local(w), "/desktop") == 0) {
        gui_sync_desktop_with_fs();
    }
    if (preferred_name && preferred_name[0]) {
        int row = fm_find_row_by_name_local(w, preferred_name);
        if (row >= 0) {
            fm_focus_row_local(w, row);
            return;
        }
    }
    int total = fm_build_rows_local(w, (fm_row_item_local_t[FS_MAX_ENTRIES + 1]){0}, FS_MAX_ENTRIES + 1);
    int visible_rows = fm_view_visible_rows_local(w);
    if (w->fm_selected >= total) w->fm_selected = total - 1;
    if (w->fm_selected < -1) w->fm_selected = -1;
    if (w->fm_scroll > total - visible_rows) w->fm_scroll = total - visible_rows;
    if (w->fm_scroll < 0) w->fm_scroll = 0;
}
static void gui_windows_append_name(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) return;
    size_t len = slen_local(dst);
    size_t i = 0;
    while (len + 1 < dst_size && src[i]) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

static void gui_windows_make_unique_name(const char* base, const char* ext, char* out, size_t out_size, const char* dir) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!base || !base[0]) base = gui_text("new_item", "新規項目");
    if (!ext) ext = "";
    if (!dir || !dir[0]) dir = "/";

    char candidate[FS_MAX_NAME];
    char full[FS_MAX_PATH];
    char suffix[16];

    for (int n = 0; n < 1000; ++n) {
        scopy_local(candidate, base, sizeof(candidate));
        if (n > 0) {
            gui_windows_append_name(candidate, sizeof(candidate), " ");
            int value = n + 1;
            int pos = 0;
            do {
                if (pos + 1 < (int)sizeof(suffix)) suffix[pos++] = (char)('0' + (value % 10));
                value /= 10;
            } while (value > 0 && pos + 1 < (int)sizeof(suffix));
            suffix[pos] = '\0';
            for (int i = 0; i < pos / 2; ++i) {
                char tmp = suffix[i];
                suffix[i] = suffix[pos - 1 - i];
                suffix[pos - 1 - i] = tmp;
            }
            gui_windows_append_name(candidate, sizeof(candidate), suffix);
        }
        gui_windows_append_name(candidate, sizeof(candidate), ext);

        scopy_local(full, dir, sizeof(full));
        if (strcmp(dir, "/") != 0) {
            size_t fl = slen_local(full);
            if (fl + 1 < sizeof(full)) {
                full[fl++] = '/';
                full[fl] = '\0';
            }
        }
        gui_windows_append_name(full, sizeof(full), candidate);

        if (fs_find(full) == NULL) {
            scopy_local(out, candidate, out_size);
            return;
        }
    }

    scopy_local(out, base, out_size);
    gui_windows_append_name(out, out_size, ext);
}

static bool gui_windows_create_item(const char* path, bool is_dir) {
    if (!path || !path[0]) path = "/";
    char name[FS_MAX_NAME];
    if (is_dir) {
        gui_windows_make_unique_name(gui_text("New Folder", "新規フォルダ"), "", name, sizeof(name), path);
        if (name[0] && fs_create_dir_at(path, name)) {
            if (strcmp(path, "/desktop") == 0) gui_sync_desktop_with_fs();
            return true;
        }
    } else {
        gui_windows_make_unique_name(gui_text("new_file", "新規ファイル"), ".txt", name, sizeof(name), path);
        if (name[0] && fs_create_file_at(path, name)) {
            if (strcmp(path, "/desktop") == 0) gui_sync_desktop_with_fs();
            return true;
        }
    }
    return false;
}


static const char* fm_type_label(const fs_entry_t* e) {
    if (!e) return gui_text("Unknown", "不明");
    if (e->is_dir) return gui_text("Directory", "フォルダ");
    if (e->file_type == FS_FILE_TYPE_TEXT) return gui_text("Text", "テキスト");
    if (e->file_type == FS_FILE_TYPE_BINARY) return gui_text("Binary", "バイナリ");
    if (e->file_type == FS_FILE_TYPE_MEDIA) return gui_text("Media", "メディア");
    if (e->file_type == FS_FILE_TYPE_AUDIO) return gui_text("Audio", "音声");
    return gui_text("File", "ファイル");
}

static void fm_format_time(uint64_t value, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (value == 0) {
        scopy_local(out, "-", out_size);
        return;
    }
    char buf[32];
    uitostr(value, buf);
    scopy_local(out, buf, out_size);
}

static void fm_path_join_simple(char* out, size_t out_sz, const char* dir, const char* leaf);

static bool fm_selected_path_local(window_t* w, char* out, size_t out_size, fs_entry_t** out_entry, bool* out_parent) {
    if (out_entry) *out_entry = NULL;
    if (out_parent) *out_parent = FALSE;
    if (!w) return false;
    fs_entry_t* entry = NULL;
    bool parent = FALSE;
    if (!fm_get_row_entry_local(w, w->fm_selected, &entry, &parent) || parent || !entry) {
        return false;
    }
    if (out_entry) *out_entry = entry;
    if (out_parent) *out_parent = parent;
    if (out && out_size > 0) {
        fm_path_join_simple(out, out_size, fm_effective_path_local(w), entry->name);
    }
    return true;
}

/* External globals */
extern window_t windows[MAX_WINDOWS];
extern int window_count;
extern int active_window;
extern int gui_clip_x, gui_clip_y, gui_clip_w, gui_clip_h;
extern bool gui_clip_enabled;

/* Color definitions matching original */
#define C_TEXT         rgb(40,  45,  60)
#define C_CONTENT_BG   rgb(250, 252, 255)
#define C_TERM_BG      rgb(22,  28,  40)
#define C_TERM_TEXT    rgb(190, 235, 190)
#define C_TERM_PROMPT  rgb(120, 190, 255)
#define C_INPUT_BG     rgb(255, 255, 255)
#define C_INPUT_BORDER rgb(150, 165, 190)
#define C_BORDER       rgb(160, 175, 200)
#define C_SELECTED     rgb(50,  100, 180)
#define C_ACCENT       rgb(50,  100, 180)
#ifndef C_TEXT_LIGHT
#define C_TEXT_LIGHT   rgb(235, 240, 250)
#endif

/* Simplified calculator state */
static char calc_display[32] = "0";
static double calc_acc = 0;
static char calc_op = 0;
static int calc_clear_on_input = 0;

/* Simple music player state */
typedef struct {
    bool loaded;
    bool playing;
    char path[256];
    char title[64];
    uint64_t size_bytes;
    uint64_t duration_ms;
    uint64_t position_ms;
    uint64_t volume;
} music_player_state_t;

static music_player_state_t g_music = {0};

static void gui_copy_leaf_name(char* out, size_t out_size, const char* path) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path) return;
    const char* leaf = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') leaf = p + 1;
    }
    scopy_local(out, leaf, out_size);
}

void WEAK music_player_open(const char* path) {
    memset(&g_music, 0, sizeof(g_music));
    if (!path || !path[0]) return;
    scopy_local(g_music.path, path, sizeof(g_music.path));
    gui_copy_leaf_name(g_music.title, sizeof(g_music.title), path);

    fs_entry_t* entry = fs_find(path);
    if (entry) {
        g_music.loaded = TRUE;
        g_music.size_bytes = entry->size;
        g_music.duration_ms = (entry->size > 0) ? (entry->size * 1000ULL) / 16000ULL : 0;
        g_music.volume = 75;
        g_music.position_ms = 0;
        g_music.playing = FALSE;
    }
}


/* Draw a file/folder icon */
static void draw_fm_icon(int x, int y, bool is_dir, int file_type, bool selected) {
    uint64_t folder = selected ? rgb(246, 204, 74) : rgb(232, 184, 48);
    uint64_t folder_hi = selected ? rgb(255, 232, 150) : rgb(250, 214, 104);
    uint64_t folder_lo = selected ? rgb(182, 132, 34) : rgb(166, 120, 28);
    uint64_t sheet = selected ? rgb(250, 252, 255) : rgb(246, 249, 255);
    uint64_t sheet_edge = selected ? rgb(96, 138, 222) : rgb(136, 152, 180);
    uint64_t accent = selected ? rgb(84, 144, 236) : rgb(74, 126, 214);
    uint64_t note = selected ? rgb(116, 128, 160) : rgb(100, 116, 146);
    uint64_t image_blue = selected ? rgb(120, 188, 248) : rgb(100, 170, 236);
    uint64_t image_green = selected ? rgb(92, 184, 110) : rgb(76, 154, 92);

    if (is_dir) {
        vga_fill_rect(x + 2, y + 7, 18, 12, folder_lo);
        vga_fill_rect(x + 4, y + 4, 10, 4, folder_hi);
        vga_fill_rect(x + 3, y + 6, 20, 13, folder);
        vga_fill_rect(x + 9, y + 10, 12, 10, rgb(248, 250, 255));
        vga_draw_rect(x + 3, y + 6, 20, 13, folder_lo);
        vga_draw_rect(x + 9, y + 10, 12, 10, rgb(180, 190, 206));
        return;
    }

    if (file_type == FS_FILE_TYPE_TEXT) {
        vga_fill_rect(x + 4, y + 4, 16, 18, sheet);
        vga_fill_rect(x + 4, y + 4, 16, 3, accent);
        vga_fill_rect(x + 15, y + 4, 5, 5, rgb(224, 233, 246));
        vga_fill_rect(x + 7, y + 10, 10, 1, sheet_edge);
        vga_fill_rect(x + 7, y + 13, 10, 1, sheet_edge);
        vga_fill_rect(x + 7, y + 16, 8, 1, sheet_edge);
        vga_draw_rect(x + 4, y + 4, 16, 18, sheet_edge);
        return;
    }

    if (file_type == FS_FILE_TYPE_AUDIO) {
        vga_fill_rect(x + 4, y + 5, 16, 16, rgb(242, 235, 255));
        vga_draw_rect(x + 4, y + 5, 16, 16, rgb(182, 166, 212));
        vga_fill_rect(x + 8, y + 8, 2, 8, note);
        vga_fill_rect(x + 8, y + 8, 5, 2, note);
        vga_fill_rect(x + 11, y + 8, 2, 6, note);
        vga_fill_circle(x + 8, y + 15, 2, note);
        vga_fill_circle(x + 12, y + 13, 2, note);
        return;
    }

    if (file_type == FS_FILE_TYPE_MEDIA) {
        vga_fill_rect(x + 4, y + 4, 16, 18, rgb(247, 250, 255));
        vga_draw_rect(x + 4, y + 4, 16, 18, sheet_edge);
        vga_fill_rect(x + 5, y + 6, 14, 10, image_blue);
        vga_fill_rect(x + 5, y + 16, 14, 5, image_green);
        vga_fill_circle(x + 9, y + 10, 2, rgb(255, 224, 92));
        return;
    }

    vga_fill_rect(x + 5, y + 4, 14, 18, sheet);
    vga_fill_rect(x + 15, y + 4, 4, 4, rgb(224, 233, 246));
    vga_fill_rect(x + 7, y + 10, 10, 1, sheet_edge);
    vga_fill_rect(x + 7, y + 13, 8, 1, sheet_edge);
    vga_fill_rect(x + 7, y + 16, 10, 1, sheet_edge);
    vga_draw_rect(x + 5, y + 4, 14, 18, sheet_edge);
}

void draw_file_manager_legacy(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x + 6, cy = w->y + 34, cw = w->w - 12, ch = w->h - 40;
    if (cw < 420 || ch < 300) {
        vga_fill_rect(cx, cy, cw, ch, C_CONTENT_BG);
        vga_draw_rect(cx, cy, cw, ch, C_BORDER);
        vga_fill_rect(cx + 10, cy + 10, 6, 28, C_ACCENT);
        vga_draw_string(cx + 24, cy + 10, gui_text("Files", "ファイル"), C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 24, cy + 32, gui_text("Expand the window for the full browser layout.", "広げるとフルレイアウトで表示できます。"), rgb(60, 74, 94), 0xFFFFFFFF);
        return;
    }

    const char* path = fm_effective_path_local(w);
    fm_row_item_local_t rows[FS_MAX_ENTRIES + 1];
    int total = fm_build_rows_local(w, rows, FS_MAX_ENTRIES + 1);

    if (mouse.wheel != 0) {
        int step = fm_view_row_height_local(w) / 2;
        if (step < 12) step = 12;
        w->fm_scroll -= mouse.wheel * step;
        if (w->fm_scroll < 0) w->fm_scroll = 0;
    }

    vga_fill_rounded_rect(cx, cy, cw, ch, 16, rgb(247, 250, 255));
    vga_draw_rounded_rect(cx, cy, cw, ch, 16, rgb(192, 210, 236));

    vga_fill_rounded_rect(cx + 12, cy + 12, cw - 24, 78, 16, rgb(223, 236, 250));
    vga_draw_rounded_rect(cx + 12, cy + 12, cw - 24, 78, 16, rgb(196, 214, 236));
    vga_fill_rect(cx + 24, cy + 20, 8, 34, C_ACCENT);
    vga_draw_string(cx + 42, cy + 20, gui_text("File Browser", "ファイルブラウザー"), rgb(24, 40, 74), 0xFFFFFFFF);
    vga_draw_string(cx + 42, cy + 40, gui_text("Browse folders, inspect details, and launch editors.", "フォルダの閲覧・詳細表示・編集起動をまとめました。"), rgb(58, 74, 102), 0xFFFFFFFF);

    uint64_t total_space = storage_get_total_space();
    uint64_t used_space = storage_get_used_space();
    uint64_t free_space = storage_get_free_space();
    char sz_used[32], sz_total[32], sz_free[32];
    format_size_bytes(used_space, sz_used, 31);
    format_size_bytes(total_space, sz_total, 31);
    format_size_bytes(free_space, sz_free, 31);

    int badge_y = cy + 24;
    int badge_w = 126;
    int badge_h = 34;
    vga_fill_rounded_rect(cx + cw - 12 - badge_w * 2 - 10, badge_y, badge_w, badge_h, 10, rgb(255,255,255));
    vga_draw_rounded_rect(cx + cw - 12 - badge_w * 2 - 10, badge_y, badge_w, badge_h, 10, rgb(206, 218, 236));
    vga_draw_string(cx + cw - 12 - badge_w * 2, badge_y + 10, sz_used, rgb(28, 44, 76), 0xFFFFFFFF);
    vga_draw_string(cx + cw - 12 - badge_w * 2 + 48, badge_y + 10, gui_text("used", "使用済み"), rgb(78, 92, 118), 0xFFFFFFFF);
    vga_fill_rounded_rect(cx + cw - 12 - badge_w, badge_y, badge_w, badge_h, 10, rgb(255,255,255));
    vga_draw_rounded_rect(cx + cw - 12 - badge_w, badge_y, badge_w, badge_h, 10, rgb(206, 218, 236));
    vga_draw_string(cx + cw - 12 - badge_w + 18, badge_y + 10, sz_free, rgb(28, 44, 76), 0xFFFFFFFF);
    vga_draw_string(cx + cw - 12 - badge_w + 66, badge_y + 10, gui_text("free", "空き"), rgb(78, 92, 118), 0xFFFFFFFF);

    int path_x = cx + 12;
    int path_y = cy + 90;
    int path_w = cw - 24;
    vga_fill_rounded_rect(path_x, path_y, path_w, 30, 10, rgb(255, 255, 255));
    vga_draw_rounded_rect(path_x, path_y, path_w, 30, 10, rgb(206, 218, 236));
    vga_draw_string(path_x + 10, path_y + 9, path, rgb(28, 44, 76), 0xFFFFFFFF);

    int toolbar_y = cy + 130;
    int button_h = 28;
    struct { const char* label; } btns[] = {
        { gui_text("Up", "上へ") },
        { gui_text("New File", "新規ファイル") },
        { gui_text("New Folder", "新規フォルダ") },
        { gui_text("Rename", "名前変更") },
        { gui_text("Copy To", "コピー先") },
        { gui_text("Move To", "移動先") },
        { gui_text("Search", "検索") },
        { gui_text("Sort", "並べ替え") },
        { gui_text("View", "表示") },
        { gui_text("Info", "情報") },
        { gui_text("Refresh", "更新") },
    };
    int bx = cx + 12;
    for (int i = 0; i < 11; ++i) {
        int bw = (int)slen_local(btns[i].label) * FONT_W + 26;
        if (bw < 54) bw = 54;
        bool active = FALSE;
        if (i == 6 && w->fm_search_active) active = TRUE;
        if ((i == 8) && w->fm_view_mode == 1) active = TRUE;
        if (i == 7 && (w->fm_sort_by != 0 || w->fm_sort_reverse)) active = TRUE;
        vga_fill_rounded_rect(bx, toolbar_y, bw, button_h, 8, active ? rgb(46, 110, 190) : rgb(34, 54, 90));
        vga_draw_rounded_rect(bx, toolbar_y, bw, button_h, 8, rgb(118, 140, 176));
        vga_draw_string(bx + 12, toolbar_y + 7, btns[i].label, rgb(255, 255, 255), 0xFFFFFFFF);
        bx += bw + 8;
    }

    int search_y = cy + 168;
    vga_fill_rounded_rect(path_x, search_y, path_w, 32, 10, rgb(255, 255, 255));
    vga_draw_rounded_rect(path_x, search_y, path_w, 32, 10, w->fm_search_active ? rgb(82, 134, 226) : rgb(206, 218, 236));
    if (w->fm_search[0]) {
        vga_draw_string(path_x + 10, search_y + 8, w->fm_search, rgb(24, 40, 74), 0xFFFFFFFF);
    } else {
        vga_draw_string(path_x + 10, search_y + 8, gui_text("Search this folder...", "このフォルダを検索..."), rgb(120, 132, 154), 0xFFFFFFFF);
    }
    vga_draw_string(path_x + path_w - 126, search_y + 8, w->fm_search_active ? gui_text("typing", "入力中") : gui_text("Ctrl+F", "Ctrl+F"), rgb(92, 106, 130), 0xFFFFFFFF);

    int list_y = cy + 210;
    int list_h = cy + ch - 58 - list_y;
    if (list_h < 80) list_h = 80;
    vga_fill_rounded_rect(path_x, list_y, path_w, list_h, 12, rgb(255, 255, 255));
    vga_draw_rounded_rect(path_x, list_y, path_w, list_h, 12, rgb(206, 218, 236));

    int mode = (w->fm_view_mode < 0 || w->fm_view_mode > 2) ? 0 : w->fm_view_mode;
    int row_h = (mode == 1) ? 44 : (mode == 2) ? 34 : 30;
    int visible_rows = list_h / row_h;
    if (visible_rows < 1) visible_rows = 1;
    if (w->fm_scroll < 0) w->fm_scroll = 0;
    if (w->fm_scroll > total - visible_rows) w->fm_scroll = total - visible_rows;
    if (w->fm_scroll < 0) w->fm_scroll = 0;

    int start = w->fm_scroll;
    if (start < 0) start = 0;
    if (start > total) start = total;

    if (mode == 2) {
        vga_fill_rect(path_x + 6, list_y + 2, path_w - 12, 18, rgb(241, 246, 253));
        vga_draw_string(path_x + 14, list_y + 4, gui_text("Name", "名前"), rgb(76, 92, 118), 0xFFFFFFFF);
        vga_draw_string(path_x + path_w - 352, list_y + 4, gui_text("Type", "種類"), rgb(76, 92, 118), 0xFFFFFFFF);
        vga_draw_string(path_x + path_w - 250, list_y + 4, gui_text("Size", "サイズ"), rgb(76, 92, 118), 0xFFFFFFFF);
        vga_draw_string(path_x + path_w - 132, list_y + 4, gui_text("Modified", "更新"), rgb(76, 92, 118), 0xFFFFFFFF);
    }

    for (int i = 0; i < visible_rows && start + i < total; ++i) {
        int row = start + i;
        int ry = list_y + i * row_h;
        bool hov = (mouse.x >= path_x && mouse.x < path_x + path_w && mouse.y >= ry && mouse.y < ry + row_h);
        bool sel = (w->fm_selected == row);
        if (sel) vga_fill_rect(path_x + 4, ry + 2, path_w - 8, row_h - 4, rgb(220, 234, 252));
        else if (hov) vga_fill_rect(path_x + 4, ry + 2, path_w - 8, row_h - 4, rgb(242, 247, 255));

        if (rows[row].parent) {
            vga_draw_string(path_x + 14, ry + 8, gui_text(".. (Parent folder)", ".. (親フォルダ)"), rgb(46, 76, 120), 0xFFFFFFFF);
            continue;
        }

        fs_entry_t* e = rows[row].entry;
        if (!e) continue;
        char size_buf[24];
        size_buf[0] = '\0';
        format_size_bytes(e->size, size_buf, sizeof(size_buf) - 1);

        if (mode == 2) {
            char mod_buf[32];
            fm_format_time(e->modified_time, mod_buf, sizeof(mod_buf));
            vga_draw_string(path_x + 14, ry + 7, e->name, rgb(28, 44, 76), 0xFFFFFFFF);
            vga_draw_string(path_x + path_w - 352, ry + 7, e->is_dir ? gui_text("Folder", "フォルダ") : gui_text("File", "ファイル"), rgb(82, 96, 118), 0xFFFFFFFF);
            vga_draw_string(path_x + path_w - 250, ry + 7, size_buf, rgb(82, 96, 118), 0xFFFFFFFF);
            vga_draw_string(path_x + path_w - 132, ry + 7, mod_buf, rgb(82, 96, 118), 0xFFFFFFFF);
        } else if (mode == 1) {
            vga_draw_string(path_x + 14, ry + 7, e->is_dir ? gui_text("[DIR]", "[DIR]") : gui_text("[FILE]", "[FILE]"), rgb(82, 96, 118), 0xFFFFFFFF);
            vga_draw_string(path_x + 80, ry + 7, e->name, rgb(28, 44, 76), 0xFFFFFFFF);
            vga_draw_string(path_x + path_w - 120, ry + 7, size_buf, rgb(82, 96, 118), 0xFFFFFFFF);
        } else {
            vga_draw_string(path_x + 14, ry + 8, e->name, rgb(28, 44, 76), 0xFFFFFFFF);
            vga_draw_string(path_x + path_w - 130, ry + 8, e->is_dir ? gui_text("Folder", "フォルダ") : size_buf, rgb(82, 96, 118), 0xFFFFFFFF);
        }
    }

    int footer_y = cy + ch - 44;
    if (w->fm_action != 0) {
        vga_fill_rounded_rect(path_x, footer_y, path_w, 34, 10, rgb(244, 247, 252));
        vga_draw_rounded_rect(path_x, footer_y, path_w, 34, 10, rgb(206,218,236));
        const char* prompt =
            (w->fm_action == 1) ? gui_text("Create file", "ファイルを作成") :
            (w->fm_action == 2) ? gui_text("Create folder", "フォルダを作成") :
            (w->fm_action == 3) ? gui_text("Rename item", "名前を変更") :
            (w->fm_action == 4) ? gui_text("Copy to path", "コピー先パス") :
            (w->fm_action == 5) ? gui_text("Move to path", "移動先パス") :
                                 gui_text("Item action", "項目操作");
        vga_draw_string(path_x + 12, footer_y + 9, prompt, rgb(28, 50, 92), 0xFFFFFFFF);
        vga_fill_rounded_rect(path_x + 136, footer_y + 5, path_w - 158, 24, 7, rgb(255,255,255));
        vga_draw_rounded_rect(path_x + 136, footer_y + 5, path_w - 158, 24, 7, rgb(206,218,236));
        vga_draw_string(path_x + 146, footer_y + 8, w->fm_input, C_TEXT, 0xFFFFFFFF);
    } else {
        char item_count_buf[96];
        char numbuf[16];
        item_count_buf[0] = '\0';
        uitostr((uint64_t)total, numbuf);
        scat_local(item_count_buf, numbuf, sizeof(item_count_buf));
        scat_local(item_count_buf, gui_text(" items", " 項目"), sizeof(item_count_buf));
        vga_draw_string(path_x + 12, footer_y + 8, item_count_buf, rgb(74, 94, 130), 0xFFFFFFFF);
        vga_draw_string(path_x + 120, footer_y + 8, gui_text("Enter=open  Backspace=up  F2=rename  Del=delete  F6=copy  F7=move", "Enter=開く  Backspace=上へ  F2=名前変更  Del=削除  F6=コピー先  F7=移動先"), rgb(100, 116, 142), 0xFFFFFFFF);

        fs_entry_t* selected_entry = NULL;
        bool selected_parent = FALSE;
        char selected_path[FS_MAX_PATH];
        selected_path[0] = '\0';
        if (fm_selected_path_local(w, selected_path, sizeof(selected_path), &selected_entry, &selected_parent) && selected_entry && !selected_parent) {
            char selected_buf[256];
            char size_buf2[24];
            format_size_bytes(selected_entry->size, size_buf2, sizeof(size_buf2) - 1);
            selected_buf[0] = '\0';
            scat_local(selected_buf, gui_text("Selected: ", "選択: "), sizeof(selected_buf));
            scat_local(selected_buf, selected_entry->name, sizeof(selected_buf));
            scat_local(selected_buf, gui_text("  [", "  ["), sizeof(selected_buf));
            scat_local(selected_buf, fm_type_label(selected_entry), sizeof(selected_buf));
            scat_local(selected_buf, gui_text("]  Size: ", "]  サイズ: "), sizeof(selected_buf));
            scat_local(selected_buf, size_buf2, sizeof(selected_buf));
            vga_draw_string(path_x + 12, footer_y + 24, selected_buf, rgb(90, 106, 136), 0xFFFFFFFF);
        }
    }
}
/* Simple syntax highlight: detect keyword type */
static uint64_t te_token_color(const char* tok, int len) {
    /* C keywords */
    static const char* kw[] = {
        "int","char","float","double","void","return","if","else","while",
        "for","do","break","continue","struct","typedef","static","const",
        "extern","include","define","ifdef","ifndef","endif","true","false",
        "NULL","bool","uint8_t","uint16_t","uint32_t","uint64_t","int32_t",NULL
    };
    char tmp[32];
    int l = (len < 31) ? len : 31;
    for (int i = 0; i < l; i++) tmp[i] = tok[i];
    tmp[l] = '\0';
    for (int i = 0; kw[i]; i++) {
        const char* k = kw[i];
        int kl = 0; while (k[kl]) kl++;
        if (kl == l) {
            bool match = TRUE;
            for (int j = 0; j < l; j++) if (tmp[j] != k[j]) { match = FALSE; break; }
            if (match) return rgb(28, 90, 180); /* keyword blue */
        }
    }
    /* Number */
    bool is_num = (l > 0);
    for (int i = 0; i < l; i++) if (tok[i] < '0' || tok[i] > '9') { is_num = FALSE; break; }
    if (is_num) return rgb(150, 96, 28); /* number orange */
    return rgb(28, 40, 60); /* default text */
}

void WEAK draw_text_editor(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x + 4, cy = w->y + 34, cw = w->w - 8, ch = w->h - 38;
    if (cw < 120 || ch < 100) {
        vga_fill_rect(cx, cy, cw, ch, C_CONTENT_BG);
        vga_draw_rect(cx, cy, cw, ch, C_BORDER);
        vga_draw_string(cx + 8, cy + 8, gui_text("Text Editor", "テキストエディター"), C_TEXT, 0xFFFFFFFF);
        vga_draw_string(cx + 8, cy + 24, gui_text("Window too small", "ウィンドウが小さすぎます"), rgb(60, 74, 94), 0xFFFFFFFF);
        return;
    }

    /* Background */
    vga_fill_rect(cx, cy, cw, ch, rgb(247, 249, 253));
    vga_draw_rect(cx, cy, cw, ch, rgb(160, 176, 202));

    /* Top toolbar */
    int toolbar_h = FONT_H + 14;
    for (int gy = 0; gy < toolbar_h; gy++) {
        float t = (float)gy / (float)toolbar_h;
        int r = (int)(235 - t * 18);
        int gv = (int)(241 - t * 16);
        int bv = (int)(249 - t * 12);
        vga_fill_rect(cx, cy + gy, cw, 1, rgb((uint8_t)r, (uint8_t)gv, (uint8_t)bv));
    }
    vga_fill_rect(cx, cy + toolbar_h, cw, 1, rgb(160, 176, 202));

    /* Filename */
    const char* fname = w->filename[0] ? w->filename : gui_text("Untitled", "無題");
    vga_draw_string(cx + 10, cy + (toolbar_h - FONT_H) / 2, fname, rgb(28, 40, 60), 0xFFFFFFFF);
    if (w->text_modified) {
        vga_draw_string(cx + 10 + (int)slen_local(fname) * FONT_W + 6, cy + (toolbar_h - FONT_H) / 2, "*", rgb(200, 70, 50), 0xFFFFFFFF);
    }

    /* Toolbar buttons */
    struct { const char* label; } tbtn[] = {
        { gui_text("New", "新規") },
        { gui_text("Open", "開く") },
        { gui_text("Save", "保存") },
        { gui_text("All", "全選択") }
    };
    int tbx = cx + cw - 320;
    for (int i = 0; i < 4; i++) {
        minimal_mouse_t* ms = _get_mouse();
        const char* label = tbtn[i].label;
        int label_w = (int)slen_local(label) * FONT_W;
        int bw = label_w + 24;
        if (bw < FONT_W * 4) bw = FONT_W * 4;
        if (bw > 132) bw = 132;
        bool hov = (ms && ms->x >= tbx && ms->x < tbx + bw &&
                    ms->y >= cy + 6 && ms->y < cy + toolbar_h - 6);
        vga_fill_rounded_rect(tbx, cy + 6, bw, toolbar_h - 12, 5,
                              hov ? rgb(80, 130, 220) : rgb(28, 46, 84));
        vga_draw_rounded_rect(tbx, cy + 6, bw, toolbar_h - 12, 5, rgb(110, 132, 170));
        vga_draw_string(tbx + (bw - label_w) / 2, cy + (toolbar_h - FONT_H) / 2, label, rgb(255, 255, 255), 0xFFFFFFFF);
        tbx += bw + 8;
    }

    /* Line number gutter */
    int gutter_w = FONT_W * 3 + 20;
    int text_area_x = cx + gutter_w;
    int text_area_y = cy + toolbar_h + 1;
    int text_area_w = cw - gutter_w - 4;
    int text_area_h = ch - toolbar_h - 20;
    if (text_area_w < 40 || text_area_h < 20) return;

    /* Gutter background */
    vga_fill_rect(cx, text_area_y, gutter_w, text_area_h, rgb(232, 236, 242));
    vga_fill_rect(cx + gutter_w - 1, text_area_y, 1, text_area_h, rgb(190, 200, 218));

    /* Text area background */
    vga_fill_rect(text_area_x, text_area_y, text_area_w, text_area_h, rgb(255, 255, 255));

    int line_h = FONT_H + 8;
    int visible_lines = text_area_h / line_h;
    if (visible_lines < 1) visible_lines = 1;
    int start_line = w->scroll_y;
    if (start_line < 0) start_line = 0;

    /* Count cursor position */
    int cursor_line = 0, cursor_col = 0;
    for (int i = 0; w->text_buf[i] && i < w->text_cursor; ++i) {
        if (w->text_buf[i] == '\n') { cursor_line++; cursor_col = 0; }
        else cursor_col++;
    }

    /* Highlight current line */
    if (cursor_line >= start_line && cursor_line < start_line + visible_lines) {
        int hl_y = text_area_y + (cursor_line - start_line) * line_h;
        vga_fill_rect(cx, hl_y, cw - 4, line_h, rgb(236, 242, 252));
    }

    const char* p = w->text_buf;
    int line_no = 0;

    while (*p && line_no < start_line + visible_lines) {
        const char* line_start = p;
        while (*p && *p != '\n') ++p;
        int len = (int)(p - line_start);

        if (line_no >= start_line) {
            int draw_line = line_no - start_line;
            int y = text_area_y + draw_line * line_h;

            /* Line number */
            char lnum[8];
            int ln = line_no + 1, li = 0;
            char ltmp[8]; int lj = 0;
            do { ltmp[lj++] = (char)('0' + ln % 10); ln /= 10; } while (ln > 0 && lj < 7);
            for (int k = lj - 1; k >= 0; k--) lnum[li++] = ltmp[k];
            lnum[li] = '\0';
            int ln_x = cx + gutter_w - (int)slen_local(lnum) * FONT_W - 6;
            uint64_t lnum_col = (cursor_line == line_no) ? rgb(200, 220, 255) : rgb(58, 74, 102);
            vga_draw_string(ln_x, y + 4, lnum, lnum_col, 0xFFFFFFFF);

            /* Text with simple syntax highlighting */
            int skip = w->scroll_x;
            if (skip < 0) skip = 0;
            if (skip > len) skip = len;

            /* Check for comment/string on this line */
            bool line_comment = FALSE;
            for (int ci = 0; ci < len - 1 && ci < 2; ci++) {
                if (line_start[ci] == '/' && line_start[ci+1] == '/') line_comment = TRUE;
                if (line_start[ci] == '#') line_comment = TRUE;
            }
            if (len > 0 && line_start[0] == '#') line_comment = TRUE;

            int draw_x = text_area_x + 6;
            int ci = skip;
            while (ci < len) {
                /* Detect token start */
                char c = line_start[ci];
                uint64_t col;

                if (line_comment) {
                    col = rgb(0, 124, 80); /* comment green */
                    /* Draw rest of line as comment */
                    int draw_len = len - ci;
                    if (draw_len > 0) {
                        char buf[256];
                        if (draw_len >= (int)sizeof(buf)) draw_len = (int)sizeof(buf) - 1;
                        for (int k = 0; k < draw_len; k++) buf[k] = line_start[ci + k];
                        buf[draw_len] = '\0';
                        vga_draw_string(draw_x, y + 4, buf, col, 0xFFFFFFFF);
                    }
                    break;
                } else if (c == '"' || c == '\'') {
                    /* String literal */
                    col = rgb(170, 92, 36);
                    char delim = c;
                    int start_ci = ci;
                    ci++;
                    while (ci < len && line_start[ci] != delim) ci++;
                    if (ci < len) ci++;
                    int slen = ci - start_ci;
                    if (slen > 0) {
                        char buf[128];
                        if (slen >= (int)sizeof(buf)) slen = (int)sizeof(buf) - 1;
                        for (int k = 0; k < slen; k++) buf[k] = line_start[start_ci + k];
                        buf[slen] = '\0';
                        vga_draw_string(draw_x, y + 1, buf, col, 0xFFFFFFFF);
                        draw_x += slen * FONT_W;
                    }
                } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
                    /* Identifier/keyword */
                    int start_ci = ci;
                    while (ci < len && ((line_start[ci] >= 'a' && line_start[ci] <= 'z') ||
                                        (line_start[ci] >= 'A' && line_start[ci] <= 'Z') ||
                                        (line_start[ci] >= '0' && line_start[ci] <= '9') ||
                                        line_start[ci] == '_')) ci++;
                    int tlen = ci - start_ci;
                    col = te_token_color(line_start + start_ci, tlen);
                    char buf[64];
                    if (tlen >= (int)sizeof(buf)) tlen = (int)sizeof(buf) - 1;
                    for (int k = 0; k < tlen; k++) buf[k] = line_start[start_ci + k];
                    buf[tlen] = '\0';
                    vga_draw_string(draw_x, y + 1, buf, col, 0xFFFFFFFF);
                    draw_x += tlen * FONT_W;
                } else if (c >= '0' && c <= '9') {
                    /* Number */
                    int start_ci = ci;
                    while (ci < len && ((line_start[ci] >= '0' && line_start[ci] <= '9') ||
                                        line_start[ci] == '.' || line_start[ci] == 'x' ||
                                        line_start[ci] == 'X')) ci++;
                    int nlen = ci - start_ci;
                    char buf[32];
                    if (nlen >= (int)sizeof(buf)) nlen = (int)sizeof(buf) - 1;
                    for (int k = 0; k < nlen; k++) buf[k] = line_start[start_ci + k];
                    buf[nlen] = '\0';
                    vga_draw_string(draw_x, y + 1, buf, rgb(150, 96, 28), 0xFFFFFFFF);
                    draw_x += nlen * FONT_W;
                } else {
                    /* Punctuation/operator or one complete UTF-8 codepoint.
                     * Non-ASCII codepoints must never be passed to the text
                     * renderer one byte at a time: that would create three
                     * fallback boxes for each Japanese character. */
                    uint64_t punct_col = (c == '{' || c == '}' || c == '(' || c == ')') ?
                                         rgb(144, 110, 40) : rgb(80, 92, 120);
                    int char_len = 1;
                    unsigned char lead = (unsigned char)c;
                    if ((lead & 0xE0u) == 0xC0u) char_len = 2;
                    else if ((lead & 0xF0u) == 0xE0u) char_len = 3;
                    else if ((lead & 0xF8u) == 0xF0u) char_len = 4;
                    if (ci + char_len > len) char_len = 1;
                    char buf[5] = {0, 0, 0, 0, 0};
                    for (int k = 0; k < char_len; ++k) buf[k] = line_start[ci + k];
                    vga_draw_string(draw_x, y + 1, buf, punct_col, 0xFFFFFFFF);
                    draw_x += (lead >= 0x80u) ? (FONT_W * 2) : FONT_W;
                    ci += char_len;
                }
            }

            /* Cursor */
            if (cursor_line == line_no) {
                int caret_x = text_area_x + 6 + (cursor_col - w->scroll_x) * FONT_W;
                if (caret_x >= text_area_x + 2 && caret_x < text_area_x + text_area_w - 2) {
                    vga_fill_rect(caret_x, y, 2, line_h, rgb(60, 120, 220));
                }
            }
        }
        if (*p == '\n') ++p;
        ++line_no;
    }

    /* Scrollbar */
    if (line_no > visible_lines) {
        int sb_x = cx + cw - 10;
        int sb_h = text_area_h;
        vga_fill_rect(sb_x, text_area_y, 8, sb_h, rgb(232, 236, 242));
        int thumb_h = (sb_h * visible_lines) / (line_no > 0 ? line_no : 1);
        if (thumb_h < 16) thumb_h = 16;
        int thumb_y = text_area_y + (sb_h - thumb_h) * start_line / (line_no - visible_lines > 0 ? line_no - visible_lines : 1);
        vga_fill_rounded_rect(sb_x + 1, thumb_y, 6, thumb_h, 3, rgb(120, 144, 184));
    }

    /* Status bar */
    int status_y = cy + ch - 18;
    vga_fill_rect(cx, status_y, cw, 18, rgb(235, 239, 246));
    vga_fill_rect(cx, status_y, cw, 1, rgb(190, 200, 218));
    /* Cursor position */
    char pos_buf[32];
    pos_buf[0] = 'L'; pos_buf[1] = 'n'; pos_buf[2] = ':';
    int pi = 3, pn = cursor_line + 1;
    char ptmp[8]; int pj = 0;
    do { ptmp[pj++] = (char)('0' + pn % 10); pn /= 10; } while (pn > 0 && pj < 7);
    for (int k = pj - 1; k >= 0; k--) pos_buf[pi++] = ptmp[k];
    pos_buf[pi++] = ' '; pos_buf[pi++] = 'C'; pos_buf[pi++] = 'o'; pos_buf[pi++] = 'l'; pos_buf[pi++] = ':';
    pn = cursor_col + 1; pj = 0;
    do { ptmp[pj++] = (char)('0' + pn % 10); pn /= 10; } while (pn > 0 && pj < 7);
    for (int k = pj - 1; k >= 0; k--) pos_buf[pi++] = ptmp[k];
    pos_buf[pi] = '\0';
    vga_draw_string(cx + 6, status_y + 2, pos_buf, rgb(48, 62, 92), 0xFFFFFFFF);
    vga_draw_string(cx + cw - 96, status_y + 2, gui_text("UTF-8", "UTF-8"), rgb(72, 88, 116), 0xFFFFFFFF);

    /* "Save As" 入力オーバーレイ (filename が空のまま保存しようとした時) */
    if (w->fm_action == 1) {
        int ow = cw > 420 ? 420 : cw - 20;
        int oh = 76;
        int ox = cx + (cw - ow) / 2;
        int oy = cy + (ch - oh) / 2;
        vga_fill_rect(cx, cy, cw, ch, 0x40000000); /* 半透明オーバーレイ (他のダイアログと同様の表現) */
        vga_fill_rounded_rect(ox, oy, ow, oh, 8, rgb(250, 251, 253));
        vga_draw_rounded_rect(ox, oy, ow, oh, 8, rgb(150, 168, 196));
        vga_draw_string(ox + 14, oy + 12, gui_text("Save As", "名前を付けて保存"), rgb(28, 40, 60), 0xFFFFFFFF);
        int fbx = ox + 14, fby = oy + 34, fbw = ow - 28, fbh = 24;
        vga_fill_rect(fbx, fby, fbw, fbh, rgb(255, 255, 255));
        vga_draw_rect(fbx, fby, fbw, fbh, rgb(120, 150, 200));
        vga_draw_string(fbx + 6, fby + 5, w->fm_input, rgb(20, 28, 40), 0xFFFFFFFF);
        /* Cursor (steady, not blinking, to avoid extra frame-counter plumbing) */
        int cursor_x = fbx + 6 + (int)slen_local(w->fm_input) * FONT_W;
        vga_fill_rect(cursor_x, fby + 4, 2, fbh - 8, rgb(40, 80, 180));
        vga_draw_string(ox + 14, oy + oh - 18, gui_text("Enter: save to /desktop  Esc: cancel",
                        "Enter:/desktopに保存  Esc:キャンセル"), rgb(110, 120, 138), 0xFFFFFFFF);
    }
}

void draw_storage_manager(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x + 4, cy = w->y + 34, cw = w->w - 8, ch = w->h - 38;
    
    vga_fill_rect(cx, cy, cw, ch, C_CONTENT_BG);
    
    vga_draw_string(cx + 20, cy + 20, gui_text("Storage Manager", "ストレージマネージャー"), rgb(45, 85, 145), 0xFFFFFFFF);
    
    vga_fill_rect(cx + 20, cy + 50, cw - 40, 20, rgb(200, 200, 200));
    vga_fill_rect(cx + 20, cy + 50, (cw - 40) / 4, 20, rgb(60, 180, 100));
    
    uint64_t total = storage_get_total_space();
    uint64_t used = storage_get_used_space();
    uint64_t free_space = storage_get_free_space();
    char total_buf[32], used_buf[32], free_buf[32];
    uitostr(total / 1024ULL, total_buf);
    uitostr(used / 1024ULL, used_buf);
    uitostr(free_space / 1024ULL, free_buf);
    vga_draw_string(cx + 20, cy + 80, total_buf, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 20 + 72, cy + 80, gui_text("KB persistent storage", "KB 永続ストレージ"), rgb(102, 110, 126), 0xFFFFFFFF);
    vga_draw_string(cx + 20, cy + 100, used_buf, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 20 + 72, cy + 100, gui_text("KB used", "KB 使用済み"), rgb(102, 110, 126), 0xFFFFFFFF);
    vga_draw_string(cx + 20, cy + 120, free_buf, C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 20 + 72, cy + 120, gui_text("KB free", "KB 空き"), rgb(102, 110, 126), 0xFFFFFFFF);
    
    vga_draw_string(cx + 20, cy + 150, gui_text("Backing: ATA disk image when attached", "バックエンド: ATA ディスクイメージを接続時に使用"), C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 20, cy + 170, storage_has_password() ? gui_text("Password: set", "パスワード: 設定済み") : gui_text("Password: not set", "パスワード: 未設定"), rgb(180, 100, 100), 0xFFFFFFFF);
}

void draw_browser(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x + 4, cy = w->y + 34, cw = w->w - 8, ch = w->h - 38;
    
    vga_fill_rect(cx, cy, cw, ch, C_CONTENT_BG);
    
    vga_draw_string(cx + 20, cy + 20, gui_text("NetSurf", "NetSurf"), rgb(45, 85, 145), 0xFFFFFFFF);
    vga_draw_string(cx + 20, cy + 50, gui_text("Address: http://localhost/", "アドレス: http://localhost/"), C_TEXT, 0xFFFFFFFF);
    vga_fill_rect(cx + 20, cy + 70, cw - 40, 20, C_INPUT_BG);
    vga_draw_rect(cx + 20, cy + 70, cw - 40, 20, C_INPUT_BORDER);
    
    vga_draw_string(cx + 20, cy + 110, gui_text("Page Title: C-OS 4.0.8 alpha", "ページタイトル: C-OS 4.0.8 alpha"), C_TEXT, 0xFFFFFFFF);
    vga_fill_rect(cx + 20, cy + 130, cw - 40, ch - 160, rgb(240, 245, 250));
    vga_draw_string(cx + 30, cy + 150, gui_text("Welcome to NetSurf on C-OS 4.0.8!", "C-OS 4.0.8 の NetSurf へようこそ"), C_ACCENT, 0xFFFFFFFF);
    vga_draw_string(cx + 30, cy + 170, gui_text("Features:", "機能"), C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 50, cy + 190, gui_text("Basic HTML rendering", "基本的な HTML 描画"), C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 50, cy + 210, gui_text("Local file browsing", "ローカルファイル閲覧"), C_TEXT, 0xFFFFFFFF);
    vga_draw_string(cx + 50, cy + 230, gui_text("Simple navigation", "シンプルなナビゲーション"), C_TEXT, 0xFFFFFFFF);
}

/* Pseudo-random spectrum for visualizer */
static uint8_t mp3_vis_bars[32] = {0};
static int mp3_vis_tick = 0;

static void mp3_update_visualizer(bool playing) {
    mp3_vis_tick++;
    for (int i = 0; i < 32; i++) {
        if (playing) {
            /* Simulate spectrum with pseudo-random motion */
            int seed = (mp3_vis_tick * 17 + i * 31 + i * mp3_vis_tick / 4) & 0xFF;
            int target = (seed % 60) + (i < 8 ? 20 : i < 16 ? 30 : i < 24 ? 15 : 10);
            if (mp3_vis_bars[i] < (uint8_t)target) mp3_vis_bars[i]++;
            else if (mp3_vis_bars[i] > (uint8_t)target) mp3_vis_bars[i]--;
        } else {
            if (mp3_vis_bars[i] > 0) mp3_vis_bars[i]--;
        }
    }
}

void WEAK draw_music_player(int idx) {
    window_t* w = &windows[idx];
    int cx = w->x + 4, cy = w->y + 34, cw = w->w - 8, ch = w->h - 38;
    minimal_mouse_t* ms = _get_mouse();

    /* Dark theme background */
    vga_fill_rect(cx, cy, cw, ch, rgb(18, 22, 34));
    vga_draw_rect(cx, cy, cw, ch, rgb(50, 70, 120));

    /* Header gradient */
    for (int gy = 0; gy < 36; gy++) {
        float t = (float)gy / 36.0f;
        int r = (int)(30 - t * 8);
        int gv = (int)(36 - t * 10);
        int bv = (int)(56 - t * 14);
        vga_fill_rect(cx, cy + gy, cw, 1, rgb((uint8_t)r, (uint8_t)gv, (uint8_t)bv));
    }
    vga_fill_rect(cx, cy + 36, cw, 1, rgb(50, 70, 120));

    /* Music note icon */
    vga_fill_rect(cx + 14, cy + 10, 3, 12, rgb(100, 180, 255));
    vga_fill_rect(cx + 14, cy + 10, 8, 3, rgb(100, 180, 255));
    vga_fill_rect(cx + 19, cy + 10, 3, 9, rgb(80, 160, 235));
    vga_fill_circle(cx + 15, cy + 22, 3, rgb(100, 180, 255));
    vga_fill_circle(cx + 20, cy + 19, 3, rgb(80, 160, 235));

    /* Title */
    vga_draw_string(cx + 36, cy + 10, gui_text("Music Player", "音楽プレーヤー"), rgb(180, 210, 255), 0xFFFFFFFF);

    if (!g_music.loaded) {
        /* Empty state */
        int box_y = cy + 50;
        vga_fill_rounded_rect(cx + 16, box_y, cw - 32, 80, 8, rgb(28, 34, 52));
        vga_draw_rounded_rect(cx + 16, box_y, cw - 32, 80, 8, rgb(60, 80, 130));
        /* Big music note */
        vga_fill_rect(cx + cw/2 - 10, box_y + 15, 4, 20, rgb(80, 100, 150));
        vga_fill_rect(cx + cw/2 - 10, box_y + 15, 14, 5, rgb(80, 100, 150));
        vga_fill_rect(cx + cw/2 + 0, box_y + 15, 4, 16, rgb(70, 90, 140));
        vga_fill_circle(cx + cw/2 - 8, box_y + 35, 5, rgb(80, 100, 150));
        vga_fill_circle(cx + cw/2 + 2, box_y + 31, 5, rgb(70, 90, 140));
        vga_draw_string(cx + cw/2 - (int)slen_local(gui_text("No track loaded", "トラックが読み込まれていません")) * FONT_W / 2, box_y + 55, gui_text("No track loaded", "トラックが読み込まれていません"), rgb(120, 140, 180), 0xFFFFFFFF);

        /* Load button */
        int lb_x = cx + cw/2 - 60, lb_y = box_y + 96;
        bool hov_load = (ms && ms->x >= lb_x && ms->x < lb_x + 120 && ms->y >= lb_y && ms->y < lb_y + 28);
        vga_fill_rounded_rect(lb_x, lb_y, 120, 28, 6, hov_load ? rgb(80, 150, 255) : rgb(60, 120, 220));
        vga_draw_rounded_rect(lb_x, lb_y, 120, 28, 6, rgb(100, 160, 255));
        vga_draw_string(lb_x + (120 - (int)slen_local(gui_text("Open File Manager", "ファイルマネージャーを開く")) * FONT_W) / 2, lb_y + (28 - FONT_H) / 2, gui_text("Open File Manager", "ファイルマネージャーを開く"), rgb(220, 235, 255), 0xFFFFFFFF);
        return;
    }

    /* Album art placeholder */
    int art_x = cx + 16, art_y = cy + 44, art_size = 80;
    vga_fill_rounded_rect(art_x, art_y, art_size, art_size, 8, rgb(30, 40, 65));
    vga_draw_rounded_rect(art_x, art_y, art_size, art_size, 8, rgb(60, 90, 150));
    /* Vinyl record effect */
    vga_fill_circle(art_x + art_size/2, art_y + art_size/2, 30, rgb(22, 28, 44));
    vga_draw_circle(art_x + art_size/2, art_y + art_size/2, 30, rgb(28, 46, 84));
    vga_draw_circle(art_x + art_size/2, art_y + art_size/2, 22, rgb(40, 55, 90));
    vga_draw_circle(art_x + art_size/2, art_y + art_size/2, 14, rgb(35, 48, 78));
    vga_fill_circle(art_x + art_size/2, art_y + art_size/2, 5, rgb(60, 80, 130));
    /* Rotating indicator if playing */
    if (g_music.playing) {
        int angle_idx = (int)(g_music.position_ms / 50) % 8;
        int dx[] = {0, 8, 11, 8, 0, -8, -11, -8};
        int dy[] = {-11, -8, 0, 8, 11, 8, 0, -8};
        vga_fill_rect(art_x + art_size/2 + dx[angle_idx] - 1,
                      art_y + art_size/2 + dy[angle_idx] - 1, 3, 3, rgb(100, 180, 255));
    }

    /* Track info */
    int info_x = art_x + art_size + 12;
    int info_y = art_y;
    vga_draw_string(info_x, info_y, g_music.title, rgb(220, 235, 255), 0xFFFFFFFF);
    /* Truncate path */
    char short_path[40];
    int pl = 0;
    const char* pp = g_music.path;
    while (*pp) pp++;
    pp = g_music.path;
    while (*pp) { short_path[pl++] = *pp++; if (pl >= 38) break; }
    short_path[pl] = '\0';
    vga_draw_string(info_x, info_y + FONT_H + 8, short_path, rgb(72, 88, 116), 0xFFFFFFFF);

    /* Duration */
    char dur_buf[32];
    uint64_t pos_s = g_music.position_ms / 1000ULL;
    uint64_t dur_s = g_music.duration_ms / 1000ULL;
    int di = 0;
    /* pos mm:ss */
    uint64_t pm = pos_s / 60, ps = pos_s % 60;
    dur_buf[di++] = (char)('0' + pm / 10); dur_buf[di++] = (char)('0' + pm % 10);
    dur_buf[di++] = ':'; dur_buf[di++] = (char)('0' + ps / 10); dur_buf[di++] = (char)('0' + ps % 10);
    dur_buf[di++] = ' '; dur_buf[di++] = '/'; dur_buf[di++] = ' ';
    uint64_t dm = dur_s / 60, ds = dur_s % 60;
    dur_buf[di++] = (char)('0' + dm / 10); dur_buf[di++] = (char)('0' + dm % 10);
    dur_buf[di++] = ':'; dur_buf[di++] = (char)('0' + ds / 10); dur_buf[di++] = (char)('0' + ds % 10);
    dur_buf[di] = '\0';
    vga_draw_string(info_x, info_y + (FONT_H + 8) * 2, dur_buf, rgb(140, 170, 220), 0xFFFFFFFF);

    /* Volume bar */
    vga_draw_string(info_x, info_y + (FONT_H + 8) * 3, gui_text("Vol:", "音量"), rgb(72, 88, 116), 0xFFFFFFFF);
    int vol_bar_x = info_x + 32, vol_bar_y = info_y + (FONT_H + 8) * 3 + 2, vol_bar_w = 80, vol_bar_h = 12;
    vga_fill_rounded_rect(vol_bar_x, vol_bar_y, vol_bar_w, vol_bar_h, 3, rgb(30, 40, 65));
    int vol_fill = (int)(vol_bar_w * g_music.volume / 100);
    if (vol_fill > 0) {
        vga_fill_rounded_rect(vol_bar_x, vol_bar_y, vol_fill, vol_bar_h, 3, rgb(80, 180, 120));
    }
    vga_draw_rounded_rect(vol_bar_x, vol_bar_y, vol_bar_w, vol_bar_h, 3, rgb(28, 46, 84));

    /* Progress bar */
    int prog_y = art_y + art_size + 20;
    int prog_x = cx + 16, prog_w = cw - 32, prog_h = 10;
    vga_fill_rounded_rect(prog_x, prog_y, prog_w, prog_h, 4, rgb(28, 36, 56));
    uint64_t fill_w = (g_music.duration_ms > 0) ? ((uint64_t)prog_w * g_music.position_ms) / g_music.duration_ms : 0;
    if (fill_w > (uint64_t)prog_w) fill_w = (uint64_t)prog_w;
    if (fill_w > 0) {
        /* Gradient progress */
        for (int px = 0; px < (int)fill_w; px++) {
            float pt = (float)px / (float)prog_w;
            int pr = (int)(60 + pt * 80);
            int pg = (int)(140 + pt * 40);
            int pb = (int)(255 - pt * 60);
            vga_fill_rect(prog_x + px, prog_y, 1, prog_h, rgb((uint8_t)pr, (uint8_t)pg, (uint8_t)pb));
        }
        /* Thumb */
        vga_fill_circle(prog_x + (int)fill_w, prog_y + prog_h/2, 6, rgb(200, 230, 255));
    }
    vga_draw_rounded_rect(prog_x, prog_y, prog_w, prog_h, 4, rgb(28, 46, 84));

    /* Spectrum visualizer */
    int vis_y = prog_y + prog_h + 14;
    int vis_h = 50;
    int vis_x = cx + 16;
    int vis_w = cw - 32;
    vga_fill_rect(vis_x, vis_y, vis_w, vis_h, rgb(18, 24, 38));
    vga_draw_rect(vis_x, vis_y, vis_w, vis_h, rgb(40, 55, 90));
    mp3_update_visualizer(g_music.playing);
    int bar_count = 32;
    int bar_w2 = (vis_w - 2) / bar_count;
    for (int i = 0; i < bar_count; i++) {
        int bh2 = (int)mp3_vis_bars[i] * vis_h / 80;
        if (bh2 < 1) bh2 = 1;
        int bx = vis_x + 1 + i * bar_w2;
        int by2 = vis_y + vis_h - bh2;
        /* Color gradient: blue -> cyan -> green */
        float fi = (float)i / (float)bar_count;
        int br = (int)(40 + fi * 60);
        int bg2 = (int)(120 + fi * 100);
        int bb = (int)(255 - fi * 120);
        vga_fill_rect(bx, by2, bar_w2 - 1, bh2, rgb((uint8_t)br, (uint8_t)bg2, (uint8_t)bb));
        /* Peak dot */
        vga_fill_rect(bx, by2 - 2, bar_w2 - 1, 2, rgb(220, 240, 255));
    }

    /* Control buttons */
    int ctrl_y = vis_y + vis_h + 14;
    int btn_y_center = ctrl_y + 18;
    int center_x = cx + cw / 2;

    /* Prev button */
    int prev_x = center_x - 120;
    bool hov_prev = (ms && ms->x >= prev_x - 18 && ms->x < prev_x + 18 && ms->y >= btn_y_center - 18 && ms->y < btn_y_center + 18);
    vga_fill_circle(prev_x, btn_y_center, 16, hov_prev ? rgb(60, 80, 130) : rgb(35, 48, 78));
    vga_draw_circle(prev_x, btn_y_center, 16, rgb(60, 90, 150));
    /* << symbol */
    vga_fill_rect(prev_x - 8, btn_y_center - 6, 3, 12, rgb(160, 190, 230));
    vga_fill_rect(prev_x - 4, btn_y_center - 6, 8, 12, rgb(160, 190, 230));

    /* Play/Pause button (larger) */
    bool hov_play = (ms && ms->x >= center_x - 22 && ms->x < center_x + 22 && ms->y >= btn_y_center - 22 && ms->y < btn_y_center + 22);
    vga_fill_circle(center_x, btn_y_center, 20, hov_play ? rgb(80, 160, 255) : rgb(60, 130, 220));
    vga_draw_circle(center_x, btn_y_center, 20, rgb(120, 180, 255));
    if (g_music.playing) {
        /* Pause icon */
        vga_fill_rect(center_x - 7, btn_y_center - 8, 5, 16, rgb(240, 248, 255));
        vga_fill_rect(center_x + 2, btn_y_center - 8, 5, 16, rgb(240, 248, 255));
    } else {
        /* Play triangle */
        for (int py = 0; py < 14; py++) {
            int pw = py * 12 / 14;
            vga_fill_rect(center_x - 5 + py/2, btn_y_center - 7 + py, pw, 1, rgb(240, 248, 255));
        }
    }

    /* Next button */
    int next_x = center_x + 120;
    bool hov_next = (ms && ms->x >= next_x - 18 && ms->x < next_x + 18 && ms->y >= btn_y_center - 18 && ms->y < btn_y_center + 18);
    vga_fill_circle(next_x, btn_y_center, 16, hov_next ? rgb(60, 80, 130) : rgb(35, 48, 78));
    vga_draw_circle(next_x, btn_y_center, 16, rgb(60, 90, 150));
    /* >> symbol */
    vga_fill_rect(next_x - 4, btn_y_center - 6, 8, 12, rgb(160, 190, 230));
    vga_fill_rect(next_x + 5, btn_y_center - 6, 3, 12, rgb(160, 190, 230));

    /* Stop button */
    int stop_x = center_x - 60;
    bool hov_stop = (ms && ms->x >= stop_x - 14 && ms->x < stop_x + 14 && ms->y >= btn_y_center - 14 && ms->y < btn_y_center + 14);
    vga_fill_circle(stop_x, btn_y_center, 12, hov_stop ? rgb(220, 80, 80) : rgb(180, 60, 60));
    vga_draw_circle(stop_x, btn_y_center, 12, rgb(240, 100, 100));
    vga_fill_rect(stop_x - 5, btn_y_center - 5, 10, 10, rgb(240, 248, 255));

    /* Vol- button */
    int volm_x = center_x + 60;
    bool hov_volm = (ms && ms->x >= volm_x - 14 && ms->x < volm_x + 14 && ms->y >= btn_y_center - 14 && ms->y < btn_y_center + 14);
    vga_fill_circle(volm_x, btn_y_center, 12, hov_volm ? rgb(80, 100, 160) : rgb(50, 70, 120));
    vga_draw_circle(volm_x, btn_y_center, 12, rgb(80, 110, 170));
    vga_fill_rect(volm_x - 5, btn_y_center - 1, 10, 2, rgb(200, 220, 255));
    vga_fill_rect(volm_x + 3, btn_y_center - 4, 2, 8, rgb(200, 220, 255));

    /* Handle clicks */
    if (ms && ms->left_click) {
        if (hov_play) g_music.playing = !g_music.playing;
        else if (hov_stop) { g_music.playing = FALSE; g_music.position_ms = 0; }
        else if (hov_prev && g_music.position_ms > 3000) { g_music.position_ms = 0; }
        else if (hov_next && g_music.duration_ms > 0) { g_music.position_ms = g_music.duration_ms; }
        else if (hov_volm && g_music.volume < 100) { g_music.volume += 5; }
        /* Click on progress bar to seek */
        else if (ms->x >= prog_x && ms->x < prog_x + prog_w &&
                 ms->y >= prog_y - 4 && ms->y < prog_y + prog_h + 4 && g_music.duration_ms > 0) {
            g_music.position_ms = (uint64_t)(ms->x - prog_x) * g_music.duration_ms / (uint64_t)prog_w;
        }
    }

    /* Advance position */
    if (g_music.playing && g_music.duration_ms > 0 && g_music.position_ms < g_music.duration_ms) {
        g_music.position_ms += 16;
        if (g_music.position_ms >= g_music.duration_ms) {
            g_music.position_ms = g_music.duration_ms;
            g_music.playing = FALSE;
        }
    }
}

void calc_press(const char* key) {
    if (key[0] >= '0' && key[0] <= '9') {
        if (calc_clear_on_input) {
            calc_display[0] = key[0];
            calc_display[1] = '\0';
            calc_clear_on_input = 0;
        } else {
            int len = strlen(calc_display);
            if (len < 15 && !(len == 1 && calc_display[0] == '0')) {
                calc_display[len] = key[0];
                calc_display[len + 1] = '\0';
            }
        }
    } else if (key[0] == '.') {
        // Decimal point - only add if not already present
        if (calc_clear_on_input) {
            scopy_local(calc_display, "0.", sizeof(calc_display));
            calc_clear_on_input = 0;
        } else {
            bool has_dot = FALSE;
            for (int i = 0; calc_display[i]; i++) {
                if (calc_display[i] == '.') { has_dot = TRUE; break; }
            }
            if (!has_dot) {
                int len = strlen(calc_display);
                if (len < 15) {
                    calc_display[len] = '.';
                    calc_display[len + 1] = '\0';
                }
            }
        }
    } else if (key[0] == 'C') {
        scopy_local(calc_display, "0", sizeof(calc_display));
        calc_acc = 0;
        calc_op = 0;
    } else if (key[0] == 'B') {  // Backspace
        int len = strlen(calc_display);
        if (len > 1) {
            calc_display[len - 1] = '\0';
        } else {
            scopy_local(calc_display, "0", sizeof(calc_display));
        }
    } else if (key[0] == 'N') {  // Negate
        if (calc_display[0] == '-') {
            // Remove minus sign by shifting
            for (int i = 0; calc_display[i]; i++) {
                calc_display[i] = calc_display[i + 1];
            }
        } else if (strcmp(calc_display, "0") != 0) {
            // Add minus sign by shifting right
            int len = strlen(calc_display);
            for (int i = len; i >= 0; i--) {
                calc_display[i + 1] = calc_display[i];
            }
            calc_display[0] = '-';
        }
    } else if (key[0] == '=' && calc_op) {
        double val = 0;
        // Parse current display value (supports decimal)
        bool is_float = FALSE;
        double frac = 0.1;
        for (int i = 0; calc_display[i]; i++) {
            if (calc_display[i] == '-') continue;
            if (calc_display[i] == '.') {
                is_float = TRUE;
            } else if (calc_display[i] >= '0' && calc_display[i] <= '9') {
                if (!is_float) {
                    val = val * 10 + (calc_display[i] - '0');
                } else {
                    val = val + (calc_display[i] - '0') * frac;
                    frac *= 0.1;
                }
            }
        }
        if (calc_display[0] == '-') val = -val;
        
        double result = calc_acc;
        switch (calc_op) {
            case '+': result += val; break;
            case '-': result -= val; break;
            case '*': result *= val; break;
            case '/': if (val != 0) result /= val; break;
        }
        
        // Convert result to string safely
        {
            char out[sizeof(calc_display)];
            out[0] = '\0';

            if (result < 0) {
                out[0] = '-';
                out[1] = '\0';
                result = -result;
            }

            snprintf(out + strlen(out), sizeof(out) - strlen(out), "%.6f", result);

            int end = (int)strlen(out);
            while (end > 0 && out[end - 1] == '0') {
                out[end - 1] = '\0';
                end--;
            }
            if (end > 0 && out[end - 1] == '.') {
                out[end - 1] = '\0';
            }

            if (out[0] == '\0' || strcmp(out, "-") == 0) {
                scopy_local(calc_display, "0", sizeof(calc_display));
            } else {
                scopy_local(calc_display, out, sizeof(calc_display));
            }
        }
        calc_op = 0;
        calc_clear_on_input = 1;
    } else if (key[0] == '+' || key[0] == '-' || key[0] == '*' || key[0] == '/') {
        // Parse current value
        double val = 0;
        bool is_float = FALSE;
        double frac = 0.1;
        for (int i = 0; calc_display[i]; i++) {
            if (calc_display[i] == '-') continue;
            if (calc_display[i] == '.') {
                is_float = TRUE;
            } else if (calc_display[i] >= '0' && calc_display[i] <= '9') {
                if (!is_float) {
                    val = val * 10 + (calc_display[i] - '0');
                } else {
                    val = val + (calc_display[i] - '0') * frac;
                    frac *= 0.1;
                }
            }
        }
        if (calc_display[0] == '-') val = -val;
        calc_acc = val;
        calc_op = key[0];
        calc_clear_on_input = 1;
    }
}


/* File manager click handling */
extern const char* fs_read_file_at(const char* path, const char* name);

static void fm_path_join_simple(char* out, size_t out_sz, const char* dir, const char* leaf) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';

    const char* d = (dir && dir[0]) ? dir : "/";
    const char* l = (leaf && leaf[0]) ? leaf : "";

    size_t pos = 0;
    if (d[0] != '/' || d[1] != '\0') {
        while (d[pos] && pos + 1 < out_sz) {
            out[pos] = d[pos];
            pos++;
        }
        if (pos + 1 < out_sz) out[pos++] = '/';
    } else {
        if (pos + 1 < out_sz) out[pos++] = '/';
    }

    for (size_t i = 0; l[i] && pos + 1 < out_sz; ++i) {
        out[pos++] = l[i];
    }
    out[pos] = '\0';
}

static void fm_open_text_editor_simple(const char* fullpath, const fs_entry_t* entry) {
    (void)entry;
    if (!fullpath || !fullpath[0]) return;

    int ex = gui_find_window(WIN_TEXT_EDITOR);
    if (ex < 0) {
        gui_open_window(WIN_TEXT_EDITOR, gui_text("Text Editor", "テキストエディター"), 140, 90, 900, 650);
        ex = gui_find_window(WIN_TEXT_EDITOR);
    }
    if (ex < 0) return;

    window_t* ed = &windows[ex];
    memset(ed->filename, 0, sizeof(ed->filename));
    scopy_local(ed->filename, fullpath, sizeof(ed->filename));
    ed->text_buf[0] = '\0';

    const char* text = fs_read_file_at(NULL, fullpath);
    if (text) {
        scopy_local(ed->text_buf, text, TEXT_BUF_SIZE);
    }

    ed->text_cursor = (int)strlen(ed->text_buf);
    ed->scroll_y = 0;
    ed->scroll_x = 0;
    ed->text_modified = false;
    gui_restore_window(ex);
    gui_focus_window(ex);
}


void handle_file_manager_click_legacy(int idx, int mx, int my) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (!w || w->kind != WIN_FILE_MGR) return;

    const char* path = fm_effective_path_local(w);
    fm_row_item_local_t rows[FS_MAX_ENTRIES + 1];
    int total = fm_build_rows_local(w, rows, FS_MAX_ENTRIES + 1);
    int mode = (w->fm_view_mode < 0 || w->fm_view_mode > 2) ? 0 : w->fm_view_mode;

    int cx = w->x + 6;
    int cy = w->y + 34;
    int cw = w->w - 12;
    int path_x = cx + 12;
    int path_w = cw - 24;
    int toolbar_y = cy + 130;
    int search_y = cy + 168;
    int list_y = cy + 210;
    int list_h = cy + (w->h - 40) - 58 - list_y;
    if (list_h < 80) list_h = 80;
    int row_h = (mode == 1) ? 44 : (mode == 2) ? 34 : 30;
    int visible_rows = list_h / row_h;
    if (visible_rows < 1) visible_rows = 1;

    if (mouse.wheel != 0) {
        int step = row_h;
        w->fm_scroll -= mouse.wheel * step;
        if (w->fm_scroll < 0) w->fm_scroll = 0;
    }

    if (my >= toolbar_y && my < toolbar_y + 28) {
        int bx = cx + 12;
        struct { const char* label; } btns[] = {
            { gui_text("Up", "上へ") },
            { gui_text("New File", "新規ファイル") },
            { gui_text("New Folder", "新規フォルダ") },
            { gui_text("Rename", "名前変更") },
            { gui_text("Copy To", "コピー先") },
            { gui_text("Move To", "移動先") },
            { gui_text("Search", "検索") },
            { gui_text("Sort", "並べ替え") },
            { gui_text("View", "表示") },
            { gui_text("Info", "情報") },
            { gui_text("Refresh", "更新") },
        };
        for (int i = 0; i < 11; ++i) {
            int bw = (int)slen_local(btns[i].label) * FONT_W + 26;
            if (bw < 54) bw = 54;
            if (mx >= bx && mx < bx + bw) {
                if (i == 0) {
                    if (!path_is_root(path)) {
                        char parent[FS_MAX_PATH];
                        fm_parent_path_local(path, parent, sizeof(parent));
                        scopy_local(w->fm_path, parent, sizeof(w->fm_path));
                        w->fm_selected = -1;
                        w->fm_scroll = 0;
                    }
                } else if (i == 1) {
                    if (gui_windows_create_item(path, false)) {
                        w->fm_scroll = 0;
                    }
                } else if (i == 2) {
                    if (gui_windows_create_item(path, true)) {
                        w->fm_scroll = 0;
                    }
                } else if (i == 3) {
                    fs_entry_t* entry = NULL; bool parent = FALSE;
                    if (fm_get_row_entry_local(w, w->fm_selected, &entry, &parent) && entry && !parent) {
                        w->fm_action = 3;
                        scopy_local(w->fm_input, entry->name, sizeof(w->fm_input));
                    }
                } else if (i == 4) {
                    fs_entry_t* entry = NULL; bool parent = FALSE;
                    if (fm_get_row_entry_local(w, w->fm_selected, &entry, &parent) && entry && !parent) {
                        char full[FS_MAX_PATH];
                        fm_selected_path_local(w, full, sizeof(full), NULL, NULL);
                        w->fm_action = 4;
                        scopy_local(w->fm_input, full, sizeof(w->fm_input));
                    }
                } else if (i == 5) {
                    fs_entry_t* entry = NULL; bool parent = FALSE;
                    if (fm_get_row_entry_local(w, w->fm_selected, &entry, &parent) && entry && !parent) {
                        char full[FS_MAX_PATH];
                        fm_selected_path_local(w, full, sizeof(full), NULL, NULL);
                        w->fm_action = 5;
                        scopy_local(w->fm_input, full, sizeof(w->fm_input));
                    }
                } else if (i == 6) {
                    w->fm_search_active = !w->fm_search_active;
                    if (w->fm_search_active) w->fm_selected = -1;
                } else if (i == 7) {
                    w->fm_sort_by = (w->fm_sort_by + 1) % 4;
                    if (w->fm_sort_by == 0) w->fm_sort_reverse = FALSE;
                } else if (i == 8) {
                    w->fm_view_mode = (w->fm_view_mode + 1) % 3;
                    if (w->fm_view_mode < 0) w->fm_view_mode = 0;
                } else if (i == 9) {
                    fs_entry_t* entry = NULL; bool parent = FALSE;
                    if (fm_get_row_entry_local(w, w->fm_selected, &entry, &parent) && entry && !parent) {
                        char full[FS_MAX_PATH];
                        char size_buf[24];
                        char mod_buf[32];
                        fm_selected_path_local(w, full, sizeof(full), NULL, NULL);
                        format_size_bytes(entry->size, size_buf, sizeof(size_buf) - 1);
                        fm_format_time(entry->modified_time, mod_buf, sizeof(mod_buf));
                        gui_notify_simple(full);
                        gui_notify_simple(size_buf);
                        gui_notify_simple(mod_buf);
                    }
                } else if (i == 10) {
                    fm_refresh_after_mutation_local(w, NULL);
                }
                return;
            }
            bx += bw + 8;
        }
    }

    if (my >= search_y && my < search_y + 30 && mx >= path_x && mx < path_x + path_w) {
        w->fm_search_active = TRUE;
        return;
    }

    if (my < list_y || my >= list_y + list_h) return;

    if (mode == 1) {
        int cell_w = 170;
        int cell_h = 44;
        int cols = path_w / cell_w;
        if (cols < 1) cols = 1;
        int row = (my - list_y) / cell_h;
        int col = (mx - path_x) / cell_w;
        if (col < 0 || col >= cols) return;
        int index = w->fm_scroll * cols + row * cols + col;
        if (index < 0 || index >= total) return;
        if (rows[index].parent) {
            if (!path_is_root(path)) {
                char parent[FS_MAX_PATH];
                fm_parent_path_local(path, parent, sizeof(parent));
                scopy_local(w->fm_path, parent, sizeof(w->fm_path));
                w->fm_selected = -1;
                w->fm_scroll = 0;
            }
            return;
        }
        fs_entry_t* e = rows[index].entry;
        if (!e) return;
        char full[FS_MAX_PATH];
        fm_path_join_simple(full, sizeof(full), path, e->name);
        w->fm_selected = index;
        if (e->is_dir) {
            scopy_local(w->fm_path, full, sizeof(w->fm_path));
            w->fm_selected = -1;
            w->fm_scroll = 0;
        } else {
            fm_open_text_editor_simple(full, e);
        }
        return;
    }

    int row = (my - list_y) / row_h;
    int index = w->fm_scroll + row;
    if (index < 0 || index >= total) return;
    if (rows[index].parent) {
        if (!path_is_root(path)) {
            char parent[FS_MAX_PATH];
            fm_parent_path_local(path, parent, sizeof(parent));
            scopy_local(w->fm_path, parent, sizeof(w->fm_path));
            w->fm_selected = -1;
            w->fm_scroll = 0;
        }
        return;
    }

    fs_entry_t* e = rows[index].entry;
    if (!e) return;
    char full[FS_MAX_PATH];
    fm_path_join_simple(full, sizeof(full), path, e->name);
    w->fm_selected = index;
    if (e->is_dir) {
        scopy_local(w->fm_path, full, sizeof(w->fm_path));
        w->fm_selected = -1;
        w->fm_scroll = 0;
    } else {
        fm_open_text_editor_simple(full, e);
    }
}
void WEAK draw_python_ide_app(int idx) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    python_ide_set_geometry(w->x, w->y, w->w, w->h);
    python_ide_draw();
}
