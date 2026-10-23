#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime/python_code_to_c_runtime.h"
#include "platform/python_code_to_c_gui.h"

static size_t backend_commands;
static size_t backend_text;
static void on_begin(uint32_t w, uint32_t h, void *u) { (void)u; if (w != 320 || h != 200) _Exit(10); }
static void on_command(const P2C_GuiCommand *c, const char *text, size_t len, void *u) {
    (void)u; if (!c) _Exit(11); backend_commands++; if (c->type == P2C_GUI_TEXT) { if (!text || len != 5) _Exit(12); backend_text++; }
}
static void on_end(void *u) { (void)u; }

static void gc_cycle_case(void) {
    P2C_Object *a = p2c_list_new();
    P2C_Object *b = p2c_list_new();
    p2c_list_append(a, b);
    p2c_list_append(b, a);
    p2c_obj_incref(a);
    p2c_obj_decref(a);
    p2c_gc_collect();
}

int main(void) {
    /* スタック境界の登録が p2c_gc_collect() の前提である（未登録なら回収は
     * 安全側に停止する）。ホストでは p2c_gc_init() が pthread から実境界を
     * 取得する。生成コードの main() も同じ順序を踏む。 */
    int stack_mark = 0;
    p2c_runtime_init(NULL, 0);
    p2c_gc_init(&stack_mark);
    if (!p2c_gc_stack_scan_available()) return 4;
    p2c_gc_set_threshold(1);
    P2C_Object *root = p2c_list_new();
    size_t before = p2c_gc_collections_run();
    p2c_gc_register_root(&root);
    p2c_gc_register_root(&root);
    if (p2c_gc_root_count() != 1 || p2c_gc_root_capacity() < 1) return 5;
    p2c_gc_collect();
    if (p2c_gc_collections_run() <= before || p2c_gc_is_collecting()) return 6;
    p2c_gc_unregister_root(&root);
    root = NULL;
    gc_cycle_case();

    P2C_GuiCommand commands[8];
    char text[32];
    P2C_GuiContext gui;
    p2c_gui_init(&gui, commands, 8, text, sizeof(text));
    p2c_gui_begin(&gui, 320, 200, (P2C_GuiColor){10, 20, 30, 255});
    p2c_gui_fill_rect(&gui, (P2C_GuiRect){1, 2, 30, 40}, (P2C_GuiColor){255, 0, 0, 255});
    p2c_gui_stroke_rect(&gui, (P2C_GuiRect){2, 3, 28, 38}, (P2C_GuiColor){0, 255, 0, 255});
    p2c_gui_line(&gui, 0, 0, 10, 10, (P2C_GuiColor){0, 0, 255, 255});
    p2c_gui_text(&gui, 4, 5, 14, (P2C_GuiColor){255, 255, 255, 255}, "hello");
    P2C_GuiBackend backend = {on_begin, on_command, on_end, NULL};
    p2c_gui_end(&gui, &backend, 320, 200);
    if (backend_commands != 5 || backend_text != 1 || gui.dropped_commands != 0) return 20;

    P2C_GuiCommand limited_commands[1];
    char limited_text[8];
    P2C_GuiContext limited;
    p2c_gui_init(&limited, limited_commands, 1, limited_text, sizeof(limited_text));
    p2c_gui_begin(&limited, 1, 1, (P2C_GuiColor){0, 0, 0, 255});
    p2c_gui_text(&limited, 0, 0, 1, (P2C_GuiColor){255, 255, 255, 255}, "x");
    if (limited.command_count != 1 || limited.text_used != 0 || limited.dropped_commands != 1) return 21;

    P2C_GuiCommand text_commands[3];
    char text_limited[3];
    P2C_GuiContext text_boundary;
    p2c_gui_init(&text_boundary, text_commands, 3, text_limited, sizeof(text_limited));
    p2c_gui_begin(&text_boundary, 1, 1, (P2C_GuiColor){0, 0, 0, 255});
    p2c_gui_text(&text_boundary, 0, 0, 1, (P2C_GuiColor){255, 255, 255, 255}, "abc");
    size_t copied_len = 0;
    const char *copied = p2c_gui_text_at(&text_boundary, &text_commands[1], &copied_len);
    if (text_boundary.command_count != 2 || text_boundary.text_used != 3 || copied_len != 3 || !copied || memcmp(copied, "abc", 3) != 0) return 22;
    p2c_gui_text(&text_boundary, 0, 0, 1, (P2C_GuiColor){255, 255, 255, 255}, "d");
    if (text_boundary.command_count != 2 || text_boundary.text_used != 3 || text_boundary.dropped_commands != 1) return 23;

    p2c_runtime_shutdown();
    puts("gc_gui_ok");
    return 0;
}
