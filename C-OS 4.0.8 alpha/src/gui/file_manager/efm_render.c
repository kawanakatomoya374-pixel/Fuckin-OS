/**
 * efm_render.c - Enhanced File Manager (描画 (ツールバー/一覧/プレビュー/ダイアログ))
 * enhanced_file_manager.c から分割生成。詳細は efm_internal.h を参照。
 */

#include "efm_internal.h"
#include "../include/serial.h"
#include "../include/memory.h"
#include "../include/types.h"
#include "vga.h"
#include "mouse.h"
#include "../fs/fs.h"
#include <string.h>

static void efm_draw_file_icon(int x, int y, int size, efm_file_type_t type, bool is_dir) {
    uint64_t color = efm_type_color(is_dir ? EFM_TYPE_FOLDER : type);
    
    if (is_dir) {
        /* フォルダアイコン */
        vga_fill_rounded_rect(x, y + size/4, size, size*3/4, 2, color);
        vga_fill_rect(x, y + size/4, size/2, size/4, color);
        vga_fill_rounded_rect(x, y + size/4 - 2, size/2, size/4, 2, color);
    } else {
        /* ファイルアイコン */
        int fold = size/4;
        vga_fill_rect(x, y, size - fold, size, color);
        vga_fill_rect(x + size - fold, y + fold, fold, size - fold, color);
        /* 折り目 */
        uint64_t shadow = (color & 0x00FEFEFE) >> 1;
        vga_fill_rect(x + size - fold, y, fold, fold, shadow);
        vga_fill_rect(x + size - fold - 1, y + fold, 1, 1, shadow);
        
        /* タイプ別カラーバー */
        if (size >= 16) {
            vga_fill_rect(x + 2, y + size/2, size - fold - 2, 2, 0x00FFFFFF);
            if (size >= 24) {
                vga_fill_rect(x + 2, y + size/2 + 4, size - fold - 2, 2, 0x00FFFFFF);
            }
        }
    }
}

/* ブレッドクラムナビゲーション描画 */
void efm_draw_breadcrumb(efm_state_t* state, int x, int y, int w) {
    if (!state) return;
    
    vga_fill_rounded_rect(x, y, w, 28, 6, EFM_C_PANEL);
    vga_draw_rounded_rect(x, y, w, 28, 6, EFM_C_BORDER);
    
    /* パスを分割して表示 */
    const char* path = state->current_path;
    int px = x + 8;
    int py = y + 7;
    
    /* ルート */
    vga_draw_string(px, py, "/", EFM_C_ACCENT, 0xFFFFFFFF);
    px += FONT_W + 4;
    
    /* パスコンポーネント */
    char component[64];
    const char* p = path;
    if (*p == '/') p++;
    
    while (*p && px < x + w - 20) {
        int ci = 0;
        while (*p && *p != '/' && ci < 63) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';
        if (*p == '/') p++;
        
        if (ci > 0) {
            /* セパレーター */
            vga_draw_string(px, py, ">", EFM_C_MUTED, 0xFFFFFFFF);
            px += FONT_W + 4;
            
            /* コンポーネント */
            vga_draw_string(px, py, component, EFM_C_TEXT, 0xFFFFFFFF);
            px += (int)strlen(component) * FONT_W + 4;
        }
    }
}

/* ツールバー描画 */
void efm_draw_toolbar(efm_state_t* state, int x, int y, int w) {
    if (!state) return;
    
    vga_fill_rect(x, y, w, 36, EFM_C_TOOLBAR);
    vga_fill_rect(x, y + 35, w, 1, EFM_C_BORDER);
    
    int bx = x + 8;
    int by = y + 4;
    int bh = 28;
    
    /* 戻る/進む ボタン */
    bool can_back = efm_can_go_back(state);
    bool can_fwd  = efm_can_go_forward(state);
    
    /* 戻るボタン */
    vga_fill_rounded_rect(bx, by, 32, bh, 4, can_back ? EFM_C_ACCENT : EFM_C_BORDER);
    vga_draw_string(bx + 10, by + 7, "<", can_back ? 0x00FFFFFF : EFM_C_MUTED, 0xFFFFFFFF);
    bx += 36;
    
    /* 進むボタン */
    vga_fill_rounded_rect(bx, by, 32, bh, 4, can_fwd ? EFM_C_ACCENT : EFM_C_BORDER);
    vga_draw_string(bx + 10, by + 7, ">", can_fwd ? 0x00FFFFFF : EFM_C_MUTED, 0xFFFFFFFF);
    bx += 36;
    
    /* 上へボタン */
    vga_fill_rounded_rect(bx, by, 32, bh, 4, EFM_C_PANEL);
    vga_draw_string(bx + 8, by + 7, "Up", EFM_C_TEXT, 0xFFFFFFFF);
    bx += 36;
    
    /* 更新ボタン */
    vga_fill_rounded_rect(bx, by, 40, bh, 4, EFM_C_PANEL);
    vga_draw_string(bx + 6, by + 7, gui_is_japanese() ? "更新" : "Refresh", EFM_C_TEXT, 0xFFFFFFFF);
    bx += 44;
    
    /* 区切り */
    vga_fill_rect(bx, by + 4, 1, bh - 8, EFM_C_BORDER);
    bx += 8;
    
    /* 新規ファイル */
    vga_fill_rounded_rect(bx, by, 56, bh, 4, EFM_C_PANEL);
    vga_draw_string(bx + 4, by + 7, gui_is_japanese() ? "新規" : "New File", EFM_C_TEXT, 0xFFFFFFFF);
    bx += 60;
    
    /* 新規フォルダ */
    vga_fill_rounded_rect(bx, by, 72, bh, 4, EFM_C_PANEL);
    vga_draw_string(bx + 4, by + 7, gui_is_japanese() ? "フォルダ" : "New Folder", EFM_C_TEXT, 0xFFFFFFFF);
    bx += 76;
    
    /* 削除ボタン (選択項目がある場合のみ有効表示) */
    bool has_sel = state->selected_count > 0 || state->focused_index >= 0;
    const char* del_lbl = gui_is_japanese() ? "削除" : "Delete";
    int del_w = (int)strlen(del_lbl) * FONT_W + 16;
    vga_fill_rounded_rect(bx, by, del_w, bh, 4, has_sel ? EFM_C_DANGER : EFM_C_PANEL);
    vga_draw_string(bx + 8, by + 7, del_lbl, has_sel ? 0x00FFFFFF : EFM_C_MUTED, 0xFFFFFFFF);
    bx += del_w + 8;
    
    /* 区切り */
    vga_fill_rect(bx, by + 4, 1, bh - 8, EFM_C_BORDER);
    bx += 8;
    
    /* 表示モードボタン */
    const char* view_labels[] = {"List", "Icons", "Details", "Thumbs"};
    const char* view_labels_ja[] = {"リスト", "アイコン", "詳細", "サムネイル"};
    for (int i = 0; i < 4; i++) {
        bool active = (state->view_mode == (efm_view_mode_t)i);
        const char* lbl = gui_is_japanese() ? view_labels_ja[i] : view_labels[i];
        int bw = (int)strlen(lbl) * FONT_W + 12;
        vga_fill_rounded_rect(bx, by, bw, bh, 4, active ? EFM_C_ACCENT : EFM_C_PANEL);
        vga_draw_string(bx + 6, by + 7, lbl, active ? 0x00FFFFFF : EFM_C_TEXT, 0xFFFFFFFF);
        bx += bw + 4;
    }
    
    /* 検索ボックス (右側) */
    int search_w = 200;
    int sx = x + w - search_w - 8;
    vga_fill_rounded_rect(sx, by, search_w, bh, 6, EFM_C_PANEL);
    vga_draw_rounded_rect(sx, by, search_w, bh, 6, state->search_active ? EFM_C_ACCENT : EFM_C_BORDER);
    if (state->search_text[0]) {
        vga_draw_string(sx + 8, by + 7, state->search_text, EFM_C_TEXT, 0xFFFFFFFF);
    } else {
        vga_draw_string(sx + 8, by + 7, gui_is_japanese() ? "検索..." : "Search...", EFM_C_MUTED, 0xFFFFFFFF);
    }
}

/* ブックマークサイドバー描画 */
void efm_draw_bookmarks(efm_state_t* state, int x, int y, int w, int h) {
    if (!state) return;
    
    vga_fill_rect(x, y, w, h, EFM_C_SIDEBAR);
    vga_fill_rect(x + w - 1, y, 1, h, EFM_C_BORDER);
    
    /* ヘッダー */
    vga_fill_rect(x, y, w, 28, EFM_C_TOOLBAR);
    vga_draw_string(x + 8, y + 8, gui_is_japanese() ? "ブックマーク" : "Bookmarks", EFM_C_MUTED, 0xFFFFFFFF);
    
    int iy = y + 32;
    for (int i = 0; i < state->bookmark_count && iy < y + h - 4; i++) {
        efm_bookmark_t* bm = &state->bookmarks[i];
        bool is_current = (strcmp(bm->path, state->current_path) == 0);
        
        if (is_current) {
            vga_fill_rounded_rect(x + 2, iy, w - 4, 26, 4, EFM_C_SELECT);
        }
        
        /* フォルダアイコン */
        efm_draw_file_icon(x + 6, iy + 5, 16, EFM_TYPE_FOLDER, true);
        
        /* 名前 */
        vga_draw_string(x + 26, iy + 7, bm->name, is_current ? EFM_C_ACCENT : EFM_C_TEXT, 0xFFFFFFFF);
        iy += 28;
    }
    
    /* クイックアクセス */
    if (iy < y + h - 60) {
        iy += 8;
        vga_fill_rect(x + 4, iy, w - 8, 1, EFM_C_BORDER);
        iy += 8;
        vga_draw_string(x + 8, iy, gui_is_japanese() ? "クイックアクセス" : "Quick Access", EFM_C_MUTED, 0xFFFFFFFF);
        iy += 20;
        
        /* よく使うパス */
        const char* quick_paths[] = {"/", "/desktop", "/documents", "/music", "/pictures"};
        const char* quick_names[] = {"Root", "Desktop", "Documents", "Music", "Pictures"};
        const char* quick_names_ja[] = {"ルート", "デスクトップ", "ドキュメント", "音楽", "ピクチャ"};
        
        for (int i = 0; i < 5 && iy < y + h - 4; i++) {
            bool is_current = (strcmp(quick_paths[i], state->current_path) == 0);
            if (is_current) vga_fill_rounded_rect(x + 2, iy, w - 4, 24, 4, EFM_C_HOVER);
            efm_draw_file_icon(x + 6, iy + 4, 14, EFM_TYPE_FOLDER, true);
            const char* name = gui_is_japanese() ? quick_names_ja[i] : quick_names[i];
            vga_draw_string(x + 24, iy + 6, name, EFM_C_TEXT, 0xFFFFFFFF);
            iy += 26;
        }
    }
}

/* ファイルリスト描画 */
void efm_draw_file_list(efm_state_t* state, int x, int y, int w, int h) {
    if (!state) return;
    
    vga_fill_rounded_rect(x, y, w, h, 8, EFM_C_PANEL);
    vga_draw_rounded_rect(x, y, w, h, 8, EFM_C_BORDER);
    
    int row_h = 0;
    int cols = 1;
    
    switch (state->view_mode) {
        case EFM_VIEW_LIST:     row_h = 26; cols = 1; break;
        case EFM_VIEW_ICONS:    row_h = 72; cols = (w - 16) / 80; if (cols < 1) cols = 1; break;
        case EFM_VIEW_DETAILS:  row_h = 28; cols = 1; break;
        case EFM_VIEW_THUMBNAILS: row_h = 80; cols = (w - 16) / 88; if (cols < 1) cols = 1; break;
    }
    
    /* 詳細表示ヘッダー */
    int list_y = y + 4;
    if (state->view_mode == EFM_VIEW_DETAILS) {
        vga_fill_rect(x + 4, list_y, w - 8, 22, EFM_C_TOOLBAR);
        vga_draw_string(x + 30, list_y + 5, gui_is_japanese() ? "名前" : "Name", EFM_C_MUTED, 0xFFFFFFFF);
        vga_draw_string(x + 8 + w - 280, list_y + 5, gui_is_japanese() ? "種類" : "Type", EFM_C_MUTED, 0xFFFFFFFF);
        vga_draw_string(x + 8 + w - 180, list_y + 5, gui_is_japanese() ? "サイズ" : "Size", EFM_C_MUTED, 0xFFFFFFFF);
        vga_draw_string(x + 8 + w - 90, list_y + 5, gui_is_japanese() ? "更新日時" : "Modified", EFM_C_MUTED, 0xFFFFFFFF);
        list_y += 24;
    }
    
    int visible_rows = (h - (list_y - y) - 4) / row_h;
    if (visible_rows < 1) visible_rows = 1;
    
    /* スクロール調整 */
    int total_rows = (state->entry_count + cols - 1) / cols;
    if (state->scroll_offset > total_rows - visible_rows) {
        state->scroll_offset = total_rows - visible_rows;
    }
    if (state->scroll_offset < 0) state->scroll_offset = 0;
    
    /* エントリ描画 */
    int start = state->scroll_offset * cols;
    int iy = list_y;
    
    for (int row = 0; row < visible_rows && start + row * cols < state->entry_count; row++) {
        for (int col = 0; col < cols; col++) {
            int idx = start + row * cols + col;
            if (idx >= state->entry_count) break;
            
            efm_entry_t* e = &state->entries[idx];
            
            /* 検索フィルター */
            if (state->search_active) {
                bool in_results = false;
                for (int r = 0; r < state->search_result_count; r++) {
                    if (state->search_results[r] == idx) { in_results = true; break; }
                }
                if (!in_results) continue;
            }
            
            int ix = x + 8 + col * (w / cols);
            int item_w = (state->view_mode == EFM_VIEW_LIST || state->view_mode == EFM_VIEW_DETAILS) ?
                         w - 16 : (w / cols) - 4;
            
            /* 選択・ホバー背景 */
            if (e->selected) {
                vga_fill_rounded_rect(ix - 2, iy + 1, item_w + 4, row_h - 2, 4, EFM_C_SELECT);
            } else if (state->focused_index == idx) {
                vga_fill_rounded_rect(ix - 2, iy + 1, item_w + 4, row_h - 2, 4, EFM_C_HOVER);
            }
            
            switch (state->view_mode) {
                case EFM_VIEW_LIST:
                case EFM_VIEW_DETAILS: {
                    /* アイコン */
                    efm_draw_file_icon(ix, iy + (row_h - 18) / 2, 18, e->type, e->is_dir);
                    
                    /* 名前 */
                    int name_x = ix + 22;
                    int max_name_w = (state->view_mode == EFM_VIEW_DETAILS) ? (w - 360) : (item_w - 24);
                    char display_name[128];
                    int max_chars = max_name_w / FONT_W;
                    efm_utf8_truncate(e->name, display_name, sizeof(display_name), max_chars);
                    vga_draw_string(name_x, iy + (row_h - FONT_H) / 2, display_name, EFM_C_TEXT, 0xFFFFFFFF);
                    
                    if (state->view_mode == EFM_VIEW_DETAILS) {
                        /* 種類 */
                        const char* type_lbl = gui_is_japanese() ? 
                            efm_get_type_label_ja(e->type) : efm_get_type_label(e->type);
                        vga_draw_string(ix + w - 280, iy + (row_h - FONT_H) / 2, type_lbl, EFM_C_MUTED, 0xFFFFFFFF);
                        
                        /* サイズ */
                        if (!e->is_dir) {
                            char size_buf[32];
                            efm_format_size(e->size, size_buf, sizeof(size_buf));
                            vga_draw_string(ix + w - 180, iy + (row_h - FONT_H) / 2, size_buf, EFM_C_MUTED, 0xFFFFFFFF);
                        }
                        
                        /* 更新日時 */
                        if (e->modified_time > 0) {
                            char time_buf[32];
                            efm_format_time(e->modified_time, time_buf, sizeof(time_buf));
                            vga_draw_string(ix + w - 90, iy + (row_h - FONT_H) / 2, time_buf, EFM_C_MUTED, 0xFFFFFFFF);
                        }
                    }
                    break;
                }
                
                case EFM_VIEW_ICONS: {
                    /* 大きなアイコン */
                    int icon_size = 40;
                    int icon_x = ix + (item_w - icon_size) / 2;
                    efm_draw_file_icon(icon_x, iy + 4, icon_size, e->type, e->is_dir);
                    
                    /* 名前 (中央揃え・UTF-8セーフ) */
                    char display_name[64];
                    efm_utf8_truncate(e->name, display_name, sizeof(display_name), 14);
                    int name_w = efm_utf8_display_width(display_name) * FONT_W;
                    int name_x2 = ix + (item_w - name_w) / 2;
                    vga_draw_string(name_x2, iy + icon_size + 8, display_name, EFM_C_TEXT, 0xFFFFFFFF);
                    break;
                }
                
                case EFM_VIEW_THUMBNAILS: {
                    /* サムネイル */
                    int thumb_x = ix + (item_w - EFM_THUMBNAIL_W) / 2;
                    int thumb_y = iy + 4;
                    
                    if (e->has_thumbnail && e->thumbnail_data) {
                        /* サムネイルをピクセル単位で描画 */
                        for (uint32_t ty = 0; ty < e->thumbnail_h && (int)(thumb_y + ty) < y + h; ty++) {
                            for (uint32_t tx = 0; tx < e->thumbnail_w; tx++) {
                                uint32_t off = (ty * e->thumbnail_w + tx) * 4;
                                uint64_t c = ((uint64_t)e->thumbnail_data[off+0] << 16) |
                                             ((uint64_t)e->thumbnail_data[off+1] << 8) |
                                             (uint64_t)e->thumbnail_data[off+2];
                                vga_set_pixel(thumb_x + (int)tx, thumb_y + (int)ty, c);
                            }
                        }
                        vga_draw_rect(thumb_x, thumb_y, EFM_THUMBNAIL_W, EFM_THUMBNAIL_H, EFM_C_BORDER);
                    } else {
                        /* プレースホルダー */
                        vga_fill_rounded_rect(thumb_x, thumb_y, EFM_THUMBNAIL_W, EFM_THUMBNAIL_H, 4,
                                              efm_type_color(e->type));
                        vga_draw_rounded_rect(thumb_x, thumb_y, EFM_THUMBNAIL_W, EFM_THUMBNAIL_H, 4, EFM_C_BORDER);
                        /* タイプラベル */
                        const char* tl = gui_is_japanese() ? efm_get_type_label_ja(e->type) : efm_get_type_label(e->type);
                        int tl_x = thumb_x + (EFM_THUMBNAIL_W - (int)strlen(tl) * FONT_W) / 2;
                        vga_draw_string(tl_x, thumb_y + (EFM_THUMBNAIL_H - FONT_H) / 2, tl, 0x00FFFFFF, 0xFFFFFFFF);
                    }
                    
                    /* 名前 (UTF-8セーフ) */
                    char display_name[48];
                    efm_utf8_truncate(e->name, display_name, sizeof(display_name), 10);
                    int name_w = efm_utf8_display_width(display_name) * FONT_W;
                    int name_x3 = ix + (item_w - name_w) / 2;
                    vga_draw_string(name_x3, iy + EFM_THUMBNAIL_H + 8, display_name, EFM_C_TEXT, 0xFFFFFFFF);
                    break;
                }
            }
        }
        iy += row_h;
    }
    
    /* スクロールバー */
    if (total_rows > visible_rows) {
        int sb_x = x + w - 12;
        int sb_h = h - (list_y - y) - 8;
        int sb_y = list_y + 2;
        vga_fill_rounded_rect(sb_x, sb_y, 8, sb_h, 4, EFM_C_BORDER);
        int thumb_h = sb_h * visible_rows / total_rows;
        if (thumb_h < 20) thumb_h = 20;
        int thumb_y2 = sb_y + (sb_h - thumb_h) * state->scroll_offset / (total_rows - visible_rows);
        vga_fill_rounded_rect(sb_x, thumb_y2, 8, thumb_h, 4, EFM_C_ACCENT);
    }
}

/* プレビューパネル描画 */
void efm_draw_preview(efm_state_t* state, int x, int y, int w, int h) {
    if (!state || !state->show_preview) return;
    
    vga_fill_rect(x, y, w, h, EFM_C_PANEL);
    vga_fill_rect(x, y, 1, h, EFM_C_BORDER);
    
    /* ヘッダー */
    vga_fill_rect(x, y, w, 28, EFM_C_TOOLBAR);
    vga_draw_string(x + 8, y + 8, gui_is_japanese() ? "プレビュー" : "Preview", EFM_C_MUTED, 0xFFFFFFFF);
    
    if (state->preview_entry_idx < 0 || state->preview_entry_idx >= state->entry_count) {
        vga_draw_string(x + 8, y + 40, gui_is_japanese() ? "ファイルを選択" : "Select a file", EFM_C_MUTED, 0xFFFFFFFF);
        return;
    }
    
    efm_entry_t* e = &state->entries[state->preview_entry_idx];
    int py = y + 32;
    
    /* ファイルアイコン */
    int icon_size = 48;
    int icon_x = x + (w - icon_size) / 2;
    efm_draw_file_icon(icon_x, py, icon_size, e->type, e->is_dir);
    py += icon_size + 8;
    
    /* ファイル名 */
    int name_w = (int)strlen(e->name) * FONT_W;
    int name_x = x + (w - name_w) / 2;
    if (name_x < x + 4) name_x = x + 4;
    vga_draw_string(name_x, py, e->name, EFM_C_TEXT, 0xFFFFFFFF);
    py += FONT_H + 8;
    
    /* 区切り線 */
    vga_fill_rect(x + 8, py, w - 16, 1, EFM_C_BORDER);
    py += 8;
    
    /* 詳細情報 */
    const char* type_lbl = gui_is_japanese() ? efm_get_type_label_ja(e->type) : efm_get_type_label(e->type);
    vga_draw_string(x + 8, py, gui_is_japanese() ? "種類:" : "Type:", EFM_C_MUTED, 0xFFFFFFFF);
    vga_draw_string(x + 60, py, type_lbl, EFM_C_TEXT, 0xFFFFFFFF);
    py += FONT_H + 6;
    
    if (!e->is_dir) {
        char size_buf[32];
        efm_format_size(e->size, size_buf, sizeof(size_buf));
        vga_draw_string(x + 8, py, gui_is_japanese() ? "サイズ:" : "Size:", EFM_C_MUTED, 0xFFFFFFFF);
        vga_draw_string(x + 60, py, size_buf, EFM_C_TEXT, 0xFFFFFFFF);
        py += FONT_H + 6;
    }
    
    if (e->modified_time > 0) {
        char time_buf[32];
        efm_format_time(e->modified_time, time_buf, sizeof(time_buf));
        vga_draw_string(x + 8, py, gui_is_japanese() ? "更新:" : "Modified:", EFM_C_MUTED, 0xFFFFFFFF);
        vga_draw_string(x + 68, py, time_buf, EFM_C_TEXT, 0xFFFFFFFF);
        py += FONT_H + 6;
    }
    
    /* 画像プレビュー */
    if (state->preview_active && e->type == EFM_TYPE_IMAGE &&
        image_viewer_is_loaded() && image_viewer_draw_scaled) {
        int img_x = x + 8;
        int img_y = py + 4;
        int img_w = w - 16;
        int img_h = h - (img_y - y) - 24;
        if (img_h > 160) img_h = 160;
        if (img_h < 72) img_h = 72;
        vga_fill_rect(img_x - 1, img_y - 1, img_w + 2, img_h + 2, EFM_C_BORDER);
        vga_fill_rect(img_x, img_y, img_w, img_h, 0x000000);
        image_viewer_draw_scaled((uint64_t)img_x, (uint64_t)img_y, (uint64_t)img_w, (uint64_t)img_h);
        py = img_y + img_h + 8;
    }

    /* テキストプレビュー */
    if (state->preview_text[0] && py < y + h - 20) {
        py += 4;
        vga_fill_rect(x + 4, py, w - 8, 1, EFM_C_BORDER);
        py += 8;
        
        /* テキストを行に分割して表示 */
        const char* text = state->preview_text;
        int line_h = FONT_H + 2;
        int max_lines = (y + h - py - 8) / line_h;
        int line_w = (w - 16) / FONT_W;
        
        char line_buf[128];
        int line_count = 0;
        
        while (*text && line_count < max_lines) {
            int i = 0;
            while (*text && *text != '\n' && i < line_w && i < 127) {
                line_buf[i++] = *text++;
            }
            line_buf[i] = '\0';
            if (*text == '\n') text++;
            
            vga_draw_string(x + 8, py, line_buf, EFM_C_TEXT, 0xFFFFFFFF);
            py += line_h;
            line_count++;
        }
    }
}

/* ステータスバー描画 */
void efm_draw_statusbar(efm_state_t* state, int x, int y, int w) {
    if (!state) return;
    
    vga_fill_rect(x, y, w, 24, EFM_C_TOOLBAR);
    vga_fill_rect(x, y, w, 1, EFM_C_BORDER);
    
    /* 項目数 */
    char count_buf[64];
    strncpy(count_buf, "", 63);
    char n_str[16];
    int n = state->entry_count, i = 0;
    if (n == 0) { n_str[i++] = '0'; }
    else { while (n > 0) { n_str[i++] = '0' + (n % 10); n /= 10; } }
    for (int j = 0; j < i/2; j++) { char t = n_str[j]; n_str[j] = n_str[i-1-j]; n_str[i-1-j] = t; }
    n_str[i] = '\0';
    strncat(count_buf, n_str, 63);
    strncat(count_buf, gui_is_japanese() ? " 項目" : " items", 63-strlen(count_buf));
    
    if (state->selected_count > 0) {
        strncat(count_buf, " (", 63-strlen(count_buf));
        char sel_str[16];
        int s = state->selected_count, si = 0;
        if (s == 0) { sel_str[si++] = '0'; }
        else { while (s > 0) { sel_str[si++] = '0' + (s % 10); s /= 10; } }
        for (int j = 0; j < si/2; j++) { char t = sel_str[j]; sel_str[j] = sel_str[si-1-j]; sel_str[si-1-j] = t; }
        sel_str[si] = '\0';
        strncat(count_buf, sel_str, 63-strlen(count_buf));
        strncat(count_buf, gui_is_japanese() ? " 選択中)" : " selected)", 63-strlen(count_buf));
    }
    
    vga_draw_string(x + 8, y + 5, count_buf, EFM_C_MUTED, 0xFFFFFFFF);
    
    /* ステータスメッセージ */
    if (state->status_msg[0] && state->status_timer > 0) {
        int msg_w = (int)strlen(state->status_msg) * FONT_W;
        vga_draw_string(x + w - msg_w - 8, y + 5, state->status_msg, EFM_C_ACCENT, 0xFFFFFFFF);
        /* タイマー減少 (フレームレートに依存) */
        state->status_timer -= 16;
        if (state->status_timer < 0) state->status_timer = 0;
    }
    
    /* 現在のパス */
    int path_x = x + (int)strlen(count_buf) * FONT_W + 20;
    vga_draw_string(path_x, y + 5, state->current_path, EFM_C_MUTED, 0xFFFFFFFF);
}

/* メイン描画関数 */
void efm_draw(efm_state_t* state, int win_x, int win_y, int win_w, int win_h) {
    if (!state || !state->initialized) return;
    
    /* レイアウト計算 */
    int toolbar_h = 40;
    int breadcrumb_h = 32;
    int statusbar_h = 24;
    int sidebar_w = 160;
    int preview_w = state->show_preview ? EFM_PREVIEW_W : 0;
    
    int content_x = win_x + sidebar_w;
    int content_y = win_y + toolbar_h + breadcrumb_h;
    int content_w = win_w - sidebar_w - preview_w;
    int content_h = win_h - toolbar_h - breadcrumb_h - statusbar_h;
    
    /* 背景 */
    vga_fill_rect(win_x, win_y, win_w, win_h, EFM_C_BG);
    
    /* ツールバー */
    efm_draw_toolbar(state, win_x, win_y, win_w);
    
    /* ブレッドクラム */
    efm_draw_breadcrumb(state, win_x + sidebar_w, win_y + toolbar_h, win_w - sidebar_w);
    
    /* サイドバー (ブックマーク) */
    efm_draw_bookmarks(state, win_x, win_y + toolbar_h, sidebar_w, win_h - toolbar_h - statusbar_h);
    
    /* ファイルリスト */
    efm_draw_file_list(state, content_x, content_y, content_w, content_h);
    
    /* プレビューパネル */
    if (state->show_preview) {
        efm_draw_preview(state, win_x + win_w - preview_w, win_y + toolbar_h, preview_w, win_h - toolbar_h - statusbar_h);
    }
    
    /* ステータスバー */
    efm_draw_statusbar(state, win_x, win_y + win_h - statusbar_h, win_w);
    
    /* ダイアログ */
    if (state->dialog_active) {
        efm_draw_dialog(state, win_x + win_w / 2, win_y + win_h / 2);
    }
    
    /* Image panel (side-by-side with file list for image preview) */
    if (state->show_preview && preview_w > 0) {
        int img_panel_x = win_x + win_w - preview_w;
        int img_panel_y = win_y + toolbar_h;
        efm_draw_image_panel(state, img_panel_x, img_panel_y, preview_w, win_h - toolbar_h - statusbar_h);
    }
    
    /* Context menu (file operations) */
    if (state->context_menu_visible) {
        int mx = mouse.x;
        int my = mouse.y;
        efm_draw_file_context_menu(state, mx, my);
    }
}

/* ダイアログ描画 */
void efm_draw_dialog(efm_state_t* state, int cx, int cy) {
    if (!state || !state->dialog_active) return;
    
    int dw = 360, dh = 120;
    int dx = cx - dw / 2;
    int dy = cy - dh / 2;
    
    /* The framebuffer compositor does not alpha-blend 0xAARRGGBB fills;
     * drawing a 0x40000000 overlay therefore replaced the entire desktop
     * with black.  Keep this as a window-local modal surface until a real
     * compositor is present. */
    
    /* ダイアログ背景 */
    vga_fill_rounded_rect(dx + 3, dy + 3, dw, dh, 10, 0x40000000);
    vga_fill_rounded_rect(dx, dy, dw, dh, 10, EFM_C_PANEL);
    vga_draw_rounded_rect(dx, dy, dw, dh, 10, EFM_C_BORDER);
    
    /* タイトル */
    const char* titles[] = {"Rename", "New File", "New Folder", "Properties", "Delete"};
    const char* titles_ja[] = {"名前を変更", "新規ファイル", "新規フォルダ", "プロパティ", "削除の確認"};
    int dt = state->dialog_type;
    if (dt < 0 || dt > 4) dt = 0;
    const char* title = gui_is_japanese() ? titles_ja[dt] : titles[dt];
    vga_fill_rect(dx, dy, dw, 32, EFM_C_TOOLBAR);
    vga_draw_string(dx + 12, dy + 9, title, EFM_C_TEXT, 0xFFFFFFFF);

    if (dt == 3) {
        /* プロパティ: 読み取り専用の情報表示 (更新日時は内部リビジョン値であり
         * 実際の日付ではないため、誤解を招かぬよう表示しない) */
        efm_entry_t* pe = (state->focused_index >= 0 && state->focused_index < state->entry_count)
            ? &state->entries[state->focused_index] : NULL;
        if (pe) {
            char line[96];
            vga_draw_string(dx + 16, dy + 46, pe->name, EFM_C_TEXT, 0xFFFFFFFF);

            snprintf(line, sizeof(line), gui_is_japanese() ? "種類: %s" : "Type: %s",
                     pe->is_dir ? (gui_is_japanese() ? "フォルダ" : "Folder") : (gui_is_japanese() ? "ファイル" : "File"));
            vga_draw_string(dx + 16, dy + 66, line, EFM_C_MUTED, 0xFFFFFFFF);

            if (pe->is_dir) {
                strncpy(line, gui_is_japanese() ? "サイズ: -" : "Size: -", sizeof(line) - 1);
            } else {
                char size_buf[32];
                efm_format_size(pe->size, size_buf, sizeof(size_buf));
                snprintf(line, sizeof(line), gui_is_japanese() ? "サイズ: %s" : "Size: %s", size_buf);
            }
            vga_draw_string(dx + 16, dy + 84, line, EFM_C_MUTED, 0xFFFFFFFF);

            snprintf(line, sizeof(line), gui_is_japanese() ? "読み取り専用: %s" : "Read-only: %s",
                     pe->is_readonly ? (gui_is_japanese() ? "はい" : "Yes") : (gui_is_japanese() ? "いいえ" : "No"));
            vga_draw_string(dx + 190, dy + 66, line, EFM_C_MUTED, 0xFFFFFFFF);
        }

        /* Close ボタン (OKボタンと同じ位置・当たり判定を再利用) */
        vga_fill_rounded_rect(dx + dw - 160, dy + dh - 36, 72, 26, 6, EFM_C_ACCENT);
        vga_draw_string(dx + dw - 148, dy + dh - 28, gui_is_japanese() ? "閉じる" : "Close", 0x00FFFFFF, 0xFFFFFFFF);
        return;
    }

    if (dt == 4) {
        /* 削除確認: 入力欄の代わりに確認メッセージを表示 */
        char msg[96];
        if (state->pending_delete_idx < 0) {
            snprintf(msg, sizeof(msg), gui_is_japanese() ? "選択中の %d 件を削除しますか?" : "Delete %d selected item(s)?", state->selected_count);
        } else if (state->pending_delete_idx < state->entry_count) {
            snprintf(msg, sizeof(msg), gui_is_japanese() ? "\"%s\" を削除しますか?" : "Delete \"%s\"?", state->entries[state->pending_delete_idx].name);
        } else {
            strncpy(msg, gui_is_japanese() ? "この項目を削除しますか?" : "Delete this item?", sizeof(msg)-1);
        }
        vga_draw_string(dx + 16, dy + 52, msg, EFM_C_TEXT, 0xFFFFFFFF);

        /* ボタン (削除=警告色 / キャンセル) */
        vga_fill_rounded_rect(dx + dw - 160, dy + dh - 36, 72, 26, 6, rgb(216, 84, 84));
        vga_draw_string(dx + dw - 148, dy + dh - 28, gui_is_japanese() ? "削除" : "Delete", 0x00FFFFFF, 0xFFFFFFFF);

        vga_fill_rounded_rect(dx + dw - 80, dy + dh - 36, 68, 26, 6, EFM_C_PANEL);
        vga_draw_rounded_rect(dx + dw - 80, dy + dh - 36, 68, 26, 6, EFM_C_BORDER);
        vga_draw_string(dx + dw - 66, dy + dh - 28, gui_is_japanese() ? "キャンセル" : "Cancel", EFM_C_TEXT, 0xFFFFFFFF);
        return;
    }
    
    /* 入力フィールド */
    vga_fill_rounded_rect(dx + 12, dy + 44, dw - 24, 28, 6, EFM_C_BG);
    vga_draw_rounded_rect(dx + 12, dy + 44, dw - 24, 28, 6, EFM_C_ACCENT);
    vga_draw_string(dx + 18, dy + 52, state->dialog_input, EFM_C_TEXT, 0xFFFFFFFF);
    
    /* カーソル */
    int cursor_x = dx + 18 + state->dialog_cursor * FONT_W;
    vga_fill_rect(cursor_x, dy + 52, 2, FONT_H, EFM_C_ACCENT);
    
    /* ボタン */
    vga_fill_rounded_rect(dx + dw - 160, dy + dh - 36, 72, 26, 6, EFM_C_ACCENT);
    vga_draw_string(dx + dw - 148, dy + dh - 28, gui_is_japanese() ? "OK" : "OK", 0x00FFFFFF, 0xFFFFFFFF);
    
    vga_fill_rounded_rect(dx + dw - 80, dy + dh - 36, 68, 26, 6, EFM_C_PANEL);
    vga_draw_rounded_rect(dx + dw - 80, dy + dh - 36, 68, 26, 6, EFM_C_BORDER);
    vga_draw_string(dx + dw - 66, dy + dh - 28, gui_is_japanese() ? "キャンセル" : "Cancel", EFM_C_TEXT, 0xFFFFFFFF);
}

/* ============================================================
 * Image Viewer Panel (right-side full-size display)
 * ============================================================ */
void efm_draw_image_panel(efm_state_t* state, int x, int y, int w, int h) {
    if (!state) return;
    
    /* Panel background */
    vga_fill_rect(x, y, w, h, EFM_C_SIDEBAR);
    vga_fill_rect(x, y, 1, h, EFM_C_BORDER);
    
    /* Panel title */
    vga_draw_string(x + 8, y + 4, gui_is_japanese() ? "プレビュー" : "Preview", EFM_C_TEXT, 0xFFFFFFFF);
    vga_fill_rect(x + 8, y + 16, w - 16, 1, EFM_C_BORDER);
    
    /* Check if focused entry is an image */
    if (state->focused_index < 0 || state->focused_index >= state->entry_count) {
        vga_draw_string(x + 8, y + 32, gui_is_japanese() ? "ファイルを選択" : "Select a file", EFM_C_MUTED, 0xFFFFFFFF);
        return;
    }
    
    efm_entry_t* e = &state->entries[state->focused_index];
    
    /* Show filename always */
    int py = y + 24;
    vga_draw_string(x + 8, py, gui_is_japanese() ? "ファイル: " : "File: ", EFM_C_MUTED, 0xFFFFFFFF);
    py += 16;
    vga_draw_string(x + 8, py, e->name, EFM_C_TEXT, 0xFFFFFFFF);
    py += 20;
    
    /* Show file type */
    const char* type_names[] = {"Image", "Audio", "Video", "Text", "Code", "Archive", "Folder", "Unknown"};
    const char* type_names_ja[] = {"画像", "音声", "動画", "テキスト", "コード", "アーカイブ", "フォルダ", "不明"};
    const char** type_text = gui_is_japanese() ? type_names_ja : type_names;
    int type_idx = (int)e->type;
    if (type_idx < 0 || type_idx >= 8) type_idx = 7;
    
    vga_draw_string(x + 8, py, gui_is_japanese() ? "種類: " : "Type: ", EFM_C_MUTED, 0xFFFFFFFF);
    vga_draw_string(x + 60, py, type_text[type_idx], EFM_C_TEXT, 0xFFFFFFFF);
    py += 20;
    
    /* Show file size */
    char size_str[64];
    if (e->size >= 1048576) {
        snprintf(size_str, sizeof(size_str), "%.1f MB", e->size / 1048576.0);
    } else if (e->size >= 1024) {
        snprintf(size_str, sizeof(size_str), "%.1f KB", e->size / 1024.0);
    } else {
        snprintf(size_str, sizeof(size_str), "%llu B", e->size);
    }
    vga_draw_string(x + 8, py, gui_is_japanese() ? "サイズ: " : "Size: ", EFM_C_MUTED, 0xFFFFFFFF);
    vga_draw_string(x + 60, py, size_str, EFM_C_TEXT, 0xFFFFFFFF);
    py += 24;
    
    /* Only try to display image if it's marked as an image file */
    if (e->type != EFM_TYPE_IMAGE) {
        vga_draw_string(x + 8, py, gui_is_japanese() ? "画像ファイルではありません" : "Not an image file", EFM_C_MUTED, 0xFFFFFFFF);
        return;
    }
    
    /* Try to load and display the image */
    extern int jpeg_viewer_load_file(const char* filename) __attribute__((weak));
    extern bool jpeg_viewer_is_loaded(void) __attribute__((weak));
    extern int jpeg_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) __attribute__((weak));
    extern const char* jpeg_viewer_get_filename(void) __attribute__((weak));

    if (!jpeg_viewer_load_file) {
        vga_draw_string(x + 8, py, "[jpeg_viewer_load_file NOT FOUND]", 0x00FF6B6B, 0xFFFFFFFF);
        serial_puts("[EFM] ERROR: jpeg_viewer_load_file is NULL\n");
        return;
    }

    /* Only re-decode when the selected file actually changed - re-reading
     * and re-decoding from disk on every single repaint (scrolling, cursor
     * blink, unrelated UI updates, ...) made the preview panel extremely
     * slow for no benefit, since the previous decode is still sitting in
     * the viewer's buffer and jpeg_viewer_is_loaded()/draw_scaled() don't
     * need a fresh load to keep working.
     *
     * s_efm_preview_cache_valid alone used to drive this, but it was only
     * ever set true on a *successful* load - a failing file (corrupt data,
     * unsupported format, ...) left it false forever, so `same_file` was
     * always false for that path and every single repaint re-ran the full
     * decode attempt again, forever (visible in serial logs as endless
     * "[EFM] Attempting to load image" / "Load result: <error>" spam for
     * as long as that file stayed selected). s_efm_preview_path_attempted
     * now tracks "have we already tried this exact path" independently of
     * whether that attempt succeeded, so a known-bad file is retried once
     * per selection instead of once per repaint. */
    static char s_efm_preview_cached_path[512] = {0};
    static bool s_efm_preview_cache_valid = false;
    static bool s_efm_preview_path_attempted = false;
    static int  s_efm_preview_last_load_result = 0;

    bool path_matches_cache = s_efm_preview_path_attempted &&
        strncmp(s_efm_preview_cached_path, e->full_path, sizeof(s_efm_preview_cached_path) - 1) == 0;

    /* The standalone Image Viewer and the EFM preview share one decoded
     * image buffer.  When the user opens the selected file, always honour a
     * live matching viewer image before consulting EFM's repaint cache.
     * Otherwise a redraw can immediately issue a second filesystem load;
     * a transient load failure then resets the shared viewer state and leaves
     * the newly opened Image Viewer blank. */
    bool viewer_has_selected_file = false;
    if (jpeg_viewer_is_loaded && jpeg_viewer_get_filename &&
        jpeg_viewer_is_loaded()) {
        const char* current = jpeg_viewer_get_filename();
        viewer_has_selected_file = current != NULL &&
            strncmp(current, e->full_path, sizeof(s_efm_preview_cached_path) - 1) == 0;
    }
    if (viewer_has_selected_file) {
        strncpy(s_efm_preview_cached_path, e->full_path,
                sizeof(s_efm_preview_cached_path) - 1);
        s_efm_preview_cached_path[sizeof(s_efm_preview_cached_path) - 1] = '\0';
        s_efm_preview_path_attempted = true;
        s_efm_preview_cache_valid = true;
        s_efm_preview_last_load_result = 0;
    }

    bool same_file = viewer_has_selected_file ||
                     (path_matches_cache && s_efm_preview_cache_valid);

    int load_result = 0;
    if (path_matches_cache && !s_efm_preview_cache_valid) {
        /* Already tried this exact path and it failed - replay the
         * remembered result instead of re-decoding on every repaint. */
        load_result = s_efm_preview_last_load_result;
    } else if (!same_file) {
        load_result = jpeg_viewer_load_file(e->full_path);

        strncpy(s_efm_preview_cached_path, e->full_path, sizeof(s_efm_preview_cached_path) - 1);
        s_efm_preview_cached_path[sizeof(s_efm_preview_cached_path) - 1] = '\0';
        s_efm_preview_path_attempted = true;
        s_efm_preview_cache_valid = (load_result == 0);
        s_efm_preview_last_load_result = load_result;
    }

    if (load_result != 0) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "Load error: %d", load_result);
        vga_draw_string(x + 8, py, err_msg, 0x00FF6B6B, 0xFFFFFFFF);
        return;
    }
    
    bool is_loaded = false;
    if (jpeg_viewer_is_loaded) {
        is_loaded = jpeg_viewer_is_loaded();
    } else {
        serial_puts("[EFM] ERROR: jpeg_viewer_is_loaded is NULL\n");
        vga_draw_string(x + 8, py, "[jpeg_viewer_is_loaded NOT FOUND]", 0x00FF6B6B, 0xFFFFFFFF);
        return;
    }
    
    if (!is_loaded) {
        vga_draw_string(x + 8, py, gui_is_japanese() ? "画像の読み込みに失敗しました" : "Failed to load image", EFM_C_MUTED, 0xFFFFFFFF);
        return;
    }
    
    /* Draw placeholder box and image inside */
    int img_pad = 8;
    int img_x = x + img_pad;
    int img_y = py + img_pad;
    int img_max_w = w - 2 * img_pad;
    int img_max_h = h - py - img_pad - 8;
    
    if (img_max_w <= 0 || img_max_h <= 0) {
        vga_draw_string(x + 8, py, gui_is_japanese() ? "表示領域が小さすぎます" : "Area too small", EFM_C_MUTED, 0xFFFFFFFF);
        return;
    }
    
    /* Draw a placeholder border for the image area */
    vga_draw_rounded_rect(img_x, img_y, img_max_w, img_max_h, 4, EFM_C_BORDER);
    
    /* Draw the image */
    if (jpeg_viewer_draw_scaled) {
        int draw_result = jpeg_viewer_draw_scaled((uint64_t)img_x, (uint64_t)img_y, (uint64_t)img_max_w, (uint64_t)img_max_h);
        if (draw_result != 0) {
            vga_draw_string(img_x + 8, img_y + 8, gui_is_japanese() ? "描画エラー" : "Draw error", 0x00FF6B6B, 0xFFFFFFFF);
        }
    } else {
        vga_draw_string(img_x + 8, img_y + 8, "[jpeg_viewer_draw_scaled NOT FOUND]", 0x00FF6B6B, 0xFFFFFFFF);
        serial_puts("[EFM] ERROR: jpeg_viewer_draw_scaled is NULL\n");
    }
}

/* ============================================================
 * Right-Click Context Menu (file operations)
 * ============================================================ */
void efm_show_file_context_menu(efm_state_t* state, int mx, int my, int idx) {
    if (!state) return;
    state->context_menu_visible = true;
    state->context_menu_x = mx;
    state->context_menu_y = my;
    state->context_menu_idx = idx;
}

void efm_draw_file_context_menu(efm_state_t* state, int mx, int my) {
    if (!state || !state->context_menu_visible) return;
    
    int menu_w = 180;
    int item_h = 28;
    int items = 6; /* Copy, Cut, Paste, Rename, Delete, Properties */
    int menu_h = items * item_h + 8;
    
    int menu_x = state->context_menu_x;
    int menu_y = state->context_menu_y;
    if (menu_x + menu_w > (int)SCREEN_W) menu_x = (int)SCREEN_W - menu_w - 4;
    if (menu_y + menu_h > (int)SCREEN_H) menu_y = (int)SCREEN_H - menu_h - 4;
    
    /* Draw background and border */
    vga_fill_rounded_rect(menu_x, menu_y, menu_w, menu_h, 4, EFM_C_PANEL);
    vga_draw_rounded_rect(menu_x, menu_y, menu_w, menu_h, 4, EFM_C_BORDER);
    
    /* Menu items */
    const char* items_en[] = {"Copy", "Cut", "Paste", "Rename", "Delete", "Properties"};
    const char* items_ja[] = {"コピー", "切り取り", "貼り付け", "リネーム", "削除", "プロパティ"};
    const char** items_text = gui_is_japanese() ? items_ja : items_en;
    
    for (int i = 0; i < items; i++) {
        int item_y = menu_y + 4 + i * item_h;
        bool hov = (mx >= menu_x && mx < menu_x + menu_w && my >= item_y && my < item_y + item_h);
        
        if (hov) {
            vga_fill_rect(menu_x + 2, item_y, menu_w - 4, item_h, EFM_C_HOVER);
        }
        
        vga_draw_string(menu_x + 12, item_y + 6, items_text[i], EFM_C_TEXT, 0xFFFFFFFF);
    }
}

void efm_handle_file_context_menu_click(efm_state_t* state, int mx, int my) {
    if (!state || !state->context_menu_visible) return;
    
    int menu_w = 180;
    int item_h = 28;
    int menu_x = state->context_menu_x;
    int menu_y = state->context_menu_y;
    if (menu_x + menu_w > (int)SCREEN_W) menu_x = (int)SCREEN_W - menu_w - 4;
    if (menu_y + (6 * item_h + 8) > (int)SCREEN_H) menu_y = (int)SCREEN_H - (6 * item_h + 8) - 4;
    
    if (mx < menu_x || mx >= menu_x + menu_w || my < menu_y || my >= menu_y + 6 * item_h + 8) {
        state->context_menu_visible = false;
        return;
    }
    
    int rel_y = my - menu_y - 4;
    if (rel_y < 0 || rel_y >= 6 * item_h) {
        state->context_menu_visible = false;
        return;
    }
    
    int item_idx = rel_y / item_h;
    int entry_idx = state->context_menu_idx;
    if (entry_idx < 0 || entry_idx >= state->entry_count) {
        state->context_menu_visible = false;
        return;
    }
    
    efm_entry_t* e = &state->entries[entry_idx];
    
    switch (item_idx) {
        case 0: /* Copy */
            efm_clipboard_copy_selection(state);
            break;
        case 1: /* Cut */
            efm_clipboard_cut_selection(state);
            break;
        case 2: /* Paste */
            efm_paste(state);
            break;
        case 3: /* Rename */
            state->dialog_type = 0;
            state->dialog_active = true;
            strncpy(state->dialog_input, e->name, sizeof(state->dialog_input) - 1);
            state->dialog_cursor = (int)strlen(state->dialog_input);
            state->focused_index = entry_idx;
            break;
        case 4: /* Delete */
            state->dialog_type = 4;
            state->dialog_active = true;
            state->pending_delete_idx = entry_idx;
            break;
        case 5: /* Properties */
            state->dialog_type = 3;
            state->dialog_active = true;
            state->focused_index = entry_idx;
            break;
    }
    
    state->context_menu_visible = false;
}
