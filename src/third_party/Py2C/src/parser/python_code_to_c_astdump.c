/*
 * python_code_to_c_astdump.c - ASTを読みやすい木構造テキストとして出力する。
 * --dump-ast オプションから使われる、デバッグ・可視化専用のモジュール。
 * コード生成のロジックには一切影響を与えない。
 */
#include "parser/python_code_to_c_astdump.h"
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdio.h>
#include <string.h>
#endif

static void ind(P2C_String *out, int indent) {
    for (int i = 0; i < indent; i++) p2c_str_append(out, "  ");
}

static const char* op_name(P2C_AstOperator op) {
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

static const char* type_name(P2C_AstType t) {
    switch (t) {
        case AST_BINOP: return "BinOp";
        case AST_UNARYOP: return "UnaryOp";
        case AST_COMPARE: return "Compare";
        case AST_BOOLOP: return "BoolOp";
        case AST_CALL: return "Call";
        case AST_ATTRIBUTE: return "Attribute";
        case AST_SUBSCRIPT: return "Subscript";
        case AST_NAME: return "Name";
        case AST_CONST: return "Const";
        case AST_IFEXP: return "IfExp";
        case AST_LAMBDA: return "Lambda";
        case AST_LIST: return "List";
        case AST_TUPLE: return "Tuple";
        case AST_DICT: return "Dict";
        case AST_COMPREHENSION: return "Comprehension";
        case AST_STARRED: return "Starred";
        case AST_ASSIGN: return "Assign";
        case AST_AUGASSIGN: return "AugAssign";
        case AST_ANNASSIGN: return "AnnAssign";
        case AST_RETURN: return "Return";
        case AST_EXPR_STMT: return "ExprStmt";
        case AST_PASS: return "Pass";
        case AST_BREAK: return "Break";
        case AST_CONTINUE: return "Continue";
        case AST_IF: return "If";
        case AST_WHILE: return "While";
        case AST_FOR: return "For";
        case AST_ASYNC_FOR: return "AsyncFor";
        case AST_TRY: return "Try";
        case AST_RAISE: return "Raise";
        case AST_ASSERT: return "Assert";
        case AST_FUNCTIONDEF: return "FunctionDef";
        case AST_CLASSDEF: return "ClassDef";
        case AST_GLOBAL: return "Global";
        case AST_NONLOCAL: return "Nonlocal";
        case AST_MODULE: return "Module";
        case AST_IMPORT: return "Import";
        case AST_IMPORTFROM: return "ImportFrom";
        case AST_WITH: return "With";
        case AST_DELETE: return "Delete";
        default: return "?Unknown";
    }
}

static void dump_stmt_list(P2C_Vector *v, P2C_String *out, int indent, const char *label) {
    ind(out, indent); p2c_str_append(out, label); p2c_str_append(out, ":\n");
    if (!v || p2c_vec_len(v) == 0) { ind(out, indent + 1); p2c_str_append(out, "(empty)\n"); return; }
    for (size_t i = 0; i < p2c_vec_len(v); i++) {
        p2c_ast_dump_stmt((P2C_AstStmt*)p2c_vec_get(v, i), out, indent + 1);
    }
}

static void dump_expr_list(P2C_Vector *v, P2C_String *out, int indent, const char *label) {
    ind(out, indent); p2c_str_append(out, label); p2c_str_append(out, ":\n");
    if (!v || p2c_vec_len(v) == 0) { ind(out, indent + 1); p2c_str_append(out, "(empty)\n"); return; }
    for (size_t i = 0; i < p2c_vec_len(v); i++) {
        p2c_ast_dump_expr((P2C_AstExpr*)p2c_vec_get(v, i), out, indent + 1);
    }
}

static void dump_header(P2C_AstNode *n, P2C_String *out, int indent) {
    ind(out, indent);
    p2c_str_append(out, type_name(n->type));
    char loc[32];
    snprintf(loc, sizeof(loc), " (line %u)", n->line);
    p2c_str_append(out, loc);
    p2c_str_append(out, "\n");
}

void p2c_ast_dump_expr(P2C_AstExpr *expr, P2C_String *out, int indent) {
    if (!expr) { ind(out, indent); p2c_str_append(out, "None\n"); return; }
    P2C_AstNode *n = &expr->base;
    dump_header(n, out, indent);
    switch (n->type) {
        case AST_BINOP:
            ind(out, indent + 1); p2c_str_append(out, "op: "); p2c_str_append(out, op_name(n->u.binop.op)); p2c_str_append(out, "\n");
            ind(out, indent + 1); p2c_str_append(out, "left:\n"); p2c_ast_dump_expr(n->u.binop.left, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "right:\n"); p2c_ast_dump_expr(n->u.binop.right, out, indent + 2);
            break;
        case AST_UNARYOP:
            ind(out, indent + 1); p2c_str_append(out, "op: "); p2c_str_append(out, op_name(n->u.unaryop.op)); p2c_str_append(out, "\n");
            ind(out, indent + 1); p2c_str_append(out, "operand:\n"); p2c_ast_dump_expr(n->u.unaryop.operand, out, indent + 2);
            break;
        case AST_COMPARE:
            ind(out, indent + 1); p2c_str_append(out, "left:\n"); p2c_ast_dump_expr(n->u.compare.left, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "ops: [");
            for (size_t i = 0; i < p2c_vec_len(n->u.compare.ops); i++) {
                if (i) p2c_str_append(out, ", ");
                P2C_AstOperator *opp = (P2C_AstOperator*)p2c_vec_get(n->u.compare.ops, i);
                p2c_str_append(out, op_name(*opp));
            }
            p2c_str_append(out, "]\n");
            dump_expr_list(n->u.compare.comparators, out, indent + 1, "comparators");
            break;
        case AST_BOOLOP:
            ind(out, indent + 1); p2c_str_append(out, "op: "); p2c_str_append(out, op_name(n->u.boolop.op)); p2c_str_append(out, "\n");
            dump_expr_list(n->u.boolop.values, out, indent + 1, "values");
            break;
        case AST_CALL:
            ind(out, indent + 1); p2c_str_append(out, "func:\n"); p2c_ast_dump_expr(n->u.call.func, out, indent + 2);
            dump_expr_list(n->u.call.args, out, indent + 1, "args");
            break;
        case AST_ATTRIBUTE:
            ind(out, indent + 1); p2c_str_append(out, "attr: "); p2c_str_append(out, n->u.attribute.attr ? n->u.attribute.attr : "?"); p2c_str_append(out, "\n");
            ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.attribute.value, out, indent + 2);
            break;
        case AST_SUBSCRIPT:
            ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.subscript.value, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "slice:\n"); p2c_ast_dump_expr(n->u.subscript.slice, out, indent + 2);
            break;
        case AST_NAME:
            ind(out, indent + 1); p2c_str_append(out, "name: "); p2c_str_append(out, n->u.name.name ? n->u.name.name : "?"); p2c_str_append(out, "\n");
            break;
        case AST_CONST:
            ind(out, indent + 1); p2c_str_append(out, "value: "); p2c_str_append(out, n->u.constant.value ? n->u.constant.value : "None"); p2c_str_append(out, "\n");
            break;
        case AST_IFEXP:
            ind(out, indent + 1); p2c_str_append(out, "test:\n"); p2c_ast_dump_expr(n->u.ifexp.test, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "body:\n"); p2c_ast_dump_expr(n->u.ifexp.body, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "orelse:\n"); p2c_ast_dump_expr(n->u.ifexp.orelse, out, indent + 2);
            break;
        case AST_LAMBDA:
            ind(out, indent + 1); p2c_str_append(out, "body:\n"); p2c_ast_dump_expr(n->u.lambda.body, out, indent + 2);
            break;
        case AST_LIST:
        case AST_TUPLE:
            dump_expr_list(n->u.list.elts, out, indent + 1, "elts");
            break;
        case AST_DICT:
            dump_expr_list(n->u.dict.keys, out, indent + 1, "keys");
            dump_expr_list(n->u.dict.values, out, indent + 1, "values");
            break;
        case AST_STARRED:
            ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.starred.value, out, indent + 2);
            break;
        default:
            break;
    }
}

void p2c_ast_dump_stmt(P2C_AstStmt *stmt, P2C_String *out, int indent) {
    if (!stmt) { ind(out, indent); p2c_str_append(out, "None\n"); return; }
    P2C_AstNode *n = &stmt->base;
    dump_header(n, out, indent);
    switch (n->type) {
        case AST_ASSIGN:
            dump_expr_list(n->u.assign.targets, out, indent + 1, "targets");
            ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.assign.value, out, indent + 2);
            break;
        case AST_AUGASSIGN:
            ind(out, indent + 1); p2c_str_append(out, "op: "); p2c_str_append(out, op_name(n->u.augassign.op)); p2c_str_append(out, "\n");
            ind(out, indent + 1); p2c_str_append(out, "target:\n"); p2c_ast_dump_expr(n->u.augassign.target, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.augassign.value, out, indent + 2);
            break;
        case AST_RETURN:
            if (n->u.return_stmt.value) { ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.return_stmt.value, out, indent + 2); }
            break;
        case AST_EXPR_STMT:
            ind(out, indent + 1); p2c_str_append(out, "value:\n"); p2c_ast_dump_expr(n->u.expr_stmt.value, out, indent + 2);
            break;
        case AST_IF:
            ind(out, indent + 1); p2c_str_append(out, "test:\n"); p2c_ast_dump_expr(n->u.if_stmt.test, out, indent + 2);
            dump_stmt_list(n->u.if_stmt.body, out, indent + 1, "body");
            dump_stmt_list(n->u.if_stmt.orelse, out, indent + 1, "orelse");
            break;
        case AST_WHILE:
            ind(out, indent + 1); p2c_str_append(out, "test:\n"); p2c_ast_dump_expr(n->u.while_stmt.test, out, indent + 2);
            dump_stmt_list(n->u.while_stmt.body, out, indent + 1, "body");
            break;
        case AST_FOR:
        case AST_ASYNC_FOR:
            ind(out, indent + 1); p2c_str_append(out, "target:\n"); p2c_ast_dump_expr(n->u.for_stmt.target, out, indent + 2);
            ind(out, indent + 1); p2c_str_append(out, "iter:\n"); p2c_ast_dump_expr(n->u.for_stmt.iter, out, indent + 2);
            dump_stmt_list(n->u.for_stmt.body, out, indent + 1, "body");
            break;
        case AST_TRY:
            dump_stmt_list(n->u.try_stmt.body, out, indent + 1, "body");
            ind(out, indent + 1); p2c_str_append(out, "handlers:\n");
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
                ind(out, indent + 2); p2c_str_append(out, "ExceptHandler");
                if (h->name) { p2c_str_append(out, " as "); p2c_str_append(out, h->name); }
                p2c_str_append(out, ":\n");
                if (h->type) { ind(out, indent + 3); p2c_str_append(out, "type:\n"); p2c_ast_dump_expr(h->type, out, indent + 4); }
                dump_stmt_list(h->body, out, indent + 3, "body");
            }
            dump_stmt_list(n->u.try_stmt.finalbody, out, indent + 1, "finalbody");
            break;
        case AST_RAISE:
            if (n->u.raise.exc) { ind(out, indent + 1); p2c_str_append(out, "exc:\n"); p2c_ast_dump_expr(n->u.raise.exc, out, indent + 2); }
            break;
        case AST_ASSERT:
            ind(out, indent + 1); p2c_str_append(out, "test:\n"); p2c_ast_dump_expr(n->u.assert_stmt.test, out, indent + 2);
            break;
        case AST_FUNCTIONDEF:
            ind(out, indent + 1); p2c_str_append(out, "name: "); p2c_str_append(out, n->u.functiondef.name ? n->u.functiondef.name : "?"); p2c_str_append(out, "\n");
            ind(out, indent + 1); p2c_str_append(out, "args: [");
            for (size_t i = 0; i < p2c_vec_len(n->u.functiondef.args); i++) {
                if (i) p2c_str_append(out, ", ");
                P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(n->u.functiondef.args, i);
                p2c_str_append(out, a->name ? a->name : "?");
            }
            p2c_str_append(out, "]\n");
            if ((size_t)n->u.functiondef.kwonly_start < p2c_vec_len(n->u.functiondef.args)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", n->u.functiondef.kwonly_start);
                ind(out, indent + 1); p2c_str_append(out, "kwonly_start: "); p2c_str_append(out, buf); p2c_str_append(out, "\n");
            }
            if (n->u.functiondef.vararg) {
                ind(out, indent + 1); p2c_str_append(out, "vararg: *"); p2c_str_append(out, n->u.functiondef.vararg); p2c_str_append(out, "\n");
            }
            if (n->u.functiondef.kwarg) {
                ind(out, indent + 1); p2c_str_append(out, "kwarg: **"); p2c_str_append(out, n->u.functiondef.kwarg); p2c_str_append(out, "\n");
            }
            dump_expr_list(n->u.functiondef.decorator_list, out, indent + 1, "decorator_list");
            dump_stmt_list(n->u.functiondef.body, out, indent + 1, "body");
            break;
        case AST_CLASSDEF:
            ind(out, indent + 1); p2c_str_append(out, "name: "); p2c_str_append(out, n->u.classdef.name ? n->u.classdef.name : "?"); p2c_str_append(out, "\n");
            dump_expr_list(n->u.classdef.decorator_list, out, indent + 1, "decorator_list");
            dump_stmt_list(n->u.classdef.body, out, indent + 1, "body");
            break;
        case AST_IMPORT:
            ind(out, indent + 1); p2c_str_append(out, "names: [");
            for (size_t i = 0; i < p2c_vec_len(n->u.import_stmt.names); i++) {
                if (i) p2c_str_append(out, ", ");
                P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.import_stmt.names, i);
                p2c_str_append(out, al->name ? al->name : "?");
            }
            p2c_str_append(out, "]\n");
            break;
        case AST_IMPORTFROM:
            ind(out, indent + 1); p2c_str_append(out, "module: "); p2c_str_append(out, n->u.importfrom.module ? n->u.importfrom.module : "?"); p2c_str_append(out, "\n");
            break;
        case AST_WITH:
            dump_stmt_list(n->u.with.body, out, indent + 1, "body");
            break;
        case AST_DELETE:
            dump_expr_list(n->u.delete.targets, out, indent + 1, "targets");
            break;
        case AST_PASS:
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_GLOBAL:
        case AST_NONLOCAL:
        default:
            break;
    }
}

void p2c_ast_dump_module(P2C_AstModule *module, P2C_String *out) {
    p2c_str_append(out, "Module\n");
    if (!module) { p2c_str_append(out, "  (empty)\n"); return; }
    /* P2C_AstModule はP2C_AstNodeのunion内(u.module)として実体化されており、
     * 単独のフラットな構造体としては配置されていない。そのため他の箇所
     * (python_code_to_c_semantic.c等)と同様に、P2C_AstNode*として再解釈してから
     * u.module.body を参照する必要がある。 */
    P2C_AstNode *n = (P2C_AstNode*)module;
    P2C_Vector *body = n->u.module.body;
    if (!body || p2c_vec_len(body) == 0) {
        p2c_str_append(out, "  (empty)\n");
        return;
    }
    for (size_t i = 0; i < p2c_vec_len(body); i++) {
        p2c_ast_dump_stmt((P2C_AstStmt*)p2c_vec_get(body, i), out, 1);
    }
}
