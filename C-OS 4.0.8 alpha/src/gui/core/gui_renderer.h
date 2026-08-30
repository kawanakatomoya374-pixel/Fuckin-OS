#ifndef GUI_RENDERER_H
#define GUI_RENDERER_H

#include "cos_api.h"

/* GUI Renderer - Enhanced Graphics Rendering System
 * Provides optimized drawing routines with double buffering
 */

#include "vga.h"

/* Screen dimensions */
#define SCREEN_WIDTH    ((uint64_t)SCREEN_W)
#define SCREEN_HEIGHT   ((uint64_t)SCREEN_H)
#define SCREEN_SIZE      (SCREEN_WIDTH * SCREEN_HEIGHT)

/* Rendering modes */
#define RENDER_MODE_SOFTWARE    0
#define RENDER_MODE_HARDWARE    1
#define RENDER_MODE_DOUBLE_BUF  2

/* Color utilities */
#define RGB(r, g, b) ((r << 16) | (g << 8) | b)
#define RGB565_TO_RGB888(rgb565) \
    (((rgb565 & 0xF800) >> 8) | ((rgb565 & 0x07E0) << 5) | ((rgb565 & 0x001F) << 3))

/* GUI Renderer API functions */
int gui_renderer_init(void);
int gui_renderer_cleanup(void);
void gui_renderer_set_clip(uint64_t x, uint64_t y, uint64_t w, uint64_t h);
void gui_renderer_reset_clip(void);
void gui_renderer_draw_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint64_t color, bool filled);
void gui_renderer_draw_gradient_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint64_t color_top, uint64_t color_bottom);
void gui_renderer_draw_circle(uint64_t cx, uint64_t cy, uint64_t radius, uint64_t color, bool filled);
void gui_renderer_clear_screen(uint64_t color);
void gui_renderer_swap_buffers(void);
void gui_renderer_present(void);
void gui_renderer_get_stats(uint64_t* fps, uint64_t* frame_count);
void gui_renderer_set_vsync(bool enabled);
void gui_renderer_set_mode(uint8_t mode);

/* Module interface declaration */
extern module_interface_t gui_renderer_module;

#endif
