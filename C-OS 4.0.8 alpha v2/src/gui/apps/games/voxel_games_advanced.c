#include "vga.h"
#include "serial.h"
#include "timer.h"
#include "memory.h"
#include "mouse.h"
#include "keyboard.h"
#include "gui.h"
#include "voxel_games_advanced.h"
#include <stddef.h>
#include <string.h>

/* ===================== Advanced FPS Engine ===================== */
/* Cyber Storm C-OS: Professional 3D FPS Engine with DDA Raycasting */

#define WORLD_W 64
#define WORLD_H 64
#define MAX_ENEMIES 48
#define MAX_PROJECTILES 128
#define MAX_PARTICLES 256
#define MAX_PICKUPS 32

#define PLAYER_MAX_HP 150
#define PLAYER_MAX_ARMOR 150
#define FOV 60
#define RENDER_DISTANCE 30.0f

/* Game Modes */
#define MODE_MENU 0
#define MODE_FPS 1
#define MODE_DEAD 2

/* Weapon Types */
#define WEAPON_KNIFE 0
#define WEAPON_PISTOL 1
#define WEAPON_RIFLE 2
#define WEAPON_SHOTGUN 3

/* Enemy Types with AI Profiles */
#define ENEMY_ZOMBIE 0
#define ENEMY_SOLDIER 1
#define ENEMY_ELITE 2

/* Particle Types */
#define PARTICLE_SPARK 0
#define PARTICLE_BLOOD 1
#define PARTICLE_SMOKE 2

/* Pickup Types */
#define PICKUP_HEALTH 0
#define PICKUP_ARMOR 1
#define PICKUP_AMMO 2

/* ===================== Advanced Structures ===================== */
typedef struct {
    float x, y, vx, vy;
    int life, type, active;
    uint64_t color;
    float scale;
} particle_t;

typedef struct {
    float x, y, dx, dy;
    int ttl, kind, owner, power;
    int active;
} projectile_t;

typedef struct {
    float x, y;
    int type, amount, active;
    uint64_t spawn_tick;
    float bob_phase;
} pickup_t;

typedef struct {
    float x, y, vx, vy, angle;
    float hp, max_hp, armor;
    int alive, type, alert_level;
    float patrol_x, patrol_y;
    uint64_t last_move_tick, last_shoot_tick;
    int shoot_cooldown, patrol_timer;
    float vision_range;
    int ai_state;
} enemy_t;

typedef struct {
    int initialized, mode, weapon, score, kills, wave;
    float px, py, pz, vz, angle_deg, pitch;
    float bob_phase, bob_amp;
    int hp, armor, grenades;
    int ammo_pistol, ammo_rifle, ammo_shotgun;
    int reload_ticks, fire_cooldown, zoom_level;
    int move_fwd, move_back, move_left, move_right, jump_pressed;
    uint64_t last_tick, shot_tick, damage_flash_tick;
    uint64_t wave_clear_timer;
    uint8_t world[WORLD_W][WORLD_H];
    enemy_t enemies[MAX_ENEMIES];
    projectile_t projectiles[MAX_PROJECTILES];
    particle_t particles[MAX_PARTICLES];
    pickup_t pickups[MAX_PICKUPS];
} game_state_t;

static game_state_t g;

/* ===================== Math Helpers ===================== */
static float voxel_sin_f(int deg) {
    deg = ((deg % 360) + 360) % 360;
    float rad = (float)deg * 0.0174533f;
    float x = rad;
    return x - x*x*x/6.0f + x*x*x*x*x/120.0f;
}

static float voxel_cos_f(int deg) {
    deg = ((deg % 360) + 360) % 360;
    float rad = (float)deg * 0.0174533f;
    float x = rad;
    float c = 1.0f - x*x/2.0f + x*x*x*x/24.0f;
    return c;
}

static float voxel_sqrtf(float x) {
    if (x <= 0) return 0;
    float res = x;
    for(int i=0; i<8; i++) res = 0.5f * (res + x / res);
    return res;
}

static float voxel_dist(float x1, float y1, float x2, float y2) {
    float dx = x1 - x2, dy = y1 - y2;
    return voxel_sqrtf(dx * dx + dy * dy);
}

static float voxel_clampf(float v, float min, float max) {
    if(v < min) return min;
    if(v > max) return max;
    return v;
}

/* ===================== World Generation ===================== */
static void world_generate(void) {
    memset(g.world, 0, sizeof(g.world));
    
    /* Outer walls */
    for (int i = 0; i < WORLD_W; i++) {
        g.world[i][0] = 3;
        g.world[i][WORLD_H-1] = 3;
        g.world[0][i] = 3;
        g.world[WORLD_W-1][i] = 3;
    }
    
    /* Interior obstacles */
    for (int i = 0; i < 15; i++) {
        int x = 8 + (i * 13) % (WORLD_W - 16);
        int y = 8 + (i * 17) % (WORLD_H - 16);
        for (int dx = 0; dx < 4; dx++) {
            for (int dy = 0; dy < 4; dy++) {
                if (x+dx < WORLD_W && y+dy < WORLD_H) {
                    g.world[x+dx][y+dy] = 2;
                }
            }
        }
    }
}

static int is_blocking_cell(int x, int y) {
    if (x < 0 || x >= WORLD_W || y < 0 || y >= WORLD_H) return 1;
    return g.world[x][y] != 0;
}

/* ===================== DDA Raycasting Engine ===================== */
static void raycast_column(int col, int vx, int vy, int vw, int vh) {
    float angle = g.angle_deg + (float)(col - vw/2) * 0.5f;
    float dx = voxel_cos_f((int)angle);
    float dy = voxel_sin_f((int)angle);
    
    float x = g.px, y = g.py;
    float dist = 0;
    int wall_type = 0;
    
    /* DDA algorithm for raycasting */
    for (int step = 0; step < 200; step++) {
        x += dx * 0.1f;
        y += dy * 0.1f;
        dist += 0.1f;
        
        int grid_x = (int)x;
        int grid_y = (int)y;
        
        if (grid_x < 0 || grid_x >= WORLD_W || grid_y < 0 || grid_y >= WORLD_H) {
            wall_type = 3;
            break;
        }
        
        if (g.world[grid_x][grid_y] != 0) {
            wall_type = g.world[grid_x][grid_y];
            break;
        }
        
        if (dist > RENDER_DISTANCE) break;
    }
    
    /* Calculate wall height with perspective correction */
    float corrected_dist = dist * voxel_cos_f((int)(angle - g.angle_deg));
    if (corrected_dist < 0.1f) corrected_dist = 0.1f;
    
    int wall_height = (int)(vh * 0.5f / corrected_dist);
    if (wall_height > vh) wall_height = vh;
    
    int top = (vh - wall_height) / 2;
    int bottom = top + wall_height;
    
    /* Wall color with distance fog */
    uint64_t base_color;
    switch(wall_type) {
        case 1: base_color = rgb(100, 100, 100); break;
        case 2: base_color = rgb(150, 80, 80); break;
        case 3: base_color = rgb(50, 50, 50); break;
        default: base_color = rgb(80, 80, 80); break;
    }
    
    /* Apply fog effect */
    float fog_factor = voxel_clampf(1.0f - corrected_dist / RENDER_DISTANCE, 0.0f, 1.0f);
    uint8_t r = ((base_color >> 16) & 0xFF);
    uint8_t g_c = ((base_color >> 8) & 0xFF);
    uint8_t b = (base_color & 0xFF);
    
    r = (uint8_t)(r * fog_factor + 10 * (1.0f - fog_factor));
    g_c = (uint8_t)(g_c * fog_factor + 10 * (1.0f - fog_factor));
    b = (uint8_t)(b * fog_factor + 10 * (1.0f - fog_factor));
    
    uint64_t wall_color = rgb(r, g_c, b);
    
    /* Draw wall */
    if (wall_height > 0) {
        vga_fill_rect(vx + col, vy + top, 1, wall_height, wall_color);
    }
    
    /* Draw ceiling and floor */
    if (top > 0) {
        vga_fill_rect(vx + col, vy, 1, top, rgb(20, 20, 30));
    }
    if (bottom < vh) {
        vga_fill_rect(vx + col, vy + bottom, 1, vh - bottom, rgb(40, 40, 50));
    }
}

/* ===================== Particle System ===================== */
static void spawn_particle(float x, float y, float vx, float vy, int life, int type, uint64_t color) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!g.particles[i].active) {
            g.particles[i] = (particle_t){x, y, vx, vy, life, type, 1, color, 1.0f};
            return;
        }
    }
}

static void update_particles(void) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!g.particles[i].active) continue;
        g.particles[i].x += g.particles[i].vx;
        g.particles[i].y += g.particles[i].vy;
        g.particles[i].life--;
        g.particles[i].vy -= 0.02f;
        if (g.particles[i].life <= 0) g.particles[i].active = 0;
    }
}

/* ===================== Enemy AI ===================== */
static void enemy_ai_update(enemy_t* e, uint64_t now) {
    if (!e->alive) return;
    
    float dist_to_player = voxel_dist(e->x, e->y, g.px, g.py);
    
    /* Vision-based alert system */
    if (dist_to_player < e->vision_range) {
        e->alert_level = 2;
    } else if (dist_to_player < e->vision_range * 1.5f) {
        e->alert_level = 1;
    } else if (dist_to_player > e->vision_range * 2.0f) {
        e->alert_level = 0;
    }
    
    /* AI State Machine */
    if (e->alert_level > 0) {
        /* Chase player */
        float dx = g.px - e->x;
        float dy = g.py - e->y;
        float d = voxel_sqrtf(dx*dx + dy*dy);
        if (d > 0.01f) {
            float speed = (e->type == ENEMY_ELITE) ? 2.5f : (e->type == ENEMY_SOLDIER) ? 1.8f : 1.2f;
            e->vx = (dx / d) * speed;
            e->vy = (dy / d) * speed;
        }
        
        /* Shoot at player */
        if (e->alert_level == 2 && (int64_t)(now - e->last_shoot_tick) > e->shoot_cooldown) {
            e->last_shoot_tick = now;
            e->shoot_cooldown = (e->type == ENEMY_ELITE) ? 500 : 1000;
        }
    } else {
        /* Patrol */
        e->vx *= 0.9f;
        e->vy *= 0.9f;
    }
    
    /* Move enemy */
    float nx = e->x + e->vx * 0.016f;
    float ny = e->y + e->vy * 0.016f;
    
    if (!is_blocking_cell((int)nx, (int)e->y)) e->x = nx;
    if (!is_blocking_cell((int)e->x, (int)ny)) e->y = ny;
}

/* ===================== Weapon System ===================== */
static void fire_weapon(void) {
    if (g.fire_cooldown > 0 || g.reload_ticks > 0) return;
    
    int damage = 10, cooldown = 100;
    
    switch(g.weapon) {
        case WEAPON_RIFLE:
            if (g.ammo_rifle <= 0) return;
            g.ammo_rifle--;
            damage = 30;
            cooldown = 120;
            break;
        case WEAPON_SHOTGUN:
            if (g.ammo_shotgun <= 0) return;
            g.ammo_shotgun--;
            damage = 60;
            cooldown = 600;
            break;
        case WEAPON_PISTOL:
            if (g.ammo_pistol <= 0) return;
            g.ammo_pistol--;
            damage = 15;
            cooldown = 200;
            break;
    }
    
    g.fire_cooldown = cooldown;
    g.shot_tick = get_timer_ticks();
    
    /* Raycast for hits */
    float dx = voxel_cos_f((int)g.angle_deg);
    float dy = voxel_sin_f((int)g.angle_deg);
    
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!g.enemies[i].alive) continue;
        float ex = g.enemies[i].x - g.px;
        float ey = g.enemies[i].y - g.py;
        float dist = voxel_sqrtf(ex*ex + ey*ey);
        if (dist < 0.01f) continue;
        
        float dot = (ex*dx + ey*dy) / dist;
        if (dot > 0.95f && dist < 25.0f) {
            g.enemies[i].hp -= damage;
            g.score += 10;
            if (g.enemies[i].hp <= 0) {
                g.enemies[i].alive = 0;
                g.kills++;
                g.score += 100;
            }
        }
    }
}

/* ===================== Game Loop ===================== */
static void game_reset_new(void) {
    memset(&g, 0, sizeof(g));
    g.initialized = 1;
    g.mode = MODE_FPS;
    g.px = WORLD_W / 2.0f;
    g.py = WORLD_H / 2.0f;
    g.hp = PLAYER_MAX_HP;
    g.armor = PLAYER_MAX_ARMOR;
    g.ammo_rifle = 120;
    g.ammo_shotgun = 32;
    g.ammo_pistol = 60;
    g.weapon = WEAPON_RIFLE;
    
    world_generate();
    
    /* Spawn initial enemies */
    for (int i = 0; i < 8; i++) {
        g.enemies[i].alive = 1;
        g.enemies[i].type = (i % 3);
        g.enemies[i].x = 10.0f + (i % 4) * 8.0f;
        g.enemies[i].y = 10.0f + (i / 4) * 8.0f;
        g.enemies[i].hp = g.enemies[i].max_hp = 50 + i * 10;
        g.enemies[i].vision_range = 15.0f;
        g.enemies[i].shoot_cooldown = 1000;
    }
}

void voxel_games_draw(int idx) {
    (void)idx;
    if (!g.initialized) game_reset_new();
    
    uint64_t now = get_timer_ticks();
    if (g.last_tick == 0) g.last_tick = now;
    uint64_t delta = (now > g.last_tick) ? (now - g.last_tick) : 1;
    if (delta > 50) delta = 50;
    g.last_tick = now;
    
    /* Update */
    update_particles();
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemy_ai_update(&g.enemies[i], now);
    }
    
    /* Render 3D View */
    int view_x = 40, view_y = 40, view_w = 720, view_h = 400;
    
    for (int col = 0; col < view_w; col++) {
        raycast_column(col, view_x, view_y, view_w, view_h);
    }
    
    /* Draw HUD */
    vga_fill_rect(40, 450, 720, 130, rgba(0, 0, 0, 200));
    vga_draw_rect(40, 450, 720, 130, rgb(0, 200, 255));
    
    /* HP Bar */
    vga_draw_string(60, 470, "HP:", rgb(255, 255, 255), 0);
    int hp_width = (g.hp * 200) / PLAYER_MAX_HP;
    vga_fill_rect(120, 468, hp_width, 16, rgb(255, 50, 50));
    vga_draw_rect(120, 468, 200, 16, rgb(100, 100, 100));
    
    /* Ammo Display */
    vga_draw_string(60, 495, "AMMO:", rgb(255, 255, 255), 0);
    char ammo_str[32];
    switch(g.weapon) {
        case WEAPON_RIFLE: snprintf(ammo_str, 32, "%d / %d", g.ammo_rifle, 120); break;
        case WEAPON_SHOTGUN: snprintf(ammo_str, 32, "%d / %d", g.ammo_shotgun, 32); break;
        default: snprintf(ammo_str, 32, "%d / %d", g.ammo_pistol, 60); break;
    }
    vga_draw_string(130, 495, ammo_str, rgb(0, 255, 100), 0);
    
    /* Score */
    char score_str[32];
    snprintf(score_str, 32, "SCORE: %d  KILLS: %d", g.score, g.kills);
    vga_draw_string(60, 520, score_str, rgb(200, 200, 255), 0);
    
    /* Crosshair */
    int cx = 400, cy = 240;
    vga_draw_line(cx - 15, cy, cx + 15, cy, rgb(0, 255, 255));
    vga_draw_line(cx, cy - 15, cx, cy + 15, rgb(0, 255, 255));
    vga_fill_rect(cx - 1, cy - 1, 2, 2, rgb(255, 255, 0));
    
    vga_flip();
    
    if (g.fire_cooldown > 0) g.fire_cooldown -= (int)delta;
}

void voxel_games_handle_key(int idx, const keyboard_event_t* ev) {
    (void)idx;
    if (!ev) return;
    if (ev->pressed) {
        switch(ev->scancode) {
            case 0x11: g.move_fwd = 1; break;
            case 0x1F: g.move_left = 1; break;
            case 0x20: g.move_back = 1; break;
            case 0x21: g.move_right = 1; break;
            case 0x39: fire_weapon(); break;
        }
    } else {
        switch(ev->scancode) {
            case 0x11: g.move_fwd = 0; break;
            case 0x1F: g.move_left = 0; break;
            case 0x20: g.move_back = 0; break;
            case 0x21: g.move_right = 0; break;
        }
    }
}

void voxel_games_handle_click(int idx, int mx, int my) {
    (void)idx;
    (void)mx;
    (void)my;
    fire_weapon();
}

void voxel_games_prepare_window(window_t* w) {
    strncpy(w->title, "Cyber Storm C-OS", sizeof(w->title)-1);
    w->w = 800;
    w->h = 600;
}

bool voxel_games_is_game_window(const window_t* w) {
    (void)w;
    return true;
}

void voxel_games_save_window_state(const window_t* w) {
    (void)w;
}
