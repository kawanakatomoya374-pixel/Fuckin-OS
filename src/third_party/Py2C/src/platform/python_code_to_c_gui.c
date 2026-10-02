#include "platform/python_code_to_c_gui.h"

static P2C_GuiCommand *push(P2C_GuiContext *ctx, P2C_GuiCommandType type, P2C_GuiColor color) {
    if (!ctx || ctx->command_count >= ctx->command_capacity) {
        if (ctx) ctx->dropped_commands++;
        return NULL;
    }
    P2C_GuiCommand *command = &ctx->commands[ctx->command_count++];
    memset(command, 0, sizeof(*command));
    command->type = type;
    command->color = color;
    return command;
}

void p2c_gui_init(P2C_GuiContext *ctx, P2C_GuiCommand *commands, size_t command_capacity,
                  char *text_arena, size_t text_capacity) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->commands = commands;
    ctx->command_capacity = command_capacity;
    ctx->text_arena = text_arena;
    ctx->text_capacity = text_capacity;
}

void p2c_gui_begin(P2C_GuiContext *ctx, uint32_t width, uint32_t height, P2C_GuiColor background) {
    (void)width; (void)height;
    if (!ctx) return;
    ctx->command_count = 0;
    ctx->text_used = 0;
    ctx->dropped_commands = 0;
    ctx->frame_number++;
    p2c_gui_clear(ctx, background);
}

void p2c_gui_clear(P2C_GuiContext *ctx, P2C_GuiColor color) {
    (void)push(ctx, P2C_GUI_CLEAR, color);
}

void p2c_gui_fill_rect(P2C_GuiContext *ctx, P2C_GuiRect rect, P2C_GuiColor color) {
    P2C_GuiCommand *command = push(ctx, P2C_GUI_FILL_RECT, color);
    if (command) command->data.rect = rect;
}

void p2c_gui_stroke_rect(P2C_GuiContext *ctx, P2C_GuiRect rect, P2C_GuiColor color) {
    P2C_GuiCommand *command = push(ctx, P2C_GUI_STROKE_RECT, color);
    if (command) command->data.rect = rect;
}

void p2c_gui_line(P2C_GuiContext *ctx, int32_t x1, int32_t y1, int32_t x2, int32_t y2, P2C_GuiColor color) {
    P2C_GuiCommand *command = push(ctx, P2C_GUI_LINE, color);
    if (command) {
        command->data.line.x1 = x1; command->data.line.y1 = y1;
        command->data.line.x2 = x2; command->data.line.y2 = y2;
    }
}

void p2c_gui_text(P2C_GuiContext *ctx, int32_t x, int32_t y, uint16_t size,
                  P2C_GuiColor color, const char *text) {
    if (!ctx || !text || !ctx->text_arena) return;
    size_t len = strlen(text);
    if (len > 65535) len = 65535;
    if (len > ctx->text_capacity - ctx->text_used) {
        ctx->dropped_commands++;
        return;
    }
    P2C_GuiCommand *command = push(ctx, P2C_GUI_TEXT, color);
    if (!command) return;
    uint32_t offset = (uint32_t)ctx->text_used;
    memcpy(ctx->text_arena + ctx->text_used, text, len);
    ctx->text_used += len;
    command->data.text.x = x;
    command->data.text.y = y;
    command->data.text.size = size;
    command->data.text.text_offset = offset;
    command->data.text.text_len = (uint16_t)len;
}

const char *p2c_gui_text_at(const P2C_GuiContext *ctx, const P2C_GuiCommand *command, size_t *len) {
    if (len) *len = 0;
    if (!ctx || !command || command->type != P2C_GUI_TEXT ||
        command->data.text.text_offset > ctx->text_used ||
        command->data.text.text_len > ctx->text_used - command->data.text.text_offset) return NULL;
    if (len) *len = command->data.text.text_len;
    return ctx->text_arena + command->data.text.text_offset;
}

void p2c_gui_end(P2C_GuiContext *ctx, const P2C_GuiBackend *backend, uint32_t width, uint32_t height) {
    if (!ctx || !backend || !backend->draw_command) return;
    if (backend->begin_frame) backend->begin_frame(width, height, backend->user);
    for (size_t i = 0; i < ctx->command_count; i++) {
        const P2C_GuiCommand *command = &ctx->commands[i];
        size_t text_len = 0;
        const char *text = p2c_gui_text_at(ctx, command, &text_len);
        backend->draw_command(command, text, text_len, backend->user);
    }
    if (backend->end_frame) backend->end_frame(backend->user);
}
