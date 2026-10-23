/* snippets.c - plugin of C-OS Studio: insert a header guard, a license header with today's date, a for loop, a struct, a C-OS window program skeleton. */
#include "cos_plugin.h"
#include "cos_plugin_studio.h"
#include "cos_plugin_util.h"
static const studio_api_t *A;
static void put(const char *s) {
    if (!A->has_doc()) { A->base.status("Open a file first"); return; }
    size_t a = A->sel_start(), b = A->sel_end(); A->doc_replace(a, b - a, s, pu_len(s)); A->set_selection(a + pu_len(s), a + pu_len(s));
}
static void guard(void) {
    const char *p = A->doc_path(); const char *base = p; for (const char *q = p; *q; q++) if (*q == '/') base = q + 1;
    char name[64] = "FILE_H"; size_t o = 0;
    if (*base) { for (const char *q = base; *q && o < 56; q++) name[o++] = (*q >= 'a' && *q <= 'z') ? (char)(*q - 32) : ((*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9')) ? *q : '_'; name[o] = 0; }
    char t[300]; A->base.snprintf(t, sizeof t, "#ifndef %s\n#define %s\n\n\n\n#endif\n", name, name); put(t);
}
static void license(void) {
    int y = 2025, mo, d, h, mi, s; A->base.datetime(&y, &mo, &d, &h, &mi, &s);
    char name[48] = "Your Name"; A->base.cfg_get("snippets", "author", name, sizeof name);
    char t[300]; A->base.snprintf(t, sizeof t, "/*\n * Copyright (c) %d %s\n *\n * Permission to use, copy, modify, and distribute this software is hereby granted.\n */\n", y, name); put(t);
}
static void forloop(void) { put("for (int i = 0; i < n; ++i) {\n    \n}\n"); }
static void structure(void) { put("typedef struct {\n    int id;\n    char name[32];\n} Item;\n"); }
static void winapp(void) {
    put("#include \"cos.h\"\n#include \"cos_ui.h\"\n\nint main(void) {\n    cos_win_info_t wi;\n    int64_t h = cos_win2_create(\"My App\", 480, 320, &wi);\n    if (h <= 0) return 1;\n"
        "    cui_canvas c;\n    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);\n    cos_win_event_t ev;\n    for (;;) {\n        cui_fill(&c, 0, 0, wi.width, wi.height, 0x20242E);\n"
        "        cui_text(&c, cui_font_default(16), 20, 20, \"Hello, C-OS\", 0xFFFFFF);\n        cos_win2_present(h);\n        if (cos_win2_wait(h, &ev, 50) > 0 && ev.type == COS_EV_CLOSE) break;\n    }\n    cos_win2_close(h);\n    return 0;\n}\n");
}
static int init(const cos_plugin_host_t *h) {
    A = (const studio_api_t *)h->api;
    if (!A || A->abi != STUDIOP_ABI) return -1;
    if (A->add_command("Snippet: header guard", guard)) return -1;
    A->add_command("Snippet: license header", license);
    A->add_command("Snippet: for loop", forloop);
    A->add_command("Snippet: struct", structure);
    A->add_command("Snippet: C-OS window app", winapp);
    return 0;
}
static void fini(void) { A = 0; }
COS_PLUGIN("studio", "snippets", "1.0", init, fini);
