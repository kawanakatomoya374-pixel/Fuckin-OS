/**
 * theme_system.c - Theme System Implementation
 * C-OS 4.0.0 - ダークモード対応を含む動的な配色管理
 */

#include "theme_system.h"
#include "../include/memory.h"
#include "../include/string.h"
#include "../include/serial.h"

#define MAX_LISTENERS 16

static theme_t g_current_theme;
static theme_mode_t g_theme_mode = THEME_MODE_LIGHT;
static theme_change_callback_t g_listeners[MAX_LISTENERS];
static int g_listener_count = 0;
static bool g_initialized = false;

/* デフォルトライトテーマ */
static void theme_init_light_theme(theme_t* theme) {
    strcpy(theme->name, "Light");
    theme->mode = THEME_MODE_LIGHT;
    
    /* 基本色 */
    theme->primary_color = (color_t){0x2E, 0x7D, 0x32, 0xFF};      /* 緑 */
    theme->secondary_color = (color_t){0x19, 0x76, 0xD2, 0xFF};  /* 青 */
    theme->accent_color = (color_t){0xFF, 0x6F, 0x00, 0xFF};       /* オレンジ */
    
    /* UI要素の色 */
    theme->background_color = (color_t){0xFF, 0xFF, 0xFF, 0xFF};   /* 白 */
    theme->foreground_color = (color_t){0xF5, 0xF5, 0xF5, 0xFF};   /* 薄いグレー */
    theme->text_color = (color_t){0x21, 0x21, 0x21, 0xFF};         /* 濃いグレー */
    theme->border_color = (color_t){0xE0, 0xE0, 0xE0, 0xFF};       /* 薄いグレー */
    
    /* ハイライト色 */
    theme->highlight_color = (color_t){0xFF, 0xF9, 0xC4, 0xFF};    /* 黄 */
    theme->shadow_color = (color_t){0x00, 0x00, 0x00, 0x40};       /* 黒（半透明） */
    
    /* ステータス色 */
    theme->success_color = (color_t){0x4C, 0xAF, 0x50, 0xFF};      /* 緑 */
    theme->warning_color = (color_t){0xFF, 0x9C, 0x00, 0xFF};      /* オレンジ */
    theme->error_color = (color_t){0xF4, 0x43, 0x36, 0xFF};        /* 赤 */
    theme->info_color = (color_t){0x29, 0xB6, 0xF6, 0xFF};         /* 青 */
    
    /* フォント */
    strcpy(theme->font_name, "Ubuntu");
    theme->font_size = 12;
    
    /* その他 */
    theme->border_radius = 4;
    theme->shadow_blur = 2;
    theme->use_gradients = true;
}

/* デフォルトダークテーマ */
static void theme_init_dark_theme(theme_t* theme) {
    strcpy(theme->name, "Dark");
    theme->mode = THEME_MODE_DARK;
    
    /* 基本色 */
    theme->primary_color = (color_t){0x66, 0xBB, 0x6A, 0xFF};      /* 明るい緑 */
    theme->secondary_color = (color_t){0x42, 0xA5, 0xF5, 0xFF};    /* 明るい青 */
    theme->accent_color = (color_t){0xFF, 0xB7, 0x4D, 0xFF};       /* 明るいオレンジ */
    
    /* UI要素の色 */
    theme->background_color = (color_t){0x12, 0x12, 0x12, 0xFF};   /* 濃いグレー */
    theme->foreground_color = (color_t){0x1E, 0x1E, 0x1E, 0xFF};   /* 濃いグレー */
    theme->text_color = (color_t){0xE0, 0xE0, 0xE0, 0xFF};         /* 薄いグレー */
    theme->border_color = (color_t){0x42, 0x42, 0x42, 0xFF};       /* グレー */
    
    /* ハイライト色 */
    theme->highlight_color = (color_t){0xFF, 0xF5, 0x9D, 0xFF};    /* 明るい黄 */
    theme->shadow_color = (color_t){0x00, 0x00, 0x00, 0x80};       /* 黒（より濃い） */
    
    /* ステータス色 */
    theme->success_color = (color_t){0x81, 0xC7, 0x84, 0xFF};      /* 明るい緑 */
    theme->warning_color = (color_t){0xFF, 0xB7, 0x4D, 0xFF};      /* 明るいオレンジ */
    theme->error_color = (color_t){0xEF, 0x53, 0x50, 0xFF};        /* 明るい赤 */
    theme->info_color = (color_t){0x64, 0xB5, 0xF6, 0xFF};         /* 明るい青 */
    
    /* フォント */
    strcpy(theme->font_name, "Ubuntu");
    theme->font_size = 12;
    
    /* その他 */
    theme->border_radius = 4;
    theme->shadow_blur = 2;
    theme->use_gradients = true;
}

/* ============================================================
 * 初期化
 * ============================================================ */

int theme_system_init(void) {
    if (g_initialized) return 0;
    
    serial_puts("[THEME] Initializing theme system...\n");
    
    /* デフォルトテーマを初期化 */
    theme_init_light_theme(&g_current_theme);
    
    g_theme_mode = THEME_MODE_LIGHT;
    g_listener_count = 0;
    g_initialized = true;
    
    serial_puts("[THEME] Theme system initialized\n");
    return 0;
}

/* ============================================================
 * テーマ管理
 * ============================================================ */

int theme_set_mode(theme_mode_t mode) {
    if (!g_initialized) return -1;
    
    g_theme_mode = mode;
    
    if (mode == THEME_MODE_DARK) {
        theme_init_dark_theme(&g_current_theme);
    } else {
        theme_init_light_theme(&g_current_theme);
    }
    
    /* リスナーに通知 */
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i]) {
            g_listeners[i](&g_current_theme);
        }
    }
    
    return 0;
}

theme_mode_t theme_get_mode(void) {
    return g_theme_mode;
}

/* ============================================================
 * テーマの取得・設定
 * ============================================================ */

theme_t* theme_get_current(void) {
    if (!g_initialized) return NULL;
    return &g_current_theme;
}

int theme_set_theme(const char* theme_name) {
    if (!g_initialized || !theme_name) return -1;
    
    if (strcmp(theme_name, "Dark") == 0) {
        return theme_set_mode(THEME_MODE_DARK);
    } else if (strcmp(theme_name, "Light") == 0) {
        return theme_set_mode(THEME_MODE_LIGHT);
    }
    
    return -1;
}

int theme_load_theme(const char* path) {
    (void)path;
    /* TODO: ファイルからテーマを読み込む */
    return 0;
}

int theme_save_theme(const char* path) {
    (void)path;
    /* TODO: テーマをファイルに保存 */
    return 0;
}

/* ============================================================
 * カラー取得
 * ============================================================ */

color_t theme_get_primary_color(void) {
    return g_current_theme.primary_color;
}

color_t theme_get_secondary_color(void) {
    return g_current_theme.secondary_color;
}

color_t theme_get_accent_color(void) {
    return g_current_theme.accent_color;
}

color_t theme_get_background_color(void) {
    return g_current_theme.background_color;
}

color_t theme_get_foreground_color(void) {
    return g_current_theme.foreground_color;
}

color_t theme_get_text_color(void) {
    return g_current_theme.text_color;
}

color_t theme_get_border_color(void) {
    return g_current_theme.border_color;
}

/* ============================================================
 * ステータス色
 * ============================================================ */

color_t theme_get_success_color(void) {
    return g_current_theme.success_color;
}

color_t theme_get_warning_color(void) {
    return g_current_theme.warning_color;
}

color_t theme_get_error_color(void) {
    return g_current_theme.error_color;
}

color_t theme_get_info_color(void) {
    return g_current_theme.info_color;
}

/* ============================================================
 * テーマ変更リスナー
 * ============================================================ */

int theme_register_listener(theme_change_callback_t callback) {
    if (!g_initialized || !callback || g_listener_count >= MAX_LISTENERS) return -1;
    
    g_listeners[g_listener_count++] = callback;
    return 0;
}

int theme_unregister_listener(theme_change_callback_t callback) {
    if (!g_initialized || !callback) return -1;
    
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i] == callback) {
            g_listeners[i] = g_listeners[g_listener_count - 1];
            g_listener_count--;
            return 0;
        }
    }
    return -1;
}

uint64_t color_to_uint64(color_t c) {
    return ((uint64_t)c.r << 16) | ((uint64_t)c.g << 8) | (uint64_t)c.b;
}
