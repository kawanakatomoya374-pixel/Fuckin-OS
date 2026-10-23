/* glfw_cos.c - see glfw_cos.h. */
#include <string.h>
#include <stdlib.h>
#include "cos.h"
#include "glfw_cos.h"
#include "r3d.h"

struct GLFWwindow {
    int64_t h; cos_win_info_t info;
    int should_close, cursor_mode, focused;
    double vx, vy;                       /* virtual cursor while the pointer is locked: it only ever accumulates the motion */
    int real_x, real_y;
    uint8_t keys[32], prev[32];
    GLFWkeyfun key_cb; GLFWcharfun char_cb; GLFWmousebuttonfun mb_cb; GLFWscrollfun scroll_cb;
    uint64_t rep_at; int rep_key;
    int mods;
};
static GLFWwindow s_win;
static uint64_t s_t0;
static float s_mouse_scale = 1.0f;
static char s_clip[512];

double glfwGetTime(void) { return (double)(cos_time_ms() - s_t0) / 1000.0; }
void glfwSetTime(double t) { s_t0 = cos_time_ms() - (uint64_t)(t * 1000.0); }
int glfwInit(void) { s_t0 = cos_time_ms(); return 1; }
void glfwTerminate(void) { if (s_win.h > 0) { cos_win2_grab(s_win.h, 0); cos_win2_close(s_win.h); s_win.h = 0; } }

GLFWwindow *glfwCreateWindow(int w, int h, const char *title, GLFWmonitor *m, GLFWwindow *share) {
    (void)m; (void)share;
    memset(&s_win, 0, sizeof s_win);
    s_win.h = cos_win2_create(title, w, h, &s_win.info);
    if (s_win.h <= 0) return NULL;
    s_win.cursor_mode = GLFW_CURSOR_NORMAL;
    return &s_win;
}
void glfwMakeContextCurrent(GLFWwindow *w) { (void)w; }
void glfwSwapInterval(int n) { (void)n; }
void plat_present_window(void) { cos_win2_present(s_win.h); }
static int s_blitted;
void plat_blit_scene(void) { r3d_present(s_win.info.pixels, s_win.info.stride, s_win.info.width, s_win.info.height); s_blitted = 1; }
void glfwSwapBuffers(GLFWwindow *w) {
    if (!s_blitted) r3d_present(w->info.pixels, w->info.stride, w->info.width, w->info.height);
    s_blitted = 0;
    cos_win2_present(w->h);
    cos_yield();                                       /* let the compositor and the rest of the desktop have the CPU */
}
void *plat_pixels(int *stride, int *w, int *h) { if (stride) *stride = s_win.info.stride; if (w) *w = s_win.info.width; if (h) *h = s_win.info.height; return s_win.info.pixels; }
int glfwWindowShouldClose(GLFWwindow *w) { return w->should_close; }
void glfwSetWindowShouldClose(GLFWwindow *w, int v) { w->should_close = v; }
void plat_set_title(const char *t) { if (s_win.h > 0) cos_win2_set_title(s_win.h, t); }
void plat_set_mouse_scale(float s) { s_mouse_scale = s; }
float plat_get_mouse_scale(void) { return s_mouse_scale; }
/* F12. Going full screen also switches the pixel surface to the screen's own resolution (as far as the 16 MiB a surface may take), so the game is
 * drawn 1:1 instead of being scaled up from the window size; leaving it brings the window's size back. */
static int s_fs, s_win_w, s_win_h;
int plat_toggle_fullscreen(void) {
    if (s_win.h <= 0) return 0;
    cos_win_info_t info;
    if (!s_fs) {
        int32_t sw = 0, sh = 0; cos_win2_screen_size(&sw, &sh);
        s_win_w = s_win.info.width; s_win_h = s_win.info.height;
        if (cos_win2_fullscreen(s_win.h, 1) != 1) return 0;
        s_fs = 1;
        while (sw > 64 && sh > 48 && (int64_t)sw * sh * 4 > (15LL << 20)) { sw = sw * 3 / 4; sh = sh * 3 / 4; }
        if (sw >= 640 && sh >= 480 && cos_win2_resize(s_win.h, sw, sh, &info) == 0) s_win.info = info;
        return 1;
    }
    if (cos_win2_resize(s_win.h, s_win_w, s_win_h, &info) == 0) s_win.info = info;
    cos_win2_fullscreen(s_win.h, 0);
    s_fs = 0;
    return 0;
}
int plat_focused(void) { return s_win.focused; }

void glfwSetInputMode(GLFWwindow *w, int mode, int value) {
    if (mode != GLFW_CURSOR) return;
    w->cursor_mode = value;
    cos_win2_grab(w->h, value == GLFW_CURSOR_DISABLED);
}
int glfwGetInputMode(GLFWwindow *w, int mode) { return mode == GLFW_CURSOR ? w->cursor_mode : 0; }
void glfwGetCursorPos(GLFWwindow *w, double *x, double *y) {
    if (w->cursor_mode == GLFW_CURSOR_DISABLED) { *x = w->vx; *y = w->vy; } else { *x = w->real_x; *y = w->real_y; }
}
void plat_client_cursor(int *x, int *y) { *x = s_win.real_x; *y = s_win.real_y; }
void glfwGetWindowSize(GLFWwindow *w, int *width, int *height) { *width = w->info.width; *height = w->info.height; }
void glfwGetFramebufferSize(GLFWwindow *w, int *width, int *height) { (void)w; *width = r3d_width(); *height = r3d_height(); }
void glfwSetKeyCallback(GLFWwindow *w, GLFWkeyfun f) { w->key_cb = f; }
void glfwSetCharCallback(GLFWwindow *w, GLFWcharfun f) { w->char_cb = f; }
void glfwSetMouseButtonCallback(GLFWwindow *w, GLFWmousebuttonfun f) { w->mb_cb = f; }
void glfwSetScrollCallback(GLFWwindow *w, GLFWscrollfun f) { w->scroll_cb = f; }
const char *glfwGetClipboardString(GLFWwindow *w) { (void)w; int64_t n = cos_clip_get(s_clip, sizeof s_clip - 1); if (n < 0) n = 0; s_clip[n] = 0; return s_clip; }

/* GLFW key code <-> set-1 scan code (the keys Craft knows about) */
static int key_to_sc(int key) {
    static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
    if (key >= '1' && key <= '9') return 0x02 + (key - '1');
    if (key == '0') return 0x0B;
    switch (key) {
        case GLFW_KEY_SPACE: return 0x39; case GLFW_KEY_ESCAPE: return 0x01; case GLFW_KEY_ENTER: return 0x1C; case GLFW_KEY_TAB: return 0x0F;
        case GLFW_KEY_F12: return 0x58; case GLFW_KEY_BACKSPACE: return 0x0E; case GLFW_KEY_LEFT_SHIFT: return 0x2A; case GLFW_KEY_LEFT_CONTROL: return 0x1D;
        case GLFW_KEY_UP: return 0x80 | 0x48; case GLFW_KEY_DOWN: return 0x80 | 0x50; case GLFW_KEY_LEFT: return 0x80 | 0x4B; case GLFW_KEY_RIGHT: return 0x80 | 0x4D;
    }
    return -1;
}
static int sc_to_key(int sc) {
    for (int k = 'A'; k <= 'Z'; ++k) if (key_to_sc(k) == sc) return k;
    for (int k = '0'; k <= '9'; ++k) if (key_to_sc(k) == sc) return k;
    static const int specials[] = { GLFW_KEY_SPACE, GLFW_KEY_ESCAPE, GLFW_KEY_ENTER, GLFW_KEY_TAB, GLFW_KEY_BACKSPACE, GLFW_KEY_LEFT_SHIFT, GLFW_KEY_LEFT_CONTROL,
                                    GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT, GLFW_KEY_F12 };
    for (unsigned i = 0; i < sizeof specials / sizeof specials[0]; ++i) if (key_to_sc(specials[i]) == sc) return specials[i];
    if (sc == 0x36) return GLFW_KEY_LEFT_SHIFT;
    if (sc == (0x80 | 0x1D)) return GLFW_KEY_LEFT_CONTROL;
    return -1;
}
static int key_down(GLFWwindow *w, int sc) { return sc >= 0 && ((w->keys[sc >> 3] >> (sc & 7)) & 1); }
int glfwGetKey(GLFWwindow *w, int key) {
    if (key == GLFW_KEY_LEFT_SHIFT) return key_down(w, 0x2A) || key_down(w, 0x36);
    return key_down(w, key_to_sc(key));
}

void glfwPollEvents(void) {
    GLFWwindow *w = &s_win;
    cos_win_event_t ev;
    while (cos_win2_poll(w->h, &ev) > 0) {
        int mods = ((ev.mods & COS_MOD_SHIFT) ? GLFW_MOD_SHIFT : 0) | ((ev.mods & COS_MOD_CTRL) ? GLFW_MOD_CONTROL : 0) | ((ev.mods & COS_MOD_ALT) ? GLFW_MOD_ALT : 0);
        switch (ev.type) {
            case COS_EV_CLOSE: w->should_close = 1; break;
            case COS_EV_KEY:
                w->mods = mods;
                if (ev.special == COS_KEY_NONE && ev.ascii >= 32 && ev.ascii < 127 && !(ev.mods & COS_MOD_CTRL) && w->char_cb) w->char_cb(w, (unsigned)(unsigned char)ev.ascii);
                break;
            case COS_EV_MOUSE_MOVE: w->real_x = ev.x; w->real_y = ev.y; break;
            case COS_EV_MOUSE_DOWN: case COS_EV_MOUSE_UP: {
                int b = ev.button == COS_MOUSE_BTN_LEFT ? GLFW_MOUSE_BUTTON_LEFT : ev.button == COS_MOUSE_BTN_RIGHT ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_MIDDLE;
                w->real_x = ev.x; w->real_y = ev.y;
                if (w->mb_cb) w->mb_cb(w, b, ev.type == COS_EV_MOUSE_DOWN ? GLFW_PRESS : GLFW_RELEASE, w->mods);
                break; }
            case COS_EV_WHEEL: if (w->scroll_cb) w->scroll_cb(w, 0.0, ev.wheel > 0 ? 1.0 : -1.0); break;
            default: break;
        }
    }
    cos_win_input_t in;
    if (cos_win2_input(w->h, &in) == 0) {
        w->focused = in.focused;
        if (w->cursor_mode == GLFW_CURSOR_DISABLED) { w->vx += in.dx * s_mouse_scale; w->vy += in.dy * s_mouse_scale; }
        memcpy(w->prev, w->keys, 32); memcpy(w->keys, in.keys, 32);
        int mods = (key_down(w, 0x2A) || key_down(w, 0x36) ? GLFW_MOD_SHIFT : 0) | (key_down(w, 0x1D) ? GLFW_MOD_CONTROL : 0) | (key_down(w, 0x38) ? GLFW_MOD_ALT : 0);
        w->mods = mods;
        uint64_t now = cos_time_ms();
        for (int sc = 0; sc < 256; ++sc) {
            int was = (w->prev[sc >> 3] >> (sc & 7)) & 1, is = (w->keys[sc >> 3] >> (sc & 7)) & 1;
            if (was == is) continue;
            int key = sc_to_key(sc);
            if (key < 0 || !w->key_cb) continue;
            w->key_cb(w, key, sc, is ? GLFW_PRESS : GLFW_RELEASE, mods);
            if (key == GLFW_KEY_BACKSPACE) { w->rep_key = is ? key : 0; w->rep_at = now + 400; }
        }
        if (w->rep_key && key_down(w, key_to_sc(w->rep_key)) && now >= w->rep_at && w->key_cb) { w->key_cb(w, w->rep_key, 0, GLFW_REPEAT, mods); w->rep_at = now + 40; }
        else if (w->rep_key && !key_down(w, key_to_sc(w->rep_key))) w->rep_key = 0;
    }
}
