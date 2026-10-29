/* Compiler: bytecode emission, constants, scopes, module variables and name resolution. */
#include "compiler_int.h"

/* ---------------------------------------------------------------- */
/* helpers                                                           */
/* ---------------------------------------------------------------- */
void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) {
        (void)fputs("out of memory\n", stderr);
        exit(14);
    }
    return q;
}

void cerr(Comp *c, const Node *n, const char *kind, int code, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    front_error(c->F, kind, code, n->line, n->col, n->offset, "%s", msg);
}

void setpos(Comp *c, const Node *n) {
    c->fs->cur_line = n->line;
    c->fs->cur_col = n->col;
}

Proto *new_proto(Comp *c, const char *name) {
    Program *pg = c->prog;
    if (pg->nprotos == pg->capprotos) {
        pg->capprotos = pg->capprotos ? pg->capprotos * 2 : 16;
        pg->protos = xrealloc(pg->protos, sizeof(Proto *) * (size_t)pg->capprotos);
    }
    Proto *p = calloc(1, sizeof *p);
    if (!p) exit(14);
    if (name) {
        p->name = malloc(strlen(name) + 1);
        if (!p->name) exit(14);
        memcpy(p->name, name, strlen(name) + 1);
    }
    p->mod = c->cur_mod;
    pg->protos[pg->nprotos++] = p;
    return p;
}

int proto_index(const Comp *c, const Proto *p) {
    for (int i = 0; i < c->prog->nprotos; i++)
        if (c->prog->protos[i] == p) return i;
    return -1;
}

/* ---- emission ---- */
static void adj(Comp *c, int d) {
    FS *fs = c->fs;
    fs->depth += d;
    if (fs->depth > fs->max_depth) fs->max_depth = fs->depth;
}

void e8(Comp *c, unsigned b) {
    Proto *p = c->fs->p;
    if (p->code_len == p->code_cap) {
        p->code_cap = p->code_cap ? p->code_cap * 2 : 64;
        p->code = xrealloc(p->code, p->code_cap);
    }
    p->code[p->code_len++] = (uint8_t)b;
}
void e16(Comp *c, unsigned v) {
    e8(c, v & 0xFFu);
    e8(c, (v >> 8) & 0xFFu);
}
void e32(Comp *c, uint32_t v) {
    e16(c, v & 0xFFFFu);
    e16(c, v >> 16);
}

void emit_op(Comp *c, int op, int delta) {
    FS *fs = c->fs;
    Proto *p = fs->p;
    if (p->nlines == 0 || fs->cur_line != fs->last_line || fs->cur_col != fs->last_col) {
        if (p->nlines == p->caplines) {
            p->caplines = p->caplines ? p->caplines * 2 : 32;
            p->lines = xrealloc(p->lines, sizeof(LineEnt) * (size_t)p->caplines);
        }
        p->lines[p->nlines].pc = p->code_len;
        p->lines[p->nlines].line = fs->cur_line;
        p->lines[p->nlines].col = fs->cur_col;
        p->nlines++;
        fs->last_line = fs->cur_line;
        fs->last_col = fs->cur_col;
    }
    e8(c, (unsigned)op);
    adj(c, delta);
}

void op0(Comp *c, int op, int delta) { emit_op(c, op, delta); }
void op_u16(Comp *c, int op, unsigned v, int delta) {
    emit_op(c, op, delta);
    e16(c, v);
}
void op_u8(Comp *c, int op, unsigned v, int delta) {
    emit_op(c, op, delta);
    e8(c, v);
}

/* emit a jump; returns the offset of its i32 operand for patching */
uint32_t jump_fwd(Comp *c, int op, int delta) {
    emit_op(c, op, delta);
    uint32_t at = c->fs->p->code_len;
    e32(c, 0);
    return at;
}
void patch_here(Comp *c, uint32_t at) {
    Proto *p = c->fs->p;
    int32_t off = (int32_t)(p->code_len - (at + 4));
    uint32_t u = (uint32_t)off;
    p->code[at] = (uint8_t)(u & 0xFF);
    p->code[at + 1] = (uint8_t)((u >> 8) & 0xFF);
    p->code[at + 2] = (uint8_t)((u >> 16) & 0xFF);
    p->code[at + 3] = (uint8_t)((u >> 24) & 0xFF);
}
void jump_back(Comp *c, int op, uint32_t target, int delta) {
    emit_op(c, op, delta);
    int32_t off = (int32_t)target - (int32_t)(c->fs->p->code_len + 4);
    e32(c, (uint32_t)off);
}

int add_const(Comp *c, SpValue v) {
    Proto *p = c->fs->p;
    if (!c->dry) {
        for (int i = 0; i < p->nconsts; i++) {
            SpValue o = p->consts[i];
            if (o.type != v.type) continue;
            if (v.type == T_STRING ? value_eq(o, v)
                : v.type == T_INT   ? o.as.i == v.as.i
                : v.type == T_FLOAT ? memcmp(&o.as.f, &v.as.f, sizeof(double)) == 0
                                    : 0)
                return i;
        }
    }
    if (p->nconsts >= 65535) front_error(c->F, "ParseError", E_PARSE, c->fs->cur_line, c->fs->cur_col, 0, "too many constants in one function");
    if (p->nconsts == p->capconsts) {
        p->capconsts = p->capconsts ? p->capconsts * 2 : 16;
        p->consts = xrealloc(p->consts, sizeof(SpValue) * (size_t)p->capconsts);
    }
    p->consts[p->nconsts] = v;
    return p->nconsts++;
}

int str_const(Comp *c, const char *s, size_t len) {
    if (c->dry) return add_const(c, v_nil());
    return add_const(c, vm_perm_string(c->vm, s, len));
}

void emit_const_str(Comp *c, const char *s, size_t len) {
    op_u16(c, OP_CONST, (unsigned)str_const(c, s, len), 1);
}

int add_link(Comp *c, int kind, const char *module, const char *member) {
    Program *pg = c->prog;
    for (int i = 0; i < pg->nlinks; i++) {
        Link *l = &pg->links[i];
        if (l->kind != kind) continue;
        if ((l->module == NULL) != (module == NULL)) continue;
        if ((l->member == NULL) != (member == NULL)) continue;
        if (module && strcmp(l->module, module) != 0) continue;
        if (member && strcmp(l->member, member) != 0) continue;
        return i;
    }
    if (pg->nlinks == pg->caplinks) {
        pg->caplinks = pg->caplinks ? pg->caplinks * 2 : 16;
        pg->links = xrealloc(pg->links, sizeof(Link) * (size_t)pg->caplinks);
    }
    Link *l = &pg->links[pg->nlinks];
    l->kind = kind;
    l->module = NULL;
    l->member = NULL;
    if (module) { l->module = malloc(strlen(module) + 1); if (!l->module) exit(14); memcpy(l->module, module, strlen(module) + 1); }
    if (member) { l->member = malloc(strlen(member) + 1); if (!l->member) exit(14); memcpy(l->member, member, strlen(member) + 1); }
    return pg->nlinks++;
}

/* ---------------------------------------------------------------- */
/* scopes and names                                                  */
/* ---------------------------------------------------------------- */
void fs_init(FS *fs, FS *parent, Proto *p) {
    memset(fs, 0, sizeof *fs);
    fs->parent = parent;
    fs->p = p;
}

void fs_free(FS *fs) {
    free(fs->loc);
    free(fs->up);
    for (int i = 0; i < fs->nloops; i++) free(fs->loops[i].breaks);
    free(fs->loops);
    free(fs->exits);
}

void begin_scope(Comp *c) { c->fs->scope_depth++; }

void end_scope(Comp *c) {
    FS *fs = c->fs;
    fs->scope_depth--;
    while (fs->nloc > 0 && fs->loc[fs->nloc - 1].depth > fs->scope_depth) fs->nloc--;
    fs->next_slot = fs->nloc > 0 ? fs->loc[fs->nloc - 1].slot + 1 : 0;
}

static int push_local(Comp *c, const Node *where, const char *name, size_t len, Decl *d) {
    FS *fs = c->fs;
    if (fs->next_slot >= 65535) cerr(c, where, "ParseError", E_PARSE, "too many local variables");
    if (fs->nloc == fs->caploc) {
        fs->caploc = fs->caploc ? fs->caploc * 2 : 16;
        fs->loc = xrealloc(fs->loc, sizeof(Local) * (size_t)fs->caploc);
    }
    Local *l = &fs->loc[fs->nloc++];
    l->name = name;
    l->len = len;
    l->decl = d;
    l->slot = fs->next_slot++;
    l->depth = fs->scope_depth;
    l->cell_ready = 0;
    if (fs->next_slot > fs->max_slot) fs->max_slot = fs->next_slot;
    return fs->nloc - 1;
}

int declare_local(Comp *c, const Node *where, Decl *d) {
    FS *fs = c->fs;
    for (int i = fs->nloc - 1; i >= 0 && fs->loc[i].depth == fs->scope_depth; i--)
        if (fs->loc[i].len == d->len && memcmp(fs->loc[i].name, d->name, d->len) == 0)
            cerr(c, where, "NameError", E_NAME, "'%s' is already declared in this scope", d->name);
    return push_local(c, where, d->name, d->len, d);
}

int declare_hidden(Comp *c, const Node *where) { return push_local(c, where, "", 0, NULL); }

static int find_local(const FS *fs, const char *name, size_t len) {
    for (int i = fs->nloc - 1; i >= 0; i--)
        if (fs->loc[i].len == len && len > 0 && memcmp(fs->loc[i].name, name, len) == 0) return i;
    return -1;
}

static int add_up(Comp *c, FS *fs, int from_local, int index, Decl *d) {
    for (int i = 0; i < fs->nup; i++)
        if (fs->up[i].from_local == from_local && fs->up[i].index == index) return i;
    if (fs->nup >= 65535) front_error(c->F, "ParseError", E_PARSE, fs->cur_line, fs->cur_col, 0, "too many captured variables");
    if (fs->nup == fs->capup) {
        fs->capup = fs->capup ? fs->capup * 2 : 8;
        fs->up = xrealloc(fs->up, sizeof(Up) * (size_t)fs->capup);
    }
    fs->up[fs->nup].from_local = from_local;
    fs->up[fs->nup].index = index;
    fs->up[fs->nup].decl = d;
    return fs->nup++;
}

static int resolve_up(Comp *c, const Node *where, FS *fs, const char *name, size_t len) {
    if (!fs->parent) return -1;
    int li = find_local(fs->parent, name, len);
    if (li >= 0) {
        Local *l = &fs->parent->loc[li];
        l->decl->captured = 1;
        if (!c->dry && !l->cell_ready)
            cerr(c, where, "ParseError", E_PARSE,
                 "capturing '%s' here is not supported (parameter used inside a default-argument closure)", l->decl->name);
        return add_up(c, fs, 1, l->slot, l->decl);
    }
    int ui = resolve_up(c, where, fs->parent, name, len);
    if (ui >= 0) return add_up(c, fs, 0, ui, fs->parent->up[ui].decl);
    return -1;
}

int find_mvar(const Comp *c, const char *name, size_t len) {
    for (int i = c->nmv - 1; i >= c->mv_start; i--)
        if (c->mv[i].len == len && memcmp(c->mv[i].name, name, len) == 0) return i;
    return -1;
}

int declare_mvar(Comp *c, const Node *where, const char *name, size_t len, int is_const, const char *modname) {
    int prior = find_mvar(c, name, len);
    if (prior >= c->dup_floor)
        cerr(c, where, "NameError", E_NAME, "'%.*s' is already declared in this scope", (int)len, name);
    if (prior >= 0) { /* REPL: redeclaring a name from an earlier input reuses its slot, so callers see the new value */
        MVar *m = &c->mv[prior];
        m->name = name;
        m->len = len;
        m->is_const = is_const;
        m->modname = modname;
        m->user_mod = -1;
        m->alias_slot = -1;
        return prior;
    }
    if (c->nmv >= 65535) cerr(c, where, "ParseError", E_PARSE, "too many module-level names");
    if (c->nmv == c->capmv) {
        c->capmv = c->capmv ? c->capmv * 2 : 32;
        c->mv = xrealloc(c->mv, sizeof(MVar) * (size_t)c->capmv);
    }
    c->mv[c->nmv].name = name;
    c->mv[c->nmv].len = len;
    c->mv[c->nmv].is_const = is_const;
    c->mv[c->nmv].modname = modname;
    c->mv[c->nmv].user_mod = -1;
    c->mv[c->nmv].alias_slot = -1;
    return c->nmv++;
}

/* an unnamed module variable (slot only), for compiler-generated functions */
int declare_hidden_mvar(Comp *c, const Node *where) {
    if (c->nmv >= 65535) cerr(c, where, "ParseError", E_PARSE, "too many module-level names");
    if (c->nmv == c->capmv) {
        c->capmv = c->capmv ? c->capmv * 2 : 32;
        c->mv = xrealloc(c->mv, sizeof(MVar) * (size_t)c->capmv);
    }
    c->mv[c->nmv].name = "";
    c->mv[c->nmv].len = 0;
    c->mv[c->nmv].is_const = 1;
    c->mv[c->nmv].modname = NULL;
    c->mv[c->nmv].user_mod = -1;
    c->mv[c->nmv].alias_slot = -1;
    return c->nmv++;
}

/* Edit distance, capped: returns > cap when the names are too different. */
static int edit_distance(const char *a, size_t la, const char *b, size_t lb, int cap) {
    if (la > 63 || lb > 63) return cap + 1;
    int prev[64], cur[64];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
            int del = prev[j] + 1, ins = cur[j - 1] + 1;
            int m = sub < del ? sub : del;
            cur[j] = m < ins ? m : ins;
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

static void consider(const char *name, size_t len, const Node *n, const char **best, size_t *best_len, int *best_d) {
    int cap = n->slen <= 2 ? 1 : 2;
    int d = edit_distance(n->str, n->slen, name, len, cap);
    if (d <= cap && d < *best_d) {
        *best = name;
        *best_len = len;
        *best_d = d;
    }
}

static SP_NORETURN void undefined_name(Comp *c, const Node *n) {
    if (lib_find_module(c->vm, n->str, n->slen))
        cerr(c, n, "NameError", E_NAME, "undefined name '%s' (did you forget 'import %s'?)", n->str, n->str);
    const char *best = NULL;
    size_t best_len = 0;
    int best_d = 99;
    for (const FS *f = c->fs; f; f = f->parent)
        for (int i = 0; i < f->nloc; i++) consider(f->loc[i].name, f->loc[i].len, n, &best, &best_len, &best_d);
    for (int i = c->mv_start; i < c->nmv; i++) consider(c->mv[i].name, c->mv[i].len, n, &best, &best_len, &best_d);
    for (int i = 0; lib_prelude_desc(i)->name; i++)
        if (lib_prelude_desc(i)->name[0] != '$') consider(lib_prelude_desc(i)->name, strlen(lib_prelude_desc(i)->name), n, &best, &best_len, &best_d);
    if (best)
        cerr(c, n, "NameError", E_NAME, "undefined name '%s' (did you mean '%.*s'?)", n->str, (int)best_len, best);
    cerr(c, n, "NameError", E_NAME, "undefined name '%s'", n->str);
}

Res resolve_name(Comp *c, const Node *n) {
    FS *fs = c->fs;
    Res r;
    memset(&r, 0, sizeof r);
    r.user_mod = -1;
    int li = find_local(fs, n->str, n->slen);
    if (li >= 0) {
        Local *l = &fs->loc[li];
        r.kind = R_LOCAL;
        r.idx = l->slot;
        r.is_const = l->decl ? l->decl->is_const : 0;
        r.cell = l->decl && l->decl->captured && l->cell_ready;
        return r;
    }
    int ui = resolve_up(c, n, fs, n->str, n->slen);
    if (ui >= 0) {
        r.kind = R_UPVAL;
        r.idx = ui;
        r.is_const = fs->up[ui].decl->is_const;
        return r;
    }
    int mi = find_mvar(c, n->str, n->slen);
    if (mi >= 0) {
        r.kind = R_MODULE;
        r.idx = c->mv[mi].alias_slot >= 0 ? c->mv[mi].alias_slot : mi;
        r.is_const = c->mv[mi].is_const;
        r.modname = c->mv[mi].modname;
        r.user_mod = c->mv[mi].user_mod;
        return r;
    }
    int pi = lib_prelude_index(n->str, n->slen);
    if (pi >= 0) {
        r.kind = R_LINK;
        r.idx = add_link(c, LINK_PRELUDE, NULL, lib_prelude_desc(pi)->name);
        r.is_const = 1;
        return r;
    }
    undefined_name(c, n);
}

void emit_load(Comp *c, Res r) {
    switch (r.kind) {
    case R_LOCAL: op_u16(c, r.cell ? OP_LOAD_CELL : OP_LOAD_LOCAL, (unsigned)r.idx, 1); break;
    case R_UPVAL: op_u16(c, OP_LOAD_UPVAL, (unsigned)r.idx, 1); break;
    case R_MODULE: op_u16(c, OP_LOAD_MODULE, (unsigned)r.idx, 1); break;
    case R_LINK: op_u16(c, OP_LOAD_LINK, (unsigned)r.idx, 1); break;
    default: break;
    }
}

void emit_store(Comp *c, Res r) {
    switch (r.kind) {
    case R_LOCAL: op_u16(c, r.cell ? OP_STORE_CELL : OP_STORE_LOCAL, (unsigned)r.idx, -1); break;
    case R_UPVAL: op_u16(c, OP_STORE_UPVAL, (unsigned)r.idx, -1); break;
    case R_MODULE: op_u16(c, OP_STORE_MODULE, (unsigned)r.idx, -1); break;
    case R_LINK:
    default: break;
    }
}

/* ---------------------------------------------------------------- */
