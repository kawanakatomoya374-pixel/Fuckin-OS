/* _cos: what the C-OS programs written in Sprint (see ports/sprint/apps) may ask the system for. Only the parts that need speed, care or the kernel are C; the screens and the logic
 * around them are Sprint. Every function needs the capability of --allow-ffi (the one that is never part of --allow-all): a script that is merely double-clicked has none of this.
 *   http_start(url) -> job (int, -1 on failure)     http_poll(job) -> 0 going, 1 finished, -1 failed       http_text(job) -> the body as a string, http_save(job, path) -> bool
 *   catalog_verify(text) -> the signed body without the signature, or nil if the signature is not the C-OS update key's
 *   sha256_file(path) -> hex string or nil           image_blit(canvas, path, x, y, size) -> bool (decodes a PNG/JPEG/BMP/WebP, scales it to size x size)
 *   spawn(path, [args]) -> pid or -1                 prefs() -> UI flags (1 = dark, 2 = Japanese)           ms() -> milliseconds since start */
#include "sp.h"
#include "cos.h"
#include "bearssl.h"
#include "update_pubkey.h"

const uint32_t *gfx_canvas_pixels(SpValue v, int *w, int *h);

static int need(VM *vm) {
    if (!(vm->caps & CAP_FFI) || vm->deterministic) return vm_set_error(vm, E_PERM, "cos is not permitted (run with --allow-ffi)");
    return 0;
}
static SpValue str_val(VM *vm, const char *s, size_t n) { return v_obj(T_STRING, vm_new_string(vm, s, n)); }

#define MAX_JOBS 4
static struct { bool used; int id; cos_http_result_t r; bool have; } jobs[MAX_JOBS];

static int c_http_start(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    if (argv[0].type != T_STRING) return vm_set_error(vm, E_TYPE, "http_start(url)");
    *ret = v_int(-1);
    for (int i = 0; i < MAX_JOBS; ++i) if (!jobs[i].used) {
        int j = cos_net_http_start(AS_STR(argv[0])->data);
        if (j < 0) return 0;
        jobs[i].used = true; jobs[i].id = j; jobs[i].have = false; *ret = v_int(i); return 0;
    }
    return 0;
}
static int job_of(VM *vm, SpValue v) {
    if (v.type != T_INT || v.as.i < 0 || v.as.i >= MAX_JOBS || !jobs[v.as.i].used) { vm_set_error(vm, E_VALUE, "no such transfer"); return -1; }
    return (int)v.as.i;
}
static int c_http_poll(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    int i = job_of(vm, argv[0]); if (i < 0) return 1;
    int p = cos_net_http_poll(jobs[i].id, &jobs[i].r);
    if (p == 0) { *ret = v_int(0); return 0; }
    if (p < 0 || jobs[i].r.error || jobs[i].r.status != 200) { if (p >= 0) cos_net_http_cancel(jobs[i].id); jobs[i].used = false; *ret = v_int(-1); return 0; }
    jobs[i].have = true; *ret = v_int(1); return 0;
}
static uint8_t *take(int i, size_t *n) {
    if (!jobs[i].have || jobs[i].r.body_len > (16u << 20)) return NULL;
    uint8_t *b = malloc((size_t)jobs[i].r.body_len + 1); if (!b) return NULL;
    ssize_t got = cos_net_http_take(jobs[i].id, b, (size_t)jobs[i].r.body_len);
    jobs[i].used = false;
    if (got < 0 || (uint64_t)got != jobs[i].r.body_len) { free(b); return NULL; }
    b[got] = 0; *n = (size_t)got; return b;
}
static int c_http_text(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    int i = job_of(vm, argv[0]); if (i < 0) return 1;
    size_t n; uint8_t *b = take(i, &n); if (!b) { *ret = v_nil(); return 0; }
    *ret = str_val(vm, (const char *)b, n); free(b); return 0;
}
static int c_http_save(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    int i = job_of(vm, argv[0]); if (i < 0) return 1;
    if (argv[1].type != T_STRING) return vm_set_error(vm, E_TYPE, "http_save(job, path)");
    size_t n; uint8_t *b = take(i, &n); *ret = v_bool(0); if (!b) return 0;
    int fd = cos_open(AS_STR(argv[1])->data, COS_O_WRONLY_CREATE);
    if (fd >= 0) { size_t o = 0; bool ok = true; while (o < n) { ssize_t w = cos_fd_write(fd, b + o, n - o > 65536 ? 65536 : n - o); if (w <= 0) { ok = false; break; } o += (size_t)w; } cos_close(fd); *ret = v_bool(ok); }
    free(b); return 0;
}

static int hexval(const char *s, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; ++i) { int v = 0; for (int k = 0; k < 2; ++k) { char ch = s[i * 2 + k]; int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; if (d < 0) return 0; v = v * 16 + d; } out[i] = (unsigned char)v; }
    return 1;
}
static int c_catalog_verify(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    if (argv[0].type != T_STRING) return vm_set_error(vm, E_TYPE, "catalog_verify(text)");
    const char *m = AS_STR(argv[0])->data; size_t len = AS_STR(argv[0])->len; const char *marker = "\n--SIG--\n", *sg = NULL;
    *ret = v_nil();
    for (size_t i = 0; i + 9 <= len; ++i) if (memcmp(m + i, marker, 9) == 0) { sg = m + i; break; }
    if (!sg) return 0;
    size_t body_len = (size_t)(sg - m) + 1; const char *hx = sg + 9; unsigned char sig[64];
    if (strlen(hx) < 128 || !hexval(hx, sig, 64)) return 0;
    unsigned char dg[32]; br_sha256_context x; br_sha256_init(&x); br_sha256_update(&x, m, body_len); br_sha256_out(&x, dg);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)cos_update_pubkey, sizeof cos_update_pubkey };
    if (br_ecdsa_i31_vrfy_raw(&br_ec_prime_i31, dg, 32, &pk, sig, 64) != 1) return 0;
    *ret = str_val(vm, m, body_len); return 0;
}
static int c_sha256_file(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    if (argv[0].type != T_STRING) return vm_set_error(vm, E_TYPE, "sha256_file(path)");
    *ret = v_nil(); int fd = cos_open(AS_STR(argv[0])->data, COS_O_RDONLY); if (fd < 0) return 0;
    br_sha256_context x; br_sha256_init(&x); static unsigned char buf[16384]; ssize_t n;
    while ((n = cos_fd_read(fd, buf, sizeof buf)) > 0) br_sha256_update(&x, buf, (size_t)n);
    cos_close(fd); unsigned char h[32]; br_sha256_out(&x, h); char out[65];
    for (int i = 0; i < 32; ++i) { out[i * 2] = "0123456789abcdef"[h[i] >> 4]; out[i * 2 + 1] = "0123456789abcdef"[h[i] & 15]; }
    *ret = str_val(vm, out, 64); return 0;
}
static int c_image_blit(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    int cw = 0, ch = 0; uint32_t *dst = (uint32_t *)gfx_canvas_pixels(argv[0], &cw, &ch);
    if (!dst || argv[1].type != T_STRING || argv[2].type != T_INT || argv[3].type != T_INT || argv[4].type != T_INT) return vm_set_error(vm, E_TYPE, "image_blit(canvas, path, x, y, size)");
    int x0 = (int)argv[2].as.i, y0 = (int)argv[3].as.i, sz = (int)argv[4].as.i; *ret = v_bool(0);
    if (sz < 1 || sz > 256) return 0;
    uint32_t *px = malloc(256 * 256 * 4); if (!px) return 0;
    cos_image_info_t info;
    if (cos_image_decode(AS_STR(argv[1])->data, px, 256 * 256 * 4, &info) != 0 || !info.width || !info.height || info.width > 256 || info.height > 256) { free(px); return 0; }
    int w = (int)info.width, h = (int)info.height;
    for (int y = 0; y < sz; ++y) for (int x = 0; x < sz; ++x) {
        int dx = x0 + x, dy = y0 + y; if (dx < 0 || dy < 0 || dx >= cw || dy >= ch) continue;
        int xa = x * w / sz, xb = (x + 1) * w / sz; if (xb <= xa) xb = xa + 1; int ya = y * h / sz, yb = (y + 1) * h / sz; if (yb <= ya) yb = ya + 1;
        unsigned r = 0, g = 0, b = 0, cnt = 0;
        for (int yy = ya; yy < yb; ++yy) for (int xx = xa; xx < xb; ++xx) { uint32_t v = px[yy * w + xx]; r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; cnt++; }
        dst[dy * cw + dx] = 0xFF000000u | ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
    }
    free(px); *ret = v_bool(1); return 0;
}
static int c_spawn(VM *vm, int argc, SpValue *argv, SpValue *ret) {
    (void)argc; if (need(vm)) return 1;
    if (argv[0].type != T_STRING || argv[1].type != T_ARRAY) return vm_set_error(vm, E_TYPE, "spawn(path, [args])");
    SArray *a = AS_ARRAY(argv[1]); const char *av[12]; int n = 0; av[n++] = AS_STR(argv[0])->data;
    for (int64_t i = 0; i < a->len && n < 10; ++i) if (a->items[i].type == T_STRING) av[n++] = AS_STR(a->items[i])->data;
    av[n] = NULL; *ret = v_int(cos_spawn_argv(AS_STR(argv[0])->data, av, NULL)); return 0;
}
static int c_prefs(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)argv; if (need(vm)) return 1; *ret = v_int((int64_t)cos_ui_prefs()); return 0; }
static int c_ms(VM *vm, int argc, SpValue *argv, SpValue *ret) { (void)argc; (void)argv; if (need(vm)) return 1; *ret = v_int((int64_t)cos_time_ms()); return 0; }

const NativeDesc lib_cos_fns[] = {
    {"http_start", 1, 1, c_http_start}, {"http_poll", 1, 1, c_http_poll}, {"http_text", 1, 1, c_http_text}, {"http_save", 2, 2, c_http_save},
    {"catalog_verify", 1, 1, c_catalog_verify}, {"sha256_file", 1, 1, c_sha256_file}, {"image_blit", 5, 5, c_image_blit},
    {"spawn", 2, 2, c_spawn}, {"prefs", 0, 0, c_prefs}, {"ms", 0, 0, c_ms}, {NULL, 0, 0, NULL}};
