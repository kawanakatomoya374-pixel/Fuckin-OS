/**
 * settings_manager.h - C-OS 4.0.8 alpha Settings Manager Header
 */

#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Initialize settings with defaults */
void settings_init_defaults(void);

/* Load settings from persistent storage */
bool settings_load(void);

/* Save settings to persistent storage */
bool settings_save(void);

/* Get setting value by key */
uint32_t settings_get(const char* key);

/* Set setting value by key */
bool settings_set(const char* key, uint32_t value);

/* Get string setting */
const char* settings_get_string(const char* key, char* buffer, size_t buffer_size);

/* Set string setting */
bool settings_set_string(const char* key, const char* value);

/* Reset to factory defaults */
void settings_reset(void);

/* Debug: dump settings to serial */
void settings_dump(void);

/* Key names for settings */
const char* settings_get_config_filename(void);
const char* settings_get_hostname(void);
const char* settings_key_wallpaper(void);
const char* settings_key_theme(void);
const char* settings_key_dark_mode(void);
const char* settings_key_autoscroll(void);
const char* settings_key_volume(void);
const char* settings_key_animations(void);
const char* settings_key_hostname(void);
const char* settings_dump_header(void);

// C-OS 4.0.8 alpha New Settings & API
const char* settings_get_os_version(void);
const char* settings_get_os_name(void);
bool settings_is_network_enabled(void);
uint32_t settings_get_max_open_files(void);
uint32_t settings_get_max_processes(void);
bool settings_get_developer_mode(void);
bool settings_set_developer_mode(bool enabled);
const char* settings_get_default_shell(void);
uint32_t settings_get_cursor_blink_rate(void);
bool settings_set_cursor_blink_rate(uint32_t rate);
const char* settings_get_timezone(void);
bool settings_set_timezone(const char* tz);
uint32_t settings_get_auto_save_interval(void);
bool settings_set_auto_save_interval(uint32_t interval);

// Storage persistence
bool storage_init_persistence(void);
bool storage_save_config(void);
bool storage_load_config(void);
void settings_export_all(void);
bool settings_import_all(const char* data);

// Context menu API (in gui_apps_missing.c)
bool gui_is_context_menu_active(void);
void gui_show_context_menu(int window_idx, int x, int y, bool is_dir);
void gui_close_context_menu(void);
void gui_get_context_menu_state(int* x, int* y, int* active, bool* is_dir, int* selected);
bool gui_handle_context_menu_click(int mx, int my);
void gui_draw_context_menus(void);

int settings_get_desktop_icon_size(void);
void settings_set_desktop_icon_size(int size);

#endif /* SETTINGS_MANAGER_H */
