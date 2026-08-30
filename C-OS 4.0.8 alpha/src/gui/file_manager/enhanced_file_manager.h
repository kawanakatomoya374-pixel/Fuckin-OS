/**
 * enhanced_file_manager.h - C-OS 5.0.0 Enhanced File Manager
 * 
 * 大幅強化されたファイルマネージャー
 * 
 * 新機能:
 *   - サムネイル表示 (JPEG/PNG プレビュー)
 *   - ドラッグ&ドロップ
 *   - ファイルプレビューパネル
 *   - ブックマーク・お気に入り
 *   - 詳細情報表示 (サイズ・日付・種類・パーミッション)
 *   - 複数ファイル選択
 *   - ファイル検索 (正規表現対応)
 *   - ファイル圧縮・展開
 *   - ファイルプロパティダイアログ
 *   - ナビゲーション履歴 (戻る/進む)
 *   - ブレッドクラムナビゲーション
 *   - ファイルタイプ別アイコン
 *   - ソート (名前・サイズ・日付・種類)
 *   - フィルタリング
 */
#ifndef ENHANCED_FILE_MANAGER_H
#define ENHANCED_FILE_MANAGER_H

#include "../include/types.h"
#include "../fs/fs.h"
#include <stdbool.h>
#include <stdint.h>

/* ファイルマネージャー設定 */
#define EFM_MAX_PATH        1024
#define EFM_MAX_ENTRIES     512
#define EFM_MAX_SELECTED    64
#define EFM_MAX_BOOKMARKS   32
#define EFM_MAX_HISTORY     64
#define EFM_MAX_SEARCH_LEN  128
#define EFM_THUMBNAIL_W     64
#define EFM_THUMBNAIL_H     48
#define EFM_PREVIEW_W       280

/* 表示モード */
typedef enum {
    EFM_VIEW_LIST       = 0,    /* リスト表示 */
    EFM_VIEW_ICONS      = 1,    /* アイコン表示 */
    EFM_VIEW_DETAILS    = 2,    /* 詳細表示 */
    EFM_VIEW_THUMBNAILS = 3,    /* サムネイル表示 */
} efm_view_mode_t;

/* ソートモード */
typedef enum {
    EFM_SORT_NAME       = 0,
    EFM_SORT_SIZE       = 1,
    EFM_SORT_DATE       = 2,
    EFM_SORT_TYPE       = 3,
} efm_sort_mode_t;

/* ファイルタイプ */
typedef enum {
    EFM_TYPE_UNKNOWN    = 0,
    EFM_TYPE_FOLDER     = 1,
    EFM_TYPE_TEXT       = 2,
    EFM_TYPE_IMAGE      = 3,    /* JPEG/PNG/BMP */
    EFM_TYPE_AUDIO      = 4,    /* MP3/WAV/OGG */
    EFM_TYPE_VIDEO      = 5,    /* MP4/AVI */
    EFM_TYPE_CODE       = 6,    /* C/H/LUA/PY */
    EFM_TYPE_ARCHIVE    = 7,    /* ZIP/TAR/GZ */
    EFM_TYPE_EXECUTABLE = 8,    /* ELF/EXE */
    EFM_TYPE_CONFIG     = 10,   /* CFG/INI/JSON */
    EFM_TYPE_LUA        = 11,   /* Lua スクリプト */
    EFM_TYPE_PARENT     = 99,   /* 親ディレクトリ (..) */
} efm_file_type_t;

/* ファイルエントリ */
typedef struct {
    char            name[256];
    char            full_path[EFM_MAX_PATH];
    uint64_t        size;
    uint64_t        modified_time;
    uint64_t        created_time;
    bool            is_dir;
    bool            is_hidden;
    bool            is_readonly;
    efm_file_type_t type;
    bool            selected;
    bool            has_thumbnail;
    uint8_t*        thumbnail_data;    /* RGBA サムネイルデータ */
    uint32_t        thumbnail_w;
    uint32_t        thumbnail_h;
} efm_entry_t;

/* ブックマーク */
typedef struct {
    char name[64];
    char path[EFM_MAX_PATH];
    efm_file_type_t type;
} efm_bookmark_t;

/* ファイルマネージャー状態 */
typedef struct {
    /* 現在のパス */
    char current_path[EFM_MAX_PATH];
    
    /* ナビゲーション履歴 */
    char history[EFM_MAX_HISTORY][EFM_MAX_PATH];
    int  history_count;
    int  history_pos;
    
    /* ファイルリスト */
    efm_entry_t entries[EFM_MAX_ENTRIES];
    int         entry_count;
    int         selected_count;
    int         focused_index;
    int         scroll_offset;
    
    /* 表示設定 */
    efm_view_mode_t view_mode;
    efm_sort_mode_t sort_mode;
    bool            sort_reverse;
    bool            show_hidden;
    bool            show_preview;
    
    /* 検索 */
    char search_text[EFM_MAX_SEARCH_LEN];
    bool search_active;
    bool search_regex;
    int  search_results[EFM_MAX_ENTRIES];
    int  search_result_count;
    
    /* ブックマーク */
    efm_bookmark_t bookmarks[EFM_MAX_BOOKMARKS];
    int            bookmark_count;
    
    /* ドラッグ&ドロップ */
    bool drag_active;
    int  drag_source_idx;
    int  drag_x, drag_y;
    
    /* クリップボード */
    char clipboard_paths[EFM_MAX_SELECTED][EFM_MAX_PATH];
    int  clipboard_count;
    bool clipboard_is_cut;
    
    /* プレビュー */
    bool    preview_active;
    int     preview_entry_idx;
    char    preview_text[4096];
    uint8_t* preview_image;
    uint32_t preview_image_w;
    uint32_t preview_image_h;
    
    /* ダイアログ */
    bool    dialog_active;
    int     dialog_type;    /* 0=rename 1=new_file 2=new_folder 3=properties 4=confirm_delete */
    char    dialog_input[256];
    int     dialog_cursor;
    int     pending_delete_idx; /* dialog_type==4 用: -1=選択中の項目をまとめて削除, >=0=単一項目 */
    
    /* コンテキストメニュー */
    bool    context_menu_visible;
    int     context_menu_x;
    int     context_menu_y;
    int     context_menu_idx;
    
    /* ステータス */
    char status_msg[256];
    int  status_timer;
    
    /* 操作中フラグ */
    bool loading;
    bool initialized;
} efm_state_t;

/* ファイルマネージャー API */
void efm_init(efm_state_t* state);
void efm_cleanup(efm_state_t* state);

/* ナビゲーション */
int  efm_navigate(efm_state_t* state, const char* path);
int  efm_navigate_up(efm_state_t* state);
int  efm_navigate_back(efm_state_t* state);
int  efm_navigate_forward(efm_state_t* state);
bool efm_can_go_back(const efm_state_t* state);
bool efm_can_go_forward(const efm_state_t* state);

/* ファイル操作 */
int  efm_refresh(efm_state_t* state);
int  efm_create_file(efm_state_t* state, const char* name);
int  efm_create_folder(efm_state_t* state, const char* name);
int  efm_rename(efm_state_t* state, int idx, const char* new_name);
int  efm_delete(efm_state_t* state, int idx);
int  efm_delete_selected(efm_state_t* state);
int  efm_copy(efm_state_t* state, int idx, const char* dest_path);
int  efm_move(efm_state_t* state, int idx, const char* dest_path);
int  efm_paste(efm_state_t* state);
void efm_clipboard_copy_selection(efm_state_t* state);
void efm_clipboard_cut_selection(efm_state_t* state);
int  efm_find_entry_by_path(const efm_state_t* state, const char* path);
void efm_delete_confirmed(efm_state_t* state);

/* ファイルマネージャーの既定動作 (設定画面から変更可能で、以後新しく開く
 * ファイルマネージャーウィンドウ全てに適用される) */
bool efm_get_default_show_hidden(void);
void efm_set_default_show_hidden(bool value);
int  efm_get_default_view_mode(void);
void efm_set_default_view_mode(int mode);
bool efm_get_confirm_delete(void);
void efm_set_confirm_delete(bool value);

/* 選択 */
void efm_select(efm_state_t* state, int idx, bool multi);
void efm_select_all(efm_state_t* state);
void efm_deselect_all(efm_state_t* state);
int  efm_get_selected_count(const efm_state_t* state);

/* 検索 */
int  efm_search(efm_state_t* state, const char* query);
void efm_search_clear(efm_state_t* state);

/* ソート */
void efm_sort(efm_state_t* state, efm_sort_mode_t mode, bool reverse);

/* ブックマーク */
int  efm_bookmark_add(efm_state_t* state, const char* name, const char* path);
int  efm_bookmark_remove(efm_state_t* state, int idx);
void efm_bookmark_navigate(efm_state_t* state, int idx);

/* サムネイル */
void efm_load_thumbnail(efm_state_t* state, int idx);
void efm_load_all_thumbnails(efm_state_t* state);

/* プレビュー */
void efm_load_preview(efm_state_t* state, int idx);
void efm_clear_preview(efm_state_t* state);

/* ファイルタイプ判定 */
efm_file_type_t efm_get_file_type(const char* name);
const char* efm_get_type_label(efm_file_type_t type);
const char* efm_get_type_label_ja(efm_file_type_t type);

/* サイズフォーマット */
void efm_format_size(uint64_t size, char* buf, size_t buf_size);
void efm_format_time(uint64_t time, char* buf, size_t buf_size);

/* 描画 */
void efm_draw(efm_state_t* state, int win_x, int win_y, int win_w, int win_h);
void efm_draw_toolbar(efm_state_t* state, int x, int y, int w);
void efm_draw_breadcrumb(efm_state_t* state, int x, int y, int w);
void efm_draw_file_list(efm_state_t* state, int x, int y, int w, int h);
void efm_draw_preview(efm_state_t* state, int x, int y, int w, int h);
void efm_draw_statusbar(efm_state_t* state, int x, int y, int w);
void efm_draw_bookmarks(efm_state_t* state, int x, int y, int w, int h);
void efm_draw_dialog(efm_state_t* state, int cx, int cy);

/* 入力処理 */
bool efm_handle_click(efm_state_t* state, int mx, int my, int win_x, int win_y, int win_w, int win_h, bool right_click);
bool efm_handle_key(efm_state_t* state, int key, char ascii, bool ctrl, bool shift);
bool efm_handle_double_click(efm_state_t* state, int mx, int my, int win_x, int win_y, int win_w, int win_h);

/* ファイルを開く */
void efm_open_entry(efm_state_t* state, int idx);

/* ステータスメッセージ */
void efm_set_status(efm_state_t* state, const char* msg, int duration_ms);

#endif /* ENHANCED_FILE_MANAGER_H */

/* Image viewer panel (right-side image preview) */
void efm_draw_image_panel(efm_state_t* state, int x, int y, int w, int h);

/* File context menu (right-click) */
void efm_show_file_context_menu(efm_state_t* state, int mx, int my, int idx);
void efm_draw_file_context_menu(efm_state_t* state, int mx, int my);
void efm_handle_file_context_menu_click(efm_state_t* state, int mx, int my);
bool efm_handle_right_click(efm_state_t* state, int mx, int my, int win_x, int win_y, int win_w, int win_h);
