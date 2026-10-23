/* mcrt.c - run time glue for the C-OS port of mc-classic-c: the gzip calls (stored uncompressed), the resource pack (unpacked into the
 * game's working directory on first start), and the entry point. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <zlib.h>
#include "cos.h"

/* the sound and music files are one shared copy (/apps/minecraft/audio): every fopen() of the game is routed here (-Dfopen=mc_fopen) */
#undef fopen
#undef fwrite
FILE *fopen(const char *path, const char *mode);      /* -Dfopen=mc_fopen renamed the header's declaration: without this the real one is implicitly int */
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
#ifdef MC_DEBUG
size_t mc_dbg_fwrite(const void *p, size_t sz, size_t n, FILE *f) {
    if ((unsigned long)f < 0x7000000000UL) { char b[120]; snprintf(b, sizeof b, "MCDBG fwrite bad FILE %p caller %p n=%d\n", (void *)f, __builtin_return_address(0), (int)n); cos_puts(b); }
    return fwrite(p, sz, n, f);
}
#endif
FILE *mc_fopen(const char *path, const char *mode) {
    if (!strncmp(path, "resources/sound/", 16) || !strncmp(path, "resources/music/", 16)) {
        char full[200]; snprintf(full, sizeof full, "/apps/minecraft/audio/%s", path + 10);
        return fopen(full, mode);
    }
    return fopen(path, mode);
}

/* gzFile: a file that starts with the gzip magic is inflated completely when it is opened for reading; everything else is plain. */
extern char *stbi_zlib_decode_noheader_malloc(const char *buffer, int len, int *outlen);
typedef struct { FILE *f; unsigned char *mem; size_t len, pos; int direct; } gz_t;

static unsigned char *gunzip(const unsigned char *d, size_t n, size_t *outn) {
    if (n < 18 || d[0] != 0x1f || d[1] != 0x8b) return NULL;
    size_t p = 10; int flg = d[3];
    if (flg & 4) { if (p + 2 > n) return NULL; p += 2 + (d[p] | d[p + 1] << 8); }
    if (flg & 8) { while (p < n && d[p]) ++p; ++p; }
    if (flg & 16) { while (p < n && d[p]) ++p; ++p; }
    if (flg & 2) p += 2;
    if (p >= n) return NULL;
    int ol = 0; char *o = stbi_zlib_decode_noheader_malloc((const char *)d + p, (int)(n - p - 8), &ol);
    if (!o) return NULL;
    *outn = (size_t)ol; return (unsigned char *)o;
}
gzFile gzopen(const char *path, const char *mode) {
    FILE *f = fopen(path, mode); if (!f) return NULL;
    gz_t *g = calloc(1, sizeof *g); if (!g) { fclose(f); return NULL; }
    g->f = f;
    if (mode[0] == 'r') {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *raw = n > 0 ? malloc((size_t)n) : NULL;
        if (raw && fread(raw, 1, (size_t)n, f) == (size_t)n) {
            size_t on = 0; unsigned char *o = gunzip(raw, (size_t)n, &on);
            if (o) { free(raw); g->mem = o; g->len = on; } else { g->mem = raw; g->len = (size_t)n; g->direct = 1; }
        } else free(raw);
    }
    return g;
}
int gzread(gzFile h, void *buf, unsigned len) {
    gz_t *g = h; if (!g || !g->mem) return 0;
    size_t n = g->len - g->pos; if (n > len) n = len;
    memcpy(buf, g->mem + g->pos, n); g->pos += n; return (int)n;
}
#ifdef MC_DEBUG
#define MCFW mc_dbg_fwrite
#else
#define MCFW fwrite
#endif
int gzwrite(gzFile h, const void *buf, unsigned len) { gz_t *g = h; return g ? (int)MCFW(buf, 1, len, g->f) : 0; }
int gzdirect(gzFile h) { gz_t *g = h; return g ? g->direct : 0; }
int gzclose(gzFile h) { gz_t *g = h; if (!g) return -1; fclose(g->f); free(g->mem); free(g); return 0; }

/* inflate: the whole input is decoded at the first call (zlib or gzip framing, found by looking at it), then handed out as the output has room */
typedef struct { unsigned char *buf; size_t len, pos; } inf_t;
int inflateInit2(z_stream *s, int wb) { s->window_bits = wb; s->total_in = s->total_out = 0; s->state = NULL; return Z_OK; }
int inflateEnd(z_stream *s) { inf_t *st = s->state; if (st) { free(st->buf); free(st); s->state = NULL; } return Z_OK; }
int inflate(z_stream *s, int flush) {
    (void)flush; inf_t *st = s->state;
    if (!st) {
        size_t on = 0; unsigned char *o = gunzip(s->next_in, s->avail_in, &on);
        if (!o && s->avail_in > 6) { int ol = 0; char *z = (char *)stbi_zlib_decode_noheader_malloc((const char *)s->next_in + 2, (int)s->avail_in - 2, &ol); if (z) { o = (unsigned char *)z; on = (size_t)ol; } }
        if (!o) return Z_DATA_ERROR;
        st = calloc(1, sizeof *st); st->buf = o; st->len = on; s->state = st;
        s->total_in += s->avail_in; s->avail_in = 0;
    }
    size_t n = st->len - st->pos; if (n > s->avail_out) n = s->avail_out;
    memcpy(s->next_out, st->buf + st->pos, n); st->pos += n;
    s->next_out += n; s->avail_out -= (uInt)n; s->total_out += n;
    return st->pos == st->len ? Z_STREAM_END : Z_OK;
}

/* deflate: stored blocks only (valid, just not smaller). Z_FINISH ends the stream, Z_NO_FLUSH leaves it open like an unclosed GZIPOutputStream. */
int deflateInit2(z_stream *s, int level, int method, int wb, int mem, int strat) { (void)level; (void)method; (void)mem; (void)strat; s->window_bits = wb; s->total_in = s->total_out = 0; s->state = NULL; return Z_OK; }
int deflateEnd(z_stream *s) { (void)s; return Z_OK; }
uLong deflateBound(z_stream *s, uLong len) { (void)s; return len + 5 * (len / 65535 + 1) + 16; }
int deflate(z_stream *s, int flush) {
    const unsigned char *in = s->next_in; size_t left = s->avail_in; unsigned char *out = s->next_out;
    int final = flush == Z_FINISH;
    do {
        size_t n = left > 65535 ? 65535 : left; int last = final && n == left;
        if (s->avail_out < n + 5) return Z_BUF_ERROR;
        out[0] = last ? 1 : 0; out[1] = n & 255; out[2] = n >> 8; out[3] = ~n & 255; out[4] = (~n >> 8) & 255;
        memcpy(out + 5, in, n); out += n + 5; s->avail_out -= (uInt)(n + 5); s->total_out += n + 5; in += n; left -= n;
        if (!final && !left) break;
    } while (left || final);
    s->total_in += s->avail_in; s->avail_in = 0; s->next_out = out;
    return final ? Z_STREAM_END : Z_OK;
}
uLong crc32(uLong crc, const Bytef *buf, uInt len) {
    uint32_t c = ~(uint32_t)crc;
    for (uInt i = 0; i < len; ++i) { c ^= buf[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

/* stb_image is built without stdio (STBI_NO_STDIO): the files are read here, the decoder gets them from memory */
extern unsigned char *stbi_load_from_memory(const unsigned char *buf, int len, int *x, int *y, int *comp, int req);
unsigned char *stbi_load(const char *path, int *x, int *y, int *comp, int req) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    unsigned char *r = stbi_load_from_memory(buf, (int)n, x, y, comp, req);
    free(buf); return r;
}

/* resource pack: "MCP1" then entries { u16 pathlen, u32 size, path, data } up to the end */
extern const unsigned char mc_pack[];
extern const unsigned int mc_pack_len;
extern const char mc_game_dir[];                 /* e.g. "/home/mc/c0.0.11a" */
extern int mc_game_main(int argc, char **argv);

static void mkdirs(const char *path) {
    char tmp[256]; size_t n = strlen(path); if (n >= sizeof tmp) return;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; ++i) if (tmp[i] == '/') { tmp[i] = 0; cos_mkdir(tmp); tmp[i] = '/'; }
    cos_mkdir(tmp);
}

static void unpack(void) {
    if (mc_pack_len < 4 || memcmp(mc_pack, "MCP1", 4)) return;
    size_t p = 4;
    while (p + 6 <= mc_pack_len) {
        unsigned plen = mc_pack[p] | mc_pack[p + 1] << 8; unsigned size = mc_pack[p + 2] | mc_pack[p + 3] << 8 | mc_pack[p + 4] << 16 | (unsigned)mc_pack[p + 5] << 24;
        p += 6; if (p + plen + size > mc_pack_len) break;
        char path[256]; if (plen < sizeof path) { memcpy(path, mc_pack + p, plen); path[plen] = 0;
            cos_stat_t st;
            if (cos_stat(path, &st) != 0 || (unsigned)st.size != size) {
                char dir[256]; strcpy(dir, path); char *sl = strrchr(dir, '/'); if (sl) { *sl = 0; mkdirs(dir); }
                FILE *f = fopen(path, "wb"); if (f) { fwrite(mc_pack + p + plen, 1, size, f); fclose(f); }
            } }
        p += plen + size;
    }
}

int main(int argc, char **argv) {
    dup2(1, 2);                                  /* the game reports problems on stderr: show them where the Terminal shows stdout */
    { printf("mc: dir %s\n", mc_game_dir); fflush(stdout); }
    mkdirs(mc_game_dir);
    { printf("mc: chdir -> %d\n", chdir(mc_game_dir)); fflush(stdout); }
    unpack();
    { printf("mc: unpacked\n"); fflush(stdout); }
    int r = mc_game_main(argc, argv);
    { printf("mc: game returned %d\n", r); fflush(stdout); }
    return r;
}
