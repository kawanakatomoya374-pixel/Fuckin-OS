#ifndef PYTHON_CODE_TO_C_GUI_H
#define PYTHON_CODE_TO_C_GUI_H

#include "common/python_code_to_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend-neutral retained command buffer. The OS may consume commands in
 * order and map them to a framebuffer, GPU, serial display, or browser bridge. */
typedef struct { int32_t x, y, w, h; } P2C_GuiRect;
typedef struct { uint8_t r, g, b, a; } P2C_GuiColor;

typedef enum {
    P2C_GUI_CLEAR = 1,
    P2C_GUI_FILL_RECT,
    P2C_GUI_STROKE_RECT,
    P2C_GUI_LINE,
    P2C_GUI_TEXT
} P2C_GuiCommandType;

typedef struct {
    P2C_GuiCommandType type;
    P2C_GuiColor color;
    union {
        P2C_GuiRect rect;
        struct { int32_t x1, y1, x2, y2; } line;
        struct { int32_t x, y; uint16_t size; uint32_t text_offset; uint16_t text_len; } text;
    } data;
} P2C_GuiCommand;

typedef struct {
    P2C_GuiCommand *commands;
    size_t command_capacity;
    size_t command_count;
    char *text_arena;
    size_t text_capacity;
    size_t text_used;
    uint32_t frame_number;
    uint32_t dropped_commands;
} P2C_GuiContext;

typedef struct {
    void (*begin_frame)(uint32_t width, uint32_t height, void *user);
    void (*draw_command)(const P2C_GuiCommand *command, const char *text, size_t text_len, void *user);
    void (*end_frame)(void *user);
    void *user;
} P2C_GuiBackend;

void p2c_gui_init(P2C_GuiContext *ctx, P2C_GuiCommand *commands, size_t command_capacity,
                  char *text_arena, size_t text_capacity);
void p2c_gui_begin(P2C_GuiContext *ctx, uint32_t width, uint32_t height, P2C_GuiColor background);
void p2c_gui_clear(P2C_GuiContext *ctx, P2C_GuiColor color);
void p2c_gui_fill_rect(P2C_GuiContext *ctx, P2C_GuiRect rect, P2C_GuiColor color);
void p2c_gui_stroke_rect(P2C_GuiContext *ctx, P2C_GuiRect rect, P2C_GuiColor color);
void p2c_gui_line(P2C_GuiContext *ctx, int32_t x1, int32_t y1, int32_t x2, int32_t y2, P2C_GuiColor color);
void p2c_gui_text(P2C_GuiContext *ctx, int32_t x, int32_t y, uint16_t size, P2C_GuiColor color, const char *text);
void p2c_gui_end(P2C_GuiContext *ctx, const P2C_GuiBackend *backend, uint32_t width, uint32_t height);
const char *p2c_gui_text_at(const P2C_GuiContext *ctx, const P2C_GuiCommand *command, size_t *len);

#ifdef __cplusplus
}
#endif
#endif
