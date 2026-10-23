/* layout.c - plugin of Settings: layout presets for the desktop. Icon size (Small / Medium / Large), font scale, and one click "Compact" / "Comfortable" / "Large print". */
#include "cos_plugin.h"
#include "cos_plugin_settings.h"
static const settings_api_t *S;
static void icons(const char *k, int idx) { (void)k; static const int px[3] = { 48, 64, 80 }; if (S && idx >= 0 && idx < 3) S->sys_set(SETP_ICON_SIZE, px[idx]); }
static void scale(const char *k, int v) { (void)k; if (S) S->sys_set(SETP_FONT_SCALE, v); }
static void compact(void) { S->sys_set(SETP_ICON_SIZE, 48); S->sys_set(SETP_FONT_SCALE, 1); S->base.status("Compact layout"); }
static void comfortable(void) { S->sys_set(SETP_ICON_SIZE, 64); S->sys_set(SETP_FONT_SCALE, 2); S->base.status("Comfortable layout"); }
static void large(void) { S->sys_set(SETP_ICON_SIZE, 80); S->sys_set(SETP_FONT_SCALE, 3); S->base.status("Large print layout"); }
static int init(const cos_plugin_host_t *h) {
    S = (const settings_api_t *)h->api; if (!S || S->abi != SETP_ABI) return -1;
    S->add_label("Sizes of the desktop. (The icon size is the system's own setting; this page only changes it.)");
    S->add_choice("icons", "Desktop icons", "Small|Medium|Large", S->sys_get(SETP_ICON_SIZE) >= 80 ? 2 : S->sys_get(SETP_ICON_SIZE) >= 64 ? 1 : 0, icons);
    S->add_slider("scale", "Font scale", 1, 4, 1, S->sys_get(SETP_FONT_SCALE) > 0 ? S->sys_get(SETP_FONT_SCALE) : 1, scale);
    S->add_button("Compact", compact); S->add_button("Comfortable", comfortable); S->add_button("Large print", large);
    return 0;
}
static void fini(void) { S = 0; }
COS_PLUGIN("settings", "layout", "1.0", init, fini);
