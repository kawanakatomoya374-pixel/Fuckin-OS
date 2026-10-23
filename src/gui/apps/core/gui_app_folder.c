/**
 * gui_app_folder.c - iOS-style "app folder" desktop icons.
 *
 * A folder is a desktop icon (win_kind == WIN_APP_FOLDER) that holds
 * other apps instead of launching one itself. Dragging one app icon
 * onto another (or onto an existing folder) merges them - the exact
 * "drag app A onto app B" gesture iOS introduced folders with. Opening
 * a folder plays a spring-eased grow animation: the folder's rounded
 * tile grows from its desktop position into a centered panel while
 * each contained app's mini icon (from the tile's 3x3 preview) flies
 * out to its full-size position in the open grid, growing as it goes.
 * Closing plays a quicker, non-bouncy shrink back to the tile. An item
 * can be dragged back out onto the desktop to remove it from the
 * folder.
 *
 * Persistence: app_folders[] is part of gui_desktop_snapshot_t (see
 * GUI_DESKTOP_SNAPSHOT_VERSION in gui_apps_core.c, bumped when this was
 * added) - a desktop_icons[] entry with win_kind==WIN_APP_FOLDER only
 * makes sense together with the app_folders[] slot its folder_id
 * points at, so both are saved/loaded together.
 */
#include "gui.h"
#include "gui_internal.h"
#include "vga.h"
#include "timer.h"
#include "mouse.h"
#include <string.h>
#include <math.h>

app_folder_t app_folders[MAX_APP_FOLDERS];

/* ---- animation state --------------------------------------------------
 * g_open_icon_idx is the desktop_icons[] index the open/opening/closing
 * folder grew from (or is shrinking back to) - both its rect (source of
 * the grow animation) and its app_folders[] contents (folder_id) come
 * from that one desktop icon, so this single index is all the state
 * needed to know "which folder, and where its tile is". -1 means fully
 * closed - no overlay, no dimming, desktop input behaves normally. */
static int      g_open_icon_idx = -1;
static bool     g_closing = false;
static uint64_t g_phase_start_ms = 0;
static float    g_phase_start_val = 0.0f;   /* animation value when the current phase (open or close) began */
static float    g_phase_target_val = 0.0f;  /* 1.0 = fully open, 0.0 = fully closed */
static float    g_last_val = 0.0f;          /* last computed value, reused for open/close rect math below */

#define FOLDER_ANIM_OPEN_MS  260
#define FOLDER_ANIM_CLOSE_MS 190

/* Drag-out-of-folder state: mouse-down on an open folder's item starts
 * a "candidate" drag; past gui_mouse_drag_threshold with the button
 * still held, it becomes a real drag showing that one icon following
 * the cursor. Releasing outside the panel removes it from the folder
 * and drops it on the desktop; releasing inside (or without having
 * dragged) is treated as "launch this app" instead. */
static int  g_drag_item = -1;
static bool g_dragging_item = false;
static int  g_drag_anchor_x = 0, g_drag_anchor_y = 0;

extern int gui_mouse_drag_threshold;

/* Springy "overshoot then settle" curve for opening - the small bounce
 * that makes iOS's folder-open read as springy rather than mechanical.
 * Closing deliberately uses a plain ease-in instead (see
 * gui_app_folder_request_close()) - a bounce reads right when
 * something is arriving and settling, not when it's retreating. */
static float ease_out_back(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float t1 = t - 1.0f;
    return 1.0f + c3 * t1 * t1 * t1 + c1 * t1 * t1;
}

static float ease_in_cubic(float t) {
    return t * t * t;
}

static float compute_anim_value(void) {
    uint64_t now = get_timer_ticks();
    uint64_t elapsed = now - g_phase_start_ms;
    uint64_t duration = g_closing ? FOLDER_ANIM_CLOSE_MS : FOLDER_ANIM_OPEN_MS;
    float t = (float)elapsed / (float)duration;
    if (t > 1.0f) t = 1.0f;
    if (t < 0.0f) t = 0.0f;
    float eased = g_closing ? ease_in_cubic(t) : ease_out_back(t);
    return g_phase_start_val + (g_phase_target_val - g_phase_start_val) * eased;
}

/* ---- data model --------------------------------------------------- */

static int folder_alloc(void) {
    for (int i = 0; i < MAX_APP_FOLDERS; ++i) {
        if (!app_folders[i].in_use) return i;
    }
    return -1;
}

bool gui_app_folder_add_item(int folder_id, int kind, const char* label) {
    if (folder_id < 0 || folder_id >= MAX_APP_FOLDERS) return false;
    app_folder_t* f = &app_folders[folder_id];
    if (!f->in_use || f->count >= MAX_APP_FOLDER_ITEMS) return false;
    f->win_kind[f->count] = kind;
    strncpy(f->label[f->count], label ? label : "", sizeof(f->label[f->count]) - 1);
    f->label[f->count][sizeof(f->label[f->count]) - 1] = '\0';
    f->count++;
    return true;
}

/* Removes item_idx from the folder, shifting later items down, and
 * returns its kind/label so the caller can place it back on the
 * desktop. Returns false (nothing removed) for an out-of-range index. */
static bool folder_remove_item(int folder_id, int item_idx, int* out_kind, char* out_label, size_t out_label_size) {
    if (folder_id < 0 || folder_id >= MAX_APP_FOLDERS) return false;
    app_folder_t* f = &app_folders[folder_id];
    if (item_idx < 0 || item_idx >= f->count) return false;
    if (out_kind) *out_kind = f->win_kind[item_idx];
    if (out_label && out_label_size > 0) {
        strncpy(out_label, f->label[item_idx], out_label_size - 1);
        out_label[out_label_size - 1] = '\0';
    }
    for (int i = item_idx; i + 1 < f->count; ++i) {
        f->win_kind[i] = f->win_kind[i + 1];
        memcpy(f->label[i], f->label[i + 1], sizeof(f->label[i]));
    }
    f->count--;
    return true;
}

static void desktop_icon_remove_at(int idx) {
    if (idx < 0 || idx >= desktop_icon_count) return;
    for (int i = idx; i + 1 < desktop_icon_count; ++i) {
        desktop_icons[i] = desktop_icons[i + 1];
    }
    if (desktop_icon_count > 0) {
        memset(&desktop_icons[desktop_icon_count - 1], 0, sizeof(desktop_icons[0]));
        desktop_icon_count--;
    }
}

/* Places a plain app icon (not a file - path stays empty) at the given
 * desktop pixel position, snapped to the grid like every other icon
 * drag/drop already does. Used both when merging two apps into a new
 * folder (the surviving slot becomes the folder, so nothing needs
 * re-adding there) and when dragging an item back out of an open
 * folder onto the desktop. */
static int add_app_icon_to_desktop(int kind, const char* label, int drop_x, int drop_y) {
    if (desktop_icon_count >= MAX_ICONS) return -1;
    int idx = desktop_icon_count++;
    desktop_icon_t* ic = &desktop_icons[idx];
    memset(ic, 0, sizeof(*ic));
    ic->x = drop_x;
    ic->y = drop_y;
    strncpy(ic->label, label ? label : "", sizeof(ic->label) - 1);
    ic->win_kind = kind;
    ic->is_file = false;
    ic->is_dynamic = false;
    ic->folder_id = 0;
    gui_snap_desktop_icon_position(&ic->x, &ic->y, idx);
    return idx;
}

/* The core "drag app A onto app B" gesture: called from gui_input.c's
 * drop handling when both the dragged icon and the icon under the
 * cursor are plain (non-file) app icons, or the target is already a
 * folder. Returns true if it consumed the drop (source_idx has been
 * removed from the desktop either way - it is now inside a folder). */
bool gui_app_folder_try_merge(int source_idx, int target_idx) {
    if (source_idx < 0 || source_idx >= desktop_icon_count) return false;
    if (target_idx < 0 || target_idx >= desktop_icon_count) return false;
    if (source_idx == target_idx) return false;
    desktop_icon_t* source = &desktop_icons[source_idx];
    desktop_icon_t* target = &desktop_icons[target_idx];
    if (source->is_file || source->win_kind == WIN_APP_FOLDER) return false;

    if (target->win_kind == WIN_APP_FOLDER) {
        if (!gui_app_folder_add_item(target->folder_id, source->win_kind, source->label)) {
            return false; /* folder full - leave both icons where they are */
        }
        desktop_icon_remove_at(source_idx);
        return true;
    }

    if (target->is_file) return false; /* file-onto-file is not a folder merge */

    int fid = folder_alloc();
    if (fid < 0) return false;
    app_folder_t* f = &app_folders[fid];
    memset(f, 0, sizeof(*f));
    f->in_use = true;
    strncpy(f->name, "App Folder", sizeof(f->name) - 1);
    gui_app_folder_add_item(fid, target->win_kind, target->label);
    gui_app_folder_add_item(fid, source->win_kind, source->label);

    /* The target's slot becomes the folder (keeps its desktop
     * position); the source is removed. Order matters: source_idx may
     * be either side of target_idx, so remove whichever index is
     * larger first to keep the other index valid. */
    target->win_kind = WIN_APP_FOLDER;
    target->folder_id = fid;
    strncpy(target->label, f->name, sizeof(target->label) - 1);
    target->is_file = false;
    target->is_dynamic = false;
    if (source_idx > target_idx) {
        desktop_icon_remove_at(source_idx);
    } else {
        desktop_icon_remove_at(source_idx);
        /* target_idx shifted down by one since it was after source_idx. */
    }
    gui_request_redraw();
    return true;
}

/* ---- geometry: mini (closed-tile) and full (open-panel) item slots -- */

#define FOLDER_GRID_N 3 /* 3x3 preview - the classic iPhone folder look */

static void folder_mini_slot(int item_idx, int box_x, int box_y, int box_size,
                              int* out_x, int* out_y, int* out_size) {
    float pad = box_size * 0.10f;
    float gap = box_size * 0.05f;
    float cell = (box_size - 2.0f * pad - (FOLDER_GRID_N - 1) * gap) / (float)FOLDER_GRID_N;
    if (cell < 2.0f) cell = 2.0f;
    int col = item_idx % FOLDER_GRID_N;
    int row = item_idx / FOLDER_GRID_N;
    *out_x = box_x + (int)(pad + col * (cell + gap));
    *out_y = box_y + (int)(pad + row * (cell + gap));
    *out_size = (int)cell;
}

static void folder_full_slot(int item_idx, int total, int panel_x, int panel_y, int panel_w, int panel_h,
                              int* out_x, int* out_y, int* out_size) {
    int cols = total <= 4 ? 2 : (total <= 9 ? 3 : 4);
    int rows = (total + cols - 1) / cols;
    if (rows < 1) rows = 1;
    int icon_size = 56;
    /* Fixed, compact cell size - matches how a real iOS folder packs
     * its icons close together in the middle regardless of panel size,
     * rather than stretching cells to fill however wide the panel
     * happens to be (which used to spread a 2-item folder's icons out
     * near the panel's opposite edges). The grid is then centered
     * within the panel. */
    int cell_w = 100;
    int cell_h = 96;
    int grid_w = cols * cell_w;
    int grid_h = rows * cell_h;
    int grid_x = panel_x + (panel_w - grid_w) / 2;
    int grid_y = panel_y + (panel_h - grid_h) / 2;
    int col = item_idx % cols;
    int row = item_idx / cols;
    *out_size = icon_size;
    *out_x = grid_x + col * cell_w + (cell_w - icon_size) / 2;
    *out_y = grid_y + row * cell_h + (cell_h - icon_size) / 2 - 6;
}

/* ---- closed-tile rendering (called from draw_desktop_icons()) ------ */

void gui_draw_app_folder_tile(int folder_id, int x, int y, int size, bool hov) {
    if (folder_id < 0 || folder_id >= MAX_APP_FOLDERS) return;
    const app_folder_t* f = &app_folders[folder_id];

    /* A soft frosted-glass tile, deliberately neutral rather than any
     * single app's color - it is a container, not an app. */
    uint64_t top = rgb(226, 230, 238);
    uint64_t bot = rgb(196, 202, 216);
    if (hov) { top = lighten(top, 12); bot = lighten(bot, 10); }
    int r = size / 5;
    if (r < 4) r = 4;
    /* Frosted glass, not a flat opaque card: blend toward the tile's
     * two-tone gradient colors rather than fully replacing whatever is
     * behind it (the desktop wallpaper) - real iOS folder tiles are
     * translucent even closed. Kept mostly-opaque (alpha 0.82) so the
     * mini icons drawn on top stay clearly legible. */
    vga_blend_rounded_rect(x, y, size, size, r, bot, 0.82f);
    vga_blend_rounded_rect(x, y, size, size - size / 6, r, top, 0.82f);
    vga_blend_rounded_rect(x, y, size, size, r, rgb(255, 255, 255), 0.10f);
    vga_draw_rounded_rect(x, y, size, size, r, darken(bot, 24));

    int shown = f->count < 9 ? f->count : 9;
    for (int i = 0; i < shown; ++i) {
        int ix, iy, isz;
        folder_mini_slot(i, x, y, size, &ix, &iy, &isz);
        if (isz < 3) continue; /* too small to render meaningfully - skip rather than smear a 1px blob */
        gui_draw_app_icon(f->win_kind[i], ix, iy, isz, false);
    }
}

/* ---- open/close control --------------------------------------------- */

bool gui_app_folder_is_active(void) {
    return g_open_icon_idx >= 0;
}

bool gui_app_folder_is_settled_open(void) {
    return g_open_icon_idx >= 0 && !g_closing && g_last_val >= 0.999f;
}

void gui_app_folder_open(int desktop_icon_index) {
    if (desktop_icon_index < 0 || desktop_icon_index >= desktop_icon_count) return;
    if (desktop_icons[desktop_icon_index].win_kind != WIN_APP_FOLDER) return;
    g_open_icon_idx = desktop_icon_index;
    g_closing = false;
    g_phase_start_ms = get_timer_ticks();
    g_phase_start_val = 0.0f;
    g_phase_target_val = 1.0f;
    g_last_val = 0.0f;
    g_drag_item = -1;
    g_dragging_item = false;
    gui_request_redraw();
}

void gui_app_folder_request_close(void) {
    if (g_open_icon_idx < 0 || g_closing) return;
    g_closing = true;
    g_phase_start_ms = get_timer_ticks();
    g_phase_start_val = g_last_val;
    g_phase_target_val = 0.0f;
    g_drag_item = -1;
    g_dragging_item = false;
    gui_request_redraw();
}

void gui_app_folder_update(void) {
    if (g_open_icon_idx < 0) return;
    g_last_val = compute_anim_value();
    if (g_closing && g_last_val <= 0.001f) {
        g_open_icon_idx = -1;
        g_closing = false;
        return;
    }
    /* Keep redrawing every frame while animating (the tile/panel/mini
     * icons are all mid-tween) - once settled open, the overlay is
     * static and does not need to force redraws on its own. */
    if (g_closing || g_last_val < 0.999f) {
        gui_request_redraw();
    }
}

/* ---- open overlay rendering ------------------------------------------ */

static void folder_panel_rect(int* px, int* py, int* pw, int* ph) {
    *pw = (int)(SCREEN_W * 0.56f);
    *ph = (int)(SCREEN_H * 0.52f);
    *px = ((int)SCREEN_W - *pw) / 2;
    *py = ((int)SCREEN_H - *ph) / 2;
}

void gui_draw_app_folder_overlay(void) {
    if (g_open_icon_idx < 0) return;
    if (g_open_icon_idx >= desktop_icon_count) { g_open_icon_idx = -1; return; }
    desktop_icon_t* ic = &desktop_icons[g_open_icon_idx];
    if (ic->win_kind != WIN_APP_FOLDER) { g_open_icon_idx = -1; return; }
    const app_folder_t* f = &app_folders[ic->folder_id];

    float v = g_last_val;
    float vc = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); /* clamped copy for dimming/alpha-like uses */

    vga_dim_screen(0.55f * vc);

    int box = gui_get_desktop_icon_render_size();
    int sx = ic->x, sy = ic->y, sw = box, sh = box;
    int px, py, pw, ph;
    folder_panel_rect(&px, &py, &pw, &ph);

    int cx = sx + (int)((px - sx) * v);
    int cy = sy + (int)((py - sy) * v);
    int cw = sw + (int)((pw - sw) * v);
    int ch = sh + (int)((ph - sh) * v);
    if (cw < 4) cw = 4;
    if (ch < 4) ch = 4;

    int r = cw / 14;
    if (r < 6) r = 6;
    /* Frosted-glass open panel: blend toward a light neutral color
     * instead of an opaque fill, so the (already dimmed) desktop
     * behind still shows through softly - real iOS's folder panel is
     * a translucent blur, not a solid card. Alpha ramps in with the
     * open animation (v) so it does not appear instantly at full
     * strength before the panel has finished growing. */
    vga_blend_rounded_rect(cx, cy, cw, ch, r, rgb(238, 240, 245), 0.72f * vc);
    vga_draw_rounded_rect(cx, cy, cw, ch, r, rgb(190, 194, 204));

    if (vc > 0.15f) {
        char title_buf[40];
        int tw;
        strncpy(title_buf, f->name, sizeof(title_buf) - 1);
        title_buf[sizeof(title_buf) - 1] = '\0';
        tw = (int)strlen(title_buf) * FONT_W;
        vga_draw_string(cx + (cw - tw) / 2, cy + 14, title_buf, rgb(70, 74, 84), 0xFFFFFFFF);
    }

    int mx = mouse.x;
    int my = mouse.y;
    int inner_y = cy + 40;
    int inner_h = ch - 56;

    for (int i = 0; i < f->count; ++i) {
        if (g_dragging_item && i == g_drag_item) continue; /* drawn separately, following the cursor */

        int fx, fy, fsz;
        folder_full_slot(i, f->count, cx, inner_y, cw, inner_h, &fx, &fy, &fsz);
        int mxs, mys, msz;
        folder_mini_slot(i < 9 ? i : 8, sx, sy, sw, &mxs, &mys, &msz);

        int ix = mxs + (int)((fx - mxs) * v);
        int iy = mys + (int)((fy - mys) * v);
        int isz = msz + (int)((fsz - msz) * v);
        if (isz < 3) continue;

        bool item_hov = gui_app_folder_is_settled_open() &&
                         mx >= fx && mx < fx + fsz && my >= fy && my < fy + fsz + 20;
        gui_draw_app_icon(f->win_kind[i], ix, iy, isz, item_hov);

        if (vc >= 0.98f) {
            char label_buf[20];
            strncpy(label_buf, f->label[i], sizeof(label_buf) - 1);
            label_buf[sizeof(label_buf) - 1] = '\0';
            int lw = (int)strlen(label_buf) * FONT_W;
            vga_draw_string(fx + (fsz - lw) / 2, fy + fsz + 4, label_buf, rgb(80, 84, 94), 0xFFFFFFFF);
        }
    }

    if (g_dragging_item && g_drag_item >= 0 && g_drag_item < f->count) {
        int isz = 56;
        gui_draw_app_icon(f->win_kind[g_drag_item], mx - isz / 2, my - isz / 2, isz, true);
    }
}

/* ---- input: called from gui_input.c whenever a folder is open/animating.
 * Returns true if the event was consumed (caller should skip its own
 * desktop/window handling for this frame). */
bool gui_app_folder_handle_input(int mx, int my, bool left_down_edge, bool left_up_edge, bool left_held) {
    if (g_open_icon_idx < 0) return false;

    /* While opening/closing, swallow input so nothing underneath reacts
     * to a click that landed mid-animation. */
    if (!gui_app_folder_is_settled_open() && !g_closing) {
        return left_down_edge || left_up_edge;
    }
    if (g_closing) {
        return left_down_edge || left_up_edge;
    }

    desktop_icon_t* ic = &desktop_icons[g_open_icon_idx];
    app_folder_t* f = &app_folders[ic->folder_id];
    int cx, cy, cw, ch;
    folder_panel_rect(&cx, &cy, &cw, &ch);
    int inner_y = cy + 40;
    int inner_h = ch - 56;

    if (g_dragging_item) {
        if (left_up_edge) {
            bool inside = mx >= cx && mx < cx + cw && my >= cy && my < cy + ch;
            int kind = 0;
            char label[32];
            if (!inside) {
                if (folder_remove_item(ic->folder_id, g_drag_item, &kind, label, sizeof(label))) {
                    add_app_icon_to_desktop(kind, label, mx - 24, my - 24);
                    gui_notify_simple(gui_text("Removed from folder", "フォルダから取り出しました"));
                }
            }
            g_dragging_item = false;
            g_drag_item = -1;
            gui_request_redraw();
        }
        return true;
    }

    if (left_down_edge) {
        for (int i = 0; i < f->count; ++i) {
            int fx, fy, fsz;
            folder_full_slot(i, f->count, cx, inner_y, cw, inner_h, &fx, &fy, &fsz);
            if (mx >= fx && mx < fx + fsz && my >= fy && my < fy + fsz + 20) {
                g_drag_item = i;
                g_drag_anchor_x = mx;
                g_drag_anchor_y = my;
                return true;
            }
        }
        bool inside_panel = mx >= cx && mx < cx + cw && my >= cy && my < cy + ch;
        if (!inside_panel) {
            gui_app_folder_request_close();
        }
        return true;
    }

    if (g_drag_item >= 0 && left_held) {
        int dx = mx - g_drag_anchor_x, dy = my - g_drag_anchor_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx >= gui_mouse_drag_threshold || dy >= gui_mouse_drag_threshold) {
            g_dragging_item = true;
            gui_request_redraw();
        }
        return true;
    }

    if (g_drag_item >= 0 && left_up_edge) {
        /* Released without moving past the threshold - a plain click:
         * launch the app and close the folder, mirroring how tapping
         * an app inside an iOS folder opens it. */
        int kind = f->win_kind[g_drag_item];
        const char* label = f->label[g_drag_item];
        int existing = gui_find_window(kind);
        if (existing >= 0) {
            gui_bring_to_front(existing);
        } else {
            gui_open_window(kind, label, 120, 100, 700, 500);
        }
        g_drag_item = -1;
        gui_app_folder_request_close();
        return true;
    }

    return left_down_edge || left_up_edge || left_held;
}
