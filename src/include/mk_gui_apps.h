/**
 * mk_gui_apps.h - Enhanced GUI Applications Header
 * C-OS 4.0.8 alpha Enhanced GUI Applications
 */
#ifndef MK_GUI_APPS_H
#define MK_GUI_APPS_H

#include "types.h"

// GUI Apps constants
#define MK_MAX_APPLICATIONS 32
#define MK_MAX_APP_WINDOWS 128
#define MK_MAX_APP_WIDGETS 256

// Application types
#define MK_APP_TYPE_SYSTEM 1
#define MK_APP_TYPE_OFFICE 2
#define MK_APP_TYPE_INTERNET 3
#define MK_APP_TYPE_MULTIMEDIA 4
#define MK_APP_TYPE_GAMES 5
#define MK_APP_TYPE_DEVELOPMENT 6
#define MK_APP_TYPE_GRAPHICS 7
#define MK_APP_TYPE_FILE_MANAGER 8
#define MK_APP_TYPE_TERMINAL 9
#define MK_APP_TYPE_TEXT_EDITOR 10
#define MK_APP_TYPE_WEB_BROWSER 11

// Window types
#define MK_WINDOW_TYPE_MAIN 1
#define MK_WINDOW_TYPE_DIALOG 2
#define MK_WINDOW_TYPE_POPUP 3
#define MK_WINDOW_TYPE_TOOLBAR 4
#define MK_WINDOW_TYPE_STATUS 5

// Widget types
#define MK_WIDGET_BUTTON 1
#define MK_WIDGET_LABEL 2
#define MK_WIDGET_TEXTBOX 3
#define MK_WIDGET_CHECKBOX 4
#define MK_WIDGET_RADIO 5
#define MK_WIDGET_LISTBOX 6
#define MK_WIDGET_COMBOBOX 7
#define MK_WIDGET_PROGRESS 8
#define MK_WIDGET_SLIDER 9
#define MK_WIDGET_MENU 10
#define MK_WIDGET_TOOLBAR 11

// Text alignment
#define MK_ALIGN_LEFT 1
#define MK_ALIGN_CENTER 2
#define MK_ALIGN_RIGHT 3

// GUI Apps functions
void mk_gui_apps_init(void);
uint64_t mk_gui_apps_create_app(const char* name, const char* description, const char* executable_path, const char* icon_path, uint64_t category, uint64_t app_type, bool system_app);
int mk_gui_apps_launch(uint64_t app_id);
int mk_gui_apps_close(uint64_t app_id);
uint64_t mk_gui_apps_create_window(uint64_t app_id, const char* title, uint64_t window_type, uint64_t x, uint64_t y, uint64_t width, uint64_t height);
int mk_gui_apps_show_window(uint64_t window_id);
int mk_gui_apps_hide_window(uint64_t window_id);
int mk_gui_apps_close_window(uint64_t window_id);
int mk_gui_apps_focus_window(uint64_t window_id);
int mk_gui_apps_unfocus_window(uint64_t window_id);
uint64_t mk_gui_apps_create_widget(uint64_t window_id, const char* name, uint64_t widget_type, uint64_t x, uint64_t y, uint64_t width, uint64_t height);
void mk_gui_apps_render_all(void);
void mk_gui_apps_handle_click(uint64_t x, uint64_t y);
void mk_gui_apps_generate_report(void);

#endif // MK_GUI_APPS_H
