/* fps.c-os - "Cyber Storm", a raycast first-person shooter, as a ring-3 program.
 *
 * The in-kernel version only drew a static view: movement flags were never applied, nothing could hurt the player and
 * there was no menu or game-over. This one is playable: DDA raycasting with distance fog, strafing movement with wall
 * collision, four weapons, three enemy types that chase and shoot (line of sight is checked against the walls),
 * pickups, waves, a minimap and a game-over screen.
 *
 * C-OS delivers key presses but not releases, so a held movement key is a key that has repeated recently; see held().
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "cos.h"
#include "cos_ui.h"

#define VIEW_W 800
#define VIEW_H 480
#define COLS   400                    /* each ray covers two screen columns */
#define WORLD  48
#define MAX_E  40
#define MAX_P  24
#define FOV    1.0471976f             /* 60 degrees */
#define FAR    30.0f

enum { M_MENU, M_PLAY, M_DEAD };
enum { W_KNIFE, W_PISTOL, W_RIFLE, W_SHOTGUN };
enum { E_ZOMBIE, E_SOLDIER, E_ELITE };
enum { P_HEALTH, P_ARMOR, P_AMMO };

typedef struct { float x, y, hp, max_hp; int type, alive; uint64_t next_attack; float flash; } enemy_t;
typedef struct { float x, y; int type, active; } pickup_t;

static cui_canvas c;
static cui_font *f_s, *f_m, *f_b, *f_h;
static int win_w, win_h;
static uint8_t world[WORLD][WORLD];
static enemy_t en[MAX_E];
static pickup_t pk[MAX_P];
static float zbuf[COLS];
static int mode = M_MENU;
static float px, py, pa;
static int hp, armor, weapon, score, kills, wave;
static int ammo[4];
static uint64_t last_ms, fire_ready, flash_until, hurt_until, wave_at;
static uint32_t rng = 0x5EEDC0DEu;
static char msg[64]; static uint64_t msg_until;

/* held keys: refreshed by every key event, expire shortly after the repeat stops */
enum { K_FWD, K_BACK, K_LEFT, K_RIGHT, K_TL, K_TR, K_COUNT };
static uint64_t key_until[K_COUNT];
static void hold(int k, uint64_t now) { key_until[k] = now + (key_until[k] > now ? 130 : 330); }
static bool held(int k, uint64_t now) { return key_until[k] > now; }

static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float frnd(void) { return (float)(rnd() & 0xFFFF) / 65536.0f; }
static void say(const char *s) { snprintf(msg, sizeof msg, "%s", s); msg_until = cos_time_ms() + 1800; }

static bool solid(int x, int y) { return x < 0 || y < 0 || x >= WORLD || y >= WORLD || world[x][y] != 0; }

static void gen_world(void) {
    memset(world, 0, sizeof world);
    for (int i = 0; i < WORLD; i++) { world[i][0] = world[i][WORLD - 1] = world[0][i] = world[WORLD - 1][i] = 3; }
    for (int i = 0; i < 22; i++) {
        int x = 5 + (int)(rnd() % (WORLD - 10)), y = 5 + (int)(rnd() % (WORLD - 10));
        if (fabsf((float)(x - WORLD / 2)) < 4 && fabsf((float)(y - WORLD / 2)) < 4) continue;
        int w = 1 + (int)(rnd() % 4), h = 1 + (int)(rnd() % 4), t = 1 + (int)(rnd() % 2);
        for (int dx = 0; dx < w; dx++) for (int dy = 0; dy < h; dy++) if (x + dx < WORLD - 1 && y + dy < WORLD - 1) world[x + dx][y + dy] = (uint8_t)t;
    }
}

static bool free_spot(float *x, float *y, float min_player_dist) {
    for (int tries = 0; tries < 200; tries++) {
        float fx = 2 + frnd() * (WORLD - 4), fy = 2 + frnd() * (WORLD - 4);
        if (solid((int)fx, (int)fy)) continue;
        if (hypotf(fx - px, fy - py) < min_player_dist) continue;
        *x = fx; *y = fy; return true;
    }
    return false;
}

static void spawn_wave(void) {
    wave++;
    int n = 4 + wave * 2; if (n > MAX_E) n = MAX_E;
    memset(en, 0, sizeof en);
    for (int i = 0; i < n; i++) {
        enemy_t *e = &en[i];
        if (!free_spot(&e->x, &e->y, 10.0f)) continue;
        int r = (int)(rnd() % 10);
        e->type = r < 5 ? E_ZOMBIE : r < 8 ? E_SOLDIER : E_ELITE;
        e->max_hp = e->hp = e->type == E_ZOMBIE ? 40.0f + wave * 4 : e->type == E_SOLDIER ? 60.0f + wave * 5 : 110.0f + wave * 8;
        e->alive = 1; e->next_attack = cos_time_ms() + 1500 + rnd() % 1500;
    }
    char b[40]; snprintf(b, sizeof b, "Wave %d", wave); say(b);
}

static void new_game(void) {
    gen_world();
    px = py = WORLD / 2.0f + 0.5f; pa = 0;
    hp = 150; armor = 50; weapon = W_RIFLE; score = kills = wave = 0;
    ammo[W_KNIFE] = 0; ammo[W_PISTOL] = 60; ammo[W_RIFLE] = 120; ammo[W_SHOTGUN] = 24;
    memset(pk, 0, sizeof pk); memset(key_until, 0, sizeof key_until);
    spawn_wave();
    mode = M_PLAY; last_ms = cos_time_ms(); fire_ready = 0; wave_at = 0;
}

/* Distance along an angle to the nearest wall (grid DDA). */
static float cast(float x, float y, float dx, float dy, int *cell) {
    int mx = (int)x, my = (int)y;
    float ddx = dx == 0 ? 1e30f : fabsf(1.0f / dx), ddy = dy == 0 ? 1e30f : fabsf(1.0f / dy);
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    float sdx = dx < 0 ? (x - mx) * ddx : (mx + 1.0f - x) * ddx;
    float sdy = dy < 0 ? (y - my) * ddy : (my + 1.0f - y) * ddy;
    float d = 0;
    for (int i = 0; i < 128; i++) {
        if (sdx < sdy) { d = sdx; sdx += ddx; mx += sx; } else { d = sdy; sdy += ddy; my += sy; }
        if (solid(mx, my)) { if (cell) *cell = (mx < 0 || my < 0 || mx >= WORLD || my >= WORLD) ? 3 : world[mx][my]; return d; }
        if (d > FAR) break;
    }
    if (cell) *cell = 0;
    return FAR;
}

static bool los(float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0, d = hypotf(dx, dy);
    if (d < 0.01f) return true;
    return cast(x0, y0, dx / d, dy / d, NULL) >= d;
}

static void damage_player(int dmg) {
    int absorbed = armor > 0 ? (dmg * 2) / 3 : 0;
    if (absorbed > armor) absorbed = armor;
    armor -= absorbed; hp -= dmg - absorbed;
    hurt_until = cos_time_ms() + 220;
    if (hp <= 0) { hp = 0; mode = M_DEAD; }
}

static void drop_pickup(float x, float y) {
    if (rnd() % 100 > 45) return;
    for (int i = 0; i < MAX_P; i++) if (!pk[i].active) { pk[i] = (pickup_t){ x, y, (int)(rnd() % 3), 1 }; return; }
}

static void fire(void) {
    uint64_t now = cos_time_ms();
    if (now < fire_ready || mode != M_PLAY) return;
    int dmg = 0, cd = 0, pellets = 1; float cone = 0.985f, range = 28.0f;
    switch (weapon) {
    case W_KNIFE: dmg = 35; cd = 350; cone = 0.8f; range = 1.7f; break;
    case W_PISTOL: if (ammo[W_PISTOL] <= 0) { say("Out of pistol ammo"); return; } ammo[W_PISTOL]--; dmg = 22; cd = 260; break;
    case W_RIFLE: if (ammo[W_RIFLE] <= 0) { say("Out of rifle ammo"); return; } ammo[W_RIFLE]--; dmg = 24; cd = 110; cone = 0.99f; break;
    default: if (ammo[W_SHOTGUN] <= 0) { say("Out of shells"); return; } ammo[W_SHOTGUN]--; dmg = 22; cd = 750; pellets = 5; cone = 0.93f; range = 14.0f; break;
    }
    fire_ready = now + cd; flash_until = now + 70;
    float dx = cosf(pa), dy = sinf(pa), wall = cast(px, py, dx, dy, NULL);
    for (int p = 0; p < pellets; p++) {
        int best = -1; float bd = 1e9f;
        for (int i = 0; i < MAX_E; i++) {
            if (!en[i].alive) continue;
            float ex = en[i].x - px, ey = en[i].y - py, d = hypotf(ex, ey);
            if (d < 0.05f || d > range || d > wall + 0.3f) continue;
            float spread = pellets > 1 ? (frnd() - 0.5f) * 0.3f : 0;
            float dot = (ex * (dx - dy * spread) + ey * (dy + dx * spread)) / d;
            float need = cone - 0.0f;
            if (d < 3.0f) need -= 0.04f * (3.0f - d);
            if (dot > need && d < bd) { bd = d; best = i; }
        }
        if (best >= 0) {
            enemy_t *e = &en[best];
            e->hp -= dmg; e->flash = 1.0f; score += 10;
            if (e->hp <= 0) { e->alive = 0; kills++; score += e->type == E_ELITE ? 250 : e->type == E_SOLDIER ? 150 : 100; drop_pickup(e->x, e->y); }
        }
    }
}

static void move_player(float dt, uint64_t now) {
    float turn = (held(K_TR, now) - held(K_TL, now)) * 2.2f * dt;
    pa += turn;
    float fx = cosf(pa), fy = sinf(pa), mx = 0, my = 0;
    if (held(K_FWD, now)) { mx += fx; my += fy; }
    if (held(K_BACK, now)) { mx -= fx; my -= fy; }
    if (held(K_RIGHT, now)) { mx -= fy; my += fx; }
    if (held(K_LEFT, now)) { mx += fy; my -= fx; }
    float l = hypotf(mx, my);
    if (l > 0.01f) {
        float s = 4.2f * dt / l, nx = px + mx * s, ny = py + my * s, r = 0.25f;
        if (!solid((int)(nx + (mx > 0 ? r : -r)), (int)py)) px = nx;
        if (!solid((int)px, (int)(ny + (my > 0 ? r : -r)))) py = ny;
    }
    for (int i = 0; i < MAX_P; i++) {
        if (!pk[i].active || hypotf(pk[i].x - px, pk[i].y - py) > 0.8f) continue;
        pk[i].active = 0;
        if (pk[i].type == P_HEALTH) { hp += 35; if (hp > 150) hp = 150; say("+35 health"); }
        else if (pk[i].type == P_ARMOR) { armor += 40; if (armor > 150) armor = 150; say("+40 armor"); }
        else { ammo[W_PISTOL] += 20; ammo[W_RIFLE] += 40; ammo[W_SHOTGUN] += 6; say("Ammo"); }
    }
}

static void update_enemies(float dt, uint64_t now) {
    int alive = 0;
    for (int i = 0; i < MAX_E; i++) {
        enemy_t *e = &en[i];
        if (!e->alive) continue;
        alive++;
        if (e->flash > 0) e->flash -= dt * 4;
        float dx = px - e->x, dy = py - e->y, d = hypotf(dx, dy);
        bool sees = d < 22.0f && los(e->x, e->y, px, py);
        float speed = e->type == E_ELITE ? 3.1f : e->type == E_SOLDIER ? 2.2f : 1.7f;
        float stop = e->type == E_ZOMBIE ? 0.9f : 5.0f;
        if (sees && d > stop) {
            float nx = e->x + dx / d * speed * dt, ny = e->y + dy / d * speed * dt;
            if (!solid((int)nx, (int)e->y)) e->x = nx;
            if (!solid((int)e->x, (int)ny)) e->y = ny;
        }
        if (sees && now >= e->next_attack) {
            if (e->type == E_ZOMBIE) { if (d < 1.2f) { damage_player(12); e->next_attack = now + 800; } }
            else if (d < 20.0f) { if (frnd() < 0.55f) damage_player(e->type == E_ELITE ? 14 : 8); e->next_attack = now + (e->type == E_ELITE ? 650 : 1100); }
        }
    }
    if (alive == 0 && mode == M_PLAY) {
        if (!wave_at) { wave_at = now + 2500; say("Wave cleared"); score += 200 * wave; }
        else if (now >= wave_at) { wave_at = 0; spawn_wave(); hp += 20; if (hp > 150) hp = 150; }
    }
}

/* ---- rendering ----------------------------------------------------------------------------------------------- */
static uint32_t shade(int r, int g, int b, float fog) {
    if (fog < 0) fog = 0;
    if (fog > 1) fog = 1;
    return ((uint32_t)(r * fog + 8 * (1 - fog)) << 16) | ((uint32_t)(g * fog + 10 * (1 - fog)) << 8) | (uint32_t)(b * fog + 16 * (1 - fog));
}

static void render_world(uint64_t now) {
    uint32_t *base = c.px;
    int stride = c.stride;
    for (int y = 0; y < VIEW_H; y++) {
        bool ceil = y < VIEW_H / 2;
        float t = ceil ? (float)(VIEW_H / 2 - y) / (VIEW_H / 2) : (float)(y - VIEW_H / 2) / (VIEW_H / 2);
        uint32_t col = ceil ? shade(30, 36, 60, 0.35f + t * 0.5f) : shade(52, 50, 60, 0.25f + t * 0.7f);
        uint32_t *row = base + y * stride;
        for (int x = 0; x < VIEW_W; x++) row[x] = col;
    }
    float dirx = cosf(pa), diry = sinf(pa), planex = -diry * tanf(FOV / 2), planey = dirx * tanf(FOV / 2);
    for (int i = 0; i < COLS; i++) {
        float cam = 2.0f * i / COLS - 1.0f, rx = dirx + planex * cam, ry = diry + planey * cam, len = hypotf(rx, ry);
        int cell;
        float d = cast(px, py, rx / len, ry / len, &cell);
        float perp = d * (dirx * rx / len + diry * ry / len);
        zbuf[i] = perp;
        if (perp < 0.05f) perp = 0.05f;
        int h = (int)(VIEW_H / perp);
        int top = (VIEW_H - h) / 2, bot = top + h;
        if (top < 0) top = 0;
        if (bot > VIEW_H) bot = VIEW_H;
        float fog = 1.0f - perp / FAR;
        int r = cell == 1 ? 120 : cell == 2 ? 170 : 90, g = cell == 1 ? 120 : cell == 2 ? 90 : 100, b = cell == 1 ? 130 : cell == 2 ? 90 : 140;
        for (int y = top; y < bot; y++) {
            float edge = (y - top) < 2 || (bot - y) < 2 ? 0.8f : 1.0f;
            uint32_t col = shade((int)(r * edge), (int)(g * edge), (int)(b * edge), fog);
            uint32_t *p = base + y * stride + i * 2;
            p[0] = p[1] = col;
        }
    }
    /* sprites, far to near */
    int order[MAX_E + MAX_P], n = 0; float dist[MAX_E + MAX_P];
    for (int i = 0; i < MAX_E; i++) if (en[i].alive) { order[n] = i; dist[n++] = hypotf(en[i].x - px, en[i].y - py); }
    for (int i = 0; i < MAX_P; i++) if (pk[i].active) { order[n] = MAX_E + i; dist[n++] = hypotf(pk[i].x - px, pk[i].y - py); }
    for (int a = 1; a < n; a++) { int o = order[a]; float d = dist[a]; int b = a - 1; while (b >= 0 && dist[b] < d) { order[b + 1] = order[b]; dist[b + 1] = dist[b]; b--; } order[b + 1] = o; dist[b + 1] = d; }
    float inv = 1.0f / (planex * diry - dirx * planey);
    for (int k = 0; k < n; k++) {
        bool is_e = order[k] < MAX_E;
        float sx = (is_e ? en[order[k]].x : pk[order[k] - MAX_E].x) - px, sy = (is_e ? en[order[k]].y : pk[order[k] - MAX_E].y) - py;
        float tx = inv * (diry * sx - dirx * sy), ty = inv * (-planey * sx + planex * sy);
        if (ty < 0.15f) continue;
        int scx = (int)((VIEW_W / 2) * (1 + tx / ty));
        int sh = (int)(VIEW_H / ty);
        if (!is_e) sh /= 3;
        int sw = is_e ? sh / 2 : sh;
        int y0 = is_e ? (VIEW_H - sh) / 2 : VIEW_H / 2 + VIEW_H / ty / 2 - sh - 4;
        uint32_t body, head, dark;
        if (is_e) {
            enemy_t *e = &en[order[k]];
            int fl = e->flash > 0;
            body = fl ? 0xFFFFFF : e->type == E_ZOMBIE ? 0x5C8A4A : e->type == E_SOLDIER ? 0x4A63A8 : 0xB03A3A;
            head = fl ? 0xFFFFFF : 0xD9B48C; dark = 0x181818;
        } else {
            int t = pk[order[k] - MAX_E].type;
            body = t == P_HEALTH ? 0xE23B3B : t == P_ARMOR ? 0x3B7BE2 : 0xE2C23B; head = body; dark = 0xFFFFFF;
        }
        float fog = 1.0f - ty / FAR;
        for (int x = scx - sw / 2; x < scx + sw / 2; x++) {
            if (x < 0 || x >= VIEW_W || zbuf[x / 2] < ty) continue;
            float u = (float)(x - (scx - sw / 2)) / (sw ? sw : 1);
            for (int y = y0; y < y0 + sh; y++) {
                if (y < 0 || y >= VIEW_H) continue;
                float v = (float)(y - y0) / (sh ? sh : 1);
                uint32_t col;
                if (is_e) {
                    if (v < 0.22f) { if (u < 0.28f || u > 0.72f) continue; col = (v > 0.1f && (u < 0.4f || u > 0.6f) && v < 0.15f) ? dark : head; }
                    else if (v < 0.62f) col = (u < 0.12f || u > 0.88f) ? dark : body;
                    else { if (u > 0.44f && u < 0.56f) continue; col = u < 0.5f ? dark : body; }
                } else {
                    float cx = u - 0.5f, cy = v - 0.5f;
                    if (fabsf(cx) > 0.5f) continue;
                    col = (fabsf(cx) < 0.12f || fabsf(cy) < 0.12f) ? dark : body;
                }
                int r = (col >> 16) & 255, g = (col >> 8) & 255, b = col & 255;
                base[y * stride + x] = shade(r, g, b, fog + 0.15f);
            }
        }
    }
    (void)now;
}

static void draw_hud(uint64_t now) {
    if (now < hurt_until) cui_fill_alpha(&c, 0, 0, VIEW_W, VIEW_H, 0xFF0000, 90);
    if (now < flash_until && weapon != W_KNIFE) cui_fill_alpha(&c, VIEW_W / 2 - 140, VIEW_H - 150, 280, 150, 0xFFE9A0, 70);
    int cx = VIEW_W / 2, cy = VIEW_H / 2;
    cui_line(&c, (float)cx - 14, (float)cy, (float)cx - 4, (float)cy, 2, 0x40FFFF); cui_line(&c, (float)cx + 4, (float)cy, (float)cx + 14, (float)cy, 2, 0x40FFFF);
    cui_line(&c, (float)cx, (float)cy - 14, (float)cx, (float)cy - 4, 2, 0x40FFFF); cui_line(&c, (float)cx, (float)cy + 4, (float)cx, (float)cy + 14, 2, 0x40FFFF);
    /* weapon */
    int wx = cx + 60, wy = VIEW_H - 30 - (now < flash_until ? 10 : 0);
    if (weapon == W_KNIFE) cui_triangle(&c, (float)wx, (float)wy - 120, (float)wx - 14, (float)wy, (float)wx + 14, (float)wy, 0xC8D0DC);
    else { cui_round_rect(&c, wx - 16, wy - 110, 32, 120, 5, weapon == W_SHOTGUN ? 0x6B4A2E : 0x404650); cui_fill(&c, wx - 6, wy - 118, 12, 20, 0x23262C); }
    /* panel */
    cui_fill(&c, 0, VIEW_H, win_w, win_h - VIEW_H, 0x10131C);
    cui_fill(&c, 0, VIEW_H, win_w, 2, 0x00B8FF);
    cui_text(&c, f_b, 20, VIEW_H + 14, "HP", 0xFFFFFF);
    cui_round_rect(&c, 60, VIEW_H + 16, 220, 16, 5, 0x2A2F3C); if (hp > 0) cui_round_rect(&c, 60, VIEW_H + 16, hp * 220 / 150 < 8 ? 8 : hp * 220 / 150, 16, 5, 0xE03C3C);
    cui_text(&c, f_b, 20, VIEW_H + 44, "ARM", 0xFFFFFF);
    cui_round_rect(&c, 60, VIEW_H + 46, 220, 16, 5, 0x2A2F3C); if (armor > 0) cui_round_rect(&c, 60, VIEW_H + 46, armor * 220 / 150 < 8 ? 8 : armor * 220 / 150, 16, 5, 0x3C8CE0);
    static const char *const WN[4] = { "Knife", "Pistol", "Rifle", "Shotgun" };
    char b[96]; snprintf(b, sizeof b, "%s   %s%d", WN[weapon], weapon == W_KNIFE ? "" : "ammo ", weapon == W_KNIFE ? 0 : ammo[weapon]);
    if (weapon == W_KNIFE) snprintf(b, sizeof b, "Knife");
    cui_text(&c, f_b, 310, VIEW_H + 14, b, 0x5CFF9A);
    snprintf(b, sizeof b, "Score %d   Kills %d   Wave %d", score, kills, wave);
    cui_text(&c, f_m, 310, VIEW_H + 44, b, 0xC8C8FF);
    cui_text(&c, f_s, 20, VIEW_H + 76, "W/S move  A/D strafe  Left/Right turn  Space/click fire  1-4 weapon  Esc menu", 0x7C8AA5);
    if (now < msg_until) cui_text_center(&c, f_h, 0, 24, VIEW_W, 36, msg, 0xFFE070);
    /* minimap */
    int mm = 4, mx0 = VIEW_W - WORLD * mm - 10, my0 = 10;
    cui_fill_alpha(&c, mx0 - 2, my0 - 2, WORLD * mm + 4, WORLD * mm + 4, 0x000000, 150);
    for (int x = 0; x < WORLD; x++) for (int y = 0; y < WORLD; y++) if (world[x][y]) cui_fill(&c, mx0 + x * mm, my0 + y * mm, mm, mm, 0x6A7088);
    for (int i = 0; i < MAX_E; i++) if (en[i].alive) cui_fill(&c, mx0 + (int)(en[i].x * mm) - 1, my0 + (int)(en[i].y * mm) - 1, 3, 3, 0xFF4040);
    cui_fill(&c, mx0 + (int)(px * mm) - 1, my0 + (int)(py * mm) - 1, 3, 3, 0x40FF80);
    cui_line(&c, (float)(mx0 + px * mm), (float)(my0 + py * mm), (float)(mx0 + (px + cosf(pa) * 2) * mm), (float)(my0 + (py + sinf(pa) * 2) * mm), 1, 0x40FF80);
}

static void draw_overlay(const char *title, const char *sub, const char *hint) {
    cui_fill_alpha(&c, 0, 0, win_w, win_h, 0x05060C, mode == M_MENU ? 235 : 170);
    cui_text_center(&c, f_h, 0, win_h / 2 - 90, win_w, 50, title, mode == M_DEAD ? 0xFF5555 : 0x40E0FF);
    cui_text_center(&c, f_m, 0, win_h / 2 - 30, win_w, 28, sub, 0xD0D8F0);
    cui_text_center(&c, f_b, 0, win_h / 2 + 20, win_w, 28, hint, 0xFFE070);
}

int main(void) {
    cos_win_info_t wi;
    int64_t h = cos_win2_create("Cyber Storm", VIEW_W, VIEW_H + 110, &wi);
    if (h <= 0) return 1;
    win_w = wi.width; win_h = wi.height;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13); f_m = cui_font_default(16); f_b = cui_font_bold(16); f_h = cui_font_bold(30);
    rng ^= (uint32_t)cos_time_ms();
    gen_world(); px = py = WORLD / 2.0f + 0.5f;

    for (;;) {
        uint64_t now = cos_time_ms();
        float dt = (float)(now - last_ms) / 1000.0f; if (dt > 0.1f) dt = 0.1f; last_ms = now;
        if (mode == M_PLAY) { move_player(dt, now); update_enemies(dt, now); }
        render_world(now);
        draw_hud(now);
        if (mode == M_MENU) draw_overlay("CYBER STORM", "Survive endless waves in a raycast arena", "Press Enter or click to start");
        else if (mode == M_DEAD) { char s[64]; snprintf(s, sizeof s, "Score %d   Kills %d   Wave %d", score, kills, wave); draw_overlay("YOU DIED", s, "Press R or click to restart"); }
        cos_win2_present(h);

        cos_win_event_t ev;
        int r = cos_win2_wait(h, &ev, 16);
        while (r > 0) {
            if (ev.type == COS_EV_CLOSE) goto done;
            uint64_t t = cos_time_ms();
            if (ev.type == COS_EV_KEY) {
                char a = ev.ascii >= 'A' && ev.ascii <= 'Z' ? (char)(ev.ascii + 32) : ev.ascii;
                if (ev.special == COS_KEY_ESC) { if (mode == M_PLAY) mode = M_MENU; else if (mode == M_MENU && wave > 0 && hp > 0) mode = M_PLAY; }
                else if (mode == M_MENU && (ev.special == COS_KEY_ENTER || a == '\n' || a == '\r' || a == ' ')) { if (wave > 0 && hp > 0) mode = M_PLAY; else new_game(); }
                else if (mode == M_DEAD && (a == 'r' || ev.special == COS_KEY_ENTER)) new_game();
                else if (mode == M_PLAY) {
                    if (a == 'w' || ev.special == COS_KEY_UP) hold(K_FWD, t);
                    else if (a == 's' || ev.special == COS_KEY_DOWN) hold(K_BACK, t);
                    else if (a == 'a') hold(K_LEFT, t);
                    else if (a == 'd') hold(K_RIGHT, t);
                    else if (ev.special == COS_KEY_LEFT || a == 'q') hold(K_TL, t);
                    else if (ev.special == COS_KEY_RIGHT || a == 'e') hold(K_TR, t);
                    else if (a == ' ') fire();
                    else if (a >= '1' && a <= '4') { weapon = a - '1'; fire_ready = t + 150; }
                }
            } else if (ev.type == COS_EV_MOUSE_DOWN && ev.button == COS_MOUSE_BTN_LEFT) {
                if (mode == M_PLAY) fire();
                else if (mode == M_MENU) { if (wave > 0 && hp > 0) mode = M_PLAY; else new_game(); }
                else if (mode == M_DEAD) new_game();
            } else if (ev.type == COS_EV_WHEEL && mode == M_PLAY) {
                weapon = (weapon + (ev.wheel > 0 ? 1 : 3)) & 3;
            }
            r = cos_win2_poll(h, &ev);
        }
    }
done:
    cos_win2_close(h);
    return 0;
}
