#include "parser/python_code_to_c_ast.h"
#include "common/python_code_to_c_common.h"

/* ========================================
 * ASTノード作成
 * ======================================== */

P2C_AstNode* p2c_ast_new(P2C_Allocator *a, P2C_AstType type, uint32_t line, uint32_t col) {
    P2C_AstNode *node = p2c_alloc(a, sizeof(P2C_AstNode));
    if (!node) return NULL;
    memset(node, 0, sizeof(P2C_AstNode));
    node->type = type;
    node->line = line;
    node->col = col;
    return node;
}

P2C_AstExpr* p2c_ast_expr_new(P2C_Allocator *a, P2C_AstType type, uint32_t line, uint32_t col) {
    return (P2C_AstExpr*)p2c_ast_new(a, type, line, col);
}

P2C_AstStmt* p2c_ast_stmt_new(P2C_Allocator *a, P2C_AstType type, uint32_t line, uint32_t col) {
    return (P2C_AstStmt*)p2c_ast_new(a, type, line, col);
}

/* ========================================
 * ASTノード破棄（再帰的）
 * ======================================== */

static void free_expr(P2C_AstExpr *expr, P2C_Allocator *a);
static void free_stmt(P2C_AstStmt *stmt, P2C_Allocator *a);

static void free_expr_list(P2C_Vector *v, P2C_Allocator *a) {
    if (!v) return;
    for (size_t i = 0; i < p2c_vec_len(v); i++) {
        free_expr((P2C_AstExpr*)p2c_vec_get(v, i), a);
    }
    /* 上のループで各要素は既に解放済み。p2c_vec_free()はv->free_fnが
     * 設定されている場合に内部でp2c_vec_clear()を呼び、残っている要素に
     * 対して再度free_fnを呼んでしまう（二重解放）ため、
     * ここでlenを0にしてからvec_freeする。 */
    v->len = 0;
    p2c_vec_free(v);
}

static void free_stmt_list(P2C_Vector *v, P2C_Allocator *a) {
    if (!v) return;
    for (size_t i = 0; i < p2c_vec_len(v); i++) {
        free_stmt((P2C_AstStmt*)p2c_vec_get(v, i), a);
    }
    /* free_expr_listと同じ理由でlenを0にしてから解放する。 */
    v->len = 0;
    p2c_vec_free(v);
}

static void free_match_pattern(P2C_AstMatchPattern *pattern, P2C_Allocator *a) {
    if (!pattern) return;
    free_expr(pattern->value, a);
    if (pattern->capture_name) p2c_free(a, pattern->capture_name);
    if (pattern->rest_name) p2c_free(a, pattern->rest_name);
    if (pattern->class_name) p2c_free(a, pattern->class_name);
    if (pattern->attr_names) {
        for (size_t i = 0; i < p2c_vec_len(pattern->attr_names); i++) p2c_free(a, p2c_vec_get(pattern->attr_names, i));
        pattern->attr_names->len = 0;
        p2c_vec_free(pattern->attr_names);
    }
    if (pattern->children) {
        for (size_t i = 0; i < p2c_vec_len(pattern->children); i++) {
            free_match_pattern((P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i), a);
        }
        pattern->children->len = 0;
        p2c_vec_free(pattern->children);
    }
    free_expr_list(pattern->keys, a);
    p2c_free(a, pattern);
}

static void free_expr(P2C_AstExpr *expr, P2C_Allocator *a) {
    if (!expr) return;
    P2C_AstNode *n = &expr->base;
    
    switch (n->type) {
        case AST_NAME:
            if (n->u.name.name) p2c_free(a, n->u.name.name);
            break;
        case AST_CONST:
            if (n->u.constant.value) p2c_free(a, n->u.constant.value);
            break;
        case AST_BINOP:
            free_expr((P2C_AstExpr*)n->u.binop.left, a);
            free_expr((P2C_AstExpr*)n->u.binop.right, a);
            break;
        case AST_UNARYOP:
            free_expr((P2C_AstExpr*)n->u.unaryop.operand, a);
            break;
        case AST_COMPARE:
            free_expr((P2C_AstExpr*)n->u.compare.left, a);
            free_expr_list(n->u.compare.comparators, a);
            if (n->u.compare.ops) p2c_vec_free(n->u.compare.ops);
            break;
        case AST_BOOLOP:
            free_expr_list(n->u.boolop.values, a);
            break;
        case AST_CALL: {
            free_expr((P2C_AstExpr*)n->u.call.func, a);
            free_expr_list(n->u.call.args, a);
            if (n->u.call.keywords) {
                for (size_t i = 0; i < p2c_vec_len(n->u.call.keywords); i++) {
                    P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, i);
                    if (kw->arg) p2c_free(a, kw->arg);
                    free_expr((P2C_AstExpr*)kw->value, a);
                    p2c_free(a, kw);
                }
                p2c_vec_free(n->u.call.keywords);
            }
            break;
        }
        case AST_ATTRIBUTE:
            free_expr((P2C_AstExpr*)n->u.attribute.value, a);
            if (n->u.attribute.attr) p2c_free(a, n->u.attribute.attr);
            break;
        case AST_SUBSCRIPT:
            free_expr((P2C_AstExpr*)n->u.subscript.value, a);
            free_expr((P2C_AstExpr*)n->u.subscript.slice, a);
            break;
        case AST_IFEXP:
            free_expr((P2C_AstExpr*)n->u.ifexp.test, a);
            free_expr((P2C_AstExpr*)n->u.ifexp.body, a);
            free_expr((P2C_AstExpr*)n->u.ifexp.orelse, a);
            break;
        case AST_LAMBDA:
            if (n->u.lambda.args) {
                for (size_t i = 0; i < p2c_vec_len(n->u.lambda.args); i++) {
                    P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(n->u.lambda.args, i);
                    if (arg->name) p2c_free(a, arg->name);
                    if (arg->annotation) free_expr((P2C_AstExpr*)arg->annotation, a);
                    if (arg->default_val) free_expr((P2C_AstExpr*)arg->default_val, a);
                    p2c_free(a, arg);
                }
                p2c_vec_free(n->u.lambda.args);
            }
            free_expr((P2C_AstExpr*)n->u.lambda.body, a);
            break;
        case AST_LIST:
        case AST_SET:
            free_expr_list(n->u.list.elts, a);
            break;
        case AST_TUPLE:
            free_expr_list(n->u.tuple.elts, a);
            break;
        case AST_DICT:
            free_expr_list(n->u.dict.keys, a);
            free_expr_list(n->u.dict.values, a);
            break;
        case AST_COMPREHENSION:
        case AST_GENERATOR_EXPRESSION:
            free_expr((P2C_AstExpr*)n->u.comprehension.elt, a);
            free_expr((P2C_AstExpr*)n->u.comprehension.dict_key, a);
            if (n->u.comprehension.generators) {
                for (size_t i = 0; i < p2c_vec_len(n->u.comprehension.generators); i++) {
                    P2C_AstComprehensionGen *gen = (P2C_AstComprehensionGen*)p2c_vec_get(n->u.comprehension.generators, i);
                    if (!gen) continue;
                    free_expr(gen->target, a);
                    free_expr(gen->iter, a);
                    free_expr_list(gen->ifs, a);
                    p2c_free(a, gen);
                }
                n->u.comprehension.generators->len = 0;
                p2c_vec_free(n->u.comprehension.generators);
            }
            break;
        case AST_STARRED:
            free_expr((P2C_AstExpr*)n->u.starred.value, a);
            break;
        case AST_NAMED_EXPR:
            free_expr((P2C_AstExpr*)n->u.named_expr.target, a);
            free_expr((P2C_AstExpr*)n->u.named_expr.value, a);
            break;
        case AST_YIELD:
            free_expr((P2C_AstExpr*)n->u.yield_expr.value, a);
            break;
        case AST_AWAIT:
            free_expr((P2C_AstExpr*)n->u.await_expr.value, a);
            break;
        default:
            break;
    }
    p2c_free(a, expr);
}

static void free_stmt(P2C_AstStmt *stmt, P2C_Allocator *a) {
    if (!stmt) return;
    P2C_AstNode *n = &stmt->base;
    
    switch (n->type) {
        case AST_MODULE:
            free_stmt_list(n->u.module.body, a);
            break;
        case AST_ASSIGN:
            free_expr_list(n->u.assign.targets, a);
            free_expr((P2C_AstExpr*)n->u.assign.value, a);
            break;
        case AST_AUGASSIGN:
            free_expr((P2C_AstExpr*)n->u.augassign.target, a);
            free_expr((P2C_AstExpr*)n->u.augassign.value, a);
            break;
        case AST_ANNASSIGN:
            free_expr((P2C_AstExpr*)n->u.annassign.target, a);
            if (n->u.annassign.annotation) free_expr((P2C_AstExpr*)n->u.annassign.annotation, a);
            if (n->u.annassign.value) free_expr((P2C_AstExpr*)n->u.annassign.value, a);
            break;
        case AST_RETURN:
            if (n->u.return_stmt.value) free_expr((P2C_AstExpr*)n->u.return_stmt.value, a);
            break;
        case AST_EXPR_STMT:
            free_expr((P2C_AstExpr*)n->u.expr_stmt.value, a);
            break;
        case AST_IF:
            free_expr((P2C_AstExpr*)n->u.if_stmt.test, a);
            free_stmt_list(n->u.if_stmt.body, a);
            free_stmt_list(n->u.if_stmt.orelse, a);
            break;
        case AST_WHILE:
            free_expr((P2C_AstExpr*)n->u.while_stmt.test, a);
            free_stmt_list(n->u.while_stmt.body, a);
            free_stmt_list(n->u.while_stmt.orelse, a);
            break;
        case AST_FOR:
        case AST_ASYNC_FOR:
            free_expr((P2C_AstExpr*)n->u.for_stmt.target, a);
            free_expr((P2C_AstExpr*)n->u.for_stmt.iter, a);
            free_stmt_list(n->u.for_stmt.body, a);
            free_stmt_list(n->u.for_stmt.orelse, a);
            break;
        case AST_TRY: {
            free_stmt_list(n->u.try_stmt.body, a);
            if (n->u.try_stmt.handlers) {
                for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
                    P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
                    if (h->type) free_expr((P2C_AstExpr*)h->type, a);
                    if (h->name) p2c_free(a, h->name);
                    free_stmt_list(h->body, a);
                    p2c_free(a, h);
                }
                p2c_vec_free(n->u.try_stmt.handlers);
            }
            free_stmt_list(n->u.try_stmt.orelse, a);
            free_stmt_list(n->u.try_stmt.finalbody, a);
            break;
        }
        case AST_RAISE:
            if (n->u.raise.exc) free_expr((P2C_AstExpr*)n->u.raise.exc, a);
            if (n->u.raise.cause) free_expr((P2C_AstExpr*)n->u.raise.cause, a);
            break;
        case AST_ASSERT:
            free_expr((P2C_AstExpr*)n->u.assert_stmt.test, a);
            if (n->u.assert_stmt.msg) free_expr((P2C_AstExpr*)n->u.assert_stmt.msg, a);
            break;
        case AST_MATCH:
            free_expr(n->u.match_stmt.subject, a);
            if (n->u.match_stmt.cases) {
                for (size_t i = 0; i < p2c_vec_len(n->u.match_stmt.cases); i++) {
                    P2C_AstMatchCase *match_case = (P2C_AstMatchCase*)p2c_vec_get(n->u.match_stmt.cases, i);
                    if (!match_case) continue;
                    free_match_pattern(match_case->pattern, a);
                    free_expr(match_case->guard, a);
                    free_stmt_list(match_case->body, a);
                    p2c_free(a, match_case);
                }
                p2c_vec_free(n->u.match_stmt.cases);
            }
            break;
        case AST_FUNCTIONDEF:
            if (n->u.functiondef.name) p2c_free(a, n->u.functiondef.name);
            if (n->u.functiondef.args) {
                for (size_t i = 0; i < p2c_vec_len(n->u.functiondef.args); i++) {
                    P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(n->u.functiondef.args, i);
                    if (arg->name) p2c_free(a, arg->name);
                    if (arg->annotation) free_expr((P2C_AstExpr*)arg->annotation, a);
                    if (arg->default_val) free_expr((P2C_AstExpr*)arg->default_val, a);
                    p2c_free(a, arg);
                }
                p2c_vec_free(n->u.functiondef.args);
            }
            if (n->u.functiondef.vararg) p2c_free(a, n->u.functiondef.vararg);
            if (n->u.functiondef.kwarg) p2c_free(a, n->u.functiondef.kwarg);
            free_stmt_list(n->u.functiondef.body, a);
            free_expr_list(n->u.functiondef.decorator_list, a);
            if (n->u.functiondef.returns) free_expr((P2C_AstExpr*)n->u.functiondef.returns, a);
            break;
        case AST_CLASSDEF:
            if (n->u.classdef.name) p2c_free(a, n->u.classdef.name);
            free_expr_list(n->u.classdef.bases, a);
            if (n->u.classdef.keywords) {
                for (size_t i = 0; i < p2c_vec_len(n->u.classdef.keywords); i++) {
                    P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.classdef.keywords, i);
                    if (kw->arg) p2c_free(a, kw->arg);
                    free_expr((P2C_AstExpr*)kw->value, a);
                    p2c_free(a, kw);
                }
                p2c_vec_free(n->u.classdef.keywords);
            }
            free_stmt_list(n->u.classdef.body, a);
            free_expr_list(n->u.classdef.decorator_list, a);
            break;
        case AST_GLOBAL:
            if (n->u.global.names) {
                for (size_t i = 0; i < p2c_vec_len(n->u.global.names); i++) {
                    p2c_free(a, p2c_vec_get(n->u.global.names, i));
                }
                p2c_vec_free(n->u.global.names);
            }
            break;
        case AST_NONLOCAL:
            if (n->u.nonlocal_stmt.names) {
                for (size_t i = 0; i < p2c_vec_len(n->u.nonlocal_stmt.names); i++) {
                    p2c_free(a, p2c_vec_get(n->u.nonlocal_stmt.names, i));
                }
                p2c_vec_free(n->u.nonlocal_stmt.names);
            }
            break;
        case AST_IMPORT:
            if (n->u.import_stmt.names) {
                for (size_t i = 0; i < p2c_vec_len(n->u.import_stmt.names); i++) {
                    P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.import_stmt.names, i);
                    if (al->name) p2c_free(a, al->name);
                    if (al->asname) p2c_free(a, al->asname);
                    p2c_free(a, al);
                }
                p2c_vec_free(n->u.import_stmt.names);
            }
            break;
        case AST_IMPORTFROM:
            if (n->u.importfrom.module) p2c_free(a, n->u.importfrom.module);
            if (n->u.importfrom.names) {
                for (size_t i = 0; i < p2c_vec_len(n->u.importfrom.names); i++) {
                    P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.importfrom.names, i);
                    if (al->name) p2c_free(a, al->name);
                    if (al->asname) p2c_free(a, al->asname);
                    p2c_free(a, al);
                }
                p2c_vec_free(n->u.importfrom.names);
            }
            break;
        case AST_WITH:
            if (n->u.with.items) {
                for (size_t i = 0; i < p2c_vec_len(n->u.with.items); i++) {
                    P2C_AstWithItem *it = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, i);
                    free_expr((P2C_AstExpr*)it->context_expr, a);
                    if (it->optional_vars) free_expr((P2C_AstExpr*)it->optional_vars, a);
                    p2c_free(a, it);
                }
                p2c_vec_free(n->u.with.items);
            }
            free_stmt_list(n->u.with.body, a);
            break;
        case AST_DELETE:
            free_expr_list(n->u.delete.targets, a);
            break;
        case AST_BLOCK:
            /* free_stmt_list と同じ手順で解放する。以前は要素を個別に解放した後、
             * len を 0 にせず p2c_vec_free を呼んでいたため、ベクタ側の free_fn が
             * 解放済みの文をもう一度解放していた（heap-use-after-free）。
             * 1 行にセミコロンで複数文を書いた入力で再現していた。 */
            free_stmt_list(n->u.block.stmts, a);
            n->u.block.stmts = NULL;
            break;
        default:
            break;
    }
    p2c_free(a, stmt);
}

void p2c_ast_free(P2C_AstNode *node, P2C_Allocator *a) {
    if (!node) return;
    /* 文として解放（ノードタイプで判定） */
    switch (node->type) {
        case AST_BINOP: case AST_UNARYOP: case AST_COMPARE: case AST_BOOLOP:
        case AST_CALL: case AST_ATTRIBUTE: case AST_SUBSCRIPT: case AST_NAME:
        case AST_CONST: case AST_IFEXP: case AST_LAMBDA: case AST_LIST: case AST_SET:
        case AST_TUPLE: case AST_DICT: case AST_COMPREHENSION: case AST_GENERATOR_EXPRESSION: case AST_STARRED: case AST_NAMED_EXPR:
        case AST_YIELD: case AST_AWAIT:
            free_expr((P2C_AstExpr*)node, a);
            return;
        default:
            free_stmt((P2C_AstStmt*)node, a);
            return;
    }
}

void p2c_ast_expr_free(P2C_AstExpr *expr, P2C_Allocator *a) {
    free_expr(expr, a);
}

void p2c_ast_stmt_free(P2C_AstStmt *stmt, P2C_Allocator *a) {
    free_stmt(stmt, a);
}

/* ========================================
 * ヘルパー：ノード作成
 * ======================================== */

static char* dup_str(P2C_Allocator *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = p2c_alloc(a, len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

P2C_AstExpr* p2c_ast_name(P2C_Allocator *a, const char *name, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(a, AST_NAME, line, col);
    if (!e) return NULL;
    e->base.u.name.name = dup_str(a, name);
    return e;
}

P2C_AstExpr* p2c_ast_const_int(P2C_Allocator *a, const char *value, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(a, AST_CONST, line, col);
    if (!e) return NULL;
    e->base.u.constant.token_type = TOK_INT_LITERAL;
    e->base.u.constant.value = dup_str(a, value);
    return e;
}

P2C_AstExpr* p2c_ast_const_float(P2C_Allocator *a, const char *value, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(a, AST_CONST, line, col);
    if (!e) return NULL;
    e->base.u.constant.token_type = TOK_FLOAT_LITERAL;
    e->base.u.constant.value = dup_str(a, value);
    return e;
}

P2C_AstExpr* p2c_ast_const_str(P2C_Allocator *a, const char *value, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(a, AST_CONST, line, col);
    if (!e) return NULL;
    e->base.u.constant.token_type = TOK_STR_LITERAL;
    e->base.u.constant.value = dup_str(a, value);
    return e;
}

P2C_AstExpr* p2c_ast_const_bool(P2C_Allocator *a, bool value, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(a, AST_CONST, line, col);
    if (!e) return NULL;
    e->base.u.constant.token_type = TOK_BOOL_LITERAL;
    e->base.u.constant.value = dup_str(a, value ? "True" : "False");
    return e;
}

P2C_AstExpr* p2c_ast_const_none(P2C_Allocator *a, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(a, AST_CONST, line, col);
    if (!e) return NULL;
    e->base.u.constant.token_type = TOK_NONE_LITERAL;
    e->base.u.constant.value = dup_str(a, "None");
    return e;
}

const char* p2c_ast_op_name(P2C_AstOperator op) {
    switch (op) {
        case OP_ADD: return "+";
        case OP_SUB: return "-";
        case OP_MULT: return "*";
        case OP_DIV: return "/";
        case OP_FLOORDIV: return "//";
        case OP_MOD: return "%";
        case OP_POW: return "**";
        case OP_LSHIFT: return "<<";
        case OP_RSHIFT: return ">>";
        case OP_BITAND: return "&";
        case OP_BITOR: return "|";
        case OP_BITXOR: return "^";
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_IS: return "is";
        case OP_ISNOT: return "is not";
        case OP_IN: return "in";
        case OP_NOTIN: return "not in";
        case OP_NOT: return "not";
        case OP_UADD: return "+";
        case OP_USUB: return "-";
        case OP_INVERT: return "~";
        case OP_AND: return "and";
        case OP_OR: return "or";
        default: return "?";
    }
}
