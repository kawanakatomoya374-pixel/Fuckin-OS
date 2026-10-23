/* extd - the extension daemon: runs the plugins of /system/plugins/system/ (see cos_plugin_system.h). No window; it sleeps between ticks. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_plugin.h"
#include "cos_plugin_system.h"

#define PL_DIR "/system/plugins/system"
#define NTICK 16
static struct { uint32_t every; uint64_t next; void (*fn)(void); int owner; } tick[NTICK];
static cos_plugins_t plugs;
static system_api_t api;

static int p_add_tick(uint32_t ms, void (*fn)(void)) {
    if (plugs.current < 0 || !fn || ms < 100) return -1;
    for (int i = 0; i < NTICK; i++) if (!tick[i].fn) { tick[i].every = ms; tick[i].next = 0; tick[i].fn = fn; tick[i].owner = plugs.current; return 0; }
    return -1;
}
static int p_sys_set(int key, int value) { return cos_settings_set(key, value); }
static int p_sys_get(int key) {
    cos_settings_t s; if (cos_settings_get(&s)) return -1;
    switch (key) {
        case SETP_THEME: return s.theme_idx; case SETP_FONT_SCALE: return s.font_scale; case SETP_WALLPAPER: return s.wallpaper_idx; case SETP_DARK_MODE: return s.dark_mode;
        case SETP_LANGUAGE: return s.japanese; case SETP_ICON_SIZE: return s.icon_size; case SETP_DESKTOP_COLOR: return s.desktop_color; case SETP_DESKTOP_COLOR2: return s.desktop_color2;
        default: return -1;
    }
}
static int p_screen(int *w, int *h) { cos_settings_t s; if (cos_settings_get(&s)) return -1; if (w) *w = s.screen_w; if (h) *h = s.screen_h; return 0; }
static void on_unload(cos_plugins_t *ps, int idx) { (void)ps; for (int i = 0; i < NTICK; i++) if (tick[i].fn && tick[i].owner == idx) tick[i].fn = NULL; }
static void on_change(cos_plugins_t *ps, int idx, int what) {
    char b[200];
    if (what > 0) snprintf(b, sizeof b, "[EXTD] plugin loaded: %s %s\n", ps->pl[idx].name, ps->pl[idx].version);
    else if (what < 0) snprintf(b, sizeof b, "[EXTD] plugin rejected: %s (%s)\n", ps->pl[idx].name, ps->pl[idx].why);
    else snprintf(b, sizeof b, "[EXTD] plugin unloaded\n");
    cos_puts(b);
}
int main(void) {
    cos_plugin_base_init(&api.base, NULL);
    api.abi = SYSP_ABI; api.size = sizeof api; api.sys_set = p_sys_set; api.sys_get = p_sys_get; api.add_tick = p_add_tick; api.screen = p_screen;
    plugs.app = "system"; plugs.dir = PL_DIR; plugs.api = &api; plugs.current = -1; plugs.on_unload = on_unload; plugs.on_change = on_change;
    cos_puts("[EXTD] started\n");
    cos_sleep_ms(4000);                                  /* let the desktop come up before the first effect is applied */
    for (;;) {
        cos_plugins_poll(&plugs, 1000);
        uint64_t now = cos_time_ms();
        for (int i = 0; i < NTICK; i++) if (tick[i].fn && now >= tick[i].next) { tick[i].next = now + tick[i].every; tick[i].fn(); }
        cos_sleep_ms(500);
    }
}
