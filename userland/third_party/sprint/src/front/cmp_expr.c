/* Compiler: expressions. */
#include "compiler_int.h"

/* expressions                                                       */
/* ---------------------------------------------------------------- */

int binop_code(int tk) {
    switch (tk) {
    case TK_PLUS: case TK_PLUSEQ: return OP_ADD;
    case TK_MINUS: case TK_MINUSEQ: return OP_SUB;
    case TK_STAR: case TK_STAREQ: return OP_MUL;
    case TK_SLASH: case TK_SLASHEQ: return OP_DIV;
    case TK_SLASHSLASH: case TK_SLASHSLASHEQ: return OP_FLOOR_DIV;
    case TK_PERCENT: case TK_PERCENTEQ: return OP_MOD;
    case TK_STARSTAR: case TK_STARSTAREQ: return OP_POW;
    case TK_AMP: case TK_AMPEQ: return OP_BAND;
    case TK_PIPE: case TK_PIPEEQ: return OP_BOR;
    case TK_CARET: case TK_CARETEQ: return OP_BXOR;
    case TK_SHL: case TK_SHLEQ: return OP_SHL;
    case TK_SHR: case TK_SHREQ: return OP_SHR;
    case TK_EQEQ: return OP_EQ;
    case TK_NE: return OP_NE;
    case TK_LT: return OP_LT;
    case TK_LE: return OP_LE;
    case TK_GT: return OP_GT;
    case TK_GE: return OP_GE;
    default: return -1;
    }
}

void emit_int(Comp *c, int64_t v) {
    if (v >= -32768 && v <= 32767) {
        emit_op(c, OP_SMALLINT, 1);
        e16(c, (unsigned)(uint16_t)(int16_t)v);
    } else {
        op_u16(c, OP_CONST, (unsigned)add_const(c, v_int(v)), 1);
    }
}

static void collect_concat(Node *n, Node **out, int *cnt, int cap) {
    if (n->kind == N_BINARY && n->op == TK_DOTDOT) {
        collect_concat(n->a, out, cnt, cap);
        if (*cnt < cap) out[(*cnt)++] = n->b;
    } else if (*cnt < cap) {
        out[(*cnt)++] = n;
    }
}

static int count_concat(const Node *n) {
    if (n->kind == N_BINARY && n->op == TK_DOTDOT) return count_concat(n->a) + 1;
    return 1;
}

void check_member(Comp *c, const Node *obj, const Node *m) {
    /* `m.name` where m is an `import m` binding: verify at compile time (SPEC 5.3) */
    if (obj->kind != N_NAME) return;
    Res r = resolve_name(c, obj);
    if (r.kind != R_MODULE || !r.modname) return;
    const ModDesc *md = lib_find_module(c->vm, r.modname, strlen(r.modname));
    if (md && !lib_module_has(md, m->str, m->slen))
        cerr(c, m, "NameError", E_NAME, "module '%s' has no export '%s'", r.modname, m->str);
}

static void compile_call(Comp *c, Node *n) {
    Node *callee = n->a;
    int spread = n->flag;
    int fixed = spread ? n->n - 1 : n->n;
    if (n->n > 255 || fixed > 254) cerr(c, n, "ParseError", E_PARSE, "too many arguments (limit 255)");
    if (callee->kind == N_MEMBER && member_user_slot(c, callee->a, callee) < 0) {
        check_member(c, callee->a, callee);
        expr(c, callee->a);
        for (int i = 0; i < fixed; i++) expr(c, n->list[i]);
        if (spread) expr(c, n->list[n->n - 1]);
        setpos(c, n);
        int nameidx = str_const(c, callee->str, callee->slen);
        emit_op(c, spread ? OP_INVOKE_SPREAD : OP_INVOKE, spread ? -(fixed + 1) : -fixed);
        e16(c, (unsigned)nameidx);
        e8(c, (unsigned)fixed);
        return;
    }
    expr(c, callee);
    for (int i = 0; i < fixed; i++) expr(c, n->list[i]);
    if (spread) expr(c, n->list[n->n - 1]);
    setpos(c, n);
    op_u8(c, spread ? OP_CALL_SPREAD : OP_CALL, (unsigned)fixed, spread ? -(fixed + 1) : -fixed);
}

void expr(Comp *c, Node *n) {
    switch (n->kind) {
    case N_NIL: setpos(c, n); op0(c, OP_NIL, 1); break;
    case N_TRUE: setpos(c, n); op0(c, OP_TRUE, 1); break;
    case N_FALSE: setpos(c, n); op0(c, OP_FALSE, 1); break;
    case N_INT: setpos(c, n); emit_int(c, n->ival); break;
    case N_FLOAT: setpos(c, n); op_u16(c, OP_CONST, (unsigned)add_const(c, v_float(n->fval)), 1); break;
    case N_STRING: setpos(c, n); emit_const_str(c, n->str, n->slen); break;
    case N_NAME: {
        setpos(c, n);
        Res nr = resolve_name(c, n);
        if (nr.kind == R_MODULE && nr.user_mod >= 0)
            cerr(c, n, "TypeError", E_TYPE, "module '%s' can only be used as '%s.name' in this build", n->str, n->str);
        emit_load(c, nr);
        break;
    }
    case N_ARRAY:
        for (int i = 0; i < n->n; i++) expr(c, n->list[i]);
        if (n->n > 65535) cerr(c, n, "ParseError", E_PARSE, "array literal too large");
        setpos(c, n);
        op_u16(c, OP_NEW_ARRAY, (unsigned)n->n, 1 - n->n);
        break;
    case N_TABLE: {
        int pairs = n->n / 2;
        if (pairs > 32767) cerr(c, n, "ParseError", E_PARSE, "table literal too large");
        for (int i = 0; i < n->n; i++) expr(c, n->list[i]);
        setpos(c, n);
        op_u16(c, OP_NEW_TABLE, (unsigned)pairs, 1 - n->n);
        break;
    }
    case N_FUNC: {
        int idx = compile_func(c, n, NULL);
        setpos(c, n);
        emit_op(c, OP_CLOSURE, 1);
        e32(c, (uint32_t)idx);
        break;
    }
    case N_UNARY:
        if (n->op == TK_MINUS && n->a->kind == N_INT) { setpos(c, n); emit_int(c, -n->a->ival); break; }
        if (n->op == TK_MINUS && n->a->kind == N_FLOAT) {
            setpos(c, n);
            op_u16(c, OP_CONST, (unsigned)add_const(c, v_float(-n->a->fval)), 1);
            break;
        }
        expr(c, n->a);
        setpos(c, n);
        op0(c, n->op == TK_MINUS ? OP_NEG : n->op == TK_TILDE ? OP_BNOT : OP_NOT, 0);
        break;
    case N_BINARY: {
        if (n->op == TK_DOTDOT) {
            int total = count_concat(n);
            Node **ops = malloc(sizeof(Node *) * (size_t)total);
            if (!ops) exit(14);
            front_push(c->F, front_free_ptr, ops);
            int cnt = 0;
            collect_concat(n, ops, &cnt, total);
            int i = 0;
            int first = 1;
            while (i < cnt) {
                int take = first ? (cnt - i > 255 ? 255 : cnt - i) : (cnt - i > 254 ? 254 : cnt - i);
                for (int k = 0; k < take; k++) expr(c, ops[i + k]);
                i += take;
                int total_ops = take + (first ? 0 : 1);
                setpos(c, n);
                emit_op(c, OP_CONCAT, 1 - total_ops);
                e8(c, (unsigned)total_ops);
                first = 0;
            }
            front_pop(c->F);
            free(ops);
            break;
        }
        expr(c, n->a);
        expr(c, n->b);
        setpos(c, n);
        op0(c, binop_code(n->op), -1);
        break;
    }
    case N_AND:
    case N_OR: {
        expr(c, n->a);
        setpos(c, n);
        uint32_t j = jump_fwd(c, n->kind == N_AND ? OP_JUMP_IF_FALSE_OR_POP : OP_JUMP_IF_TRUE_OR_POP, -1);
        expr(c, n->b);
        patch_here(c, j);
        break;
    }
    case N_SEQ: /* comprehension: statements, then the result expression (SPEC 11.5) */
        begin_scope(c);
        block_stmts(c, n->a);
        expr(c, n->b);
        end_scope(c);
        break;
    case N_COALESCE: {
        /* a ?? b : a unless a is nil (b is evaluated only then) */
        expr(c, n->a);
        op0(c, OP_DUP, 1);
        op0(c, OP_NIL, 1);
        op0(c, OP_NE, -1);
        setpos(c, n);
        uint32_t j = jump_fwd(c, OP_JUMP_IF_TRUE, -1);
        op0(c, OP_POP, -1);
        expr(c, n->b);
        patch_here(c, j);
        break;
    }
    case N_COND: {
        expr(c, n->a);
        setpos(c, n);
        uint32_t jf = jump_fwd(c, OP_JUMP_IF_FALSE, -1);
        expr(c, n->b);
        uint32_t je = jump_fwd(c, OP_JUMP, 0);
        c->fs->depth -= 1;
        patch_here(c, jf);
        expr(c, n->c);
        patch_here(c, je);
        break;
    }
    case N_CALL: compile_call(c, n); break;
    case N_SUPER_CALL: compile_super_call(c, n); break;
    case N_SUPER_METHOD: compile_super_method(c, n); break;
    case N_INDEX:
        expr(c, n->a);
        expr(c, n->b);
        setpos(c, n);
        op0(c, OP_GET_INDEX, -1);
        break;
    case N_MEMBER: {
        int us = member_user_slot(c, n->a, n);
        if (us >= 0) {
            setpos(c, n);
            op_u16(c, OP_LOAD_MODULE, (unsigned)us, 1);
            break;
        }
        check_member(c, n->a, n);
        expr(c, n->a);
        setpos(c, n);
        op_u16(c, OP_GET_MEMBER, (unsigned)str_const(c, n->str, n->slen), 0);
        break;
    }
    default:
        cerr(c, n, "ParseError", E_PARSE, "internal: unexpected expression node %d", n->kind);
    }
}

/* ---------------------------------------------------------------- */
