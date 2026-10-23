/* cos_plugin_settings.h - what a plugin of the Settings app ("settings") can use: items on the "Extensions" tab, and the system's own settings.
 *
 * Every item has a key; its value is stored in the plugin's settings file (/system/plugins/config/<plugin>.cfg), so it survives restarts and the plugin can
 * read it back. When the plugin registers an item the callback is called once with the stored (or default) value, so what the item controls is applied at
 * once; it is called again whenever the user changes the item.
 *
 * A plugin that wants its effect at every boot, not only while Settings is open, can also be installed for the "system" program (app "settings,system"):
 * the same library then runs in the extension daemon, reads the same keys with base.cfg_get and applies them (see cos_plugin_system.h).
 */
#ifndef COS_PLUGIN_SETTINGS_H
#define COS_PLUGIN_SETTINGS_H
#include "cos_plugin_base.h"

#define SETP_ABI 1u

/* keys for sys_set / sys_get: the COS_SET_* numbers of cos.h */
enum { SETP_THEME = 1, SETP_FONT_SCALE, SETP_WALLPAPER, SETP_DARK_MODE, SETP_LANGUAGE, SETP_ICON_SIZE,
       SETP_DESKTOP_COLOR = 27, SETP_DESKTOP_COLOR2 = 28 };      /* 0x01RRGGBB: a plain colour / gradient bottom instead of the wallpaper; 0 = the wallpaper */

typedef struct {
    cos_pbase_t base;
    uint32_t abi, size;
    int (*add_label)(const char *text);
    int (*add_toggle)(const char *key, const char *label, int def, void (*cb)(const char *key, int value));
    int (*add_slider)(const char *key, const char *label, int min, int max, int step, int def, void (*cb)(const char *key, int value));
    int (*add_choice)(const char *key, const char *label, const char *options /* "One|Two|Three" */, int def, void (*cb)(const char *key, int index));
    int (*add_color)(const char *key, const char *label, uint32_t def_rgb, void (*cb)(const char *key, uint32_t rgb));
    int (*add_text)(const char *key, const char *label, const char *def, void (*cb)(const char *key, const char *text));
    int (*add_button)(const char *label, void (*fn)(void));
    int (*sys_set)(int key, int value);                 /* one of the settings of the system (needs the Settings capability, which this program has) */
    int (*sys_get)(int key);                            /* current value; -1 for a key that cannot be read */
} settings_api_t;
#endif
