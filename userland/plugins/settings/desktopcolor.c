/* desktopcolor.c - plugin of Settings and of the extension daemon: a plain colour (or a gradient) for the desktop background instead of the wallpaper.
 * Settings > Extensions: switch, two colours, gradient on/off. The same library runs in the daemon and applies the stored values at every start. */
#include "cos_plugin.h"
#include "cos_plugin_settings.h"
#include "cos_plugin_system.h"
static const cos_pbase_t *B;
static int (*set)(int, int);
static int last_en = -2, last_top, last_bot, last_grad;

static void apply(void) {
    int en = pcfg_int(B, "desktopcolor", "enabled", 0), top = pcfg_int(B, "desktopcolor", "top", 0x1E3A5F) & 0xFFFFFF, bot = pcfg_int(B, "desktopcolor", "bottom", 0x0F2A44) & 0xFFFFFF;
    int grad = pcfg_int(B, "desktopcolor", "gradient", 1);
    if (en == last_en && top == last_top && bot == last_bot && grad == last_grad) return;
    last_en = en; last_top = top; last_bot = bot; last_grad = grad;
    { char m[100]; B->snprintf(m, sizeof m, "[DESKTOPCOLOR] enabled=%d top=%06X bottom=%06X gradient=%d\n", en, top, bot, grad); B->log(m); }
    if (en) { set(SETP_DESKTOP_COLOR, 0x01000000 | top); set(SETP_DESKTOP_COLOR2, grad ? (0x01000000 | bot) : 0); }
    else { set(SETP_DESKTOP_COLOR, 0); set(SETP_DESKTOP_COLOR2, 0); }
}
static void on_int(const char *k, int v) { (void)k; (void)v; last_en = -2; apply(); }
static void on_rgb(const char *k, uint32_t v) { (void)k; (void)v; last_en = -2; apply(); }

static int init(const cos_plugin_host_t *h) {
    if (!h->api) return -1;
    const char *app = h->app;
    if (app[0] == 's' && app[1] == 'y') {                                   /* the daemon */
        const system_api_t *A = (const system_api_t *)h->api; if (A->abi != SYSP_ABI) return -1;
        B = &A->base; set = A->sys_set;
        return A->add_tick(2000, apply);                                   /* follows changes of the stored values, applies them at the start */
    }
    const settings_api_t *S = (const settings_api_t *)h->api; if (S->abi != SETP_ABI) return -1;
    B = &S->base; set = S->sys_set;
    S->add_label("A plain colour (or a gradient) in place of the wallpaper.");
    S->add_toggle("enabled", "Use a colour instead of the wallpaper", 0, on_int);
    S->add_color("top", "Colour (top)", 0x1E3A5F, on_rgb);
    S->add_toggle("gradient", "Gradient to a second colour", 1, on_int);
    S->add_color("bottom", "Colour (bottom)", 0x0F2A44, on_rgb);
    return 0;
}
static void fini(void) {
    /* switched off: give the wallpaper back (the daemon also does this when the file is removed) */
    if (set) { set(SETP_DESKTOP_COLOR, 0); set(SETP_DESKTOP_COLOR2, 0); }
    set = 0; B = 0; last_en = -2;
}
COS_PLUGIN("settings,system", "desktopcolor", "1.0", init, fini);
