/**
 * storage_gui.h - Advanced Storage Manager GUI
 */

#ifndef STORAGE_GUI_H
#define STORAGE_GUI_H

#include "types.h"

#define STORAGE_GUI_MAX_PARTITIONS  21

/* Storage partition UI info */
typedef struct {
    int id;
    char name[32];
    char label[48];
    uint64_t total_mb;
    uint64_t used_mb;
    uint64_t free_mb;
    uint64_t used_percent;
    char role_desc[64];
    int mounted;
    int selected;
    uint64_t color;
} storage_gui_partition_t;

/* Storage manager window */
typedef struct {
    int active;
    int x, y, w, h;
    
    /* View mode: 0=grid, 1=list, 2=detail */
    int view_mode;
    
    /* Selected partition */
    int selected_partition;
    
    /* Scroll position */
    int scroll_y;
    
    /* Action menu */
    int show_actions;
    int action_x, action_y;
    
    /* Partitions */
    storage_gui_partition_t partitions[STORAGE_GUI_MAX_PARTITIONS];
    int partition_count;
    
    /* Status message */
    char status_msg[128];
    uint64_t status_time;
} storage_gui_t;

/* Initialization */
void storage_gui_init(void);
void storage_gui_open(void);
void storage_gui_close(void);
int storage_gui_is_open(void);

/* Update */
void storage_gui_refresh(void);
void storage_gui_update_partition(int id);

/* Rendering */
void storage_gui_render(void);
void storage_gui_render_grid(void);
void storage_gui_render_list(void);
void storage_gui_render_detail(void);
void storage_gui_render_partition_card(int idx, int x, int y, int w, int h);
void storage_gui_render_partition_list_item(int idx, int x, int y, int w);
void storage_gui_render_progress_bar(int x, int y, int w, int h, int percent, uint64_t color);
void storage_gui_render_toolbar(void);
void storage_gui_render_actions_menu(void);
void storage_gui_render_status(void);

/* Input handling */
void storage_gui_handle_click(int mx, int my);
void storage_gui_handle_key(char key);
void storage_gui_handle_scroll(int delta);

/* Actions */
void storage_gui_mount_selected(void);
void storage_gui_unmount_selected(void);
void storage_gui_format_selected(void);
void storage_gui_backup_selected(void);
void storage_gui_check_selected(void);
void storage_gui_defrag_selected(void);

/* Utilities */
const char* storage_gui_format_size(uint64_t mb);
void storage_gui_set_status(const char* msg);

#endif /* STORAGE_GUI_H */
