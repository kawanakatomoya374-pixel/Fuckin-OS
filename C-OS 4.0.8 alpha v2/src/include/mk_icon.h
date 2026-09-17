/**
 * mk_icon.h - Advanced Icon System Header
 * C-OS 4.0.8 alpha Advanced Icon Manager
 */
#ifndef MK_ICON_H
#define MK_ICON_H

#include "types.h"

// Icon constants
#define MK_MAX_ICONS 512
#define MK_MAX_ICON_CATEGORIES 32
#define MK_MAX_ICON_THEMES 16
#define MK_MAX_ICON_SIZES 8

// Icon formats
#define MK_ICON_FORMAT_PNG 1
#define MK_ICON_FORMAT_JPG 2
#define MK_ICON_FORMAT_BMP 3
#define MK_ICON_FORMAT_SVG 4

// Cache priorities
#define MK_CACHE_PRIORITY_LOW 1
#define MK_CACHE_PRIORITY_NORMAL 2
#define MK_CACHE_PRIORITY_HIGH 3
#define MK_CACHE_PRIORITY_CRITICAL 4

// Color schemes
#define MK_COLOR_SCHEME_DEFAULT 1
#define MK_COLOR_SCHEME_DARK 2
#define MK_COLOR_SCHEME_LIGHT 3

// Icon functions
void mk_icon_init(void);
uint64_t mk_icon_create(const char* name, const char* file_path, uint64_t category, uint64_t width, uint64_t height, uint64_t format);
int mk_icon_load(uint64_t icon_id);
int mk_icon_unload(uint64_t icon_id);
int mk_icon_render(uint64_t icon_id, uint64_t x, uint64_t y, uint64_t size, uint64_t transparency);
int mk_icon_resize(uint64_t icon_id, uint64_t new_width, uint64_t new_height);
uint64_t mk_icon_create_category(const char* name, const char* description, uint64_t default_size, bool system_category);
uint64_t mk_icon_create_theme(const char* name, const char* description, const char* icon_path, bool system_theme);
int mk_icon_set_theme(uint64_t theme_id);
void mk_icon_cache_cleanup(void);
void mk_icon_update_animations(void);
void mk_icon_generate_report(void);

#endif // MK_ICON_H
