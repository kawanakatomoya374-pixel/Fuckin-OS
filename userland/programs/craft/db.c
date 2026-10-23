/* db.c - the world database of the C-OS port of Craft.
 *
 * The original keeps blocks, lights and signs in SQLite. C-OS has no SQLite, and a Craft world only needs to remember what the
 * player changed (the terrain itself is generated from noise), so the changes are kept in memory, per chunk, in the same Map /
 * SignList structures the game already uses, and written to one file - atomically from the game's point of view: the whole
 * file is rewritten every few seconds (db_commit) and at exit. Same API as Craft's db.h.
 *
 * File: "CRFT" 1, player state, then per chunk: p q, blocks (x y z w), lights, signs (x y z face text).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cos.h"
#include <math.h>
#include "config.h"
#include "db.h"
#include "tinycthread.h"

typedef struct DbChunk { int p, q; Map blocks, lights; SignList signs; } DbChunk;

static int s_enabled;
static char s_path[256];
static DbChunk **s_chunks; static int s_nchunks, s_capchunks;
static mtx_t s_mtx; static int s_mtx_ready;
static int s_dirty;
static int s_have_state; static float s_state[5];

static void lock(void) { if (s_mtx_ready) mtx_lock(&s_mtx); }
static void unlock(void) { if (s_mtx_ready) mtx_unlock(&s_mtx); }

void db_enable() { s_enabled = 1; }
void db_disable() { s_enabled = 0; }
int get_db_enabled() { return s_enabled; }

static DbChunk *find_chunk(int p, int q, int create) {
    for (int i = 0; i < s_nchunks; ++i) if (s_chunks[i]->p == p && s_chunks[i]->q == q) return s_chunks[i];
    if (!create) return NULL;
    if (s_nchunks == s_capchunks) {
        int nc = s_capchunks ? s_capchunks * 2 : 64;
        DbChunk **n = (DbChunk **)realloc(s_chunks, (size_t)nc * sizeof *n);
        if (!n) return NULL;
        s_chunks = n; s_capchunks = nc;
    }
    DbChunk *c = (DbChunk *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->p = p; c->q = q;
    map_alloc(&c->blocks, p * CHUNK_SIZE, 0, q * CHUNK_SIZE, 0x3FF);
    map_alloc(&c->lights, p * CHUNK_SIZE, 0, q * CHUNK_SIZE, 0xF);
    sign_list_alloc(&c->signs, 4);
    s_chunks[s_nchunks++] = c;
    return c;
}

/* ---- persistence ---- */
static void put32(unsigned char **o, int v) { memcpy(*o, &v, 4); *o += 4; }
static int get32(const unsigned char **i) { int v; memcpy(&v, *i, 4); *i += 4; return v; }

static void save_file(void) {
    size_t cap = 64 + 32;
    for (int i = 0; i < s_nchunks; ++i) cap += 32 + (size_t)(s_chunks[i]->blocks.size + s_chunks[i]->lights.size) * 16 + (size_t)s_chunks[i]->signs.size * (20 + MAX_SIGN_LENGTH);
    unsigned char *buf = (unsigned char *)malloc(cap), *o = buf;
    if (!buf) return;
    memcpy(o, "CRFT", 4); o += 4; put32(&o, 1);
    put32(&o, s_have_state);
    for (int i = 0; i < 5; ++i) { memcpy(o, &s_state[i], 4); o += 4; }
    put32(&o, s_nchunks);
    for (int i = 0; i < s_nchunks; ++i) {
        DbChunk *c = s_chunks[i];
        put32(&o, c->p); put32(&o, c->q);
        put32(&o, (int)c->blocks.size);
        { Map *map = &c->blocks; MAP_FOR_EACH(map, ex, ey, ez, ew) { put32(&o, ex); put32(&o, ey); put32(&o, ez); put32(&o, ew); } END_MAP_FOR_EACH; }
        put32(&o, (int)c->lights.size);
        { Map *map = &c->lights; MAP_FOR_EACH(map, ex, ey, ez, ew) { put32(&o, ex); put32(&o, ey); put32(&o, ez); put32(&o, ew); } END_MAP_FOR_EACH; }
        put32(&o, (int)c->signs.size);
        for (unsigned k = 0; k < c->signs.size; ++k) {
            Sign *s = &c->signs.data[k];
            put32(&o, s->x); put32(&o, s->y); put32(&o, s->z); put32(&o, s->face);
            memcpy(o, s->text, MAX_SIGN_LENGTH); o += MAX_SIGN_LENGTH;
        }
    }
    { int64_t wr = cos_write_file(s_path, buf, (size_t)(o - buf)); char m[96]; snprintf(m, sizeof m, "[CRAFT] world saved: %d chunks, %d bytes (result %d)\n", s_nchunks, (int)(o - buf), (int)wr); cos_puts(m); }
    free(buf);
}

static void load_file(void) {
    cos_stat_t st;
    if (cos_stat(s_path, &st) != 0 || st.is_dir || st.size < 16 || st.size > (256u << 20)) return;
    unsigned char *buf = (unsigned char *)malloc((size_t)st.size);
    if (!buf) return;
    ssize_t n = cos_read_file(s_path, buf, (size_t)st.size);
    const unsigned char *i = buf, *end = buf + (n > 0 ? n : 0);
    if (n < 16 || memcmp(i, "CRFT", 4)) { free(buf); return; }
    i += 4;
    if (get32(&i) != 1) { free(buf); return; }
    s_have_state = get32(&i);
    for (int k = 0; k < 5; ++k) { memcpy(&s_state[k], i, 4); i += 4; }
    int nch = get32(&i);
    for (int c = 0; c < nch && i + 12 <= end; ++c) {
        int p = get32(&i), q = get32(&i);
        DbChunk *ch = find_chunk(p, q, 1);
        for (int pass = 0; pass < 2 && ch; ++pass) {
            int cnt = get32(&i);
            for (int k = 0; k < cnt && i + 16 <= end; ++k) {
                int x = get32(&i), y = get32(&i), z = get32(&i), w = get32(&i);
                map_set(pass == 0 ? &ch->blocks : &ch->lights, x, y, z, w);
            }
        }
        int ns = ch ? get32(&i) : 0;
        for (int k = 0; k < ns && i + 16 + MAX_SIGN_LENGTH <= end; ++k) {
            int x = get32(&i), y = get32(&i), z = get32(&i), face = get32(&i);
            char text[MAX_SIGN_LENGTH]; memcpy(text, i, MAX_SIGN_LENGTH); text[MAX_SIGN_LENGTH - 1] = 0; i += MAX_SIGN_LENGTH;
            sign_list_add(&ch->signs, x, y, z, face, text);
        }
    }
    { char m[64]; snprintf(m, sizeof m, "[CRAFT] world loaded: %d chunks\n", nch); cos_puts(m); }
    free(buf);
}

int db_init(char *path) {
    if (!s_mtx_ready) { mtx_init(&s_mtx, mtx_plain); s_mtx_ready = 1; }
    snprintf(s_path, sizeof s_path, "%s", path);
    lock();
    for (int i = 0; i < s_nchunks; ++i) { map_free(&s_chunks[i]->blocks); map_free(&s_chunks[i]->lights); sign_list_free(&s_chunks[i]->signs); free(s_chunks[i]); }
    s_nchunks = 0; s_have_state = 0; s_dirty = 0;
    load_file();
    unlock();
    return 0;
}
void db_close() { lock(); if (s_dirty) { save_file(); s_dirty = 0; } unlock(); }
void db_commit() { lock(); if (s_dirty) { save_file(); s_dirty = 0; } unlock(); }

/* ---- accounts of the multiplayer server: not used offline ---- */
void db_auth_set(char *username, char *identity_token) { (void)username; (void)identity_token; }
int db_auth_select(char *username) { (void)username; return 0; }
void db_auth_select_none() {}
int db_auth_get(char *username, char *identity_token, int identity_token_length) { (void)username; (void)identity_token; (void)identity_token_length; return 0; }
int db_auth_get_selected(char *username, int username_length, char *identity_token, int identity_token_length) {
    (void)username; (void)username_length; (void)identity_token; (void)identity_token_length; return 0;
}

void db_save_state(float x, float y, float z, float rx, float ry) {
    lock(); s_state[0] = x; s_state[1] = y; s_state[2] = z; s_state[3] = rx; s_state[4] = ry; s_have_state = 1; s_dirty = 1; unlock();
}
int db_load_state(float *x, float *y, float *z, float *rx, float *ry) {
    lock();
    int ok = s_have_state;
    if (ok) { *x = s_state[0]; *y = s_state[1]; *z = s_state[2]; *rx = s_state[3]; *ry = s_state[4]; }
    unlock();
    return ok;
}
void db_insert_block(int p, int q, int x, int y, int z, int w) {
    lock(); DbChunk *c = find_chunk(p, q, 1); if (c) { map_set(&c->blocks, x, y, z, w); s_dirty = 1; } unlock();
}
void db_insert_light(int p, int q, int x, int y, int z, int w) {
    lock(); DbChunk *c = find_chunk(p, q, 1); if (c) { map_set(&c->lights, x, y, z, w); s_dirty = 1; } unlock();
}
void db_insert_sign(int p, int q, int x, int y, int z, int face, const char *text) {
    lock(); DbChunk *c = find_chunk(p, q, 1);
    if (c) { sign_list_remove(&c->signs, x, y, z, face); sign_list_add(&c->signs, x, y, z, face, text); s_dirty = 1; }
    unlock();
}
void db_delete_sign(int x, int y, int z, int face) {
    lock(); int p = (int)floorf(roundf((float)x) / CHUNK_SIZE), q = (int)floorf(roundf((float)z) / CHUNK_SIZE);
    DbChunk *c = find_chunk(p, q, 0); if (c && sign_list_remove(&c->signs, x, y, z, face)) s_dirty = 1; unlock();
}
void db_delete_signs(int x, int y, int z) {
    lock(); int p = (int)floorf(roundf((float)x) / CHUNK_SIZE), q = (int)floorf(roundf((float)z) / CHUNK_SIZE);
    DbChunk *c = find_chunk(p, q, 0); if (c && sign_list_remove_all(&c->signs, x, y, z)) s_dirty = 1; unlock();
}
void db_delete_all_signs() { lock(); for (int i = 0; i < s_nchunks; ++i) s_chunks[i]->signs.size = 0; s_dirty = 1; unlock(); }

static void copy_map(Map *dst, Map *src) { MAP_FOR_EACH(src, ex, ey, ez, ew) { map_set(dst, ex, ey, ez, ew); } END_MAP_FOR_EACH; }
void db_load_blocks(Map *map, int p, int q) { lock(); DbChunk *c = find_chunk(p, q, 0); if (c) copy_map(map, &c->blocks); unlock(); }
void db_load_lights(Map *map, int p, int q) { lock(); DbChunk *c = find_chunk(p, q, 0); if (c) copy_map(map, &c->lights); unlock(); }
void db_load_signs(SignList *list, int p, int q) {
    lock(); DbChunk *c = find_chunk(p, q, 0);
    if (c) for (unsigned k = 0; k < c->signs.size; ++k) { Sign *s = &c->signs.data[k]; sign_list_add(list, s->x, s->y, s->z, s->face, s->text); }
    unlock();
}
int db_get_key(int p, int q) { (void)p; (void)q; return 0; }
void db_set_key(int p, int q, int key) { (void)p; (void)q; (void)key; }
void db_worker_start() {}
void db_worker_stop() {}
int db_worker_run(void *arg) { (void)arg; return 0; }
