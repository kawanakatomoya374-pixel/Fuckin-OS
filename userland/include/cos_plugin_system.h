/* cos_plugin_system.h - what a plugin of the extension daemon ("system") can use. The daemon (userland/programs/extd) starts at boot, has no window and
 * runs the plugins of /system/plugins/system/: effects that must hold at every start, whether or not Settings is open (a desktop colour, dark mode by the
 * time of day ...). A plugin library can serve Settings and the daemon at once (app "settings,system"): in Settings it adds items, in the daemon it reads the
 * same keys from its settings file (base.cfg_get) and applies them - host->app tells which of the two it is.
 */
#ifndef COS_PLUGIN_SYSTEM_H
#define COS_PLUGIN_SYSTEM_H
#include "cos_plugin_base.h"
#include "cos_plugin_settings.h"     /* the SETP_* keys of sys_set / sys_get */

#define SYSP_ABI 1u

typedef struct {
    cos_pbase_t base;
    uint32_t abi, size;
    int (*sys_set)(int key, int value);
    int (*sys_get)(int key);
    int (*add_tick)(uint32_t interval_ms, void (*fn)(void));       /* fn is called about every interval_ms (and once right away) */
    int (*screen)(int *w, int *h);
} system_api_t;
#endif
