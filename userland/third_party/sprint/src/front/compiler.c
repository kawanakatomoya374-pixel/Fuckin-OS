/* Compiler: function bodies, module/program assembly, the two-pass driver and the REPL scope.
 *
 * Closure capture analysis: the whole program is compiled twice. The first
 * ("dry") pass only records which declarations are captured by an inner
 * function (Decl.captured); the second pass emits the real code and uses that
 * knowledge to decide which locals live in heap cells.
 */
#include "compiler_int.h"

/* functions                                                         */
/* ---------------------------------------------------------------- */
int compile_func(Comp *c, Node *fn, Proto *shell) {
    FS nfs;
    Proto *p = shell ? shell : new_proto(c, fn->str);
    if (shell && !p->name && fn->str) {
        p->name = malloc(fn->slen + 1);
        if (!p->name) exit(14);
        memcpy(p->name, fn->str, fn->slen + 1);
    }
    int pidx = proto_index(c, p);
    fs_init(&nfs, c->fs, p);
    nfs.cur_line = fn->line;
    nfs.cur_col = fn->col;
    FS *saved = c->fs;
    c->fs = &nfs;
    nfs.cls = c->pending_cls;
    nfs.is_init = c->pending_init;
    c->pending_cls = NULL;
    c->pending_init = 0;
    begin_scope(c); /* parameter scope */

    int nreq = 0;
    int nparams = fn->n;
    if (nparams > 255) cerr(c, fn, "ParseError", E_PARSE, "too many parameters (limit 255)");
    int *pslot = malloc(sizeof(int) * (size_t)(nparams + 1));
    if (!pslot) exit(14);
    front_push(c->F, front_free_ptr, pslot);
    for (int i = 0; i < nparams; i++) {
        int li = declare_local(c, fn->list[i], fn->list[i]->decl);
        pslot[i] = nfs.loc[li].slot;
        if (!fn->list[i]->a) nreq++;
    }
    int rest_slot = -1;
    if (fn->c) {
        int li = declare_local(c, fn->c, fn->c->decl);
        rest_slot = nfs.loc[li].slot;
    }
    if (nfs.is_init) {
        int si = declare_hidden(c, fn);
        nfs.super_slot = nfs.loc[si].slot;
    }
    p->nparams = nparams;
    p->nrequired = nreq;
    p->has_rest = fn->c != NULL;
    p->ndefaults = nparams - nreq;
    free(p->default_pcs);
    p->default_pcs = malloc(sizeof(uint32_t) * (size_t)(p->ndefaults + 1));
    if (!p->default_pcs) exit(14);

    /* default-argument prologue */
    for (int i = nreq; i < nparams; i++) {
        p->default_pcs[i - nreq] = p->code_len;
        expr(c, fn->list[i]->a);
        setpos(c, fn->list[i]);
        op_u16(c, OP_STORE_LOCAL, (unsigned)pslot[i], -1);
    }
    p->default_pcs[p->ndefaults] = p->code_len;

    /* captured parameters live in cells from here on */
    for (int i = 0; i < nparams; i++) {
        if (fn->list[i]->decl->captured) {
            op_u16(c, OP_MAKE_CELL, (unsigned)pslot[i], 0);
            for (int k = 0; k < nfs.nloc; k++)
                if (nfs.loc[k].decl == fn->list[i]->decl) nfs.loc[k].cell_ready = 1;
        }
    }
    if (fn->c && fn->c->decl->captured) {
        op_u16(c, OP_MAKE_CELL, (unsigned)rest_slot, 0);
        for (int k = 0; k < nfs.nloc; k++)
            if (nfs.loc[k].decl == fn->c->decl) nfs.loc[k].cell_ready = 1;
    }
    front_pop(c->F);
    free(pslot);

    if (fn->flag) { /* expression-bodied lambda */
        expr(c, fn->a);
        setpos(c, fn->a);
        op0(c, OP_RETURN, -1);
    } else {
        begin_scope(c);
        block_stmts(c, fn->a);
        end_scope(c);
        setpos(c, fn);
        op0(c, OP_RETURN_NIL, 0);
    }
    end_scope(c);

    p->nlocals = nfs.max_slot;
    p->max_stack = nfs.max_depth + 4;
    p->nupvals = nfs.nup;
    free(p->upvals);
    p->upvals = NULL;
    if (nfs.nup) {
        p->upvals = malloc(sizeof(UpDesc) * (size_t)nfs.nup);
        if (!p->upvals) exit(14);
        for (int i = 0; i < nfs.nup; i++) {
            p->upvals[i].from_local = (uint8_t)nfs.up[i].from_local;
            p->upvals[i].index = (uint16_t)nfs.up[i].index;
        }
    }
    fs_free(&nfs);
    c->fs = saved;
    return pidx;
}

/* ---------------------------------------------------------------- */
/* program                                                           */
/* ---------------------------------------------------------------- */
static void proto_free(Proto *p) {
    free(p->name);
    free(p->upvals);
    free(p->consts);
    free(p->default_pcs);
    jit_free_proto(p);
    free(p->ocode);
    free(p->code);
    free(p->lines);
    free(p);
}

void program_free(Program *pg) {
    if (!pg) return;
    for (int i = 0; i < pg->nprotos; i++) proto_free(pg->protos[i]);
    free(pg->protos);
    for (int i = 0; i < pg->nlinks; i++) {
        free(pg->links[i].module);
        free(pg->links[i].member);
    }
    free(pg->links);
    for (int i = 0; i < pg->nclasses; i++) {
        ClassInfo *ci = &pg->classes[i];
        free(ci->name);
        for (int k = 0; k < ci->nmethods; k++) free(ci->method_names[k]);
        for (int k = 0; k < ci->nfields; k++) free(ci->field_names[k]);
        free(ci->method_names);
        free(ci->field_names);
    }
    free(pg->classes);
    free_foreign_classes(pg);
    for (int i = 0; i < pg->nmods; i++) {
        free(pg->mods[i].name);
        for (int k = 0; k < pg->mods[i].nexports; k++) free(pg->mods[i].exports[k].name);
        free(pg->mods[i].exports);
    }
    free(pg->mods);
    for (int i = 0; i < pg->nslots; i++) free(pg->slot_names[i]);
    free(pg->slot_names);
    free(pg);
}

static void compile_module_body(Comp *c, Node *root) {
    Proto *mp = new_proto(c, "<module>");
    mp->is_module = 1;
    c->entry_proto = proto_index(c, mp);
    FS fs;
    fs_init(&fs, NULL, mp);
    fs.is_module = 1;
    fs.cur_line = 1;
    fs.cur_col = 1;
    c->fs = &fs;

    /* hoist top-level functions: names are visible module-wide and their
     * closures are created before the body runs (SPEC 5.2, 14.5) */
    for (int i = 0; i < root->n; i++) {
        Node *s = root->list[i];
        if (s->kind != S_FUNC) continue;
        int slot = declare_mvar(c, s, s->decl->name, s->decl->len, 1, NULL);
        if (s->exported) export_add(c, s, s->decl->name, s->decl->len, slot);
        Proto *shell = new_proto(c, s->str);
        s->ival = proto_index(c, shell);
        s->op = slot;
        setpos(c, s);
        emit_op(c, OP_CLOSURE, 1);
        e32(c, (uint32_t)s->ival);
        op_u16(c, OP_STORE_MODULE, (unsigned)slot, -1);
    }
    stmt_list(c, root->list, root->n);
    setpos(c, root);
    op0(c, OP_RETURN_NIL, 0);

    mp->nlocals = fs.max_slot;
    mp->max_stack = fs.max_depth + 4;
    {
        char **old = c->prog->slot_names;
        int nold = c->prog->nslots;
        c->prog->slot_names = calloc((size_t)(c->nmv ? c->nmv : 1), sizeof(char *));
        if (!c->prog->slot_names) exit(14);
        for (int i = 0; i < nold && i < c->nmv; i++) c->prog->slot_names[i] = old[i];
        free(old);
        c->prog->nslots = c->nmv;
    }
    for (int i = c->name_start; i < c->nmv; i++) {
        c->prog->slot_names[i] = malloc(c->mv[i].len + 1);
        if (!c->prog->slot_names[i]) exit(14);
        memcpy(c->prog->slot_names[i], c->mv[i].name, c->mv[i].len);
        c->prog->slot_names[i][c->mv[i].len] = 0;
    }
    fs_free(&fs);
    c->fs = NULL;
}

Program *program_new(void) {
    Program *pg = calloc(1, sizeof(Program));
    if (!pg) exit(14);
    return pg;
}

static void free_comp_exports(Comp *c) {
    for (int i = 0; i < c->nexp; i++) free(c->exports[i].name);
    free(c->exports);
    c->exports = NULL;
    c->nexp = c->capexp = 0;
}

static void comp_abort(void *p) {
    Comp *c = p;
    for (FS *f = c->fs; f; f = f->parent) fs_free(f);
    free(c->mv);
    for (int i = c->ncls_keep; i < c->ncls; i++) free(c->classes[i]);
    free(c->classes);
    free_comp_exports(c);
    if (c->dry && c->prog != c->real) program_free(c->prog);
}

/* REPL: the module-variable names and class contexts of earlier input chunks, which
 * later chunks keep compiling against (the AST arenas they point into stay alive). */
struct ReplScope {
    MVar *mv;
    int nmv, capmv;
    ClassCtx **classes;
    int ncls, capcls;
    int want_echo; /* in: turn a lone expression statement into `_ = expr` */
    int echoed;    /* out: that happened for the chunk just compiled */
};

void replscope_echo(ReplScope *r, int want) {
    r->want_echo = want;
    r->echoed = 0;
}
int replscope_echoed(const ReplScope *r) { return r->echoed; }
int replscope_wants_echo(const ReplScope *r) { return r->want_echo; }
void replscope_set_echoed(ReplScope *r) { r->echoed = 1; }

ReplScope *replscope_new(void) {
    ReplScope *r = calloc(1, sizeof *r);
    if (!r) exit(14);
    return r;
}

void replscope_free(ReplScope *r) {
    if (!r) return;
    for (int i = 0; i < r->ncls; i++) free(r->classes[i]);
    free(r->classes);
    free(r->mv);
    free(r);
}

/* Extend the scope to `nslots` entries: slots added by modules imported since the last chunk are not visible by name. */
static void replscope_sync(ReplScope *r, int nslots) {
    if (r->nmv >= nslots) return;
    if (r->capmv < nslots) {
        r->capmv = nslots + 16;
        r->mv = xrealloc(r->mv, sizeof(MVar) * (size_t)r->capmv);
    }
    for (int i = r->nmv; i < nslots; i++) {
        memset(&r->mv[i], 0, sizeof r->mv[i]);
        r->mv[i].name = "";
        r->mv[i].user_mod = -1;
        r->mv[i].alias_slot = -1;
    }
    r->nmv = nslots;
}

int replscope_count(const ReplScope *r) { return r->nmv; }
const char *replscope_name(const ReplScope *r, int i) { return r->mv[i].len ? r->mv[i].name : NULL; }
size_t replscope_name_len(const ReplScope *r, int i) { return r->mv[i].len; }
int replscope_is_import(const ReplScope *r, int i) { return r->mv[i].modname || r->mv[i].user_mod >= 0 || r->mv[i].alias_slot >= 0; }

/* Slot of the newest module variable called `name`, or -1. */
int replscope_slot(const ReplScope *r, const char *name) {
    size_t len = strlen(name);
    for (int i = r->nmv - 1; i >= 0; i--)
        if (r->mv[i].len == len && memcmp(r->mv[i].name, name, len) == 0) return i;
    return -1;
}

void compile_module_scoped(Front *F, VM *vm, Program *prog, Node *root, const char *modname, const char *file,
                           ReplScope *rs) {
    Comp c;
    memset(&c, 0, sizeof c);
    front_push(F, comp_abort, &c);
    c.F = F;
    c.vm = vm;
    c.real = prog;
    c.cur_mod = prog->nmods;
    if (rs) {
        c.ncls_keep = rs->ncls;
        replscope_sync(rs, prog->nslots);
    }
    for (int pass = 0; pass < 2; pass++) {
        c.dry = pass == 0;
        c.prog = c.dry ? program_new() : prog;
        /* module variables of earlier modules keep their global slots */
        c.nmv = prog->nslots;
        c.name_start = prog->nslots;
        c.dup_floor = prog->nslots;
        c.mv_start = rs ? 0 : prog->nslots;
        if (c.nmv > c.capmv) {
            c.capmv = c.nmv + 16;
            c.mv = xrealloc(c.mv, sizeof(MVar) * (size_t)c.capmv);
        }
        if (rs) {
            if (rs->nmv != c.nmv) cerr(&c, root, "ParseError", E_PARSE, "internal: REPL scope out of sync");
            if (c.nmv) memcpy(c.mv, rs->mv, sizeof(MVar) * (size_t)c.nmv);
        } else if (c.nmv) {
            memset(c.mv, 0, sizeof(MVar) * (size_t)c.nmv);
        }
        for (int i = c.ncls_keep; i < c.ncls; i++) free(c.classes[i]);
        c.ncls = c.ncls_keep;
        if (rs && c.ncls_keep) {
            if (c.capcls < c.ncls_keep) {
                c.capcls = c.ncls_keep + 8;
                c.classes = xrealloc(c.classes, sizeof(ClassCtx *) * (size_t)c.capcls);
            }
            memcpy(c.classes, rs->classes, sizeof(ClassCtx *) * (size_t)c.ncls_keep);
            c.ncls = c.ncls_keep;
        }
        free_comp_exports(&c);
        compile_module_body(&c, root);
        if (c.dry) {
            program_free(c.prog);
        }
    }
    for (int i = c.ncls_keep; !rs && i < c.ncls; i++)
        if (c.classes[i]->decl->exported) (void)persist_chain(prog, c.classes[i]);
    if (prog->nmods == prog->capmods) {
        prog->capmods = prog->capmods ? prog->capmods * 2 : 8;
        prog->mods = xrealloc(prog->mods, sizeof(ModInfo) * (size_t)prog->capmods);
    }
    ModInfo *mi = &prog->mods[prog->nmods++];
    memset(mi, 0, sizeof *mi);
    mi->name = malloc(strlen(modname) + 1);
    if (!mi->name) exit(14);
    memcpy(mi->name, modname, strlen(modname) + 1);
    (void)snprintf(mi->file, sizeof mi->file, "%s", file ? file : "<input>");
    mi->entry_proto = c.entry_proto;
    mi->exports = c.exports;
    mi->nexports = c.nexp;
    front_pop(F);
    if (rs) {
        /* hand the scope over to the session */
        free(rs->mv);
        rs->mv = c.mv;
        rs->nmv = c.nmv;
        rs->capmv = c.capmv;
        c.mv = NULL;
        free(rs->classes);
        rs->classes = c.classes;
        rs->ncls = c.ncls;
        rs->capcls = c.capcls;
        c.classes = NULL;
        c.ncls = 0;
    }
    for (int i = 0; i < c.ncls; i++) free(c.classes[i]);
    free(c.classes);
    free(c.mv);
}

void compile_module(Front *F, VM *vm, Program *prog, Node *root, const char *modname, const char *file) {
    compile_module_scoped(F, vm, prog, root, modname, file, NULL);
}

/* ---------------------------------------------------------------- */
