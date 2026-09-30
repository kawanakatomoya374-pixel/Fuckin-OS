/* gui_file_drop.c - where a file dragged out of the File Manager lands.
 *
 * Called by efm_winui_tick() when a drag is released outside the source
 * window. The topmost window under the pointer decides:
 *   File Manager  -> move (Ctrl: copy) into the folder it shows
 *   ring-3 app    -> COS_EV_DROP with the path (e.g. the music player plays it)
 *   other app     -> open the file with its default application
 *   no window     -> the desktop: move (Ctrl: copy) into /desktop
 */
#include "gui.h"
#include "gui_internal.h"
#include "serial.h"
#include "../file_manager/enhanced_file_manager.h"
#include "../../kernel/cos_app_window.h"

extern void gui_notify(const char* msg, int ms);
extern void gui_open_file_in_app(const char* path, int file_type);
extern void gui_refresh_desktop_icons(void);
extern void fm_bridge_refresh_all(void);
extern bool gui_is_japanese(void);

void gui_handle_file_drop(void* source_window, int mx, int my, const char (*paths)[EFM_MAX_PATH], int count, bool copy) {
    if (!paths || count <= 0) return;
    int target = -1;
    for (int i = window_count - 1; i >= 0; --i) {
        window_t* w = &windows[i];
        if ((void*)w == source_window) continue;
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) { target = i; break; }
    }
    if (target < 0) {
        efm_drop_paths_into(paths, count, "/desktop", copy);
        gui_refresh_desktop_icons();
        fm_bridge_refresh_all();
        return;
    }
    window_t* w = &windows[target];
    if (w->kind == WIN_FILE_MGR) {
        efm_drop_paths_into(paths, count, w->fm_path[0] ? w->fm_path : "/", copy);
        fm_bridge_refresh_all();
        gui_bring_to_front(target);
        return;
    }
    if (w->kind == WIN_COS_APP) {
        if (cos_app_window_deliver_drop_uid(w->uid, paths[0], mx - w->x, my - w->y - TITLEBAR_H)) {
            if (count > 1) gui_notify(gui_is_japanese() ? "アプリには最初のファイルだけを渡しました" : "Only the first file was given to the app", 2000);
            return;
        }
    }
    /* any other application: open with its default app */
    gui_open_file_in_app(paths[0], 0);
}
