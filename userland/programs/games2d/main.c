/* games2d.c-os - Minesweeper, Snake and Tetris as a ring-3 program (they used to be drawn by the kernel).
 * The game logic is unchanged; drawing uses cos_ui and input arrives as window events. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

#define RGB(r, g, b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

static cui_canvas c;
static cui_font *f_s, *f_m, *f_b, *f_h;
static int win_w, win_h;

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
static const uint32_t s_tet_color[8] = {
    0, 0xE5E7EB, 0xFACC15, 0xC084FC,
    0x60A5FA, 0x34D399, 0xF87171, 0xFB923C
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
    s_food_x = 14; s_food_y = 8; s_snake_last_tick = cos_time_ms();
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
    s_tet_last_tick = cos_time_ms(); tet_spawn();
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


static void text(int x, int y, const char *s, uint32_t col) { cui_text(&c, f_m, x, y, s, col); }

static void draw_button(int x, int y, int w, int h, const char *label, uint32_t color) {
    cui_round_rect(&c, x, y, w, h, 9, color);
    cui_round_rect_outline(&c, x, y, w, h, 9, 1.5f, RGB(226, 232, 240));
    cui_text(&c, f_b, x + 16, y + (h - cui_font_height(f_b)) / 2, label, RGB(255, 255, 255));
}

static void draw_menu(void) {
    int x = 34, y = 40;
    cui_fill(&c, 0, 0, win_w, win_h, RGB(15, 23, 42));
    cui_text(&c, f_h, x, y, "2D Games", RGB(248, 250, 252));
    text(x, y + 38, "Classic games for C-OS", RGB(148, 163, 184));
    draw_button(x, y + 80, 300, 48, "1  Minesweeper", RGB(37, 99, 235));
    draw_button(x, y + 140, 300, 48, "2  Snake", RGB(5, 150, 105));
    draw_button(x, y + 200, 300, 48, "3  Tetris", RGB(147, 51, 234));
    text(x, y + 272, "Click a game or press 1 / 2 / 3", RGB(203, 213, 225));
}

#define MINES_OX 44
#define MINES_OY 78
#define MINES_CELL 34

static void draw_mines(void) {
    int ox = MINES_OX, oy = MINES_OY, cell = MINES_CELL;
    cui_fill(&c, 0, 0, win_w, win_h, RGB(16, 27, 45));
    text(ox, oy - 52, "Minesweeper", RGB(226, 232, 240));
    cui_text(&c, f_s, ox, oy - 28, "Arrows: move   Space/click: reveal   F/right click: flag   R: reset   Esc: menu", RGB(148, 163, 184));
    for (int y = 0; y < MINE_H; ++y) for (int x = 0; x < MINE_W; ++x) {
        uint32_t bg = s_revealed[y][x] ? RGB(226, 232, 240) : RGB(71, 85, 105);
        cui_round_rect(&c, ox + x * cell, oy + y * cell, cell - 3, cell - 3, 4, bg);
        if (x == s_mx && y == s_my) cui_round_rect_outline(&c, ox + x * cell - 1, oy + y * cell - 1, cell, cell, 4, 2.0f, RGB(250, 204, 21));
        const char *g = NULL; uint32_t col = 0; char b[2] = { 0, 0 };
        if (s_revealed[y][x] && s_mine[y][x]) { g = "*"; col = RGB(239, 68, 68); }
        else if (s_flagged[y][x]) { g = "F"; col = RGB(251, 191, 36); }
        else if (s_revealed[y][x]) { int n = mines_neighbours(x, y); if (n) { b[0] = (char)('0' + n); g = b; col = RGB(37, 99, 235); } }
        if (g) cui_text_center(&c, f_b, ox + x * cell, oy + y * cell, cell - 3, cell - 3, g, col);
    }
    if (s_mines_lost) text(ox, oy + MINE_H * cell + 12, "Mine hit. Press R to restart.", RGB(248, 113, 113));
    if (s_mines_won) text(ox, oy + MINE_H * cell + 12, "Board cleared. Press R to play again.", RGB(74, 222, 128));
}

static void draw_snake(void) {
    int ox = 38, oy = 74, cell = 24;
    cui_fill(&c, 0, 0, win_w, win_h, RGB(11, 29, 25));
    text(ox, oy - 52, "Snake", RGB(209, 250, 229));
    cui_text(&c, f_s, ox, oy - 28, "Arrows: steer   R: reset   Esc: menu", RGB(148, 163, 184));
    cui_round_rect_outline(&c, ox - 3, oy - 3, SNAKE_W * cell + 6, SNAKE_H * cell + 6, 4, 2.0f, RGB(52, 211, 153));
    for (int i = 0; i < s_snake_len; ++i) cui_round_rect(&c, ox + s_snake[i].x * cell + 2, oy + s_snake[i].y * cell + 2, cell - 4, cell - 4, 5, i ? RGB(52, 211, 153) : RGB(16, 185, 129));
    cui_circle(&c, (float)(ox + s_food_x * cell + cell / 2), (float)(oy + s_food_y * cell + cell / 2), (float)(cell / 2 - 4), RGB(248, 113, 113));
    char b[48]; snprintf(b, sizeof b, "Length: %d", s_snake_len);
    text(ox, oy + SNAKE_H * cell + 12, b, RGB(167, 243, 208));
    if (s_snake_over) text(ox + 160, oy + SNAKE_H * cell + 12, "Game over - R to restart", RGB(248, 113, 113));
}

static void draw_tetris(void) {
    int ox = 70, oy = 70, cell = 26;
    cui_fill(&c, 0, 0, win_w, win_h, RGB(29, 15, 48));
    text(ox, oy - 58, "Tetris", RGB(233, 213, 255));
    cui_text(&c, f_s, ox, oy - 32, "Left/Right: move   Up: rotate   Down: step   Space: drop   R: reset   Esc: menu", RGB(160, 150, 190));
    cui_round_rect_outline(&c, ox - 3, oy - 3, TET_W * cell + 6, TET_H * cell + 6, 4, 2.0f, RGB(192, 132, 252));
    for (int y = 0; y < TET_H; ++y) for (int x = 0; x < TET_W; ++x) if (s_tet[y][x]) cui_round_rect(&c, ox + x * cell + 1, oy + y * cell + 1, cell - 2, cell - 2, 4, s_tet_color[s_tet[y][x]]);
    if (!s_tet_over) for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) if (tet_cell(s_piece, s_rot, x, y) && s_py + y >= 0) cui_round_rect(&c, ox + (s_px + x) * cell + 1, oy + (s_py + y) * cell + 1, cell - 2, cell - 2, 4, s_tet_color[s_piece + 1]);
    char b[48]; snprintf(b, sizeof b, "Score: %d", s_tet_score);
    text(ox + TET_W * cell + 28, oy + 8, b, RGB(233, 213, 255));
    if (s_tet_over) text(ox + TET_W * cell + 28, oy + 40, "Game over", RGB(248, 113, 113));
}

static void tick(void) {
    uint64_t now = cos_time_ms();
    if (s_mode == G2D_SNAKE && now - s_snake_last_tick >= 180) { snake_step(); s_snake_last_tick = now; }
    if (s_mode == G2D_TETRIS && now - s_tet_last_tick >= 420) { tet_step(); s_tet_last_tick = now; }
}

static void on_key(const cos_win_event_t *ev) {
    char a = ev->ascii >= 'A' && ev->ascii <= 'Z' ? (char)(ev->ascii + 32) : ev->ascii;
    if (ev->special == COS_KEY_ESC) { s_mode = G2D_MENU; return; }
    if (s_mode == G2D_MENU) {
        if (a == '1') { mines_reset(); s_mode = G2D_MINES; }
        else if (a == '2') { snake_reset(); s_mode = G2D_SNAKE; }
        else if (a == '3') { tet_reset(); s_mode = G2D_TETRIS; }
        return;
    }
    uint8_t k = ev->special;
    if (s_mode == G2D_MINES) {
        if (k == COS_KEY_UP && s_my > 0) s_my--; else if (k == COS_KEY_DOWN && s_my < MINE_H - 1) s_my++;
        else if (k == COS_KEY_LEFT && s_mx > 0) s_mx--; else if (k == COS_KEY_RIGHT && s_mx < MINE_W - 1) s_mx++;
        else if (a == ' ') mines_reveal(s_mx, s_my);
        else if (a == 'f' && !s_revealed[s_my][s_mx]) s_flagged[s_my][s_mx] = !s_flagged[s_my][s_mx];
        else if (a == 'r') mines_reset();
    } else if (s_mode == G2D_SNAKE) {
        if (k == COS_KEY_UP && s_sdy == 0) { s_sdx = 0; s_sdy = -1; }
        else if (k == COS_KEY_DOWN && s_sdy == 0) { s_sdx = 0; s_sdy = 1; }
        else if (k == COS_KEY_LEFT && s_sdx == 0) { s_sdx = -1; s_sdy = 0; }
        else if (k == COS_KEY_RIGHT && s_sdx == 0) { s_sdx = 1; s_sdy = 0; }
        else if (a == 'r') snake_reset();
    } else if (s_mode == G2D_TETRIS && !s_tet_over) {
        if (k == COS_KEY_LEFT && !tet_collides(s_piece, s_rot, s_px - 1, s_py)) s_px--;
        else if (k == COS_KEY_RIGHT && !tet_collides(s_piece, s_rot, s_px + 1, s_py)) s_px++;
        else if (k == COS_KEY_DOWN) tet_step();
        else if (k == COS_KEY_UP && !tet_collides(s_piece, s_rot + 1, s_px, s_py)) s_rot = (s_rot + 1) & 3;
        else if (a == ' ') { while (!tet_collides(s_piece, s_rot, s_px, s_py + 1)) s_py++; tet_step(); }
        else if (a == 'r') tet_reset();
    } else if (s_mode == G2D_TETRIS && a == 'r') tet_reset();
}

static void on_click(int mx, int my, bool right) {
    if (s_mode == G2D_MENU) {
        int y = 40 + 80;
        if (mx < 34 || mx > 334) return;
        if (my >= y && my < y + 48) { mines_reset(); s_mode = G2D_MINES; }
        else if (my >= y + 60 && my < y + 108) { snake_reset(); s_mode = G2D_SNAKE; }
        else if (my >= y + 120 && my < y + 168) { tet_reset(); s_mode = G2D_TETRIS; }
    } else if (s_mode == G2D_MINES) {
        int x = (mx - MINES_OX) / MINES_CELL, y = (my - MINES_OY) / MINES_CELL;
        if (mx >= MINES_OX && my >= MINES_OY && x < MINE_W && y < MINE_H) {
            s_mx = x; s_my = y;
            if (right) { if (!s_revealed[y][x]) s_flagged[y][x] = !s_flagged[y][x]; }
            else mines_reveal(x, y);
        }
    }
}

int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("2D Games", 640, 620, &wi);
    if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(16); f_b = cui_font_bold(17); f_h = cui_font_bold(30);
    s_rng ^= (uint32_t)cos_time_ms();

    for (;;) {
        tick();
        switch (s_mode) { case G2D_MENU: draw_menu(); break; case G2D_MINES: draw_mines(); break; case G2D_SNAKE: draw_snake(); break; default: draw_tetris(); break; }
        cos_win2_present(h);
        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, s_mode == G2D_SNAKE || s_mode == G2D_TETRIS ? 40 : 300);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            if (ev.type == COS_EV_KEY) on_key(&ev);
            else if (ev.type == COS_EV_MOUSE_DOWN) on_click(ev.x, ev.y, ev.button == COS_MOUSE_BTN_RIGHT);
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
