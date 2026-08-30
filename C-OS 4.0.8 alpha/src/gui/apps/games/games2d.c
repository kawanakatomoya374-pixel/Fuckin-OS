/*
 * games2d.c - C-OS 2DGAMES
 *
 * A compact, self-contained collection of Minesweeper, Snake and Tetris.
 * Each game uses the native C-OS GUI primitives and keyboard event stream;
 * no hosted runtime or external game engine is required.
 */
#include "gui.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "string.h"

#define MINE_W 9
#define MINE_H 9
#define MINE_COUNT 12
#define SNAKE_MAX 128
#define SNAKE_W 20
#define SNAKE_H 16
#define TET_W 10
#define TET_H 20

typedef enum { G2D_MENU = 0, G2D_MINES, G2D_SNAKE, G2D_TETRIS } games2d_mode_t;
typedef struct { int x, y; } g2d_point_t;

static games2d_mode_t s_mode = G2D_MENU;
static uint32_t s_rng = 0xC05A2026u;

static bool s_mine[MINE_H][MINE_W];
static bool s_revealed[MINE_H][MINE_W];
static bool s_flagged[MINE_H][MINE_W];
static int s_mx, s_my, s_mines_lost, s_mines_won;

static g2d_point_t s_snake[SNAKE_MAX];
static int s_snake_len, s_sdx, s_sdy, s_food_x, s_food_y;
static bool s_snake_over;
static uint64_t s_snake_last_tick;

static uint8_t s_tet[TET_H][TET_W];
static int s_piece, s_rot, s_px, s_py, s_tet_score;
static bool s_tet_over;
static uint64_t s_tet_last_tick;

static const uint16_t s_tet_shape[7][4] = {
    { 0x00F0, 0x2222, 0x00F0, 0x2222 }, /* I */
    { 0x0660, 0x0660, 0x0660, 0x0660 }, /* O */
    { 0x0270, 0x0262, 0x0072, 0x0232 }, /* T */
    { 0x0360, 0x0462, 0x0036, 0x0231 }, /* L */
    { 0x0630, 0x0264, 0x0063, 0x0132 }, /* J */
    { 0x0360, 0x0462, 0x0036, 0x0231 }, /* S */
    { 0x0630, 0x0264, 0x0063, 0x0132 }, /* Z */
};
static const uint64_t s_tet_color[8] = {
    0, 0x00E5E7EB, 0x00FACC15, 0x00C084FC,
    0x0060A5FA, 0x0034D399, 0x00F87171, 0x00FB923C
};

static uint32_t games2d_rand(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static void mines_reset(void) {
    memset(s_mine, 0, sizeof(s_mine));
    memset(s_revealed, 0, sizeof(s_revealed));
    memset(s_flagged, 0, sizeof(s_flagged));
    s_mx = s_my = 0; s_mines_lost = 0; s_mines_won = 0;
    int placed = 0;
    while (placed < MINE_COUNT) {
        int x = (int)(games2d_rand() % MINE_W);
        int y = (int)(games2d_rand() % MINE_H);
        if (!s_mine[y][x]) { s_mine[y][x] = true; placed++; }
    }
}

static int mines_neighbours(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
        int nx = x + dx, ny = y + dy;
        if (nx >= 0 && nx < MINE_W && ny >= 0 && ny < MINE_H && s_mine[ny][nx]) n++;
    }
    return n;
}

static void mines_reveal(int x, int y) {
    if (x < 0 || x >= MINE_W || y < 0 || y >= MINE_H || s_revealed[y][x] || s_flagged[y][x]) return;
    s_revealed[y][x] = true;
    if (s_mine[y][x]) { s_mines_lost = 1; return; }
    if (mines_neighbours(x, y) == 0) {
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            if (dx || dy) mines_reveal(x + dx, y + dy);
        }
    }
    int hidden = 0;
    for (int yy = 0; yy < MINE_H; ++yy) for (int xx = 0; xx < MINE_W; ++xx)
        if (!s_mine[yy][xx] && !s_revealed[yy][xx]) hidden++;
    if (hidden == 0) s_mines_won = 1;
}

static void snake_reset(void) {
    s_snake_len = 4; s_sdx = 1; s_sdy = 0; s_snake_over = false;
    for (int i = 0; i < s_snake_len; ++i) { s_snake[i].x = 9 - i; s_snake[i].y = 8; }
    s_food_x = 14; s_food_y = 8; s_snake_last_tick = get_timer_ticks();
}

static bool snake_contains(int x, int y) {
    for (int i = 0; i < s_snake_len; ++i) if (s_snake[i].x == x && s_snake[i].y == y) return true;
    return false;
}

static void snake_place_food(void) {
    for (int tries = 0; tries < 200; ++tries) {
        int x = (int)(games2d_rand() % SNAKE_W), y = (int)(games2d_rand() % SNAKE_H);
        if (!snake_contains(x, y)) { s_food_x = x; s_food_y = y; return; }
    }
}

static void snake_step(void) {
    if (s_snake_over) return;
    int nx = s_snake[0].x + s_sdx, ny = s_snake[0].y + s_sdy;
    if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H || snake_contains(nx, ny)) { s_snake_over = true; return; }
    for (int i = s_snake_len; i > 0; --i) s_snake[i] = s_snake[i - 1];
    s_snake[0].x = nx; s_snake[0].y = ny;
    if (nx == s_food_x && ny == s_food_y) {
        if (s_snake_len < SNAKE_MAX - 1) s_snake_len++;
        snake_place_food();
    }
}

static bool tet_cell(int piece, int rot, int x, int y) {
    return (s_tet_shape[piece][rot & 3] & (1u << (y * 4 + x))) != 0;
}

static bool tet_collides(int piece, int rot, int px, int py) {
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if (tet_cell(piece, rot, x, y)) {
        int gx = px + x, gy = py + y;
        if (gx < 0 || gx >= TET_W || gy >= TET_H) return true;
        if (gy >= 0 && s_tet[gy][gx]) return true;
    }
    return false;
}

static void tet_spawn(void) {
    s_piece = (int)(games2d_rand() % 7); s_rot = 0; s_px = 3; s_py = -1;
    if (tet_collides(s_piece, s_rot, s_px, s_py)) s_tet_over = true;
}

static void tet_reset(void) {
    memset(s_tet, 0, sizeof(s_tet)); s_tet_score = 0; s_tet_over = false;
    s_tet_last_tick = get_timer_ticks(); tet_spawn();
}

static void tet_lock(void) {
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if (tet_cell(s_piece, s_rot, x, y)) {
        int gx = s_px + x, gy = s_py + y;
        if (gy >= 0 && gy < TET_H && gx >= 0 && gx < TET_W) s_tet[gy][gx] = (uint8_t)(s_piece + 1);
    }
    int lines = 0;
    for (int y = TET_H - 1; y >= 0; --y) {
        int full = 1; for (int x = 0; x < TET_W; ++x) if (!s_tet[y][x]) { full = 0; break; }
        if (full) { for (int yy = y; yy > 0; --yy) memcpy(s_tet[yy], s_tet[yy - 1], TET_W); memset(s_tet[0], 0, TET_W); y++; lines++; }
    }
    s_tet_score += lines ? lines * lines * 100 : 10; tet_spawn();
}

static void tet_step(void) {
    if (s_tet_over) return;
    if (!tet_collides(s_piece, s_rot, s_px, s_py + 1)) s_py++;
    else tet_lock();
}

static void draw_button(int x, int y, int w, int h, const char* text, uint64_t color) {
    vga_fill_rounded_rect(x, y, w, h, 9, color);
    vga_draw_rounded_rect(x, y, w, h, 9, rgb(226, 232, 240));
    vga_draw_string(x + 16, y + (h - FONT_H) / 2, text, rgb(255, 255, 255), 0);
}

static void draw_menu(window_t* w) {
    int x = w->x + 34, y = w->y + 58;
    vga_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H, rgb(15, 23, 42));
    vga_draw_string(x, y, "2DGAMES", rgb(248, 250, 252), 0);
    vga_draw_string(x, y + 30, "Classic games built directly into C-OS", rgb(148, 163, 184), 0);
    draw_button(x, y + 70, 280, 44, "1  Minesweeper", rgb(37, 99, 235));
    draw_button(x, y + 128, 280, 44, "2  Snake", rgb(5, 150, 105));
    draw_button(x, y + 186, 280, 44, "3  Tetris", rgb(147, 51, 234));
    vga_draw_string(x, y + 254, "Click a game or press 1 / 2 / 3", rgb(203, 213, 225), 0);
}

static void draw_mines(window_t* w) {
    int ox = w->x + 44, oy = w->y + 78, cell = 30;
    vga_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H, rgb(16, 27, 45));
    vga_draw_string(ox, oy - 46, "Minesweeper  Arrow: move  Space: reveal  F: flag  R: reset  Esc: menu", rgb(226, 232, 240), 0);
    for (int y = 0; y < MINE_H; ++y) for (int x = 0; x < MINE_W; ++x) {
        uint64_t bg = s_revealed[y][x] ? rgb(226, 232, 240) : rgb(71, 85, 105);
        vga_fill_rect(ox + x * cell, oy + y * cell, cell - 2, cell - 2, bg);
        if (x == s_mx && y == s_my) vga_draw_rect(ox + x * cell - 1, oy + y * cell - 1, cell, cell, rgb(250, 204, 21));
        if (s_revealed[y][x] && s_mine[y][x]) vga_draw_string(ox + x * cell + 9, oy + y * cell + 8, "*", rgb(239, 68, 68), 0);
        else if (s_flagged[y][x]) vga_draw_string(ox + x * cell + 9, oy + y * cell + 8, "F", rgb(251, 191, 36), 0);
        else if (s_revealed[y][x]) { int n = mines_neighbours(x, y); if (n) { char b[2] = {(char)('0' + n), 0}; vga_draw_string(ox + x * cell + 10, oy + y * cell + 8, b, rgb(37, 99, 235), 0); } }
    }
    if (s_mines_lost) vga_draw_string(ox, oy + MINE_H * cell + 16, "Mine hit. Press R to restart.", rgb(248, 113, 113), 0);
    if (s_mines_won) vga_draw_string(ox, oy + MINE_H * cell + 16, "Board cleared. Press R to play again.", rgb(74, 222, 128), 0);
}

static void draw_snake(window_t* w) {
    int ox = w->x + 38, oy = w->y + 74, cell = 20;
    vga_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H, rgb(11, 29, 25));
    vga_draw_string(ox, oy - 44, "Snake  Arrow: steer  R: reset  Esc: menu", rgb(209, 250, 229), 0);
    vga_draw_rect(ox - 2, oy - 2, SNAKE_W * cell + 4, SNAKE_H * cell + 4, rgb(52, 211, 153));
    for (int i = 0; i < s_snake_len; ++i) vga_fill_rect(ox + s_snake[i].x * cell + 2, oy + s_snake[i].y * cell + 2, cell - 3, cell - 3, i ? rgb(52, 211, 153) : rgb(16, 185, 129));
    vga_fill_rect(ox + s_food_x * cell + 4, oy + s_food_y * cell + 4, cell - 7, cell - 7, rgb(248, 113, 113));
    char b[48]; snprintf(b, sizeof(b), "Length: %d", s_snake_len); vga_draw_string(ox, oy + SNAKE_H * cell + 16, b, rgb(167, 243, 208), 0);
    if (s_snake_over) vga_draw_string(ox + 140, oy + SNAKE_H * cell + 16, "Game over - R to restart", rgb(248, 113, 113), 0);
}

static void draw_tetris(window_t* w) {
    int ox = w->x + 70, oy = w->y + 64, cell = 22;
    vga_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h - TITLEBAR_H, rgb(29, 15, 48));
    vga_draw_string(ox, oy - 38, "Tetris  Arrow: move/rotate  Space: drop  R: reset  Esc: menu", rgb(233, 213, 255), 0);
    vga_draw_rect(ox - 2, oy - 2, TET_W * cell + 4, TET_H * cell + 4, rgb(192, 132, 252));
    for (int y = 0; y < TET_H; ++y) for (int x = 0; x < TET_W; ++x) if (s_tet[y][x]) vga_fill_rect(ox + x * cell + 1, oy + y * cell + 1, cell - 2, cell - 2, s_tet_color[s_tet[y][x]]);
    if (!s_tet_over) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if (tet_cell(s_piece, s_rot, x, y) && s_py + y >= 0) vga_fill_rect(ox + (s_px + x) * cell + 1, oy + (s_py + y) * cell + 1, cell - 2, cell - 2, s_tet_color[s_piece + 1]);
    char b[48]; snprintf(b, sizeof(b), "Score: %d", s_tet_score); vga_draw_string(ox + TET_W * cell + 26, oy + 8, b, rgb(233, 213, 255), 0);
    if (s_tet_over) vga_draw_string(ox + TET_W * cell + 26, oy + 40, "Game over", rgb(248, 113, 113), 0);
}

void games2d_draw(int idx) {
    window_t* w = &windows[idx];
    uint64_t now = get_timer_ticks();
    if (s_mode == G2D_SNAKE && now - s_snake_last_tick >= 180) { snake_step(); s_snake_last_tick = now; }
    if (s_mode == G2D_TETRIS && now - s_tet_last_tick >= 420) { tet_step(); s_tet_last_tick = now; }
    if (s_mode == G2D_MENU) draw_menu(w);
    else if (s_mode == G2D_MINES) draw_mines(w);
    else if (s_mode == G2D_SNAKE) draw_snake(w);
    else draw_tetris(w);
}

void games2d_handle_key(int idx, const keyboard_event_t* ev) {
    (void)idx; if (!ev || !ev->pressed) return;
    if (ev->scancode == 0x01) { s_mode = G2D_MENU; return; } /* Esc */
    if (s_mode == G2D_MENU) {
        if (ev->ascii == '1') { mines_reset(); s_mode = G2D_MINES; }
        else if (ev->ascii == '2') { snake_reset(); s_mode = G2D_SNAKE; }
        else if (ev->ascii == '3') { tet_reset(); s_mode = G2D_TETRIS; }
        return;
    }
    if (s_mode == G2D_MINES) {
        if (ev->scancode == 0x48 && s_my > 0) s_my--; else if (ev->scancode == 0x50 && s_my < MINE_H - 1) s_my++;
        else if (ev->scancode == 0x4B && s_mx > 0) s_mx--; else if (ev->scancode == 0x4D && s_mx < MINE_W - 1) s_mx++;
        else if (ev->scancode == 0x39) mines_reveal(s_mx, s_my);
        else if (ev->scancode == 0x21 && !s_revealed[s_my][s_mx]) s_flagged[s_my][s_mx] = !s_flagged[s_my][s_mx];
        else if (ev->scancode == 0x13) mines_reset();
    } else if (s_mode == G2D_SNAKE) {
        if (ev->scancode == 0x48 && s_sdy == 0) { s_sdx = 0; s_sdy = -1; }
        else if (ev->scancode == 0x50 && s_sdy == 0) { s_sdx = 0; s_sdy = 1; }
        else if (ev->scancode == 0x4B && s_sdx == 0) { s_sdx = -1; s_sdy = 0; }
        else if (ev->scancode == 0x4D && s_sdx == 0) { s_sdx = 1; s_sdy = 0; }
        else if (ev->scancode == 0x13) snake_reset();
    } else if (s_mode == G2D_TETRIS) {
        if (ev->scancode == 0x4B && !tet_collides(s_piece, s_rot, s_px - 1, s_py)) s_px--;
        else if (ev->scancode == 0x4D && !tet_collides(s_piece, s_rot, s_px + 1, s_py)) s_px++;
        else if (ev->scancode == 0x50) tet_step();
        else if (ev->scancode == 0x48 && !tet_collides(s_piece, s_rot + 1, s_px, s_py)) s_rot = (s_rot + 1) & 3;
        else if (ev->scancode == 0x39) while (!tet_collides(s_piece, s_rot, s_px, s_py + 1)) s_py++; 
        else if (ev->scancode == 0x13) tet_reset();
    }
}

void games2d_handle_click(int idx, int mx, int my) {
    window_t* w = &windows[idx];
    if (s_mode == G2D_MENU) {
        int y = w->y + 128;
        if (my >= y && my < y + 44) { mines_reset(); s_mode = G2D_MINES; }
        else if (my >= y + 58 && my < y + 102) { snake_reset(); s_mode = G2D_SNAKE; }
        else if (my >= y + 116 && my < y + 160) { tet_reset(); s_mode = G2D_TETRIS; }
    } else if (s_mode == G2D_MINES) {
        int ox = w->x + 44, oy = w->y + 78, cell = 30;
        int x = (mx - ox) / cell, y = (my - oy) / cell;
        if (x >= 0 && x < MINE_W && y >= 0 && y < MINE_H) { s_mx = x; s_my = y; mines_reveal(x, y); }
    }
}
