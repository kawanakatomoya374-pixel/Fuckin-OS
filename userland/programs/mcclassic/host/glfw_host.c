/* glfw_host.c - a headless GLFW for building mc-classic-c on a Linux host (tools/mc_hostbench.sh): no window, a fixed number of frames
 * (MC_FRAMES), the last frame is written as PPM (MC_PPM) and the time per frame is printed. Used to profile mcgl.c with the real game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLFW/glfw3.h>
#include "../mcgl.h"

struct GLFWwindow { int w, h, frames, max_frames; uint32_t *px; double t0, tlast; GLFWkeyfun key_cb; GLFWcharfun char_cb; GLFWmousebuttonfun mb_cb; GLFWscrollfun scroll_cb; GLFWcursorposfun cp_cb; int cursor_mode; };
static GLFWwindow s_win; static GLFWvidmode s_mode = { 1024, 768, 8, 8, 8, 60 };
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
double glfwGetTime(void) { return now(); }
void glfwSetTime(double t) { (void)t; }
int glfwInit(void) { return 1; }
void glfwWindowHint(int h, int v) { (void)h; (void)v; }
void glfwDestroyWindow(GLFWwindow *w) { (void)w; }
void glfwTerminate(void) {}
GLFWmonitor *glfwGetPrimaryMonitor(void) { return (GLFWmonitor *)1; }
const GLFWvidmode *glfwGetVideoMode(GLFWmonitor *m) { (void)m; return &s_mode; }
GLFWwindow *glfwCreateWindow(int w, int h, const char *t, GLFWmonitor *m, GLFWwindow *s) {
    (void)t; (void)m; (void)s;
    if (getenv("MC_W")) w = atoi(getenv("MC_W")); if (getenv("MC_H")) h = atoi(getenv("MC_H"));
    memset(&s_win, 0, sizeof s_win); s_win.w = w; s_win.h = h; s_win.px = calloc((size_t)w * h, 4);
    s_win.max_frames = getenv("MC_FRAMES") ? atoi(getenv("MC_FRAMES")) : 120; s_win.t0 = s_win.tlast = now();
    mcgl_resize(w, h); { extern void mcgl_set_fovea(int); if (getenv("MC_FOVEA")) mcgl_set_fovea(atoi(getenv("MC_FOVEA"))); } { extern void mcgl_set_far_cap(float); if (getenv("MC_CAP")) mcgl_set_far_cap((float)atof(getenv("MC_CAP"))); } return &s_win;
}
void glfwMakeContextCurrent(GLFWwindow *w) { (void)w; }
void glfwSwapInterval(int n) { (void)n; }
void glfwSwapBuffers(GLFWwindow *w) {
    mcgl_present(w->px, w->w, w->w, w->h);
    w->frames++;
    if (w->frames % 30 == 0) { double t = now(); fprintf(stderr, "frames %d: %.1f ms/frame (%.1f fps)\n", w->frames, (t - w->tlast) * 1000 / 30, 30 / (t - w->tlast)); w->tlast = t; }
    if (w->frames == w->max_frames) {
        const char *p = getenv("MC_PPM");
        if (p) { FILE *f = fopen(p, "wb"); if (f) { fprintf(f, "P6\n%d %d\n255\n", w->w, w->h); for (int i = 0; i < w->w * w->h; ++i) { uint32_t c = w->px[i]; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); } fclose(f); } }
        fprintf(stderr, "average %.2f ms/frame\n", (now() - s_win.t0) * 1000 / w->frames);
    }
}
int glfwWindowShouldClose(GLFWwindow *w) { return w->frames >= w->max_frames; }
void glfwSetWindowShouldClose(GLFWwindow *w, int v) { if (v) w->frames = w->max_frames; }
void glfwSetWindowTitle(GLFWwindow *w, const char *t) { (void)w; (void)t; }
int glfwGetWindowAttrib(GLFWwindow *w, int a) { (void)w; return a == GLFW_FOCUSED; }
void glfwSetInputMode(GLFWwindow *w, int m, int v) { if (m == GLFW_CURSOR) w->cursor_mode = v; }
int glfwGetInputMode(GLFWwindow *w, int m) { return m == GLFW_CURSOR ? w->cursor_mode : 0; }
void glfwGetCursorPos(GLFWwindow *w, double *x, double *y) { *x = w->w * 0.5; *y = w->h * 0.5; }
void glfwSetCursorPos(GLFWwindow *w, double x, double y) { (void)w; (void)x; (void)y; }
int glfwGetMouseButton(GLFWwindow *w, int b) { (void)w; (void)b; return 0; }
void glfwGetWindowSize(GLFWwindow *w, int *a, int *b) { *a = w->w; *b = w->h; }
void glfwGetFramebufferSize(GLFWwindow *w, int *a, int *b) { (void)w; *a = mcgl_width(); *b = mcgl_height(); }
GLFWkeyfun glfwSetKeyCallback(GLFWwindow *w, GLFWkeyfun f) { GLFWkeyfun o = w->key_cb; w->key_cb = f; return o; }
GLFWcharfun glfwSetCharCallback(GLFWwindow *w, GLFWcharfun f) { GLFWcharfun o = w->char_cb; w->char_cb = f; return o; }
GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow *w, GLFWmousebuttonfun f) { GLFWmousebuttonfun o = w->mb_cb; w->mb_cb = f; return o; }
GLFWscrollfun glfwSetScrollCallback(GLFWwindow *w, GLFWscrollfun f) { GLFWscrollfun o = w->scroll_cb; w->scroll_cb = f; return o; }
GLFWcursorposfun glfwSetCursorPosCallback(GLFWwindow *w, GLFWcursorposfun f) { GLFWcursorposfun o = w->cp_cb; w->cp_cb = f; return o; }
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow *w, GLFWframebuffersizefun f) { (void)w; (void)f; return 0; }
int glfwGetKey(GLFWwindow *w, int key) { (void)w; (void)key; return 0; }
void glfwPollEvents(void) {}
int AudioBackend_init(void *fill) { (void)fill; return -1; }
void AudioBackend_shutdown(void) {}
