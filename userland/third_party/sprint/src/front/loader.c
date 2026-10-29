/* Module loader: resolves user imports, loads them depth-first (post-order
 * initialisation) and compiles each into the shared Program (SPEC 14). */
#define _XOPEN_SOURCE 700
#include "sp.h"
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *sp_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap);
    if (!b) { (void)fclose(f); return NULL; }
    for (;;) {
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(b, cap);
            if (!nb) { free(b); (void)fclose(f); return NULL; }
            b = nb;
        }
        size_t r = fread(b + n, 1, cap - n - 1, f);
        if (r == 0) break;
        n += r;
    }
    int bad = ferror(f);
    (void)fclose(f);
    if (bad) { free(b); return NULL; }
    b[n] = 0;
    *len = n;
    return b;
}

/* CRLF/CR -> LF (SPEC 3.1); also strips a leading UTF-8 BOM. */
size_t sp_normalize(char *b, size_t n) {
    size_t r = 0, w = 0;
    if (n >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) r = 3;
    for (; r < n; r++) {
        if (b[r] == '\r') {
            b[w++] = '\n';
            if (r + 1 < n && b[r + 1] == '\n') r++;
        } else {
            b[w++] = b[r];
        }
    }
    b[w] = 0;
    return w;
}

enum { MS_LOADING = 1, MS_DONE = 2 };

typedef struct LMod {
    char *name;
    char *path;
    char *real;
    int state;
} LMod;

typedef struct Loader {
    Front *F;
    VM *vm;
    Program *prog;
    char **roots;
    int nroots;
    LMod *mods;
    int nmods, capmods;
    /* REPL chunk loading */
    int persist;        /* prog belongs to the session: never freed here */
    ReplScope *rs;      /* scope carried across chunks (NULL otherwise) */
    Arena **keep;       /* receives the chunk's AST arena, which the scope points into */
    int chunk_idx;      /* index of the chunk module in mods */
} Loader;

static void *xalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(14);
    return p;
}

static char *xdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = xalloc(n);
    memcpy(p, s, n);
    return p;
}

static char *dir_of(const char *path) {
    const char *sl = strrchr(path, '/');
    if (!sl) return xdup(".");
    if (sl == path) return xdup("/");
    size_t n = (size_t)(sl - path);
    char *d = xalloc(n + 1);
    memcpy(d, path, n);
    d[n] = 0;
    return d;
}

static void add_root(Loader *L, const char *dir) {
    L->roots = realloc(L->roots, sizeof(char *) * (size_t)(L->nroots + 1));
    if (!L->roots) exit(14);
    L->roots[L->nroots++] = xdup(dir);
}

/* Directory of the nearest Sprint.toml at or above `dir`, or NULL (malloc'd). */
static char *find_manifest_dir(const char *dir) {
    char cur[PATH_MAX];
    if (!realpath(dir, cur)) return NULL;
    for (;;) {
        char probe[PATH_MAX + 16];
        (void)snprintf(probe, sizeof probe, "%.*s/Sprint.toml", PATH_MAX - 1, cur);
        FILE *f = fopen(probe, "rb");
        if (f) {
            (void)fclose(f);
            return xdup(cur);
        }
        char *sl = strrchr(cur, '/');
        if (!sl) return NULL;
        if (sl == cur) {
            if (cur[1] == 0) return NULL;
            cur[1] = 0;
        } else {
            *sl = 0;
        }
    }
}

/* source_root = "dir" from a Sprint.toml (only this one key is needed here). */
static char *manifest_source_root(const char *mdir) {
    char probe[PATH_MAX + 16];
    (void)snprintf(probe, sizeof probe, "%.*s/Sprint.toml", PATH_MAX - 1, mdir);
    size_t len = 0;
    char *txt = sp_read_file(probe, &len);
    if (!txt) return NULL;
    char *result = NULL;
    for (char *line = strtok(txt, "\n"); line && !result; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '[') break; /* only the top-level table matters */
        if (strncmp(line, "source_root", 11) != 0) continue;
        char *q = strchr(line, '"');
        char *e = q ? strchr(q + 1, '"') : NULL;
        if (!e) continue;
        *e = 0;
        result = xdup(q + 1);
    }
    free(txt);
    return result;
}

static int under_roots(const Loader *L, const char *real) {
    for (int r = 0; r < L->nroots; r++) {
        char rr[PATH_MAX];
        if (!realpath(L->roots[r], rr)) continue;
        size_t n = strlen(rr);
        if (strncmp(real, rr, n) == 0 && (real[n] == '/' || real[n] == 0 || n == 1)) return 1;
    }
    return 0;
}

static LMod *find_mod(Loader *L, const char *name) {
    for (int i = 0; i < L->nmods; i++)
        if (strcmp(L->mods[i].name, name) == 0) return &L->mods[i];
    return NULL;
}

static LMod *add_mod(Loader *L, const char *name, const char *path) {
    if (L->nmods == L->capmods) {
        L->capmods = L->capmods ? L->capmods * 2 : 8;
        L->mods = realloc(L->mods, sizeof(LMod) * (size_t)L->capmods);
        if (!L->mods) exit(14);
    }
    LMod *m = &L->mods[L->nmods++];
    m->name = xdup(name);
    m->path = xdup(path);
    char rp[PATH_MAX];
    m->real = realpath(path, rp) ? xdup(rp) : xdup(path);
    m->state = MS_LOADING;
    return m;
}

static SP_NORETURN void import_error(Loader *L, const char *file, const Node *n, const char *fmt, ...) SP_PRINTF(4, 5);
static void import_error(Loader *L, const char *file, const Node *n, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    L->F->file = file;
    front_error(L->F, "ImportError", E_IMPORT, n->line, n->col, n->offset, "%s", msg);
}

/* dotted name -> existing file path (malloc'd) or NULL */
static char *resolve(Loader *L, const char *name) {
    size_t nl = strlen(name);
    for (int r = 0; r < L->nroots; r++) {
        size_t cap = strlen(L->roots[r]) + nl + 8;
        char *p = xalloc(cap);
        size_t w = (size_t)snprintf(p, cap, "%s/", L->roots[r]);
        for (size_t i = 0; i < nl; i++) p[w++] = name[i] == '.' ? '/' : name[i];
        memcpy(p + w, ".sp", 4);
        FILE *f = fopen(p, "rb");
        if (f) {
            (void)fclose(f);
            return p;
        }
        free(p);
    }
    return NULL;
}

static void free_ptr(void *p) { free(p); }

const BundledMod *bundled_find(const char *name, size_t len) {
    for (const BundledMod *b = bundled_mods; b->name; b++)
        if (strlen(b->name) == len && memcmp(b->name, name, len) == 0) return b;
    return NULL;
}

/* Everything one module's load owns; released on success or on a front-end error. */
typedef struct ModRes {
    char *src, *name, *path;
    Arena *A;
    TokenList toks;
} ModRes;
static void modres_free(void *p) {
    ModRes *r = p;
    toklist_free(&r->toks);
    if (r->A) arena_free(r->A);
    free(r->src);
    free(r->name);
    free(r->path);
    free(r);
}

static void load_module(Loader *L, LMod *self, char *src, size_t len);

static void load_dep(Loader *L, const char *importer, const Node *n) {
    const char *name = n->str;
    if (lib_find_module(L->vm, name, n->slen)) return; /* built-in module */
    const char *dot = strchr(name, '.');
    if (dot && lib_find_module(L->vm, name, (size_t)(dot - name)))
        import_error(L, importer, n, "'%s' would shadow a built-in module", name);
    LMod *have = find_mod(L, name);
    if (have) {
        if (have->state == MS_LOADING) {
            char chain[200];
            chain[0] = 0;
            int started = 0;
            for (int i = 0; i < L->nmods; i++) {
                if (&L->mods[i] == have) started = 1;
                if (started && L->mods[i].state == MS_LOADING) {
                    size_t u = strlen(chain), k = strlen(L->mods[i].name);
                    if (u + k + 5 < sizeof chain) {
                        memcpy(chain + u, L->mods[i].name, k);
                        memcpy(chain + u + k, " -> ", 5);
                    }
                }
            }
            import_error(L, importer, n, "import cycle: %s%s", chain, name);
        }
        return;
    }
    const BundledMod *bm = bundled_find(name, n->slen);
    if (bm) { /* stdlib module written in Sprint and compiled into the binary */
        char *copy = xalloc(bm->len + 1);
        memcpy(copy, bm->src, bm->len);
        copy[bm->len] = 0;
        front_push(L->F, free_ptr, copy);
        char bpath[80];
        (void)snprintf(bpath, sizeof bpath, "<std>/%s.sp", bm->name);
        LMod *bmod = add_mod(L, name, bpath);
        front_pop(L->F);
        load_module(L, bmod, copy, bm->len);
        return;
    }
    if (L->vm && L->vm->hloader) { /* embedding: the host supplies module sources */
        const char *hs = NULL, *disp = NULL;
        size_t hl = 0;
        int st = L->vm->hloader(L->vm->hloader_ud, name, &hs, &hl, &disp);
        if (st != 0 || !hs) import_error(L, importer, n, "module '%s' not found", name);
        char *copy = xalloc(hl + 1);
        memcpy(copy, hs, hl);
        copy[hl] = 0;
        front_push(L->F, free_ptr, copy);
        hl = sp_normalize(copy, hl);
        size_t hbad;
        char *hpath = xdup(disp ? disp : name);
        front_push(L->F, free_ptr, hpath);
        if (!utf8_validate(copy, hl, &hbad) || memchr(copy, 0, hl)) {
            L->F->file = hpath;
            front_error(L->F, "LexError", E_LEX, 1, 1, 0, "invalid UTF-8 or NUL byte in source");
        }
        LMod *hm = add_mod(L, name, hpath);
        front_pop(L->F);
        front_pop(L->F);
        free(hpath);
        load_module(L, hm, copy, hl);
        return;
    }
    char *path = resolve(L, name);
    if (!path) import_error(L, importer, n, "module '%s' not found", name);
    front_push(L->F, free_ptr, path);
    char rp[PATH_MAX];
    const char *real = realpath(path, rp) ? rp : path;
    if (!under_roots(L, real)) {
        import_error(L, importer, n, "module '%s' resolves outside the search roots", name);
    }
    for (int i = 0; i < L->nmods; i++)
        if (strcmp(L->mods[i].real, real) == 0)
            import_error(L, importer, n, "module '%s' is the same file as module '%s'", name, L->mods[i].name);
    size_t len = 0;
    char *src = sp_read_file(path, &len);
    if (!src) import_error(L, importer, n, "cannot read module '%s'", name);
    front_push(L->F, free_ptr, src);
    len = sp_normalize(src, len);
    size_t bad;
    if (!utf8_validate(src, len, &bad) || memchr(src, 0, len)) {
        L->F->file = path;
        front_error(L->F, "LexError", E_LEX, 1, 1, 0, "invalid UTF-8 or NUL byte in source");
    }
    LMod *m = add_mod(L, name, path);
    front_pop(L->F); /* src: owned by load_module from here on */
    front_pop(L->F); /* path */
    free(path);
    load_module(L, m, src, len);
}

static void load_module(Loader *L, LMod *self, char *src, size_t len) {
    size_t idx = (size_t)(self - L->mods);
    ModRes *R = xalloc(sizeof *R);
    memset(R, 0, sizeof *R);
    R->src = src;
    R->name = xdup(self->name);
    R->path = xdup(self->path);
    R->A = arena_new();
    front_push(L->F, modres_free, R);
    const char *name = R->name;
    const char *path = R->path;
    L->F->file = path;
    lex_source(L->F, src, len, &R->toks);
    Node *root = parse_program(L->F, R->A, &R->toks);
    if (L->rs && (int)idx == L->chunk_idx && replscope_wants_echo(L->rs) && root->n == 1 &&
        root->list[0]->kind == S_EXPR) {
        /* REPL echo: `expr` becomes `_ = expr` so the host can print the value afterwards */
        Node *e = root->list[0];
        Node *nm = arena_alloc(R->A, sizeof *nm);
        Node *as = arena_alloc(R->A, sizeof *as);
        memset(nm, 0, sizeof *nm);
        memset(as, 0, sizeof *as);
        nm->kind = N_NAME;
        nm->line = e->line;
        nm->col = e->col;
        nm->offset = e->offset;
        nm->str = arena_strndup(R->A, "_", 1);
        nm->slen = 1;
        *as = *e;
        as->kind = S_ASSIGN;
        as->op = TK_ASSIGN;
        as->a = nm;
        as->b = e->a;
        root->list[0] = as;
        replscope_set_echoed(L->rs);
    }
    for (int i = 0; i < root->n; i++) {
        const Node *s = root->list[i];
        if (s->kind == S_IMPORT || s->kind == S_FROM) load_dep(L, path, s);
    }
    L->F->file = path;
    if (L->rs && (int)idx == L->chunk_idx) {
        compile_module_scoped(L->F, L->vm, L->prog, root, name, path, L->rs);
        *L->keep = R->A; /* the scope now points into this arena */
        R->A = NULL;
    } else {
        compile_module(L->F, L->vm, L->prog, root, name, path);
    }
    {
        ModInfo *mi = &L->prog->mods[L->prog->nmods - 1];
        const BundledMod *bm = strncmp(path, "<std>/", 6) == 0 ? bundled_find(name, strlen(name)) : NULL;
        if (bm) {
            mi->lazy_src = bm->src; /* static data: hash it only if an image is written */
            mi->lazy_len = bm->len;
        } else {
            sp_sha256(src, len, mi->sha);
        }
        if (idx == 0) {
            const char *base = strrchr(path, '/');
            (void)snprintf(mi->rel, sizeof mi->rel, "%s", base ? base + 1 : path);
        } else {
            size_t k = 0;
            for (const char *q = name; *q && k + 4 < sizeof mi->rel; q++) mi->rel[k++] = *q == '.' ? '/' : *q;
            memcpy(mi->rel + k, ".sp", 4);
        }
    }
    L->mods[idx].state = MS_DONE;
    front_pop(L->F);
    modres_free(R);
}

/* Embedding: compile the entry module and everything it imports; all sources
 * come from the host's loader callback (vm->hloader). */
static void loader_abort(void *p) {
    Loader *L = p;
    for (int i = 0; i < L->nmods; i++) {
        free(L->mods[i].name);
        free(L->mods[i].path);
        free(L->mods[i].real);
    }
    free(L->mods);
    for (int i = 0; i < L->nroots; i++) free(L->roots[i]);
    free(L->roots);
    if (!L->persist) program_free(L->prog);
}

Program *load_program_host(Front *F, VM *vm, const char *disp_path, char *src, size_t len) {
    Loader L;
    memset(&L, 0, sizeof L);
    L.F = F;
    L.vm = vm;
    L.prog = program_new();
    L.prog->hostvm = vm;
    front_push(F, loader_abort, &L);
    (void)snprintf(L.prog->file, sizeof L.prog->file, "%s", disp_path);
    LMod *m = add_mod(&L, "main", disp_path);
    load_module(&L, m, src, len);
    front_pop(F);
    front_done(F);
    Program *pg = L.prog;
    for (int i = 0; i < L.nmods; i++) {
        free(L.mods[i].name);
        free(L.mods[i].path);
        free(L.mods[i].real);
    }
    free(L.mods);
    return pg;
}

Program *load_program(Front *F, VM *vm, const char *path, char *src, size_t len) {
    Loader L;
    memset(&L, 0, sizeof L);
    L.F = F;
    L.vm = vm;
    L.prog = program_new();
    L.prog->hostvm = vm;
    front_push(F, loader_abort, &L);
    char *d = dir_of(path);
    char *mdir = find_manifest_dir(d);
    char *sroot = mdir ? manifest_source_root(mdir) : NULL;
    if (mdir && sroot) {
        size_t ml = strlen(mdir), sl = strlen(sroot);
        char *full = xalloc(ml + sl + 2);
        memcpy(full, mdir, ml);
        full[ml] = '/';
        memcpy(full + ml + 1, sroot, sl + 1);
        add_root(&L, full);
        free(full);
    } else {
        add_root(&L, d);
    }
    free(mdir);
    free(sroot);
    free(d);
    const char *sp = getenv("SPRINT_PATH");
    if (sp) {
        char *copy = xdup(sp);
        for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":"))
            if (*tok) add_root(&L, tok);
        free(copy);
    }
    (void)snprintf(L.prog->file, sizeof L.prog->file, "%s", path);
    LMod *m = add_mod(&L, "main", path);
    load_module(&L, m, src, len);
    front_pop(F);
    front_done(F);
    Program *pg = L.prog;
    for (int i = 0; i < L.nmods; i++) {
        free(L.mods[i].name);
        free(L.mods[i].path);
        free(L.mods[i].real);
    }
    free(L.mods);
    for (int i = 0; i < L.nroots; i++) free(L.roots[i]);
    free(L.roots);
    return pg;
}

/* REPL: compile `src` as one more module ("chunk") of the session program `prog`. Imports are
 * resolved against the working directory (and SPRINT_PATH) like `sprint run`; modules loaded by
 * earlier chunks are reused. Takes ownership of `src`. Longjmps through F on error, leaving the
 * session program and scope unchanged except for unreferenced garbage. *keep receives the AST
 * arena that the scope refers to (the caller frees it when the session ends). */
void load_chunk(Front *F, VM *vm, Program *prog, ReplScope *rs, Arena **keep, char *src, size_t len, const char *name) {
    Loader L;
    memset(&L, 0, sizeof L);
    L.F = F;
    L.vm = vm;
    L.prog = prog;
    L.persist = 1;
    L.rs = rs;
    L.keep = keep;
    front_push(F, loader_abort, &L);
    add_root(&L, ".");
    const char *sp = getenv("SPRINT_PATH");
    if (sp) {
        char *copy = xdup(sp);
        for (char *tok = strtok(copy, ":"); tok; tok = strtok(NULL, ":"))
            if (*tok) add_root(&L, tok);
        free(copy);
    }
    for (int i = 0; i < prog->nmods; i++) {
        LMod *m = add_mod(&L, prog->mods[i].name, prog->mods[i].file);
        m->state = MS_DONE;
    }
    LMod *m = add_mod(&L, name, "<repl>");
    L.chunk_idx = (int)(m - L.mods);
    load_module(&L, m, src, len);
    front_pop(F);
    front_done(F);
    for (int i = 0; i < L.nmods; i++) {
        free(L.mods[i].name);
        free(L.mods[i].path);
        free(L.mods[i].real);
    }
    free(L.mods);
    for (int i = 0; i < L.nroots; i++) free(L.roots[i]);
    free(L.roots);
}
