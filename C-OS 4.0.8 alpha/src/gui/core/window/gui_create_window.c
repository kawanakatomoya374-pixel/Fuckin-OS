#include "gui.h"

window_t* gui_create_window(int x, int y, int w, int h, const char* title) {
    return gui_open_window(WIN_NONE, title ? title : "Window", x, y, w, h);
}
