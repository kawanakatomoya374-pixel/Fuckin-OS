/* autodark.c - plugin of Settings and of the extension daemon: dark mode follows the time of day (dark from one hour, light from another). */
#include "cos_plugin.h"
#include "cos_plugin_settings.h"
#include "cos_plugin_system.h"
static const cos_pbase_t *B;
static int (*set)(int, int);
static int (*get)(int);
static void apply(void) {
    if (!pcfg_int(B, "autodark", "enabled", 0)) return;
    int y, mo, d, h, mi, s; if (B->datetime(&y, &mo, &d, &h, &mi, &s)) return;
    int dark_from = pcfg_int(B, "autodark", "dark_from", 19), light_from = pcfg_int(B, "autodark", "light_from", 7);
    int dark = dark_from > light_from ? (h >= dark_from || h < light_from) : (h >= dark_from && h < light_from);
    if (get(SETP_DARK_MODE) != dark) { set(SETP_DARK_MODE, dark); B->log(dark ? "[AUTODARK] dark\n" : "[AUTODARK] light\n"); }
}
static void on_int(const char *k, int v) { (void)k; (void)v; apply(); }
static int init(const cos_plugin_host_t *h) {
    if (!h->api) return -1;
    if (h->app[0] == 's' && h->app[1] == 'y') {
        const system_api_t *A = (const system_api_t *)h->api; if (A->abi != SYSP_ABI) return -1;
        B = &A->base; set = A->sys_set; get = A->sys_get; return A->add_tick(30000, apply);
    }
    const settings_api_t *S = (const settings_api_t *)h->api; if (S->abi != SETP_ABI) return -1;
    B = &S->base; set = S->sys_set; get = S->sys_get;
    S->add_label("Switches between dark and light mode by the clock (checked every 30 s by the extension daemon).");
    S->add_toggle("enabled", "Dark mode follows the time of day", 0, on_int);
    S->add_slider("dark_from", "Dark from (hour)", 0, 23, 1, 19, on_int);
    S->add_slider("light_from", "Light from (hour)", 0, 23, 1, 7, on_int);
    return 0;
}
static void fini(void) { set = 0; get = 0; B = 0; }
COS_PLUGIN("settings,system", "autodark", "1.0", init, fini);
