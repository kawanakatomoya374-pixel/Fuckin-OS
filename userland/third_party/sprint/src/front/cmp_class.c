/* Compiler: classes (SPEC 10) - layout, constructors, super, and the class descriptions other modules extend. */
#include "compiler_int.h"

/* ---- classes (SPEC 10) ---- */
static ClassCtx *find_class(const Comp *c, const char *name, size_t len) {
    for (int i = 0; i < c->ncls; i++) {
        const Node *d = c->classes[i]->decl;
        if (d->slen == len && memcmp(d->str, name, len) == 0) return c->classes[i];
    }
    return NULL;
}

static Node *class_member(const ClassCtx *k, int kind, const char *name, size_t len) {
    for (int i = 0; i < k->decl->n; i++) {
        Node *m = k->decl->list[i];
        if (m->kind != kind) continue;
        size_t ml = kind == S_LET ? m->decl->len : m->slen;
        const char *mn = kind == S_LET ? m->decl->name : m->str;
        if (ml == len && memcmp(mn, name, len) == 0) return m;
    }
    return NULL;
}

/* class (starting at k and walking up) that defines the method, or NULL */
static ClassCtx *class_find_method(ClassCtx *k, const char *name, size_t len) {
    for (; k; k = k->parent)
        if (class_member(k, S_FUNC, name, len)) return k;
    return NULL;
}

static ClassCtx *class_find_field(ClassCtx *k, const char *name, size_t len) {
    for (; k; k = k->parent)
        if (class_member(k, S_LET, name, len)) return k;
    return NULL;
}

static int add_class_info(Comp *c, ClassInfo *ci) {
    Program *pg = c->prog;
    if (pg->nclasses == pg->capclasses) {
        pg->capclasses = pg->capclasses ? pg->capclasses * 2 : 8;
        pg->classes = xrealloc(pg->classes, sizeof(ClassInfo) * (size_t)pg->capclasses);
    }
    pg->classes[pg->nclasses] = *ci;
    return pg->nclasses++;
}

static char *dup_n(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) exit(14);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void emit_raise_err(Comp *c, int code, const char *msg) {
    int k = str_const(c, msg, strlen(msg));
    emit_op(c, OP_RAISE_ERR, 0);
    e8(c, (unsigned)code);
    e16(c, (unsigned)k);
}

/* field initialisers, root ancestor first; evaluated in module scope. An ancestor that has an
 * initialiser function (an exported class: always when it comes from another module, and for a
 * class of this module when `use_fin` is set) is initialised by calling that function, which
 * already covers that ancestor's own ancestors. */
static void emit_field_inits(Comp *c, ClassCtx *cx, int self, const Node *decl, int use_fin) {
    ClassCtx *chain[64];
    int depth = 0;
    for (ClassCtx *k = cx; k; k = k->parent) {
        if (depth >= 64) cerr(c, decl, "ParseError", E_PARSE, "class hierarchy too deep");
        chain[depth++] = k;
        if (k != cx && (k->foreign || (use_fin && k->has_fin))) break;
    }
    for (int ci = depth - 1; ci >= 0; ci--) {
        ClassCtx *k = chain[ci];
        if (k->foreign || (use_fin && k != cx && k->has_fin)) {
            if (k->has_fin) {
                setpos(c, decl);
                op_u16(c, OP_LOAD_MODULE, (unsigned)k->fin_slot, 1);
                op_u16(c, OP_LOAD_LOCAL, (unsigned)self, 1);
                op_u8(c, OP_CALL, 1, -1);
                op0(c, OP_POP, -1);
            }
            continue;
        }
        int idx = 0;
        for (int i = 0; i < k->decl->n; i++) {
            Node *m = k->decl->list[i];
            if (m->kind != S_LET) continue;
            if (m->a) {
                op_u16(c, OP_LOAD_LOCAL, (unsigned)self, 1);
                expr(c, m->a);
                setpos(c, m);
                op_u16(c, OP_SET_SLOT, (unsigned)(k->base_fields + idx), -2);
            }
            idx++;
        }
    }
}

static int chain_has_inits(const ClassCtx *cx) {
    for (const ClassCtx *k = cx; k; k = k->parent) {
        if (k != cx && k->has_fin) return 1;
        if (k->foreign) return 0;
        for (int i = 0; i < k->decl->n; i++)
            if (k->decl->list[i]->kind == S_LET && k->decl->list[i]->a) return 1;
    }
    return 0;
}

/* exported class: a function(self) running the initialisers of the whole chain in this module's scope,
 * so that classes in other modules can extend it */
static int compile_finit(Comp *c, ClassCtx *cx) {
    Node *decl = cx->decl;
    FS nfs;
    Proto *p = new_proto(c, decl->str);
    fs_init(&nfs, c->fs, p);
    nfs.cur_line = decl->line;
    nfs.cur_col = decl->col;
    FS *saved = c->fs;
    c->fs = &nfs;
    begin_scope(c);
    int self_i = declare_hidden(c, decl);
    int self = nfs.loc[self_i].slot;
    p->nparams = 1;
    p->nrequired = 1;
    p->has_rest = 0;
    p->ndefaults = 0;
    setpos(c, decl);
    emit_field_inits(c, cx, self, decl, 1);
    op0(c, OP_RETURN_NIL, 0);
    end_scope(c);
    p->nlocals = nfs.max_slot;
    p->max_stack = nfs.max_depth + 4;
    p->nupvals = nfs.nup;
    if (nfs.nup) cerr(c, decl, "ParseError", E_PARSE, "internal: initialiser captured a variable");
    fs_free(&nfs);
    c->fs = saved;
    return proto_index(c, p);
}

static int compile_ctor(Comp *c, ClassCtx *cx) {
    Node *decl = cx->decl;
    FS nfs;
    Proto *p = new_proto(c, decl->str);
    fs_init(&nfs, c->fs, p);
    nfs.cur_line = decl->line;
    nfs.cur_col = decl->col;
    FS *saved = c->fs;
    c->fs = &nfs;
    begin_scope(c);
    int rest_i = declare_hidden(c, decl);
    int rest = nfs.loc[rest_i].slot;
    int self_i = declare_hidden(c, decl);
    int self = nfs.loc[self_i].slot;
    p->nparams = 0;
    p->nrequired = 0;
    p->has_rest = 1;
    p->ndefaults = 0;
    setpos(c, decl);
    op_u16(c, OP_LOAD_MODULE, (unsigned)cx->slot, 1);
    op0(c, OP_NEW_INSTANCE, 0);
    op_u16(c, OP_STORE_LOCAL, (unsigned)self, -1);

    emit_field_inits(c, cx, self, decl, 0);
    ClassCtx *ik = class_find_method(cx, "init", 4);
    setpos(c, decl);
    if (ik) {
        op_u16(c, OP_LOAD_MODULE, (unsigned)ik->slot, 1);
        op_u16(c, OP_GET_MEMBER, (unsigned)str_const(c, "init", 4), 0);
        op_u16(c, OP_LOAD_LOCAL, (unsigned)self, 1);
        op_u16(c, OP_LOAD_LOCAL, (unsigned)rest, 1);
        op_u8(c, OP_CALL_SPREAD, 1, -2);
        op0(c, OP_POP, -1);
    } else {
        op_u16(c, OP_CTOR_CHECK, (unsigned)rest, 0);
    }
    op_u16(c, OP_LOAD_LOCAL, (unsigned)self, 1);
    op0(c, OP_RETURN, -1);
    end_scope(c);
    p->nlocals = nfs.max_slot;
    p->max_stack = nfs.max_depth + 4;
    p->nupvals = nfs.nup;
    if (nfs.nup) cerr(c, decl, "ParseError", E_PARSE, "internal: constructor captured a variable");
    fs_free(&nfs);
    c->fs = saved;
    return proto_index(c, p);
}

static void classinfo_abort(void *p) {
    ClassInfo *ci = p;
    free(ci->name);
    for (int i = 0; ci->method_names && i < ci->nmethods; i++) free(ci->method_names[i]);
    for (int i = 0; ci->field_names && i < ci->nfields; i++) free(ci->field_names[i]);
    free(ci->method_names);
    free(ci->field_names);
}


/* ---- classes exported to other modules ----
 * A class extended from another module is described by a name-only copy kept in the Program
 * (the AST of the module that declared it is gone by then). */
typedef struct ForeignCls {
    ClassCtx ctx;
    Node decl;
    Node **list;
    Node *nodes;
    Decl *decls;
    char *names;
    char *cname;
} ForeignCls;

static ForeignCls *persist_class(Program *pg, const ClassCtx *k, const ClassCtx *parent_copy) {
    const Node *d = k->decl;
    ForeignCls *f = calloc(1, sizeof *f);
    if (!f) exit(14);
    size_t nb = d->slen + 1;
    for (int i = 0; i < d->n; i++) {
        const Node *m = d->list[i];
        nb += (m->kind == S_LET ? m->decl->len : m->slen) + 1;
    }
    f->list = calloc((size_t)(d->n ? d->n : 1), sizeof(Node *));
    f->nodes = calloc((size_t)(d->n ? d->n : 1), sizeof(Node));
    f->decls = calloc((size_t)(d->n ? d->n : 1), sizeof(Decl));
    f->names = malloc(nb);
    if (!f->list || !f->nodes || !f->decls || !f->names) exit(14);
    char *w = f->names;
    f->cname = w;
    memcpy(w, d->str, d->slen);
    w[d->slen] = 0;
    w += d->slen + 1;
    for (int i = 0; i < d->n; i++) {
        const Node *m = d->list[i];
        Node *o = &f->nodes[i];
        o->kind = m->kind;
        o->line = m->line;
        o->col = m->col;
        size_t len = m->kind == S_LET ? m->decl->len : m->slen;
        const char *src = m->kind == S_LET ? m->decl->name : m->str;
        memcpy(w, src, len);
        w[len] = 0;
        if (m->kind == S_LET) {
            f->decls[i].name = w;
            f->decls[i].len = len;
            o->decl = &f->decls[i];
        } else {
            o->str = w;
            o->slen = len;
        }
        w += len + 1;
        f->list[i] = o;
    }
    f->decl.kind = S_CLASS;
    f->decl.str = f->cname;
    f->decl.slen = d->slen;
    f->decl.list = f->list;
    f->decl.n = d->n;
    f->ctx = *k;
    f->ctx.decl = &f->decl;
    f->ctx.parent = (ClassCtx *)(uintptr_t)parent_copy;
    f->ctx.foreign = 1;
    if (pg->nfcls == pg->capfcls) {
        pg->capfcls = pg->capfcls ? pg->capfcls * 2 : 8;
        pg->fcls = xrealloc(pg->fcls, sizeof(void *) * (size_t)pg->capfcls);
    }
    pg->fcls[pg->nfcls++] = f;
    return f;
}

const ClassCtx *persist_chain(Program *pg, const ClassCtx *k) {
    if (!k) return NULL;
    if (k->foreign) return k; /* already owned by the program */
    for (int i = 0; i < pg->nfcls; i++) {
        ForeignCls *f = pg->fcls[i];
        if (f->ctx.slot == k->slot) return &f->ctx;
    }
    const ClassCtx *par = persist_chain(pg, k->parent);
    return &persist_class(pg, k, par)->ctx;
}

void free_foreign_classes(Program *pg) {
    for (int i = 0; i < pg->nfcls; i++) {
        ForeignCls *f = pg->fcls[i];
        free(f->list);
        free(f->nodes);
        free(f->decls);
        free(f->names);
        free(f);
    }
    free(pg->fcls);
}

/* the exported class of another module that `Name` or `mod.Name` (an extends clause) refers to */
static ClassCtx *find_imported_class(Comp *c, const Node *e) {
    int slot = -1;
    if (e->kind == N_NAME) {
        Res r = resolve_name(c, e);
        if (r.kind != R_MODULE) return NULL;
        slot = r.idx;
    } else if (e->kind == N_MEMBER && e->a && e->a->kind == N_NAME) {
        slot = member_user_slot(c, e->a, e);
    }
    if (slot < 0) return NULL;
    const Program *pg = c->real;
    for (int i = 0; i < pg->nfcls; i++) {
        const ForeignCls *f = pg->fcls[i];
        if (f->ctx.slot == slot) return (ClassCtx *)(uintptr_t)&f->ctx;
    }
    return NULL;
}

void compile_class(Comp *c, Node *n) {
    if (!at_module_top(c)) cerr(c, n, "ParseError", E_PARSE, "'class' is only allowed at module top level");
    ClassCtx *cx = calloc(1, sizeof *cx);
    if (!cx) exit(14);
    front_push(c->F, front_free_ptr, cx); /* until it is registered in c->classes */
    cx->decl = n;
    if (c->ncls == c->capcls) {
        c->capcls = c->capcls ? c->capcls * 2 : 8;
        c->classes = xrealloc(c->classes, sizeof(ClassCtx *) * (size_t)c->capcls);
    }
    if (n->a) {
        cx->parent = n->a->kind == N_NAME ? find_class(c, n->a->str, n->a->slen) : NULL;
        if (!cx->parent) cx->parent = find_imported_class(c, n->a);
        if (!cx->parent)
            cerr(c, n->a, "NameError", E_NAME, "'%s' is not a class declared earlier at module top level or exported by an imported module", n->a->str);
    }
    cx->base_fields = 0;
    if (cx->parent) {
        const ClassCtx *pk = cx->parent;
        cx->base_fields = pk->base_fields + pk->nfields;
    }

    /* static checks (SPEC 10.1) */
    for (int i = 0; i < n->n; i++) {
        Node *m = n->list[i];
        const char *mn = m->kind == S_LET ? m->decl->name : m->str;
        size_t ml = m->kind == S_LET ? m->decl->len : m->slen;
        for (int j = 0; j < i; j++) {
            Node *o = n->list[j];
            const char *on = o->kind == S_LET ? o->decl->name : o->str;
            size_t ol = o->kind == S_LET ? o->decl->len : o->slen;
            if (ol == ml && memcmp(on, mn, ml) == 0)
                cerr(c, m, "ParseError", E_PARSE, "duplicate member '%s' in class '%s'", mn, n->str);
        }
        if (m->kind == S_LET) {
            cx->nfields++;
            if (class_find_field(cx->parent, mn, ml) || class_find_method(cx->parent, mn, ml))
                cerr(c, m, "ParseError", E_PARSE, "field '%s' is already declared in a parent class", mn);
        } else {
            if (class_find_field(cx->parent, mn, ml))
                cerr(c, m, "ParseError", E_PARSE, "method '%s' conflicts with a field of a parent class", mn);
            int in_parent = class_find_method(cx->parent, mn, ml) != NULL;
            if (in_parent && !m->op)
                cerr(c, m, "OverrideError", E_OVERRIDE, "method '%s' overrides a parent method and needs 'override'", mn);
            if (!in_parent && m->op)
                cerr(c, m, "OverrideError", E_OVERRIDE, "method '%s' has 'override' but the parent has no such method", mn);
            Node *fn = m->a;
            if (fn->n < 1 || fn->list[0]->decl->len != 4 || memcmp(fn->list[0]->decl->name, "self", 4) != 0)
                cerr(c, m, "ParseError", E_PARSE, "the first parameter of method '%s' must be named 'self'", mn);
        }
    }
    for (int i = 0; i < n->n; i++)
        if (n->list[i]->kind == S_LET && class_member(cx, S_FUNC, n->list[i]->decl->name, n->list[i]->decl->len))
            cerr(c, n->list[i], "ParseError", E_PARSE, "field '%s' conflicts with a method", n->list[i]->decl->name);

    cx->slot = declare_mvar(c, n, n->decl->name, n->decl->len, 1, NULL);
    if (n->exported) export_add(c, n, n->decl->name, n->decl->len, cx->slot);
    front_pop(c->F);
    c->classes[c->ncls++] = cx;

    /* class value construction */
    setpos(c, n);
    if (cx->parent) op_u16(c, OP_LOAD_MODULE, (unsigned)cx->parent->slot, 1);
    else op0(c, OP_NIL, 1);
    ClassInfo ci;
    memset(&ci, 0, sizeof ci);
    front_push(c->F, classinfo_abort, &ci);
    ci.name = dup_n(n->str, n->slen);
    int nm = 0, nf = 0;
    for (int i = 0; i < n->n; i++) {
        if (n->list[i]->kind == S_FUNC) nm++;
        else nf++;
    }
    ci.nmethods = nm;
    ci.nfields = nf;
    ci.method_names = calloc((size_t)(nm ? nm : 1), sizeof(char *));
    ci.field_names = calloc((size_t)(nf ? nf : 1), sizeof(char *));
    if (!ci.method_names || !ci.field_names) exit(14);
    int mi = 0, fi = 0;
    for (int i = 0; i < n->n; i++) {
        Node *m = n->list[i];
        if (m->kind == S_FUNC) {
            ci.method_names[mi++] = dup_n(m->str, m->slen);
            c->pending_cls = cx;
            c->pending_init = m->slen == 4 && memcmp(m->str, "init", 4) == 0;
            int idx = compile_func(c, m->a, NULL);
            c->pending_cls = NULL;
            c->pending_init = 0;
            setpos(c, m);
            emit_op(c, OP_CLOSURE, 1);
            e32(c, (uint32_t)idx);
        } else {
            ci.field_names[fi++] = dup_n(m->decl->name, m->decl->len);
        }
    }
    ci.ctor_proto = compile_ctor(c, cx);
    front_pop(c->F);
    int info = add_class_info(c, &ci);
    setpos(c, n);
    op_u16(c, OP_CLASS, (unsigned)info, -nm);
    op_u16(c, OP_STORE_MODULE, (unsigned)cx->slot, -1);
    if (n->exported && chain_has_inits(cx)) {
        int fp = compile_finit(c, cx);
        cx->fin_slot = declare_hidden_mvar(c, n);
        cx->has_fin = 1;
        setpos(c, n);
        emit_op(c, OP_CLOSURE, 1);
        e32(c, (uint32_t)fp);
        op_u16(c, OP_STORE_MODULE, (unsigned)cx->fin_slot, -1);
    }
}

void compile_super_call(Comp *c, Node *n) {
    FS *fs = c->fs;
    if (!fs->cls || !fs->is_init) cerr(c, n, "ParseError", E_PARSE, "'super(...)' is only valid directly inside 'init'");
    if (!fs->cls->parent) cerr(c, n, "ParseError", E_PARSE, "'super(...)' in a class without 'extends'");
    setpos(c, n);
    op_u16(c, OP_LOAD_LOCAL, (unsigned)fs->super_slot, 1);
    uint32_t jf = jump_fwd(c, OP_JUMP_IF_FALSE, -1);
    emit_raise_err(c, E_RUNTIME, "super(...) called more than once in init");
    patch_here(c, jf);
    op0(c, OP_TRUE, 1);
    op_u16(c, OP_STORE_LOCAL, (unsigned)fs->super_slot, -1);
    ClassCtx *k = class_find_method(fs->cls->parent, "init", 4);
    if (k) {
        Node selfn;
        memset(&selfn, 0, sizeof selfn);
        selfn.kind = N_NAME;
        selfn.line = n->line;
        selfn.col = n->col;
        selfn.str = (char *)(uintptr_t)"self";
        selfn.slen = 4;
        op_u16(c, OP_LOAD_MODULE, (unsigned)k->slot, 1);
        op_u16(c, OP_GET_MEMBER, (unsigned)str_const(c, "init", 4), 0);
        expr(c, &selfn);
        for (int i = 0; i < n->n; i++) expr(c, n->list[i]);
        setpos(c, n);
        op_u8(c, OP_CALL, (unsigned)(n->n + 1), -(n->n + 1));
    } else {
        if (n->n > 0) emit_raise_err(c, E_TYPE, "the parent classes have no init that takes arguments");
        op0(c, OP_NIL, 1);
    }
}

void compile_super_method(Comp *c, Node *n) {
    FS *fs = c->fs;
    if (!fs->cls) cerr(c, n, "ParseError", E_PARSE, "'super.%s' is only valid directly inside a method", n->str);
    if (!fs->cls->parent) cerr(c, n, "ParseError", E_PARSE, "'super.%s' in a class without 'extends'", n->str);
    ClassCtx *k = class_find_method(fs->cls->parent, n->str, n->slen);
    if (!k) cerr(c, n, "NameError", E_NAME, "no parent class defines method '%s'", n->str);
    Node selfn;
    memset(&selfn, 0, sizeof selfn);
    selfn.kind = N_NAME;
    selfn.line = n->line;
    selfn.col = n->col;
    selfn.str = (char *)(uintptr_t)"self";
    selfn.slen = 4;
    setpos(c, n);
    op_u16(c, OP_LOAD_MODULE, (unsigned)k->slot, 1);
    op_u16(c, OP_GET_MEMBER, (unsigned)str_const(c, n->str, n->slen), 0);
    expr(c, &selfn);
    for (int i = 0; i < n->n; i++) expr(c, n->list[i]);
    setpos(c, n);
    op_u8(c, OP_CALL, (unsigned)(n->n + 1), -(n->n + 1));
}
