/* main.c - Files: entry point and event loop. */
#include "files.h"

int main(int argc, char **argv) {
    app_init_state();
    G.win = cos_win2_create("File Manager", WIN_W, WIN_H, &G.info);
    if (G.win <= 0) { cos_printf("files: cannot create a window\n"); return 1; }
    cui_canvas_init(&G.cv, G.info.pixels, G.info.width, G.info.height, G.info.stride);

    char start[PATHN];
    snprintf(start, sizeof start, "%s", G.path);                           /* the folder we were in last time */
    if (argc > 1 && argv[1] && argv[1][0]) snprintf(start, sizeof start, "%s", argv[1]);
    if (!is_dir_path(start)) snprintf(start, sizeof start, "/");
    tree_rebuild();
    nav_to(start, true);
    G.dirty = true;

    int last_phase = -1; bool tip_shown = false, status_shown = false;
    while (!G.quit) {
        cos_win_event_t ev;
        memset(&ev, 0, sizeof ev);
        int r = cos_win2_wait(G.win, &ev, 30);
        if (r < 0) break;
        if (r > 0) {
            do { input_event(&ev); memset(&ev, 0, sizeof ev); } while (!G.quit && cos_win2_poll(G.win, &ev) > 0);
        }
        job_tick();
        thumb_load_pending();
        dir_poll();
        uint64_t now = cos_time_ms();
        bool typing = G.addr_edit || G.search_focus || G.dlg == D_INPUT;
        int phase = typing ? (int)(((now - G.caret_t) / 530) & 1) : -1;
        if (phase != last_phase) { last_phase = phase; G.dirty = true; }
        bool tip_now = G.tip_btn > 0 && now - G.tip_t > 450;
        if (tip_now != tip_shown) { tip_shown = tip_now; G.dirty = true; }
        bool st_now = G.status[0] && now - G.status_t < 5000;
        if (st_now != status_shown) { status_shown = st_now; G.dirty = true; }
        if (G.job.mode) G.dirty = true;
        if (G.dirty) { ui_draw(); cos_win2_present(G.win); G.dirty = false; }
    }
    config_save();
    cos_win2_close(G.win);
    return 0;
}
