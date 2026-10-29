/* _cosgui: the C-OS window behind Sprint's `window` module (see cosgui.sp). One window per program.
 *   _cosgui.open(w, h, title)   -> 1 on success, 0 when the window cannot be made
 *   _cosgui.poll(timeout_ms)    -> a flat array of 7-int records: type, x, y, button, mods, special, ascii
 *                                  type 1 key, 2 move, 3 down, 4 up, 5 wheel (button = wheel steps), 6 close, 7 resize (x, y = new size)
 *   _cosgui.present(canvas)     -> copies a gfx canvas of the window size into the window and shows it */
#include "sp.h"
#include "cos.h"

static int64_t s_win;
static cos_win_info_t s_wi;

const uint32_t *gfx_canvas_pixels(SpValue v, int *w, int *h);

static int c_open(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_INT || argv[1].type != T_INT || argv[2].type != T_STRING) return vm_set_error(vm, E_TYPE, "open(w, h, title)");
    if (s_win > 0) { *ret = v_int(1); return 0; }
    int64_t w = argv[0].as.i, h = argv[1].as.i;
    if (w < 1 || h < 1 || w > 4096 || h > 4096) return vm_set_error(vm, E_VALUE, "window size out of range");
    s_win = cos_win2_create(AS_STR(argv[2])->data, (int32_t)w, (int32_t)h, &s_wi);
    *ret = v_int(s_win > 0 ? 1 : 0);
    return 0;
}

static void put(SArray *a, VM *vm, int64_t v) { array_push(vm, a, v_int(v)); }

static int c_poll(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_INT) return vm_set_error(vm, E_TYPE, "poll(timeout_ms)");
    SArray *a = vm_new_array(vm, 0);
    *ret = v_obj(T_ARRAY, a);
    if (s_win <= 0) return 0;
    cos_win_event_t ev;
    int r = cos_win2_wait(s_win, &ev, (uint32_t)(argv[0].as.i < 0 ? 0 : argv[0].as.i));
    int n = 0;
    while (r > 0 && n < 64) {
        int type = 0;
        switch (ev.type) {
            case COS_EV_KEY: type = 1; break;
            case COS_EV_MOUSE_MOVE: type = 2; break;
            case COS_EV_MOUSE_DOWN: type = 3; break;
            case COS_EV_MOUSE_UP: type = 4; break;
            case COS_EV_WHEEL: type = 5; break;
            case COS_EV_CLOSE: type = 6; break;
            case COS_EV_RESIZE: type = 7; break;
            default: break;
        }
        if (type) {
            put(a, vm, type); put(a, vm, ev.x); put(a, vm, ev.y);
            put(a, vm, type == 5 ? ev.wheel : ev.button); put(a, vm, ev.mods); put(a, vm, ev.special); put(a, vm, (unsigned char)ev.ascii);
            n++;
        }
        r = cos_win2_poll(s_win, &ev);
    }
    return 0;
}

static int c_present(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    int w = 0, h = 0;
    const uint32_t *px = gfx_canvas_pixels(argv[0], &w, &h);
    if (!px) return vm_set_error(vm, E_TYPE, "present(canvas) needs a gfx canvas");
    if (s_win > 0) {
        int cw = w < s_wi.width ? w : s_wi.width, ch = h < s_wi.height ? h : s_wi.height;
        for (int y = 0; y < ch; ++y) memcpy(s_wi.pixels + (size_t)y * s_wi.stride, px + (size_t)y * w, (size_t)cw * 4);
        cos_win2_present(s_win);
    }
    *ret = v_nil();
    return 0;
}

static int c_title(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc;
    if (argv[0].type != T_STRING) return vm_set_error(vm, E_TYPE, "set_title(s)");
    if (s_win > 0) cos_win2_set_title(s_win, AS_STR(argv[0])->data);
    *ret = v_nil();
    return 0;
}

static int c_close(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)vm; (void)argc; (void)argv;
    if (s_win > 0) { cos_win2_close(s_win); s_win = 0; }
    *ret = v_nil();
    return 0;
}

const NativeDesc lib_cosgui_fns[] = {
    {"open", 3, 3, c_open}, {"poll", 1, 1, c_poll}, {"present", 1, 1, c_present}, {"set_title", 1, 1, c_title}, {"close", 0, 0, c_close}, {NULL, 0, 0, NULL}};
