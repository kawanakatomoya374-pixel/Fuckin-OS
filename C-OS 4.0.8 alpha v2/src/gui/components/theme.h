/**
 * theme.h - Theme System
 * 
 * C-OS 4.0.8 alpha テーマシステム
 */

#ifndef THEME_H
#define THEME_H

#include <stdint.h>

typedef struct {
    uint64_t bg_color;
    uint64_t panel_color;
    uint64_t text_color;
    uint64_t accent_color;
    uint64_t border_color;
    char name[32];
} theme_t;

void theme_init(void);
void theme_set_dark(bool dark);
theme_t* theme_get_current(void);
uint64_t theme_get_color(const char* key);

#endif
