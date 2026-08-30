/**
 * efm_core.c - Enhanced File Manager (初期化・既定値・ナビゲーション履歴・ソート・ディレクトリ再読込)
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "../fs/fs.h"
#include <string.h>

static bool g_efm_default_show_hidden = false;
static int  g_efm_default_view_mode   = EFM_VIEW_DETAILS;
static bool g_efm_confirm_delete      = true;

bool efm_get_default_show_hidden(void) { return g_efm_default_show_hidden; }
void efm_set_default_show_hidden(bool value) { g_efm_default_show_hidden = value; }
int  efm_get_default_view_mode(void) { return g_efm_default_view_mode; }
void efm_set_default_view_mode(int mode) { g_efm_default_view_mode = mode; }
bool efm_get_confirm_delete(void) { return g_efm_confirm_delete; }
void efm_set_confirm_delete(bool value) { g_efm_confirm_delete = value; }

void efm_init(efm_state_t* state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    strncpy(state->current_path, "/", EFM_MAX_PATH-1);
    state->view_mode = (efm_view_mode_t)g_efm_default_view_mode;
    state->sort_mode = EFM_SORT_NAME;
    state->sort_reverse = false;
    state->show_hidden = g_efm_default_show_hidden;
    state->show_preview = true;
    state->focused_index = -1;
    state->scroll_offset = 0;
    state->pending_delete_idx = -1;
    state->context_menu_visible = false;
    state->context_menu_x = 0;
    state->context_menu_y = 0;
    state->context_menu_idx = -1;
    state->initialized = true;
    
    /* デフォルトブックマーク */
    efm_bookmark_add(state, "Root", "/");
    efm_bookmark_add(state, "Desktop", "/desktop");
    efm_bookmark_add(state, "Documents", "/documents");
    efm_bookmark_add(state, "Music", "/music");
    efm_bookmark_add(state, "Pictures", "/pictures");
    efm_bookmark_add(state, "Scripts", "/scripts");
    
    serial_puts("[EFM] Enhanced File Manager initialized\n");
}

void efm_cleanup(efm_state_t* state) {
    if (!state) return;
    /* サムネイルメモリ解放 */
    for (int i = 0; i < state->entry_count; i++) {
        if (state->entries[i].thumbnail_data) {
            kfree(state->entries[i].thumbnail_data);
            state->entries[i].thumbnail_data = NULL;
        }
    }
    if (state->preview_image) {
        kfree(state->preview_image);
        state->preview_image = NULL;
    }
    state->initialized = false;
}

/* ============================================================
 * ナビゲーション
 * ============================================================ */

static void efm_push_history(efm_state_t* state, const char* path) {
    /* 現在位置以降の履歴を削除 */
    state->history_count = state->history_pos + 1;
    if (state->history_count >= EFM_MAX_HISTORY) {
        /* 古い履歴をシフト */
        memmove(state->history, state->history + 1, (EFM_MAX_HISTORY - 1) * EFM_MAX_PATH);
        state->history_count = EFM_MAX_HISTORY - 1;
    }
    strncpy(state->history[state->history_count], path, EFM_MAX_PATH-1);
    state->history[state->history_count][EFM_MAX_PATH-1] = '\0';
    state->history_count++;
    state->history_pos = state->history_count - 1;
}

int efm_navigate(efm_state_t* state, const char* path) {
    if (!state || !path) return -1;
    efm_push_history(state, path);
    strncpy(state->current_path, path, EFM_MAX_PATH-1);
    state->current_path[EFM_MAX_PATH-1] = '\0';
    state->scroll_offset = 0;
    state->focused_index = -1;
    efm_deselect_all(state);
    return efm_refresh(state);
}

int efm_navigate_up(efm_state_t* state) {
    if (!state) return -1;
    char parent[EFM_MAX_PATH];
    strncpy(parent, state->current_path, EFM_MAX_PATH-1);
    parent[EFM_MAX_PATH-1] = '\0';
    
    /* 末尾の / を除去 */
    size_t len = strlen(parent);
    if (len > 1 && parent[len-1] == '/') { parent[len-1] = '\0'; len--; }
    
    /* 最後の / を見つける */
    int last_slash = -1;
    for (int i = (int)len - 1; i >= 0; i--) {
        if (parent[i] == '/') { last_slash = i; break; }
    }
    
    if (last_slash <= 0) {
        strncpy(parent, "/", EFM_MAX_PATH-1);
    } else {
        parent[last_slash] = '\0';
    }
    
    return efm_navigate(state, parent);
}

int efm_navigate_back(efm_state_t* state) {
    if (!state || !efm_can_go_back(state)) return -1;
    state->history_pos--;
    strncpy(state->current_path, state->history[state->history_pos], EFM_MAX_PATH-1);
    state->scroll_offset = 0;
    return efm_refresh(state);
}

int efm_navigate_forward(efm_state_t* state) {
    if (!state || !efm_can_go_forward(state)) return -1;
    state->history_pos++;
    strncpy(state->current_path, state->history[state->history_pos], EFM_MAX_PATH-1);
    state->scroll_offset = 0;
    return efm_refresh(state);
}

bool efm_can_go_back(const efm_state_t* state) {
    return state && state->history_pos > 0;
}

bool efm_can_go_forward(const efm_state_t* state) {
    return state && state->history_pos < state->history_count - 1;
}

/* ============================================================
 * ファイルリスト更新
 * ============================================================ */

static int efm_compare_entries(const efm_entry_t* a, const efm_entry_t* b, efm_sort_mode_t mode, bool reverse) {
    /* フォルダを先頭に */
    if (a->is_dir && !b->is_dir) return -1;
    if (!a->is_dir && b->is_dir) return 1;
    
    int cmp = 0;
    switch (mode) {
        case EFM_SORT_NAME:
            cmp = strcmp(a->name, b->name);
            break;
        case EFM_SORT_SIZE:
            cmp = (a->size < b->size) ? -1 : (a->size > b->size) ? 1 : 0;
            break;
        case EFM_SORT_DATE:
            cmp = (a->modified_time < b->modified_time) ? -1 : (a->modified_time > b->modified_time) ? 1 : 0;
            break;
        case EFM_SORT_TYPE:
            cmp = (int)a->type - (int)b->type;
            if (cmp == 0) cmp = strcmp(a->name, b->name);
            break;
    }
    return reverse ? -cmp : cmp;
}

void efm_sort_entries(efm_state_t* state) {
    /* バブルソート (エントリ数が少ないため) */
    for (int i = 0; i < state->entry_count - 1; i++) {
        for (int j = 0; j < state->entry_count - i - 1; j++) {
            if (efm_compare_entries(&state->entries[j], &state->entries[j+1],
                                    state->sort_mode, state->sort_reverse) > 0) {
                efm_entry_t tmp = state->entries[j];
                state->entries[j] = state->entries[j+1];
                state->entries[j+1] = tmp;
            }
        }
    }
}

int efm_refresh(efm_state_t* state) {
    if (!state) return -1;
    
    /* 古いサムネイルを解放 */
    for (int i = 0; i < state->entry_count; i++) {
        if (state->entries[i].thumbnail_data) {
            kfree(state->entries[i].thumbnail_data);
            state->entries[i].thumbnail_data = NULL;
        }
    }
    
    state->entry_count = 0;
    state->loading = true;
    
    /* ファイルシステムからエントリを取得 */
    fs_entry_t* raw_entries = fs_list_dir(state->current_path);
    int count = fs_entry_count_for_path(state->current_path);
    
    if (!raw_entries || count <= 0) {
        /* フォールバック: ルートディレクトリ */
        strncpy(state->current_path, "/", EFM_MAX_PATH-1);
        raw_entries = fs_list_dir("/");
        count = fs_entry_count_for_path("/");
        if (!raw_entries || count < 0) count = 0;
    }
    
    /* エントリを変換 */
    for (int i = 0; i < count && state->entry_count < EFM_MAX_ENTRIES; i++) {
        efm_entry_t* e = &state->entries[state->entry_count];
        memset(e, 0, sizeof(*e));
        
        strncpy(e->name, raw_entries[i].name, 255);
        e->name[255] = '\0';
        
        /* フルパス構築 */
        strncpy(e->full_path, state->current_path, EFM_MAX_PATH-1);
        size_t plen = strlen(e->full_path);
        if (plen > 0 && e->full_path[plen-1] != '/') {
            strncat(e->full_path, "/", EFM_MAX_PATH-plen-1);
        }
        strncat(e->full_path, e->name, EFM_MAX_PATH-strlen(e->full_path)-1);
        
        e->size = raw_entries[i].size;
        e->modified_time = raw_entries[i].modified_time;
        e->is_dir = raw_entries[i].is_dir;
        e->is_hidden = (e->name[0] == '.');
        e->type = e->is_dir ? EFM_TYPE_FOLDER : efm_get_file_type(e->name);
        e->selected = false;
        e->has_thumbnail = false;
        e->thumbnail_data = NULL;
        
        /* 隠しファイルをスキップ (設定による) */
        if (e->is_hidden && !state->show_hidden) continue;
        
        state->entry_count++;
    }
    
    /* ソート */
    efm_sort_entries(state);
    
    state->loading = false;
    
    char msg[128];
    strncpy(msg, "Loaded: ", 127);
    char cnt[16];
    int n = state->entry_count;
    int i = 0;
    if (n == 0) { cnt[i++] = '0'; }
    else { while (n > 0) { cnt[i++] = '0' + (n % 10); n /= 10; } }
    for (int j = 0; j < i/2; j++) { char t = cnt[j]; cnt[j] = cnt[i-1-j]; cnt[i-1-j] = t; }
    cnt[i] = '\0';
    strncat(msg, cnt, 127-strlen(msg));
    strncat(msg, " items", 127-strlen(msg));
    efm_set_status(state, msg, 2000);
    
    serial_puts("[EFM] Refreshed: ");
    serial_puts(state->current_path);
    serial_puts(", count=");
    serial_putdec(state->entry_count);
    serial_puts("\n");
    
    return state->entry_count;
}

/* ============================================================
 * ファイル操作
 * ============================================================ */

/* 指定した名前のエントリを一覧内で探し、フォーカス・選択し、
 * リストが収まりきらない場合でもすぐ見えるようスクロール位置を合わせる */
