/**
 * efm_fileops.c - Enhanced File Manager (ファイル作成/削除/リネーム/コピー/移動、選択、クリップボード)
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "../fs/fs.h"
#include <string.h>

static void efm_reveal_by_name(efm_state_t* state, const char* name) {
    if (!state || !name) return;
    for (int i = 0; i < state->entry_count; i++) {
        if (strcmp(state->entries[i].name, name) == 0) {
            efm_deselect_all(state);
            state->entries[i].selected = true;
            state->selected_count = 1;
            state->focused_index = i;
            /* efm_draw_file_list がフレーム毎に妥当な範囲へ丸めてくれるので、
             * ここでは対象行が見える位置を指定するだけでよい */
            state->scroll_offset = i;
            efm_load_preview(state, i);
            break;
        }
    }
}

int efm_create_file(efm_state_t* state, const char* name) {
    if (!state || !name) return -1;
    char path[EFM_MAX_PATH];
    strncpy(path, state->current_path, EFM_MAX_PATH-1);
    size_t plen = strlen(path);
    if (plen > 0 && path[plen-1] != '/') strncat(path, "/", EFM_MAX_PATH-plen-1);
    strncat(path, name, EFM_MAX_PATH-strlen(path)-1);
    
    extern bool fs_create_file(const char* path);
    if (!fs_create_file(path)) return -1;
    efm_set_status(state, "File created", 2000);
    int result = efm_refresh(state);
    efm_reveal_by_name(state, name);
    return result;
}

int efm_create_folder(efm_state_t* state, const char* name) {
    if (!state || !name) return -1;
    char path[EFM_MAX_PATH];
    strncpy(path, state->current_path, EFM_MAX_PATH-1);
    size_t plen = strlen(path);
    if (plen > 0 && path[plen-1] != '/') strncat(path, "/", EFM_MAX_PATH-plen-1);
    strncat(path, name, EFM_MAX_PATH-strlen(path)-1);
    
    extern bool fs_create_dir_at(const char* parent, const char* name);
    if (!fs_create_dir_at(state->current_path, name)) return -1;
    efm_set_status(state, "Folder created", 2000);
    int result = efm_refresh(state);
    efm_reveal_by_name(state, name);
    return result;
}

int efm_rename(efm_state_t* state, int idx, const char* new_name) {
    if (!state || idx < 0 || idx >= state->entry_count || !new_name) return -1;
    efm_entry_t* e = &state->entries[idx];
    
    char new_path[EFM_MAX_PATH];
    strncpy(new_path, state->current_path, EFM_MAX_PATH-1);
    size_t plen = strlen(new_path);
    if (plen > 0 && new_path[plen-1] != '/') strncat(new_path, "/", EFM_MAX_PATH-plen-1);
    strncat(new_path, new_name, EFM_MAX_PATH-strlen(new_path)-1);
    
    if (fs_rename(e->full_path, new_path) != 0) return -1;
    efm_set_status(state, "Renamed", 2000);
    return efm_refresh(state);
}

int efm_delete(efm_state_t* state, int idx) {
    if (!state || idx < 0 || idx >= state->entry_count) return -1;
    if (efm_get_confirm_delete()) {
        state->pending_delete_idx = idx;
        state->dialog_type = 4;
        state->dialog_active = true;
        return 0;
    }
    efm_entry_t* e = &state->entries[idx];
    extern bool fs_delete(const char* path);
    if (!fs_delete(e->full_path)) return -1;
    efm_set_status(state, "Deleted", 2000);
    return efm_refresh(state);
}

int efm_delete_selected(efm_state_t* state) {
    if (!state) return -1;
    if (efm_get_confirm_delete()) {
        state->pending_delete_idx = -1;
        state->dialog_type = 4;
        state->dialog_active = true;
        return 0;
    }
    int deleted = 0;
    for (int i = state->entry_count - 1; i >= 0; i--) {
        if (state->entries[i].selected) {
            extern bool fs_delete(const char* path);
            if (fs_delete(state->entries[i].full_path)) deleted++;
        }
    }
    char msg[64];
    strncpy(msg, "Deleted ", 63);
    char cnt[16];
    int n = deleted, ii = 0;
    if (n == 0) { cnt[ii++] = '0'; }
    else { while (n > 0) { cnt[ii++] = '0' + (n % 10); n /= 10; } }
    for (int j = 0; j < ii/2; j++) { char t = cnt[j]; cnt[j] = cnt[ii-1-j]; cnt[ii-1-j] = t; }
    cnt[ii] = '\0';
    strncat(msg, cnt, 63-strlen(msg));
    strncat(msg, " items", 63-strlen(msg));
    efm_set_status(state, msg, 2000);
    return efm_refresh(state);
}

/* 削除確認ダイアログでOK(削除)が押されたときに呼ばれる実処理。
 * pending_delete_idx == -1 なら選択中の全項目、それ以外なら単一項目を削除する。 */
void efm_delete_confirmed(efm_state_t* state) {
    if (!state) return;
    extern bool fs_delete(const char* path);

    if (state->pending_delete_idx < 0) {
        int deleted = 0;
        for (int i = state->entry_count - 1; i >= 0; i--) {
            if (state->entries[i].selected) {
                if (fs_delete(state->entries[i].full_path)) deleted++;
            }
        }
        char msg[64];
        strncpy(msg, "Deleted ", 63);
        char cnt[16];
        int n = deleted, ii = 0;
        if (n == 0) { cnt[ii++] = '0'; }
        else { while (n > 0) { cnt[ii++] = '0' + (n % 10); n /= 10; } }
        for (int j = 0; j < ii/2; j++) { char t = cnt[j]; cnt[j] = cnt[ii-1-j]; cnt[ii-1-j] = t; }
        cnt[ii] = '\0';
        strncat(msg, cnt, 63-strlen(msg));
        strncat(msg, " items", 63-strlen(msg));
        efm_set_status(state, msg, 2000);
        efm_refresh(state);
    } else if (state->pending_delete_idx < state->entry_count) {
        efm_entry_t* e = &state->entries[state->pending_delete_idx];
        if (fs_delete(e->full_path)) {
            efm_set_status(state, "Deleted", 2000);
        }
        efm_refresh(state);
    }
    state->pending_delete_idx = -1;
}

int efm_paste(efm_state_t* state) {
    if (!state || state->clipboard_count == 0) return -1;
    int pasted = 0;
    for (int i = 0; i < state->clipboard_count; i++) {
        const char* src = state->clipboard_paths[i];
        /* ファイル名を取得 */
        const char* fname = src + strlen(src) - 1;
        while (fname > src && *fname != '/') fname--;
        if (*fname == '/') fname++;
        
        char dest[EFM_MAX_PATH];
        strncpy(dest, state->current_path, EFM_MAX_PATH-1);
        size_t plen = strlen(dest);
        if (plen > 0 && dest[plen-1] != '/') strncat(dest, "/", EFM_MAX_PATH-plen-1);
        strncat(dest, fname, EFM_MAX_PATH-strlen(dest)-1);
        
        extern bool fs_copy_file(const char* src, const char* dst);
            
        if (state->clipboard_is_cut) {
            if (fs_rename(src, dest)) pasted++;
        } else {
            if (fs_copy_file(src, dest)) pasted++;
        }
    }
    if (state->clipboard_is_cut) {
        state->clipboard_count = 0;
        state->clipboard_is_cut = false;
    }
    efm_set_status(state, "Pasted", 2000);
    return efm_refresh(state);
}

int efm_copy(efm_state_t* state, int idx, const char* dest_path) {
    if (!state || idx < 0 || idx >= state->entry_count || !dest_path) return -1;
    extern bool fs_copy_path(const char* src, const char* dst);
    efm_entry_t* e = &state->entries[idx];
    if (!fs_copy_path(e->full_path, dest_path)) return -1;
    return efm_refresh(state);
}

int efm_move(efm_state_t* state, int idx, const char* dest_path) {
    if (!state || idx < 0 || idx >= state->entry_count || !dest_path) return -1;
    extern bool fs_move_path(const char* src, const char* dst);
    efm_entry_t* e = &state->entries[idx];
    if (!fs_move_path(e->full_path, dest_path)) return -1;
    return efm_refresh(state);
}

/* ============================================================
 * 選択
 * ============================================================ */

void efm_select(efm_state_t* state, int idx, bool multi) {
    if (!state || idx < 0 || idx >= state->entry_count) return;
    if (!multi) efm_deselect_all(state);
    state->entries[idx].selected = !state->entries[idx].selected;
    state->focused_index = idx;
    
    /* 選択数更新 */
    state->selected_count = 0;
    for (int i = 0; i < state->entry_count; i++) {
        if (state->entries[i].selected) state->selected_count++;
    }
}

void efm_select_all(efm_state_t* state) {
    if (!state) return;
    for (int i = 0; i < state->entry_count; i++) state->entries[i].selected = true;
    state->selected_count = state->entry_count;
}

void efm_deselect_all(efm_state_t* state) {
    if (!state) return;
    for (int i = 0; i < state->entry_count; i++) state->entries[i].selected = false;
    state->selected_count = 0;
}

int efm_get_selected_count(const efm_state_t* state) {
    return state ? state->selected_count : 0;
}

/* 選択中の項目 (なければフォーカス中の項目) をクリップボードへコピー/切り取り。
 * Ctrl+C / Ctrl+X ショートカットと右クリックメニューの Copy/Cut から共通で使う。 */
static void efm_clipboard_stash_selection(efm_state_t* state, bool is_cut) {
    if (!state) return;
    state->clipboard_count = 0;
    for (int i = 0; i < state->entry_count && state->clipboard_count < EFM_MAX_SELECTED; i++) {
        if (state->entries[i].selected) {
            strncpy(state->clipboard_paths[state->clipboard_count], state->entries[i].full_path, EFM_MAX_PATH-1);
            state->clipboard_paths[state->clipboard_count][EFM_MAX_PATH-1] = '\0';
            state->clipboard_count++;
        }
    }
    if (state->clipboard_count == 0 && state->focused_index >= 0 && state->focused_index < state->entry_count) {
        strncpy(state->clipboard_paths[0], state->entries[state->focused_index].full_path, EFM_MAX_PATH-1);
        state->clipboard_paths[0][EFM_MAX_PATH-1] = '\0';
        state->clipboard_count = 1;
    }
    state->clipboard_is_cut = is_cut;
    if (state->clipboard_count > 0) {
        efm_set_status(state, is_cut ? "Cut" : "Copied", 2000);
    }
}

void efm_clipboard_copy_selection(efm_state_t* state) {
    efm_clipboard_stash_selection(state, false);
}

void efm_clipboard_cut_selection(efm_state_t* state) {
    efm_clipboard_stash_selection(state, true);
}

int efm_find_entry_by_path(const efm_state_t* state, const char* path) {
    if (!state || !path || !path[0]) return -1;
    for (int i = 0; i < state->entry_count; i++) {
        if (strcmp(state->entries[i].full_path, path) == 0) return i;
    }
    return -1;
}
