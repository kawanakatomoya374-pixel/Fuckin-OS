#ifndef GUI_RENDER_ENGINE_H
#define GUI_RENDER_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

int gui_render_init(void);
void gui_render_shutdown(void);
void gui_render_set_clip(int x, int y, int w, int h);
void gui_render_reset_clip(void);
void gui_render_fill_rect(int x, int y, int w, int h, uint64_t color);
void gui_render_draw_rect(int x, int y, int w, int h, uint64_t color);
void gui_render_draw_rounded_rect(int x, int y, int w, int h, int radius, uint64_t color);
void gui_render_fill_rounded_rect(int x, int y, int w, int h, int radius, uint64_t color);
void gui_render_draw_line(int x0, int y0, int x1, int y1, uint64_t color);
void gui_render_draw_circle(int cx, int cy, int radius, uint64_t color, bool filled);
void gui_render_draw_text(int x, int y, const char* text, uint64_t fg, uint64_t bg);

#endif
