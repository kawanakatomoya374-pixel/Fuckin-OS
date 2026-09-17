/**
 * file_manager_explorer.h - Windows Explorer-style File Manager
 * 
 * C-OS 5.0.0 Windowsエクスプローラー風ファイルマネージャー
 * 詳細リスト表示、サイドバー、ドラッグ&ドロップ対応
 */

#ifndef FILE_MANAGER_EXPLORER_H
#define FILE_MANAGER_EXPLORER_H

#include <stdint.h>
#include <stdbool.h>
#include "../fs/fs_unified.h"

/* View modes */
typedef enum {
    FM_VIEW_LIST,
    FM_VIEW_GRID,
    FM_VIEW_DETAILS,
} fm_view_mode_t;

/* Sort modes */
typedef enum {
    FM_SORT_NAME,
    FM_SORT_SIZE,
    FM_SORT_DATE,
    FM_SORT_TYPE,
} fm_sort_mode_t;

/* Sidebar items */
typedef enum {
    FM_SIDEBAR_QUICK_ACCESS,
    FM_SIDEBAR_DESKTOP,
    FM_SIDEBAR_DOCUMENTS,
    FM_SIDEBAR_DOWNLOADS,
    FM_SIDEBAR_MUSIC,
    FM_SIDEBAR_PICTURES,
    FM_SIDEBAR_VIDEOS,
    FM_SIDEBAR_PC,
    FM_SIDEBAR_NETWORK,
    FM_SIDEBAR_BOOKMARKS,
} fm_sidebar_item_t;

/* File manager state */
typedef struct {
    char current_path[FS_UNIFIED_MAX_PATH];
    char history[16][FS_UNIFIED_MAX_PATH];
    int history_index;
    int history_count;

    fm_view_mode_t view_mode;
    fm_sort_mode_t sort_mode;
    bool sort_ascending;
    /* Sort column widths for the Windows-Explorer style list. */
    int col_name_x;
    int col_size_x;
    int col_type_x;
    int col_modified_x;

    fs_unified_dirent_t entries[128];
    int entry_count;
    int selected_index;
    int scroll_offset;
    int selected_indices[128];  /* For multi-selection */
    int selected_count;

    char search_query[FS_UNIFIED_MAX_PATH];
    bool search_active;
    bool show_hidden;

    /* Clipboard */
    char clipboard_path[FS_UNIFIED_MAX_PATH];
    bool clipboard_is_cut;

    /* Sidebar state */
    int sidebar_selected;
    bool sidebar_expanded[10];

    /* Column widths for the file list */
    int file_col_name_w;
    int file_col_size_w;
    int file_col_type_w;
    int file_col_modified_w;

    /* UI state */
    int window_x, window_y;
    int window_w, window_h;
    int sidebar_width;
    int toolbar_height;
    int titlebar_height;
    int menubar_height;
    int searchbar_height;
    int statusbar_height;
    int column_header_height;
} file_manager_state_t;

/* ============================================================
 * Initialization & Lifecycle
 * ============================================================ */

/**
 * Initialize file manager
 * @return 0 on success, -1 on error
 */
int fm_explorer_init(void);

/**
 * Create a new file manager window
 * @param x Window X position
 * @param y Window Y position
 * @param w Window width
 * @param h Window height
 * @return Pointer to file manager state, NULL on error
 */
file_manager_state_t* fm_explorer_create_window(int x, int y, int w, int h);

/**
 * Destroy a file manager window
 * @param fm File manager state
 */
void fm_explorer_destroy_window(file_manager_state_t* fm);

/* ============================================================
 * Navigation
 * ============================================================ */

/**
 * Open a path in the file manager
 * @param fm File manager state
 * @param path Path to open
 * @return 0 on success, -1 on error
 */
int fm_explorer_open_path(file_manager_state_t* fm, const char* path);

/**
 * Navigate back
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_navigate_back(file_manager_state_t* fm);

/**
 * Navigate forward
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_navigate_forward(file_manager_state_t* fm);

/**
 * Navigate up (parent directory)
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_navigate_up(file_manager_state_t* fm);

/**
 * Refresh current directory
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_refresh(file_manager_state_t* fm);

/* ============================================================
 * View & Display
 * ============================================================ */

/**
 * Set view mode
 * @param fm File manager state
 * @param mode View mode (LIST, GRID, DETAILS)
 */
void fm_explorer_set_view_mode(file_manager_state_t* fm, fm_view_mode_t mode);

/**
 * Set sort mode
 * @param fm File manager state
 * @param mode Sort mode (NAME, SIZE, DATE, TYPE)
 * @param ascending Sort order (true = ascending, false = descending)
 */
void fm_explorer_set_sort_mode(file_manager_state_t* fm, fm_sort_mode_t mode, bool ascending);

/**
 * Draw the file manager window
 * @param fm File manager state
 */
void fm_explorer_draw(file_manager_state_t* fm);

/**
 * Draw the toolbar
 * @param fm File manager state
 */
void fm_explorer_draw_toolbar(file_manager_state_t* fm);

/**
 * Draw the sidebar
 * @param fm File manager state
 */
void fm_explorer_draw_sidebar(file_manager_state_t* fm);

/**
 * Draw the main file list
 * @param fm File manager state
 */
void fm_explorer_draw_file_list(file_manager_state_t* fm);

/**
 * Draw the status bar
 * @param fm File manager state
 */
void fm_explorer_draw_statusbar(file_manager_state_t* fm);

/* ============================================================
 * File Operations
 * ============================================================ */

/**
 * Copy selected file
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_copy(file_manager_state_t* fm);

/**
 * Cut selected file
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_cut(file_manager_state_t* fm);

/**
 * Paste from clipboard
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_paste(file_manager_state_t* fm);

/**
 * Delete selected file
 * @param fm File manager state
 * @return 0 on success, -1 on error
 */
int fm_explorer_delete(file_manager_state_t* fm);

/**
 * Rename selected file
 * @param fm File manager state
 * @param new_name New name
 * @return 0 on success, -1 on error
 */
int fm_explorer_rename(file_manager_state_t* fm, const char* new_name);

/**
 * Create new folder
 * @param fm File manager state
 * @param name Folder name
 * @return 0 on success, -1 on error
 */
int fm_explorer_create_folder(file_manager_state_t* fm, const char* name);

/* ============================================================
 * Search & Filter
 * ============================================================ */

/**
 * Search for files
 * @param fm File manager state
 * @param query Search query
 * @return Number of results, -1 on error
 */
int fm_explorer_search(file_manager_state_t* fm, const char* query);

/**
 * Toggle hidden files visibility
 * @param fm File manager state
 */
void fm_explorer_toggle_hidden(file_manager_state_t* fm);

/* ============================================================
 * Event Handling
 * ============================================================ */

/**
 * Handle mouse click
 * @param fm File manager state
 * @param x Mouse X coordinate
 * @param y Mouse Y coordinate
 * @param button Mouse button (1=left, 2=middle, 3=right)
 */
void fm_explorer_handle_mouse_click(file_manager_state_t* fm, int x, int y, int button);

/**
 * Handle mouse drag
 * @param fm File manager state
 * @param x Mouse X coordinate
 * @param y Mouse Y coordinate
 */
void fm_explorer_handle_mouse_drag(file_manager_state_t* fm, int x, int y);

/**
 * Handle keyboard input
 * @param fm File manager state
 * @param key Key code
 */
void fm_explorer_handle_key(file_manager_state_t* fm, int key);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/**
 * Get file type color
 * @param type File type
 * @return Color value (0xRRGGBB)
 */
uint32_t fm_explorer_get_type_color(fs_unified_type_t type);

/**
 * Get file type icon
 * @param type File type
 * @return Icon character
 */
char fm_explorer_get_type_icon(fs_unified_type_t type);

/**
 * Format file size for display
 * @param size File size in bytes
 * @param buf Output buffer
 * @param buf_size Buffer size
 */
void fm_explorer_format_size(uint64_t size, char* buf, int buf_size);

#endif /* FILE_MANAGER_EXPLORER_H */
