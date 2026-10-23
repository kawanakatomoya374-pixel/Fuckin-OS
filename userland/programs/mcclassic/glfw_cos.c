/* glfw_cos.c - the part of GLFW 3 that mc-classic-c uses, on the C-OS window API (cos_win2_*), including the pointer lock
 * (cos_win2_grab) that makes GLFW_CURSOR_DISABLED work. The picture comes from mcgl.c (the CPU rasteriser). */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cos.h"
#include <GLFW/glfw3.h>
#include "mcgl.h"

struct GLFWwindow {
    int64_t h; cos_win_info_t info;
    int should_close, cursor_mode, focused;
    double vx, vy;                       /* virtual cursor while the pointer is locked: it only accumulates the motion */
    int real_x, real_y, buttons, mods;
    uint8_t keys[32], prev[32];
    GLFWkeyfun key_cb; GLFWcharfun char_cb; GLFWmousebuttonfun mb_cb; GLFWscrollfun scroll_cb; GLFWcursorposfun cp_cb;
    uint64_t rep_at; int rep_key;
};
static GLFWwindow s_win;
static uint64_t s_t0;
/* The picture is always drawn at the window's full size, and so is everything the player looks at. What gives way on a machine that is too slow is what is far
 * away: the frame time decides a level that caps the view distance (mcgl_set_far_cap: geometry beyond it is not drawn, the fog is pulled in to meet it so the
 * world fades out). F10: auto, or a fixed level (0 the whole world ... 7 the shortest). (mcgl.c can also draw the part of the picture away from the crosshair in 2x2
 * blocks, mcgl_set_fovea(): it saves pixels, but a frame there costs more for its triangles than for its pixels, so it is not used.) */
void mcgl_set_far_cap(float d);
void mcgl_set_fovea(int level);
static const struct { int fovea; float cap; } s_levels[] = { {0, 1e9f}, {0, 256}, {0, 192}, {0, 128}, {0, 96}, {0, 64}, {0, 48}, {0, 32} };
#define NLEVELS ((int)(sizeof s_levels / sizeof s_levels[0]))
static int s_level_mode = -1;                              /* -1 auto, else a fixed level */
static int s_level;                                        /* the level in force */
static int s_fs, s_win_w, s_win_h, s_since;
static uint64_t s_last_ms, s_ema_ms;
static void apply_level(int l) { s_level = l; mcgl_set_fovea(s_levels[l].fovea); mcgl_set_far_cap(s_levels[l].cap); }
static char s_title[64] = "Minecraft";
static GLFWvidmode s_mode;

double glfwGetTime(void) { return (double)(cos_time_ms() - s_t0) / 1000.0; }
void glfwSetTime(double t) { s_t0 = cos_time_ms() - (uint64_t)(t * 1000.0); }
int glfwInit(void) { s_t0 = cos_time_ms(); return 1; }
void glfwWindowHint(int hint, int value) { (void)hint; (void)value; }
void glfwDestroyWindow(GLFWwindow *w) { (void)w; if (s_win.h > 0) { cos_win2_grab(s_win.h, 0); cos_win2_close(s_win.h); s_win.h = 0; } }
void glfwTerminate(void) { glfwDestroyWindow(&s_win); }
GLFWmonitor *glfwGetPrimaryMonitor(void) { return (GLFWmonitor *)1; }
const GLFWvidmode *glfwGetVideoMode(GLFWmonitor *m) {
    (void)m; int32_t w = 1024, h = 768; cos_win2_screen_size(&w, &h);
    s_mode.width = w; s_mode.height = h; s_mode.redBits = s_mode.greenBits = s_mode.blueBits = 8; s_mode.refreshRate = 60; return &s_mode;
}

static void plat_toggle_fullscreen_hook(void);
GLFWwindow *glfwCreateWindow(int w, int h, const char *title, GLFWmonitor *m, GLFWwindow *share) {
    (void)m; (void)share;
    memset(&s_win, 0, sizeof s_win);
    s_win.h = cos_win2_create(title, w, h, &s_win.info);
    if (s_win.h <= 0) { printf("glfw: cos_win2_create(%d,%d) failed: %d\n", w, h, (int)s_win.h); return NULL; }
    s_win.cursor_mode = GLFW_CURSOR_NORMAL;
    snprintf(s_title, sizeof s_title, "%s", title);
    mcgl_resize(s_win.info.width, s_win.info.height);
    apply_level(0);
    s_last_ms = cos_time_ms();
    if (m) plat_toggle_fullscreen_hook();
    return &s_win;
}
void glfwMakeContextCurrent(GLFWwindow *w) { (void)w; }
void glfwSwapInterval(int n) { (void)n; }
static void apply_scale(GLFWwindow *w) { mcgl_resize(w->info.width, w->info.height); s_since = 0; }
static void update_title(GLFWwindow *w) {
    char t[112]; snprintf(t, sizeof t, "%s  [detail %d%s]", s_title, s_level, s_level_mode < 0 ? " auto" : "");
    cos_win2_set_title(w->h, t);
}
void mcgl_prof_report(void);
void glfwSwapBuffers(GLFWwindow *w) {
    mcgl_present(w->info.pixels, w->info.stride, w->info.width, w->info.height);
    cos_win2_present(w->h);
    cos_yield();                                       /* let the compositor and the rest of the desktop have the CPU */
    mcgl_prof_report();
    uint64_t now = cos_time_ms(), dt = now - s_last_ms; s_last_ms = now;
    if (dt > 150) dt = 150;                            /* a loading screen or a hitch is not what the level should answer to */
    s_ema_ms = s_ema_ms ? (s_ema_ms * 7 + dt) / 8 : dt;
    if (s_level_mode < 0 && ++s_since >= 8) {
        s_since = 0;
        /* aim for 50-90 ms a frame (11-20 fps) before anything is reduced further; on a fast machine the level falls back to full quality */
        if (s_ema_ms > 90 && s_level < NLEVELS - 1) { apply_level(s_level + 1); update_title(w); }
        else if (s_ema_ms < 50 && s_level > 0) { apply_level(s_level - 1); update_title(w); }
    }
}
int glfwWindowShouldClose(GLFWwindow *w) { return w->should_close; }
void glfwSetWindowShouldClose(GLFWwindow *w, int v) { w->should_close = v; }
void glfwSetWindowTitle(GLFWwindow *w, const char *t) { snprintf(s_title, sizeof s_title, "%s", t); update_title(w); }
/* F11. Full screen also switches the pixel surface to the screen's own resolution (as far as the 16 MiB a surface may take); leaving it brings the
 * window's size back. The game sees the new size through glfwGetFramebufferSize(), the picture is re-created at the current scale. */
static void toggle_fullscreen(GLFWwindow *w) {
    cos_win_info_t info;
    if (!s_fs) {
        int32_t sw = 0, sh = 0; cos_win2_screen_size(&sw, &sh);
        s_win_w = w->info.width; s_win_h = w->info.height;
        if (cos_win2_fullscreen(w->h, 1) != 1) return;
        s_fs = 1;
        while (sw > 64 && sh > 48 && (int64_t)sw * sh * 4 > (15LL << 20)) { sw = sw * 3 / 4; sh = sh * 3 / 4; }
        if (sw >= 320 && sh >= 240 && cos_win2_resize(w->h, sw, sh, &info) == 0) w->info = info;
    } else {
        if (cos_win2_resize(w->h, s_win_w, s_win_h, &info) == 0) w->info = info;
        cos_win2_fullscreen(w->h, 0);
        s_fs = 0;
    }
    apply_scale(w);
}
static void plat_toggle_fullscreen_hook(void) { toggle_fullscreen(&s_win); }
int glfwGetWindowAttrib(GLFWwindow *w, int a) { return a == GLFW_FOCUSED ? w->focused : 0; }

void glfwSetInputMode(GLFWwindow *w, int mode, int value) {
    if (mode != GLFW_CURSOR) return;
    w->cursor_mode = value;
    if (value == GLFW_CURSOR_DISABLED) { w->vx = w->info.width * 0.5; w->vy = w->info.height * 0.5; }   /* like GLFW: the virtual cursor starts where the real one was (the game re-centres it every frame) */
    cos_win2_grab(w->h, value == GLFW_CURSOR_DISABLED);
}
int glfwGetInputMode(GLFWwindow *w, int mode) { return mode == GLFW_CURSOR ? w->cursor_mode : 0; }
void glfwGetCursorPos(GLFWwindow *w, double *x, double *y) {
    if (w->cursor_mode == GLFW_CURSOR_DISABLED) { *x = w->vx; *y = w->vy; } else { *x = w->real_x; *y = w->real_y; }
}
void glfwSetCursorPos(GLFWwindow *w, double x, double y) { if (w->cursor_mode == GLFW_CURSOR_DISABLED) { w->vx = x; w->vy = y; } }
int glfwGetMouseButton(GLFWwindow *w, int b) { return (w->buttons >> b) & 1; }
void glfwGetWindowSize(GLFWwindow *w, int *width, int *height) { *width = w->info.width; *height = w->info.height; }
void glfwGetFramebufferSize(GLFWwindow *w, int *width, int *height) { (void)w; *width = mcgl_width(); *height = mcgl_height(); }
GLFWkeyfun glfwSetKeyCallback(GLFWwindow *w, GLFWkeyfun f) { GLFWkeyfun o = w->key_cb; w->key_cb = f; return o; }
GLFWcharfun glfwSetCharCallback(GLFWwindow *w, GLFWcharfun f) { GLFWcharfun o = w->char_cb; w->char_cb = f; return o; }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow *w, GLFWmousebuttonfun f) { GLFWmousebuttonfun o = w->mb_cb; w->mb_cb = f; return o; }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow *w, GLFWscrollfun f) { GLFWscrollfun o = w->scroll_cb; w->scroll_cb = f; return o; }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow *w, GLFWcursorposfun f) { GLFWcursorposfun o = w->cp_cb; w->cp_cb = f; return o; }
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow *w, GLFWframebuffersizefun f) { (void)w; (void)f; return 0; }

/* GLFW key <-> set-1 make code (+128 for the extended keys) */
static const struct { int key; int sc; } k_table[] = {
    { GLFW_KEY_ESCAPE, 0x01 }, { GLFW_KEY_MINUS, 0x0C }, { GLFW_KEY_EQUAL, 0x0D }, { GLFW_KEY_BACKSPACE, 0x0E }, { GLFW_KEY_TAB, 0x0F },
    { GLFW_KEY_LEFT_BRACKET, 0x1A }, { GLFW_KEY_RIGHT_BRACKET, 0x1B }, { GLFW_KEY_ENTER, 0x1C }, { GLFW_KEY_LEFT_CONTROL, 0x1D },
    { GLFW_KEY_SEMICOLON, 0x27 }, { GLFW_KEY_APOSTROPHE, 0x28 }, { GLFW_KEY_GRAVE_ACCENT, 0x29 }, { GLFW_KEY_LEFT_SHIFT, 0x2A },
    { GLFW_KEY_BACKSLASH, 0x2B }, { GLFW_KEY_COMMA, 0x33 }, { GLFW_KEY_PERIOD, 0x34 }, { GLFW_KEY_SLASH, 0x35 }, { GLFW_KEY_RIGHT_SHIFT, 0x36 },
    { GLFW_KEY_KP_MULTIPLY, 0x37 }, { GLFW_KEY_LEFT_ALT, 0x38 }, { GLFW_KEY_SPACE, 0x39 }, { GLFW_KEY_CAPS_LOCK, 0x3A },
    { GLFW_KEY_F1, 0x3B }, { GLFW_KEY_F2, 0x3C }, { GLFW_KEY_F3, 0x3D }, { GLFW_KEY_F4, 0x3E }, { GLFW_KEY_F5, 0x3F }, { GLFW_KEY_F6, 0x40 },
    { GLFW_KEY_F7, 0x41 }, { GLFW_KEY_F8, 0x42 }, { GLFW_KEY_F9, 0x43 }, { GLFW_KEY_F10, 0x44 }, { GLFW_KEY_F11, 0x57 }, { GLFW_KEY_F12, 0x58 },
    { GLFW_KEY_NUM_LOCK, 0x45 }, { GLFW_KEY_SCROLL_LOCK, 0x46 },
    { GLFW_KEY_KP_7, 0x47 }, { GLFW_KEY_KP_8, 0x48 }, { GLFW_KEY_KP_9, 0x49 }, { GLFW_KEY_KP_SUBTRACT, 0x4A }, { GLFW_KEY_KP_4, 0x4B },
    { GLFW_KEY_KP_5, 0x4C }, { GLFW_KEY_KP_6, 0x4D }, { GLFW_KEY_KP_ADD, 0x4E }, { GLFW_KEY_KP_1, 0x4F }, { GLFW_KEY_KP_2, 0x50 },
    { GLFW_KEY_KP_3, 0x51 }, { GLFW_KEY_KP_0, 0x52 }, { GLFW_KEY_KP_DECIMAL, 0x53 },
    { GLFW_KEY_KP_ENTER, 0x80 | 0x1C }, { GLFW_KEY_RIGHT_CONTROL, 0x80 | 0x1D }, { GLFW_KEY_KP_DIVIDE, 0x80 | 0x35 }, { GLFW_KEY_RIGHT_ALT, 0x80 | 0x38 },
    { GLFW_KEY_HOME, 0x80 | 0x47 }, { GLFW_KEY_UP, 0x80 | 0x48 }, { GLFW_KEY_PAGE_UP, 0x80 | 0x49 }, { GLFW_KEY_LEFT, 0x80 | 0x4B },
    { GLFW_KEY_RIGHT, 0x80 | 0x4D }, { GLFW_KEY_END, 0x80 | 0x4F }, { GLFW_KEY_DOWN, 0x80 | 0x50 }, { GLFW_KEY_PAGE_DOWN, 0x80 | 0x51 },
    { GLFW_KEY_INSERT, 0x80 | 0x52 }, { GLFW_KEY_DELETE, 0x80 | 0x53 },
};
#define NK ((int)(sizeof k_table / sizeof k_table[0]))
static int key_to_sc(int key) {
    static const uint8_t letters[26] = { 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C };
    if (key >= 'A' && key <= 'Z') return letters[key - 'A'];
    if (key >= '1' && key <= '9') return 0x02 + (key - '1');
    if (key == '0') return 0x0B;
    for (int i = 0; i < NK; ++i) if (k_table[i].key == key) return k_table[i].sc;
    return -1;
}
static int sc_to_key(int sc) {
    for (int i = 0; i < NK; ++i) if (k_table[i].sc == sc) return k_table[i].key;
    for (int k = 'A'; k <= 'Z'; ++k) if (key_to_sc(k) == sc) return k;
    for (int k = '0'; k <= '9'; ++k) if (key_to_sc(k) == sc) return k;
    return -1;
}
static int sc_down(GLFWwindow *w, int sc) { return sc >= 0 && ((w->keys[sc >> 3] >> (sc & 7)) & 1); }
int glfwGetKey(GLFWwindow *w, int key) { return sc_down(w, key_to_sc(key)); }

void glfwPollEvents(void) {
    GLFWwindow *w = &s_win;
    cos_win_event_t ev;
    while (cos_win2_poll(w->h, &ev) > 0) {
        switch (ev.type) {
            case COS_EV_CLOSE: w->should_close = 1; break;
            case COS_EV_KEY:
                if (ev.special == COS_KEY_NONE && ev.ascii >= 32 && ev.ascii < 127 && !(ev.mods & COS_MOD_CTRL) && w->char_cb) w->char_cb(w, (unsigned)(unsigned char)ev.ascii);
                break;
            case COS_EV_MOUSE_MOVE: w->real_x = ev.x; w->real_y = ev.y; break;
            case COS_EV_MOUSE_DOWN: case COS_EV_MOUSE_UP: {
                int b = ev.button == COS_MOUSE_BTN_LEFT ? GLFW_MOUSE_BUTTON_LEFT : ev.button == COS_MOUSE_BTN_RIGHT ? GLFW_MOUSE_BUTTON_RIGHT : GLFW_MOUSE_BUTTON_MIDDLE;
                w->real_x = ev.x; w->real_y = ev.y;
                if (ev.type == COS_EV_MOUSE_DOWN) w->buttons |= 1 << b; else w->buttons &= ~(1 << b);
                if (w->mb_cb) w->mb_cb(w, b, ev.type == COS_EV_MOUSE_DOWN ? GLFW_PRESS : GLFW_RELEASE, w->mods);
                break; }
            case COS_EV_WHEEL: if (w->scroll_cb) w->scroll_cb(w, 0.0, ev.wheel > 0 ? 1.0 : -1.0); break;
            default: break;
        }
    }
    cos_win_input_t in;
    if (cos_win2_input(w->h, &in) == 0) {
        w->focused = in.focused;
        if (!in.focused) w->buttons = 0;
        if (w->cursor_mode == GLFW_CURSOR_DISABLED) { w->vx += in.dx; w->vy += in.dy; if (w->cp_cb && (in.dx || in.dy)) w->cp_cb(w, w->vx, w->vy); }
        memcpy(w->prev, w->keys, 32); memcpy(w->keys, in.keys, 32);
        int mods = (sc_down(w, 0x2A) || sc_down(w, 0x36) ? GLFW_MOD_SHIFT : 0) | (sc_down(w, 0x1D) ? GLFW_MOD_CONTROL : 0) | (sc_down(w, 0x38) ? GLFW_MOD_ALT : 0);
        w->mods = mods;
        uint64_t now = cos_time_ms();
        for (int sc = 0; sc < 256; ++sc) {
            int was = (w->prev[sc >> 3] >> (sc & 7)) & 1, is = (w->keys[sc >> 3] >> (sc & 7)) & 1;
            if (was == is) continue;
            int key = sc_to_key(sc);
            if (is && key == GLFW_KEY_F11) { toggle_fullscreen(w); continue; }
            if (is && key == GLFW_KEY_F10) {
                static const int cyc[] = { -1, 0, 2, 4, 6, 7 };
                int i = 0; for (int k = 0; k < 6; ++k) if (cyc[k] == s_level_mode) i = k;
                s_level_mode = cyc[(i + 1) % 6]; s_since = 0;
                apply_level(s_level_mode < 0 ? 0 : s_level_mode);
                update_title(w); continue;
            }
            if (key < 0 || !w->key_cb) continue;
            w->key_cb(w, key, sc, is ? GLFW_PRESS : GLFW_RELEASE, mods);
            if (key == GLFW_KEY_BACKSPACE || key == GLFW_KEY_DELETE || key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT) { w->rep_key = is ? key : 0; w->rep_at = now + 400; }
        }
        if (w->rep_key && sc_down(w, key_to_sc(w->rep_key)) && now >= w->rep_at && w->key_cb) { w->key_cb(w, w->rep_key, 0, GLFW_REPEAT, mods); w->rep_at = now + 40; }
        else if (w->rep_key && !sc_down(w, key_to_sc(w->rep_key))) w->rep_key = 0;
    }
}
