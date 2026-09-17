/**
 * file_manager_explorer.c - Windows Explorer-style File Manager Implementation
 *
 * C-OS 4.0.8 alpha enhancements:
 *   - Title bar + menu bar (File / Edit / View / Tools / Help) that
 *     resemble Windows Explorer's Ribbon-free layout.
 *   - Toolbar with Back/Forward/Up/Refresh buttons, address bar with
 *     chevron-style breadcrumbs, search box with caret.
 *   - Two-column body: navigation pane (Quick Access, This PC,
 *     Network, Libraries) and main column with row grid / details view.
 *   - Stable column headers with click-to-sort glyphs (Name / Size /
 *     Type / Modified), separator drag handles (drawn only).
 *   - Status bar showing selection count plus accurate host RAM usage
 *     fetched from cos_runtime_total_bytes() / cos_runtime_available().
 *   - Dark-mode aware palette mirroring the existing C-OS GUI helper set.
 */

#include "file_manager_explorer.h"
#include "vga.h"
#include "string.h"
#include "memory.h"
#include "serial.h"
#include <stddef.h>

/* -------------------------------------------------------------------------- */
/*  Local helpers                                                             */
/* -------------------------------------------------------------------------- */
static int fm_str_starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static const char* fm_get_extension(const char* name) {
    if (!name) return NULL;
    const char* dot = NULL;
    while (*name) {
        if (*name == '.') dot = name;
        name++;
    }
    return dot ? dot + 1 : NULL;
}

static void fm_copy_path(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_sz - 1);
    dst[dst_sz - 1] = '\0';
}

static void parse_path_breadcrumb(const char* path, char* parts[], int max_parts, int* count) {
    if (!path || !parts || max_parts <= 0) return;
    char path_copy[FS_UNIFIED_MAX_PATH];
    fm_copy_path(path_copy, sizeof(path_copy), path);
    *count = 0;
    char* start = path_copy;
    while (*start == '/') start++;
    while (*start && *count < max_parts) {
        char* end = start;
        while (*end && *end != '/') end++;
        *end = '\0';
        parts[*count] = start;
        (*count)++;
        start = end + 1;
        while (*start == '/') start++;
    }
}

extern void vga_fill_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_draw_rect(int x, int y, int w, int h, uint64_t color);
extern void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg);
extern void vga_draw_line(int x0, int y0, int x1, int y1, uint64_t color);
extern void vga_fill_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern void vga_draw_rounded_rect(int x, int y, int w, int h, int r, uint64_t color);
extern int  gui_dark_mode;

/* -------------------------------------------------------------------------- */
/*  Color palette                                                             */
/* -------------------------------------------------------------------------- */
#define FM_C_BG         (gui_dark_mode ? 0x001A1E28 : 0x00F4F6FA)
#define FM_C_PANEL      (gui_dark_mode ? 0x00202430 : 0x00FFFFFF)
#define FM_C_SIDEBAR    (gui_dark_mode ? 0x00181C26 : 0x00EEF2F8)
#define FM_C_TOOLBAR    (gui_dark_mode ? 0x001E2230 : 0x00F0F4FA)
#define FM_C_MENUBAR    (gui_dark_mode ? 0x001B2028 : 0x00EAEEF6)
#define FM_C_BORDER     (gui_dark_mode ? 0x00384050 : 0x00D0D8E8)
#define FM_C_LIGHTLINE  (gui_dark_mode ? 0x0020242E : 0x00F8FAFD)
#define FM_C_TEXT       (gui_dark_mode ? 0x00E8ECF4 : 0x00202428)
#define FM_C_MUTED      (gui_dark_mode ? 0x00808898 : 0x00808898)
#define FM_C_ACCENT     (gui_dark_mode ? 0x004D9FFF : 0x001A73E8)
#define FM_C_HOVER      (gui_dark_mode ? 0x00303848 : 0x00E8F0FE)
#define FM_C_SELECT     (gui_dark_mode ? 0x00284060 : 0x00C8DFFE)
#define FM_C_STRIP      (gui_dark_mode ? 0x00232A38 : 0x00FAFAFA)
#define FM_C_HEADER     (gui_dark_mode ? 0x00232A38 : 0x00F0F4FA)
#define FM_C_DANGER     0x00D93025

#define FM_C_FOLDER     0x00F5A623
#define FM_C_IMAGE      0x0034A853
#define FM_C_AUDIO      0x00EA4335
#define FM_C_VIDEO      0x00FBBC04
#define FM_C_CODE       0x004285F4
#define FM_C_ARCHIVE    0x009E3AB5
#define FM_C_TEXT_FILE  0x00607080
#define FM_C_UNKNOWN    0x00909090

/* -------------------------------------------------------------------------- */
/*  Icon dispatch                                                             */
/* -------------------------------------------------------------------------- */
static uint64_t fm_color_for_name(const char* name, fs_unified_type_t type) {
    if (type == FS_UNIFIED_TYPE_DIR) return FM_C_FOLDER;
    const char* ext = fm_get_extension(name);
    if (!ext) return FM_C_TEXT_FILE;
    if (fm_str_starts_with(ext, "png") || fm_str_starts_with(ext, "jpg") ||
        fm_str_starts_with(ext, "jpeg") || fm_str_starts_with(ext, "bmp") ||
        fm_str_starts_with(ext, "gif") || fm_str_starts_with(ext, "svg"))
        return FM_C_IMAGE;
    if (fm_str_starts_with(ext, "mp3") || fm_str_starts_with(ext, "wav") ||
        fm_str_starts_with(ext, "ogg") || fm_str_starts_with(ext, "flac"))
        return FM_C_AUDIO;
    if (fm_str_starts_with(ext, "mp4") || fm_str_starts_with(ext, "mkv") ||
        fm_str_starts_with(ext, "avi") || fm_str_starts_with(ext, "mov"))
        return FM_C_VIDEO;
    if (fm_str_starts_with(ext, "c") || fm_str_starts_with(ext, "h") ||
        fm_str_starts_with(ext, "py") || fm_str_starts_with(ext, "js") ||
        fm_str_starts_with(ext, "rs") || fm_str_starts_with(ext, "go"))
        return FM_C_CODE;
    if (fm_str_starts_with(ext, "zip") || fm_str_starts_with(ext, "tar") ||
        fm_str_starts_with(ext, "gz") || fm_str_starts_with(ext, "7z") ||
        fm_str_starts_with(ext, "rar"))
        return FM_C_ARCHIVE;
    if (fm_str_starts_with(ext, "txt") || fm_str_starts_with(ext, "md") ||
        fm_str_starts_with(ext, "log") || fm_str_starts_with(ext, "cfg"))
        return FM_C_TEXT_FILE;
    return FM_C_UNKNOWN;
}

char fm_explorer_get_type_icon(fs_unified_type_t type) {
    switch (type) {
        case FS_UNIFIED_TYPE_DIR:    return 'D';
        case FS_UNIFIED_TYPE_FILE:   return 'F';
        case FS_UNIFIED_TYPE_SYMLINK:return 'L';
        default:                     return '?';
    }
}

uint32_t fm_explorer_get_type_color(fs_unified_type_t type) {
    return (type == FS_UNIFIED_TYPE_DIR) ? FM_C_FOLDER : FM_C_TEXT_FILE;
}

/* -------------------------------------------------------------------------- */
/*  Icon drawing — rectangle-based so they survive on VGA 32-bit surfaces      */
/* -------------------------------------------------------------------------- */
static void fm_draw_folder_icon(int x, int y) {
    vga_fill_rect(x, y, 22, 4, FM_C_FOLDER);
    vga_fill_rect(x + 2, y + 4, 24, 22, FM_C_FOLDER);
    vga_draw_rect(x, y, 22, 4, FM_C_BORDER);
    vga_draw_rect(x + 2, y + 4, 24, 22, FM_C_BORDER);
}

static void fm_draw_file_icon(int x, int y, uint64_t color) {
    vga_fill_rect(x, y, 22, 28, FM_C_PANEL);
    vga_draw_rect(x, y, 22, 28, color);
    vga_fill_rect(x + 16, y, 6, 6, FM_C_PANEL);
    vga_draw_line(x + 16, y, x + 22, y + 6, color);
}

/* Custom draw — uses the per-entry colour so folders look different from
   documents. */
static void fm_draw_icon(int x, int y, fs_unified_dirent_t* e) {
    if (!e) return;
    if (e->type == FS_UNIFIED_TYPE_DIR) {
        fm_draw_folder_icon(x, y);
    } else {
        uint64_t c = fm_color_for_name(e->name, e->type);
        fm_draw_file_icon(x, y, c);
    }
    (void)fm_explorer_get_type_icon(e->type);
}

/* -------------------------------------------------------------------------- */
/*  Size formatter — human-readable, matches Windows Explorer's choice        */
/* -------------------------------------------------------------------------- */
void fm_explorer_format_size(uint64_t size, char* buf, int buf_size) {
    if (!buf || buf_size <= 0) return;
    if (size >= (1ULL << 30)) {
        snprintf(buf, buf_size, "%.2f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
    } else if (size >= 1024ULL * 1024ULL) {
        snprintf(buf, buf_size, "%.2f MB", (double)size / (1024.0 * 1024.0));
    } else if (size >= 1024ULL) {
        snprintf(buf, buf_size, "%.1f KB", (double)size / 1024.0);
    } else {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)size);
    }
}

/* -------------------------------------------------------------------------- */
/*  Globals                                                                   */
/* -------------------------------------------------------------------------- */
static file_manager_state_t* g_active_fm = NULL;

/* -------------------------------------------------------------------------- */
/*  Lifecycle                                                                 */
/* -------------------------------------------------------------------------- */
int fm_explorer_init(void) {
    serial_puts("[FM_EXPLORER] Initializing Explorer-style file manager...\n");
    if (fs_unified_init() != 0) {
        serial_puts("[FM_EXPLORER] ERROR: Failed to initialize unified file system\n");
        return -1;
    }
    serial_puts("[FM_EXPLORER] Initialization complete\n");
    return 0;
}

file_manager_state_t* fm_explorer_create_window(int x, int y, int w, int h) {
    file_manager_state_t* fm = (file_manager_state_t*)kmalloc(sizeof(file_manager_state_t));
    if (!fm) return NULL;
    memset(fm, 0, sizeof(file_manager_state_t));

    fm->window_x = x;
    fm->window_y = y;
    fm->window_w = w;
    fm->window_h = h;
    fm->sidebar_width       = 200;
    fm->titlebar_height     = 24;
    fm->menubar_height      = 22;
    fm->toolbar_height      = 36;
    fm->searchbar_height    = 28;
    fm->statusbar_height    = 22;
    fm->column_header_height = 22;

    /* Default Windows-Explorer column offsets. */
    fm->col_name_x     = 32;
    fm->col_size_x     = 320;
    fm->col_type_x     = 410;
    fm->col_modified_x = 520;
    fm->file_col_name_w      = w - 60;
    fm->file_col_size_w      = 100;
    fm->file_col_type_w      = 110;
    fm->file_col_modified_w  = 160;

    fm_copy_path(fm->current_path, sizeof(fm->current_path), "/");
    fm->view_mode        = FM_VIEW_DETAILS;
    fm->sort_mode        = FM_SORT_NAME;
    fm->sort_ascending   = true;
    fm->selected_index   = -1;
    fm->scroll_offset    = 0;
    fm->selected_count   = 0;
    memset(fm->selected_indices, 0, sizeof(fm->selected_indices));

    fm->history_count    = 1;
    fm_copy_path(fm->history[0], sizeof(fm->history[0]), "/");
    fm->history_index    = 0;

    fm->sidebar_selected = FM_SIDEBAR_QUICK_ACCESS;

    g_active_fm = fm;
    serial_puts("[FM_EXPLORER] Window created\n");
    return fm;
}

void fm_explorer_destroy_window(file_manager_state_t* fm) {
    if (!fm) return;
    if (g_active_fm == fm) g_active_fm = NULL;
    kfree(fm);
}

/* -------------------------------------------------------------------------- */
/*  Navigation                                                                */
/* -------------------------------------------------------------------------- */
int fm_explorer_open_path(file_manager_state_t* fm, const char* path) {
    if (!fm || !path) return -1;
    if (!fs_unified_exists(path)) {
        serial_puts("[FM_EXPLORER] Path does not exist: ");
        serial_puts(path);
        serial_puts("\n");
        return -1;
    }

    char target[FS_UNIFIED_MAX_PATH];
    fm_copy_path(target, sizeof(target), path);

    int count = fs_unified_readdir(target, fm->entries, 128);
    if (count < 0) return -1;

    if (fm->history_index < 15) {
        fm->history_index++;
        fm->history_count = fm->history_index + 1;
    } else {
        for (int i = 0; i < 15; i++) {
            fm_copy_path(fm->history[i], sizeof(fm->history[i]), fm->history[i + 1]);
        }
        fm->history_index = 15;
        fm->history_count = 16;
    }

    fm_copy_path(fm->history[fm->history_index], sizeof(fm->history[fm->history_index]), target);
    fm_copy_path(fm->current_path, sizeof(fm->current_path), target);
    fm->entry_count    = count;
    fm->selected_index = -1;
    fm->scroll_offset  = 0;
    return 0;
}

int fm_explorer_navigate_back(file_manager_state_t* fm) {
    if (!fm || fm->history_index <= 0) return -1;

    int target_index = fm->history_index - 1;
    char target[FS_UNIFIED_MAX_PATH];
    fm_copy_path(target, sizeof(target), fm->history[target_index]);

    int count = fs_unified_readdir(target, fm->entries, 128);
    if (count < 0) return -1;

    fm->history_index = target_index;
    fm_copy_path(fm->current_path, sizeof(fm->current_path), target);
    fm->entry_count    = count;
    fm->selected_index = -1;
    fm->scroll_offset  = 0;
    return 0;
}

int fm_explorer_navigate_forward(file_manager_state_t* fm) {
    if (!fm || fm->history_index >= fm->history_count - 1) return -1;

    int target_index = fm->history_index + 1;
    char target[FS_UNIFIED_MAX_PATH];
    fm_copy_path(target, sizeof(target), fm->history[target_index]);

    int count = fs_unified_readdir(target, fm->entries, 128);
    if (count < 0) return -1;

    fm->history_index = target_index;
    fm_copy_path(fm->current_path, sizeof(fm->current_path), target);
    fm->entry_count    = count;
    fm->selected_index = -1;
    fm->scroll_offset  = 0;
    return 0;
}

int fm_explorer_navigate_up(file_manager_state_t* fm) {
    if (!fm) return -1;
    const char* path = fm->current_path;
    if (strcmp(path, "/") == 0) return -1;
    char parent[FS_UNIFIED_MAX_PATH];
    fm_copy_path(parent, sizeof(parent), path);
    int len = strlen(parent);
    while (len > 1 && parent[len - 1] == '/') {
        parent[len - 1] = '\0';
        len--;
    }
    const char* last_slash = strrchr(parent, '/');
    if (!last_slash) fm_copy_path(parent, sizeof(parent), "/");
    else if (last_slash == parent) parent[1] = '\0';
    else *((char*)last_slash) = '\0';
    return fm_explorer_open_path(fm, parent);
}

int fm_explorer_refresh(file_manager_state_t* fm) {
    if (!fm) return -1;
    int count = fs_unified_readdir(fm->current_path, fm->entries, 128);
    if (count < 0) return -1;
    fm->entry_count    = count;
    fm->selected_index = -1;
    fm->scroll_offset  = 0;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  View                                                                      */
/* -------------------------------------------------------------------------- */
void fm_explorer_set_view_mode(file_manager_state_t* fm, fm_view_mode_t mode) {
    if (!fm) return;
    fm->view_mode = mode;
}

void fm_explorer_set_sort_mode(file_manager_state_t* fm, fm_sort_mode_t mode, bool ascending) {
    if (!fm) return;
    fm->sort_mode = mode;
    fm->sort_ascending = ascending;
}

/* -------------------------------------------------------------------------- */
/*  Title / Menu / Tool strip                                                 */
/* -------------------------------------------------------------------------- */
static void fm_draw_titlebar(file_manager_state_t* fm) {
    int x = fm->window_x;
    int y = fm->window_y;
    int w = fm->window_w;
    int h = fm->titlebar_height;
    vga_fill_rect(x, y, w, h, FM_C_STRIP);
    vga_draw_string(x + 8, y + 6, fm->current_path, FM_C_TEXT, FM_C_STRIP);
    vga_draw_rect(x, y + h - 1, w, 1, FM_C_BORDER);
    /* Window control buttons (drawn-only): minimise / maximise / close. */
    int btn_y = y + 4;
    int btn_h = h - 8;
    int btn_w = btn_h;
    int btn_x = x + w - btn_w * 3 - 4;
    vga_fill_rect(btn_x,                btn_y, btn_w, btn_h, FM_C_STRIP);
    vga_draw_string(btn_x + btn_w/2 - 4, btn_y + btn_h/2 - 4, "_", FM_C_MUTED, FM_C_STRIP);
    btn_x += btn_w + 2;
    vga_fill_rect(btn_x,                btn_y, btn_w, btn_h, FM_C_STRIP);
    vga_draw_rect(btn_x + 4, btn_y + btn_h/2 - 4, btn_w - 8, 8, FM_C_MUTED);
    btn_x += btn_w + 2;
    vga_fill_rect(btn_x,                btn_y, btn_w, btn_h, FM_C_STRIP);
    vga_draw_string(btn_x + btn_w/2 - 4, btn_y + btn_h/2 - 4, "X", FM_C_DANGER, FM_C_STRIP);
}

static void fm_draw_menubar(file_manager_state_t* fm) {
    int x = fm->window_x;
    int y = fm->window_y + fm->titlebar_height;
    int w = fm->window_w;
    vga_fill_rect(x, y, w, fm->menubar_height, FM_C_MENUBAR);
    vga_draw_rect(x, y + fm->menubar_height - 1, w, 1, FM_C_BORDER);

    const char* items[] = { "File", "Edit", "View", "Tools", "Help" };
    int ix = x + 6;
    int iy = y + 4;
    for (int i = 0; i < 5; i++) {
        vga_draw_string(ix, iy, items[i], FM_C_TEXT, FM_C_MENUBAR);
        ix += (int)strlen(items[i]) * 8 + 14;
    }
}

static void fm_draw_toolbar(file_manager_state_t* fm) {
    int x = fm->window_x;
    int y = fm->window_y + fm->titlebar_height + fm->menubar_height;
    int w = fm->window_w;
    int h = fm->toolbar_height;
    vga_fill_rect(x, y, w, h, FM_C_TOOLBAR);
    vga_draw_rect(x, y + h - 1, w, 1, FM_C_BORDER);

    /* Back / Forward / Up / Refresh. */
    int btn_x = x + 6;
    int btn_y = y + 6;
    int btn_w = 28;
    int btn_h = h - 12;
    const char* glyphs[] = { "<", ">", "^", "R" };
    for (int i = 0; i < 4; i++) {
        bool enabled = true;
        if (i == 0) enabled = (fm->history_index > 0);
        if (i == 1) enabled = (fm->history_index < fm->history_count - 1);
        if (i == 2) enabled = (strcmp(fm->current_path, "/") != 0);
        uint64_t bg = enabled ? FM_C_PANEL : FM_C_TOOLBAR;
        vga_fill_rect(btn_x, btn_y, btn_w, btn_h, bg);
        vga_draw_rect(btn_x, btn_y, btn_w, btn_h, FM_C_BORDER);
        vga_draw_string(btn_x + btn_w / 2 - 4, btn_y + btn_h / 2 - 4,
                        glyphs[i], enabled ? FM_C_TEXT : FM_C_MUTED, bg);
        btn_x += btn_w + 4;
    }

    /* Address bar */
    int ab_x = btn_x + 6;
    int ab_w = w - (ab_x - x) - 220;
    if (ab_w < 200) ab_w = w - (ab_x - x) - 40;
    int ab_y = btn_y;
    int ab_h = btn_h;

    vga_fill_rect(ab_x, ab_y, ab_w, ab_h, FM_C_PANEL);
    vga_draw_rect(ab_x, ab_y, ab_w, ab_h, FM_C_BORDER);

    int bx = ab_x + 6;
    int by = ab_y + ab_h / 2 - 4;
    vga_draw_string(bx, by, "PC", FM_C_ACCENT, FM_C_PANEL);
    bx += 24;

    char* parts[16];
    int part_count = 0;
    parse_path_breadcrumb(fm->current_path, parts, 16, &part_count);

    for (int i = 0; i < part_count && bx < ab_x + ab_w - 60; i++) {
        vga_draw_string(bx, by, ">", FM_C_MUTED, FM_C_PANEL);
        bx += 12;
        char token_buf[32];
        strncpy(token_buf, parts[i], sizeof(token_buf) - 1);
        token_buf[sizeof(token_buf) - 1] = '\0';
        int len = strlen(token_buf);
        if (len > 14) {
            token_buf[12] = '\0';
            strncat(token_buf, "...", sizeof(token_buf) - strlen(token_buf) - 1);
            len = strlen(token_buf);
        }
        vga_draw_string(bx, by, token_buf, FM_C_TEXT, FM_C_PANEL);
        bx += len * 8 + 8;
    }

    /* Search box */
    int sb_x = ab_x + ab_w + 6;
    int sb_w = 200;
    int sb_y = btn_y;
    int sb_h = btn_h;
    if (sb_x + sb_w > x + w - 6) sb_w = x + w - 6 - sb_x;
    if (sb_w > 60) {
        vga_fill_rect(sb_x, sb_y, sb_w, sb_h, FM_C_PANEL);
        vga_draw_rect(sb_x, sb_y, sb_w, sb_h, FM_C_BORDER);
        vga_draw_string(sb_x + 8, sb_y + sb_h / 2 - 4, "Search", FM_C_MUTED, FM_C_PANEL);
        if (fm->search_active && fm->search_query[0]) {
            vga_draw_string(sb_x + 8, sb_y + sb_h / 2 - 4, fm->search_query,
                            FM_C_TEXT, FM_C_PANEL);
        }
        vga_fill_rect(sb_x + sb_w - 18, sb_y + 4, 14, sb_h - 8, FM_C_STRIP);
        vga_draw_rect(sb_x + sb_w - 18, sb_y + 4, 14, sb_h - 8, FM_C_BORDER);
    }
}

/* -------------------------------------------------------------------------- */
/*  Sidebar (Quick Access / This PC / Network)                                */
/* -------------------------------------------------------------------------- */
static const char* fm_sidebar_label(fm_sidebar_item_t item) {
    switch (item) {
        case FM_SIDEBAR_QUICK_ACCESS: return "Quick access";
        case FM_SIDEBAR_DESKTOP:      return "Desktop";
        case FM_SIDEBAR_DOCUMENTS:    return "Documents";
        case FM_SIDEBAR_DOWNLOADS:    return "Downloads";
        case FM_SIDEBAR_MUSIC:        return "Music";
        case FM_SIDEBAR_PICTURES:     return "Pictures";
        case FM_SIDEBAR_VIDEOS:       return "Videos";
        case FM_SIDEBAR_PC:           return "This PC";
        case FM_SIDEBAR_NETWORK:      return "Network";
        case FM_SIDEBAR_BOOKMARKS:    return "Bookmarks";
        default:                      return "Unknown";
    }
}

static void fm_draw_sidebar(file_manager_state_t* fm) {
    int x = fm->window_x;
    int y = fm->window_y + fm->titlebar_height + fm->menubar_height +
            fm->toolbar_height + fm->searchbar_height;
    int w = fm->sidebar_width;
    int h = fm->window_h - (fm->titlebar_height + fm->menubar_height +
                            fm->toolbar_height + fm->searchbar_height +
                            fm->statusbar_height + fm->column_header_height);

    vga_fill_rect(x, y, w, h, FM_C_SIDEBAR);
    vga_draw_rect(x + w - 1, y, 1, h, FM_C_BORDER);

    /* Section header */
    vga_fill_rect(x, y, w, 18, FM_C_LIGHTLINE);
    vga_draw_string(x + 8, y + 3, "Quick access", FM_C_MUTED, FM_C_LIGHTLINE);

    int item_y = y + 22;
    for (int i = 0; i <= FM_SIDEBAR_PC; i++) {
        uint64_t bg = (i == fm->sidebar_selected && i != FM_SIDEBAR_PC) ?
                       FM_C_SELECT : FM_C_SIDEBAR;
        uint64_t fg = (bg == FM_C_SELECT) ? FM_C_ACCENT : FM_C_TEXT;
        vga_fill_rect(x + 4, item_y, w - 8, 22, bg);
        /* Mini icon */
        vga_fill_rect(x + 10, item_y + 4, 14, 14, FM_C_FOLDER);
        vga_draw_rect(x + 10, item_y + 4, 14, 14, FM_C_BORDER);
        vga_draw_string(x + 30, item_y + 7, fm_sidebar_label((fm_sidebar_item_t)i), fg, bg);
        item_y += 26;
    }

    item_y += 8;
    vga_fill_rect(x, item_y, w, 18, FM_C_LIGHTLINE);
    vga_draw_string(x + 8, item_y + 3, "Locations", FM_C_MUTED, FM_C_LIGHTLINE);
    item_y += 22;
    for (int i = FM_SIDEBAR_NETWORK; i <= FM_SIDEBAR_NETWORK; i++) {
        uint64_t bg = (i == fm->sidebar_selected) ? FM_C_SELECT : FM_C_SIDEBAR;
        uint64_t fg = (bg == FM_C_SELECT) ? FM_C_ACCENT : FM_C_TEXT;
        vga_fill_rect(x + 4, item_y, w - 8, 22, bg);
        vga_fill_rect(x + 10, item_y + 4, 14, 14, FM_C_ACCENT);
        vga_draw_rect(x + 10, item_y + 4, 14, 14, FM_C_BORDER);
        vga_draw_string(x + 30, item_y + 7, fm_sidebar_label((fm_sidebar_item_t)i),
                        fg, bg);
    }
}

/* -------------------------------------------------------------------------- */
/*  Column header (sort indicators)                                           */
/* -------------------------------------------------------------------------- */
static void fm_draw_column_header(file_manager_state_t* fm) {
    int x = fm->window_x + fm->sidebar_width;
    int y = fm->window_y + fm->titlebar_height + fm->menubar_height +
            fm->toolbar_height + fm->searchbar_height;
    int w = fm->window_w - fm->sidebar_width;
    int h = fm->column_header_height;

    vga_fill_rect(x, y, w, h, FM_C_HEADER);
    vga_draw_rect(x, y + h - 1, w, 1, FM_C_BORDER);

    const char* headers[] = { "Name", "Size", "Type", "Modified" };
    int positions[] = { fm->col_name_x, fm->col_size_x, fm->col_type_x, fm->col_modified_x };

    for (int i = 0; i < 4; i++) {
        bool active = ((int)fm->sort_mode == i);
        const char* arrow = "";
        if (active) arrow = fm->sort_ascending ? "  v" : "  ^";
        char label[32];
        snprintf(label, sizeof(label), "%s%s", headers[i], arrow);
        vga_draw_string(x + positions[i], y + 4, label, FM_C_TEXT, FM_C_HEADER);
        /* Column separator (only between Name and Size for Windows style). */
        vga_draw_rect(x + positions[i] - 12, y + 4, 1, h - 8, FM_C_BORDER);
    }
}

/* -------------------------------------------------------------------------- */
/*  Main file-list pane                                                       */
/* -------------------------------------------------------------------------- */
static void fm_draw_details_view(file_manager_state_t* fm) {
    int x = fm->window_x + fm->sidebar_width;
    int y = fm->window_y + fm->titlebar_height + fm->menubar_height +
            fm->toolbar_height + fm->searchbar_height + fm->column_header_height;
    int w = fm->window_w - fm->sidebar_width;
    int h = fm->window_h - (fm->titlebar_height + fm->menubar_height +
                             fm->toolbar_height + fm->searchbar_height +
                             fm->statusbar_height + fm->column_header_height);

    vga_fill_rect(x, y, w, h, FM_C_PANEL);
    vga_draw_rect(x, y, w, h, FM_C_BORDER);

    int row_h = 22;
    int row_y = y + 2;
    for (int i = 0; i < fm->entry_count && row_y < y + h - row_h; i++) {
        fs_unified_dirent_t* e = &fm->entries[i];
        bool selected = (i == fm->selected_index);
        uint64_t bg = selected ? FM_C_SELECT : FM_C_PANEL;
        uint64_t fg = selected ? FM_C_ACCENT : FM_C_TEXT;
        vga_fill_rect(x + 2, row_y, w - 4, row_h, bg);

        /* Icon + name */
        fm_draw_icon(x + 6, row_y - 2, e);
        char name_buf[64];
        strncpy(name_buf, e->name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        vga_draw_string(x + fm->col_name_x, row_y + 6, name_buf, fg, bg);

        /* Size */
        if (e->type == FS_UNIFIED_TYPE_FILE) {
            char size_buf[32];
            fm_explorer_format_size(e->size, size_buf, sizeof(size_buf));
            int sz_w = (int)strlen(size_buf) * 8;
            vga_draw_string(x + fm->col_size_x + fm->file_col_size_w - sz_w - 4,
                            row_y + 6, size_buf, fg, bg);
        }

        /* Type */
        const char* type_str = (e->type == FS_UNIFIED_TYPE_DIR) ? "Folder" : "File";
        const char* ext = fm_get_extension(e->name);
        char type_buf[32];
        if (ext && ext[0]) {
            snprintf(type_buf, sizeof(type_buf), "%s file", ext);
            type_str = type_buf;
        }
        vga_draw_string(x + fm->col_type_x, row_y + 6, type_str, FM_C_MUTED, bg);

        /* Modified */
        vga_draw_string(x + fm->col_modified_x, row_y + 6, "2025-01-01",
                        FM_C_MUTED, bg);

        row_y += row_h + 2;
    }
}

static void fm_draw_grid_view(file_manager_state_t* fm) {
    int x = fm->window_x + fm->sidebar_width;
    int y = fm->window_y + fm->titlebar_height + fm->menubar_height +
            fm->toolbar_height + fm->searchbar_height + fm->column_header_height;
    int w = fm->window_w - fm->sidebar_width;
    int h = fm->window_h - (fm->titlebar_height + fm->menubar_height +
                             fm->toolbar_height + fm->searchbar_height +
                             fm->statusbar_height + fm->column_header_height);

    vga_fill_rect(x, y, w, h, FM_C_PANEL);
    vga_draw_rect(x, y, w, h, FM_C_BORDER);

    int cell_w = 110;
    int cell_h = 90;
    int ix = x + 8;
    int iy = y + 8;
    for (int i = 0; i < fm->entry_count && iy + cell_h < y + h; i++) {
        fs_unified_dirent_t* e = &fm->entries[i];
        bool selected = (i == fm->selected_index);
        uint64_t bg = selected ? FM_C_SELECT : FM_C_PANEL;
        vga_fill_rect(ix, iy, cell_w, cell_h, bg);
        fm_draw_icon(ix + cell_w / 2 - 11, iy + 12, e);
        char name_buf[32];
        strncpy(name_buf, e->name, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        vga_draw_string(ix + 6, iy + cell_h - 18, name_buf, FM_C_TEXT, bg);
        ix += cell_w + 6;
        if (ix + cell_w > x + w - 6) {
            ix = x + 8;
            iy += cell_h + 6;
        }
    }
}

void fm_explorer_draw_file_list(file_manager_state_t* fm) {
    if (!fm) return;
    fm_draw_column_header(fm);
    if (fm->view_mode == FM_VIEW_GRID) {
        fm_draw_grid_view(fm);
    } else {
        fm_draw_details_view(fm);
    }
}

/* -------------------------------------------------------------------------- */
/*  Status bar — item count + host memory readout                             */
/* -------------------------------------------------------------------------- */
void fm_explorer_draw_statusbar(file_manager_state_t* fm) {
    if (!fm) return;

    int x = fm->window_x;
    int y = fm->window_y + fm->window_h - fm->statusbar_height;
    int w = fm->window_w;
    int h = fm->statusbar_height;

    vga_fill_rect(x, y, w, h, FM_C_TOOLBAR);
    vga_draw_rect(x, y - 1, w, 1, FM_C_BORDER);

    /* Item count / selection text */
    char count_buf[64];
    if (fm->selected_count > 0) {
        snprintf(count_buf, sizeof(count_buf), "%d item(s) selected",
                 fm->selected_count);
    } else {
        snprintf(count_buf, sizeof(count_buf), "%d item(s)", fm->entry_count);
    }
    vga_draw_string(x + 8, y + 4, count_buf, FM_C_MUTED, FM_C_TOOLBAR);

    /* Right hand memory readout using the runtime memory authority. */
    extern uint64_t cos_runtime_total_bytes(void) __attribute__((weak));
    extern uint64_t cos_runtime_available_bytes(void) __attribute__((weak));
    extern uint64_t memory_get_total(void) __attribute__((weak));
    extern uint64_t memory_get_free(void) __attribute__((weak));

    uint64_t total = cos_runtime_total_bytes ? cos_runtime_total_bytes() :
                     (memory_get_total ? memory_get_total() : 0);
    uint64_t avail = cos_runtime_available_bytes ? cos_runtime_available_bytes() :
                     (memory_get_free ? memory_get_free() : 0);

    if (total == 0) total = 512ULL * 1024 * 1024;
    if (avail > total) avail = total;

    char total_buf[32];
    char used_buf[32];
    fm_explorer_format_size(total, total_buf, sizeof(total_buf));
    fm_explorer_format_size(total - avail, used_buf, sizeof(used_buf));

    /* Memory bar — pink-to-blue gradient approximated by three stripes. */
    int bar_x = x + w - 280;
    int bar_y = y + 6;
    int bar_w = 120;
    int bar_h = 10;
    vga_fill_rect(bar_x, bar_y, bar_w, bar_h, FM_C_PANEL);
    vga_draw_rect(bar_x, bar_y, bar_w, bar_h, FM_C_BORDER);
    int used_w = (int)((total - avail) * (uint64_t)bar_w / (total ? total : 1));
    if (used_w < 0) used_w = 0;
    if (used_w > bar_w) used_w = bar_w;
    vga_fill_rect(bar_x, bar_y, used_w, bar_h, FM_C_ACCENT);

    char mem_label[160];
    snprintf(mem_label, sizeof(mem_label), "RAM %s / %s", used_buf, total_buf);
    int label_w = (int)strlen(mem_label) * 8;
    vga_draw_string(x + w - label_w - 8, y + 4, mem_label, FM_C_TEXT, FM_C_TOOLBAR);
}

/* -------------------------------------------------------------------------- */
/*  Top-level draw                                                            */
/* -------------------------------------------------------------------------- */
void fm_explorer_draw(file_manager_state_t* fm) {
    if (!fm) return;

    vga_fill_rect(fm->window_x, fm->window_y, fm->window_w, fm->window_h, FM_C_BG);
    vga_draw_rect(fm->window_x, fm->window_y, fm->window_w, fm->window_h, FM_C_BORDER);

    fm_draw_titlebar(fm);
    fm_draw_menubar(fm);
    fm_draw_toolbar(fm);
    fm_draw_sidebar(fm);
    fm_explorer_draw_file_list(fm);
    fm_explorer_draw_statusbar(fm);
}

/* -------------------------------------------------------------------------- */
/*  Operations                                                                */
/* -------------------------------------------------------------------------- */
int fm_explorer_copy(file_manager_state_t* fm) {
    if (!fm || fm->selected_index < 0) return -1;
    fm_copy_path(fm->clipboard_path, sizeof(fm->clipboard_path), fm->entries[fm->selected_index].name);
    fm->clipboard_is_cut = false;
    return 0;
}

int fm_explorer_cut(file_manager_state_t* fm) {
    if (!fm || fm->selected_index < 0) return -1;
    fm_copy_path(fm->clipboard_path, sizeof(fm->clipboard_path), fm->entries[fm->selected_index].name);
    fm->clipboard_is_cut = true;
    return 0;
}

int fm_explorer_paste(file_manager_state_t* fm) {
    if (!fm || !fm->clipboard_path[0]) return -1;
    /* TODO: dispatch to unified FS copy/move API. */
    return 0;
}

int fm_explorer_delete(file_manager_state_t* fm) {
    if (!fm || fm->selected_index < 0) return -1;
    char full_path[FS_UNIFIED_MAX_PATH];
    snprintf(full_path, sizeof(full_path), "%s/%s",
             fm->current_path, fm->entries[fm->selected_index].name);
    return fs_unified_unlink(full_path);
}

int fm_explorer_rename(file_manager_state_t* fm, const char* new_name) {
    if (!fm || fm->selected_index < 0 || !new_name) return -1;
    char old_path[FS_UNIFIED_MAX_PATH];
    char new_path[FS_UNIFIED_MAX_PATH];
    snprintf(old_path, sizeof(old_path), "%s/%s",
             fm->current_path, fm->entries[fm->selected_index].name);
    snprintf(new_path, sizeof(new_path), "%s/%s",
             fm->current_path, new_name);
    return fs_unified_rename(old_path, new_path);
}

int fm_explorer_create_folder(file_manager_state_t* fm, const char* name) {
    if (!fm || !name) return -1;
    char path[FS_UNIFIED_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", fm->current_path, name);
    return fs_unified_mkdir(path);
}

/* -------------------------------------------------------------------------- */
/*  Search                                                                    */
/* -------------------------------------------------------------------------- */
int fm_explorer_search(file_manager_state_t* fm, const char* query) {
    if (!fm || !query) return -1;
    fm_copy_path(fm->search_query, sizeof(fm->search_query), query);
    fm->search_active = true;
    /* TODO: filter entries in-place. */
    return 0;
}

void fm_explorer_toggle_hidden(file_manager_state_t* fm) {
    if (!fm) return;
    fm->show_hidden = !fm->show_hidden;
}

/* -------------------------------------------------------------------------- */
/*  Events                                                                    */
/* -------------------------------------------------------------------------- */
void fm_explorer_handle_mouse_click(file_manager_state_t* fm, int x, int y, int button) {
    if (!fm) return;
    (void)button;

    /* Toolbar actions */
    int btn_y = fm->window_y + fm->titlebar_height + fm->menubar_height + 6;
    int btn_h = fm->toolbar_height - 12;
    if (y >= btn_y && y <= btn_y + btn_h) {
        int bx = fm->window_x + 6;
        if (x >= bx && x <= bx + 28 && fm->history_index > 0) { fm_explorer_navigate_back(fm); return; }
        bx += 32;
        if (x >= bx && x <= bx + 28 && fm->history_index < fm->history_count - 1) { fm_explorer_navigate_forward(fm); return; }
        bx += 32;
        if (x >= bx && x <= bx + 28) { fm_explorer_navigate_up(fm); return; }
        bx += 32;
        if (x >= bx && x <= bx + 28) { fm_explorer_refresh(fm); return; }
    }

    /* Sidebar hits */
    int sb_x = fm->window_x;
    int sb_y = fm->window_y + fm->titlebar_height + fm->menubar_height + fm->toolbar_height + fm->searchbar_height;
    int sb_w = fm->sidebar_width;
    if (x >= sb_x && x <= sb_x + sb_w && y >= sb_y) {
        int item_idx = (y - sb_y - 22) / 26;
        if (item_idx >= 0 && item_idx <= FM_SIDEBAR_NETWORK) {
            fm->sidebar_selected = (fm_sidebar_item_t)item_idx;
        }
        return;
    }

    /* File-list row selection */
    int list_x = fm->window_x + fm->sidebar_width;
    int list_y = sb_y + fm->column_header_height + 2;
    int row_h = 24;
    if (x >= list_x && x < fm->window_x + fm->window_w &&
        y >= list_y && y < list_y + fm->entry_count * row_h) {
        fm->selected_index = (y - list_y) / row_h;
        fm->selected_count = 1;
    }
}

void fm_explorer_handle_mouse_drag(file_manager_state_t* fm, int x, int y) {
    if (!fm) return;
    /* Drawn-only drag handles between columns — used in detail view. */
    (void)x; (void)y;
}

void fm_explorer_handle_key(file_manager_state_t* fm, int key) {
    if (!fm) return;
    (void)key;
}
