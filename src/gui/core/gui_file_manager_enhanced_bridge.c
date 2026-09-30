/**
 * gui_file_manager_enhanced_bridge.c
 *
 * C-OS 4.0.8 alpha
 *
 * enhanced_file_manager.c (efm_*) を実際のデスクトップGUIの
 * 「File Manager」ウィンドウ (WIN_FILE_MGR) で使えるようにする橋渡し層。
 *
 * これまで enhanced_file_manager.c はビルドはされていたものの、
 * どこからも呼び出されていなかった (デッドコード)。本ファイルは
 * draw_file_manager() / handle_file_manager_click() /
 * handle_file_manager_key() を efm_* API 経由の実装に差し替える。
 *
 * 旧実装は draw_file_manager_legacy() / handle_file_manager_click_legacy() /
 * handle_file_manager_key_legacy() としてそのまま残してあるので、
 * 何か問題があれば下の USE_ENHANCED_FILE_MANAGER を 0 にするだけで
 * 即座に旧実装へ戻せる。
 */

#include "gui.h"
#include "enhanced_file_manager.h"
#include "enhanced_context_menu.h"
#include "mouse.h"
#include "serial.h"
#include "memory.h"
#include "../../include/string.h"
#include "../../kernel/ai/lua_config.h"
#include <string.h>

#ifndef USE_ENHANCED_FILE_MANAGER
#define USE_ENHANCED_FILE_MANAGER 1
#endif

/* 旧実装 (フォールバック用。関数名を変更して残している) */
void draw_file_manager_legacy(int idx);
void handle_file_manager_click_legacy(int idx, int mx, int my);
void handle_file_manager_key_legacy(int idx, int key, char ascii, bool ctrl);

/* window_t のスロットごとに efm_state_t を保持する */
static efm_state_t  g_efm_states[MAX_WINDOWS];
/* Per-window state is keyed by window_t.uid, not by the window's index:
 * windows[] is compacted on close and reordered by gui_bring_to_front(), so
 * an index-keyed slot followed whichever window happened to move into that
 * position (and a closed window's state was never released - the release
 * function had no callers). A slot whose window no longer exists is
 * reclaimed on the next allocation. */
static uint32_t     g_efm_uid[MAX_WINDOWS];
static int fm_slot(window_t* w);
static bool          g_efm_inited[MAX_WINDOWS];
static char          g_efm_last_path[MAX_WINDOWS][EFM_MAX_PATH];

static bool fm_bridge_is_lua_script(const char* path) {
    if (!path || path[0] == '\0') return false;
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    return strcmp(dot, ".lua") == 0 || strcmp(dot, ".luac") == 0;
}

static bool fm_bridge_request_lua_script(efm_state_t* st, const char* path) {
    if (!st || !fm_bridge_is_lua_script(path)) return false;
    char reason[160];
    bool ok = cos_lua_request_run(path, reason, sizeof(reason));
    efm_set_status(st, reason, 2500);
    return ok || reason[0] != '\0';
}

/* ダブルクリック検出用 (フォルダを開く/ファイルを開くために必要) */
static int      g_efm_last_click_row[MAX_WINDOWS];
static uint64_t g_efm_last_click_time[MAX_WINDOWS];

static int fm_slot(window_t* w) {
    if (!w) return 0;
    for (int s = 0; s < MAX_WINDOWS; ++s) if (g_efm_uid[s] == w->uid) return s;
    for (int s = 0; s < MAX_WINDOWS; ++s) {
        if (g_efm_uid[s] == 0 || gui_window_index_by_uid(g_efm_uid[s]) < 0) {
            if (g_efm_uid[s] && g_efm_inited[s]) efm_cleanup(&g_efm_states[s]);
            g_efm_uid[s] = w->uid;
            g_efm_inited[s] = false;
            return s;
        }
    }
    return 0;
}

static efm_state_t* fm_bridge_get_state(int idx, window_t* w) {
    if (idx < 0 || idx >= MAX_WINDOWS || !w) return NULL;

    if (!g_efm_inited[fm_slot(w)]) {
        memset(&g_efm_states[fm_slot(w)], 0, sizeof(efm_state_t));
        efm_init(&g_efm_states[fm_slot(w)]);
        const char* start_path = (w->fm_path[0] != '\0') ? w->fm_path : "/";
        efm_navigate(&g_efm_states[fm_slot(w)], start_path);
        cos_strlcpy(g_efm_last_path[fm_slot(w)], start_path, sizeof(g_efm_last_path[fm_slot(w)]));
        g_efm_last_path[fm_slot(w)][EFM_MAX_PATH - 1] = '\0';
        g_efm_inited[fm_slot(w)] = true;
        g_efm_last_click_row[fm_slot(w)] = -1;
        g_efm_last_click_time[fm_slot(w)] = 0;
        serial_puts("[EFM_BRIDGE] efm_state_t initialized for File Manager window\n");
    }

    /* 他の場所 (アドレスバー直接編集やコンテキストメニュー等) から
     * w->fm_path が書き換えられた場合に追従する */
    if (w->fm_path[0] != '\0' &&
        strncmp(w->fm_path, g_efm_last_path[fm_slot(w)], EFM_MAX_PATH) != 0) {
        efm_navigate(&g_efm_states[fm_slot(w)], w->fm_path);
        cos_strlcpy(g_efm_last_path[fm_slot(w)], w->fm_path, sizeof(g_efm_last_path[fm_slot(w)]));
        g_efm_last_path[fm_slot(w)][EFM_MAX_PATH - 1] = '\0';
    }

    return &g_efm_states[fm_slot(w)];
}

/* efm 側の current_path を window_t 側にも反映しておく
 * (タイトルバーやタブ、他コンポーネントが w->fm_path を参照するため) */
static void fm_bridge_sync_path(int idx, window_t* w, efm_state_t* st) {
    if (!w || !st) return;
    if (strncmp(w->fm_path, st->current_path, sizeof(w->fm_path)) != 0) {
        cos_strlcpy(w->fm_path, st->current_path, sizeof(w->fm_path));
        w->fm_path[sizeof(w->fm_path) - 1] = '\0';
        cos_strlcpy(g_efm_last_path[fm_slot(w)], st->current_path, sizeof(g_efm_last_path[fm_slot(w)]));
        g_efm_last_path[fm_slot(w)][EFM_MAX_PATH - 1] = '\0';
    }
}

void fm_bridge_release_window(int idx) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    int sl = fm_slot(w);
    if (g_efm_inited[sl]) efm_cleanup(&g_efm_states[sl]);
    g_efm_inited[sl] = false;
    g_efm_uid[sl] = 0;
}

/* 右クリックメニューで選ばれたアクションを実際のファイル操作にディスパッチする。
 * ここでハンドルしていないアクション (Open With.../Compress/Move To... 等) は
 * このデモではまだ実装されていない機能なので、黙って無視するのではなく
 * ステータスバーに "未実装" と表示してユーザーに分かるようにする。 */
static void fm_bridge_dispatch_ecm_action(efm_state_t* st) {
    int action = ecm_get_last_action();
    if (!st || action == 0) return;

    const char* target_path = ecm_get_target_path();
    int target_idx = efm_find_entry_by_path(st, target_path);

    switch (action) {
        case ECM_ACT_OPEN:
            if (target_idx >= 0) {
                efm_entry_t* e = &st->entries[target_idx];
                if (!e->is_dir && fm_bridge_request_lua_script(st, e->full_path)) {
                    break;
                }
                efm_open_entry(st, target_idx);
            }
            break;

        case ECM_ACT_COPY:
            efm_clipboard_copy_selection(st);
            break;
        case ECM_ACT_CUT:
            efm_clipboard_cut_selection(st);
            break;
        case ECM_ACT_PASTE:
            efm_paste(st);
            break;

        case ECM_ACT_RENAME:
            if (target_idx >= 0) {
                st->dialog_type = 0; /* rename */
                st->focused_index = target_idx;
                cos_strlcpy(st->dialog_input, st->entries[target_idx].name, sizeof(st->dialog_input));
                st->dialog_cursor = (int)strlen(st->dialog_input);
                st->dialog_active = true;
            }
            break;

        case ECM_ACT_DELETE:
            if (st->selected_count > 1) {
                efm_delete_selected(st);
            } else if (target_idx >= 0) {
                efm_delete(st, target_idx);
            }
            break;

        case ECM_ACT_NEW_FILE:
            st->dialog_type = 1;
            st->dialog_input[0] = '\0';
            st->dialog_cursor = 0;
            st->dialog_active = true;
            break;

        case ECM_ACT_NEW_FOLDER:
            st->dialog_type = 2;
            st->dialog_input[0] = '\0';
            st->dialog_cursor = 0;
            st->dialog_active = true;
            break;

        case ECM_ACT_SELECT_ALL:
            efm_select_all(st);
            break;

        case ECM_ACT_SORT_NAME:
            efm_sort(st, EFM_SORT_NAME, st->sort_mode == EFM_SORT_NAME && !st->sort_reverse);
            break;
        case ECM_ACT_SORT_SIZE:
            efm_sort(st, EFM_SORT_SIZE, st->sort_mode == EFM_SORT_SIZE && !st->sort_reverse);
            break;
        case ECM_ACT_SORT_DATE:
            efm_sort(st, EFM_SORT_DATE, st->sort_mode == EFM_SORT_DATE && !st->sort_reverse);
            break;
        case ECM_ACT_SORT_TYPE:
            efm_sort(st, EFM_SORT_TYPE, st->sort_mode == EFM_SORT_TYPE && !st->sort_reverse);
            break;

        case ECM_ACT_VIEW_LIST:       st->view_mode = EFM_VIEW_LIST; break;
        case ECM_ACT_VIEW_ICONS:      st->view_mode = EFM_VIEW_ICONS; break;
        case ECM_ACT_VIEW_DETAILS:    st->view_mode = EFM_VIEW_DETAILS; break;
        case ECM_ACT_VIEW_THUMBNAILS:
            st->view_mode = EFM_VIEW_THUMBNAILS;
            efm_load_all_thumbnails(st);
            break;

        case ECM_ACT_SEARCH:
            st->search_active = true;
            break;

        case ECM_ACT_REFRESH:
            efm_refresh(st);
            break;

        case ECM_ACT_COPY_PATH: {
            char msg[256];
            cos_strlcpy(msg, target_path[0] ? target_path : st->current_path, sizeof(msg));
            efm_set_status(st, msg, 3000);
            break;
        }

        case ECM_ACT_ADD_FAVORITE:
            if (target_idx >= 0) {
                efm_bookmark_add(st, st->entries[target_idx].name, st->entries[target_idx].full_path);
                efm_set_status(st, "Added to Favorites", 2000);
            }
            break;

        case ECM_ACT_PROPERTIES: {
            char info[256];
            if (target_idx >= 0) {
                efm_entry_t* e = &st->entries[target_idx];
                char size_buf[32];
                efm_format_size(e->size, size_buf, sizeof(size_buf));
                snprintf(info, sizeof(info), "%s  |  %s  |  %s",
                         e->name,
                         e->is_dir ? "Folder" : efm_get_type_label(e->type),
                         size_buf);
            } else {
                snprintf(info, sizeof(info), "%s  |  %d items", st->current_path, st->entry_count);
            }
            st->dialog_type = 3; /* properties (read-only info) */
            cos_strlcpy(st->dialog_input, info, sizeof(st->dialog_input));
            st->dialog_cursor = 0;
            st->dialog_active = true;
            break;
        }

        /* このデモではまだ実装していない操作: 無反応にせず、状況を伝える */
        case ECM_ACT_OPEN_WITH:
        case ECM_ACT_OPEN_TERMINAL:
        case ECM_ACT_COPY_TO:
        case ECM_ACT_MOVE_TO:
        case ECM_ACT_COMPRESS:
        case ECM_ACT_EXTRACT:
        case ECM_ACT_SEND_TO_DESKTOP:
        case ECM_ACT_CREATE_SHORTCUT:
            efm_set_status(st, "Not implemented in this demo yet", 2500);
            break;

        case ECM_ACT_RUN_LUA: {
            char reason[160];
            if (target_idx >= 0 && !st->entries[target_idx].is_dir) {
                cos_lua_request_run(target_path, reason, sizeof(reason));
                efm_set_status(st, reason, 2500);
            } else {
                efm_set_status(st, "Please select a Lua script", 2500);
            }
            break;
        }

        default:
            break;
    }

    ecm_clear_action();
}

void draw_file_manager(int idx) {
#if USE_ENHANCED_FILE_MANAGER
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (!w || w->kind != WIN_FILE_MGR) return;

    int cx = w->x + 6, cy = w->y + 34, cw = w->w - 12, ch = w->h - 40;
    if (cw < 420 || ch < 300) {
        /* 小さいウィンドウでは簡易表示にフォールバック */
        draw_file_manager_legacy(idx);
        return;
    }

    efm_state_t* st = fm_bridge_get_state(idx, w);
    if (!st) { draw_file_manager_legacy(idx); return; }

    efm_winui_tick(st, w, cx, cy, cw, ch);          /* hover, splitter, drag and drop */
    efm_draw(st, cx, cy, cw, ch);
    fm_bridge_sync_path(idx, w, st);
    if (st->close_requested) {                         /* File > Close */
        st->close_requested = false;
        gui_close_window(idx);
        return;
    }

    /* 右クリックメニュー (このファイルマネージャーウィンドウが開いたものだけ描画する。
     * ecmはグローバルな単一インスタンスなので、他のウィンドウが誤って描画しないよう
     * target_windowで紐付けを確認する) */
    if (ecm_is_visible() && g_ecm.target_window == idx) {
        ecm_handle_mouse_move((int)mouse.x, (int)mouse.y);
        ecm_draw();
    }
#else
    draw_file_manager_legacy(idx);
#endif
}

void handle_file_manager_click(int idx, int mx, int my) {
#if USE_ENHANCED_FILE_MANAGER
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (!w || w->kind != WIN_FILE_MGR) return;

    int cx = w->x + 6, cy = w->y + 34, cw = w->w - 12, ch = w->h - 40;
    if (cw < 420 || ch < 300) {
        handle_file_manager_click_legacy(idx, mx, my);
        return;
    }

    efm_state_t* st = fm_bridge_get_state(idx, w);
    if (!st) { handle_file_manager_click_legacy(idx, mx, my); return; }

    /* 右クリックメニューがこのウィンドウで開いている間は、クリックはすべて
     * メニュー側で処理し、下のファイルリストへは渡さない。 */
    if (ecm_is_visible() && g_ecm.target_window == idx) {
        bool consumed = ecm_handle_click(mx, my);
        if (consumed) {
            fm_bridge_dispatch_ecm_action(st);
            fm_bridge_sync_path(idx, w, st);
        }
        return;
    }

    /* gui_input.c may detect the press through its button-edge fallback
     * before the driver's one-shot flag is mirrored here.  Treat the live
     * physical right-button state as part of the same gesture so the EFM
     * context menu cannot miss a valid right-click. */
    bool right_click = mouse.right_click || mouse.right;
    bool left_click = mouse.left_click || mouse.left;

    /* メニューを表示する前に、ファイルマネージャー自身のクリップボード状態を
     * ecm側へ反映しておく (「貼り付け」項目の有効/無効判定に使われる) */
    g_ecm.clipboard_has_data = (st->clipboard_count > 0);

    /* ダブルクリック検出: efm_handle_click はリスト領域内のクリックなら
     * (単発クリックでも) 常に true を返してしまうため、"handled == false"
     * を待つ efm_handle_double_click は事実上呼ばれなかった。
     * ここでは行番号を自前で算出し、直近クリックと比較して
     * 500ms 以内の同一行クリックをダブルクリックとして扱う。 */
    if (!st->dialog_active && !right_click) {
        efm_layout_t L;
        efm_compute_layout(st, cx, cy, cw, ch, &L);
        if (mx >= L.list_x && mx < L.list_x + L.list_w && my >= L.body_y && my < L.body_y + L.body_h) {
            {
                int row = efm_list_hit_row(st, &L, mx, my);
            if (row >= 0 && row < st->entry_count) {
                    extern uint64_t get_timer_ticks(void);
                    uint64_t current_time = get_timer_ticks();
                    if (g_efm_last_click_row[fm_slot(w)] == row &&
                        (current_time - g_efm_last_click_time[fm_slot(w)]) < 500) {
                        /* ダブルクリック確定: Lua script は Lua へ直接回し、それ以外は通常のファイルオープン */
                        if (row >= 0 && row < st->entry_count &&
                            !st->entries[row].is_dir && fm_bridge_request_lua_script(st, st->entries[row].full_path)) {
                            g_efm_last_click_row[fm_slot(w)] = -1;
                            g_efm_last_click_time[fm_slot(w)] = 0;
                            fm_bridge_sync_path(idx, w, st);
                            return;
                        }
                        efm_open_entry(st, row);
                        g_efm_last_click_row[fm_slot(w)] = -1;
                        g_efm_last_click_time[fm_slot(w)] = 0;
                        fm_bridge_sync_path(idx, w, st);
                        return;
                    }
                    g_efm_last_click_row[fm_slot(w)] = row;
                    g_efm_last_click_time[fm_slot(w)] = current_time;
                } else {
                    g_efm_last_click_row[fm_slot(w)] = -1;
                }
            }
        } else {
            g_efm_last_click_row[fm_slot(w)] = -1;
        }
    }

    bool was_visible = ecm_is_visible();
    
    /* Right-click context menu for files */
    if (right_click && !st->dialog_active && !st->context_menu_visible) {
        (void)efm_handle_right_click(st, mx, my, cx, cy, cw, ch);
    }
    
    /* Keep a file context menu alive after the opening right button is
     * released.  The prior code treated the release-latched right_click as a
     * second dismissal request, so a normal click opened and immediately
     * closed the menu before the user could choose an action.  A left click
     * still selects a menu item or dismisses it when outside. */
    if (st->context_menu_visible) {
        if (left_click) {
            efm_handle_file_context_menu_click(st, mx, my);
        } else if (mx < cx || mx >= cx + cw || my < cy || my >= cy + ch) {
            st->context_menu_visible = false;
        }
    }
    
    efm_handle_click(st, mx, my, cx, cy, cw, ch, right_click && !st->context_menu_visible);
    if (!was_visible && ecm_is_visible()) {
        /* このウィンドウでメニューを開いたことを記録しておく。
         * (efm_handle_click内部で ecm_show() が呼ばれた場合)
         * これにより右クリックメニューはファイルマネージャーUI内でのみ
         * 描画・操作可能になり、他のウィンドウ/デスクトップには影響しない。 */
        g_ecm.target_window = idx;
    }
    fm_bridge_sync_path(idx, w, st);
#else
    handle_file_manager_click_legacy(idx, mx, my);
#endif
}

void handle_file_manager_key(int idx, int key, char ascii, bool ctrl) {
#if USE_ENHANCED_FILE_MANAGER
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    if (!w || w->kind != WIN_FILE_MGR) return;

    efm_state_t* st = fm_bridge_get_state(idx, w);
    if (!st) { handle_file_manager_key_legacy(idx, key, ascii, ctrl); return; }

    if (ecm_is_visible() && g_ecm.target_window == idx) {
        ecm_handle_key(key);
        return;
    }

    efm_handle_key(st, key, ascii, ctrl, false);
    fm_bridge_sync_path(idx, w, st);
#else
    handle_file_manager_key_legacy(idx, key, ascii, ctrl);
#endif
}

/* After files were dropped somewhere: every open file manager re-reads its folder. */
extern void efm_tree_rebuild(efm_state_t* state);
void fm_bridge_refresh_all(void) {
    for (int s = 0; s < MAX_WINDOWS; ++s)
        if (g_efm_uid[s] && g_efm_inited[s] && gui_window_index_by_uid(g_efm_uid[s]) >= 0) {
            efm_refresh(&g_efm_states[s]);
            efm_tree_rebuild(&g_efm_states[s]);
        }
}
