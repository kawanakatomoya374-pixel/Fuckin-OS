/* cosarc_cli.c - the commands tar, gzip, gunzip, zcat, zip and unzip, on top of cosarc.c.
 *
 *   gzip [-k] [-1..-9] FILE...        FILE -> FILE.gz (FILE is removed unless -k)          gunzip [-k] FILE.gz...   zcat FILE.gz
 *   tar -c|-x|-t [-z] [-v] -f ARCHIVE [-C DIR] [PATH...]     (a .tar, or a .tar.gz / .tgz with -z; -x and -t see a gzip file by itself)
 *   zip [-r] [-0] ARCHIVE.zip PATH...  (folders are added with everything in them)       unzip [-l] [-d DIR] ARCHIVE.zip [NAME...]
 *
 * Big files: every size is 64 bit; tar uses the base-256 size field above 8 GiB, zip writes and reads zip64. Memory stays constant (streams). Names that would leave the
 * target folder (absolute, "..") are refused when extracting; characters FAT and exFAT cannot hold (\ : * ? " < > |) become "_". Links and device files in a tar are skipped.
 * Not supported: xz/bzip2/zstd, encrypted zip, zip methods other than stored and deflate, names in a legacy code page (shown as they are stored).
 * Built for C-OS (cos_* calls) or, with -DCOSARC_HOST, for the host, where the same source is tested against gzip, tar and unzip. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cosarc.h"
#include "cosarc_in.h"

#ifdef COSARC_HOST
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
typedef struct { FILE *f; } afile;
static afile *a_open(const char *p, int w) { FILE *f = fopen(p, w ? "wb" : "rb"); if (!f) return NULL; afile *a = (afile *)malloc(sizeof *a); a->f = f; return a; }
static size_t a_read(afile *a, void *b, size_t n) { return fread(b, 1, n, a->f); }
static int a_write(afile *a, const void *b, size_t n) { return n == 0 || fwrite(b, 1, n, a->f) == n ? 0 : 1; }
static int a_close(afile *a) { int r = fclose(a->f); free(a); return r; }
static int a_seek(afile *a, uint64_t off) { return fseeko(a->f, (off_t)off, SEEK_SET); }
static int a_stat(const char *p, uint64_t *size, int *isdir) { struct stat st; if (stat(p, &st)) return -1; if (size) *size = (uint64_t)st.st_size; if (isdir) *isdir = S_ISDIR(st.st_mode); return 0; }
static int a_mkdir(const char *p) { return mkdir(p, 0755); }
static int a_unlink(const char *p) { return unlink(p); }
static uint64_t a_now(void) { return (uint64_t)time(NULL); }
static int a_listdir(const char *path, int (*cb)(void *, const char *, int), void *ctx) {
    DIR *d = opendir(path); if (!d) return -1;
    struct dirent *e; char *names[4096]; int n = 0;
    while ((e = readdir(d)) && n < 4096) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) names[n++] = strdup(e->d_name);
    closedir(d);
    for (int i = 0; i < n; i++) { char full[4096]; snprintf(full, sizeof full, "%s/%s", path, names[i]); int isd = 0; a_stat(full, NULL, &isd); cb(ctx, names[i], isd); free(names[i]); }
    return 0;
}
#else
#include "cos.h"
typedef struct { cos_FILE *f; } afile;
static afile *a_open(const char *p, int w) { cos_FILE *f = cos_fopen(p, w ? "w" : "r"); if (!f) return NULL; afile *a = (afile *)malloc(sizeof *a); a->f = f; return a; }
static size_t a_read(afile *a, void *b, size_t n) { return cos_fread(b, 1, n, a->f); }
static int a_write(afile *a, const void *b, size_t n) { return n == 0 || cos_fwrite(b, 1, n, a->f) == n ? 0 : 1; }
static int a_close(afile *a) { int r = cos_fclose(a->f); free(a); return r; }
static int a_seek(afile *a, uint64_t off) { return cos_fseek(a->f, (long)off); }
static int a_stat(const char *p, uint64_t *size, int *isdir) { cos_stat_t st; if (cos_stat(p, &st) < 0) return -1; if (size) *size = st.size; if (isdir) *isdir = st.is_dir; return 0; }
static int a_mkdir(const char *p) { return cos_mkdir(p); }
static int a_unlink(const char *p) { return cos_unlink(p); }
static uint64_t a_now(void) { return cos_time_unix(); }
static int a_listdir(const char *path, int (*cb)(void *, const char *, int), void *ctx) {
    int fd = cos_opendir(path); if (fd < 0) return -1;
    static cos_dirent_t ent; char **names = (char **)malloc(sizeof(char *) * 8192); int n = 0;
    while (n < 8192 && cos_readdir(fd, &ent) == 1) if (strcmp(ent.name, ".") && strcmp(ent.name, "..")) names[n++] = strdup(ent.name);
    cos_close(fd);
    for (int i = 0; i < n; i++) { char full[1024]; snprintf(full, sizeof full, "%s/%s", path, names[i]); int isd = 0; a_stat(full, NULL, &isd); cb(ctx, names[i], isd); free(names[i]); }
    free(names); return 0;
}
#endif

/* ---- messages, paths ----------------------------------------------------------------------------------------------------------------------------- */
static void (*g_print)(const char *line);
static const char *g_cwd = "/";
static int g_quiet;
static void pr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void pr(const char *fmt, ...) { char b[1200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (g_print) g_print(b); }

static void join(const char *rel, char *out, size_t cap) {                         /* a path as the user typed it -> an absolute one */
    if (rel[0] == '/') { snprintf(out, cap, "%s", rel); return; }
    if (rel[0] == '.' && rel[1] == '/') rel += 2;
    if (g_cwd[0] == '/' && g_cwd[1] == 0) snprintf(out, cap, "/%s", rel); else snprintf(out, cap, "%s/%s", g_cwd, rel);
}
static int mkdirs(const char *path) {                                              /* mkdir -p */
    char t[2300]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; int isd = 0; if (a_stat(t, NULL, &isd) != 0) a_mkdir(t); *p = '/'; }
    int isd = 0; if (a_stat(t, NULL, &isd) == 0) return isd ? 0 : -1;
    return a_mkdir(t);
}
static void parent_dirs(const char *file) { char t[2300]; snprintf(t, sizeof t, "%s", file); char *s = strrchr(t, '/'); if (s && s != t) { *s = 0; mkdirs(t); } }

/* A name from an archive -> a safe relative path below the target folder: no leading "/", no "." or ".." parts, no characters FAT cannot hold. Returns 0 when nothing usable is left or it tries to escape. */
static int safe_name(const char *in, char *out, size_t cap) {
    size_t n = 0; const char *p = in; out[0] = 0;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        const char *e = p; while (*e && *e != '/' && *e != '\\') e++;
        size_t l = (size_t)(e - p);
        if (l == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (l == 0 || (l == 1 && p[0] == '.')) { p = e; continue; }
        if (n + l + 2 >= cap) return 0;
        if (n) out[n++] = '/';
        for (size_t i = 0; i < l; i++) { unsigned char c = (unsigned char)p[i]; out[n++] = (c < 32 || c == 127 || strchr(":*?\"<>|", c)) ? '_' : (char)c; }
        out[n] = 0; p = e;
    }
    return n > 0;
}

/* ---- file streams for cosarc --------------------------------------------------------------------------------------------------------------------- */
/* ---- progress: cosarc_set_progress(FILE) makes the commands write "DONE TOTAL\nNAME\n" to FILE every 256 KiB of work (the File Manager shows a bar from it) ---- */
static char g_prog_path[512];
static uint64_t g_prog_done, g_prog_total, g_prog_last; static char g_prog_name[200];
void cosarc_set_progress(const char *path) { snprintf(g_prog_path, sizeof g_prog_path, "%s", path ? path : ""); g_prog_done = g_prog_total = g_prog_last = 0; g_prog_name[0] = 0; }
static void prog_flush(void) {
    if (!g_prog_path[0]) return;
    char b[320]; int n = snprintf(b, sizeof b, "%llu %llu\n%s\n", (unsigned long long)g_prog_done, (unsigned long long)g_prog_total, g_prog_name);
    afile *f = a_open(g_prog_path, 1); if (!f) return; a_write(f, b, (size_t)n); a_close(f); g_prog_last = g_prog_done;
}
static void prog_total(uint64_t t) { g_prog_total = t; prog_flush(); }
static void prog_add_total(uint64_t t) { g_prog_total += t; }
static void prog_name(const char *n) { snprintf(g_prog_name, sizeof g_prog_name, "%s", n ? n : ""); prog_flush(); }
static void prog_add(uint64_t n) { g_prog_done += n; if (g_prog_path[0] && g_prog_done - g_prog_last >= 262144) prog_flush(); }
static size_t fp_rd(void *c, void *b, size_t n) { size_t g = a_read((afile *)c, b, n); prog_add(g); return g; }
static int fp_wr(void *c, const void *b, size_t n) { int r = a_write((afile *)c, b, n); if (!r) prog_add(n); return r; }
static uint64_t scan_total(const char *path);
typedef struct { uint64_t sum; const char *base; } scan_t;
static int scan_child(void *c, const char *child, int isdir) { scan_t *t = (scan_t *)c; (void)isdir; char fp[2300]; snprintf(fp, sizeof fp, "%s/%s", t->base, child); t->sum += scan_total(fp); return 0; }
static uint64_t scan_total(const char *path) { uint64_t sz = 0; int isd = 0; if (a_stat(path, &sz, &isd) != 0) return 0; if (!isd) return sz; scan_t t = { 0, path }; a_listdir(path, scan_child, &t); return t.sum; }

static size_t f_rd(void *c, void *b, size_t n) { return a_read((afile *)c, b, n); }
static int f_wr(void *c, const void *b, size_t n) { return a_write((afile *)c, b, n); }
typedef struct { afile *f; uint64_t left; } lim_t;                                  /* a read limited to `left` bytes (a zip entry) */
static size_t lim_rd(void *c, void *b, size_t n) { lim_t *l = (lim_t *)c; if (n > l->left) n = (size_t)l->left; size_t g = n ? a_read(l->f, b, n) : 0; l->left -= g; return g; }
typedef struct { uint64_t n; } cnt_t;
typedef struct { const uint8_t *p; size_t n, pos; } memrd_t;
static size_t mem_rd(void *c, void *b, size_t n) { memrd_t *m = (memrd_t *)c; if (n > m->n - m->pos) n = m->n - m->pos; memcpy(b, m->p + m->pos, n); m->pos += n; return n; }
static int cnt_wr(void *c, const void *b, size_t n) { (void)b; ((cnt_t *)c)->n += n; return 0; }
typedef struct { char line[1024]; size_t len; } linebuf_t;                         /* zcat: text to the terminal, line by line */
static int line_wr(void *c, const void *b, size_t n) {
    linebuf_t *l = (linebuf_t *)c; const unsigned char *p = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) { if (p[i] == '\n' || l->len >= sizeof l->line - 1) { l->line[l->len] = 0; pr("%s", l->line); l->len = 0; if (p[i] == '\n') continue; } if (p[i] != '\r') l->line[l->len++] = (char)p[i]; }
    return 0;
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static int ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcmp(s + a - b, suf); }
static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v & 0xFFFF); le16(p + 2, v >> 16); }
static void le64(uint8_t *p, uint64_t v) { le32(p, (uint32_t)v); le32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static const char *err_text(int rc) { return rc == 1 ? "the data ends too early" : rc == 2 ? "the data is damaged" : rc == 3 ? "cannot write (disk full?)" : rc == 4 ? "out of memory" : "error"; }

/* ---- gzip ---------------------------------------------------------------------------------------------------------------------------------------- */
static int gz_compress(const char *src, const char *dst, int level) {
    afile *in = a_open(src, 0); if (!in) { pr("gzip: cannot read %s", src); return 1; }
    { uint64_t sz0 = 0; a_stat(src, &sz0, NULL); if (!g_prog_total || g_prog_done >= g_prog_total) prog_total(g_prog_done + sz0); else prog_add_total(0); prog_name(base_name(src)); }
    afile *out = a_open(dst, 1); if (!out) { a_close(in); pr("gzip: cannot write %s", dst); return 1; }
    const char *nm = base_name(src); uint8_t h[10] = { 0x1F, 0x8B, 8, 8, 0, 0, 0, 0, 0, 3 };
    le32(h + 4, (uint32_t)a_now());
    int rc = a_write(out, h, 10) || a_write(out, nm, strlen(nm) + 1);
    uint32_t crc = 0; uint64_t size = 0, cs = 0;
    if (!rc) rc = arc_deflate(fp_rd, in, f_wr, out, &crc, &size, &cs, level);
    uint8_t t[8]; le32(t, crc); le32(t + 4, (uint32_t)size);
    if (!rc) rc = a_write(out, t, 8);
    a_close(in); if (a_close(out)) rc = 3;
    if (rc) { a_unlink(dst); pr("gzip: %s: %s", src, err_text(rc)); return 1; }
    return 0;
}

/* One gzip member: header, deflate stream, trailer. wr gets the data. Returns 0 ok, 1 not gzip / bad, and sets *more if bytes follow. */
static int gz_member(arc_in *in, arc_write_fn wr, void *wctx, uint64_t *total, const char *what) {
    uint8_t h[10]; if (arc_in_read(in, h, 10) != 10 || h[0] != 0x1F || h[1] != 0x8B || h[2] != 8) { pr("%s: not in gzip format", what); return 1; }
    int flg = h[3];
    if (flg & 4) { uint8_t x[2]; if (arc_in_read(in, x, 2) != 2) return 1; size_t n = rd16(x); while (n--) if (arc_in_getc(in) < 0) return 1; }
    if (flg & 8) { int c; while ((c = arc_in_getc(in)) > 0) {} }
    if (flg & 16) { int c; while ((c = arc_in_getc(in)) > 0) {} }
    if (flg & 2) { arc_in_getc(in); arc_in_getc(in); }
    uint32_t crc = 0; uint64_t size = 0;
    int rc = arc_inflate_in(in, wr, wctx, &crc, &size);
    if (rc) { pr("%s: %s", what, err_text(rc)); return 1; }
    uint8_t t[8]; if (arc_in_read(in, t, 8) != 8) { pr("%s: the end of the file is missing", what); return 1; }
    if (rd32(t) != crc || rd32(t + 4) != (uint32_t)size) { pr("%s: checksum error (the file is damaged)", what); return 1; }
    *total += size; return 0;
}
static int gz_all(arc_in *in, arc_write_fn wr, void *wctx, const char *what) {
    uint64_t total = 0; int first = 1;
    for (;;) {
        int c = arc_in_getc(in); if (c < 0) break;
        in->pos--;                                                                                 /* peek */
        if (!first && c != 0x1F) break;                                                            /* trailing bytes after the last member are ignored, as gzip does */
        if (gz_member(in, wr, wctx, &total, what)) return 1;
        first = 0;
    }
    if (first) { pr("%s: not in gzip format", what); return 1; }
    return 0;
}
static int gz_decompress(const char *src, const char *dst) {
    afile *f = a_open(src, 0); if (!f) { pr("gunzip: cannot read %s", src); return 1; }
    afile *out = a_open(dst, 1); if (!out) { a_close(f); pr("gunzip: cannot write %s", dst); return 1; }
    { uint64_t sz0 = 0; a_stat(src, &sz0, NULL); prog_total(sz0); prog_name(base_name(src)); }
    arc_in *in = (arc_in *)malloc(sizeof *in); arc_in_init(in, fp_rd, f);
    int rc = gz_all(in, f_wr, out, src);
    free(in); a_close(f); if (a_close(out)) rc = 1;
    if (rc) a_unlink(dst);
    return rc;
}

static int cmd_gzip(int argc, char **argv, int decompress, int tocat) {
    int keep = tocat, level = 6, nfiles = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) { if (*p == 'k') keep = 1; else if (*p == 'd') decompress = 1; else if (*p >= '1' && *p <= '9') level = *p - '0'; else if (*p == 'c') tocat = 1, keep = 1; else if (*p == 'q' || *p == 'f' || *p == 'v') {} else { pr("%s: unknown option -%c", argv[0], *p); return 1; } }
            continue;
        }
        nfiles++;
        char path[2300], dst[2400]; join(a, path, sizeof path);
        uint64_t sz; int isd; if (a_stat(path, &sz, &isd) != 0) { pr("%s: %s: no such file", argv[0], a); rc = 1; continue; }
        if (isd) { pr("%s: %s is a folder (use tar or zip)", argv[0], a); rc = 1; continue; }
        if (tocat) {
            afile *f = a_open(path, 0); if (!f) { rc = 1; continue; }
            arc_in *in = (arc_in *)malloc(sizeof *in); arc_in_init(in, f_rd, f); linebuf_t *lb = (linebuf_t *)calloc(1, sizeof *lb);
            if (gz_all(in, line_wr, lb, a)) rc = 1; else if (lb->len) { lb->line[lb->len] = 0; pr("%s", lb->line); }
            free(lb); free(in); a_close(f); continue;
        }
        if (decompress) {
            size_t l = strlen(path);
            if (ends_with(path, ".gz") && l > 3) { memcpy(dst, path, l - 3); dst[l - 3] = 0; }
            else if (ends_with(path, ".tgz")) { memcpy(dst, path, l - 4); strcpy(dst + l - 4, ".tar"); }
            else { pr("gunzip: %s: unknown suffix (expected .gz or .tgz)", a); rc = 1; continue; }
            if (a_stat(dst, NULL, NULL) == 0) { pr("gunzip: %s already exists", dst); rc = 1; continue; }
            if (gz_decompress(path, dst)) { rc = 1; continue; }
            if (!g_quiet) pr("%s -> %s", a, base_name(dst));
        } else {
            if (ends_with(path, ".gz")) { pr("gzip: %s already has the .gz suffix", a); rc = 1; continue; }
            snprintf(dst, sizeof dst, "%s.gz", path);
            if (a_stat(dst, NULL, NULL) == 0) { pr("gzip: %s already exists", dst); rc = 1; continue; }
            if (gz_compress(path, dst, level)) { rc = 1; continue; }
            uint64_t csz = 0; a_stat(dst, &csz, NULL);
            if (!g_quiet) pr("%s -> %s.gz  (%llu -> %llu bytes)", a, base_name(path), (unsigned long long)sz, (unsigned long long)csz);
        }
        if (!keep) a_unlink(path);
    }
    if (!nfiles) { pr("usage: %s [-k] FILE...", argv[0]); return 1; }
    return rc;
}

/* ---- tar: writing ------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { afile *out; int verbose; uint64_t files; int err; } tarw_t;
static void tar_octal(char *p, size_t n, uint64_t v) { for (size_t i = n - 1; i-- > 0; ) { p[i] = (char)('0' + (v & 7)); v >>= 3; } p[n - 1] = 0; }
static int tar_header(tarw_t *w, const char *name, char type, uint64_t size, int mode) {
    uint8_t h[512]; memset(h, 0, sizeof h);
    size_t nl = strlen(name); char nm[1100]; snprintf(nm, sizeof nm, "%s%s", name, type == '5' && (nl == 0 || name[nl - 1] != '/') ? "/" : "");
    nl = strlen(nm);
    const char *use = nm; char pre[160] = "";
    if (nl > 100) {                                                                                          /* split at a "/" so that prefix <= 155 and name <= 100, else a pax header */
        int ok = 0; for (size_t i = nl > 156 ? nl - 156 : 0; i < nl; i++) if (nm[i] == '/' && i <= 155 && nl - i - 1 <= 100 && nl - i - 1 > 0) { memcpy(pre, nm, i); pre[i] = 0; use = nm + i + 1; ok = 1; break; }
        if (!ok) {
            char rec[1400]; size_t body = nl + 7; char digits[8]; size_t dl = 1; size_t len = body + dl;
            for (;;) { dl = (size_t)snprintf(digits, sizeof digits, "%zu", len); if (body + dl == len) break; len = body + dl; }
            int n = snprintf(rec, sizeof rec, "%zu path=%s\n", len, nm);
            uint8_t px[512]; memset(px, 0, sizeof px); snprintf((char *)px, 100, "pax_header"); tar_octal((char *)px + 100, 8, 0644); tar_octal((char *)px + 108, 8, 0); tar_octal((char *)px + 116, 8, 0);
            tar_octal((char *)px + 124, 12, (uint64_t)n); tar_octal((char *)px + 136, 12, a_now()); px[156] = 'x'; memcpy(px + 257, "ustar", 6); memcpy(px + 263, "00", 2);
            memset(px + 148, ' ', 8); uint32_t sum = 0; for (int i = 0; i < 512; i++) sum += px[i]; snprintf((char *)px + 148, 7, "%06o", sum); px[155] = ' ';
            if (a_write(w->out, px, 512) || a_write(w->out, rec, (size_t)n)) return 1;
            size_t pad = (512 - (size_t)n % 512) % 512; uint8_t z[512] = { 0 }; if (a_write(w->out, z, pad)) return 1;
            use = nm; nm[99] = 0; use = nm;                                                                 /* the ordinary header carries a cut-off name */
        }
    }
    snprintf((char *)h, 100, "%s", use);
    tar_octal((char *)h + 100, 8, (uint64_t)mode); tar_octal((char *)h + 108, 8, 0); tar_octal((char *)h + 116, 8, 0);
    if (size >= (1ull << 33)) { h[124] = 0x80; for (int i = 0; i < 8; i++) h[135 - i] = (uint8_t)(size >> (8 * i)); } else tar_octal((char *)h + 124, 12, size);
    tar_octal((char *)h + 136, 12, a_now()); h[156] = (uint8_t)type; memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
    snprintf((char *)h + 345, 156, "%s", pre);
    memset(h + 148, ' ', 8); uint32_t sum = 0; for (int i = 0; i < 512; i++) sum += h[i]; snprintf((char *)h + 148, 7, "%06o", sum); h[155] = ' ';
    return a_write(w->out, h, 512);
}
static int tar_add(tarw_t *w, const char *fs_path, const char *name);
typedef struct { tarw_t *w; const char *fs_path, *name; } tardir_t;
static int tar_add_child(void *c, const char *child, int isdir) {
    tardir_t *d = (tardir_t *)c; (void)isdir; if (d->w->err) return 0;
    char fp[1100], nm[1100]; snprintf(fp, sizeof fp, "%s/%s", d->fs_path, child); snprintf(nm, sizeof nm, "%s/%s", d->name, child);
    if (tar_add(d->w, fp, nm)) d->w->err = 1;
    return 0;
}
static int tar_add(tarw_t *w, const char *fs_path, const char *name) {
    uint64_t size = 0; int isd = 0;
    if (a_stat(fs_path, &size, &isd) != 0) { pr("tar: %s: no such file", fs_path); return 1; }
    if (isd) {
        if (tar_header(w, name, '5', 0, 0755)) return 1;
        if (w->verbose) pr("%s/", name);
        tardir_t d = { w, fs_path, name }; a_listdir(fs_path, tar_add_child, &d);
        return w->err;
    }
    afile *f = a_open(fs_path, 0); if (!f) { pr("tar: cannot read %s", fs_path); return 1; }
    prog_name(name);
    if (tar_header(w, name, '0', size, 0644)) { a_close(f); return 1; }
    uint8_t *buf = (uint8_t *)malloc(65536); uint64_t left = size; int rc = 0;
    while (left && !rc) {
        size_t want = left > 65536 ? 65536 : (size_t)left, got = a_read(f, buf, want);
        if (got == 0) { rc = 1; pr("tar: %s changed while it was read", fs_path); break; }
        rc = a_write(w->out, buf, got); left -= got; prog_add(got);
    }
    if (!rc) { size_t pad = (512 - (size_t)(size % 512)) % 512; memset(buf, 0, 512); rc = a_write(w->out, buf, pad); }
    free(buf); a_close(f); w->files++;
    if (w->verbose && !rc) pr("%s", name);
    return rc;
}

/* ---- tar: reading (a push parser: the same code reads a plain tar and the output of the inflate) -------------------------------------------------- */
typedef struct {
    int list, verbose; char dest[1024];
    int state;                     /* 0 header, 1 data, 2 padding, 3 long name, 4 pax */
    uint8_t hdr[512]; size_t hn;
    uint64_t left, pad; afile *out; char curname[1100], longname[1100], paxname[1100]; int skip, have_long, have_pax; uint64_t paxsize; int have_paxsize;
    char *meta; size_t metan, metacap; int zero_blocks, done; uint64_t files, skipped; char type; uint64_t cursize;
    const char **want; int nwant; int err;
} tarr_t;

static int tar_name_wanted(tarr_t *t, const char *name) {
    if (!t->nwant) return 1;
    for (int i = 0; i < t->nwant; i++) { size_t l = strlen(t->want[i]); while (l > 1 && t->want[i][l - 1] == '/') l--; if (!strncmp(name, t->want[i], l) && (name[l] == 0 || name[l] == '/')) return 1; }
    return 0;
}
static uint64_t tar_num(const uint8_t *p, size_t n) {
    if (p[0] & 0x80) { uint64_t v = p[0] & 0x7F; for (size_t i = 1; i < n; i++) v = (v << 8) | p[i]; return v; }
    uint64_t v = 0; size_t i = 0; while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = (v << 3) | (uint64_t)(p[i] - '0');
    return v;
}
static void tar_pax_parse(tarr_t *t) {
    size_t i = 0;
    while (i < t->metan) {
        size_t len = 0, j = i; while (j < t->metan && t->meta[j] >= '0' && t->meta[j] <= '9') len = len * 10 + (size_t)(t->meta[j++] - '0');
        if (j >= t->metan || t->meta[j] != ' ' || len == 0 || i + len > t->metan) break;
        const char *kv = t->meta + j + 1; size_t kl = len - (j - i) - 2;                                  /* "key=value" without the final newline */
        if (kl > 5 && !strncmp(kv, "path=", 5) && kl - 5 < sizeof t->paxname) { memcpy(t->paxname, kv + 5, kl - 5); t->paxname[kl - 5] = 0; t->have_pax = 1; }
        else if (kl > 5 && !strncmp(kv, "size=", 5)) { t->paxsize = 0; for (size_t k = 5; k < kl; k++) t->paxsize = t->paxsize * 10 + (uint64_t)(kv[k] - '0'); t->have_paxsize = 1; }
        i += len;
    }
}
static int tar_begin_entry(tarr_t *t) {                                                    /* a complete header is in t->hdr */
    uint8_t *h = t->hdr; int zero = 1; for (int i = 0; i < 512; i++) if (h[i]) { zero = 0; break; }
    if (zero) { if (++t->zero_blocks >= 2) t->done = 1; return 0; }
    t->zero_blocks = 0;
    uint32_t sum = 0, want = (uint32_t)tar_num(h + 148, 8); for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != want) { pr("tar: the archive is damaged (bad header checksum)"); t->err = 1; return 1; }
    char type = (char)h[156]; uint64_t size = tar_num(h + 124, 12);
    if (type == 'L' || type == 'x') { t->state = type == 'L' ? 3 : 4; t->left = size; t->metan = 0; t->pad = (512 - size % 512) % 512; if (!size) t->state = 0; return 0; }
    if (type == 'g' || type == 'K') { t->state = 1; t->left = size; t->pad = (512 - size % 512) % 512; t->skip = 1; t->out = NULL; if (!size && !t->pad) t->state = 0; else if (!size) t->state = 2; return 0; }
    char name[1100];
    if (t->have_long) snprintf(name, sizeof name, "%s", t->longname);
    else if (t->have_pax) snprintf(name, sizeof name, "%s", t->paxname);
    else { char nm[101], pre[156]; memcpy(nm, h, 100); nm[100] = 0; memcpy(pre, h + 345, 155); pre[155] = 0; if (pre[0] && !memcmp(h + 257, "ustar", 5)) snprintf(name, sizeof name, "%s/%s", pre, nm); else snprintf(name, sizeof name, "%s", nm); }
    if (t->have_paxsize) size = t->paxsize;
    t->have_long = t->have_pax = t->have_paxsize = 0;
    t->type = type; t->cursize = size; t->left = size; t->pad = (512 - size % 512) % 512; t->skip = 1; t->out = NULL;
    snprintf(t->curname, sizeof t->curname, "%s", name);
    int isdir = type == '5' || (type == 0 && name[0] && name[strlen(name) - 1] == '/');
    int isfile = type == '0' || type == 0 || type == '7';
    if (!isdir && !isfile) { if (!t->list) { pr("tar: skipped %s (a link or special file)", name); } t->skipped++; }
    else if (!tar_name_wanted(t, name)) { /* not asked for */ }
    else if (t->list) { if (t->verbose) pr("%10llu  %s", (unsigned long long)size, name); else pr("%s", name); t->files++; }
    else {
        char rel[1100]; if (!safe_name(name, rel, sizeof rel)) { pr("tar: refused %s (the name leaves the target folder)", name); t->skipped++; }
        else {
            char full[2200]; snprintf(full, sizeof full, "%s/%s", t->dest, rel);
            if (isdir) { mkdirs(full); if (t->verbose) pr("%s/", rel); }
            else { parent_dirs(full); t->out = a_open(full, 1); if (!t->out) { pr("tar: cannot create %s (skipped)", rel); t->skipped++; } else { t->skip = 0; t->files++; if (t->verbose) pr("%s", rel); } }
        }
    }
    t->state = t->left ? 1 : (t->pad ? 2 : 0);
    if (!t->left && t->out) { a_close(t->out); t->out = NULL; }
    return 0;
}
static int tar_feed(void *ctx, const void *data, size_t n) {
    tarr_t *t = (tarr_t *)ctx; const uint8_t *p = (const uint8_t *)data;
    while (n && !t->done && !t->err) {
        if (t->state == 0) {
            size_t k = 512 - t->hn; if (k > n) k = n; memcpy(t->hdr + t->hn, p, k); t->hn += k; p += k; n -= k;
            if (t->hn == 512) { t->hn = 0; if (tar_begin_entry(t)) return 1; }
        } else if (t->state == 1) {
            size_t k = n > t->left ? (size_t)t->left : n;
            if (t->out && a_write(t->out, p, k)) { pr("tar: cannot write %s (disk full?)", t->curname); t->err = 1; return 1; }
            p += k; n -= k; t->left -= k;
            if (!t->left) { if (t->out) { a_close(t->out); t->out = NULL; } t->state = t->pad ? 2 : 0; t->skip = 1; }
        } else if (t->state == 2) {
            size_t k = n > t->pad ? (size_t)t->pad : n; p += k; n -= k; t->pad -= k; if (!t->pad) t->state = 0;
        } else {                                                                              /* 3 long name, 4 pax: collect the body */
            size_t k = n > t->left ? (size_t)t->left : n;
            if (t->metan + k > t->metacap) { size_t nc = (t->metan + k) * 2 + 1024; if (nc > (1u << 20)) { pr("tar: header too long"); t->err = 1; return 1; } t->meta = (char *)realloc(t->meta, nc); t->metacap = nc; }
            memcpy(t->meta + t->metan, p, k); t->metan += k; p += k; n -= k; t->left -= k;
            if (!t->left) {
                if (t->state == 3) { size_t l = t->metan < sizeof t->longname - 1 ? t->metan : sizeof t->longname - 1; memcpy(t->longname, t->meta, l); t->longname[l] = 0; t->longname[strlen(t->longname)] = 0; t->have_long = 1; }
                else tar_pax_parse(t);
                t->state = t->pad ? 2 : 0;
            }
        }
    }
    return t->err ? 1 : 0;
}

static int cmd_tar(int argc, char **argv) {
    int mode = 0, z = 0, verbose = 0; const char *archive = NULL, *dir = NULL; const char *paths[512]; int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' || (i == 1 && a[0] != '/' && a[0] != '.' && strpbrk(a, "cxt") && !strchr(a, '.') && !strchr(a, '/'))) {
            const char *p = a[0] == '-' ? a + 1 : a;
            for (; *p; p++) {
                if (*p == 'c' || *p == 'x' || *p == 't') mode = *p;
                else if (*p == 'z') z = 1;
                else if (*p == 'v') verbose = 1;
                else if (*p == 'f') { if (i + 1 < argc) archive = argv[++i]; }
                else if (*p == 'C') { if (i + 1 < argc) dir = argv[++i]; }
                else if (*p == 'j' || *p == 'J') { pr("tar: bzip2 and xz are not supported (use -z for gzip)"); return 2; }
                else { pr("tar: unknown option -%c", *p); return 2; }
            }
        } else if (np < 512) paths[np++] = a;
    }
    if (!mode || !archive) { pr("usage: tar -c|-x|-t [-z] [-v] -f ARCHIVE [-C DIR] [PATH...]"); return 2; }
    char apath[2300]; join(archive, apath, sizeof apath);
    char base[2300]; if (dir) join(dir, base, sizeof base); else snprintf(base, sizeof base, "%s", g_cwd);
    if (mode == 'c') {
        if (!np) { pr("tar: nothing to add (name the files and folders)"); return 2; }
        char tmp[2400]; const char *target = apath;
        if (z) { snprintf(tmp, sizeof tmp, "%s.part", apath); target = tmp; }
        tarw_t w; memset(&w, 0, sizeof w); w.verbose = verbose; w.out = a_open(target, 1);
        if (!w.out) { pr("tar: cannot write %s", target); return 1; }
        int rc = 0;
        { uint64_t S = 0; for (int i = 0; i < np; i++) { char fp0[2300]; if (dir && paths[i][0] != '/') snprintf(fp0, sizeof fp0, "%s/%s", base, paths[i]); else join(paths[i], fp0, sizeof fp0); S += scan_total(fp0); } prog_total(z ? 2 * S : S); }
        for (int i = 0; i < np && !rc; i++) {
            char fp[2300]; if (dir) { char rel[1100]; snprintf(rel, sizeof rel, "%s", paths[i]); if (rel[0] == '/') snprintf(fp, sizeof fp, "%s", rel); else snprintf(fp, sizeof fp, "%s/%s", base, rel); } else join(paths[i], fp, sizeof fp);
            size_t l = strlen(paths[i]); char nm[1100]; snprintf(nm, sizeof nm, "%s", paths[i]); while (l > 1 && nm[l - 1] == '/') nm[--l] = 0;
            const char *name = nm; while (name[0] == '/') name++; while (name[0] == '.' && name[1] == '/') name += 2;
            rc = tar_add(&w, fp, name);
        }
        if (!rc) { uint8_t z2[1024]; memset(z2, 0, sizeof z2); rc = a_write(w.out, z2, sizeof z2); }
        if (a_close(w.out)) rc = 1;
        if (rc) { a_unlink(target); return 1; }
        if (z) {
            int r2 = gz_compress(tmp, apath, 6); a_unlink(tmp);
            if (r2) return 1;
        }
        if (!g_quiet) pr("%s: %llu file(s) added", base_name(apath), (unsigned long long)w.files);
        return 0;
    }
    afile *f = a_open(apath, 0); if (!f) { pr("tar: cannot read %s", apath); return 1; }
    tarr_t *t = (tarr_t *)calloc(1, sizeof *t); t->list = mode == 't'; t->verbose = verbose; snprintf(t->dest, sizeof t->dest, "%s", base); t->want = paths; t->nwant = np;
    if (!t->list) mkdirs(base);
    { uint64_t fsz0 = 0; a_stat(apath, &fsz0, NULL); prog_total(fsz0); }
    arc_in *in = (arc_in *)malloc(sizeof *in); arc_in_init(in, fp_rd, f);
    int c0 = arc_in_getc(in), c1 = arc_in_getc(in); in->pos = in->pos >= 2 ? in->pos - 2 : 0;                  /* peek at the magic: gzip or not */
    int rc;
    if (c0 == 0x1F && c1 == 0x8B) rc = gz_all(in, tar_feed, t, base_name(apath));
    else {
        uint8_t *buf = (uint8_t *)malloc(65536); rc = 0; size_t n;
        while (!rc && (n = arc_in_read(in, buf, 65536)) > 0) rc = tar_feed(t, buf, n);
        free(buf);
    }
    if (t->out) { a_close(t->out); t->out = NULL; }
    if (rc || t->err) rc = 1; else if (t->state != 0 && !t->done && t->hn == 0 && t->left) { pr("tar: the archive ends too early"); rc = 1; }
    if (!rc && !g_quiet && mode == 'x') pr("%llu file(s) extracted%s", (unsigned long long)t->files, t->skipped ? " (some entries were skipped)" : "");
    free(t->meta); free(t); free(in); a_close(f);
    return rc;
}

/* ---- zip ------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { char *name; uint64_t csize, size, off; uint32_t crc, dostime; uint16_t method, flags; int isdir, zip64; } zent_t;
typedef struct { afile *out; uint64_t pos; zent_t *e; int n, cap; int store; int err; } zipw_t;
static int zw(zipw_t *z, const void *b, size_t n) { if (a_write(z->out, b, n)) { z->err = 1; return 1; } z->pos += n; return 0; }
typedef struct { zipw_t *z; } zout_t;
static int zout_wr(void *c, const void *b, size_t n) { return zw(((zout_t *)c)->z, b, n); }
static uint32_t dos_time(uint64_t t) {
    uint64_t days = t / 86400, rem = t % 86400; int64_t z = (int64_t)days + 719468; int64_t era = (z >= 0 ? z : z - 146096) / 146097; uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; int64_t y = (int64_t)yoe + era * 400; uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9; if (m <= 2) y++;
    if (y < 1980) { y = 1980; m = 1; d = 1; }
    return (uint32_t)((((uint32_t)(y - 1980) << 9 | (uint32_t)m << 5 | (uint32_t)d) << 16) | (uint32_t)(rem / 3600) << 11 | (uint32_t)(rem % 3600 / 60) << 5 | (uint32_t)(rem % 60 / 2));
}
static int zip_add_entry(zipw_t *z, const char *fs_path, const char *name, int isdir, uint64_t size, int verbose);
typedef struct { zipw_t *z; const char *fs_path, *name; int verbose; } zdir_t;
static int zip_add_child(void *c, const char *child, int isdir) {
    zdir_t *d = (zdir_t *)c; (void)isdir; if (d->z->err) return 0;
    char fp[1100], nm[1100]; snprintf(fp, sizeof fp, "%s/%s", d->fs_path, child); snprintf(nm, sizeof nm, "%s/%s", d->name, child);
    uint64_t sz = 0; int isd = 0; if (a_stat(fp, &sz, &isd) != 0) return 0;
    if (zip_add_entry(d->z, fp, nm, isd, sz, d->verbose)) d->z->err = 1;
    return 0;
}
static int zip_add_entry(zipw_t *z, const char *fs_path, const char *name, int isdir, uint64_t size, int verbose) {
    if (z->n == z->cap) { z->cap = z->cap ? z->cap * 2 : 64; z->e = (zent_t *)realloc(z->e, sizeof(zent_t) * (size_t)z->cap); }
    zent_t *e = &z->e[z->n]; memset(e, 0, sizeof *e);
    char nm[1100]; snprintf(nm, sizeof nm, "%s%s", name, isdir ? "/" : ""); e->name = strdup(nm); e->isdir = isdir; e->off = z->pos; e->flags = 0x0808; e->dostime = dos_time(a_now());
    e->zip64 = size >= 0xFFFFFFFFull; size_t nl = strlen(nm);
    afile *f = NULL; int store = z->store || isdir;
    if (!isdir) { f = a_open(fs_path, 0); if (!f) { pr("zip: cannot read %s", fs_path); free(e->name); return 1; } }
    if (!isdir && !store && size > 0) {                                                  /* is it worth compressing? try the first 64 KiB: data that does not shrink is stored */
        uint8_t *probe = (uint8_t *)malloc(65536); size_t pn = a_read(f, probe, 65536);
        if (pn >= 4096) { memrd_t m = { probe, pn, 0 }; cnt_t c = { 0 }; uint64_t a1, a2, a3; uint32_t cc; if (arc_deflate(mem_rd, &m, cnt_wr, &c, &cc, &a1, &a2, 1) == 0 && c.n * 100 >= (uint64_t)pn * 97) store = 1; }
        free(probe); a_close(f); f = a_open(fs_path, 0);                                      /* start again from the first byte (a new stream: no dependence on seeking back) */
        if (!f) { pr("zip: cannot read %s", fs_path); free(e->name); return 1; }
    }
    e->method = (store || size == 0) ? 0 : 8; if (isdir) { e->method = 0; e->flags = 0x0800; }
    uint8_t lh[30]; le32(lh, 0x04034b50); le16(lh + 4, e->zip64 ? 45 : 20); le16(lh + 6, e->flags); le16(lh + 8, e->method); le16(lh + 10, e->dostime & 0xFFFF); le16(lh + 12, e->dostime >> 16);
    le32(lh + 14, 0); le32(lh + 18, e->zip64 ? 0xFFFFFFFFu : 0); le32(lh + 22, e->zip64 ? 0xFFFFFFFFu : 0); le16(lh + 26, (uint32_t)nl); le16(lh + 28, e->zip64 ? 20 : 0);
    if (zw(z, lh, 30) || zw(z, nm, nl)) { if (f) a_close(f); return 1; }
    if (e->zip64) { uint8_t x[20]; le16(x, 1); le16(x + 2, 16); le64(x + 4, 0); le64(x + 12, 0); if (zw(z, x, 20)) { a_close(f); return 1; } }
    prog_name(name);
    uint64_t data_start = z->pos; int rc = 0;
    if (isdir) { e->crc = 0; }
    else if (e->method == 8) { zout_t zo = { z }; uint64_t sz2 = 0, cs = 0; rc = arc_deflate(fp_rd, f, zout_wr, &zo, &e->crc, &sz2, &cs, 6); e->size = sz2; }
    else {
        uint8_t *buf = (uint8_t *)malloc(65536); size_t n; uint32_t crc = 0;
        while (!rc && (n = a_read(f, buf, 65536)) > 0) { crc = arc_crc32(crc, buf, n); e->size += n; prog_add(n); rc = zw(z, buf, n); }
        free(buf); e->crc = crc;
    }
    if (f) a_close(f);
    if (rc) { if (!z->err) pr("zip: %s: %s", fs_path, err_text(rc)); z->err = 1; return 1; }
    e->csize = z->pos - data_start;
    if (!e->zip64 && (e->csize >= 0xFFFFFFFFull || e->size >= 0xFFFFFFFFull)) { pr("zip: %s grew past 4 GiB while it was read", fs_path); z->err = 1; return 1; }
    uint8_t dd[24]; size_t dn;
    le32(dd, 0x08074b50); le32(dd + 4, e->crc);
    if (e->zip64) { le64(dd + 8, e->csize); le64(dd + 16, e->size); dn = 24; } else { le32(dd + 8, (uint32_t)e->csize); le32(dd + 12, (uint32_t)e->size); dn = 16; }
    if (zw(z, dd, dn)) return 1;
    z->n++;
    if (verbose) pr("  adding: %s%s", nm, e->method == 8 ? "" : (isdir ? "" : " (stored)"));
    if (isdir) { zdir_t d = { z, fs_path, name, verbose }; a_listdir(fs_path, zip_add_child, &d); }
    return z->err;
}
static int cmd_zip(int argc, char **argv) {
    int rec = 0, store = 0; const char *archive = NULL; const char *paths[512]; int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) { for (const char *p = a + 1; *p; p++) { if (*p == 'r') rec = 1; else if (*p == '0') store = 1; else if (*p == 'q') g_quiet = 1; else if ((*p >= '1' && *p <= '9') || *p == 'v') {} else { pr("zip: unknown option -%c", *p); return 2; } } }
        else if (!archive) archive = a; else if (np < 512) paths[np++] = a;
    }
    if (!archive || !np) { pr("usage: zip [-r] [-0] ARCHIVE.zip PATH..."); return 2; }
    (void)rec;                                                                           /* folders always go in with their contents */
    char apath[2300]; if (ends_with(archive, ".zip")) join(archive, apath, sizeof apath); else { char t[2300]; snprintf(t, sizeof t, "%s.zip", archive); join(t, apath, sizeof apath); }
    char tmp[2400]; snprintf(tmp, sizeof tmp, "%s.part", apath);
    zipw_t z; memset(&z, 0, sizeof z); z.store = store; z.out = a_open(tmp, 1); if (!z.out) { pr("zip: cannot write %s", apath); return 1; }
    int rc = 0;
    { uint64_t S = 0; for (int i = 0; i < np; i++) { char fp0[2300]; join(paths[i], fp0, sizeof fp0); S += scan_total(fp0); } prog_total(S); }
    for (int i = 0; i < np && !rc; i++) {
        char fp[1100]; join(paths[i], fp, sizeof fp); uint64_t sz = 0; int isd = 0;
        if (a_stat(fp, &sz, &isd) != 0) { pr("zip: %s: no such file", paths[i]); rc = 1; break; }
        char nm[1100]; snprintf(nm, sizeof nm, "%s", paths[i]); size_t l = strlen(nm); while (l > 1 && nm[l - 1] == '/') nm[--l] = 0;
        const char *name = nm; while (name[0] == '/') name++; while (name[0] == '.' && name[1] == '/') name += 2;
        rc = zip_add_entry(&z, fp, name, isd, sz, !g_quiet);
    }
    if (!rc) {                                                                          /* central directory */
        uint64_t cd_off = z.pos;
        for (int i = 0; i < z.n && !rc; i++) {
            zent_t *e = &z.e[i]; size_t nl = strlen(e->name); int big = e->zip64 || e->csize >= 0xFFFFFFFFull || e->size >= 0xFFFFFFFFull || e->off >= 0xFFFFFFFFull;
            uint8_t ch[46]; le32(ch, 0x02014b50); le16(ch + 4, (3 << 8) | (big ? 45 : 20)); le16(ch + 6, big ? 45 : 20); le16(ch + 8, e->flags); le16(ch + 10, e->method);
            le16(ch + 12, e->dostime & 0xFFFF); le16(ch + 14, e->dostime >> 16); le32(ch + 16, e->crc);
            le32(ch + 20, big ? 0xFFFFFFFFu : (uint32_t)e->csize); le32(ch + 24, big ? 0xFFFFFFFFu : (uint32_t)e->size); le16(ch + 28, (uint32_t)nl);
            le16(ch + 30, big ? 28 : 0); le16(ch + 32, 0); le16(ch + 34, 0); le16(ch + 36, 0); le32(ch + 38, e->isdir ? ((0040755u << 16) | 0x10) : (0100644u << 16));
            le32(ch + 42, big ? 0xFFFFFFFFu : (uint32_t)e->off);
            rc = zw(&z, ch, 46) || zw(&z, e->name, nl);
            if (!rc && big) { uint8_t x[32]; le16(x, 1); le16(x + 2, 24); le64(x + 4, e->size); le64(x + 12, e->csize); le64(x + 20, e->off); rc = zw(&z, x, 28); }
        }
        uint64_t cd_size = z.pos - cd_off; int need64 = z.n >= 0xFFFF || cd_off >= 0xFFFFFFFFull || cd_size >= 0xFFFFFFFFull;
        if (!rc && need64) {
            uint8_t r[56]; le32(r, 0x06064b50); le64(r + 4, 44); le16(r + 12, 45); le16(r + 14, 45); le32(r + 16, 0); le32(r + 20, 0); le64(r + 24, (uint64_t)z.n); le64(r + 32, (uint64_t)z.n); le64(r + 40, cd_size); le64(r + 48, cd_off);
            uint8_t loc[20]; le32(loc, 0x07064b50); le32(loc + 4, 0); le64(loc + 8, z.pos); le32(loc + 16, 1);
            rc = zw(&z, r, 56) || zw(&z, loc, 20);
        }
        if (!rc) { uint8_t eo[22]; le32(eo, 0x06054b50); le16(eo + 4, 0); le16(eo + 6, 0); le16(eo + 8, z.n >= 0xFFFF ? 0xFFFF : (uint32_t)z.n); le16(eo + 10, z.n >= 0xFFFF ? 0xFFFF : (uint32_t)z.n);
            le32(eo + 12, cd_size >= 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)cd_size); le32(eo + 16, cd_off >= 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)cd_off); le16(eo + 20, 0); rc = zw(&z, eo, 22); }
    }
    if (a_close(z.out)) rc = 1;
    for (int i = 0; i < z.n; i++) free(z.e[i].name);
    free(z.e);
    if (rc || z.err) { a_unlink(tmp); return 1; }
    a_unlink(apath);
    {   /* the finished file takes the real name: write it again by name is not possible without a rename, so use the rename of the platform */
#ifdef COSARC_HOST
        if (rename(tmp, apath)) { pr("zip: cannot finish %s", apath); return 1; }
#else
        if (cos_rename(tmp, apath) != 0) { pr("zip: cannot finish %s", apath); return 1; }
#endif
    }
    if (!g_quiet) pr("%s: %d entr%s", base_name(apath), z.n, z.n == 1 ? "y" : "ies");
    return 0;
}

/* ---- unzip ---------------------------------------------------------------------------------------------------------------------------------------- */
static int cmd_unzip(int argc, char **argv) {
    int list = 0; const char *archive = NULL, *dir = NULL; const char *want[256]; int nwant = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) { for (const char *p = a + 1; *p; p++) { if (*p == 'l' || *p == 'v') list = 1; else if (*p == 'd') { if (i + 1 < argc) dir = argv[++i]; } else if (*p == 'o' || *p == 'q' || *p == 'n') { if (*p == 'q') g_quiet = 1; } else { pr("unzip: unknown option -%c", *p); return 2; } } }
        else if (!archive) archive = a; else if (nwant < 256) want[nwant++] = a;
    }
    if (!archive) { pr("usage: unzip [-l] [-d DIR] ARCHIVE.zip [NAME...]"); return 2; }
    char apath[2300]; join(archive, apath, sizeof apath);
    uint64_t fsize = 0; int isd = 0; if (a_stat(apath, &fsize, &isd) != 0 && ends_with(apath, ".zip") == 0) { char t[2400]; snprintf(t, sizeof t, "%s.zip", apath); snprintf(apath, sizeof apath, "%s", t); }
    if (a_stat(apath, &fsize, &isd) != 0 || isd) { pr("unzip: cannot find %s", archive); return 1; }
    char base[2300]; if (dir) join(dir, base, sizeof base); else snprintf(base, sizeof base, "%s", g_cwd);
    afile *f = a_open(apath, 0); if (!f) { pr("unzip: cannot read %s", apath); return 1; }
    /* the end of central directory: the last 22..65557 bytes */
    size_t tail = fsize < 70000 ? (size_t)fsize : 70000; uint8_t *tb = (uint8_t *)malloc(tail);
    a_seek(f, fsize - tail); if (a_read(f, tb, tail) != tail) { pr("unzip: cannot read the end of %s", archive); free(tb); a_close(f); return 1; }
    long eo = -1; for (long i = (long)tail - 22; i >= 0; i--) if (rd32(tb + i) == 0x06054b50) { eo = i; break; }
    if (eo < 0) { pr("unzip: %s is not a zip file (no end record)", archive); free(tb); a_close(f); return 1; }
    uint64_t total = rd16(tb + eo + 10), cd_size = rd32(tb + eo + 12), cd_off = rd32(tb + eo + 16);
    if ((total == 0xFFFF || cd_size == 0xFFFFFFFFull || cd_off == 0xFFFFFFFFull) && eo >= 20 && rd32(tb + eo - 20) == 0x07064b50) {           /* zip64 */
        uint64_t r64 = rd64(tb + eo - 20 + 8); uint8_t z[56]; a_seek(f, r64);
        if (a_read(f, z, 56) == 56 && rd32(z) == 0x06064b50) { total = rd64(z + 32); cd_size = rd64(z + 40); cd_off = rd64(z + 48); }
    }
    free(tb);
    if (cd_size > (512u << 20)) { pr("unzip: the directory of %s is too big", archive); a_close(f); return 1; }
    uint8_t *cd = (uint8_t *)malloc((size_t)cd_size + 1); a_seek(f, cd_off);
    if (a_read(f, cd, (size_t)cd_size) != (size_t)cd_size) { pr("unzip: the directory of %s is damaged", archive); free(cd); a_close(f); return 1; }
    int rc = 0; uint64_t pos = 0, count = 0, extracted = 0, skipped = 0;
    if (!list) {
        mkdirs(base);
        uint64_t sum = 0, p2 = 0, c2 = 0;                                                   /* the bar's total: the sizes of the entries (a zip64 entry counts by its compressed size field) */
        while (p2 + 46 <= cd_size && c2 < total) { const uint8_t *q = cd + p2; if (rd32(q) != 0x02014b50) break; uint64_t sz2 = rd32(q + 24); if (sz2 != 0xFFFFFFFFull) sum += sz2; p2 += 46 + rd16(q + 28) + rd16(q + 30) + rd16(q + 32); c2++; }
        prog_total(sum);
    }
    while (pos + 46 <= cd_size && count < total) {
        const uint8_t *c = cd + pos; if (rd32(c) != 0x02014b50) { pr("unzip: the directory of %s is damaged", archive); rc = 1; break; }
        uint32_t flags = rd16(c + 8), method = rd16(c + 10), crc = rd32(c + 16); uint64_t csize = rd32(c + 20), size = rd32(c + 24), off = rd32(c + 42);
        size_t nl = rd16(c + 28), el = rd16(c + 30), cl = rd16(c + 32); if (pos + 46 + nl + el + cl > cd_size) { rc = 1; break; }
        char name[1100]; size_t cn = nl < sizeof name - 1 ? nl : sizeof name - 1; memcpy(name, c + 46, cn); name[cn] = 0;
        const uint8_t *x = c + 46 + nl; size_t xl = el;
        while (xl >= 4) { uint32_t id = rd16(x), len = rd16(x + 2); if (4 + len > xl) break; if (id == 1) { const uint8_t *q = x + 4; if (size == 0xFFFFFFFFull && q + 8 <= x + 4 + len) { size = rd64(q); q += 8; } if (csize == 0xFFFFFFFFull && q + 8 <= x + 4 + len) { csize = rd64(q); q += 8; } if (off == 0xFFFFFFFFull && q + 8 <= x + 4 + len) { off = rd64(q); q += 8; } } x += 4 + len; xl -= 4 + len; }
        pos += 46 + nl + el + cl; count++;
        int isdir = nl && name[nl - 1] == '/';
        int wanted = !nwant; for (int i = 0; i < nwant; i++) { size_t l = strlen(want[i]); if (!strncmp(name, want[i], l)) wanted = 1; }
        if (!wanted) continue;
        if (list) { pr("%12llu  %s", (unsigned long long)size, name); continue; }
        char rel[1100]; if (!safe_name(name, rel, sizeof rel)) { pr("unzip: refused %s (the name leaves the target folder)", name); skipped++; continue; }
        char full[2200]; snprintf(full, sizeof full, "%s/%s", base, rel);
        if (isdir) { mkdirs(full); continue; }
        if (flags & 1) { pr("unzip: skipped %s (encrypted)", name); skipped++; continue; }
        if (method != 0 && method != 8) { pr("unzip: skipped %s (compression method %u is not supported)", name, method); skipped++; continue; }
        uint8_t lh[30]; a_seek(f, off); if (a_read(f, lh, 30) != 30 || rd32(lh) != 0x04034b50) { pr("unzip: %s: the local header is damaged", name); rc = 1; continue; }
        a_seek(f, off + 30 + rd16(lh + 26) + rd16(lh + 28));
        parent_dirs(full); afile *out = a_open(full, 1); if (!out) { pr("unzip: cannot create %s", full); rc = 1; continue; }
        prog_name(rel);
        lim_t lim = { f, csize }; uint32_t got_crc = 0; uint64_t got_size = 0; int r;
        if (method == 8) r = arc_inflate(lim_rd, &lim, fp_wr, out, &got_crc, &got_size);
        else { uint8_t *buf = (uint8_t *)malloc(65536); size_t n; r = 0; while (!r && (n = lim_rd(&lim, buf, 65536)) > 0) { got_crc = arc_crc32(got_crc, buf, n); got_size += n; prog_add(n); r = a_write(out, buf, n); } free(buf); if (!r && got_size != csize) r = 1; }
        if (a_close(out)) r = 3;
        if (!r && (got_crc != crc || got_size != size)) { pr("unzip: %s: checksum error (the file is damaged)", name); r = 2; }
        else if (r) pr("unzip: %s: %s", name, err_text(r));
        if (r) { a_unlink(full); rc = 1; } else { extracted++; if (!g_quiet) pr("  extracting: %s", rel); }
    }
    free(cd); a_close(f);
    if (!list && !rc && !g_quiet) pr("%llu file(s) extracted%s", (unsigned long long)extracted, skipped ? " (some entries were skipped)" : "");
    return rc;
}

/* ---- entry point ---------------------------------------------------------------------------------------------------------------------------------- */
int cosarc_is_command(const char *n) { return !strcmp(n, "tar") || !strcmp(n, "gzip") || !strcmp(n, "gunzip") || !strcmp(n, "zcat") || !strcmp(n, "zip") || !strcmp(n, "unzip"); }
int cosarc_main(int argc, char **argv, const char *cwd, void (*print)(const char *line)) {
    g_print = print; g_cwd = cwd && cwd[0] ? cwd : "/"; g_quiet = 0;
    if (argc < 1) return 2;
    const char *c = argv[0];
    if (!strcmp(c, "gzip")) return cmd_gzip(argc, argv, 0, 0);
    if (!strcmp(c, "gunzip")) return cmd_gzip(argc, argv, 1, 0);
    if (!strcmp(c, "zcat")) return cmd_gzip(argc, argv, 1, 1);
    if (!strcmp(c, "tar")) return cmd_tar(argc, argv);
    if (!strcmp(c, "zip")) return cmd_zip(argc, argv);
    if (!strcmp(c, "unzip")) return cmd_unzip(argc, argv);
    return 2;
}
