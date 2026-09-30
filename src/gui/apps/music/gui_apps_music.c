/**
 * gui_apps_music.c - C-OS 4.0.8 alpha GUI Apps (分割ファイル)
 * MP3プレイヤー
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

/* ============================================================
 * MP3 Player
 * ============================================================ */
#define MUSIC_MAX_PLAYLIST 32

typedef struct {
    char current_path[FS_MAX_PATH];
    char current_dir[FS_MAX_PATH];
    char playlist[MUSIC_MAX_PLAYLIST][FS_MAX_PATH];
    char titles[MUSIC_MAX_PLAYLIST][FS_MAX_NAME];
    int  playlist_count;
    int  current_index;
    int  selected_index;
    bool loaded;
    bool playing;
    bool paused;
    bool repeat;
    bool shuffle;
    int  playlist_scroll;
    uint64_t duration_ms;
    uint64_t position_ms;
    uint64_t volume;
    char status[128];
} music_player_state_t;

static music_player_state_t g_music = {0};
static int g_music_last_click_row = -1;
static uint64_t g_music_last_click_tick = 0;

static void music_copy_str(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_size && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static bool music_has_suffix_ci(const char* name, const char* suffix) {
    if (!name || !suffix) return FALSE;
    int nl = slen(name), sl = slen(suffix);
    if (sl > nl) return FALSE;
    const char* p = name + (nl - sl);
    for (int i = 0; i < sl; ++i) {
        char a = p[i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return FALSE;
    }
    return TRUE;
}

static bool music_is_audio_file(const char* name) {
    return music_has_suffix_ci(name, ".mp3") || music_has_suffix_ci(name, ".wav") ||
           music_has_suffix_ci(name, ".ogg") || music_has_suffix_ci(name, ".flac");
}

static const char* music_basename(const char* path) {
    if (!path || !path[0]) return "<no track>";
    const char* last = path;
    for (const char* p = path; *p; ++p) if (*p == '/') last = p + 1;
    return *last ? last : path;
}

static bool music_path_is_dir(const char* path) {
    if (!path || !path[0]) return TRUE;
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/') return TRUE;
    return !music_is_audio_file(music_basename(path));
}

static void music_parent_path(const char* path, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!path || !path[0] || smatch(path, "/")) { scopy(out, "/", (int)out_size - 1); return; }
    const char* last = NULL;
    for (const char* p = path; *p; ++p) if (*p == '/') last = p;
    if (!last || last == path) { scopy(out, "/", (int)out_size - 1); return; }
    size_t len = (size_t)(last - path);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static void music_set_status(const char* msg) {
    music_copy_str(g_music.status, sizeof(g_music.status), msg ? msg : "Ready");
}

static bool music_path_equals(const char* a, const char* b) {
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static bool music_playlist_has_path(const char* path) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < g_music.playlist_count; ++i) {
        if (music_path_equals(g_music.playlist[i], path)) return true;
    }
    return false;
}

static void music_add_playlist_item(const char* path, const char* title) {
    if (!path || !path[0] || g_music.playlist_count >= MUSIC_MAX_PLAYLIST) return;
    if (music_playlist_has_path(path)) return;
    music_copy_str(g_music.playlist[g_music.playlist_count], sizeof(g_music.playlist[g_music.playlist_count]), path);
    music_copy_str(g_music.titles[g_music.playlist_count], sizeof(g_music.titles[g_music.playlist_count]), (title && title[0]) ? title : music_basename(path));
    g_music.playlist_count++;
}

static void music_collect_directory_recursive(const char* dir, int depth) {
    if (!dir || !dir[0] || depth > 8 || g_music.playlist_count >= MUSIC_MAX_PLAYLIST) return;

    fs_entry_t* entries = fs_list_dir(dir);
    int total = fs_entry_count_for_path(dir);
    char subdirs[FS_MAX_ENTRIES][FS_MAX_PATH];
    char files[FS_MAX_ENTRIES][FS_MAX_PATH];
    char titles[FS_MAX_ENTRIES][FS_MAX_NAME];
    int subdir_count = 0;
    int file_count = 0;

    for (int i = 0; entries && i < total; ++i) {
        if (strcmp(entries[i].path, dir) != 0) continue;
        char full[FS_MAX_PATH];
        fm_join_path(full, sizeof(full), dir, entries[i].name);
        if (entries[i].is_dir) {
            if (subdir_count < FS_MAX_ENTRIES) {
                music_copy_str(subdirs[subdir_count], sizeof(subdirs[subdir_count]), full);
                subdir_count++;
            }
        } else if (music_is_audio_file(entries[i].name)) {
            if (file_count < FS_MAX_ENTRIES) {
                music_copy_str(files[file_count], sizeof(files[file_count]), full);
                music_copy_str(titles[file_count], sizeof(titles[file_count]), entries[i].name);
                file_count++;
            }
        }
    }

    for (int i = 0; i < file_count && g_music.playlist_count < MUSIC_MAX_PLAYLIST; ++i) {
        music_add_playlist_item(files[i], titles[i]);
    }
    for (int i = 0; i < subdir_count && g_music.playlist_count < MUSIC_MAX_PLAYLIST; ++i) {
        music_collect_directory_recursive(subdirs[i], depth + 1);
    }
}

static int music_visible_start(void) {
    const int visible = 8;
    int max_start = g_music.playlist_count - visible;
    if (max_start < 0) max_start = 0;
    if (g_music.playlist_scroll < 0) g_music.playlist_scroll = 0;
    if (g_music.playlist_scroll > max_start) g_music.playlist_scroll = max_start;
    return g_music.playlist_scroll;
}

static const char* music_repeat_label(void) {
    return g_music.repeat ? "Repeat On" : "Repeat Off";
}


typedef struct {
    int x, y, cw, ch;
    int list_x, list_y, list_w, list_h;
    int right_x, right_y, right_w, right_h;
    int playlist_rows_y;
    int control_y;
    int progress_x, progress_y, progress_w;
    int volume_x, volume_y, volume_w;
    int visible_rows;
    int row_h;
} music_layout_t;

static int music_clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint64_t music_mix64(uint64_t a, uint64_t b) {
    uint64_t x = a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static void music_build_layout(window_t* w, music_layout_t* L) {
    memset(L, 0, sizeof(*L));
    L->x = w->x + 10;
    L->y = w->y + TITLEBAR_H + 10;
    L->cw = w->w - 20;
    L->ch = w->h - TITLEBAR_H - 20;
    if (L->cw < 560) L->cw = 560;
    if (L->ch < 360) L->ch = 360;
    L->list_w = music_clampi((int)(L->cw * 36 / 100), 250, 320);
    if (L->list_w > L->cw - 280) L->list_w = L->cw - 280;
    if (L->list_w < 240) L->list_w = 240;
    L->list_x = L->x + 12;
    L->list_y = L->y + 14;
    L->list_h = L->ch - 28;
    L->right_x = L->list_x + L->list_w + 14;
    L->right_y = L->list_y;
    L->right_w = L->cw - (L->list_w + 14) - 12;
    L->right_h = L->list_h;
    L->playlist_rows_y = L->list_y + 38;
    L->row_h = FONT_H + 8;
    L->visible_rows = 8;
    L->control_y = L->right_y + L->right_h - 72;
    L->progress_x = L->right_x + 18;
    L->progress_y = L->right_y + 248;
    L->progress_w = L->right_w - 36;
    L->volume_x = L->right_x + 120;
    /* Was right_y + 330, which sat underneath the "Audio"/"Mode"/"Index" info
     * chips (now drawn at right_y + 270..362) instead of below them, so the
     * volume slider and the chip boxes were rendered on top of each other.
     * Push it below the chip row instead. */
    L->volume_y = L->right_y + 392;
    L->volume_w = L->right_w - 138;
}

static void music_sync_scroll_visible(void) {
    const int visible = 8;
    if (g_music.playlist_count <= visible) {
        g_music.playlist_scroll = 0;
        return;
    }
    if (g_music.selected_index < 0) {
        if (g_music.playlist_scroll < 0) g_music.playlist_scroll = 0;
        return;
    }
    int max_start = g_music.playlist_count - visible;
    int target = g_music.selected_index - (visible / 2);
    if (target < 0) target = 0;
    if (target > max_start) target = max_start;
    g_music.playlist_scroll = target;
}

static void music_draw_button(int x, int y, int w, int h, const char* text, bool active, bool accent) {
    uint64_t shadow = rgb(12, 16, 24);
    uint64_t top = accent ? rgb(44, 160, 255) : (active ? rgb(64, 74, 90) : rgb(34, 42, 54));
    uint64_t bottom = accent ? rgb(24, 116, 210) : (active ? rgb(50, 60, 76) : rgb(24, 30, 40));
    vga_fill_rounded_rect(x + 2, y + 3, w, h, 8, shadow);
    vga_fill_rounded_rect(x, y, w, h, 8, bottom);
    vga_fill_rect(x, y, w, h / 2, top);
    vga_draw_rounded_rect(x, y, w, h, 8, rgb(88, 100, 118));
    vga_draw_string(x + (w - slen(text) * FONT_W) / 2, y + (h - FONT_H) / 2, text, rgb(243, 247, 252), 0xFFFFFFFF);
}

static void music_draw_progress(int x, int y, int w, int h, uint64_t position, uint64_t duration) {
    vga_fill_rounded_rect(x, y, w, h, 8, rgb(22, 28, 38));
    vga_draw_rounded_rect(x, y, w, h, 8, rgb(62, 74, 90));
    int fill = 0;
    if (duration > 0) {
        fill = (int)((position * (uint64_t)w) / duration);
        if (fill < 0) fill = 0;
        if (fill > w) fill = w;
    }
    if (fill > 0) {
        vga_fill_rounded_rect(x, y, fill, h, 8, rgb(44, 176, 134));
        if (fill > 6) vga_fill_rect(x, y, fill, h / 2, rgb(70, 220, 170));
        int hx = x + fill - 3;
        if (hx < x) hx = x;
        vga_fill_circle(hx + 4, y + h / 2, 5, rgb(230, 242, 252));
        vga_draw_circle(hx + 4, y + h / 2, 5, rgb(20, 24, 34));
    }
}

static void music_draw_visualizer(int x, int y, int w, int h, bool playing) {
    const int bars = 18;
    int gap = 4;
    int bar_w = (w - (bars - 1) * gap) / bars;
    if (bar_w < 4) bar_w = 4;
    uint64_t seed = music_mix64(gui_frame_counter, (uint64_t)get_timer_ticks());
    for (int i = 0; i < bars; ++i) {
        uint64_t wave = (seed >> ((i % 8) * 8)) & 0xFF;
        int amp = (int)(wave % (h - 10));
        if (!playing) amp = 4 + (i % 3);
        int bx = x + i * (bar_w + gap);
        int by = y + (h - amp);
        vga_fill_rounded_rect(bx, by, bar_w, amp, 4, rgb(40 + (i * 9) % 90, 140 + (i * 3) % 80, 220 - (i * 4) % 80));
    }
    vga_draw_rounded_rect(x - 2, y - 2, w + 4, h + 4, 8, rgb(54, 66, 82));
}

static void music_draw_disc(int cx, int cy, int radius, bool playing) {
    vga_fill_circle(cx, cy, radius + 10, rgb(8, 12, 18));
    vga_fill_circle(cx, cy, radius, rgb(36, 40, 50));
    vga_draw_circle(cx, cy, radius, rgb(88, 98, 118));
    vga_draw_circle(cx, cy, radius - 10, rgb(54, 62, 78));
    vga_fill_circle(cx, cy, 9, rgb(18, 22, 30));
    vga_draw_circle(cx, cy, 9, rgb(106, 118, 138));
    uint64_t tick = gui_frame_counter + (playing ? get_timer_ticks() : 0);
    int ox = (int)((tick >> 1) % (radius - 6));
    int oy = (int)((tick >> 2) % (radius - 6));
    if (ox < 0) ox = -ox;
    if (oy < 0) oy = -oy;
    vga_fill_circle(cx + ox - radius / 2, cy - oy + radius / 3, 4, rgb(82, 176, 246));
    vga_fill_circle(cx - ox / 2, cy + oy / 2, 2, rgb(242, 248, 255));
    for (int i = 0; i < 4; ++i) {
        int rr = radius - 18 - i * 6;
        if (rr > 0) vga_draw_circle(cx, cy, rr, rgb(42 + i * 10, 48 + i * 10, 60 + i * 8));
    }
}

static void music_draw_playlist_scrollbar(int x, int y, int h, int visible_rows, int total_count, int start_row) {
    vga_fill_rounded_rect(x, y, 12, h, 6, rgb(18, 24, 34));
    vga_draw_rounded_rect(x, y, 12, h, 6, rgb(58, 68, 84));
    if (total_count <= visible_rows) {
        vga_fill_rounded_rect(x + 2, y + 2, 8, h - 4, 4, rgb(52, 160, 230));
        return;
    }
    int thumb_h = (h * visible_rows) / total_count;
    if (thumb_h < 18) thumb_h = 18;
    if (thumb_h > h - 4) thumb_h = h - 4;
    int max_start = total_count - visible_rows;
    int thumb_y = y + 2 + ((h - thumb_h - 4) * start_row) / (max_start > 0 ? max_start : 1);
    vga_fill_rounded_rect(x + 2, thumb_y, 8, thumb_h, 4, rgb(76, 186, 255));
    vga_draw_rounded_rect(x + 2, thumb_y, 8, thumb_h, 4, rgb(180, 230, 255));
}

static void music_draw_playlist_row(int x, int y, int w, int h, int index, bool current, bool selected) {
    uint64_t bg = current ? rgb(20, 64, 56) : (selected ? rgb(34, 52, 76) : rgb(22, 26, 36));
    uint64_t edge = current ? rgb(52, 176, 132) : (selected ? rgb(78, 110, 168) : rgb(54, 64, 80));
    vga_fill_rounded_rect(x, y, w, h, 8, bg);
    vga_draw_rounded_rect(x, y, w, h, 8, edge);
    char num[12];
    uitostr((uint64_t)(index + 1), num);
    char label[FS_MAX_NAME + 24];
    label[0] = '\0';
    if (current) scopy(label, "▶ ", sizeof(label) - 1);
    else if (selected) scopy(label, "• ", sizeof(label) - 1);
    else scopy(label, "  ", sizeof(label) - 1);
    if (index + 1 < 10) scat(label, "0", sizeof(label) - 1);
    scat(label, num, sizeof(label) - 1);
    scat(label, "  ", sizeof(label) - 1);
    scat(label, g_music.titles[index], sizeof(label) - 1);
    vga_draw_string(x + 10, y + (h - FONT_H) / 2, label, rgb(236, 242, 250), 0xFFFFFFFF);
}

static void music_draw_info_chip(int x, int y, int w, const char* label, const char* value) {
    vga_fill_rounded_rect(x, y, w, 44, 8, rgb(18, 24, 34));
    vga_draw_rounded_rect(x, y, w, 44, 8, rgb(60, 70, 86));
    vga_draw_string(x + 10, y + 6, label, rgb(120, 136, 160), 0xFFFFFFFF);
    vga_draw_string(x + 10, y + 22, value, rgb(238, 242, 248), 0xFFFFFFFF);
}

static void music_format_track_label(char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    const char* title = mk_mp3_get_current_title ? mk_mp3_get_current_title() : NULL;
    const char* artist = mk_mp3_get_current_artist ? mk_mp3_get_current_artist() : NULL;
    const char* album = mk_mp3_get_current_album ? mk_mp3_get_current_album() : NULL;
    uint64_t year = mk_mp3_get_current_year ? mk_mp3_get_current_year() : 0;
    if (title && title[0]) {
        scopy(out, title, (int)out_size - 1);
    } else {
        scopy(out, music_basename(g_music.current_path), (int)out_size - 1);
    }
    if (artist && artist[0]) {
        scat(out, " — ", (int)out_size - 1);
        scat(out, artist, (int)out_size - 1);
    }
    if (album && album[0]) {
        scat(out, " [", (int)out_size - 1);
        scat(out, album, (int)out_size - 1);
        if (year > 0) {
            char ybuf[16];
            scat(out, " ", (int)out_size - 1);
            uitostr(year, ybuf);
            scat(out, ybuf, (int)out_size - 1);
        }
        scat(out, "]", (int)out_size - 1);
    }
}

static void music_sync_from_backend_playlist(void) {
    if (!mk_mp3_get_playlist) return;
    mk_mp3_playlist_t* pl = mk_mp3_get_playlist();
    if (!pl) return;
    g_music.repeat = pl->repeat;
    g_music.shuffle = pl->shuffle;
    if (pl->count == 0) return;

    uint64_t current = (pl->current < pl->count) ? pl->current : 0;
    const char* path = pl->entries[current].filename;
    if (path && path[0]) {
        char dirbuf[FS_MAX_PATH];
        music_copy_str(g_music.current_path, sizeof(g_music.current_path), path);
        if (music_path_is_dir(path)) {
            music_copy_str(dirbuf, sizeof(dirbuf), path);
        } else {
            music_parent_path(path, dirbuf, sizeof(dirbuf));
        }
        music_copy_str(g_music.current_dir, sizeof(g_music.current_dir), dirbuf);
        for (int i = 0; i < g_music.playlist_count; ++i) {
            if (smatch(g_music.playlist[i], path)) {
                g_music.current_index = i;
                g_music.selected_index = i;
                break;
            }
        }
        if (g_music.status[0] == '\0') {
            const char* base = music_basename(path);
            music_copy_str(g_music.status, sizeof(g_music.status), base);
        }
    }
}

static void music_refresh_from_backend(void) {
    if (mk_mp3_get_player_state) {
        mk_mp3_player_t* st = mk_mp3_get_player_state();
        if (st) {
            g_music.volume = st->volume;
            g_music.playing = (st->state == 1);
            g_music.paused  = (st->state == 2);
            g_music.position_ms = st->current_position;
            g_music.duration_ms = st->total_duration;
            g_music.repeat = st->repeat;
            g_music.shuffle = st->shuffle;
        }
    }
    music_sync_from_backend_playlist();
}


static void music_load_playlist_from_path(const char* path) {
    g_music.playlist_count = 0;
    g_music.current_index = -1;
    g_music.selected_index = -1;
    g_music.loaded = FALSE;
    g_music.playing = FALSE;
    g_music.paused = FALSE;
    g_music.duration_ms = 0;
    g_music.position_ms = 0;
    g_music.volume = 80;
    g_music.playlist_scroll = 0;
    g_music.current_path[0] = '\0';
    g_music.current_dir[0] = '\0';

    if (!path || !path[0]) {
        music_set_status("Open an audio file or folder");
        return;
    }

    char dir[FS_MAX_PATH];
    char base[FS_MAX_NAME];
    base[0] = '\0';
    if (music_path_is_dir(path)) {
        music_copy_str(dir, sizeof(dir), path);
        size_t len = strlen(dir);
        if (len > 1 && dir[len - 1] == '/') dir[len - 1] = '\0';
    } else {
        music_parent_path(path, dir, sizeof(dir));
        music_copy_str(base, sizeof(base), music_basename(path));
    }
    music_copy_str(g_music.current_dir, sizeof(g_music.current_dir), dir);

    if (!base[0]) {
        /* Folder launches use the same bounded recursive collector as the
         * player backend, so /music/Artist/Album/*.mp3 is usable from the UI. */
        music_collect_directory_recursive(dir, 0);
    } else {
        fs_entry_t* entries = fs_list_dir(dir);
        int total = fs_entry_count_for_path(dir);
        for (int i = 0; entries && i < total && g_music.playlist_count < MUSIC_MAX_PLAYLIST; ++i) {
            if (entries[i].is_dir) continue;
            if (!music_is_audio_file(entries[i].name)) continue;
            fm_join_path(g_music.playlist[g_music.playlist_count], sizeof(g_music.playlist[g_music.playlist_count]), dir, entries[i].name);
            music_copy_str(g_music.titles[g_music.playlist_count], sizeof(g_music.titles[g_music.playlist_count]), entries[i].name);
            if (base[0] && smatch(entries[i].name, base)) {
                g_music.current_index = g_music.playlist_count;
                g_music.selected_index = g_music.playlist_count;
            }
            g_music.playlist_count++;
        }
    }

    if (g_music.playlist_count == 0) {
        music_set_status("No MP3/audio files found in folder");
        return;
    }

    if (g_music.current_index < 0) g_music.current_index = 0;
    g_music.selected_index = g_music.current_index;
    music_copy_str(g_music.current_path, sizeof(g_music.current_path), g_music.playlist[g_music.current_index]);
    g_music.loaded = TRUE;
    music_set_status("Track ready");

    if (mk_mp3_load_file) {
        if (mk_mp3_load_file(g_music.current_path) == 0) {
            music_set_status("Track loaded");
            music_refresh_from_backend();
        } else {
            music_set_status("Backend load failed; playlist only");
        }
    }
}

static void music_select_index(int index, bool start_playback) {
    if (index < 0 || index >= g_music.playlist_count) return;
    g_music.current_index = index;
    g_music.selected_index = index;
    music_copy_str(g_music.current_path, sizeof(g_music.current_path), g_music.playlist[index]);
    g_music.loaded = TRUE;
    g_music.position_ms = 0;
    g_music.playing = FALSE;
    g_music.paused = FALSE;
    music_set_status(music_basename(g_music.current_path));
    music_sync_scroll_visible();

    if (mk_mp3_load_file) {
        if (mk_mp3_load_file(g_music.current_path) == 0) {
            music_refresh_from_backend();
            if (start_playback && mk_mp3_play) {
                if (mk_mp3_play() == 0) {
                    g_music.playing = TRUE;
                    g_music.paused = FALSE;
                    music_set_status("Playing");
                } else {
                    music_set_status("Play failed");
                }
            }
        } else {
            music_set_status("Failed to load track");
        }
    }
}


static void music_step_track(int delta) {
    if (g_music.playlist_count <= 0) return;
    int next = g_music.current_index + delta;
    if (g_music.shuffle && g_music.playlist_count > 1 && delta != 0) {
        uint64_t seed = gui_frame_counter ^ (uint64_t)get_timer_ticks();
        int offset = (int)(seed % (uint64_t)(g_music.playlist_count - 1)) + 1;
        if (delta > 0) {
            next = (g_music.current_index + offset) % g_music.playlist_count;
        } else {
            next = (g_music.current_index + g_music.playlist_count - offset) % g_music.playlist_count;
        }
    } else {
        if (next < 0) next = g_music.playlist_count - 1;
        if (next >= g_music.playlist_count) next = 0;
    }
    music_select_index(next, g_music.playing || g_music.loaded == FALSE);
}


static void music_toggle_play(void) {
    if (!g_music.loaded) {
        if (g_music.playlist_count > 0) music_select_index(g_music.selected_index >= 0 ? g_music.selected_index : 0, TRUE);
        return;
    }
    if (g_music.playing) {
        int rc = mk_mp3_pause ? mk_mp3_pause() : 0;
        if (rc == 0) {
            g_music.playing = FALSE;
            g_music.paused = TRUE;
            music_set_status("Paused");
        } else {
            music_set_status("Pause failed");
        }
    } else {
        int rc = 0;
        if (g_music.paused && mk_mp3_resume) {
            rc = mk_mp3_resume();
        } else if (mk_mp3_play) {
            rc = mk_mp3_play();
        }
        if (rc == 0 || (!mk_mp3_resume && !mk_mp3_play)) {
            g_music.playing = TRUE;
            g_music.paused = FALSE;
            music_set_status("Playing");
        } else {
            music_set_status("Play failed");
        }
    }
}


static void music_stop(void) {
    int rc = mk_mp3_stop ? mk_mp3_stop() : 0;
    if (rc == 0) {
        g_music.playing = FALSE;
        g_music.paused = FALSE;
        g_music.position_ms = 0;
        music_set_status("Stopped");
    } else {
        music_set_status("Stop failed");
    }
}


static void music_change_volume(int delta) {
    int vol = (int)g_music.volume + delta;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    g_music.volume = (uint64_t)vol;
    if (mk_mp3_set_volume) mk_mp3_set_volume(g_music.volume);
}


static void music_format_time(uint64_t ms, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    uint64_t total = ms / 1000;
    uint64_t h = total / 3600;
    uint64_t m = (total % 3600) / 60;
    uint64_t s = total % 60;
    char hbuf[16], mbuf[16], sbuf[16];
    out[0] = '\0';
    if (h > 0) {
        uitostr(h, hbuf);
        scopy(out, hbuf, out_size - 1);
        scat(out, ":", out_size - 1);
        if (m < 10) scat(out, "0", out_size - 1);
        uitostr(m, mbuf);
        scat(out, mbuf, out_size - 1);
        scat(out, ":", out_size - 1);
        if (s < 10) scat(out, "0", out_size - 1);
        uitostr(s, sbuf);
        scat(out, sbuf, out_size - 1);
    } else {
        uitostr(m, mbuf);
        uitostr(s, sbuf);
        scopy(out, mbuf, out_size - 1);
        scat(out, ":", out_size - 1);
        if (s < 10) scat(out, "0", out_size - 1);
        scat(out, sbuf, out_size - 1);
    }
}


static void music_seek_to_ratio(int64_t click_x, int bar_x, int bar_w) {
    if (!g_music.loaded || g_music.duration_ms == 0 || bar_w <= 0) return;
    int rel = (int)(click_x - bar_x);
    if (rel < 0) rel = 0;
    if (rel > bar_w) rel = bar_w;
    uint64_t pos = (g_music.duration_ms * (uint64_t)rel) / (uint64_t)bar_w;
    if (mk_mp3_seek) {
        if (mk_mp3_seek(pos) != 0) {
            /* fall back to local position */ 
        }
    }
    g_music.position_ms = pos;
}

static void music_toggle_repeat(void) {
    g_music.repeat = !g_music.repeat;
    if (mk_mp3_set_repeat) {
        mk_mp3_set_repeat(g_music.repeat ? 2 : 0);
    }
    music_set_status(g_music.repeat ? "Repeat on" : "Repeat off");
}


static void music_toggle_shuffle(void) {
    g_music.shuffle = !g_music.shuffle;
    if (mk_mp3_set_shuffle) {
        mk_mp3_set_shuffle(g_music.shuffle);
    }
    music_set_status(g_music.shuffle ? "Shuffle on" : "Shuffle off");
}


void music_player_open(const char* path) {
    /* The launcher historically passed "/", which opened a blank player when
     * tracks were stored below /music. Prefer the conventional music folder,
     * then fall back to the requested path so both layouts remain usable. */
    if ((!path || !path[0] || smatch(path, "/")) && fs_entry_count_for_path("/music") > 0) {
        music_load_playlist_from_path("/music");
        if (!g_music.loaded) music_load_playlist_from_path(path ? path : "/");
    } else {
        music_load_playlist_from_path(path ? path : "/");
    }
    int ex = gui_find_window(WIN_MUSIC);
    if (ex < 0) {
        gui_open_window(WIN_MUSIC, gui_text("MP3 Player", "MP3プレーヤー"), 120, 72, 1040, 680);
        ex = gui_find_window(WIN_MUSIC);
    }
    if (ex >= 0) {
        window_t* w = &windows[ex];
        w->visible = TRUE;
        w->minimized = FALSE;
        gui_focus_window(ex);
    }
    /* Double-clicking a media file should play it, not just load it into
     * an idle player requiring a separate manual Play click. */
    if (g_music.loaded && mk_mp3_play) {
        if (mk_mp3_play() == 0) {
            g_music.playing = TRUE;
            g_music.paused = FALSE;
            music_set_status("Playing");
        }
    }
}

void music_player_handle_click(int idx, int mx, int my) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    music_layout_t L;
    music_build_layout(w, &L);
    music_sync_scroll_visible();

    int start_row = music_visible_start();
    int rows_x = L.list_x + 10;
    int rows_y = L.list_y + 40;
    int rows_w = L.list_w - 28;
    int rows_h = L.visible_rows * L.row_h;
    int scrollbar_x = L.list_x + L.list_w - 14;
    int scrollbar_y = rows_y;
    int scrollbar_h = rows_h;

    if (mx >= rows_x && mx < rows_x + rows_w && my >= rows_y && my < rows_y + rows_h) {
        int row = (my - rows_y) / L.row_h;
        int index = start_row + row;
        if (row >= 0 && index >= 0 && index < g_music.playlist_count) {
            uint64_t now = get_timer_ticks();
            bool dbl = (g_music_last_click_row == index && (now - g_music_last_click_tick) < 500);
            g_music.selected_index = index;
            music_sync_scroll_visible();
            g_music_last_click_row = index;
            g_music_last_click_tick = now;
            if (dbl) music_select_index(index, TRUE);
        }
        return;
    }

    if (mx >= scrollbar_x && mx < scrollbar_x + 12 && my >= scrollbar_y && my < scrollbar_y + scrollbar_h) {
        if (g_music.playlist_count > L.visible_rows) {
            int thumb_h = (scrollbar_h * L.visible_rows) / g_music.playlist_count;
            if (thumb_h < 18) thumb_h = 18;
            int max_start = g_music.playlist_count - L.visible_rows;
            int rel = my - scrollbar_y - thumb_h / 2;
            if (rel < 0) rel = 0;
            int max_rel = scrollbar_h - thumb_h;
            if (max_rel < 1) max_rel = 1;
            int new_scroll = (rel * max_start) / max_rel;
            g_music.playlist_scroll = music_clampi(new_scroll, 0, max_start);
        }
        return;
    }

    if (mx >= L.right_x + 16 && mx < L.right_x + 16 + 70 && my >= L.control_y && my < L.control_y + 28) { music_step_track(-1); return; }
    if (mx >= L.right_x + 92 && mx < L.right_x + 92 + 86 && my >= L.control_y && my < L.control_y + 28) { music_toggle_play(); return; }
    if (mx >= L.right_x + 184 && mx < L.right_x + 184 + 68 && my >= L.control_y && my < L.control_y + 28) { music_stop(); return; }
    if (mx >= L.right_x + 258 && mx < L.right_x + 258 + 70 && my >= L.control_y && my < L.control_y + 28) { music_step_track(+1); return; }
    if (mx >= L.right_x + 334 && mx < L.right_x + 334 + 96 && my >= L.control_y && my < L.control_y + 28) { music_toggle_repeat(); return; }
    if (mx >= L.right_x + 436 && mx < L.right_x + 436 + 102 && my >= L.control_y && my < L.control_y + 28) { music_toggle_shuffle(); return; }

    if (mx >= L.right_x + 16 && mx < L.right_x + 16 + 74 && my >= L.control_y + 40 && my < L.control_y + 64) { music_change_volume(-10); return; }
    if (mx >= L.right_x + 96 && mx < L.right_x + 96 + 74 && my >= L.control_y + 40 && my < L.control_y + 64) { music_change_volume(+10); return; }
    if (mx >= L.right_x + 180 && mx < L.right_x + 180 + 118 && my >= L.control_y + 40 && my < L.control_y + 64) {
        int sel = (g_music.selected_index >= 0) ? g_music.selected_index : g_music.current_index;
        if (sel >= 0 && sel < g_music.playlist_count) music_select_index(sel, TRUE);
        return;
    }
    if (mx >= L.right_x + 304 && mx < L.right_x + 304 + 118 && my >= L.control_y + 40 && my < L.control_y + 64) {
        if (g_music.current_dir[0]) music_load_playlist_from_path(g_music.current_dir);
        return;
    }

    if (mx >= L.progress_x && mx < L.progress_x + L.progress_w && my >= L.progress_y && my < L.progress_y + 14) {
        music_seek_to_ratio(mx, L.progress_x, L.progress_w);
        return;
    }
    if (mx >= L.volume_x && mx < L.volume_x + L.volume_w && my >= L.volume_y - 3 && my < L.volume_y + 15) {
        int rel = mx - L.volume_x;
        if (rel < 0) rel = 0;
        if (rel > L.volume_w) rel = L.volume_w;
        g_music.volume = (uint64_t)((rel * 100) / (L.volume_w > 0 ? L.volume_w : 1));
        if (mk_mp3_set_volume) mk_mp3_set_volume(g_music.volume);
        music_set_status("Volume adjusted");
        return;
    }
}


void draw_jpeg_viewer(int idx) {
    if (idx < 0 || idx >= window_count) {
        serial_puts("[JPEG_VIEW] ERROR: Invalid window index\n");
        return;
    }
    window_t* w = &windows[idx];

    if (w->w < 640) w->w = 640;
    if (w->h < 480) w->h = 480;
    if (w->x + w->w > (int)SCREEN_W) w->x = (int)SCREEN_W - w->w;
    if (w->y + w->h > (int)SCREEN_H - 40) w->y = (int)SCREEN_H - 40 - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;

    const int pad = 12;
    const int header_h = 26;
    int area_x = w->x + pad;
    int area_y = w->y + TITLEBAR_H + pad + header_h;
    int area_w = w->w - pad * 2;
    int area_h = w->h - TITLEBAR_H - pad * 2 - header_h - 30;
    if (area_w < 1) area_w = 1;
    if (area_h < 1) area_h = 1;

    /* Draw window background and header */
    vga_fill_rounded_rect(w->x + 4, w->y + TITLEBAR_H + 2, w->w - 8, w->h - TITLEBAR_H - 6, 14, rgb(15, 18, 24));
    vga_fill_rect(w->x + 8, w->y + TITLEBAR_H + 8, w->w - 16, 1, rgb(40, 46, 58));
    vga_draw_string(w->x + 16, w->y + TITLEBAR_H + 12, "Image Viewer", rgb(242, 246, 252), 0xFFFFFFFF);

    /* Check if image is loaded */
    bool is_loaded = false;
    extern bool jpeg_viewer_is_loaded(void);
    if (jpeg_viewer_is_loaded) {
        is_loaded = jpeg_viewer_is_loaded();
    } else {
        serial_puts("[JPEG_VIEW] ERROR: jpeg_viewer_is_loaded function pointer is NULL\n");
    }

    /* EFM's preview and this standalone window share one decoder state.  If
     * another preview update cleared that state, the window still owns the
     * requested path and can deterministically restore it on the next frame
     * instead of remaining permanently blank. */
    if (!is_loaded && w->filename[0]) {
        extern int jpeg_viewer_load_file(const char* file_path);
        if (jpeg_viewer_load_file && jpeg_viewer_load_file(w->filename) == 0 &&
            jpeg_viewer_is_loaded) {
            is_loaded = jpeg_viewer_is_loaded();
        }
    }

    if (is_loaded) {
        /* Draw the decoded image. */
        extern int jpeg_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
        if (jpeg_viewer_draw_scaled) {
            int draw_result = jpeg_viewer_draw_scaled((uint64_t)area_x, (uint64_t)area_y, (uint64_t)area_w, (uint64_t)area_h);
            if (draw_result != 0) {
                vga_fill_rounded_rect(area_x, area_y, area_w, area_h, 12, rgb(50, 30, 30));
                vga_draw_rounded_rect(area_x, area_y, area_w, area_h, 12, rgb(200, 80, 80));
                vga_draw_string(area_x + 16, area_y + 16, "Draw error", rgb(232, 238, 246), 0xFFFFFFFF);
            }
        } else {
            serial_puts("[JPEG_VIEW] ERROR: jpeg_viewer_draw_scaled function pointer is NULL\n");
            vga_fill_rounded_rect(area_x, area_y, area_w, area_h, 12, rgb(50, 30, 30));
            vga_draw_rounded_rect(area_x, area_y, area_w, area_h, 12, rgb(200, 80, 80));
            vga_draw_string(area_x + 16, area_y + 16, "Draw function not available", rgb(232, 238, 246), 0xFFFFFFFF);
        }

        /* Draw image info */
        uint64_t iw = 0, ih = 0;
        uint8_t ic = 0;
        extern int jpeg_viewer_get_info(uint64_t* width, uint64_t* height, uint8_t* components);
        if (jpeg_viewer_get_info && jpeg_viewer_get_info(&iw, &ih, &ic) == 0) {
            char info[128];
            char tmp[32];
            info[0] = '\0';
            {
                extern const char* jpeg_viewer_get_filename(void);
                if (jpeg_viewer_get_filename) {
                    const char* fn = jpeg_viewer_get_filename();
                    if (fn && fn[0]) {
                        scopy(info, fn, sizeof(info) - 1);
                    }
                }
            }
            if (info[0]) {
                scat(info, "  ", sizeof(info) - 1);
            }
            uitostr(iw, tmp); scat(info, tmp, sizeof(info) - 1);
            scat(info, "x", sizeof(info) - 1);
            uitostr(ih, tmp); scat(info, tmp, sizeof(info) - 1);
            scat(info, "  ", sizeof(info) - 1);
            uitostr(ic, tmp); scat(info, tmp, sizeof(info) - 1);
            scat(info, " ch", sizeof(info) - 1);
            vga_draw_string(w->x + 16, w->y + w->h - 22, info, rgb(160, 172, 188), 0xFFFFFFFF);
        }
    } else {
        /* Draw "no image" message */
        vga_fill_rounded_rect(area_x, area_y, area_w, area_h, 12, rgb(23, 28, 38));
        vga_draw_rounded_rect(area_x, area_y, area_w, area_h, 12, rgb(70, 82, 100));
        
        vga_draw_string(area_x + 16, area_y + 16, "No image loaded", rgb(232, 238, 246), 0xFFFFFFFF);
        vga_draw_string(area_x + 16, area_y + 40, "Double-click a .jpg, .png or .bmp file from File Manager", rgb(152, 164, 182), 0xFFFFFFFF);
        
        /* Show filename if trying to load something */
        if (w->filename[0] != '\0') {
            vga_draw_string(area_x + 16, area_y + 60, "Loading: ", rgb(200, 200, 100), 0xFFFFFFFF);
            vga_draw_string(area_x + 80, area_y + 60, w->filename, rgb(200, 200, 100), 0xFFFFFFFF);
        }
    }
}

void draw_music_player(int idx) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];

    /* Handle mouse wheel for volume */
    if (mouse.wheel != 0) {
        music_change_volume(mouse.wheel * 5);
    }

    if (w->w < 980) w->w = 980;
    if (w->h < 660) w->h = 660;
    if (w->x + w->w > (int)SCREEN_W) w->x = (int)SCREEN_W - w->w;
    if (w->y + w->h > (int)SCREEN_H - 40) w->y = (int)SCREEN_H - 40 - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    music_refresh_from_backend();
    music_sync_scroll_visible();

    music_layout_t L;
    music_build_layout(w, &L);

    vga_fill_rounded_rect(L.x, L.y, L.cw, L.ch, 18, rgb(10, 14, 22));
    vga_draw_rounded_rect(L.x, L.y, L.cw, L.ch, 18, rgb(54, 66, 82));
    vga_fill_rect(L.x + 2, L.y + 2, L.cw - 4, 26, rgb(16, 20, 30));
    vga_draw_string(L.x + 18, L.y + 10, "C-OS 4.0.8 alpha Audio", rgb(244, 248, 252), 0xFFFFFFFF);

    vga_fill_rounded_rect(L.list_x, L.list_y, L.list_w, L.list_h, 14, rgb(14, 18, 28));
    vga_draw_rounded_rect(L.list_x, L.list_y, L.list_w, L.list_h, 14, rgb(62, 74, 92));
    vga_draw_string(L.list_x + 14, L.list_y + 12, "Playlist", rgb(236, 242, 250), 0xFFFFFFFF);
    char countbuf[48];
    char tmp[16];
    scopy(countbuf, "Tracks ", sizeof(countbuf) - 1);
    uitostr((uint64_t)g_music.playlist_count, tmp);
    scat(countbuf, tmp, sizeof(countbuf) - 1);
    vga_draw_string(L.list_x + L.list_w - 102, L.list_y + 12, countbuf, rgb(120, 136, 160), 0xFFFFFFFF);

    int start_row = music_visible_start();
    int visible_rows = L.visible_rows;
    int rows_x = L.list_x + 10;
    int rows_y = L.list_y + 40;
    int rows_w = L.list_w - 28;
    int rows_h = visible_rows * L.row_h;
    for (int i = 0; i < visible_rows; ++i) {
        int track_index = start_row + i;
        if (track_index >= g_music.playlist_count) break;
        int ry = rows_y + i * L.row_h;
        music_draw_playlist_row(rows_x, ry, rows_w, L.row_h - 2, track_index,
                                track_index == g_music.current_index,
                                track_index == g_music.selected_index);
    }
    if (g_music.playlist_count == 0) {
        vga_draw_string(rows_x + 10, rows_y + 18, "Load a folder to build the playlist.", rgb(130, 146, 170), 0xFFFFFFFF);
    }
    music_draw_playlist_scrollbar(L.list_x + L.list_w - 14, rows_y, rows_h, visible_rows, g_music.playlist_count, start_row);
    if (g_music.playlist_count > visible_rows) {
        char rangebuf[64];
        char nbuf[16];
        char tbuf[16];
        uint64_t shown_from = (uint64_t)(start_row + 1);
        uint64_t shown_to = (uint64_t)((start_row + visible_rows) > g_music.playlist_count ? g_music.playlist_count : (start_row + visible_rows));
        scopy(rangebuf, "Showing ", sizeof(rangebuf) - 1);
        uitostr(shown_from, nbuf); scat(rangebuf, nbuf, sizeof(rangebuf) - 1);
        scat(rangebuf, "-", sizeof(rangebuf) - 1);
        uitostr(shown_to, tbuf); scat(rangebuf, tbuf, sizeof(rangebuf) - 1);
        scat(rangebuf, " / ", sizeof(rangebuf) - 1);
        uitostr((uint64_t)g_music.playlist_count, tbuf); scat(rangebuf, tbuf, sizeof(rangebuf) - 1);
        vga_draw_string(L.list_x + 14, L.list_y + L.list_h - 18, rangebuf, rgb(120, 136, 160), 0xFFFFFFFF);
    }

    vga_fill_rounded_rect(L.right_x, L.right_y, L.right_w, L.right_h, 14, rgb(14, 18, 28));
    vga_draw_rounded_rect(L.right_x, L.right_y, L.right_w, L.right_h, 14, rgb(62, 74, 92));

    vga_draw_string(L.right_x + 16, L.right_y + 12, "Now playing", rgb(236, 242, 250), 0xFFFFFFFF);
    const char* title = mk_mp3_get_current_title ? mk_mp3_get_current_title() : NULL;
    const char* artist = mk_mp3_get_current_artist ? mk_mp3_get_current_artist() : NULL;
    const char* album = mk_mp3_get_current_album ? mk_mp3_get_current_album() : NULL;
    uint64_t year = mk_mp3_get_current_year ? mk_mp3_get_current_year() : 0;
    mk_mp3_player_t* st = mk_mp3_get_player_state ? mk_mp3_get_player_state() : NULL;

    char line1[192];
    char yearbuf[16];
    line1[0] = '\0';
    if (title && title[0]) scopy(line1, title, sizeof(line1) - 1);
    else scopy(line1, music_basename(g_music.current_path), sizeof(line1) - 1);
    if (artist && artist[0]) { scat(line1, " - ", sizeof(line1) - 1); scat(line1, artist, sizeof(line1) - 1); }
    if (slen(line1) > 58) { line1[55] = '.'; line1[56] = '.'; line1[57] = '.'; line1[58] = '\0'; }
    if (album && album[0]) {
        scat(line1, " [", sizeof(line1) - 1);
        scat(line1, album, sizeof(line1) - 1);
        if (year > 0) { scopy(yearbuf, " ", sizeof(yearbuf) - 1); uitostr(year, yearbuf); scat(line1, yearbuf, sizeof(line1) - 1); }
        scat(line1, "]", sizeof(line1) - 1);
    }
    vga_draw_string(L.right_x + 16, L.right_y + 34, line1, rgb(244, 248, 252), 0xFFFFFFFF);
    vga_draw_string(L.right_x + 16, L.right_y + 54, g_music.status[0] ? g_music.status : "Ready", rgb(122, 138, 162), 0xFFFFFFFF);

    music_draw_disc(L.right_x + 88, L.right_y + 128, 58, g_music.playing || g_music.paused);
    music_draw_visualizer(L.right_x + 170, L.right_y + 82, L.right_w - 190, 48, g_music.playing);

    char timebuf[64];
    char posbuf[24], durbuf[24];
    music_format_time(g_music.position_ms, posbuf, sizeof(posbuf));
    music_format_time(g_music.duration_ms, durbuf, sizeof(durbuf));
    scopy(timebuf, posbuf, sizeof(timebuf) - 1);
    scat(timebuf, " / ", sizeof(timebuf) - 1);
    scat(timebuf, durbuf, sizeof(timebuf) - 1);
    vga_draw_string(L.progress_x, L.right_y + 220, timebuf, rgb(122, 138, 162), 0xFFFFFFFF);

    music_draw_progress(L.progress_x, L.progress_y, L.progress_w, 14, g_music.position_ms, g_music.duration_ms);
    vga_draw_string(L.progress_x, L.progress_y - 18, "Progress", rgb(152, 168, 192), 0xFFFFFFFF);

    char info[96];
    info[0] = '\0';
    if (st) {
        char nbuf[24];
        scopy(info, "Rate ", sizeof(info) - 1);
        uitostr(st->sample_rate, nbuf); scat(info, nbuf, sizeof(info) - 1);
        scat(info, " Hz  Ch ", sizeof(info) - 1);
        uitostr(st->channels, nbuf); scat(info, nbuf, sizeof(info) - 1);
        scat(info, "  Bitrate ", sizeof(info) - 1);
        uitostr(st->bitrate, nbuf); scat(info, nbuf, sizeof(info) - 1);
        scat(info, " kbps", sizeof(info) - 1);
    } else {
        scopy(info, "Audio info unavailable", sizeof(info) - 1);
    }
    /* These chips used to be drawn at right_y + 246, which overlapped the
     * progress bar directly above it (right_y + 248, 14px tall) and the
     * volume slider directly below it (right_y + 330). They now start after
     * the progress bar ends, with the volume slider (see music_build_layout)
     * moved further down to clear the "Mode"/"Index" row below. */
    music_draw_info_chip(L.progress_x, L.right_y + 270, L.right_w - 36, "Audio", info);

    char status_chip[96];
    scopy(status_chip, g_music.repeat ? "Repeat on" : "Repeat off", sizeof(status_chip) - 1);
    scat(status_chip, g_music.shuffle ? "   Shuffle on" : "   Shuffle off", sizeof(status_chip) - 1);
    music_draw_info_chip(L.progress_x, L.right_y + 318, (L.right_w - 48) / 2, "Mode", status_chip);
    char track_chip[96];
    scopy(track_chip, "Track ", sizeof(track_chip) - 1);
    uitostr((uint64_t)((g_music.current_index >= 0 ? g_music.current_index : 0) + 1), tmp);
    scat(track_chip, tmp, sizeof(track_chip) - 1);
    scat(track_chip, " / ", sizeof(track_chip) - 1);
    uitostr((uint64_t)(g_music.playlist_count > 0 ? g_music.playlist_count : 0), tmp);
    scat(track_chip, tmp, sizeof(track_chip) - 1);
    music_draw_info_chip(L.progress_x + (L.right_w - 48) / 2 + 12, L.right_y + 318, (L.right_w - 48) / 2, "Index", track_chip);

    /* "Volume NN%" is drawn just above the slider below; the old separate
     * plain "Volume" label at right_y + 356 sat visually inside the slider
     * track (right_y + 330..342) and duplicated this one, so it was removed
     * rather than repositioned again. */
    vga_fill_rounded_rect(L.volume_x, L.volume_y, L.volume_w, 12, 6, rgb(24, 30, 40));
    int vol_fill = (int)((g_music.volume * (uint64_t)L.volume_w) / 100ULL);
    if (vol_fill < 0) vol_fill = 0;
    if (vol_fill > L.volume_w) vol_fill = L.volume_w;
    vga_fill_rounded_rect(L.volume_x, L.volume_y, vol_fill, 12, 6, rgb(76, 186, 255));
    vga_draw_rounded_rect(L.volume_x, L.volume_y, L.volume_w, 12, 6, rgb(62, 74, 90));
    int knob_x = L.volume_x + vol_fill;
    if (knob_x < L.volume_x + 6) knob_x = L.volume_x + 6;
    if (knob_x > L.volume_x + L.volume_w - 6) knob_x = L.volume_x + L.volume_w - 6;
    vga_fill_circle(knob_x, L.volume_y + 6, 6, rgb(242, 248, 255));
    vga_draw_circle(knob_x, L.volume_y + 6, 6, rgb(18, 24, 34));
    char voltxt[32];
    scopy(voltxt, "Volume ", sizeof(voltxt) - 1);
    uitostr(g_music.volume, tmp); scat(voltxt, tmp, sizeof(voltxt) - 1); scat(voltxt, "%", sizeof(voltxt) - 1);
    vga_draw_string(L.right_x + 16, L.volume_y - 18, voltxt, rgb(236, 242, 250), 0xFFFFFFFF);

    int btn_y = L.control_y;
    music_draw_button(L.right_x + 16, btn_y, 70, 28, "Prev", false, false);
    music_draw_button(L.right_x + 92, btn_y, 86, 28, g_music.playing ? "Pause" : "Play", g_music.playing || g_music.paused, true);
    music_draw_button(L.right_x + 184, btn_y, 68, 28, "Stop", false, false);
    music_draw_button(L.right_x + 258, btn_y, 70, 28, "Next", false, false);
    music_draw_button(L.right_x + 334, btn_y, 96, 28, g_music.repeat ? "Repeat on" : "Repeat off", g_music.repeat, false);
    music_draw_button(L.right_x + 436, btn_y, 102, 28, g_music.shuffle ? "Shuffle on" : "Shuffle off", g_music.shuffle, false);

    music_draw_button(L.right_x + 16, btn_y + 40, 74, 24, "Vol -", false, false);
    music_draw_button(L.right_x + 96, btn_y + 40, 74, 24, "Vol +", false, false);
    music_draw_button(L.right_x + 180, btn_y + 40, 118, 24, "Play selected", false, true);
    music_draw_button(L.right_x + 304, btn_y + 40, 118, 24, "Reload folder", false, false);

    draw_statusbar(L.x, L.y + L.ch - C_STATUSBAR_H - 2, L.cw, g_music.status, g_music.current_dir[0] ? g_music.current_dir : "/");
}





