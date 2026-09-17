/**
 * efm_search_bookmark.c - Enhanced File Manager (検索・ソートAPI・ブックマーク)
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "../fs/fs.h"
#include <string.h>


/* ============================================================
 * 検索
 * ============================================================ */

int efm_search(efm_state_t* state, const char* query) {
    if (!state || !query) return -1;
    strncpy(state->search_text, query, EFM_MAX_SEARCH_LEN-1);
    state->search_text[EFM_MAX_SEARCH_LEN-1] = '\0';
    state->search_active = (query[0] != '\0');
    state->search_result_count = 0;
    
    if (!state->search_active) return 0;
    
    for (int i = 0; i < state->entry_count; i++) {
        /* 大文字小文字無視の部分一致 */
        const char* name = state->entries[i].name;
        const char* q = query;
        bool found = false;
        
        /* 簡易検索 */
        size_t qlen = strlen(q);
        size_t nlen = strlen(name);
        for (size_t j = 0; j + qlen <= nlen; j++) {
            bool match = true;
            for (size_t k = 0; k < qlen; k++) {
                char a = name[j+k], b = q[k];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match) { found = true; break; }
        }
        
        if (found && state->search_result_count < EFM_MAX_ENTRIES) {
            state->search_results[state->search_result_count++] = i;
        }
    }
    
    return state->search_result_count;
}

void efm_search_clear(efm_state_t* state) {
    if (!state) return;
    state->search_text[0] = '\0';
    state->search_active = false;
    state->search_result_count = 0;
}

/* ============================================================
 * ソート
 * ============================================================ */

void efm_sort(efm_state_t* state, efm_sort_mode_t mode, bool reverse) {
    if (!state) return;
    state->sort_mode = mode;
    state->sort_reverse = reverse;
    efm_sort_entries(state);
}

/* ============================================================
 * ブックマーク
 * ============================================================ */

int efm_bookmark_add(efm_state_t* state, const char* name, const char* path) {
    if (!state || !name || !path || state->bookmark_count >= EFM_MAX_BOOKMARKS) return -1;
    efm_bookmark_t* bm = &state->bookmarks[state->bookmark_count++];
    strncpy(bm->name, name, 63);
    bm->name[63] = '\0';
    strncpy(bm->path, path, EFM_MAX_PATH-1);
    bm->path[EFM_MAX_PATH-1] = '\0';
    bm->type = EFM_TYPE_FOLDER;
    return state->bookmark_count - 1;
}

int efm_bookmark_remove(efm_state_t* state, int idx) {
    if (!state || idx < 0 || idx >= state->bookmark_count) return -1;
    memmove(&state->bookmarks[idx], &state->bookmarks[idx+1],
            (state->bookmark_count - idx - 1) * sizeof(efm_bookmark_t));
    state->bookmark_count--;
    return 0;
}

void efm_bookmark_navigate(efm_state_t* state, int idx) {
    if (!state || idx < 0 || idx >= state->bookmark_count) return;
    efm_navigate(state, state->bookmarks[idx].path);
}

/* ============================================================
 * サムネイル
 * ============================================================ */

