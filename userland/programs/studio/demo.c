/* demo.c - HOST-ONLY fixture used to reproduce cos_studio_gui_mockup.png.
 * Enabled by STUDIO_DEMO=1; not part of the .c-os build. It writes the sample
 * project the mockup shows, opens it, and puts the UI into the mockup's state
 * (build failed, tooltip open, caret at Ln 8 Col 31). */
#include "studio.h"

#define ROOT "/tmp/studio_demo/hello_studio"

static const char *HELLO_C =
    "#include <stdio.h>\n"
    "#include \"cos.h\"\n"
    "#include \"cos_ui.h\"\n"
    "\n"
    "// Hello, C-OS Studio\n"
    "int main(void) {\n"
    "    cos_win_info_t wi;\n"
    "    int64_t h = cos_win2_create(\"Hello\", 320, 200, &wi);\n"
    "    if (h <= 0) return 1;\n"
    "    cui_canvas c;\n"
    "    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);\n"
    "    cui_fill(&c, 0, 0, c.w, c.h, 0x1E2129);\n"
    "    cui_text(&c, cui_font_bold(20), 16, 16, \"Hello, C-OS!\", 0xFFFFFF)\n"
    "    cos_win2_present(h);\n"
    "    cos_win_event_t ev;\n"
    "    while (cos_win2_wait(h, &ev, 100) >= 0) {\n"
    "        if (ev.type == COS_EV_CLOSE) break;\n"
    "    }\n"
    "    cos_win2_close(h);\n"
    "    return 0;\n"
    "}\n"
    "\n"
    "// Redraw the window contents.\n"
    "static void draw_frame(cui_canvas *c, uint32_t bg, uint32_t fg) {\n"
    "    cui_fill(c, 0, 0, c->w, c->h, bg);\n"
    "    cui_text(c, cui_font_bold(20), 16, 16, \"Hello, C-OS!\", fg);\n"
    "}\n"
    "\n"
    "// Handle one window event; returns 0 to quit.\n"
    "static int on_event(const cos_win_event_t *ev) {\n"
    "    switch (ev->type) {\n"
    "    case COS_EV_CLOSE:\n"
    "        return 0;\n"
    "    case COS_EV_KEY:\n"
    "        if (ev->special == COS_KEY_ESC) return 0;\n"
    "        break;\n"
    "    default:\n"
    "        break;\n"
    "    }\n"
    "    return 1;\n"
    "}\n"
    "\n"
    "struct Theme {\n"
    "    uint32_t bg;\n"
    "    uint32_t fg;\n"
    "    uint32_t accent;\n"
    "};\n";

static void mk(const char *p) { cos_mkdir(p); }
static void wr(const char *rel, const char *text) { char p[300]; path_join(p, sizeof p, ROOT, rel); fs_write_all(p, text, strlen(text)); }

void app_demo_setup(void) {
    mk("/tmp/studio_demo"); mk(ROOT);
    mk(ROOT "/src"); mk(ROOT "/assets");
    wr("src/hello.c", HELLO_C);
    wr("src/ui.c", "#include \"util.h\"\n\nvoid ui_init(void) {\n}\n");
    wr("src/util.h", "#ifndef UTIL_H\n#define UTIL_H\nint clamp(int v, int lo, int hi);\n#endif\n");
    wr("assets/icon.raw", "x");
    wr("cosproj.txt", "name=hello_studio\ntarget=hello\ncflags=-O0\nsources=src\n");
    wr("README.txt", "hello_studio\n");

    app_open_path(ROOT);
    app_open_file(ROOT "/src/ui.c", 0);
    app_open_file(ROOT "/src/hello.c", 0);
    Doc *d = app_doc();
    doc_goto_line(d, 8, 31);
    d->scroll_y = 0;
    d->dirty = true;                                     /* the mockup's tab shows an unsaved/error dot */
    G.caret_t = cos_time_ms();

    /* the build output of the mockup, parsed exactly like a real build would be */
    app_output_clear();
    app_output_addf("$ tcc -I/system/sdk/include hello.c -o /tmp/hello.c-os\n");
    const char *e = "hello.c:14: error: ';' expected (got \"cos_win2_present\")";
    app_output_addf("%s\n", e);
    app_output_addf("!Build failed - 1 error, 0 warnings   (0.31 s)\n\n");
    app_output_addf("[last run]  hello.c-os  pid 14  exit code 0   (2.4 s)\n");
    Diag dg;
    if (diag_parse_line(e, &dg)) { G.diags[G.ndiag++] = dg; diag_refine(&G.diags[0]); }
    G.bld.st = B_FAILED; G.bld.nerr = 1;

    G.status_msg[0] = 0;
    G.tip_visible = true; G.tip_diag = 0; G.mx = 566; G.my = 322;
    G.demo_extra = true; G.demo_line = 9; G.demo_c0 = 8; G.demo_c1 = 21;
}
