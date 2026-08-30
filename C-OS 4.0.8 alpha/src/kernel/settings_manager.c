/**
 * settings_manager.c - C-OS 4.0.8 alpha Settings Persistence Manager
 * Handles saving and loading OS settings to persistent storage.
 */
#include "settings_manager.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../include/serial.h"
#include "../include/string.h"
#include "../drivers/disk/storage.h"

/* Weak hooks so the module can still build in reduced configurations. */
extern bool storage_read_file(const char* filename, void* buffer, uint64_t buffer_size, uint64_t* out_size) __attribute__((weak));
extern bool storage_write_file(const char* filename, const void* data, uint64_t size) __attribute__((weak));

#define SETTINGS_MAGIC   0x434F5334u  /* 'COS4' */
#define SETTINGS_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t checksum;

    uint32_t current_wallpaper;
    uint32_t current_theme;
    bool dark_mode;

    bool terminal_autoscroll;
    uint32_t cursor_type;

    uint32_t network_mode;
    char hostname[64];

    uint32_t desktop_icon_size;
    bool show_hidden_files;
    uint32_t language;

    uint32_t default_window_x;
    uint32_t default_window_y;
    uint32_t default_window_w;
    uint32_t default_window_h;
    bool snap_to_grid;

    uint32_t system_volume;
    bool system_sounds_enabled;

    bool animations_enabled;
    uint32_t animation_speed;

    uint8_t reserved[128];
} __attribute__((packed)) settings_t;

static settings_t g_settings;
static bool g_settings_loaded = false;

static const char settings_config_filename[] = ".cos4settings";
static const char settings_default_hostname[] = "C-OS-4-0-5";
static const char settings_key_wallpaper_str[] = "wallpaper";
static const char settings_key_theme_str[] = "theme";
static const char settings_key_darkmode_str[] = "dark_mode";
static const char settings_key_autoscroll_str[] = "autoscroll";
static const char settings_key_volume_str[] = "volume";
static const char settings_key_animations_str[] = "animations";
static const char settings_key_hostname_str[] = "hostname";
static const char settings_dump_header_str[] = "[SETTINGS]\r\n";

static uint32_t settings_checksum(const settings_t* s) {
    uint32_t sum = 0;
    const uint8_t* data = (const uint8_t*)s;
    for (size_t i = 0; i < offsetof(settings_t, checksum); ++i) {
        sum = (sum << 5) - sum + data[i];
    }
    return sum;
}

static const char* settings_safe_key(const char* key) {
    return key ? key : "";
}

static bool settings_storage_available(void) {
    return storage_read_file && storage_write_file;
}

void settings_init_defaults(void) {
    memset(&g_settings, 0, sizeof(g_settings));
    g_settings.magic = SETTINGS_MAGIC;
    g_settings.version = SETTINGS_VERSION;

    g_settings.current_wallpaper = 0;
    g_settings.current_theme = 0;
    g_settings.dark_mode = false;

    g_settings.terminal_autoscroll = true;
    g_settings.cursor_type = 0;

    g_settings.network_mode = 0;
    cos_strlcpy(g_settings.hostname, settings_get_hostname(), sizeof(g_settings.hostname));

    g_settings.desktop_icon_size = 48;
    g_settings.show_hidden_files = false;
    g_settings.language = 0;

    g_settings.default_window_x = 100;
    g_settings.default_window_y = 100;
    g_settings.default_window_w = 640;
    g_settings.default_window_h = 480;
    g_settings.snap_to_grid = true;

    g_settings.system_volume = 75;
    g_settings.system_sounds_enabled = true;

    g_settings.animations_enabled = true;
    g_settings.animation_speed = 50;

    g_settings.checksum = settings_checksum(&g_settings);
}

bool settings_load(void) {
    if (storage_read_file) {
        uint64_t out_size = 0;
        if (storage_read_file(settings_get_config_filename(), &g_settings, sizeof(g_settings), &out_size) &&
            out_size == sizeof(g_settings) &&
            g_settings.magic == SETTINGS_MAGIC &&
            g_settings.version == SETTINGS_VERSION &&
            g_settings.checksum == settings_checksum(&g_settings)) {
            g_settings_loaded = true;
            return true;
        }
    }

    settings_init_defaults();
    g_settings_loaded = true;
    return false;
}

bool settings_save(void) {
    g_settings.checksum = settings_checksum(&g_settings);
    if (!storage_write_file) {
        return false;
    }
    return storage_write_file(settings_get_config_filename(), &g_settings, sizeof(g_settings));
}

uint32_t settings_get(const char* key) {
    if (!g_settings_loaded) {
        settings_load();
    }
    key = settings_safe_key(key);

    if (strcmp(key, settings_key_wallpaper()) == 0) return g_settings.current_wallpaper;
    if (strcmp(key, settings_key_theme()) == 0) return g_settings.current_theme;
    if (strcmp(key, settings_key_dark_mode()) == 0) return g_settings.dark_mode ? 1u : 0u;
    if (strcmp(key, settings_key_autoscroll()) == 0) return g_settings.terminal_autoscroll ? 1u : 0u;
    if (strcmp(key, settings_key_volume()) == 0) return g_settings.system_volume;
    if (strcmp(key, settings_key_animations()) == 0) return g_settings.animations_enabled ? 1u : 0u;
    return 0;
}

bool settings_set(const char* key, uint32_t value) {
    if (!g_settings_loaded) {
        settings_load();
    }
    key = settings_safe_key(key);

    if (strcmp(key, settings_key_wallpaper()) == 0) {
        g_settings.current_wallpaper = value;
    } else if (strcmp(key, settings_key_theme()) == 0) {
        g_settings.current_theme = value;
    } else if (strcmp(key, settings_key_dark_mode()) == 0) {
        g_settings.dark_mode = (value != 0);
    } else if (strcmp(key, settings_key_autoscroll()) == 0) {
        g_settings.terminal_autoscroll = (value != 0);
    } else if (strcmp(key, settings_key_volume()) == 0) {
        g_settings.system_volume = value;
    } else if (strcmp(key, settings_key_animations()) == 0) {
        g_settings.animations_enabled = (value != 0);
    } else {
        return false;
    }

    return settings_save();
}

const char* settings_get_string(const char* key, char* buffer, size_t buffer_size) {
    if (!g_settings_loaded) {
        settings_load();
    }
    if (!buffer || buffer_size == 0) {
        return NULL;
    }

    key = settings_safe_key(key);
    if (strcmp(key, settings_key_hostname()) == 0) {
        cos_strlcpy(buffer, g_settings.hostname, buffer_size);
        return buffer;
    }
    return NULL;
}

bool settings_set_string(const char* key, const char* value) {
    if (!g_settings_loaded) {
        settings_load();
    }
    key = settings_safe_key(key);
    value = value ? value : "";

    if (strcmp(key, settings_key_hostname()) == 0) {
        cos_strlcpy(g_settings.hostname, value, sizeof(g_settings.hostname));
        return settings_save();
    }
    return false;
}

void settings_reset(void) {
    settings_init_defaults();
    (void)settings_save();
}

void settings_dump(void) {
    if (!g_settings_loaded) {
        settings_load();
    }
    serial_puts(settings_dump_header());
    serial_puts("hostname=");
    serial_puts(g_settings.hostname);
    serial_puts("\r\n");
    serial_puts("volume=");
    serial_putdec(g_settings.system_volume);
    serial_puts("\r\n");
    serial_puts("animations=");
    serial_puts(g_settings.animations_enabled ? "1" : "0");
    serial_puts("\r\n");
}

__attribute__((weak)) const char* settings_get_config_filename(void) { return settings_config_filename; }
__attribute__((weak)) const char* settings_get_hostname(void) { return settings_default_hostname; }
__attribute__((weak)) const char* settings_key_wallpaper(void) { return settings_key_wallpaper_str; }
__attribute__((weak)) const char* settings_key_theme(void) { return settings_key_theme_str; }
__attribute__((weak)) const char* settings_key_dark_mode(void) { return settings_key_darkmode_str; }
__attribute__((weak)) const char* settings_key_autoscroll(void) { return settings_key_autoscroll_str; }
__attribute__((weak)) const char* settings_key_volume(void) { return settings_key_volume_str; }
__attribute__((weak)) const char* settings_key_animations(void) { return settings_key_animations_str; }
__attribute__((weak)) const char* settings_key_hostname(void) { return settings_key_hostname_str; }
__attribute__((weak)) const char* settings_dump_header(void) { return settings_dump_header_str; }

const char* settings_get_os_version(void) { return "C-OS 4.0.8 alpha"; }
const char* settings_get_os_name(void) { return "C-OS 4.0.8 alpha"; }
bool settings_is_network_enabled(void) { return g_settings.network_mode != 0; }
uint32_t settings_get_max_open_files(void) { return 64; }
uint32_t settings_get_max_processes(void) { return 128; }
bool settings_get_developer_mode(void) { return false; }
bool settings_set_developer_mode(bool enabled) { (void)enabled; return false; }
const char* settings_get_default_shell(void) { return "/bin/sh"; }
uint32_t settings_get_cursor_blink_rate(void) { return 500; }
bool settings_set_cursor_blink_rate(uint32_t rate) { (void)rate; return false; }
const char* settings_get_timezone(void) { return "UTC"; }
bool settings_set_timezone(const char* tz) { (void)tz; return false; }
uint32_t settings_get_auto_save_interval(void) { return 30; }
bool settings_set_auto_save_interval(uint32_t interval) { (void)interval; return false; }

bool storage_init_persistence(void) {
    serial_puts("[Settings] Initializing storage persistence...\r\n");
    return true;
}

bool storage_save_config(void) {
    return settings_save();
}

bool storage_load_config(void) {
    return settings_load();
}

void settings_export_all(void) {
    serial_puts("=== C-OS 4.0.8 alpha Settings Export ===\r\n");
    serial_puts("OS: C-OS 4.0.8 alpha\r\n");
    serial_puts("Wallpaper: "); serial_putdec(g_settings.current_wallpaper); serial_puts("\r\n");
    serial_puts("Theme: "); serial_putdec(g_settings.current_theme); serial_puts("\r\n");
    serial_puts("Dark Mode: "); serial_puts(g_settings.dark_mode ? "ON" : "OFF"); serial_puts("\r\n");
    serial_puts("Volume: "); serial_putdec(g_settings.system_volume); serial_puts("\r\n");
    serial_puts("Animations: "); serial_puts(g_settings.animations_enabled ? "ON" : "OFF"); serial_puts("\r\n");
    serial_puts("Auto-scroll: "); serial_puts(g_settings.terminal_autoscroll ? "ON" : "OFF"); serial_puts("\r\n");
    serial_puts("=================================\r\n");
}

bool settings_import_all(const char* data) {
    if (!data) {
        return false;
    }

    /* Minimal sanity check rather than pretending to parse arbitrary text. */
    if (strstr(data, "hostname=") != NULL) {
        const char* p = strstr(data, "hostname=");
        p += 9;
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(g_settings.hostname)) {
            len = sizeof(g_settings.hostname) - 1;
        }
        memcpy(g_settings.hostname, p, len);
        g_settings.hostname[len] = '\0';
        (void)settings_save();
        return true;
    }

    return false;
}

bool gui_is_context_menu_active(void) { return false; }
void gui_show_context_menu(int window_idx, int x, int y, bool is_dir) { (void)window_idx; (void)x; (void)y; (void)is_dir; }
void gui_close_context_menu(void) {}
void gui_get_context_menu_state(int* x, int* y, int* active, bool* is_dir, int* selected) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (active) *active = 0;
    if (is_dir) *is_dir = false;
    if (selected) *selected = -1;
}
bool gui_handle_context_menu_click(int mx, int my) { (void)mx; (void)my; return false; }
void gui_draw_context_menus(void) {}


// Desktop icon size setting
static int desktop_icon_size = 64;

int settings_get_desktop_icon_size(void)
{
    return desktop_icon_size;
}

void settings_set_desktop_icon_size(int size)
{
    if(size < 32) size = 32;
    if(size > 128) size = 128;

    desktop_icon_size = size;
}
