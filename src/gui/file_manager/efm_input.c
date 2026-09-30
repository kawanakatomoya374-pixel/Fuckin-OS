/**
 * efm_input.c - Enhanced File Manager (マウス/キーボード入力処理・ファイルを開く)
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "../fs/fs.h"
#include <string.h>


bool efm_handle_double_click(efm_state_t* state, int mx, int my, int win_x, int win_y, int win_w, int win_h) {
    if (!state) return false;
    
    int toolbar_h = 40;
    int breadcrumb_h = 32;
    int statusbar_h = 24;
    int sidebar_w = 160;
    int preview_w = state->show_preview ? EFM_PREVIEW_W : 0;
    
    int list_x = win_x + sidebar_w;
    int list_y = win_y + toolbar_h + breadcrumb_h;
    int list_w = win_w - sidebar_w - preview_w;
    int list_h = win_h - toolbar_h - breadcrumb_h - statusbar_h;
    
    if (mx < list_x || mx >= list_x + list_w || my < list_y || my >= list_y + list_h) return false;
    
    int row_h = (state->view_mode == EFM_VIEW_LIST) ? 26 :
                (state->view_mode == EFM_VIEW_DETAILS) ? 28 :
                (state->view_mode == EFM_VIEW_ICONS) ? 72 : 80;
    
    int header_h = (state->view_mode == EFM_VIEW_DETAILS) ? 24 : 0;
    int rel_y = my - list_y - header_h;
    if (rel_y < 0) return false;
    
    int row = rel_y / row_h + state->scroll_offset;
    if (row < 0 || row >= state->entry_count) return false;
    
    efm_open_entry(state, row);
    return true;
}

void efm_open_entry(efm_state_t* state, int idx) {
    if (!state || idx < 0 || idx >= state->entry_count) return;
    efm_entry_t* e = &state->entries[idx];
    
    if (e->is_dir) {
        efm_navigate(state, e->full_path);
    } else {
        /* ファイルを適切なアプリで開く */
        serial_puts("[EFM] Opening file: ");
        serial_puts(e->full_path);
        serial_puts("\n");
        
        /* ファイルタイプ別に処理 */
        gui_open_file_in_app(e->full_path, (int)e->type);
    }
}

bool efm_handle_click(efm_state_t* state, int mx, int my, int win_x, int win_y, int win_w, int win_h, bool right_click) {
    if (!state) return false;
    
    efm_layout_t L;
    efm_compute_layout(state, win_x, win_y, win_w, win_h, &L);
    
    /* ダイアログ処理 */
    if (state->dialog_active) {
        int dw = 360, dh = 120;
        int dx = win_x + win_w / 2 - dw / 2;
        int dy = win_y + win_h / 2 - dh / 2;
        
        /* OKボタン */
        if (mx >= dx + dw - 160 && mx < dx + dw - 88 && my >= dy + dh - 36 && my < dy + dh - 10) {
            state->dialog_active = false;
            /* 操作実行 */
            if (state->dialog_type == 0 && state->focused_index >= 0) {
                efm_rename(state, state->focused_index, state->dialog_input);
            } else if (state->dialog_type == 1) {
                efm_create_file(state, state->dialog_input);
            } else if (state->dialog_type == 2) {
                efm_create_folder(state, state->dialog_input);
            } else if (state->dialog_type == 4) {
                efm_delete_confirmed(state);
            }
            return true;
        }
        /* キャンセルボタン */
        if (mx >= dx + dw - 80 && mx < dx + dw - 12 && my >= dy + dh - 36 && my < dy + dh - 10) {
            state->dialog_active = false;
            return true;
        }
        return true;
    }
    
    /* menu bar, toolbar, places, splitter, column headers (efm_winui.c) */
    if (efm_winui_click(state, mx, my, win_x, win_y, win_w, win_h, right_click)) return true;

    /* サイドバークリック - hierarchical tree (efm_tree.c).
     *
     * A row has two distinct click targets: the disclosure triangle
     * expands/collapses that folder in place, and the name navigates into
     * it. efm_tree_hit_test() reports which was hit so one row can serve
     * both actions - the standard behaviour of a tree navigation pane,
     * and the reason the hit test returns the arrow flag separately
     * rather than just a row index. */
    if (mx >= L.tree_x && mx < L.tree_x + L.tree_w && my >= L.body_y && my < L.body_y + L.body_h) {
        bool hit_arrow = false;
        int node_idx = efm_tree_hit_test(state, mx, my, L.tree_x, L.body_y, &hit_arrow);
        if (node_idx >= 0 && node_idx < state->tree_count) {
            if (hit_arrow) {
                efm_tree_toggle(state, state->tree[node_idx].path);
            } else {
                efm_navigate(state, state->tree[node_idx].path);
                /* Rebuild so the newly-current directory highlights, and
                 * so anything that changed on disk since the tree was
                 * last built is reflected. */
                efm_tree_rebuild(state);
            }
        }
        return true;
    }
    
    /* ファイルリストクリック */
    if (mx >= L.list_x && mx < L.list_x + L.list_w && my >= L.body_y && my < L.body_y + L.body_h) {
        extern void ecm_show(int ctx_type, int x, int y, int target_window, int target_icon, const char* file_path);
        extern bool keyboard_is_ctrl_pressed(void);
        extern bool keyboard_is_shift_pressed(void);
        int row = efm_list_hit_row(state, &L, mx, my);
        if (row >= 0) {
            /* right-click on an item that is part of a multi-selection keeps it */
            bool keep = right_click && state->entries[row].selected && state->selected_count > 1;
            if (!keep) {
                if (!right_click && keyboard_is_ctrl_pressed()) {
                    efm_select(state, row, true);                         /* toggle */
                } else if (!right_click && keyboard_is_shift_pressed() && state->focused_index >= 0) {
                    int a0 = state->focused_index < row ? state->focused_index : row;
                    int a1 = state->focused_index < row ? row : state->focused_index;
                    efm_deselect_all(state);
                    for (int k = a0; k <= a1; ++k) { state->entries[k].selected = true; }
                    state->selected_count = a1 - a0 + 1;
                } else if (!(state->entries[row].selected && state->selected_count > 1)) {
                    efm_select(state, row, false);
                }
                state->focused_index = row;
                efm_load_preview(state, row);
            }
            if (right_click) {
                efm_entry_t* e = &state->entries[row];
                ecm_show(e->is_dir ? 9 : 8, mx, my, -1, -1, e->full_path);   /* ECM_CTX_FM_FOLDER / _FILE */
            } else {
                efm_winui_arm_drag(state, row, mx, my);                      /* may become a drag */
            }
            return true;
        }
        /* empty area */
        efm_deselect_all(state);
        if (right_click) ecm_show(10, mx, my, -1, -1, state->current_path);  /* ECM_CTX_FM_EMPTY */
        return true;
    }
    
    return false;
}

bool efm_handle_key(efm_state_t* state, int key, char ascii, bool ctrl, bool shift) {
    if (!state) return false;
    
    /* ダイアログ入力 */
    if (state->dialog_active) {
        if (key == 0x01) { /* ESC */
            state->dialog_active = false;
            return true;
        }
        if (key == 0x1C) { /* Enter */
            state->dialog_active = false;
            if (state->dialog_type == 0 && state->focused_index >= 0) {
                efm_rename(state, state->focused_index, state->dialog_input);
            } else if (state->dialog_type == 1) {
                efm_create_file(state, state->dialog_input);
            } else if (state->dialog_type == 2) {
                efm_create_folder(state, state->dialog_input);
            } else if (state->dialog_type == 4) {
                efm_delete_confirmed(state);
            }
            return true;
        }
        if (key == 0x0E) { /* Backspace */
            if (state->dialog_cursor > 0) {
                int len = (int)strlen(state->dialog_input);
                if (state->dialog_cursor <= len) {
                    memmove(state->dialog_input + state->dialog_cursor - 1,
                            state->dialog_input + state->dialog_cursor,
                            len - state->dialog_cursor + 1);
                    state->dialog_cursor--;
                }
            }
            return true;
        }
        if (ascii >= 32 && ascii < 127) {
            int len = (int)strlen(state->dialog_input);
            if (len < 255) {
                memmove(state->dialog_input + state->dialog_cursor + 1,
                        state->dialog_input + state->dialog_cursor,
                        len - state->dialog_cursor + 1);
                state->dialog_input[state->dialog_cursor] = ascii;
                state->dialog_cursor++;
            }
            return true;
        }
        return true;
    }
    
    /* 検索入力 */
    if (state->search_active) {
        if (key == 0x01) { /* ESC */
            efm_search_clear(state);
            return true;
        }
        if (key == 0x0E && strlen(state->search_text) > 0) { /* Backspace */
            state->search_text[strlen(state->search_text) - 1] = '\0';
            efm_search(state, state->search_text);
            return true;
        }
        if (ascii >= 32 && ascii < 127) {
            size_t len = strlen(state->search_text);
            if (len < EFM_MAX_SEARCH_LEN - 1) {
                state->search_text[len] = ascii;
                state->search_text[len + 1] = '\0';
                efm_search(state, state->search_text);
            }
            return true;
        }
    }
    
    /* ファイルリスト操作 */
    if (ctrl) {
        if (ascii == 'a' || ascii == 'A') { efm_select_all(state); return true; }
        if (ascii == 'c' || ascii == 'C') {
            efm_clipboard_copy_selection(state);
            return true;
        }
        if (ascii == 'x' || ascii == 'X') {
            efm_clipboard_cut_selection(state);
            return true;
        }
        if (ascii == 'v' || ascii == 'V') { efm_paste(state); return true; }
        if (ascii == 'f' || ascii == 'F') { state->search_active = true; return true; }
        if (ascii == 'r' || ascii == 'R') { efm_refresh(state); return true; }
        if (ascii == 'n' || ascii == 'N') {
            state->dialog_type = shift ? 2 : 1;
            state->dialog_input[0] = '\0';
            state->dialog_cursor = 0;
            state->dialog_active = true;
            return true;
        }
    }
    
    /* ナビゲーション */
    if (key == 0x48) { /* 上矢印 */
        if (state->focused_index > 0) {
            efm_select(state, state->focused_index - 1, shift);
            if (state->focused_index < state->scroll_offset) state->scroll_offset--;
        }
        return true;
    }
    if (key == 0x50) { /* 下矢印 */
        if (state->focused_index < state->entry_count - 1) {
            efm_select(state, state->focused_index + 1, shift);
        }
        return true;
    }
    if (key == 0x1C) { /* Enter */
        if (state->focused_index >= 0) efm_open_entry(state, state->focused_index);
        return true;
    }
    if (key == 0x3C) { /* F2 - 名前変更 */
        if (state->focused_index >= 0) {
            state->dialog_type = 0;
            strncpy(state->dialog_input, state->entries[state->focused_index].name, 255);
            state->dialog_cursor = (int)strlen(state->dialog_input);
            state->dialog_active = true;
        }
        return true;
    }
    if (key == 0x53) { /* Delete */
        if (state->selected_count > 1) {
            efm_delete_selected(state);
        } else if (state->focused_index >= 0) {
            efm_delete(state, state->focused_index);
        }
        return true;
    }
    if (key == 0x3F) { /* F5 - 更新 */
        efm_refresh(state);
        return true;
    }
    
    return false;
}

/* ============================================================
 * Right-Click Context Menu Handler
 * ============================================================ */
bool efm_handle_right_click(efm_state_t* state, int mx, int my, int win_x, int win_y, int win_w, int win_h) {
    if (!state) return false;
    efm_layout_t L;
    efm_compute_layout(state, win_x, win_y, win_w, win_h, &L);
    int row = efm_list_hit_row(state, &L, mx, my);
    if (row < 0) return false;
    efm_show_file_context_menu(state, mx, my, row);
    return true;
}
