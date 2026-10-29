/* Compiler: statements (if/while/for, match, try, assignment, the statement dispatcher). */
#include "compiler_int.h"

static void stmt(Comp *c, Node *n);

/* statements                                                        */
/* ---------------------------------------------------------------- */
int at_module_top(const Comp *c) { return c->fs->is_module && c->fs->scope_depth == 0; }

/* declare a variable whose initial value is on top of the stack, and store it */
static void bind_new_var(Comp *c, Node *where, Decl *d) {
    if (at_module_top(c)) {
        int idx = declare_mvar(c, where, d->name, d->len, d->is_const, NULL);
        op_u16(c, OP_STORE_MODULE, (unsigned)idx, -1);
        return;
    }
    int li = declare_local(c, where, d);
    int slot = c->fs->loc[li].slot;
    op_u16(c, OP_STORE_LOCAL, (unsigned)slot, -1);
    if (d->captured) {
        op_u16(c, OP_MAKE_CELL, (unsigned)slot, 0);
        c->fs->loc[li].cell_ready = 1;
    }
}

static Loop *push_loop(Comp *c, const Node *n) {
    FS *fs = c->fs;
    if (fs->nloops == fs->caploops) {
        fs->caploops = fs->caploops ? fs->caploops * 2 : 4;
        fs->loops = xrealloc(fs->loops, sizeof(Loop) * (size_t)fs->caploops);
    }
    Loop *l = &fs->loops[fs->nloops++];
    memset(l, 0, sizeof *l);
    l->label = n->str;
    l->llen = n->slen;
    l->nexits = fs->nexits;
    for (int i = 0; i < fs->nloops - 1; i++)
        if (n->str && fs->loops[i].label && fs->loops[i].llen == n->slen &&
            memcmp(fs->loops[i].label, n->str, n->slen) == 0)
            cerr(c, n, "ParseError", E_PARSE, "duplicate loop label '%s'", n->str);
    return l;
}

static void pop_loop(Comp *c) {
    FS *fs = c->fs;
    Loop *l = &fs->loops[--fs->nloops];
    for (int i = 0; i < l->nbreaks; i++) patch_here(c, l->breaks[i]);
    free(l->breaks);
}

static Loop *find_loop(Comp *c, const Node *n) {
    FS *fs = c->fs;
    if (fs->nloops == 0) cerr(c, n, "ParseError", E_PARSE, "'%s' outside of a loop", n->kind == S_BREAK ? "break" : "continue");
    if (!n->str) return &fs->loops[fs->nloops - 1];
    for (int i = fs->nloops - 1; i >= 0; i--)
        if (fs->loops[i].label && fs->loops[i].llen == n->slen && memcmp(fs->loops[i].label, n->str, n->slen) == 0)
            return &fs->loops[i];
    cerr(c, n, "NameError", E_NAME, "unknown loop label '%s'", n->str);
}

static void exit_push(Comp *c, int is_defer, int slot) {
    FS *fs = c->fs;
    if (fs->nexits == fs->capexits) {
        fs->capexits = fs->capexits ? fs->capexits * 2 : 8;
        fs->exits = xrealloc(fs->exits, sizeof(ExitEnt) * (size_t)fs->capexits);
    }
    fs->exits[fs->nexits].is_defer = is_defer;
    fs->exits[fs->nexits].slot = slot;
    fs->nexits++;
}

/* Leave every try/defer scope above index `lo`, innermost first (SPEC 17.5). */
static void emit_exits(Comp *c, int lo) {
    FS *fs = c->fs;
    for (int i = fs->nexits - 1; i >= lo; i--) {
        if (fs->exits[i].is_defer) op_u16(c, OP_RUN_DEFERS, (unsigned)fs->exits[i].slot, 0);
        else op0(c, OP_TRY_END, 0);
    }
}

static int exits_have_defer(const FS *fs) {
    for (int i = 0; i < fs->nexits; i++)
        if (fs->exits[i].is_defer) return 1;
    return 0;
}

static void compile_if(Comp *c, Node *n) {
    uint32_t *ends = malloc(sizeof(uint32_t) * (size_t)(n->n / 2 + 1));
    if (!ends) exit(14);
    front_push(c->F, front_free_ptr, ends);
    int nend = 0;
    for (int i = 0; i < n->n; i += 2) {
        expr(c, n->list[i]);
        setpos(c, n->list[i]);
        uint32_t jf = jump_fwd(c, OP_JUMP_IF_FALSE, -1);
        begin_scope(c);
        block_stmts(c, n->list[i + 1]);
        end_scope(c);
        if (i + 2 < n->n || n->c) ends[nend++] = jump_fwd(c, OP_JUMP, 0);
        patch_here(c, jf);
    }
    if (n->c) {
        begin_scope(c);
        block_stmts(c, n->c);
        end_scope(c);
    }
    for (int i = 0; i < nend; i++) patch_here(c, ends[i]);
    front_pop(c->F);
    free(ends);
}

static void compile_while(Comp *c, Node *n) {
    uint32_t start = c->fs->p->code_len;
    expr(c, n->a);
    setpos(c, n);
    uint32_t jf = jump_fwd(c, OP_JUMP_IF_FALSE, -1);
    Loop *l = push_loop(c, n);
    l->cont_pc = start;
    begin_scope(c);
    block_stmts(c, n->b);
    end_scope(c);
    setpos(c, n);
    jump_back(c, OP_JUMP, start, 0);
    patch_here(c, jf);
    pop_loop(c);
}

static void store_loop_vars(Comp *c, Node *n) {
    int nv = (int)n->ival;
    int li1 = declare_local(c, n, n->decl);
    int li2 = nv == 2 ? declare_local(c, n, n->decl2) : -1;
    FS *fs = c->fs;
    if (li2 >= 0) op_u16(c, OP_STORE_LOCAL, (unsigned)fs->loc[li2].slot, -1);
    op_u16(c, OP_STORE_LOCAL, (unsigned)fs->loc[li1].slot, -1);
    if (n->decl->captured) {
        op_u16(c, OP_MAKE_CELL, (unsigned)fs->loc[li1].slot, 0);
        fs->loc[li1].cell_ready = 1;
    }
    if (li2 >= 0 && n->decl2->captured) {
        op_u16(c, OP_MAKE_CELL, (unsigned)fs->loc[li2].slot, 0);
        fs->loc[li2].cell_ready = 1;
    }
}

static int is_range_call(Comp *c, Node *it) {
    if (it->kind != N_CALL || it->flag || it->a->kind != N_NAME) return 0;
    if (it->a->slen != 5 || memcmp(it->a->str, "range", 5) != 0) return 0;
    if (it->n < 1 || it->n > 3) return 0;
    Res r = resolve_name(c, it->a);
    return r.kind == R_LINK;
}

static void compile_for(Comp *c, Node *n) {
    begin_scope(c);
    int range = is_range_call(c, n->a);
    if (range && n->ival == 2) cerr(c, n, "TypeError", E_TYPE, "range() yields one value; use a single loop variable");
    int li = declare_hidden(c, n);
    (void)declare_hidden(c, n);
    (void)declare_hidden(c, n);
    int s = c->fs->loc[li].slot;
    uint32_t start;
    uint32_t jend;
    Loop *l;
    if (range) {
        Node *it = n->a;
        if (it->n == 1) emit_int(c, 0);
        for (int i = 0; i < it->n; i++) expr(c, it->list[i]);
        if (it->n < 3) emit_int(c, 1);
        setpos(c, it);
        op_u16(c, OP_RANGE_PREP, (unsigned)s, -3);
        start = c->fs->p->code_len;
        setpos(c, n);
        emit_op(c, OP_RANGE_NEXT, 1);
        e16(c, (unsigned)s);
        jend = c->fs->p->code_len;
        e32(c, 0);
    } else {
        expr(c, n->a);
        setpos(c, n);
        op_u16(c, OP_FOR_PREP, (unsigned)s, -1);
        start = c->fs->p->code_len;
        setpos(c, n);
        emit_op(c, OP_FOR_NEXT, (int)n->ival);
        e16(c, (unsigned)s);
        e8(c, (unsigned)n->ival);
        jend = c->fs->p->code_len;
        e32(c, 0);
    }
    l = push_loop(c, n);
    l->cont_pc = start;
    begin_scope(c);
    store_loop_vars(c, n);
    block_stmts(c, n->b);
    end_scope(c);
    setpos(c, n);
    jump_back(c, OP_JUMP, start, 0);
    patch_here(c, jend);
    pop_loop(c);
    end_scope(c);
}


/* ---- match (SPEC 11.4) ---- */
typedef struct FailList {
    uint32_t *at;
    int n, cap;
} FailList;

static void fail_push(FailList *f, uint32_t at) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->at = xrealloc(f->at, sizeof(uint32_t) * (size_t)f->cap);
    }
    f->at[f->n++] = at;
}

static void faillist_free(void *p) { free(((FailList *)p)->at); }

/* Match the value held in hidden local `vs` against pattern `p`. Every
 * failure path jumps with an empty operand stack. */
static void compile_pattern(Comp *c, Node *p, int vs, FailList *fail) {
    setpos(c, p);
    switch (p->kind) {
    case P_WILD: break;
    case P_BIND:
        op_u16(c, OP_LOAD_LOCAL, (unsigned)vs, 1);
        bind_new_var(c, p, p->decl);
        break;
    case P_LIT:
        op_u16(c, OP_LOAD_LOCAL, (unsigned)vs, 1);
        expr(c, p->a);
        op0(c, OP_EQ, -1);
        fail_push(fail, jump_fwd(c, OP_JUMP_IF_FALSE, -1));
        break;
    case P_ARRAY:
        op_u16(c, OP_LOAD_LOCAL, (unsigned)vs, 1);
        op_u16(c, OP_TEST_ARRAY, (unsigned)p->n, 0);
        fail_push(fail, jump_fwd(c, OP_JUMP_IF_FALSE, -1));
        for (int i = 0; i < p->n; i++) {
            Node *sp = p->list[i];
            if (sp->kind == P_WILD) continue;
            op_u16(c, OP_LOAD_LOCAL, (unsigned)vs, 1);
            emit_int(c, i);
            op0(c, OP_GET_INDEX, -1);
            int ti = declare_hidden(c, sp);
            int ts = c->fs->loc[ti].slot;
            op_u16(c, OP_STORE_LOCAL, (unsigned)ts, -1);
            compile_pattern(c, sp, ts, fail);
        }
        break;
    case P_TABLE:
        for (int i = 0; i < p->n; i++) {
            Node *e = p->list[i];
            int k = str_const(c, e->str, e->slen);
            op_u16(c, OP_LOAD_LOCAL, (unsigned)vs, 1);
            op_u16(c, OP_TEST_HAS, (unsigned)k, 0);
            fail_push(fail, jump_fwd(c, OP_JUMP_IF_FALSE, -1));
            if (e->a->kind == P_WILD) continue;
            op_u16(c, OP_LOAD_LOCAL, (unsigned)vs, 1);
            op_u16(c, OP_GET_MEMBER, (unsigned)k, 0);
            int ti = declare_hidden(c, e->a);
            int ts = c->fs->loc[ti].slot;
            op_u16(c, OP_STORE_LOCAL, (unsigned)ts, -1);
            compile_pattern(c, e->a, ts, fail);
        }
        break;
    default: cerr(c, p, "ParseError", E_PARSE, "internal: bad pattern node");
    }
}

static void compile_match(Comp *c, Node *n) {
    setpos(c, n);
    begin_scope(c);
    expr(c, n->a);
    int si = declare_hidden(c, n);
    int ss = c->fs->loc[si].slot;
    op_u16(c, OP_STORE_LOCAL, (unsigned)ss, -1);
    uint32_t *ends = malloc(sizeof(uint32_t) * (size_t)n->n);
    if (!ends) exit(14);
    front_push(c->F, front_free_ptr, ends);
    for (int i = 0; i < n->n; i++) {
        Node *cs = n->list[i];
        FailList fail = {0, 0, 0};
        front_push(c->F, faillist_free, &fail);
        begin_scope(c);
        compile_pattern(c, cs->a, ss, &fail);
        if (cs->b) {
            expr(c, cs->b);
            fail_push(&fail, jump_fwd(c, OP_JUMP_IF_FALSE, -1));
        }
        block_stmts(c, cs->c);
        end_scope(c);
        ends[i] = jump_fwd(c, OP_JUMP, 0);
        for (int k = 0; k < fail.n; k++) patch_here(c, fail.at[k]);
        front_pop(c->F);
        free(fail.at);
    }
    setpos(c, n);
    op_u16(c, OP_LOAD_LOCAL, (unsigned)ss, 1);
    op0(c, OP_MATCH_FAIL, -1);
    for (int i = 0; i < n->n; i++) patch_here(c, ends[i]);
    front_pop(c->F);
    free(ends);
    end_scope(c);
}


static void compile_try(Comp *c, Node *n) {
    FS *fs = c->fs;
    int d0 = fs->depth;
    setpos(c, n);
    uint32_t hb = jump_fwd(c, OP_TRY_BEGIN, 0);
    exit_push(c, 0, 0);
    begin_scope(c);
    block_stmts(c, n->a);
    end_scope(c);
    fs->nexits--;
    op0(c, OP_TRY_END, 0);
    uint32_t je = jump_fwd(c, OP_JUMP, 0);
    patch_here(c, hb);
    fs->depth = d0 + 1; /* the VM pushes the error */
    if (fs->max_depth < fs->depth) fs->max_depth = fs->depth;
    begin_scope(c);
    if (n->decl) bind_new_var(c, n, n->decl);
    else op0(c, OP_POP, -1);
    block_stmts(c, n->b);
    end_scope(c);
    patch_here(c, je);
}

static void compile_assign(Comp *c, Node *n) {
    Node *t = n->a;
    int compound = n->op != TK_ASSIGN;
    int opc = n->op == TK_DOTDOTEQ ? OP_CONCAT : (compound ? binop_code(n->op) : -1);
    if (t->kind == N_NAME) {
        Res r = resolve_name(c, t);
        if (r.is_const || r.kind == R_LINK)
            cerr(c, n, "ConstError", E_CONST, "cannot assign to constant '%s'", t->str);
        if (compound) emit_load(c, r);
        expr(c, n->b);
        if (compound) {
            setpos(c, n);
            if (opc == OP_CONCAT) { emit_op(c, OP_CONCAT, -1); e8(c, 2); }
            else op0(c, opc, -1);
        }
        setpos(c, n);
        emit_store(c, r);
    } else if (t->kind == N_INDEX) {
        expr(c, t->a);
        expr(c, t->b);
        if (compound) {
            setpos(c, t);
            op0(c, OP_DUP2, 2);
            op0(c, OP_GET_INDEX, -1);
        }
        expr(c, n->b);
        if (compound) {
            setpos(c, n);
            if (opc == OP_CONCAT) { emit_op(c, OP_CONCAT, -1); e8(c, 2); }
            else op0(c, opc, -1);
        }
        setpos(c, t);
        op0(c, OP_SET_INDEX, -3);
    } else { /* N_MEMBER */
        if (member_user_slot(c, t->a, t) >= 0)
            cerr(c, n, "ConstError", E_CONST, "cannot assign to module member '%s'", t->str);
        if (!compound) check_member(c, t->a, t);
        expr(c, t->a);
        int k = str_const(c, t->str, t->slen);
        if (compound) {
            setpos(c, t);
            op0(c, OP_DUP, 1);
            op_u16(c, OP_GET_MEMBER, (unsigned)k, 0);
        }
        expr(c, n->b);
        if (compound) {
            setpos(c, n);
            if (opc == OP_CONCAT) { emit_op(c, OP_CONCAT, -1); e8(c, 2); }
            else op0(c, opc, -1);
        }
        setpos(c, t);
        op_u16(c, OP_SET_MEMBER, (unsigned)k, -2);
    }
}



static void stmt(Comp *c, Node *n) {
    FS *fs = c->fs;
    switch (n->kind) {
    case S_PASS: break;
    case S_MULTI:
        for (int i = 0; i < n->n; i++) stmt(c, n->list[i]);
        break;
    case S_EXPR:
        expr(c, n->a);
        op0(c, OP_POP, -1);
        break;
    case S_LET:
    case S_CONST:
        if (n->a) expr(c, n->a);
        else op0(c, OP_NIL, 1);
        setpos(c, n);
        bind_new_var(c, n, n->decl);
        if (n->exported) {
            if (!at_module_top(c)) cerr(c, n, "ParseError", E_PARSE, "'export' is only allowed at module top level");
            export_add(c, n, n->decl->name, n->decl->len, find_mvar(c, n->decl->name, n->decl->len));
        }
        break;
    case S_ASSIGN: compile_assign(c, n); break;
    case S_IF: compile_if(c, n); break;
    case S_WHILE: compile_while(c, n); break;
    case S_FOR: compile_for(c, n); break;
    case S_BREAK: {
        Loop *l = find_loop(c, n);
        emit_exits(c, l->nexits);
        setpos(c, n);
        uint32_t j = jump_fwd(c, OP_JUMP, 0);
        if (l->nbreaks == l->capbreaks) {
            l->capbreaks = l->capbreaks ? l->capbreaks * 2 : 4;
            l->breaks = xrealloc(l->breaks, sizeof(uint32_t) * (size_t)l->capbreaks);
        }
        l->breaks[l->nbreaks++] = j;
        break;
    }
    case S_CONTINUE: {
        Loop *l = find_loop(c, n);
        emit_exits(c, l->nexits);
        setpos(c, n);
        jump_back(c, OP_JUMP, l->cont_pc, 0);
        break;
    }
    case S_RETURN:
        if (fs->is_module) cerr(c, n, "ParseError", E_PARSE, "'return' outside of a function");
        if (fs->is_init && n->a) cerr(c, n, "ParseError", E_PARSE, "'return' with a value is not allowed in init");
        if (exits_have_defer(fs)) {
            if (n->a) expr(c, n->a);
            else op0(c, OP_NIL, 1);
            int ri = declare_hidden(c, n);
            int rs = fs->loc[ri].slot;
            op_u16(c, OP_STORE_LOCAL, (unsigned)rs, -1);
            setpos(c, n);
            emit_exits(c, 0);
            op_u16(c, OP_LOAD_LOCAL, (unsigned)rs, 1);
            op0(c, OP_RETURN, -1);
        } else if (n->a) {
            expr(c, n->a);
            setpos(c, n);
            op0(c, OP_RETURN, -1);
        } else {
            setpos(c, n);
            op0(c, OP_RETURN_NIL, 0);
        }
        break;
    case S_THROW:
        expr(c, n->a);
        setpos(c, n);
        op0(c, OP_THROW, -1);
        break;
    case S_PANIC:
        expr(c, n->a);
        setpos(c, n);
        op0(c, OP_PANIC, -1);
        break;
    case S_ASSERT: {
        expr(c, n->a);
        setpos(c, n);
        uint32_t ok = jump_fwd(c, OP_JUMP_IF_TRUE, -1);
        if (n->b) expr(c, n->b);
        else op0(c, OP_NIL, 1);
        setpos(c, n);
        op0(c, OP_ASSERT_FAIL, -1);
        patch_here(c, ok);
        break;
    }
    case S_TRY: compile_try(c, n); break;
    case S_YIELD:
        if (fs->is_module) cerr(c, n, "ParseError", E_PARSE, "'yield' outside of a function");
        if (n->a) expr(c, n->a);
        else op0(c, OP_NIL, 1);
        setpos(c, n);
        op0(c, OP_YIELD, -1);
        break;
    case S_MATCH: compile_match(c, n); break;
    case S_CLASS: compile_class(c, n); break;
    case S_DEFER: {
        Node *call = n->a;
        Node *callee = call->a;
        int fixed = call->n;
        if (fixed > 255) cerr(c, n, "ParseError", E_PARSE, "too many arguments (limit 255)");
        if (callee->kind == N_MEMBER && member_user_slot(c, callee->a, callee) < 0) {
            check_member(c, callee->a, callee);
            expr(c, callee->a);
            for (int i = 0; i < fixed; i++) expr(c, call->list[i]);
            setpos(c, n);
            int nameidx = str_const(c, callee->str, callee->slen);
            emit_op(c, OP_DEFER_INVOKE, -(fixed + 1));
            e16(c, (unsigned)nameidx);
            e8(c, (unsigned)fixed);
        } else {
            expr(c, callee);
            for (int i = 0; i < fixed; i++) expr(c, call->list[i]);
            setpos(c, n);
            op_u8(c, OP_DEFER, (unsigned)fixed, -(fixed + 1));
        }
        break;
    }
    case S_BLOCK:
        begin_scope(c);
        block_stmts(c, n);
        end_scope(c);
        break;
    case S_IMPORT: compile_import(c, n); break;
    case S_FROM: compile_from(c, n); break;
    case S_FUNC: {
        if (at_module_top(c)) {
            /* hoisted: the closure was created at module start; compile the body now */
            Proto *shell = c->prog->protos[n->ival];
            (void)compile_func(c, n->a, shell);
            break;
        }
        int li = declare_local(c, n, n->decl);
        int slot = fs->loc[li].slot;
        op0(c, OP_NIL, 1);
        op_u16(c, OP_STORE_LOCAL, (unsigned)slot, -1);
        if (n->decl->captured) {
            op_u16(c, OP_MAKE_CELL, (unsigned)slot, 0);
            fs->loc[li].cell_ready = 1;
        }
        int idx = compile_func(c, n->a, NULL);
        setpos(c, n);
        emit_op(c, OP_CLOSURE, 1);
        e32(c, (uint32_t)idx);
        op_u16(c, fs->loc[li].cell_ready ? OP_STORE_CELL : OP_STORE_LOCAL, (unsigned)slot, -1);
        break;
    }
    default:
        cerr(c, n, "ParseError", E_PARSE, "internal: unexpected statement node %d", n->kind);
    }
}

void stmt_list(Comp *c, Node **list, int n) {
    int has_defer = 0;
    for (int i = 0; i < n; i++)
        if (list[i]->kind == S_DEFER) has_defer = 1;
    int slot = 0;
    if (has_defer) {
        int li = declare_hidden(c, list[0]);
        slot = c->fs->loc[li].slot;
        op_u16(c, OP_DEFER_SCOPE, (unsigned)slot, 0);
        exit_push(c, 1, slot);
    }
    for (int i = 0; i < n; i++) stmt(c, list[i]);
    if (has_defer) {
        c->fs->nexits--;
        op_u16(c, OP_RUN_DEFERS, (unsigned)slot, 0);
    }
}

void block_stmts(Comp *c, Node *b) { stmt_list(c, b->list, b->n); }

/* ---------------------------------------------------------------- */
