/**
 * mk_desktop.h - Advanced Desktop System Header
 * C-OS 4.0.8 alpha Advanced Desktop Environment
 */
#ifndef MK_DESKTOP_H
#define MK_DESKTOP_H

#include "types.h"

// Desktop constants
#define MK_MAX_DESKTOP_ICONS 128
#define MK_MAX_DESKTOP_WIDGETS 64
#define MK_MAX_DESKTOP_WALLPAPERS 16
#define MK_MAX_DESKTOP_THEMES 32

// Icon categories
#define MK_ICON_CATEGORY_SYSTEM 1
#define MK_ICON_CATEGORY_OFFICE 2
#define MK_ICON_CATEGORY_INTERNET 3
#define MK_ICON_CATEGORY_MULTIMEDIA 4
#define MK_ICON_CATEGORY_ENTERTAINMENT 5
#define MK_ICON_CATEGORY_DEVELOPMENT 6
#define MK_ICON_CATEGORY_GRAPHICS 7
#define MK_ICON_CATEGORY_UTILITIES 8
#define MK_ICON_CATEGORY_DOCUMENTS 9
#define MK_ICON_CATEGORY_FOLDERS 10

// Wallpaper formats
#define MK_WALLPAPER_FORMAT_RGB 1
#define MK_WALLPAPER_FORMAT_RGBA 2
#define MK_WALLPAPER_FORMAT_JPEG 3
#define MK_WALLPAPER_FORMAT_PNG 4

// Desktop functions
int mk_desktop_init(void);
uint64_t mk_desktop_create_icon(const char* name, const char* executable_path, const char* icon_path, uint64_t x, uint64_t y, uint64_t category);
int mk_desktop_launch_icon(uint64_t icon_id);
int mk_desktop_move_icon(uint64_t icon_id, uint64_t x, uint64_t y);
int mk_desktop_resize_icon(uint64_t icon_id, uint64_t width, uint64_t height);
int mk_desktop_set_icon_visibility(uint64_t icon_id, bool visible);
uint64_t mk_desktop_create_widget(const char* name, const char* type, uint64_t x, uint64_t y, uint64_t width, uint64_t height);
int mk_desktop_update_widget(uint64_t widget_id);
uint64_t mk_desktop_create_wallpaper(const char* name, const char* file_path, uint64_t width, uint64_t height, bool tiled, bool stretched, bool centered);
int mk_desktop_set_wallpaper(uint64_t wallpaper_id);
uint64_t mk_desktop_create_theme(const char* name, const char* description, uint64_t background_color, uint64_t foreground_color, uint64_t accent_color);
int mk_desktop_set_theme(uint64_t theme_id);
int mk_desktop_render(void);
int mk_desktop_update(void);
int mk_desktop_handle_click(uint64_t x, uint64_t y);
void mk_desktop_report(void);

#endif // MK_DESKTOP_H
