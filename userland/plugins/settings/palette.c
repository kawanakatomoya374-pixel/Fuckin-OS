/* palette.c - plugin of Settings: colour schemes in one click. Each sets the desktop colours (a gradient) and dark mode together. */
#include "cos_plugin.h"
#include "cos_plugin_settings.h"
static const settings_api_t *S;
static const struct { const char *name; uint32_t top, bot; int dark; } P[] = {
    { "Midnight", 0x0B1020, 0x1B2A4A, 1 }, { "Forest", 0x10261A, 0x2D5A3D, 1 }, { "Plum", 0x241028, 0x5A2D4A, 1 },
    { "Amber", 0x3A2410, 0x8A5A22, 1 }, { "Slate", 0x2A2F38, 0x4A5260, 1 }, { "Daylight", 0xCFE3F7, 0xF2F6FA, 0 },
};
static void pick(int idx) {
    if (!S) return;
    S->sys_set(SETP_DESKTOP_COLOR, (int)(0x01000000 | P[idx].top)); S->sys_set(SETP_DESKTOP_COLOR2, (int)(0x01000000 | P[idx].bot)); S->sys_set(SETP_DARK_MODE, P[idx].dark);
    /* keep the Desktop colour plugin's stored values in step, so the daemon does not undo it */
    S->base.cfg_set("desktopcolor", "enabled", "1"); S->base.cfg_set("desktopcolor", "gradient", "1");
    char v[16]; S->base.snprintf(v, sizeof v, "%d", (int)P[idx].top); S->base.cfg_set("desktopcolor", "top", v);
    S->base.snprintf(v, sizeof v, "%d", (int)P[idx].bot); S->base.cfg_set("desktopcolor", "bottom", v);
    S->base.status(P[idx].name);
}
#define BTN(n) static void b##n(void) { pick(n); }
BTN(0) BTN(1) BTN(2) BTN(3) BTN(4) BTN(5)
static int init(const cos_plugin_host_t *h) {
    S = (const settings_api_t *)h->api; if (!S || S->abi != SETP_ABI) return -1;
    S->add_label("Pick a scheme: it sets the desktop gradient and dark / light mode at once.");
    S->add_button(P[0].name, b0); S->add_button(P[1].name, b1); S->add_button(P[2].name, b2);
    S->add_button(P[3].name, b3); S->add_button(P[4].name, b4); S->add_button(P[5].name, b5);
    return 0;
}
static void fini(void) { S = 0; }
COS_PLUGIN("settings", "palette", "1.0", init, fini);
