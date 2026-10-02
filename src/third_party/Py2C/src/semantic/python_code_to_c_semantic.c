#include "semantic/python_code_to_c_semantic.h"
#include "common/python_code_to_c_common.h"
#include <stddef.h>
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdio.h>
#endif

/* ========================================
 * 型システム
 * ======================================== */

/* -Wwrite-strings のもとでは文字列リテラルを char* へ代入するとconst破棄の
 * 指摘になる。組み込み型名は書き換え可能な静的配列として保持する
 * （型定義側の所有権は「読み出し専用の名前」であり、解放対象ではない）。 */
#define P2C_BUILTIN_TYPE_NAME(sym, text) static char sym[] = text

P2C_BUILTIN_TYPE_NAME(name_unknown, "unknown");
P2C_BUILTIN_TYPE_NAME(name_none, "NoneType");
P2C_BUILTIN_TYPE_NAME(name_bool, "bool");
P2C_BUILTIN_TYPE_NAME(name_int, "int");
P2C_BUILTIN_TYPE_NAME(name_float, "float");
P2C_BUILTIN_TYPE_NAME(name_str, "str");
P2C_BUILTIN_TYPE_NAME(name_list, "list");
P2C_BUILTIN_TYPE_NAME(name_dict, "dict");
P2C_BUILTIN_TYPE_NAME(name_tuple, "tuple");
P2C_BUILTIN_TYPE_NAME(name_function, "function");
P2C_BUILTIN_TYPE_NAME(name_class, "class");
P2C_BUILTIN_TYPE_NAME(name_object, "object");
P2C_BUILTIN_TYPE_NAME(name_module, "module");
P2C_BUILTIN_TYPE_NAME(name_any, "Any");

static P2C_Type builtin_types[] = {
    {TYPE_UNKNOWN, name_unknown, NULL, NULL, NULL},
    {TYPE_NONE, name_none, NULL, NULL, NULL},
    {TYPE_BOOL, name_bool, NULL, NULL, NULL},
    {TYPE_INT, name_int, NULL, NULL, NULL},
    {TYPE_FLOAT, name_float, NULL, NULL, NULL},
    {TYPE_STR, name_str, NULL, NULL, NULL},
    {TYPE_LIST, name_list, NULL, NULL, NULL},
    {TYPE_DICT, name_dict, NULL, NULL, NULL},
    {TYPE_TUPLE, name_tuple, NULL, NULL, NULL},
    {TYPE_FUNCTION, name_function, NULL, NULL, NULL},
    {TYPE_CLASS, name_class, NULL, NULL, NULL},
    {TYPE_OBJECT, name_object, NULL, NULL, NULL},
    {TYPE_MODULE, name_module, NULL, NULL, NULL},
    {TYPE_ANY, name_any, NULL, NULL, NULL},
};

P2C_Type* p2c_type_builtin(P2C_TypeKind kind) {
    if (kind >= TYPE_UNKNOWN && kind <= TYPE_ANY) return &builtin_types[kind];
    return &builtin_types[TYPE_UNKNOWN];
}

P2C_Type* p2c_type_new(P2C_Allocator *a, P2C_TypeKind kind, const char *name) {
    P2C_Type *t = p2c_alloc(a, sizeof(P2C_Type));
    if (!t) return NULL;
    t->kind = kind;
    if (name) {
        size_t len = strlen(name);
        t->name = p2c_alloc(a, len + 1);
        if (t->name) memcpy(t->name, name, len + 1);
    } else {
        t->name = NULL;
    }
    t->elem_type = NULL;
    t->arg_types = NULL;
    t->ret_type = NULL;
    return t;
}

void p2c_type_free(P2C_Type *t, P2C_Allocator *a) {
    if (!t) return;
    if (t->name) p2c_free(a, t->name);
    if (t->arg_types) p2c_vec_free(t->arg_types);
    p2c_free(a, t);
}

/* ========================================
 * シンボルテーブル
 * ======================================== */

P2C_SymbolTable* p2c_symtab_new(P2C_Allocator *a) {
    P2C_SymbolTable *tab = p2c_alloc(a, sizeof(P2C_SymbolTable));
    if (!tab) return NULL;
    tab->alloc = a;
    
    /* グローバルスコープ作成 */
    P2C_SymbolScope *global = p2c_alloc(a, sizeof(P2C_SymbolScope));
    if (!global) { p2c_free(a, tab); return NULL; }
    global->parent = NULL;
    global->scope_type = SCOPE_MODULE;
    global->symbols = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    if (!global->symbols) { p2c_free(a, global); p2c_free(a, tab); return NULL; }
    
    tab->global = global;
    tab->current = global;
    
    /* 組み込みシンボルを登録 */
    const char *builtins[] = {"int", "float", "str", "list", "dict", "set", "frozenset", "tuple", 
                              "bool", "True", "False", "None", "Ellipsis", "print", "len",
                              "range", "Exception", "TypeError", "ValueError", "MemoryError",
                              "RuntimeError", "AttributeError", "IndexError",
                              "KeyError", "ZeroDivisionError", "AssertionError",
                              "StopIteration", "input", "open", "close", "read",
                              "write", "append", "ord", "chr", "hex", "oct", "bin",
                              "abs", "min", "max", "sum", "sorted", "reversed",
                              "enumerate", "zip", "map", "filter", "any", "all",
                              "round", "pow", "divmod", "isinstance", "hasattr",
                              "getattr", "setattr", "delattr", "dir", "repr",
                              "format", "iter", "next", "slice", "super",
                              "staticmethod", "classmethod", "property",
                              "type", "id", "hash", "callable",
                              "p2c_format_fixed", "p2c_fstr_fmt", "__name__"};
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        P2C_Symbol *sym = p2c_alloc(a, sizeof(P2C_Symbol));
        sym->name = p2c_alloc(a, strlen(builtins[i]) + 1);
        if (sym->name) strcpy(sym->name, builtins[i]);
        sym->kind = SYM_BUILTIN;
        sym->type = p2c_type_builtin(TYPE_ANY);
        sym->def_node = NULL;
        sym->is_initialized = true;
        p2c_map_insert(global->symbols, sym->name, sym);
    }
    
    return tab;
}

static void p2c_symtab_free_scope(P2C_SymbolScope *scope, P2C_Allocator *alloc) {
    if (!scope) return;
    if (scope->symbols) {
        for (size_t i = 0; i < scope->symbols->bucket_count; i++) {
            for (P2C_MapEntry *entry = scope->symbols->buckets[i]; entry; entry = entry->next) {
                P2C_Symbol *sym = (P2C_Symbol*)entry->val;
                if (sym) {
                    if (sym->name) p2c_free(alloc, sym->name);
                    p2c_free(alloc, sym);
                }
            }
        }
        p2c_map_free(scope->symbols);
    }
    p2c_free(alloc, scope);
}

void p2c_symtab_free(P2C_SymbolTable *tab) {
    if (!tab) return;
    while (tab->current && tab->current != tab->global) {
        P2C_SymbolScope *scope = tab->current;
        tab->current = scope->parent;
        p2c_symtab_free_scope(scope, tab->alloc);
    }
    p2c_symtab_free_scope(tab->global, tab->alloc);
    p2c_free(tab->alloc, tab);
}

void p2c_symtab_push_scope(P2C_SymbolTable *tab, P2C_SymbolScopeType type) {
    if (!tab) return;
    P2C_SymbolScope *scope = p2c_alloc(tab->alloc, sizeof(P2C_SymbolScope));
    if (!scope) return;
    scope->parent = tab->current;
    scope->scope_type = type;
    scope->symbols = p2c_map_new(tab->alloc, p2c_hash_str, p2c_eq_str);
    tab->current = scope;
}

void p2c_symtab_pop_scope(P2C_SymbolTable *tab) {
    if (!tab || !tab->current || tab->current == tab->global) return;
    P2C_SymbolScope *old = tab->current;
    tab->current = old->parent;
    p2c_symtab_free_scope(old, tab->alloc);
}

P2C_SymbolScope* p2c_symtab_current_scope(P2C_SymbolTable *tab) {
    return tab ? tab->current : NULL;
}

P2C_Symbol* p2c_symtab_lookup(P2C_SymbolTable *tab, const char *name) {
    if (!tab || !name) return NULL;
    P2C_SymbolScope *scope = tab->current;
    while (scope) {
        P2C_Symbol *sym = (P2C_Symbol*)p2c_map_get(scope->symbols, name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return NULL;
}

P2C_Symbol* p2c_symtab_lookup_local(P2C_SymbolTable *tab, const char *name) {
    if (!tab || !tab->current || !name) return NULL;
    return (P2C_Symbol*)p2c_map_get(tab->current->symbols, name);
}

P2C_Result p2c_symtab_define(P2C_SymbolTable *tab, const char *name, P2C_SymbolKind kind, P2C_Type *type) {
    if (!tab || !tab->current || !name) return P2C_ERR_INTERNAL;
    P2C_Symbol *sym = p2c_alloc(tab->alloc, sizeof(P2C_Symbol));
    if (!sym) return P2C_ERR_NOMEM;
    sym->name = p2c_alloc(tab->alloc, strlen(name) + 1);
    if (sym->name) strcpy(sym->name, name);
    sym->kind = kind;
    sym->type = type;
    sym->def_node = NULL;
    sym->is_initialized = false;
    sym->next = NULL;
    p2c_map_insert(tab->current->symbols, sym->name, sym);
    return P2C_OK;
}

/* ========================================
 * セマンティックアナライザー
 * ======================================== */

static void set_sem_error(P2C_Semantic *sem, const char *msg, uint32_t line, uint32_t col) {
    if (sem->error_msg) p2c_free(sem->alloc, sem->error_msg);
    sem->error_msg = p2c_alloc(sem->alloc, strlen(msg) + 1);
    if (sem->error_msg) strcpy(sem->error_msg, msg);
    sem->error_line = line;
    sem->error_col = col;
    sem->last_error = P2C_ERR_SEMANTIC;
}

P2C_Semantic* p2c_semantic_new(P2C_Allocator *a) {
    P2C_Semantic *sem = p2c_alloc(a, sizeof(P2C_Semantic));
    if (!sem) return NULL;
    sem->alloc = a;
    sem->symtab = p2c_symtab_new(a);
    sem->last_error = P2C_OK;
    sem->error_msg = NULL;
    sem->error_line = 0;
    sem->error_col = 0;
    return sem;
}

void p2c_semantic_free(P2C_Semantic *sem) {
    if (!sem) return;
    p2c_symtab_free(sem->symtab);
    if (sem->error_msg) p2c_free(sem->alloc, sem->error_msg);
    p2c_free(sem->alloc, sem);
}

const char* p2c_semantic_error_msg(P2C_Semantic *sem) {
    return sem ? sem->error_msg : NULL;
}

/* 式の型を推論 */
static P2C_Result visit_expr(P2C_Semantic *sem, P2C_AstExpr *expr, P2C_Type **out_type);

static P2C_Result visit_expr_list(P2C_Semantic *sem, P2C_Vector *exprs) {
    if (!exprs) return P2C_OK;
    for (size_t i = 0; i < p2c_vec_len(exprs); i++) {
        P2C_Type *t = NULL;
        P2C_Result r = visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(exprs, i), &t);
        if (r != P2C_OK) return r;
    }
    return P2C_OK;
}

static P2C_Result visit_expr(P2C_Semantic *sem, P2C_AstExpr *expr, P2C_Type **out_type) {
    if (!expr) {
        if (out_type) *out_type = p2c_type_builtin(TYPE_NONE);
        return P2C_OK;
    }
    
    P2C_AstNode *n = &expr->base;
    P2C_Type *t = p2c_type_builtin(TYPE_ANY);
    
    switch (n->type) {
        case AST_NAME: {
            /* --fallback が作る未対応構文のスタブ名は、変数ではないため
             * 名前解決の対象外（codegenがランタイム呼び出しへ変換する）。 */
            if (n->u.name.name &&
                strncmp(n->u.name.name, P2C_UNSUPPORTED_NAME_PREFIX,
                        strlen(P2C_UNSUPPORTED_NAME_PREFIX)) == 0) {
                break;
            }
            /* パーサが a[b:c] を展開する際に作る p2c_obj_slice() も変数では
             * ないため名前解決の対象外にする（以前は代入の右辺にスライスを
             * 書くと 'undefined name' で失敗していた）。 */
            if (n->u.name.name && strcmp(n->u.name.name, "p2c_obj_slice") == 0) {
                break;
            }
            P2C_Symbol *sym = p2c_symtab_lookup(sem->symtab, n->u.name.name);
            if (!sym) {
                char buf[256];
                snprintf(buf, sizeof(buf), "undefined name '%s'", n->u.name.name);
                set_sem_error(sem, buf, n->line, n->col);
                return P2C_ERR_SEMANTIC;
            }
            t = sym->type ? sym->type : p2c_type_builtin(TYPE_ANY);
            break;
        }
        case AST_CONST:
            switch (n->u.constant.token_type) {
                case TOK_INT_LITERAL: t = p2c_type_builtin(TYPE_INT); break;
                case TOK_FLOAT_LITERAL: t = p2c_type_builtin(TYPE_FLOAT); break;
                case TOK_STR_LITERAL: t = p2c_type_builtin(TYPE_STR); break;
                case TOK_BOOL_LITERAL: t = p2c_type_builtin(TYPE_BOOL); break;
                case TOK_NONE_LITERAL: t = p2c_type_builtin(TYPE_NONE); break;
                default: break;
            }
            break;
        case AST_BINOP: {
            P2C_Type *lt = NULL, *rt = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.binop.left, &lt);
            if (r != P2C_OK) return r;
            r = visit_expr(sem, (P2C_AstExpr*)n->u.binop.right, &rt);
            if (r != P2C_OK) return r;
            /* 結果型の決定（簡易版） */
            if (n->u.binop.op == OP_LT || n->u.binop.op == OP_LE || 
                n->u.binop.op == OP_GT || n->u.binop.op == OP_GE ||
                n->u.binop.op == OP_EQ || n->u.binop.op == OP_NE) {
                t = p2c_type_builtin(TYPE_BOOL);
            } else if (lt && lt->kind == TYPE_FLOAT && rt && rt->kind == TYPE_FLOAT) {
                t = p2c_type_builtin(TYPE_FLOAT);
            } else {
                t = p2c_type_builtin(TYPE_INT);
            }
            break;
        }
        case AST_UNARYOP: {
            P2C_Type *ot = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.unaryop.operand, &ot);
            if (r != P2C_OK) return r;
            if (n->u.unaryop.op == OP_NOT) t = p2c_type_builtin(TYPE_BOOL);
            else t = ot;
            break;
        }
        case AST_COMPARE: {
            P2C_Type *lt = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.compare.left, &lt);
            if (r != P2C_OK) return r;
            r = visit_expr_list(sem, n->u.compare.comparators);
            if (r != P2C_OK) return r;
            t = p2c_type_builtin(TYPE_BOOL);
            break;
        }
        case AST_BOOLOP: {
            P2C_Result r = visit_expr_list(sem, n->u.boolop.values);
            if (r != P2C_OK) return r;
            t = p2c_type_builtin(TYPE_BOOL);
            break;
        }
        case AST_CALL: {
            P2C_Type *ft = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.call.func, &ft);
            if (r != P2C_OK) return r;
            r = visit_expr_list(sem, n->u.call.args);
            if (r != P2C_OK) return r;
            t = ft && ft->ret_type ? ft->ret_type : p2c_type_builtin(TYPE_ANY);
            break;
        }
        case AST_ATTRIBUTE: {
            P2C_Type *vt = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.attribute.value, &vt);
            if (r != P2C_OK) return r;
            t = p2c_type_builtin(TYPE_ANY);
            break;
        }
        case AST_SUBSCRIPT: {
            P2C_Type *vt = NULL, *st = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.subscript.value, &vt);
            if (r != P2C_OK) return r;
            r = visit_expr(sem, (P2C_AstExpr*)n->u.subscript.slice, &st);
            if (r != P2C_OK) return r;
            t = vt && vt->elem_type ? vt->elem_type : p2c_type_builtin(TYPE_ANY);
            break;
        }
        case AST_IFEXP: {
            P2C_Type *tt = NULL, *bt = NULL, *et = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.ifexp.test, &tt);
            if (r != P2C_OK) return r;
            r = visit_expr(sem, (P2C_AstExpr*)n->u.ifexp.body, &bt);
            if (r != P2C_OK) return r;
            r = visit_expr(sem, (P2C_AstExpr*)n->u.ifexp.orelse, &et);
            if (r != P2C_OK) return r;
            t = bt;
            break;
        }
        case AST_LIST:
            visit_expr_list(sem, n->u.list.elts);
            t = p2c_type_builtin(TYPE_LIST);
            break;
        case AST_TUPLE:
            visit_expr_list(sem, n->u.tuple.elts);
            t = p2c_type_builtin(TYPE_TUPLE);
            break;
        case AST_DICT:
            visit_expr_list(sem, n->u.dict.keys);
            visit_expr_list(sem, n->u.dict.values);
            t = p2c_type_builtin(TYPE_DICT);
            break;
        case AST_LAMBDA:
            t = p2c_type_builtin(TYPE_FUNCTION);
            break;
        default:
            break;
    }
    
    if (out_type) *out_type = t;
    return P2C_OK;
}

static bool is_slice_call(P2C_AstExpr *expr) {
    P2C_AstNode *node;
    P2C_AstExpr *func;
    if (!expr || expr->base.type != AST_CALL) return false;
    node = &expr->base;
    func = node->u.call.func;
    return func && func->base.type == AST_NAME &&
           strcmp(func->base.u.name.name, "p2c_obj_slice") == 0;
}

/* 文の分析 */
static bool contains_finally_control_flow(P2C_Vector *stmts) {
    if (!stmts) return false;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt) continue;
        P2C_AstNode *n = &stmt->base;
        if (n->type == AST_RETURN || n->type == AST_BREAK || n->type == AST_CONTINUE) return true;
        if (n->type == AST_IF && (contains_finally_control_flow(n->u.if_stmt.body) || contains_finally_control_flow(n->u.if_stmt.orelse))) return true;
        if (n->type == AST_WHILE && (contains_finally_control_flow(n->u.while_stmt.body) || contains_finally_control_flow(n->u.while_stmt.orelse))) return true;
        if (n->type == AST_FOR && (contains_finally_control_flow(n->u.for_stmt.body) || contains_finally_control_flow(n->u.for_stmt.orelse))) return true;
        if (n->type == AST_TRY && (contains_finally_control_flow(n->u.try_stmt.body) || contains_finally_control_flow(n->u.try_stmt.orelse) || contains_finally_control_flow(n->u.try_stmt.finalbody))) return true;
        if (n->type == AST_WITH && contains_finally_control_flow(n->u.with.body)) return true;
        if (n->type == AST_BLOCK && contains_finally_control_flow(n->u.block.stmts)) return true;
    }
    return false;
}

/* for文・with文・代入のターゲットに現れる単純名を登録する。
 * タプル/リスト/starredを再帰的に辿る（属性・添字は代入先なので登録しない）。 */
static bool bind_with_target(P2C_Semantic *sem, P2C_AstExpr *target) {
    if (!target) return true;
    if (target->base.type == AST_NAME) {
        if (!p2c_symtab_lookup_local(sem->symtab, target->base.u.name.name)) {
            p2c_symtab_define(sem->symtab, target->base.u.name.name, SYM_VARIABLE, p2c_type_builtin(TYPE_ANY));
        }
        return true;
    }
    if (target->base.type == AST_STARRED) return bind_with_target(sem, target->base.u.starred.value);
    if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.type == AST_TUPLE ? target->base.u.tuple.elts : target->base.u.list.elts;
        for (size_t i = 0; i < p2c_vec_len(elts); i++) {
            if (!bind_with_target(sem, (P2C_AstExpr*)p2c_vec_get(elts, i))) return false;
        }
        return true;
    }
    return false;
}

static P2C_Result visit_stmt(P2C_Semantic *sem, P2C_AstStmt *stmt) {
    if (!stmt) return P2C_OK;
    P2C_AstNode *n = &stmt->base;
    
    switch (n->type) {
        case AST_ASSIGN: {
            P2C_Type *vt = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.assign.value, &vt);
            if (r != P2C_OK) return r;
            /* ターゲットを変数として登録 */
            for (size_t i = 0; i < p2c_vec_len(n->u.assign.targets); i++) {
                P2C_AstExpr *t = (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, i);
                if (is_slice_call(t)) {
                    if (t->base.u.call.args && p2c_vec_len(t->base.u.call.args) == 4) {
                        visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(t->base.u.call.args, 0), NULL);
                        visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(t->base.u.call.args, 1), NULL);
                        visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(t->base.u.call.args, 2), NULL);
                        visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(t->base.u.call.args, 3), NULL);
                    }
                    continue;
                }
                if (t && t->base.type == AST_NAME) {
                    P2C_Symbol *existing = p2c_symtab_lookup_local(sem->symtab, t->base.u.name.name);
                    if (!existing) {
                        p2c_symtab_define(sem->symtab, t->base.u.name.name, SYM_VARIABLE, vt);
                    }
                }
            }
            break;
        }
        case AST_AUGASSIGN: {
            P2C_Type *vt = NULL;
            visit_expr(sem, (P2C_AstExpr*)n->u.augassign.target, &vt);
            visit_expr(sem, (P2C_AstExpr*)n->u.augassign.value, &vt);
            break;
        }
        case AST_RETURN: {
            P2C_Type *rt = NULL;
            visit_expr(sem, (P2C_AstExpr*)n->u.return_stmt.value, &rt);
            break;
        }
        case AST_EXPR_STMT:
            visit_expr(sem, (P2C_AstExpr*)n->u.expr_stmt.value, NULL);
            break;
        case AST_IF: {
            P2C_Type *tt = NULL;
            P2C_Result r = visit_expr(sem, (P2C_AstExpr*)n->u.if_stmt.test, &tt);
            if (r != P2C_OK) return r;
            for (size_t i = 0; i < p2c_vec_len(n->u.if_stmt.body); i++) {
                r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.if_stmt.body, i));
                if (r != P2C_OK) return r;
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.if_stmt.orelse); i++) {
                r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.if_stmt.orelse, i));
                if (r != P2C_OK) return r;
            }
            break;
        }
        case AST_WHILE: {
            P2C_Type *tt = NULL;
            visit_expr(sem, (P2C_AstExpr*)n->u.while_stmt.test, &tt);
            for (size_t i = 0; i < p2c_vec_len(n->u.while_stmt.body); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.while_stmt.body, i));
                if (r != P2C_OK) return r;
            }
            break;
        }
        case AST_FOR: {
            /* イテレータ変数を登録する。タプル/リスト/starredターゲット
             * （for k, v in ... / for first, *rest in ...）は、括弧内の全ての
             * 単純名を束縛する必要がある。以前は単純名ターゲットしか登録して
             * おらず、タプルターゲットのループ本体で参照すると
             * 「undefined name」で誤って失敗していた。 */
            if (n->u.for_stmt.target && !bind_with_target(sem, (P2C_AstExpr*)n->u.for_stmt.target)) {
                set_sem_error(sem, "for target must be a name, tuple, list, or starred name",
                              ((P2C_AstNode*)n->u.for_stmt.target)->line,
                              ((P2C_AstNode*)n->u.for_stmt.target)->col);
                return P2C_ERR_SEMANTIC;
            }
            P2C_Type *it = NULL;
            visit_expr(sem, (P2C_AstExpr*)n->u.for_stmt.iter, &it);
            for (size_t i = 0; i < p2c_vec_len(n->u.for_stmt.body); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.for_stmt.body, i));
                if (r != P2C_OK) return r;
            }
            break;
        }
        case AST_TRY: {
            /* finally内のreturn/break/continueはPythonでは「finallyを実行してから
             * 脱出し、進行中の制御フロー（例外・return）を上書きする」意味を持つ。
             * 生成Cはtry開始時に例外フレームを保存し、脱出時（in_finalbody）に
             * その復元だけを行って本体を再実行しないため、これを受理できる。 */
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.body); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.try_stmt.body, i));
                if (r != P2C_OK) return r;
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
                if (h->name) {
                    p2c_symtab_define(sem->symtab, h->name, SYM_VARIABLE,
                                     p2c_type_builtin(TYPE_OBJECT));
                }
                for (size_t j = 0; j < p2c_vec_len(h->body); j++) {
                    P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(h->body, j));
                    if (r != P2C_OK) return r;
                }
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.orelse); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.try_stmt.orelse, i));
                if (r != P2C_OK) return r;
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.finalbody); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.try_stmt.finalbody, i));
                if (r != P2C_OK) return r;
            }
            break;
        }
        case AST_RAISE:
            visit_expr(sem, (P2C_AstExpr*)n->u.raise.exc, NULL);
            break;
        case AST_ASSERT:
            visit_expr(sem, (P2C_AstExpr*)n->u.assert_stmt.test, NULL);
            visit_expr(sem, (P2C_AstExpr*)n->u.assert_stmt.msg, NULL);
            break;
        case AST_FUNCTIONDEF: {
            if (n->u.functiondef.decorator_list && p2c_vec_len(n->u.functiondef.decorator_list) > 0) {
                P2C_SymbolScope *enclosing = p2c_symtab_current_scope(sem->symtab);
                bool is_method_decorator = false;
                if (enclosing && enclosing->scope_type == SCOPE_CLASS) {
                    /* クラス本体内のメソッドデコレータは、メソッド種別を指定する
                     * 組込みの3つ（@staticmethod/@classmethod/@property）だけを受理する。
                     * 任意のデコレータ適用（呼び出しと再束縛）はクラス本体では
                     * 未対応のため、明示的に診断する。 */
                    for (size_t di = 0; di < p2c_vec_len(n->u.functiondef.decorator_list); di++) {
                        P2C_AstExpr *dec = (P2C_AstExpr*)p2c_vec_get(n->u.functiondef.decorator_list, di);
                        const char *dn = (dec && dec->base.type == AST_NAME) ? dec->base.u.name.name : NULL;
                        if (dec && dec->base.type == AST_ATTRIBUTE && dec->base.u.attribute.attr &&
                            strcmp(dec->base.u.attribute.attr, "setter") == 0) {
                            /* @x.setter（プロパティ x の setter）は対応済み。 */
                            continue;
                        }
                        if (dec && dec->base.type == AST_ATTRIBUTE && dec->base.u.attribute.attr &&
                            strcmp(dec->base.u.attribute.attr, "deleter") == 0) {
                            set_sem_error(sem, "@x.deleter is not supported yet (only @property and @x.setter)", n->line, n->col);
                            return P2C_ERR_SEMANTIC;
                        }
                        if (!dn || (strcmp(dn, "staticmethod") != 0 &&
                                    strcmp(dn, "classmethod") != 0 &&
                                    strcmp(dn, "property") != 0)) {
                            set_sem_error(sem, "only @staticmethod, @classmethod, @property, and @x.setter are supported as method decorators", n->line, n->col);
                            return P2C_ERR_SEMANTIC;
                        }
                    }
                    is_method_decorator = true;
                } else {
                    if (!enclosing || enclosing->scope_type != SCOPE_MODULE) {
                        set_sem_error(sem, "decorators are currently supported only on module-level functions", n->line, n->col);
                        return P2C_ERR_SEMANTIC;
                    }
                    if (n->u.functiondef.vararg || n->u.functiondef.kwarg) {
                        set_sem_error(sem, "decorated functions with *args or **kwargs are not supported yet", n->line, n->col);
                        return P2C_ERR_SEMANTIC;
                    }
                }
                /* メソッド種別デコレータは組込みの意味を持つため、デコレータ式の
                 * 名前解決（visit_expr）は行わない（staticmethod等は変数ではない）。 */
                if (!is_method_decorator) {
                    for (size_t i = 0; i < p2c_vec_len(n->u.functiondef.decorator_list); i++) {
                        P2C_Result decorator_result = visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(n->u.functiondef.decorator_list, i), NULL);
                        if (decorator_result != P2C_OK) return decorator_result;
                    }
                }
            }
            /* クラスメソッドの *args / **kwargs はadapterでtuple/dictへ束縛する。 */
            
            /* 関数名を現在のスコープに登録 */
            P2C_Type *ft = p2c_type_builtin(TYPE_FUNCTION);
            p2c_symtab_define(sem->symtab, n->u.functiondef.name, SYM_FUNCTION, ft);
            
            /* 新しいスコープ */
            p2c_symtab_push_scope(sem->symtab, SCOPE_FUNCTION);
            
            /* 引数を登録 */
            for (size_t i = 0; i < p2c_vec_len(n->u.functiondef.args); i++) {
                P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(n->u.functiondef.args, i);
                P2C_Type *at = p2c_type_builtin(TYPE_ANY);
                if (arg->annotation) {
                    /* 型注釈から型を決定（簡易版） */
                    if (arg->annotation->base.type == AST_NAME) {
                        const char *tn = arg->annotation->base.u.name.name;
                        if (strcmp(tn, "int") == 0) at = p2c_type_builtin(TYPE_INT);
                        else if (strcmp(tn, "float") == 0) at = p2c_type_builtin(TYPE_FLOAT);
                        else if (strcmp(tn, "str") == 0) at = p2c_type_builtin(TYPE_STR);
                        else if (strcmp(tn, "bool") == 0) at = p2c_type_builtin(TYPE_BOOL);
                    }
                }
                p2c_symtab_define(sem->symtab, arg->name, SYM_PARAMETER, at);
            }
            if (n->u.functiondef.vararg) {
                p2c_symtab_define(sem->symtab, n->u.functiondef.vararg, SYM_PARAMETER, p2c_type_builtin(TYPE_TUPLE));
            }
            if (n->u.functiondef.kwarg) {
                p2c_symtab_define(sem->symtab, n->u.functiondef.kwarg, SYM_PARAMETER, p2c_type_builtin(TYPE_DICT));
            }
            
            /* 関数本体 */
            for (size_t i = 0; i < p2c_vec_len(n->u.functiondef.body); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.functiondef.body, i));
                if (r != P2C_OK) { p2c_symtab_pop_scope(sem->symtab); return r; }
            }
            
            p2c_symtab_pop_scope(sem->symtab);
            break;
        }
        case AST_CLASSDEF: {
            if (n->u.classdef.decorator_list && p2c_vec_len(n->u.classdef.decorator_list) > 0) {
                P2C_SymbolScope *decorator_scope = p2c_symtab_current_scope(sem->symtab);
                if (!decorator_scope || decorator_scope->scope_type != SCOPE_MODULE) {
                    set_sem_error(sem, "class decorators are currently supported only on module-level classes", n->line, n->col);
                    return P2C_ERR_SEMANTIC;
                }
                for (size_t i = 0; i < p2c_vec_len(n->u.classdef.decorator_list); i++) {
                    P2C_Result decorator_result = visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(n->u.classdef.decorator_list, i), NULL);
                    if (decorator_result != P2C_OK) return decorator_result;
                }
            }
            /* ネストしたクラス定義（class本体の直下に別のclassを書く）は対応済みで、
             * コード生成側が外側クラス名を前置したC名（Outer__Inner）へ解決し、
             * 外側クラスの__classobjで属性として登録する。ここではクラス本体と
             * モジュール直下だけを許可する。関数本体の中のclass定義はCでは
             * 関数の入れ子定義になり不正なCになるため、はっきり診断する。 */
            {
                P2C_SymbolScope *enclosing = p2c_symtab_current_scope(sem->symtab);
                if (enclosing && enclosing->scope_type != SCOPE_MODULE && enclosing->scope_type != SCOPE_CLASS) {
                    set_sem_error(sem,
                        "class definitions inside functions are not supported yet "
                        "(define the class at module level or directly inside another class)",
                        n->line, n->col);
                    return P2C_ERR_SEMANTIC;
                }
            }
            p2c_symtab_define(sem->symtab, n->u.classdef.name, SYM_CLASS, p2c_type_builtin(TYPE_CLASS));
            
            p2c_symtab_push_scope(sem->symtab, SCOPE_CLASS);
            /* クラスメンバーの処理 */
            for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i));
                if (r != P2C_OK) { p2c_symtab_pop_scope(sem->symtab); return r; }
            }
            p2c_symtab_pop_scope(sem->symtab);
            break;
        }
        case AST_GLOBAL:
            /* global宣言の検証 */
            break;
        case AST_IMPORT:
            /* import mod [as alias] で束縛される名前をシンボルテーブルに登録する。
             * これを怠ると、"import pygame" の後で pygame を代入文やif条件など
             * （visit_exprの戻り値を実際にチェックする文脈）で使った際に、
             * 「undefined name 'pygame'」という誤ったセマンティックエラーで
             * コンパイルが止まってしまう（式文単体としての呼び出しはこの戻り値を
             * チェックしていなかったため、これまで見過ごされていた）。 */
            for (size_t i = 0; i < p2c_vec_len(n->u.import_stmt.names); i++) {
                P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.import_stmt.names, i);
                if (!al) continue;
                const char *bound_name = al->asname ? al->asname : al->name;
                if (bound_name && !p2c_symtab_lookup_local(sem->symtab, bound_name)) {
                    p2c_symtab_define(sem->symtab, bound_name, SYM_VARIABLE, p2c_type_builtin(TYPE_MODULE));
                }
            }
            break;
        case AST_IMPORTFROM:
            for (size_t i = 0; i < p2c_vec_len(n->u.importfrom.names); i++) {
                P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.importfrom.names, i);
                if (!al) continue;
                const char *bound_name = al->asname ? al->asname : al->name;
                if (bound_name && !p2c_symtab_lookup_local(sem->symtab, bound_name)) {
                    p2c_symtab_define(sem->symtab, bound_name, SYM_VARIABLE, p2c_type_builtin(TYPE_ANY));
                }
            }
            break;
        case AST_WITH: {
            if (contains_finally_control_flow(n->u.with.body)) {
                set_sem_error(sem, "return, break, and continue inside with are not supported because __exit__ must run before control leaves the body", n->line, n->col);
                return P2C_ERR_SEMANTIC;
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.with.items); i++) {
                P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, i);
                if (item->optional_vars && !bind_with_target(sem, item->optional_vars)) {
                    set_sem_error(sem, "with ... as target must be a name, tuple, list, or starred name", item->optional_vars->base.line, item->optional_vars->base.col);
                    return P2C_ERR_SEMANTIC;
                }
                P2C_Type *ct = NULL;
                visit_expr(sem, (P2C_AstExpr*)item->context_expr, &ct);
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.with.body); i++) {
                P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.with.body, i));
                if (r != P2C_OK) return r;
            }
            break;
        }
        case AST_DELETE: {
            for (size_t i = 0; i < p2c_vec_len(n->u.delete.targets); i++) {
                visit_expr(sem, (P2C_AstExpr*)p2c_vec_get(n->u.delete.targets, i), NULL);
            }
            break;
        }
        default:
            break;
    }
    return P2C_OK;
}

/* メインエントリ */
P2C_Result p2c_semantic_analyze(P2C_Semantic *sem, P2C_AstModule *mod) {
    if (!sem || !mod) return P2C_ERR_INTERNAL;
    sem->last_error = P2C_OK;
    
    P2C_AstNode *n = (P2C_AstNode*)mod;
    for (size_t i = 0; i < p2c_vec_len(n->u.module.body); i++) {
        P2C_Result r = visit_stmt(sem, (P2C_AstStmt*)p2c_vec_get(n->u.module.body, i));
        if (r != P2C_OK) return r;
    }
    return P2C_OK;
}

P2C_Result p2c_semantic_visit_stmt(P2C_Semantic *sem, P2C_AstStmt *stmt) {
    return visit_stmt(sem, stmt);
}

P2C_Result p2c_semantic_visit_expr(P2C_Semantic *sem, P2C_AstExpr *expr, P2C_Type **out_type) {
    return visit_expr(sem, expr, out_type);
}
