/* widgets.c-os - demo and regression target for the cos_widget toolkit (userland/lib/cos_widget.c).
 * Every control here is a retained widget: the program only builds the tree and reacts to callbacks; repainting,
 * hover/press/focus and the rectangle-only present are the toolkit's job. */
#include <stdio.h>
#include <string.h>
#include "cos.h"
#include "cos_widget.h"

static cw_app *g_app;
static cw_widget *g_name, *g_greeting, *g_volume_val, *g_list, *g_check, *g_slider;

static void report(const char *why) {
    uint32_t f, r; uint64_t px;
    cw_app_stats(g_app, &f, &r, &px);
    char b[160];
    snprintf(b, sizeof b, "[WIDGETS] %s frames=%u rects=%u pixels=%llu\n", why, f, r, (unsigned long long)px);
    cos_puts(b);
}
static void logf2(const char *k, int v) { char b[80]; snprintf(b, sizeof b, "[WIDGETS] %s=%d\n", k, v); cos_puts(b); }
static void on_greet(cw_widget *w, void *u) { (void)w; (void)u; char b[96]; snprintf(b, sizeof b, "Hello, %s!", cw_textbox_get(g_name)); cw_label_set_text(g_greeting, b); cos_puts("[WIDGETS] greet "); cos_puts(cw_textbox_get(g_name)); cos_puts("\n"); }
static void on_enter(cw_widget *w, const char *t, void *u) { (void)w; (void)t; on_greet(NULL, u); }
static void on_check(cw_widget *w, int v, void *u) { (void)w; (void)u; logf2("check", v); cw_widget_set_enabled(g_slider, v != 0); }
static void on_slider(cw_widget *w, int v, void *u) { (void)w; (void)u; logf2("slider", v); char b[16]; snprintf(b, sizeof b, "%d%%", v); cw_label_set_text(g_volume_val, b); }
static void on_select(cw_widget *w, int idx, void *u) { (void)w; (void)u; logf2("select", idx); char b[64]; snprintf(b, sizeof b, "Selected item %d", idx + 1); cw_label_set_text(g_greeting, b); }
static void on_stats(cw_widget *w, void *u) { (void)w; (void)u; report("stats"); }
static void on_quit(cw_widget *w, void *u) { (void)w; cw_app_quit((cw_app *)u); }

int main(void) {
    g_app = cw_app_create("Widgets", 520, 440);
    if (!g_app) return 1;
    cw_widget *root = cw_app_root(g_app);
    cw_panel_set_layout(root, CW_VBOX, 16, 10);

    cw_label_create(root, "cos_widget demo", 22, CW_TEXT);

    cw_widget *row = cw_panel_create(root, 0xFFFFFFFFu);
    row->h = 34;
    cw_panel_set_layout(row, CW_HBOX, 0, 8);
    cw_label_create(row, "Name", 14, CW_MUTED);
    g_name = cw_textbox_create(row, "", 32, NULL, on_enter, NULL);
    cw_widget_set_weight(g_name, 1);
    cw_button_create(row, "Greet", on_greet, NULL);

    g_greeting = cw_label_create(root, "Type a name and press Greet", 15, CW_ACCENT_LO);
    g_greeting->h = 24;
    g_check = cw_checkbox_create(root, "Enable the volume slider", true, on_check, NULL);

    cw_widget *srow = cw_panel_create(root, 0xFFFFFFFFu);
    srow->h = 28;
    cw_panel_set_layout(srow, CW_HBOX, 0, 8);
    cw_label_create(srow, "Volume", 14, CW_MUTED)->w = 56;
    g_slider = cw_slider_create(srow, 0, 100, 40, on_slider, NULL);
    cw_widget_set_weight(g_slider, 1);
    g_volume_val = cw_label_create(srow, "40%", 14, CW_TEXT);
    g_volume_val->w = 44;

    g_list = cw_list_create(root, on_select, NULL);
    cw_widget_set_weight(g_list, 1);
    for (int i = 1; i <= 24; ++i) { char b[48]; snprintf(b, sizeof b, "List item %d", i); cw_list_add(g_list, b); }

    cw_widget *brow = cw_panel_create(root, 0xFFFFFFFFu);
    brow->h = 34;
    cw_panel_set_layout(brow, CW_HBOX, 0, 8);
    cw_button_create(brow, "Stats", on_stats, NULL);
    cw_widget *sp = cw_panel_create(brow, 0xFFFFFFFFu);
    cw_widget_set_weight(sp, 1);
    cw_button_create(brow, "Quit", on_quit, g_app);

    cw_app_layout(g_app);
    cos_puts("[WIDGETS] ready\n");
    int rc = cw_app_run(g_app);
    report("exit");
    return rc;
}
