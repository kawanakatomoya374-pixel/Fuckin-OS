/* settings.c - /etc/studio.conf : one "key=value" per line. */
#include "studio.h"

#define CONF_PATH "/etc/studio.conf"

void app_settings_load(void) {
    size_t n = 0;
    char *t = fs_read_all(CONF_PATH, &n);
    if (!t) return;
    for (char *line = strtok(t, "\n"); line; line = strtok(NULL, "\n")) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        int v = atoi(eq + 1);
        if (!strcmp(line, "tab_width") && v >= 1 && v <= 16) G.tab_width = v;
        else if (!strcmp(line, "code_px") && v >= 10 && v <= 20) G.code_px = v;
        else if (!strcmp(line, "theme")) app_apply_theme(strncmp(eq + 1, "light", 5) != 0);
        else if (!strcmp(line, "save_before_run")) G.save_before_run = v != 0;
        else if (!strcmp(line, "autosave")) G.autosave = v != 0;
        else if (!strcmp(line, "sidebar_w") && v >= SIDE_W_MIN && v <= SIDE_W_MAX) G.side_w = v;
        else if (!strcmp(line, "panel_h") && v >= PANEL_H_MIN && v <= PANEL_H_MAX) G.panel_h = v;
    }
    free(t);
}

void app_settings_save(void) {
    char b[256];
    int n = snprintf(b, sizeof b,
        "tab_width=%d\ncode_px=%d\ntheme=%s\nsave_before_run=%d\nautosave=%d\nsidebar_w=%d\npanel_h=%d\n",
        G.tab_width, G.code_px, G.dark ? "dark" : "light", G.save_before_run ? 1 : 0, G.autosave ? 1 : 0, G.side_w, G.panel_h);
    fs_write_all(CONF_PATH, b, (size_t)n);
}
