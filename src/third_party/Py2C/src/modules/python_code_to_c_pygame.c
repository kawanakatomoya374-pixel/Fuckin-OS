/*
 * python_code_to_c_pygame.c - ヘッドレスpygame互換モジュール
 * 詳細はinclude/python_code_to_c_pygame.hのコメントを参照。
 */
#include "modules/python_code_to_c_pygame.h"
#include "runtime/python_code_to_c_runtime.h"
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#endif

/* ============================================================
 * Rect クラス: pygame.Rect(x, y, width, height)
 * ============================================================ */
static P2C_Object *g_rect_class = NULL;
static P2C_Object* rect_classobj(void);

static void rect_init_self(P2C_Object *self, P2C_Object **args, size_t nargs) {
    P2C_Object *x = (nargs > 0) ? args[0] : p2c_obj_from_int(0);
    P2C_Object *y = (nargs > 1) ? args[1] : p2c_obj_from_int(0);
    P2C_Object *w = (nargs > 2) ? args[2] : p2c_obj_from_int(0);
    P2C_Object *h = (nargs > 3) ? args[3] : p2c_obj_from_int(0);
    p2c_setattr(self, "x", x);
    p2c_setattr(self, "y", y);
    p2c_setattr(self, "width", w);
    p2c_setattr(self, "height", h);
    p2c_setattr(self, "w", w);
    p2c_setattr(self, "h", h);
}

static P2C_Object* rect_ctor(P2C_Object **args, size_t nargs) {
    P2C_Object *self = p2c_instance_new(rect_classobj());
    rect_init_self(self, args, nargs);
    return self;
}

static P2C_Object* rect_init_method(P2C_Object *self, P2C_Object **args, size_t nargs) {
    rect_init_self(self, args, nargs);
    return &P2C_None;
}

static P2C_Object* rect_move(P2C_Object *self, P2C_Object **args, size_t nargs) {
    int64_t dx = (nargs > 0) ? p2c_obj_as_int(args[0]) : 0;
    int64_t dy = (nargs > 1) ? p2c_obj_as_int(args[1]) : 0;
    int64_t x = p2c_obj_as_int(p2c_getattr(self, "x"));
    int64_t y = p2c_obj_as_int(p2c_getattr(self, "y"));
    P2C_Object *new_args[4];
    new_args[0] = p2c_obj_from_int(x + dx);
    new_args[1] = p2c_obj_from_int(y + dy);
    new_args[2] = p2c_getattr(self, "width");
    new_args[3] = p2c_getattr(self, "height");
    return rect_ctor(new_args, 4);
}

static P2C_Object* rect_colliderect(P2C_Object *self, P2C_Object **args, size_t nargs) {
    if (nargs < 1) return p2c_obj_from_bool(false);
    P2C_Object *other = args[0];
    int64_t ax = p2c_obj_as_int(p2c_getattr(self, "x"));
    int64_t ay = p2c_obj_as_int(p2c_getattr(self, "y"));
    int64_t aw = p2c_obj_as_int(p2c_getattr(self, "width"));
    int64_t ah = p2c_obj_as_int(p2c_getattr(self, "height"));
    int64_t bx = p2c_obj_as_int(p2c_getattr(other, "x"));
    int64_t by = p2c_obj_as_int(p2c_getattr(other, "y"));
    int64_t bw = p2c_obj_as_int(p2c_getattr(other, "width"));
    int64_t bh = p2c_obj_as_int(p2c_getattr(other, "height"));
    bool overlap = (ax < bx + bw) && (ax + aw > bx) && (ay < by + bh) && (ay + ah > by);
    return p2c_obj_from_bool(overlap);
}

static P2C_Object* rect_contains(P2C_Object *self, P2C_Object **args, size_t nargs) {
    if (nargs < 1) return p2c_obj_from_bool(false);
    P2C_Object *other = args[0];
    int64_t ax = p2c_obj_as_int(p2c_getattr(self, "x"));
    int64_t ay = p2c_obj_as_int(p2c_getattr(self, "y"));
    int64_t aw = p2c_obj_as_int(p2c_getattr(self, "width"));
    int64_t ah = p2c_obj_as_int(p2c_getattr(self, "height"));
    int64_t bx = p2c_obj_as_int(p2c_getattr(other, "x"));
    int64_t by = p2c_obj_as_int(p2c_getattr(other, "y"));
    int64_t bw = p2c_obj_as_int(p2c_getattr(other, "width"));
    int64_t bh = p2c_obj_as_int(p2c_getattr(other, "height"));
    bool inside = (bx >= ax) && (by >= ay) && (bx + bw <= ax + aw) && (by + bh <= ay + ah);
    return p2c_obj_from_bool(inside);
}

static P2C_MethodDef g_rect_methods[] = {
    {"__init__", rect_init_method, NULL, P2C_METHOD_INSTANCE},
    {"move", rect_move, NULL, P2C_METHOD_INSTANCE},
    {"colliderect", rect_colliderect, NULL, P2C_METHOD_INSTANCE},
    {"contains", rect_contains, NULL, P2C_METHOD_INSTANCE},
    {NULL, NULL, NULL, P2C_METHOD_INSTANCE}
};

static P2C_Object* rect_classobj(void) {
    if (!g_rect_class) g_rect_class = p2c_class_new("Rect", rect_ctor, g_rect_methods, NULL);
    return g_rect_class;
}

/* ============================================================
 * Surface クラス: pygame.Surface((width, height))
 * ============================================================ */
static P2C_Object *g_surface_class = NULL;
static P2C_Object* surface_classobj(void);

static void surface_init_self(P2C_Object *self, P2C_Object **args, size_t nargs) {
    P2C_Object *w = p2c_obj_from_int(0), *h = p2c_obj_from_int(0);
    if (nargs > 0 && args[0]) {
        w = p2c_subscript_get(args[0], p2c_obj_from_int(0));
        h = p2c_subscript_get(args[0], p2c_obj_from_int(1));
    }
    p2c_setattr(self, "width", w);
    p2c_setattr(self, "height", h);
}

static P2C_Object* surface_ctor(P2C_Object **args, size_t nargs) {
    P2C_Object *self = p2c_instance_new(surface_classobj());
    surface_init_self(self, args, nargs);
    return self;
}

static P2C_Object* surface_init_method(P2C_Object *self, P2C_Object **args, size_t nargs) {
    surface_init_self(self, args, nargs);
    return &P2C_None;
}

static P2C_Object* surface_fill(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)self; (void)args; (void)nargs;
    return &P2C_None;
}
static P2C_Object* surface_blit(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)self; (void)args; (void)nargs;
    return &P2C_None;
}
static P2C_Object* surface_get_rect(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    P2C_Object *rect_args[4] = { p2c_obj_from_int(0), p2c_obj_from_int(0), p2c_getattr(self, "width"), p2c_getattr(self, "height") };
    return rect_ctor(rect_args, 4);
}
static P2C_Object* surface_get_width(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    return p2c_getattr(self, "width");
}
static P2C_Object* surface_get_height(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    return p2c_getattr(self, "height");
}

static P2C_MethodDef g_surface_methods[] = {
    {"__init__", surface_init_method, NULL, P2C_METHOD_INSTANCE},
    {"fill", surface_fill, NULL, P2C_METHOD_INSTANCE},
    {"blit", surface_blit, NULL, P2C_METHOD_INSTANCE},
    {"get_rect", surface_get_rect, NULL, P2C_METHOD_INSTANCE},
    {"get_width", surface_get_width, NULL, P2C_METHOD_INSTANCE},
    {"get_height", surface_get_height, NULL, P2C_METHOD_INSTANCE},
    {NULL, NULL, NULL, P2C_METHOD_INSTANCE}
};

static P2C_Object* surface_classobj(void) {
    if (!g_surface_class) g_surface_class = p2c_class_new("Surface", surface_ctor, g_surface_methods, NULL);
    return g_surface_class;
}

/* ============================================================
 * pygame.time.Clock
 * ============================================================ */
static P2C_Object *g_clock_class = NULL;
static P2C_Object* clock_classobj(void);

static P2C_Object* clock_ctor(P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    return p2c_instance_new(clock_classobj());
}
static P2C_Object* clock_tick(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)self; (void)args; (void)nargs;
    return p2c_obj_from_int(16);
}
static P2C_Object* clock_get_fps(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)self; (void)args; (void)nargs;
    return p2c_obj_from_float(60.0);
}
static P2C_Object* clock_init_method(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)self; (void)args; (void)nargs;
    return &P2C_None;
}
static P2C_MethodDef g_clock_methods[] = {
    {"__init__", clock_init_method, NULL, P2C_METHOD_INSTANCE},
    {"tick", clock_tick, NULL, P2C_METHOD_INSTANCE},
    {"tick_busy_loop", clock_tick, NULL, P2C_METHOD_INSTANCE},
    {"get_fps", clock_get_fps, NULL, P2C_METHOD_INSTANCE},
    {NULL, NULL, NULL, P2C_METHOD_INSTANCE}
};
static P2C_Object* clock_classobj(void) {
    if (!g_clock_class) g_clock_class = p2c_class_new("Clock", clock_ctor, g_clock_methods, NULL);
    return g_clock_class;
}

/* ============================================================
 * pygame.sprite.Sprite / Group
 * ============================================================ */
static P2C_Object *g_sprite_class = NULL;
static P2C_Object* sprite_classobj(void);

static void sprite_init_self(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    p2c_setattr(self, "rect", &P2C_None);
    p2c_setattr(self, "image", &P2C_None);
}

static P2C_Object* sprite_ctor(P2C_Object **args, size_t nargs) {
    P2C_Object *self = p2c_instance_new(sprite_classobj());
    sprite_init_self(self, args, nargs);
    return self;
}
static P2C_Object* sprite_init_method(P2C_Object *self, P2C_Object **args, size_t nargs) {
    sprite_init_self(self, args, nargs);
    return &P2C_None;
}
static P2C_MethodDef g_sprite_methods[] = { {"__init__", sprite_init_method, NULL, P2C_METHOD_INSTANCE}, {NULL, NULL, NULL, P2C_METHOD_INSTANCE} };
static P2C_Object* sprite_classobj(void) {
    if (!g_sprite_class) g_sprite_class = p2c_class_new("Sprite", sprite_ctor, g_sprite_methods, NULL);
    return g_sprite_class;
}

static P2C_Object *g_group_class = NULL;
static P2C_Object* group_classobj(void);

static void group_init_self(P2C_Object *self, P2C_Object **args, size_t nargs) {
    P2C_Object *list = p2c_list_new();
    for (size_t i = 0; i < nargs; i++) p2c_list_append(list, args[i]);
    p2c_setattr(self, "_sprites", list);
}
static P2C_Object* group_ctor(P2C_Object **args, size_t nargs) {
    P2C_Object *self = p2c_instance_new(group_classobj());
    group_init_self(self, args, nargs);
    return self;
}
static P2C_Object* group_init_method(P2C_Object *self, P2C_Object **args, size_t nargs) {
    group_init_self(self, args, nargs);
    return &P2C_None;
}
static P2C_Object* group_add(P2C_Object *self, P2C_Object **args, size_t nargs) {
    P2C_Object *list = p2c_getattr(self, "_sprites");
    for (size_t i = 0; i < nargs; i++) p2c_list_append(list, args[i]);
    return &P2C_None;
}
static P2C_Object* group_sprites(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    return p2c_getattr(self, "_sprites");
}
static P2C_Object* group_update(P2C_Object *self, P2C_Object **args, size_t nargs) {
    P2C_Object *list = p2c_getattr(self, "_sprites");
    size_t n = p2c_list_len(list);
    for (size_t i = 0; i < n; i++) {
        P2C_Object *sp = p2c_list_get(list, i);
        if (p2c_has_method(sp, "update")) p2c_call_attr(sp, "update", args, nargs);
    }
    return &P2C_None;
}
static P2C_Object* group_draw(P2C_Object *self, P2C_Object **args, size_t nargs) {
    (void)self; (void)args; (void)nargs;
    return &P2C_None;
}
static P2C_MethodDef g_group_methods[] = {
    {"__init__", group_init_method, NULL, P2C_METHOD_INSTANCE},
    {"add", group_add, NULL, P2C_METHOD_INSTANCE},
    {"sprites", group_sprites, NULL, P2C_METHOD_INSTANCE},
    {"update", group_update, NULL, P2C_METHOD_INSTANCE},
    {"draw", group_draw, NULL, P2C_METHOD_INSTANCE},
    {NULL, NULL, NULL, P2C_METHOD_INSTANCE}
};
static P2C_Object* group_classobj(void) {
    if (!g_group_class) g_group_class = p2c_class_new("Group", group_ctor, g_group_methods, NULL);
    return g_group_class;
}

/* ============================================================
 * モジュール関数群
 * ============================================================ */
static P2C_Object* pg_init(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_quit(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }

static P2C_Object* pg_display_set_mode(P2C_Object **args, size_t nargs) {
    return surface_ctor(args, nargs);
}
static P2C_Object* pg_display_set_caption(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_display_flip(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_display_update(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }

static int64_t g_ticks_counter = 0;
static P2C_Object* pg_time_get_ticks(P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    g_ticks_counter += 16;
    return p2c_obj_from_int(g_ticks_counter);
}
static P2C_Object* pg_time_delay(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_time_wait(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return p2c_obj_from_int(0); }

static P2C_Object* pg_event_get(P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    return p2c_list_new();
}
static P2C_Object* pg_event_pump(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }

static P2C_Object* pg_draw_rect(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_draw_circle(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_draw_line(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_draw_polygon(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }
static P2C_Object* pg_draw_ellipse(P2C_Object **args, size_t nargs) { (void)args; (void)nargs; return &P2C_None; }

static P2C_Object* pg_key_get_pressed(P2C_Object **args, size_t nargs) {
    (void)args; (void)nargs;
    P2C_Object *list = p2c_list_new();
    for (int i = 0; i < 512; i++) p2c_list_append(list, p2c_obj_from_bool(false));
    return list;
}

/* ============================================================
 * モジュール登録
 * ============================================================ */
void p2c_pygame_runtime_reset(void) {
    g_rect_class = NULL;
    g_surface_class = NULL;
    g_clock_class = NULL;
    g_sprite_class = NULL;
    g_group_class = NULL;
    g_ticks_counter = 0;
}

void p2c_register_pygame_module(void) {
    P2C_Object *pygame_mod = p2c_module_new("pygame");
    if (!pygame_mod) return;

    p2c_module_set_attr(pygame_mod, "init", p2c_function_new("init", pg_init));
    p2c_module_set_attr(pygame_mod, "quit", p2c_function_new("quit", pg_quit));

    p2c_module_set_attr(pygame_mod, "QUIT", p2c_obj_from_int(0));
    p2c_module_set_attr(pygame_mod, "KEYDOWN", p2c_obj_from_int(1));
    p2c_module_set_attr(pygame_mod, "KEYUP", p2c_obj_from_int(2));
    p2c_module_set_attr(pygame_mod, "MOUSEBUTTONDOWN", p2c_obj_from_int(3));
    p2c_module_set_attr(pygame_mod, "MOUSEBUTTONUP", p2c_obj_from_int(4));
    p2c_module_set_attr(pygame_mod, "MOUSEMOTION", p2c_obj_from_int(5));
    p2c_module_set_attr(pygame_mod, "K_LEFT", p2c_obj_from_int(10));
    p2c_module_set_attr(pygame_mod, "K_RIGHT", p2c_obj_from_int(11));
    p2c_module_set_attr(pygame_mod, "K_UP", p2c_obj_from_int(12));
    p2c_module_set_attr(pygame_mod, "K_DOWN", p2c_obj_from_int(13));
    p2c_module_set_attr(pygame_mod, "K_SPACE", p2c_obj_from_int(14));
    p2c_module_set_attr(pygame_mod, "K_ESCAPE", p2c_obj_from_int(15));
    p2c_module_set_attr(pygame_mod, "K_RETURN", p2c_obj_from_int(16));

    p2c_module_set_attr(pygame_mod, "Surface", surface_classobj());
    p2c_module_set_attr(pygame_mod, "Rect", rect_classobj());

    P2C_Object *display_mod = p2c_module_new("display");
    if (display_mod) {
        p2c_module_set_attr(display_mod, "set_mode", p2c_function_new("set_mode", pg_display_set_mode));
        p2c_module_set_attr(display_mod, "set_caption", p2c_function_new("set_caption", pg_display_set_caption));
        p2c_module_set_attr(display_mod, "flip", p2c_function_new("flip", pg_display_flip));
        p2c_module_set_attr(display_mod, "update", p2c_function_new("update", pg_display_update));
        p2c_module_set_attr(pygame_mod, "display", display_mod);
    }

    P2C_Object *time_mod = p2c_module_new("time");
    if (time_mod) {
        p2c_module_set_attr(time_mod, "Clock", clock_classobj());
        p2c_module_set_attr(time_mod, "get_ticks", p2c_function_new("get_ticks", pg_time_get_ticks));
        p2c_module_set_attr(time_mod, "delay", p2c_function_new("delay", pg_time_delay));
        p2c_module_set_attr(time_mod, "wait", p2c_function_new("wait", pg_time_wait));
        p2c_module_set_attr(pygame_mod, "time", time_mod);
    }

    P2C_Object *event_mod = p2c_module_new("event");
    if (event_mod) {
        p2c_module_set_attr(event_mod, "get", p2c_function_new("get", pg_event_get));
        p2c_module_set_attr(event_mod, "pump", p2c_function_new("pump", pg_event_pump));
        p2c_module_set_attr(pygame_mod, "event", event_mod);
    }

    P2C_Object *draw_mod = p2c_module_new("draw");
    if (draw_mod) {
        p2c_module_set_attr(draw_mod, "rect", p2c_function_new("rect", pg_draw_rect));
        p2c_module_set_attr(draw_mod, "circle", p2c_function_new("circle", pg_draw_circle));
        p2c_module_set_attr(draw_mod, "line", p2c_function_new("line", pg_draw_line));
        p2c_module_set_attr(draw_mod, "polygon", p2c_function_new("polygon", pg_draw_polygon));
        p2c_module_set_attr(draw_mod, "ellipse", p2c_function_new("ellipse", pg_draw_ellipse));
        p2c_module_set_attr(pygame_mod, "draw", draw_mod);
    }

    P2C_Object *key_mod = p2c_module_new("key");
    if (key_mod) {
        p2c_module_set_attr(key_mod, "get_pressed", p2c_function_new("get_pressed", pg_key_get_pressed));
        p2c_module_set_attr(pygame_mod, "key", key_mod);
    }

    P2C_Object *sprite_mod = p2c_module_new("sprite");
    if (sprite_mod) {
        p2c_module_set_attr(sprite_mod, "Sprite", sprite_classobj());
        p2c_module_set_attr(sprite_mod, "Group", group_classobj());
        p2c_module_set_attr(pygame_mod, "sprite", sprite_mod);
    }

    p2c_register_module(pygame_mod);
}
