/**
 * theme_system.h - Theme System
 * C-OS 5.0.0 - ダークモード対応を含む動的な配色管理
 */

#ifndef THEME_SYSTEM_H
#define THEME_SYSTEM_H

#include <stdint.h>
#include <stdbool.h>

/* カラー定義 */
typedef struct {
    uint8_t r, g, b, a;
} color_t;

/* テーマモード */
typedef enum {
    THEME_MODE_LIGHT = 0,
    THEME_MODE_DARK = 1,
    THEME_MODE_AUTO = 2,
} theme_mode_t;

/* テーマ構造体 */
typedef struct {
    char name[64];
    theme_mode_t mode;
    
    /* 基本色 */
    color_t primary_color;
    color_t secondary_color;
    color_t accent_color;
    
    /* UI要素の色 */
    color_t background_color;
    color_t foreground_color;
    color_t text_color;
    color_t border_color;
    
    /* ハイライト色 */
    color_t highlight_color;
    color_t shadow_color;
    
    /* ステータス色 */
    color_t success_color;
    color_t warning_color;
    color_t error_color;
    color_t info_color;
    
    /* フォント設定 */
    char font_name[64];
    uint32_t font_size;
    
    /* その他の設定 */
    uint32_t border_radius;
    uint32_t shadow_blur;
    bool use_gradients;
} theme_t;

/* 初期化 */
int theme_system_init(void);

/* テーマ管理 */
int theme_set_mode(theme_mode_t mode);

/* テーマの取得・設定 */

/* カラー取得 */

/* ステータス色 */

/* テーマ変更リスナー */
typedef void (*theme_change_callback_t)(theme_t* new_theme);

#endif /* THEME_SYSTEM_H */
