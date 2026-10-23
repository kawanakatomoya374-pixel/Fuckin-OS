/* glfw_cos.h - the part of GLFW that Craft uses, implemented on the C-OS window API (cos_win2_*), including the pointer
 * lock (cos_win2_grab) that makes GLFW_CURSOR_DISABLED work: the cursor disappears and the mouse turns the camera. */
#ifndef GLFW_COS_H
#define GLFW_COS_H
#include "gl_cos.h"

typedef struct GLFWwindow GLFWwindow;
typedef struct GLFWmonitor GLFWmonitor;
typedef void (*GLFWkeyfun)(GLFWwindow *, int, int, int, int);
typedef void (*GLFWcharfun)(GLFWwindow *, unsigned int);
typedef void (*GLFWmousebuttonfun)(GLFWwindow *, int, int, int);
typedef void (*GLFWscrollfun)(GLFWwindow *, double, double);

#define GLFW_RELEASE 0
#define GLFW_PRESS 1
#define GLFW_REPEAT 2
#define GLFW_MOD_SHIFT 1
#define GLFW_MOD_CONTROL 2
#define GLFW_MOD_ALT 4
#define GLFW_MOD_SUPER 8
#define GLFW_MOUSE_BUTTON_LEFT 0
#define GLFW_MOUSE_BUTTON_RIGHT 1
#define GLFW_MOUSE_BUTTON_MIDDLE 2
#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_DISABLED 0x00034003
#define GLFW_KEY_SPACE 32
#define GLFW_KEY_ESCAPE 256
#define GLFW_KEY_ENTER 257
#define GLFW_KEY_TAB 258
#define GLFW_KEY_BACKSPACE 259
#define GLFW_KEY_RIGHT 262
#define GLFW_KEY_LEFT 263
#define GLFW_KEY_DOWN 264
#define GLFW_KEY_UP 265
#define GLFW_KEY_F12 301
#define GLFW_KEY_LEFT_SHIFT 340
#define GLFW_KEY_LEFT_CONTROL 341

int glfwInit(void);
void glfwTerminate(void);
GLFWwindow *glfwCreateWindow(int width, int height, const char *title, GLFWmonitor *monitor, GLFWwindow *share);
void glfwMakeContextCurrent(GLFWwindow *w);
void glfwSwapInterval(int n);
void glfwSwapBuffers(GLFWwindow *w);
void glfwPollEvents(void);
int glfwWindowShouldClose(GLFWwindow *w);
void glfwSetWindowShouldClose(GLFWwindow *w, int v);
void glfwSetInputMode(GLFWwindow *w, int mode, int value);
int glfwGetInputMode(GLFWwindow *w, int mode);
int glfwGetKey(GLFWwindow *w, int key);
void glfwGetCursorPos(GLFWwindow *w, double *x, double *y);
void glfwGetWindowSize(GLFWwindow *w, int *width, int *height);
void glfwGetFramebufferSize(GLFWwindow *w, int *width, int *height);
void glfwSetKeyCallback(GLFWwindow *w, GLFWkeyfun f);
void glfwSetCharCallback(GLFWwindow *w, GLFWcharfun f);
void glfwSetMouseButtonCallback(GLFWwindow *w, GLFWmousebuttonfun f);
void glfwSetScrollCallback(GLFWwindow *w, GLFWscrollfun f);
const char *glfwGetClipboardString(GLFWwindow *w);
void glfwSetTime(double t);

/* C-OS additions */
void plat_set_title(const char *t);
void plat_blit_scene(void);                       /* copy the 3-D picture into the window now; 2-D overlays are then drawn on top at full resolution */
int  plat_toggle_fullscreen(void);                /* F12: real full screen on/off; returns 1 if fullscreen now */
void plat_client_cursor(int *x, int *y);          /* the real pointer position in window pixels (valid while the cursor is not locked) */
void plat_set_mouse_scale(float s);               /* camera speed multiplier (1 = default) */
float plat_get_mouse_scale(void);
int  plat_focused(void);
void *plat_pixels(int *stride, int *w, int *h);   /* the window's own pixel buffer */
void plat_present_window(void);
#endif
