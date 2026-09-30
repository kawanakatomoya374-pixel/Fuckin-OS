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
#define EFM_MAX_TREE_NODES  128
#define EFM_MAX_TREE_EXPANDED 32
#define EFM_TREE_ROW_H      22
#define EFM_TREE_INDENT     14
#define EFM_MAX_ENTRIES     512
#define EFM_MAX_SELECTED    64
#define EFM_MAX_BOOKMARKS   32
#define EFM_MAX_HISTORY     64
#define EFM_MAX_SEARCH_LEN  128
#define EFM_THUMBNAIL_W     64
#define EFM_THUMBNAIL_H     48
#define EFM_PREVIEW_W       280
/* Below this content-pane width, the Type/Size/Modified columns are
 * hidden and Name takes the full row - see efm_draw_file_list()'s
 * show_extra_cols. 430 = 280 (combined Type+Size+Modified reserved
 * width, matching the existing w-280/w-180/w-90 offsets) + 150 (a
 * minimum usable Name column) - below this, those columns would
 * overlap the Name column or run out of the pane entirely. */
#define EFM_EXTRA_COLS_MIN_W 430

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
    uint32_t        thumbnail_generation;  /* bumped whenever thumbnail_data is (re)loaded - see efm_load_thumbnail() */
    uint8_t         attr;              /* FAT attributes: 0x01 R, 0x02 H, 0x04 S, 0x20 A */
} efm_entry_t;

/* ブックマーク */
typedef struct {
    char name[64];
    char path[EFM_MAX_PATH];
    efm_file_type_t type;
} efm_bookmark_t;

/* One visible row of the sidebar directory tree. */
typedef struct {
    char path[EFM_MAX_PATH];
    char name[128];
    int  depth;          /* 0 = root; drives the indentation */
    bool is_expanded;
    bool has_children;   /* whether to draw a disclosure triangle at all */
} efm_tree_node_t;

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

    /* Hierarchical directory tree shown in the sidebar - the expandable
     * folder structure a modern file manager has, replacing a flat list
     * of fixed shortcuts that could not show where the current directory
     * actually sits in the filesystem.
     *
     * Stored as a FLAT array in display order rather than as a node graph
     * with child pointers: the sidebar only ever renders it top-to-bottom,
     * so display order IS the useful order, and a flat array makes
     * hit-testing a click trivial (index = y offset / row height) with no
     * traversal. `depth` carries the nesting level for indentation, which
     * is all the rendering needs to reconstruct the hierarchy visually. */
    efm_tree_node_t tree[EFM_MAX_TREE_NODES];
    int             tree_count;
    /* Paths the user has expanded. Kept SEPARATELY from the node array
     * because that array is rebuilt from scratch on every refresh
     * (directories change on disk), while expansion is UI state that must
     * survive the rebuild - storing "expanded" only on the node would
     * silently collapse the entire tree every time it refreshed. */
    char            expanded[EFM_MAX_TREE_EXPANDED][EFM_MAX_PATH];
    int             expanded_count;
    
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
    /* ---- Windows-style shell (efm_winui.c) ---- */
    int     open_menu;          /* -1 = none, else index into the menu bar */
    int     menu_hover;
    int     tree_w;             /* splitter position (tree pane width) */
    bool    split_drag;
    int     hover_row;
    /* drag and drop */
    bool    dnd_armed;          /* left button went down on an item */
    bool    dnd_active;         /* moved far enough: dragging */
    int     dnd_start_x, dnd_start_y;
    int     dnd_item;           /* item pressed */
    int     dnd_count;
    char    dnd_paths[EFM_MAX_SELECTED][EFM_MAX_PATH];
    bool    last_left;          /* mouse button state last frame */
    bool    close_requested;    /* File > Close */
} efm_state_t;

/* One place for every region of the window: the renderer, the click
 * handler and the double-click detection in the bridge all use this, so
 * they cannot disagree about where things are (they used to repeat the
 * same constants in three files). */
typedef struct {
    int x, y, w, h;                 /* whole client area */
    int menu_y, menu_h;
    int tool_y, tool_h;
    int places_y, places_h;
    int crumb_y, crumb_h;
    int body_y, body_h;
    int tree_x, tree_w;
    int split_x;                    /* 4 px splitter */
    int list_x, list_w;
    int header_h;                   /* details view column header */
    int preview_x, preview_w;
    int status_y, status_h;
    int row_h;
    int col_name_w, col_size_w, col_type_w, col_date_w, col_attr_w;
} efm_layout_t;
void efm_compute_layout(const efm_state_t* state, int x, int y, int w, int h, efm_layout_t* L);
int  efm_list_hit_row(const efm_state_t* state, const efm_layout_t* L, int mx, int my);   /* -1 if none */
void efm_winui_draw(efm_state_t* state, int x, int y, int w, int h);
bool efm_winui_click(efm_state_t* state, int mx, int my, int x, int y, int w, int h, bool right_click);
void efm_winui_arm_drag(efm_state_t* state, int idx, int mx, int my);
void efm_icon16(int x, int y, const efm_entry_t* e);
void efm_drop_paths_into(const char (*paths)[EFM_MAX_PATH], int count, const char* dest_dir, bool copy);
void efm_winui_tick(efm_state_t* state, void* window, int x, int y, int w, int h);   /* per frame: hover, splitter, drag & drop */

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
