#include "codegen/python_code_to_c_codegen.h"
#include "runtime/python_code_to_c_runtime.h"
#include <stddef.h>
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdio.h>
#endif

const P2C_CodeGenOptions P2C_DEFAULT_OPTIONS = {
    true,
    false,
    true,
    false,
    false,
    4,
    "p2c_",
    NULL,
    false
};

static void gen_expr(P2C_CodeGen *cg, P2C_AstExpr *expr);

static bool builtin_keyword_allowed(const char *name, const char *arg) {
    if (!name) return false;
    if (strcmp(name, "dict") == 0 && !arg) return true;
    if (!arg) return false;
    if (strcmp(name, "print") == 0) return strcmp(arg, "sep") == 0 || strcmp(arg, "end") == 0;
    if (strcmp(name, "sorted") == 0) return strcmp(arg, "key") == 0 || strcmp(arg, "reverse") == 0;
    if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) return strcmp(arg, "key") == 0;
    if (strcmp(name, "enumerate") == 0) return strcmp(arg, "start") == 0;
    if (strcmp(name, "round") == 0) return strcmp(arg, "ndigits") == 0;
    if (strcmp(name, "dict") == 0) return true;
    return false;
}

static bool is_keyword_checked_builtin(const char *name) {
    static const char *const names[] = {"print", "sorted", "min", "max", "enumerate", "round", "dict"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(name, names[i]) == 0) return true;
    }
    return false;
}

/* Pythonの識別子がCの予約語や、生成コードが内部的に使う名前
 * （特にエントリポイントの main）と衝突する場合に安全な名前へ書き換える。
 * これをしないと、Python側で def main(): や main = ... のような
 * ユーザー定義が生成される int main(void) と C レベルで衝突しコンパイル
 * や実行に失敗する。 */
static const char *mangle_ident(const char *name) {
    if (!name) return name;
    /* Cの標準ライブラリ・libmと衝突しうる名前は "p2c_user_" を付けて回避する。
     * "main" 以外にも、"from math import sqrt" のように import した名前が
     * そのままCのグローバル変数名になるケースがあり、libmの同名関数
     * (sqrt, sin, cos, pow, exp, log, floor, ceil, abs 等)と衝突すると
     * コンパイラ/リンカが型の異なるシンボルを混同し、クラッシュに繋がる。 */
    static const char *reserved[] = {
        "main", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
        "exp", "log", "log10", "log2", "pow", "floor", "ceil", "round", "fabs",
        "abs", "fmod", "hypot", "cbrt", "trunc", "modf", "frexp", "ldexp",
        "sinh", "cosh", "tanh", "index", "rindex", "bcopy", "bzero", NULL
    };
    for (int i = 0; reserved[i]; i++) {
        if (strcmp(name, reserved[i]) == 0) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "p2c_user_%s", name);
            return buf;
        }
    }
    return name;
}

/* callable(name) を静的に判定するためのビルトイン名一覧。
 * これらは常に呼び出し可能なので、値として評価せずに True へ畳み込める。 */
static bool is_builtin_callable_name(const char *name) {
    static const char *callables[] = {
        "print", "len", "range", "str", "int", "float", "bool", "abs", "round", "min", "max",
        "sum", "sorted", "reversed", "enumerate", "zip", "type", "isinstance", "any", "all",
        "map", "filter", "list", "tuple", "dict", "set", "frozenset", "ord", "chr", "bin",
        "oct", "hex", "repr", "iter", "next", "input", "hasattr", "getattr", "setattr",
        "delattr", "divmod", "pow", "format", "callable", NULL
    };
    if (!name) return false;
    for (int i = 0; callables[i]; i++) {
        if (strcmp(name, callables[i]) == 0) return true;
    }
    return false;
}

static void write_ident(P2C_CodeGen *cg, const char *name);
static void gen_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
static void gen_stmt_list(P2C_CodeGen *cg, P2C_Vector *stmts);
static void predeclare_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
static void predeclare_expr(P2C_CodeGen *cg, P2C_AstExpr *expr);
static void scan_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
static void gen_suspension_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd);
static void gen_generator_expression(P2C_CodeGen *cg, P2C_AstExpr *expr);

/* return/break/continueで脱出する途中に実行しなければならない後処理を表す。
 * gen_try_stmt()がCローカル変数として積み、cg->cleanup_topから辿るため、
 * 入れ子のtryはCスタックで自動的にLIFO連結される（コード生成中のみ有効な
 * 一時データであり、生成コードの実行時状態ではない）。 */
struct P2C_CleanupFrame {
    struct P2C_CleanupFrame *prev;
    int try_id;             /* 生成済み例外フレーム _p2c_ef_<try_id> の番号 */
    P2C_Vector *finalbody;  /* finally本体（NULL/空なら例外フレームの復元のみ） */
    int loop_depth;         /* このtryを生成し始めた時点のループ入れ子数 */
    /* finally本体を生成している最中はtrue。finallyの中のreturn/break/continueは
     * 「このfinallyを再実行せず、例外フレームの復元だけを行う」必要があるため、
     * このフラグで区別する。 */
    bool in_finalbody;
};

static void indent(P2C_CodeGen *cg) {
    for (int i = 0; i < cg->indent_level * cg->opts.indent_width; i++) p2c_str_append_char(cg->current, ' ');
}
static void write_str(P2C_CodeGen *cg, const char *s) { p2c_str_append(cg->current, s); }
static void write_char(P2C_CodeGen *cg, char c) { p2c_str_append_char(cg->current, c); }
static void write_newline(P2C_CodeGen *cg) { p2c_str_append_char(cg->current, '\n'); }
static void write_line(P2C_CodeGen *cg, const char *s) { indent(cg); write_str(cg, s); write_newline(cg); }
static void push_indent(P2C_CodeGen *cg) { cg->indent_level++; }
static void pop_indent(P2C_CodeGen *cg) { if (cg->indent_level > 0) cg->indent_level--; }

static void map_set_name(P2C_Map *map, const char *name) {
    if (!map || !name || p2c_map_get(map, name)) return;
    char *dup = p2c_alloc(map->alloc, strlen(name) + 1);
    if (!dup) return;
    strcpy(dup, name);
    p2c_map_insert(map, dup, dup);
}
static bool map_has_name(P2C_Map *map, const char *name) { return map && name && p2c_map_get(map, name) != NULL; }

/* 関数名 -> P2C_AstFunctionDef*（ASTが所有するものをそのまま参照）のマップに登録する。
 * デフォルト引数・*args・**kwargsを省略/簡略化した呼び出しへ、呼び出し側で
 * 正しい引数を組み立てるために使う。 */
static void map_set_func_args(P2C_Map *map, const char *name, P2C_AstFunctionDef *fdef) {
    if (!map || !name || p2c_map_get(map, name)) return;
    char *dup = p2c_alloc(map->alloc, strlen(name) + 1);
    if (!dup) return;
    strcpy(dup, name);
    p2c_map_insert(map, dup, fdef);
}

P2C_CodeGen* p2c_codegen_new(P2C_Allocator *a, P2C_CodeGenOptions *opts, P2C_SymbolTable *symtab) {
    P2C_CodeGen *cg = p2c_alloc(a, sizeof(P2C_CodeGen));
    if (!cg) return NULL;
    cg->alloc = a;
    cg->opts = opts ? *opts : P2C_DEFAULT_OPTIONS;
    cg->symtab = symtab;
    cg->header = p2c_str_new(a);
    cg->forward = p2c_str_new(a);
    cg->deferred_defs = NULL;
    cg->body = p2c_str_new(a);
    cg->toplevel = p2c_str_new(a);
    cg->current = cg->body;
    cg->indent_level = 0;
    cg->temp_counter = 0;
    cg->active_loop_id = 0;
    cg->cleanup_top = NULL;
    cg->loop_depth = 0;
    cg->function_depth = 0;
    cg->declared_vars = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->known_classes = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->module_globals = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->class_init_adapter = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->func_args = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->decorated_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->module_function_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->decorator_callable_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->class_methods = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->current_class = NULL;
    cg->closure_env_names = NULL;
    cg->nonlocal_names = NULL;
    cg->cell_names = NULL;
    cg->closure_env_var = NULL;
    cg->genexpr_locals = NULL;
    cg->last_error = P2C_OK;
    cg->error_msg = NULL;
    cg->source_text = NULL;
    cg->lambda_counter = 0;
    cg->generator_expression_counter = 0;
    cg->scan_class_cname = NULL;
    cg->nested_class_aliases = NULL;
    cg->in_class_body = false;
    return cg;
}

void p2c_codegen_set_source(P2C_CodeGen *cg, const char *source_text) {
    if (cg) cg->source_text = source_text;
}

/* 生成中のクラス本体で可視なネストクラスのC名を返す（可視でなければNULL）。
 * ネストクラスはC名を外側クラス名で前置してシンボルを分離しているため、
 * Python名からC名への解決が必要になる。 */
static const char* nested_class_alias(P2C_CodeGen *cg, const char *name) {
    if (!cg || !cg->nested_class_aliases || !name) return NULL;
    return (const char*)p2c_map_get(cg->nested_class_aliases, name);
}

static void free_name_map(P2C_Map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->bucket_count; i++) {
        for (P2C_MapEntry *entry = map->buckets[i]; entry; entry = entry->next) {
            if (entry->key) p2c_free(map->alloc, entry->key);
        }
    }
    p2c_map_free(map);
}

static void free_string_map(P2C_Map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->bucket_count; i++) {
        for (P2C_MapEntry *entry = map->buckets[i]; entry; entry = entry->next) {
            if (entry->key) p2c_free(map->alloc, entry->key);
            if (entry->val) p2c_free(map->alloc, entry->val);
        }
    }
    p2c_map_free(map);
}

static void free_ast_value_map(P2C_Map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->bucket_count; i++) {
        for (P2C_MapEntry *entry = map->buckets[i]; entry; entry = entry->next) {
            if (entry->key) p2c_free(map->alloc, entry->key);
        }
    }
    p2c_map_free(map);
}

void p2c_codegen_free(P2C_CodeGen *cg) {
    if (!cg) return;
    p2c_str_free(cg->header);
    p2c_str_free(cg->forward);
    if (cg->deferred_defs) {
        for (size_t _i = 0; _i < p2c_vec_len(cg->deferred_defs); _i++) {
            P2C_String *_d = (P2C_String*)p2c_vec_get(cg->deferred_defs, _i);
            if (_d) p2c_str_free(_d);
        }
        p2c_vec_free(cg->deferred_defs);
        cg->deferred_defs = NULL;
    }
    p2c_str_free(cg->body);
    p2c_str_free(cg->toplevel);
    free_name_map(cg->declared_vars);
    free_name_map(cg->known_classes);
    free_name_map(cg->module_globals);
    free_name_map(cg->nonlocal_names);
    free_name_map(cg->cell_names);
    free_string_map(cg->class_init_adapter);
    free_ast_value_map(cg->func_args);
    free_name_map(cg->decorated_names);
    free_name_map(cg->module_function_names);
    free_name_map(cg->decorator_callable_names);
    if (cg->class_methods) {
        /* 値はネストしたP2C_Map*なので、外側を解放する前に個々にも解放する */
        for (size_t bi = 0; bi < cg->class_methods->bucket_count; bi++) {
            for (P2C_MapEntry *e = cg->class_methods->buckets[bi]; e; e = e->next) {
                if (e->key) p2c_free(cg->class_methods->alloc, e->key);
                free_name_map((P2C_Map*)e->val);
            }
        }
        p2c_map_free(cg->class_methods);
    }
    if (cg->error_msg) p2c_free(cg->alloc, cg->error_msg);
    p2c_free(cg->alloc, cg);
}

const char* p2c_codegen_error_msg(P2C_CodeGen *cg) { return cg ? cg->error_msg : NULL; }

/* コード生成中に「対応していない組み合わせ」を検出したときに使うエラー設定。
 * これまでcg->last_error/error_msgは構造体にフィールドだけあって実際には
 * 一度も使われておらず、p2c_codegen_generate()の戻り値も常にP2C_OKだった
 * （設定しても呼び出し元まで伝わらなかった）。p2c_codegen_generate()の
 * 末尾でこの状態を見るようにして、実際に機能するようにしてある。 */
static void codegen_set_error(P2C_CodeGen *cg, P2C_Result err, const char *msg) {
    if (!cg) return;
    if (cg->last_error != P2C_OK) return; /* 最初のエラーを優先する */
    cg->last_error = err;
    if (cg->error_msg) p2c_free(cg->alloc, cg->error_msg);
    size_t len = strlen(msg) + 1;
    cg->error_msg = p2c_alloc(cg->alloc, len);
    if (cg->error_msg) memcpy(cg->error_msg, msg, len);
}

static void reset_declared_vars(P2C_CodeGen *cg) {
    if (cg->declared_vars) p2c_map_free(cg->declared_vars);
    cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
}
static void remember_declared(P2C_CodeGen *cg, const char *name) { map_set_name(cg->declared_vars, name); }
static bool is_declared(P2C_CodeGen *cg, const char *name) { return map_has_name(cg->declared_vars, name); }

static void declare_name_if_needed(P2C_CodeGen *cg, const char *name) {
    if (!name || is_declared(cg, name) || map_has_name(cg->nonlocal_names, name)) return;
    indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, name); write_str(cg, " = &P2C_None;"); write_newline(cg);
    remember_declared(cg, name);
}

/* except ... as NAME の束縛変数は setjmp/longjmp をまたいで書き込まれ、
 * longjmp 後に読まれる。C11 6.13.2.1 により、そのような自動変数は volatile
 * でなければ値が不定になりうる（-Wclobbered が指摘するレジスタ退避の最適化が
 * まさにその状況）。他と同様に宣言済み管理へ登録しつつ volatile を付ける。 */
static void declare_volatile_name_if_needed(P2C_CodeGen *cg, const char *name) {
    if (!name || is_declared(cg, name) || map_has_name(cg->nonlocal_names, name)) return;
    indent(cg); write_str(cg, "P2C_Object * volatile "); write_ident(cg, name); write_str(cg, " = &P2C_None;"); write_newline(cg);
    remember_declared(cg, name);
}

static void emit_usize(P2C_CodeGen *cg, size_t value) { p2c_str_append_fmt(cg->current, "%zu", value); }

static void predeclare_expr_list(P2C_CodeGen *cg, P2C_Vector *exprs) {
    if (!exprs) return;
    for (size_t i = 0; i < p2c_vec_len(exprs); i++) predeclare_expr(cg, (P2C_AstExpr*)p2c_vec_get(exprs, i));
}

static void predeclare_expr(P2C_CodeGen *cg, P2C_AstExpr *expr) {
    if (!expr) return;
    P2C_AstNode *n = &expr->base;
    switch (n->type) {
        case AST_NAMED_EXPR:
            if (n->u.named_expr.target && n->u.named_expr.target->base.type == AST_NAME) declare_name_if_needed(cg, n->u.named_expr.target->base.u.name.name);
            predeclare_expr(cg, n->u.named_expr.value);
            break;
        case AST_BINOP:
            predeclare_expr(cg, n->u.binop.left); predeclare_expr(cg, n->u.binop.right);
            break;
        case AST_UNARYOP:
            predeclare_expr(cg, n->u.unaryop.operand);
            break;
        case AST_COMPARE:
            predeclare_expr(cg, n->u.compare.left); predeclare_expr_list(cg, n->u.compare.comparators);
            break;
        case AST_BOOLOP:
            predeclare_expr_list(cg, n->u.boolop.values);
            break;
        case AST_CALL:
            predeclare_expr(cg, n->u.call.func); predeclare_expr_list(cg, n->u.call.args);
            if (n->u.call.keywords) for (size_t i = 0; i < p2c_vec_len(n->u.call.keywords); i++) { P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, i); if (kw) predeclare_expr(cg, kw->value); }
            break;
        case AST_ATTRIBUTE:
            predeclare_expr(cg, n->u.attribute.value);
            break;
        case AST_SUBSCRIPT:
            predeclare_expr(cg, n->u.subscript.value); predeclare_expr(cg, n->u.subscript.slice);
            break;
        case AST_IFEXP:
            predeclare_expr(cg, n->u.ifexp.test); predeclare_expr(cg, n->u.ifexp.body); predeclare_expr(cg, n->u.ifexp.orelse);
            break;
        case AST_LIST: case AST_TUPLE: case AST_SET:
            predeclare_expr_list(cg, n->u.list.elts);
            break;
        case AST_DICT:
            predeclare_expr_list(cg, n->u.dict.keys); predeclare_expr_list(cg, n->u.dict.values);
            break;
        case AST_STARRED:
            predeclare_expr(cg, n->u.starred.value);
            break;
        case AST_COMPREHENSION:
        case AST_GENERATOR_EXPRESSION:
            predeclare_expr(cg, n->u.comprehension.elt); predeclare_expr(cg, n->u.comprehension.dict_key);
            if (n->u.comprehension.generators) for (size_t i = 0; i < p2c_vec_len(n->u.comprehension.generators); i++) { P2C_AstComprehensionGen *gen = (P2C_AstComprehensionGen*)p2c_vec_get(n->u.comprehension.generators, i); if (gen) { predeclare_expr(cg, gen->iter); predeclare_expr_list(cg, gen->ifs); } }
            break;
        default:
            break;
    }
}

static void emit_args_array(P2C_CodeGen *cg, P2C_Vector *args) {
    size_t argc = args ? p2c_vec_len(args) : 0;
    if (argc == 0) {
        write_str(cg, "NULL, 0");
        return;
    }
    /* *args アンパック (f(*lst)) は、呼び出し先の引数の個数が
     * コンパイル時に分かっている「既知の関数への直接呼び出し」でのみ対応
     * している（そちらは別の場所で展開済み）。ここ(emit_args_array)は
     * ラムダ・変数に代入された関数・メソッド呼び出しなど、呼び出し先の
     * シグネチャが分からない経路を担当しており、*args を安全に展開する
     * 手段が無い。誤って要素数が合わない配列を渡すと、呼び出し先が
     * 存在しない要素を読みに行きクラッシュするため、はっきりエラーに
     * する（README/PORTING.mdに記載の既知の制限と同じ考え方）。 */
    for (size_t i = 0; i < argc; i++) {
        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(args, i);
        if (a && a->base.type == AST_STARRED) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                "*args unpacking (f(*lst)) at a call site is only supported when the "
                "callee is a known top-level function with a fixed signature. "
                "It is not supported for lambdas, variables holding a function, "
                "or method calls. Pass the arguments explicitly instead.");
            write_str(cg, "NULL, 0"); /* エラー扱いになるので実際には使われない */
            return;
        }
    }
    write_str(cg, "(P2C_Object*[]){");
    for (size_t i = 0; i < argc; i++) {
        if (i) write_str(cg, ", ");
        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(args, i));
    }
    write_str(cg, "}, ");
    emit_usize(cg, argc);
}

static void gen_string_literal_contents(P2C_CodeGen *cg, const char *src) {
    if (!src) return;
    for (const char *p = src; *p; p++) {
        if (*p == '"') write_str(cg, "\\\"");
        else if (*p == '\\') write_str(cg, "\\\\");
        else if (*p == '\n') write_str(cg, "\\n");
        else if (*p == '\r') write_str(cg, "\\r");
        else if (*p == '\t') write_str(cg, "\\t");
        else if (*p == '\v') write_str(cg, "\\v");
        else if (*p == '\f') write_str(cg, "\\f");
        else if (*p == '\b') write_str(cg, "\\b");
        else if (*p == '\a') write_str(cg, "\\a");
        else write_char(cg, *p);
    }
}

/* モジュールのトップレベル文（関数/クラス定義の中は除く）を走査し、
 * 単純名への代入・複合代入・forループ変数を module_globals に登録する。
 * これらは実際のC言語のファイルスコープ変数として宣言され、関数内から
 * `global x` で参照・再代入できるようにする。 */
static void collect_module_level_name(P2C_CodeGen *cg, const char *name) {
    map_set_name(cg->module_globals, name);
}
static void collect_module_globals(P2C_CodeGen *cg, P2C_Vector *stmts) {
    if (!stmts) return;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt) continue;
        P2C_AstNode *n = &stmt->base;
        switch (n->type) {
            case AST_ASSIGN:
                for (size_t j = 0; j < p2c_vec_len(n->u.assign.targets); j++) {
                    P2C_AstExpr *t = (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, j);
                    if (t && t->base.type == AST_NAME) collect_module_level_name(cg, t->base.u.name.name);
                    else if (t && t->base.type == AST_TUPLE) {
                        P2C_Vector *elts = t->base.u.tuple.elts;
                        for (size_t k = 0; k < p2c_vec_len(elts); k++) {
                            P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, k);
                            if (e && e->base.type == AST_NAME) collect_module_level_name(cg, e->base.u.name.name);
                            else if (e && e->base.type == AST_STARRED && e->base.u.starred.value && e->base.u.starred.value->base.type == AST_NAME) collect_module_level_name(cg, e->base.u.starred.value->base.u.name.name);
                        }
                    }
                }
                break;
            case AST_AUGASSIGN:
                if (n->u.augassign.target && n->u.augassign.target->base.type == AST_NAME) collect_module_level_name(cg, n->u.augassign.target->base.u.name.name);
                break;
            case AST_IMPORT:
                /* import mod [as alias] で束縛される名前も、Cのファイルスコープ変数
                 * として扱う必要がある。以前はここが欠けており、import文がmain()の
                 * ローカル変数として生成されていたため、import されたモジュールを
                 * main以外の関数（すなわちPython側の関数のほとんど）から参照すると
                 * "undeclared identifier" というCコンパイルエラーになっていた。 */
                for (size_t j = 0; j < p2c_vec_len(n->u.import_stmt.names); j++) {
                    P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.import_stmt.names, j);
                    if (al) collect_module_level_name(cg, al->asname ? al->asname : al->name);
                }
                break;
            case AST_IMPORTFROM:
                for (size_t j = 0; j < p2c_vec_len(n->u.importfrom.names); j++) {
                    P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.importfrom.names, j);
                    if (al) collect_module_level_name(cg, al->asname ? al->asname : al->name);
                }
                break;
            case AST_FOR:
                if (n->u.for_stmt.target && n->u.for_stmt.target->base.type == AST_NAME) collect_module_level_name(cg, n->u.for_stmt.target->base.u.name.name);
                collect_module_globals(cg, n->u.for_stmt.body);
                break;
            case AST_WHILE:
                collect_module_globals(cg, n->u.while_stmt.body);
                break;
            case AST_IF:
                collect_module_globals(cg, n->u.if_stmt.body);
                collect_module_globals(cg, n->u.if_stmt.orelse);
                break;
            case AST_TRY:
                collect_module_globals(cg, n->u.try_stmt.body);
                collect_module_globals(cg, n->u.try_stmt.finalbody);
                for (size_t j = 0; j < p2c_vec_len(n->u.try_stmt.handlers); j++) {
                    P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, j);
                    if (h) collect_module_globals(cg, h->body);
                }
                break;
            default:
                break;
        }
    }
}

/* super()解決のため、scan_stmt (この少し下) から使う。定義自体は
 * 継承解決コードの近くにある（ずっと下）。 */
static const char* extract_base_name(P2C_AstExpr *b);

static void scan_stmt_list(P2C_CodeGen *cg, P2C_Vector *stmts) {
    if (!stmts) return;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) scan_stmt(cg, (P2C_AstStmt*)p2c_vec_get(stmts, i));
}

static void scan_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) {
    if (!stmt) return;
    P2C_AstNode *n = &stmt->base;
    switch (n->type) {
        case AST_CLASSDEF: {
            /* ネストしたクラス定義はC名を外側クラス名で前置して衝突を避ける
             * （Outer__Inner）。モジュール直下のクラスはクラス名そのものを使う
             * 従来の名前付けを維持する。 */
            char nested_scan_cname[512];
            const char *class_cname = n->u.classdef.name;
            if (cg->scan_class_cname) {
                snprintf(nested_scan_cname, sizeof(nested_scan_cname), "%s__%s", cg->scan_class_cname, n->u.classdef.name);
                class_cname = nested_scan_cname;
            } else {
                map_set_name(cg->known_classes, n->u.classdef.name);
            }
            if (n->u.classdef.decorator_list && p2c_vec_len(n->u.classdef.decorator_list) > 0) {
                map_set_name(cg->decorated_names, n->u.classdef.name);
                for (size_t di = 0; di < p2c_vec_len(n->u.classdef.decorator_list); di++) {
                    P2C_AstExpr *decorator = (P2C_AstExpr*)p2c_vec_get(n->u.classdef.decorator_list, di);
                    if (decorator && decorator->base.type == AST_NAME) map_set_name(cg->decorator_callable_names, decorator->base.u.name.name);
                }
            }
            P2C_Map *methods = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
            for (size_t mi = 0; mi < p2c_vec_len(n->u.classdef.body); mi++) {
                P2C_AstStmt *member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, mi);
                if (member->base.type == AST_FUNCTIONDEF) map_set_name(methods, member->base.u.functiondef.name);
            }
            {
                char *cname_dup2 = p2c_alloc(cg->alloc, strlen(class_cname) + 1);
                if (cname_dup2) { strcpy(cname_dup2, class_cname); p2c_map_insert(cg->class_methods, cname_dup2, methods); }
            }
            {
                const char *saved_scan_cname = cg->scan_class_cname;
                cg->scan_class_cname = class_cname;
                scan_stmt_list(cg, n->u.classdef.body);
                cg->scan_class_cname = saved_scan_cname;
            }
            break;
        }
        case AST_FUNCTIONDEF:
            /* クラス本体内のメソッドは、モジュール直下の関数ではないため
             * モジュール関数名やデコレータとして登録しない（同名のグローバルを
             * 上書きしたり、デコレータ適用扱いにしてしまうのを防ぐ）。 */
            if (cg->function_depth == 0 && !cg->scan_class_cname) {
                map_set_func_args(cg->func_args, n->u.functiondef.name, &n->u.functiondef);
            }
            if (cg->function_depth == 0 && !cg->scan_class_cname) {
                map_set_name(cg->module_function_names, n->u.functiondef.name);
                if (n->u.functiondef.decorator_list && p2c_vec_len(n->u.functiondef.decorator_list) > 0) {
                    map_set_name(cg->decorated_names, n->u.functiondef.name);
                    for (size_t di = 0; di < p2c_vec_len(n->u.functiondef.decorator_list); di++) {
                        P2C_AstExpr *decorator = (P2C_AstExpr*)p2c_vec_get(n->u.functiondef.decorator_list, di);
                        if (decorator && decorator->base.type == AST_NAME) map_set_name(cg->decorator_callable_names, decorator->base.u.name.name);
                    }
                }
            }
            scan_stmt_list(cg, n->u.functiondef.body);
            break;
        case AST_IF:
            scan_stmt_list(cg, n->u.if_stmt.body);
            scan_stmt_list(cg, n->u.if_stmt.orelse);
            break;
        case AST_WHILE:
            scan_stmt_list(cg, n->u.while_stmt.body);
            scan_stmt_list(cg, n->u.while_stmt.orelse);
            break;
        case AST_FOR:
            scan_stmt_list(cg, n->u.for_stmt.body);
            scan_stmt_list(cg, n->u.for_stmt.orelse);
            break;
        case AST_TRY:
            scan_stmt_list(cg, n->u.try_stmt.body);
            scan_stmt_list(cg, n->u.try_stmt.orelse);
            scan_stmt_list(cg, n->u.try_stmt.finalbody);
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
                if (h) scan_stmt_list(cg, h->body);
            }
            break;
        case AST_BLOCK:
            scan_stmt_list(cg, n->u.block.stmts);
            break;
        default:
            break;
    }
}

static void predeclare_stmt_list(P2C_CodeGen *cg, P2C_Vector *stmts) {
    if (!stmts) return;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) predeclare_stmt(cg, (P2C_AstStmt*)p2c_vec_get(stmts, i));
}

static void predeclare_import_aliases(P2C_CodeGen *cg, P2C_Vector *names) {
    if (!names) return;
    for (size_t i = 0; i < p2c_vec_len(names); i++) {
        P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(names, i);
        if (al) declare_name_if_needed(cg, al->asname ? al->asname : al->name);
    }
}

static void predeclare_match_pattern(P2C_CodeGen *cg, P2C_AstMatchPattern *pattern) {
    if (!pattern) return;
    if (pattern->capture_name) declare_name_if_needed(cg, pattern->capture_name);
    if (pattern->rest_name) declare_name_if_needed(cg, pattern->rest_name);
    if (pattern->children) {
        for (size_t i = 0; i < p2c_vec_len(pattern->children); i++) {
            predeclare_match_pattern(cg, (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i));
        }
    }
}

static void predeclare_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) {
    if (!cg || !stmt) return;
    P2C_AstNode *n = &stmt->base;
    switch (n->type) {
        case AST_ASSIGN:
            for (size_t i = 0; i < p2c_vec_len(n->u.assign.targets); i++) {
                P2C_AstExpr *t = (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, i);
                if (!t) continue;
                if (t->base.type == AST_NAME) declare_name_if_needed(cg, t->base.u.name.name);
                else if (t->base.type == AST_TUPLE) {
                    P2C_Vector *elts = t->base.u.tuple.elts;
                    for (size_t j = 0; j < p2c_vec_len(elts); j++) {
                        P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, j);
                        if (e && e->base.type == AST_NAME) declare_name_if_needed(cg, e->base.u.name.name);
                        else if (e && e->base.type == AST_STARRED && e->base.u.starred.value && e->base.u.starred.value->base.type == AST_NAME) declare_name_if_needed(cg, e->base.u.starred.value->base.u.name.name);
                    }
                }
            }
            predeclare_expr(cg, n->u.assign.value);
            break;
        case AST_ANNASSIGN:
            if (n->u.annassign.target && n->u.annassign.target->base.type == AST_NAME) declare_name_if_needed(cg, n->u.annassign.target->base.u.name.name);
            predeclare_expr(cg, n->u.annassign.value);
            break;
        case AST_AUGASSIGN:
            if (n->u.augassign.target && n->u.augassign.target->base.type == AST_NAME) declare_name_if_needed(cg, n->u.augassign.target->base.u.name.name);
            predeclare_expr(cg, n->u.augassign.value);
            break;
        case AST_FOR:
            if (n->u.for_stmt.target && n->u.for_stmt.target->base.type == AST_NAME) declare_name_if_needed(cg, n->u.for_stmt.target->base.u.name.name);
            else if (n->u.for_stmt.target && n->u.for_stmt.target->base.type == AST_TUPLE) {
                P2C_Vector *elts = n->u.for_stmt.target->base.u.tuple.elts;
                for (size_t i = 0; i < p2c_vec_len(elts); i++) {
                    P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, i);
                    if (e && e->base.type == AST_NAME) declare_name_if_needed(cg, e->base.u.name.name);
                    else if (e && e->base.type == AST_STARRED && e->base.u.starred.value && e->base.u.starred.value->base.type == AST_NAME) declare_name_if_needed(cg, e->base.u.starred.value->base.u.name.name);
                }
            }
            predeclare_expr(cg, n->u.for_stmt.iter);
            predeclare_stmt_list(cg, n->u.for_stmt.body);
            predeclare_stmt_list(cg, n->u.for_stmt.orelse);
            break;
        case AST_IF:
            predeclare_expr(cg, n->u.if_stmt.test);
            predeclare_stmt_list(cg, n->u.if_stmt.body);
            predeclare_stmt_list(cg, n->u.if_stmt.orelse);
            break;
        case AST_WHILE:
            predeclare_expr(cg, n->u.while_stmt.test);
            predeclare_stmt_list(cg, n->u.while_stmt.body);
            predeclare_stmt_list(cg, n->u.while_stmt.orelse);
            break;
        case AST_TRY:
            predeclare_stmt_list(cg, n->u.try_stmt.body);
            predeclare_stmt_list(cg, n->u.try_stmt.orelse);
            predeclare_stmt_list(cg, n->u.try_stmt.finalbody);
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
                if (h && h->name) declare_volatile_name_if_needed(cg, h->name);
                if (h) predeclare_stmt_list(cg, h->body);
            }
            break;
        case AST_MATCH:
            predeclare_expr(cg, n->u.match_stmt.subject);
            if (n->u.match_stmt.cases) for (size_t i = 0; i < p2c_vec_len(n->u.match_stmt.cases); i++) { P2C_AstMatchCase *match_case = (P2C_AstMatchCase*)p2c_vec_get(n->u.match_stmt.cases, i); if (match_case) { predeclare_match_pattern(cg, match_case->pattern); predeclare_expr(cg, match_case->guard); predeclare_stmt_list(cg, match_case->body); } }
            break;
        case AST_WITH:
            for (size_t i = 0; i < p2c_vec_len(n->u.with.items); i++) {
                P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, i);
                if (!item || !item->optional_vars) continue;
                if (item->optional_vars->base.type == AST_NAME) {
                    declare_name_if_needed(cg, item->optional_vars->base.u.name.name);
                } else if (item->optional_vars->base.type == AST_TUPLE || item->optional_vars->base.type == AST_LIST) {
                    P2C_Vector *elts = item->optional_vars->base.type == AST_TUPLE ? item->optional_vars->base.u.tuple.elts : item->optional_vars->base.u.list.elts;
                    for (size_t j = 0; j < p2c_vec_len(elts); j++) {
                        P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, j);
                        if (elt && elt->base.type == AST_NAME) declare_name_if_needed(cg, elt->base.u.name.name);
                        else if (elt && elt->base.type == AST_STARRED && elt->base.u.starred.value && elt->base.u.starred.value->base.type == AST_NAME) declare_name_if_needed(cg, elt->base.u.starred.value->base.u.name.name);
                    }
                }
            }
            for (size_t i = 0; i < p2c_vec_len(n->u.with.items); i++) { P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, i); if (item) predeclare_expr(cg, item->context_expr); }
            predeclare_stmt_list(cg, n->u.with.body);
            break;
        case AST_FUNCTIONDEF:
            declare_name_if_needed(cg, n->u.functiondef.name);
            break;
        case AST_IMPORT:
            predeclare_import_aliases(cg, n->u.import_stmt.names);
            break;
        case AST_IMPORTFROM:
            predeclare_import_aliases(cg, n->u.importfrom.names);
            break;
        case AST_RETURN:
            predeclare_expr(cg, n->u.return_stmt.value);
            break;
        case AST_EXPR_STMT:
            predeclare_expr(cg, n->u.expr_stmt.value);
            break;
        case AST_RAISE:
            predeclare_expr(cg, n->u.raise.exc); predeclare_expr(cg, n->u.raise.cause);
            break;
        case AST_ASSERT:
            predeclare_expr(cg, n->u.assert_stmt.test); predeclare_expr(cg, n->u.assert_stmt.msg);
            break;
        case AST_BLOCK:
            predeclare_stmt_list(cg, n->u.block.stmts);
            break;
        default:
            break;
    }
}

static void write_ident(P2C_CodeGen *cg, const char *name) {
    write_str(cg, mangle_ident(name));
}

/* リスト内包表記の generators を再帰的にコード生成する。
 * gen_idx番目のfor節から始めて、最後まで到達したらeltをリストへappendする。
 * 複数のfor節(ネストしたループ)・複数のif条件(フィルタ)の両方に対応する。 */
static void gen_comprehension_body(P2C_CodeGen *cg, int comp_id, P2C_Vector *generators, size_t gen_idx, P2C_AstExpr *elt, P2C_AstExpr *dict_key, const char *list_var, const char *append_fn) {
    if (gen_idx >= p2c_vec_len(generators)) {
        if (dict_key) {
            write_str(cg, "p2c_dict_set("); write_str(cg, list_var); write_str(cg, ", ");
            gen_expr(cg, dict_key);
            write_str(cg, ", "); gen_expr(cg, elt); write_str(cg, ");");
        } else {
            write_str(cg, append_fn); write_str(cg, "("); write_str(cg, list_var); write_str(cg, ", ");
            gen_expr(cg, elt);
            write_str(cg, ");");
        }
        return;
    }
    P2C_AstComprehensionGen *gen = (P2C_AstComprehensionGen*)p2c_vec_get(generators, gen_idx);
    char iter_var[64], n_var[64], i_var[64], item_var[64];
    snprintf(iter_var, sizeof(iter_var), "_p2c_citer_%d_%zu", comp_id, gen_idx);
    snprintf(n_var, sizeof(n_var), "_p2c_cn_%d_%zu", comp_id, gen_idx);
    snprintf(i_var, sizeof(i_var), "_p2c_ci_%d_%zu", comp_id, gen_idx);
    snprintf(item_var, sizeof(item_var), "_p2c_citem_%d_%zu", comp_id, gen_idx);

    write_str(cg, "{ P2C_Object *"); write_str(cg, iter_var); write_str(cg, " = "); gen_expr(cg, gen->iter); write_str(cg, "; ");
    write_str(cg, "size_t "); write_str(cg, n_var); write_str(cg, " = (size_t)p2c_len("); write_str(cg, iter_var); write_str(cg, "); ");
    write_str(cg, "for (size_t "); write_str(cg, i_var); write_str(cg, " = 0; "); write_str(cg, i_var); write_str(cg, " < "); write_str(cg, n_var); write_str(cg, "; "); write_str(cg, i_var); write_str(cg, "++) { ");
    write_str(cg, "P2C_Object *"); write_str(cg, item_var); write_str(cg, " = p2c_iter_at("); write_str(cg, iter_var); write_str(cg, ", (int64_t)"); write_str(cg, i_var); write_str(cg, "); ");

    if (gen->target->base.type == AST_NAME) {
        write_str(cg, "P2C_Object *"); write_ident(cg, gen->target->base.u.name.name); write_str(cg, " = "); write_str(cg, item_var); write_str(cg, "; ");
    } else if (gen->target->base.type == AST_TUPLE) {
        P2C_Vector *elts = gen->target->base.u.tuple.elts;
        for (size_t k = 0; k < p2c_vec_len(elts); k++) {
            P2C_AstExpr *te = (P2C_AstExpr*)p2c_vec_get(elts, k);
            if (te->base.type != AST_NAME) continue;
            write_str(cg, "P2C_Object *"); write_ident(cg, te->base.u.name.name); write_str(cg, " = p2c_subscript_get(");
            write_str(cg, item_var); write_str(cg, ", p2c_obj_from_int("); emit_usize(cg, k); write_str(cg, ")); ");
        }
    }

    size_t nifs = p2c_vec_len(gen->ifs);
    if (nifs > 0) {
        write_str(cg, "if (");
        for (size_t k = 0; k < nifs; k++) {
            if (k > 0) write_str(cg, " && ");
            write_str(cg, "p2c_obj_is_truthy(");
            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(gen->ifs, k));
            write_str(cg, ")");
        }
        write_str(cg, ") { ");
    }
    gen_comprehension_body(cg, comp_id, generators, gen_idx + 1, elt, dict_key, list_var, append_fn);
    if (nifs > 0) write_str(cg, " }");
    write_str(cg, " } }");
}

/* 未対応の式/文に遭遇したときの共通処理。
 *   --fallback 指定時: 実行時に NotImplementedError を送出するスタブを出力し、
 *     変換は続行する（未対応構文を含んでいてもビルドを失敗させない）。
 *   既定: 変換エラーとして報告する。以前は式で &P2C_None、文でコメントを
 *     出力して黙って続行していたため、未対応構文が「静かに違う動作」になる
 *     原因になっていた（フォールバックとエラーのどちらでも、出力の整合を
 *     保つために &P2C_None / 空文は必ず書く）。 */
static void emit_unsupported_construct(P2C_CodeGen *cg, P2C_AstNode *n, const char *what, bool as_expr) {
    uint32_t line = n ? n->line : 0u;
    if (cg->opts.fallback_unsupported) {
        if (as_expr) {
            write_str(cg, "p2c_fallback_expr(\"line ");
            emit_usize(cg, (size_t)line);
            write_str(cg, "\", \"");
            write_str(cg, what);
            write_str(cg, "\")");
        } else {
            indent(cg);
            write_str(cg, "p2c_fallback_stmt(\"line ");
            emit_usize(cg, (size_t)line);
            write_str(cg, "\", \"");
            write_str(cg, what);
            write_str(cg, "\");");
            write_newline(cg);
        }
        return;
    }
    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                      "unsupported construct encountered during code generation "
                      "(rerun with --fallback to replace it with a runtime stub)");
    if (as_expr) {
        write_str(cg, "&P2C_None");
    } else {
        indent(cg);
        write_str(cg, "/* unsupported statement */");
        write_newline(cg);
    }
}

static void gen_expr(P2C_CodeGen *cg, P2C_AstExpr *expr) {
    if (!expr) { write_str(cg, "&P2C_None"); return; }
    P2C_AstNode *n = &expr->base;
    switch (n->type) {
        case AST_NAME:
            /* --fallback（未対応構文のフォールバック）: parserが未対応リテラル等の
             * 代わりに作った名前ノード。実行時 NotImplementedError になるスタブへ
             * 変換する（strictモードではparserが先にエラーにするため到達しない）。 */
            if (n->u.name.name &&
                strncmp(n->u.name.name, P2C_UNSUPPORTED_NAME_PREFIX,
                        strlen(P2C_UNSUPPORTED_NAME_PREFIX)) == 0) {
                emit_unsupported_construct(cg, n,
                    n->u.name.name + strlen(P2C_UNSUPPORTED_NAME_PREFIX), true);
                break;
            }
            /* genexpr（および状態機械関数）のステップ関数の中では、Pythonの
             * ローカル名をジェネレータのローカル辞書から読む。これにより
             * gen_exprが持つ組込み関数ディスパッチやタプル/辞書表示、
             * 添字・属性アクセスの中でもgenexprのローカル名を正しく解決できる
             * （以前は生の識別子として出力され、生成Cが未宣言エラーになっていた）。 */
            if (cg->genexpr_locals && map_has_name(cg->genexpr_locals, n->u.name.name)) {
                write_str(cg, "p2c_generator_local_get(generator, \"");
                write_str(cg, n->u.name.name);
                write_str(cg, "\")");
                break;
            }
            /* Ellipsis はビルトインの単一値。Cの識別子ではないため、
             * 名前置換が起きる前にシングルトンへ解決する。
             * 合成ノード（--fallback 等）では name が NULL になり得るので、
             * 非NULLを確認してから比較する（clang静的解析の指摘）。 */
            if (n->u.name.name && strcmp(n->u.name.name, "Ellipsis") == 0) {
                write_str(cg, "&P2C_Ellipsis");
            } else if (map_has_name(cg->closure_env_names, n->u.name.name) && !map_has_name(cg->declared_vars, n->u.name.name) && cg->closure_env_var) {
                write_str(cg, "p2c_cell_get(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", p2c_obj_from_str(\""); write_str(cg, n->u.name.name); write_str(cg, "\")))");
            } else if (map_has_name(cg->cell_names, n->u.name.name)) {
                write_str(cg, "p2c_cell_get(_p2c_cell_"); write_ident(cg, n->u.name.name); write_str(cg, ")");
            } else if (map_has_name(cg->decorated_names, n->u.name.name)) {
                write_str(cg, "_p2c_decorated_"); write_ident(cg, n->u.name.name);
            } else if (n->u.name.name && is_builtin_callable_name(n->u.name.name) &&
                       !map_has_name(cg->closure_env_names, n->u.name.name) &&
                       !map_has_name(cg->cell_names, n->u.name.name)) {
                /* 組込み関数を「値」として使う場合（sorted(a, key=len) など）は、
                 * 実行時に呼び出し可能オブジェクトへ解決する。以前は生の識別子
                 * len を出力していたため生成 C がコンパイルできなかった。
                 * 未対応の名前は NotImplementedError として明示的に失敗する。 */
                write_str(cg, "p2c_builtin_ref_checked(\"");
                write_str(cg, n->u.name.name);
                write_str(cg, "\")");
            } else {
                const char *nested_alias = nested_class_alias(cg, n->u.name.name);
                if (nested_alias) {
                    /* クラス本体ではネストクラス名を前置済みC名へ解決する。
                     * Pythonではクラススコープの名前はメソッド本体から見えない
                     * （NameError）ため、その位置での参照は黙って壊れたCを
                     * 出さず明示的に診断する。 */
                    if (!cg->in_class_body) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                            "nested class names are not visible inside method bodies; refer to the class through its enclosing class (Outer.Inner)");
                    }
                    write_str(cg, nested_alias);
                } else {
                    write_ident(cg, n->u.name.name);
                }
            }
            break;
        case AST_CONST:
            switch (n->u.constant.token_type) {
                case TOK_INT_LITERAL:
                    write_str(cg, "p2c_obj_from_int("); write_str(cg, n->u.constant.value); write_str(cg, ")");
                    break;
                case TOK_FLOAT_LITERAL:
                    write_str(cg, "p2c_obj_from_float("); write_str(cg, n->u.constant.value); write_str(cg, ")");
                    break;
                case TOK_STR_LITERAL:
                    write_str(cg, "p2c_obj_from_str(\""); gen_string_literal_contents(cg, n->u.constant.value); write_str(cg, "\")");
                    break;
                case TOK_BOOL_LITERAL:
                    write_str(cg, strcmp(n->u.constant.value, "True") == 0 ? "&P2C_True" : "&P2C_False");
                    break;
                case TOK_NONE_LITERAL:
                    write_str(cg, "&P2C_None");
                    break;
                case TOK_ELLIPSIS:
                    write_str(cg, "&P2C_Ellipsis");
                    break;
                default:
                    write_str(cg, "&P2C_None");
                    break;
            }
            break;
        case AST_BINOP: {
            if (n->u.binop.is_fstring_concat) {
                write_str(cg, "(p2c_fstr_begin(), p2c_fstr_append(");
                gen_expr(cg, n->u.binop.left);
                write_str(cg, "), p2c_fstr_append(");
                gen_expr(cg, n->u.binop.right);
                write_str(cg, "), p2c_fstr_finish())");
                break;
            }
            const char *func = "p2c_obj_add";
            switch (n->u.binop.op) {
                case OP_ADD: func = "p2c_obj_add"; break;
                case OP_SUB: func = "p2c_obj_sub"; break;
                case OP_MULT: func = "p2c_obj_mul"; break;
                case OP_DIV: func = "p2c_obj_div"; break;
                case OP_FLOORDIV: func = "p2c_obj_floordiv"; break;
                case OP_MOD: func = "p2c_obj_mod"; break;
                case OP_POW: func = "p2c_obj_pow"; break;
                case OP_LSHIFT: func = "p2c_obj_lshift"; break;
                case OP_RSHIFT: func = "p2c_obj_rshift"; break;
                case OP_BITAND: func = "p2c_obj_bitand"; break;
                case OP_BITOR: func = "p2c_obj_bitor"; break;
                case OP_BITXOR: func = "p2c_obj_bitxor"; break;
                default: break;
            }
            write_str(cg, "(p2c_binop_begin(");
            gen_expr(cg, n->u.binop.left);
            write_str(cg, "), p2c_binop_finish("); write_str(cg, func); write_str(cg, ", ");
            gen_expr(cg, n->u.binop.right); write_str(cg, "))");
            break;
        }
        case AST_UNARYOP:
            if (n->u.unaryop.op == OP_NOT) {
                write_str(cg, "(!p2c_obj_is_truthy("); gen_expr(cg, n->u.unaryop.operand); write_str(cg, ") ? &P2C_True : &P2C_False)");
            } else if (n->u.unaryop.op == OP_USUB) {
                write_str(cg, "p2c_obj_mul(p2c_obj_from_int(-1), "); gen_expr(cg, n->u.unaryop.operand); write_str(cg, ")");
            } else if (n->u.unaryop.op == OP_INVERT) {
                write_str(cg, "p2c_obj_invert("); gen_expr(cg, n->u.unaryop.operand); write_str(cg, ")");
            } else {
                gen_expr(cg, n->u.unaryop.operand);
            }
            break;
        case AST_COMPARE: {
            size_t nops = p2c_vec_len(n->u.compare.ops);
            if (nops == 0) { gen_expr(cg, n->u.compare.left); break; }
            if (nops == 1) {
                /* 単一比較は従来通りP2C_Object*をそのまま返す（値としても使えるようにするため）。 */
                P2C_AstOperator *op = (P2C_AstOperator*)p2c_vec_get(n->u.compare.ops, 0);
                P2C_AstExpr *right = (P2C_AstExpr*)p2c_vec_get(n->u.compare.comparators, 0);
                const char *func = "p2c_obj_eq";
                bool negate = false;
                switch (*op) {
                    case OP_LT: func = "p2c_obj_lt"; break;
                    case OP_LE: func = "p2c_obj_le"; break;
                    case OP_EQ: func = "p2c_obj_eq"; break;
                    case OP_NE: func = "p2c_obj_ne"; break;
                    case OP_GT: func = "p2c_obj_gt"; break;
                    case OP_GE: func = "p2c_obj_ge"; break;
                    case OP_IN: func = "p2c_obj_contains"; break;
                    case OP_NOTIN: func = "p2c_obj_contains"; negate = true; break;
                    case OP_IS: func = "p2c_obj_is"; break;
                    case OP_ISNOT: func = "p2c_obj_is"; negate = true; break;
                    default: break;
                }
                /* IN/NOTIN: 引数順は (container, item) */
                bool swap_args = (*op == OP_IN || *op == OP_NOTIN);
                if (negate) write_str(cg, "p2c_bool_not(");
                write_str(cg, func); write_str(cg, "(");
                if (swap_args) { gen_expr(cg, right); write_str(cg, ", "); gen_expr(cg, n->u.compare.left); }
                else           { gen_expr(cg, n->u.compare.left); write_str(cg, ", "); gen_expr(cg, right); }
                write_str(cg, ")");
                if (negate) write_str(cg, ")");
                break;
            }
            /* Pythonの連鎖比較 (a < b < c は (a<b) and (b<c) と同じ) に対応する。
             * 中間のオペランド(b)は式として2回評価される点に注意
             * （副作用のある式を連鎖比較の中間項に書くのは稀なため許容している）。 */
            write_str(cg, "p2c_obj_from_bool(");
            for (size_t i = 0; i < nops; i++) {
                P2C_AstOperator *op = (P2C_AstOperator*)p2c_vec_get(n->u.compare.ops, i);
                P2C_AstExpr *right = (P2C_AstExpr*)p2c_vec_get(n->u.compare.comparators, i);
                P2C_AstExpr *left = (i == 0) ? n->u.compare.left : (P2C_AstExpr*)p2c_vec_get(n->u.compare.comparators, i - 1);
                const char *func = "p2c_obj_eq";
                bool negate = false;
                switch (*op) {
                    case OP_LT: func = "p2c_obj_lt"; break;
                    case OP_LE: func = "p2c_obj_le"; break;
                    case OP_EQ: func = "p2c_obj_eq"; break;
                    case OP_NE: func = "p2c_obj_ne"; break;
                    case OP_GT: func = "p2c_obj_gt"; break;
                    case OP_GE: func = "p2c_obj_ge"; break;
                    case OP_IN: func = "p2c_obj_contains"; break;
                    case OP_NOTIN: func = "p2c_obj_contains"; negate = true; break;
                    case OP_IS: func = "p2c_obj_is"; break;
                    case OP_ISNOT: func = "p2c_obj_is"; negate = true; break;
                    default: break;
                }
                bool swap_args = (*op == OP_IN || *op == OP_NOTIN);
                if (i > 0) write_str(cg, " && ");
                if (negate) write_str(cg, "!");
                write_str(cg, "p2c_obj_is_truthy("); write_str(cg, func); write_str(cg, "(");
                if (swap_args) { gen_expr(cg, right); write_str(cg, ", "); gen_expr(cg, left); }
                else           { gen_expr(cg, left); write_str(cg, ", "); gen_expr(cg, right); }
                write_str(cg, "))");
            }
            write_str(cg, ")");
            break;
        }
        case AST_BOOLOP: {
            if (cg->opts.strict_c11) {
                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "and/or requires expression hoisting and is unavailable in strict ISO C11 mode");
                write_str(cg, "&P2C_None");
                break;
            }
            size_t value_count = p2c_vec_len(n->u.boolop.values);
            if (value_count == 0) {
                write_str(cg, "&P2C_None");
                break;
            }
            int bool_id = ++cg->temp_counter;
            write_str(cg, "({ P2C_Object *_p2c_bool_"); emit_usize(cg, (size_t)bool_id); write_str(cg, " = ");
            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.boolop.values, 0));
            write_str(cg, "; ");
            for (size_t i = 1; i < value_count; i++) {
                write_str(cg, "if (");
                if (n->u.boolop.op == OP_AND) write_str(cg, "p2c_obj_is_truthy(_p2c_bool_");
                else write_str(cg, "!p2c_obj_is_truthy(_p2c_bool_");
                emit_usize(cg, (size_t)bool_id); write_str(cg, ")) _p2c_bool_"); emit_usize(cg, (size_t)bool_id); write_str(cg, " = ");
                gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.boolop.values, i));
                write_str(cg, "; ");
            }
            write_str(cg, "_p2c_bool_"); emit_usize(cg, (size_t)bool_id); write_str(cg, "; })");
            break;
        }
        case AST_CALL: {
            P2C_AstExpr *func = n->u.call.func;
            size_t argc = p2c_vec_len(n->u.call.args);
            size_t nkw  = p2c_vec_len(n->u.call.keywords);
            if (func->base.type == AST_ATTRIBUTE && func->base.u.attribute.value &&
                func->base.u.attribute.value->base.type == AST_NAME &&
                strcmp(func->base.u.attribute.value->base.u.name.name, "asyncio") == 0 &&
                strcmp(func->base.u.attribute.attr, "run") == 0 && argc == 1 && nkw == 0) {
                write_str(cg, "p2c_async_run(");
                gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                write_str(cg, ")");
            } else if (func->base.type == AST_NAME) {
                const char *name = func->base.u.name.name;
                if (is_keyword_checked_builtin(name)) {
                    for (size_t ki = 0; ki < nkw; ki++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                        if (!kw || !builtin_keyword_allowed(name, kw->arg)) {
                            char keyword_error[256];
                            snprintf(keyword_error, sizeof(keyword_error), "unsupported keyword argument for builtin function '%s'", name);
                            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, keyword_error);
                        }
                    }
                }
                if (strcmp(name, "print") == 0) {
                    P2C_AstExpr *sep_expr = NULL;
                    P2C_AstExpr *end_expr = NULL;
                    for (size_t ki = 0; ki < nkw; ki++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                        if (!kw || !kw->arg) continue;
                        if (strcmp(kw->arg, "sep") == 0) sep_expr = kw->value;
                        else if (strcmp(kw->arg, "end") == 0) end_expr = kw->value;
                        else codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "print supports only sep and end keyword arguments");
                    }
                    if (!sep_expr && !end_expr) {
                        write_str(cg, "p2c_print_multi("); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                    } else {
                        write_str(cg, "p2c_print_multi_opts("); emit_args_array(cg, n->u.call.args); write_str(cg, ", ");
                        if (sep_expr) gen_expr(cg, sep_expr); else write_str(cg, "&P2C_None");
                        write_str(cg, ", ");
                        if (end_expr) gen_expr(cg, end_expr); else write_str(cg, "&P2C_None");
                        write_str(cg, ")");
                    }
                } else if (strcmp(name, "len") == 0) {
                    write_str(cg, "p2c_obj_from_int(p2c_len(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "&P2C_None");
                    write_str(cg, "))");
                } else if (strcmp(name, "range") == 0) {
                    write_str(cg, "p2c_range(");
                    if (argc == 1) { write_str(cg, "p2c_obj_from_int(0), "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", p2c_obj_from_int(1)"); }
                    else if (argc == 2) { gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ", p2c_obj_from_int(1)"); }
                    else if (argc >= 3) { gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 2)); }
                    else write_str(cg, "p2c_obj_from_int(0), p2c_obj_from_int(0), p2c_obj_from_int(1)");
                    write_str(cg, ")");
                } else if (strcmp(name, "input") == 0) {
                    write_str(cg, "p2c_input()");
                } else if (strcmp(name, "str") == 0) {
                    write_str(cg, "p2c_obj_str(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "&P2C_None");
                    write_str(cg, ")");
                } else if (strcmp(name, "int") == 0 && argc >= 2) {
                    /* int(x, base): 基数付き変換（CPython 準拠）。 */
                    write_str(cg, "p2c_builtin_int_from_base(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                    write_str(cg, ")");
                } else if (strcmp(name, "int") == 0) {
                    write_str(cg, "p2c_obj_from_int(p2c_obj_as_int(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "p2c_obj_from_int(0)");
                    write_str(cg, "))");
                } else if (strcmp(name, "float") == 0) {
                    write_str(cg, "p2c_obj_from_float(p2c_obj_as_float(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "p2c_obj_from_float(0)");
                    write_str(cg, "))");
                } else if (strcmp(name, "bool") == 0) {
                    write_str(cg, "p2c_obj_from_bool(p2c_obj_is_truthy(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "&P2C_None");
                    write_str(cg, "))");
                } else if (strcmp(name, "abs") == 0) {
                    write_str(cg, "p2c_obj_abs(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "&P2C_None");
                    write_str(cg, ")");
                } else if (strcmp(name, "round") == 0) {
                    /* round(x) or round(x, ndigits) */
                    if (argc == 0) { write_str(cg, "p2c_obj_from_int(0)"); break; }
                    if (argc == 1) {
                        write_str(cg, "p2c_builtin_round("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", &P2C_None)");
                    } else {
                        write_str(cg, "p2c_builtin_round("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ")");
                    }
                } else if (strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
                    P2C_AstExpr *key_expr = NULL;
                    bool invalid_kw = false;
                    for (size_t ki = 0; ki < nkw; ki++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                        if (!kw || !kw->arg || strcmp(kw->arg, "key") != 0 || key_expr) invalid_kw = true;
                        else key_expr = kw->value;
                    }
                    if (invalid_kw) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "min/max support only the key keyword argument");
                    }
                    write_str(cg, strcmp(name, "min") == 0 ? "p2c_builtin_min_key(" : "p2c_builtin_max_key(");
                    emit_args_array(cg, n->u.call.args);
                    write_str(cg, ", ");
                    if (key_expr && key_expr->base.type == AST_NAME &&
                        (strcmp(key_expr->base.u.name.name, "len") == 0 || strcmp(key_expr->base.u.name.name, "abs") == 0)) {
                        write_str(cg, "p2c_builtin_function_object(\"");
                        write_str(cg, key_expr->base.u.name.name);
                        write_str(cg, "\")");
                    } else if (key_expr) gen_expr(cg, key_expr); else write_str(cg, "&P2C_None");
                    write_str(cg, ")");
                } else if (strcmp(name, "sum") == 0) {
                    write_str(cg, "p2c_builtin_sum("); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else if (strcmp(name, "ord") == 0 && argc == 1 && nkw == 0) {
                    write_str(cg, "p2c_builtin_ord("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "chr") == 0 && argc == 1 && nkw == 0) {
                    write_str(cg, "p2c_builtin_chr("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "bin") == 0 && argc == 1 && nkw == 0) {
                    write_str(cg, "p2c_builtin_int_base("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", 2u, \"0b\")");
                } else if (strcmp(name, "oct") == 0 && argc == 1 && nkw == 0) {
                    write_str(cg, "p2c_builtin_int_base("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", 8u, \"0o\")");
                } else if (strcmp(name, "hex") == 0 && argc == 1 && nkw == 0) {
                    write_str(cg, "p2c_builtin_int_base("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", 16u, \"0x\")");
                } else if (strcmp(name, "divmod") == 0 && argc == 2 && nkw == 0) {
                    write_str(cg, "p2c_builtin_divmod(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ")");
                } else if (strcmp(name, "pow") == 0 && argc == 3 && nkw == 0) {
                    /* 3引数のpowはモジュラべき乗（整数専用）。2引数は既存のp2c_obj_pow。 */
                    write_str(cg, "p2c_builtin_pow_mod(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 2)); write_str(cg, ")");
                } else if (strcmp(name, "pow") == 0 && argc == 2 && nkw == 0) {
                    /* 2引数のpow(a, b) は ** と同じ p2c_obj_pow へ落とす。
                     * （未対応だと存在しない p2c_user_pow を呼んでコンパイルに失敗していた） */
                    write_str(cg, "p2c_obj_pow(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ")");
                } else if (strcmp(name, "format") == 0 && argc == 2 && nkw == 0) {
                    write_str(cg, "p2c_builtin_format(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ")");
                } else if (strcmp(name, "callable") == 0 && argc == 1 && nkw == 0) {
                    /* callable(x) は、x が「呼び出せる名前」なら静的に True と分かる。
                     * 関数ポインタやビルトイン名を値として評価すると生成Cが壊れるため、
                     * 判定可能な場合は定数へ畳み込み、それ以外だけ実行時判定に委ねる。 */
                    P2C_AstExpr *arg = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0);
                    bool statically_callable = false;
                    if (arg && arg->base.type == AST_NAME &&
                        !map_has_name(cg->module_globals, arg->base.u.name.name)) {
                        const char *aname = arg->base.u.name.name;
                        if (map_has_name(cg->func_args, aname) || map_has_name(cg->known_classes, aname) ||
                            is_builtin_callable_name(aname)) {
                            statically_callable = true;
                        }
                    }
                    if (statically_callable) write_str(cg, "&P2C_True");
                    else {
                        write_str(cg, "p2c_builtin_callable(");
                        gen_expr(cg, arg);
                        write_str(cg, ")");
                    }
                } else if (strcmp(name, "sorted") == 0) {
                    P2C_AstExpr *key_expr = NULL;
                    P2C_AstExpr *rev_expr = NULL;
                    bool invalid_kw = argc > 1;
                    for (size_t ki = 0; ki < nkw; ki++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                        if (!kw || !kw->arg) invalid_kw = true;
                        else if (strcmp(kw->arg, "key") == 0 && !key_expr) key_expr = kw->value;
                        else if (strcmp(kw->arg, "reverse") == 0 && !rev_expr) rev_expr = kw->value;
                        else invalid_kw = true;
                    }
                    if (argc == 0) invalid_kw = true;
                    if (invalid_kw) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "sorted supports one iterable and only key/reverse keyword arguments");
                    }
                    write_str(cg, "p2c_builtin_sorted_key(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "&P2C_None");
                    write_str(cg, ", ");
                    if (key_expr && key_expr->base.type == AST_NAME &&
                        (strcmp(key_expr->base.u.name.name, "len") == 0 || strcmp(key_expr->base.u.name.name, "abs") == 0)) {
                        write_str(cg, "p2c_builtin_function_object(\"");
                        write_str(cg, key_expr->base.u.name.name);
                        write_str(cg, "\")");
                    } else if (key_expr) gen_expr(cg, key_expr); else write_str(cg, "&P2C_None");
                    write_str(cg, ", ");
                    if (rev_expr) gen_expr(cg, rev_expr); else write_str(cg, "&P2C_False");
                    write_str(cg, ")");
                } else if (strcmp(name, "enumerate") == 0 && (argc == 1 || argc == 2)) {
                    write_str(cg, "p2c_builtin_enumerate(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", ");
                    if (argc == 2) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); else write_str(cg, "p2c_obj_from_int(0)");
                    write_str(cg, ")");
                } else if (strcmp(name, "zip") == 0) {
                    write_str(cg, "p2c_builtin_zip("); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else if (strcmp(name, "type") == 0) {
                    write_str(cg, "p2c_builtin_type(");
                    if (argc) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "&P2C_None");
                    write_str(cg, ")");
                } else if (strcmp(name, "isinstance") == 0 && argc >= 2) {
                    P2C_AstExpr *type_arg = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1);
                    /* isinstance(x, (int, str, ...)) のタプルケース */
                    if (type_arg->base.type == AST_TUPLE || type_arg->base.type == AST_LIST) {
                        P2C_Vector *types = (type_arg->base.type == AST_TUPLE)
                            ? type_arg->base.u.tuple.elts : type_arg->base.u.list.elts;
                        size_t ntypes = p2c_vec_len(types);
                        if (ntypes == 0) { write_str(cg, "p2c_obj_from_bool(false)"); break; }
                        write_str(cg, "p2c_obj_from_bool(");
                        for (size_t ti = 0; ti < ntypes; ti++) {
                            P2C_AstExpr *te = (P2C_AstExpr*)p2c_vec_get(types, ti);
                            if (ti > 0) write_str(cg, " || ");
                            const char *tn = (te->base.type == AST_NAME) ? te->base.u.name.name : NULL;
                            const char *ck = NULL;
                            if (tn) {
                                if (strcmp(tn,"int")==0) ck="p2c_obj_is_int";
                                else if (strcmp(tn,"float")==0) ck="p2c_obj_is_float";
                                else if (strcmp(tn,"str")==0) ck="p2c_obj_is_str";
                                else if (strcmp(tn,"bool")==0) ck="p2c_obj_is_bool";
                                else if (strcmp(tn,"list")==0) ck="p2c_obj_is_list";
                                else if (strcmp(tn,"dict")==0) ck="p2c_obj_is_dict";
                                else if (strcmp(tn,"set")==0 || strcmp(tn,"frozenset")==0) ck="p2c_obj_is_set";
                                else if (strcmp(tn,"tuple")==0) ck="p2c_obj_is_tuple";
                            }
                            if (ck) { write_str(cg, ck); write_str(cg, "("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")"); }
                            else { write_str(cg, "false"); }
                        }
                        write_str(cg, ")");
                    } else {
                        const char *type_name = (type_arg->base.type == AST_NAME) ? type_arg->base.u.name.name : NULL;
                        const char *checker = NULL;
                        if (type_name) {
                            if (strcmp(type_name, "int") == 0) checker = "p2c_obj_is_int";
                            else if (strcmp(type_name, "float") == 0) checker = "p2c_obj_is_float";
                            else if (strcmp(type_name, "str") == 0) checker = "p2c_obj_is_str";
                            else if (strcmp(type_name, "bool") == 0) checker = "p2c_obj_is_bool";
                            else if (strcmp(type_name, "list") == 0) checker = "p2c_obj_is_list";
                            else if (strcmp(type_name, "dict") == 0) checker = "p2c_obj_is_dict";
                            else if (strcmp(type_name, "set") == 0 || strcmp(type_name, "frozenset") == 0) checker = "p2c_obj_is_set";
                            else if (strcmp(type_name, "tuple") == 0) checker = "p2c_obj_is_tuple";
                        }
                        if (checker) {
                            write_str(cg, "p2c_obj_from_bool("); write_str(cg, checker); write_str(cg, "(");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                            write_str(cg, "))");
                        } else if (type_name && map_has_name(cg->known_classes, type_name)) {
                            write_str(cg, "p2c_obj_from_bool(p2c_isinstance_of_class(");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                            write_str(cg, ", \""); write_str(cg, type_name); write_str(cg, "\"))");
                        } else {
                            /* クラス名がbare nameでない場合（Outer.Innerのような属性経由や、
                             * クラスオブジェクトを保持した変数）は第2引数をクラスオブジェクト
                             * として評価して判定する。以前はここが常にfalseになり、
                             * isinstance(x, ns.Class) が黙ってFalseを返していた。 */
                            write_str(cg, "p2c_obj_from_bool(p2c_isinstance_of_object(");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                            write_str(cg, ", ");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                            write_str(cg, "))");
                        }
                    }
                } else if (strcmp(name, "any") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_any("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "all") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_all("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "map") == 0 && argc == 2) {
                    write_str(cg, "p2c_builtin_map("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ")");
                } else if (strcmp(name, "filter") == 0 && argc == 2) {
                    write_str(cg, "p2c_builtin_filter("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); write_str(cg, ")");
                } else if (strcmp(name, "dict") == 0 && nkw > 0 && cg->opts.strict_c11) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "dict() keyword arguments are unavailable in strict ISO C11 mode");
                    write_str(cg, "&P2C_None");
                } else if (strcmp(name, "dict") == 0 && nkw > 0) {
                    int dict_id = ++cg->temp_counter;
                    write_str(cg, "({ P2C_Object *_p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, " = ");
                    if (argc == 1) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); else write_str(cg, "p2c_dict_new()");
                    write_str(cg, ";");
                    if (argc == 1) { write_str(cg, " _p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, " = p2c_builtin_dict(_p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, ");"); }
                    for (size_t ki = 0; ki < nkw; ki++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                        if (!kw) continue;
                        if (kw->arg) {
                            write_str(cg, " p2c_dict_set(_p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, ", p2c_obj_from_str(\""); write_str(cg, kw->arg); write_str(cg, "\"), "); gen_expr(cg, kw->value); write_str(cg, ");");
                        } else {
                            write_str(cg, " p2c_dict_update(_p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, ", "); gen_expr(cg, kw->value); write_str(cg, ");");
                        }
                    }
                    write_str(cg, " _p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, "; })");
                } else if (strcmp(name, "dict") == 0 && argc == 0) {
                    write_str(cg, "p2c_dict_new()");
                } else if (strcmp(name, "dict") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_dict("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if ((strcmp(name, "set") == 0 || strcmp(name, "frozenset") == 0) && argc == 0) {
                    write_str(cg, "p2c_set_new()");
                } else if ((strcmp(name, "set") == 0 || strcmp(name, "frozenset") == 0) && argc == 1) {
                    write_str(cg, "p2c_builtin_set("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "list") == 0 && argc == 0) {
                    write_str(cg, "p2c_list_new()");
                } else if (strcmp(name, "list") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_list("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "tuple") == 0 && argc == 0) {
                    write_str(cg, "p2c_tuple_new(0)");
                } else if (strcmp(name, "tuple") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_tuple("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "reversed") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_reversed("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "hasattr") == 0 && argc == 2) {
                    write_str(cg, "p2c_obj_from_bool(p2c_hasattr(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", p2c_obj_as_str(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                    write_str(cg, ")))");
                } else if (strcmp(name, "getattr") == 0 && (argc == 2 || argc == 3)) {
                    write_str(cg, "p2c_getattr_default(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", p2c_obj_as_str(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                    write_str(cg, "), ");
                    if (argc == 3) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 2)); else write_str(cg, "NULL");
                    write_str(cg, ")");
                } else if (strcmp(name, "setattr") == 0 && argc == 3 && cg->opts.strict_c11) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "setattr() expression form is unavailable in strict ISO C11 mode");
                    write_str(cg, "&P2C_None");
                } else if (strcmp(name, "setattr") == 0 && argc == 3) {
                    /* GCC文式({ ...; 値; })を使う: (void式, &P2C_None) という
                     * コンマ式にすると、文として単独で使われたときに
                     * -Wunused-value（コンマ式の右辺が捨てられる）で
                     * -Werror下ではコンパイルエラーになってしまうため。 */
                    write_str(cg, "({ p2c_setattr(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", p2c_obj_as_str(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                    write_str(cg, "), ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 2));
                    write_str(cg, "); &P2C_None; })");
                } else if (strcmp(name, "delattr") == 0 && argc == 2 && cg->opts.strict_c11) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "delattr() expression form is unavailable in strict ISO C11 mode");
                    write_str(cg, "&P2C_None");
                } else if (strcmp(name, "delattr") == 0 && argc == 2) {
                    write_str(cg, "({ p2c_delattr(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", p2c_obj_as_str(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                    write_str(cg, ")); &P2C_None; })");
                } else if (strcmp(name, "repr") == 0 && argc == 1) {
                    write_str(cg, "p2c_obj_repr("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "iter") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_iter("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "next") == 0 && (argc == 1 || argc == 2)) {
                    if (argc == 1) {
                        write_str(cg, "p2c_builtin_next("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                    } else if (cg->opts.strict_c11) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "next(iterator, default) is unavailable in strict ISO C11 mode");
                        write_str(cg, "&P2C_None");
                    } else {
                        /* next(it, default): StopIterationをtry相当で捕まえてdefaultにフォールバック
                         * するのは文単位のtry/exceptが必要で式の中では組めないため、GCCの文式拡張で
                         * 実現する（既にlambdaやcomprehensionで同拡張に依存している既存方針を踏襲）。 */
                        write_str(cg, "({ P2C_Object *_p2c_next_it = ");
                        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                        write_str(cg, "; P2C_Object *_p2c_next_res; P2C_ExceptFrame _p2c_next_ef; _p2c_next_ef.prev = p2c_exc_stack; p2c_exc_stack = &_p2c_next_ef; "
                                      "if (P2C_SETJMP(_p2c_next_ef.env) == 0) { _p2c_next_res = p2c_builtin_next(_p2c_next_it); p2c_exc_stack = _p2c_next_ef.prev; } "
                                      "else { p2c_exc_stack = _p2c_next_ef.prev; _p2c_next_res = ");
                        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                        write_str(cg, "; } _p2c_next_res; })");
                    }
                } else if (map_has_name(cg->decorated_names, name)) {
                    if (nkw != 0) codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "keyword arguments for decorated functions are not supported yet");
                    write_str(cg, "p2c_call(_p2c_decorated_"); write_ident(cg, name); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else if (nested_class_alias(cg, name)) {
                    /* クラス本体内でのネストクラス呼び出し（例: factory = Inner()）は
                     * 前置済みC名のクラスオブジェクトを呼び出す。 */
                    if (!cg->in_class_body) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                            "nested class names are not visible inside method bodies; refer to the class through its enclosing class (Outer.Inner)");
                    }
                    write_str(cg, "p2c_call("); write_str(cg, nested_class_alias(cg, name)); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else if (map_has_name(cg->known_classes, name) || is_declared(cg, name)) {
                    write_str(cg, "p2c_call("); write_ident(cg, name); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else {
                    /* 呼び出し側の位置引数・キーワード引数・省略された引数の
                     * デフォルト値を、関数の実際のパラメータ順序に解決してから
                     * 呼び出す。例: def f(a, b=2): ... の f(b=5, a=1) や f(1) を
                     * 正しい順序 f(1, 5) / f(1, 2) として生成する。
                     * （以前はキーワード引数の解決が一切なく、呼び出し時の
                     * 構文自体がパースエラーになっていた。）
                     * 関数が *args / **kwargs を持つ場合、名前付きパラメータに
                     * 収まらない位置引数・キーワード引数はそれぞれタプル・辞書に
                     * まとめてCの隠れた追加引数として渡す。呼び出し1箇所ごとに
                     * 個数はコンパイル時に確定しているので、実行時の可変長
                     * 呼び出し機構は不要（生成されるC関数は普通の固定引数関数）。 */
                    P2C_AstFunctionDef *fdef = (P2C_AstFunctionDef*)p2c_map_get(cg->func_args, name);
                    P2C_Vector *params = fdef ? fdef->args : NULL;
                    size_t total_params = params ? p2c_vec_len(params) : 0;
                    size_t posonly_count = fdef ? (size_t)fdef->posonly_count : 0;
                    size_t kwonly_start = fdef ? (size_t)fdef->kwonly_start : total_params;
                    const char *vararg = fdef ? fdef->vararg : NULL;
                    const char *kwarg = fdef ? fdef->kwarg : NULL;
                    size_t call_nkw = p2c_vec_len(n->u.call.keywords);
                    /* *args アンパック (f(*lst)) が含まれるか検査 */
                    bool has_starred = false;
                    for (size_t i = 0; i < argc; i++) {
                        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
                        if (a && a->base.type == AST_STARRED) { has_starred = true; break; }
                    }
                    /* **kwargs アンパック (f(**d)) が含まれるか検査 */
                    bool has_dstar = false;
                    for (size_t k = 0; k < call_nkw; k++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
                        if (kw && kw->arg == NULL) { has_dstar = true; break; }
                    }
                    write_ident(cg, name); write_str(cg, "(");
                    if (!fdef || has_starred || has_dstar) {
                        /* 未知の関数 or *args・**kwargsアンパックあり。
                         * ケース1: f(*lst) のみ & 既知関数 -> インデックスで各引数を展開。
                         *          vararg がある場合は残りをタプルとして渡す。
                         * ケース2: f(**d) のみ & 既知関数 -> 辞書から各パラメータ名でget。
                         * それ以外: そのまま渡す（ベストエフォート）。 */
                        if (fdef && has_starred && !has_dstar && argc == 1 && nkw == 0) {
                            /* f(*lst) 単純展開 */
                            P2C_AstExpr *star_expr = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0);
                            if (star_expr && star_expr->base.type == AST_STARRED) {
                                P2C_AstExpr *src = star_expr->base.u.starred.value;
                                bool wrote_param = false;
                                /* 固定引数部分: seq[0..kwonly_start-1] */
                                for (size_t i = 0; i < kwonly_start; i++) {
                                    if (wrote_param) write_str(cg, ", ");
                                    wrote_param = true;
                                    write_str(cg, "p2c_subscript_get(");
                                    gen_expr(cg, src);
                                    char ibuf[32]; snprintf(ibuf, sizeof(ibuf), "%zu", i);
                                    write_str(cg, ", p2c_obj_from_int("); write_str(cg, ibuf); write_str(cg, "))");
                                }
                                /* vararg: 残りをスライスしてタプルにまとめる */
                                if (vararg) {
                                    if (wrote_param) write_str(cg, ", ");
                                    wrote_param = true;
                                    /* p2c_obj_slice(seq, start, None, None) で [kwonly_start:] を取り出す */
                                    write_str(cg, "p2c_builtin_tuple(p2c_obj_slice(");
                                    gen_expr(cg, src);
                                    write_str(cg, ", p2c_obj_from_int(");
                                    emit_usize(cg, kwonly_start);
                                    write_str(cg, "), &P2C_None, &P2C_None))");
                                }
                                if (kwarg) {
                                    if (wrote_param) write_str(cg, ", ");
                                    write_str(cg, "p2c_dict_from_pairs(NULL, NULL, 0)");
                                }
                            } else { gen_expr(cg, star_expr); }
                        } else if (fdef && has_dstar && !has_starred && argc == 0 && nkw == 1) {
                            /* f(**d) 単純展開: 辞書から各パラメータ名で p2c_dict_get */
                            P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, 0);
                            if (kw && kw->arg == NULL && kw->value) {
                                P2C_AstExpr *src = kw->value;
                                bool wrote_param = false;
                                for (size_t i = 0; i < total_params; i++) {
                                    if (wrote_param) write_str(cg, ", ");
                                    wrote_param = true;
                                    P2C_AstArg *pa = (P2C_AstArg*)p2c_vec_get(params, i);
                                    /* d.get(name, default) */
                                    write_str(cg, "p2c_dict_get_with_default(");
                                    gen_expr(cg, src);
                                    write_str(cg, ", p2c_obj_from_str(\"");
                                    write_str(cg, pa->name);
                                    write_str(cg, "\"), ");
                                    if (pa->default_val) gen_expr(cg, pa->default_val);
                                    else write_str(cg, "&P2C_None");
                                    write_str(cg, ")");
                                }
                                if (vararg) {
                                    if (wrote_param) write_str(cg, ", ");
                                    write_str(cg, "p2c_tuple_from_array(NULL, 0)");
                                }
                                if (kwarg) {
                                    if (wrote_param) write_str(cg, ", ");
                                    /* 名前付きパラメータとして個別に取り出した名前を除いた
                                     * 残りをkwargsとして渡す。以前はここが常に空の辞書を
                                     * 生成しており、呼び出し元が渡した**dの中身が
                                     * （名前付きパラメータに一致しない分もまとめて）
                                     * 静かにすべて失われていた。 */
                                    write_str(cg, "p2c_dict_exclude_keys(");
                                    gen_expr(cg, src);
                                    write_str(cg, ", (const char*[]){");
                                    for (size_t i = 0; i < total_params; i++) {
                                        if (i) write_str(cg, ", ");
                                        P2C_AstArg *pa = (P2C_AstArg*)p2c_vec_get(params, i);
                                        write_str(cg, "\""); write_str(cg, pa->name); write_str(cg, "\"");
                                    }
                                    if (total_params == 0) write_str(cg, "NULL"); /* 空配列リテラルはC11で不可のためNULLで代用(要素数0なので参照されない) */
                                    char nbuf3[32]; snprintf(nbuf3, sizeof(nbuf3), "}, %zu)", total_params);
                                    write_str(cg, nbuf3);
                                }
                            }
                        } else {
                            /* 複数混合 or unknown: ベストエフォートでそのまま渡す */
                            bool wrote2 = false;
                            for (size_t i = 0; i < argc; i++) {
                                P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
                                if (!a) continue;
                                if (wrote2) write_str(cg, ", ");
                                wrote2 = true;
                                if (a->base.type == AST_STARRED) gen_expr(cg, a->base.u.starred.value);
                                else gen_expr(cg, a);
                            }
                        }
                    } else {
                        bool wrote = false;
                        for (size_t i = 0; i < total_params; i++) {
                            if (wrote) write_str(cg, ", ");
                            wrote = true;
                            P2C_AstArg *pa = (P2C_AstArg*)p2c_vec_get(params, i);
                            /* キーワード専用引数（*args/裸の* より後ろ）は位置では絶対に埋まらない */
                            if (i < kwonly_start && i < argc) {
                                gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i));
                                continue;
                            }
                            P2C_AstExpr *kw_val = NULL;
                            for (size_t k = 0; k < nkw; k++) {
                                P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
                                if (kw && pa && kw->arg && strcmp(kw->arg, pa->name) == 0) { kw_val = kw->value; break; }
                            }
                            if (kw_val) {
                                if (i < posonly_count) {
                                    write_str(cg, "p2c_posonly_keyword_error(\""); write_str(cg, name); write_str(cg, "\", \""); write_str(cg, pa->name); write_str(cg, "\")");
                                } else gen_expr(cg, kw_val);
                            }
                            else if (pa && pa->default_val) gen_expr(cg, pa->default_val);
                            else write_str(cg, "&P2C_None"); /* 必須引数が省略されている場合は元のPython側が不正 */
                        }
                        /* *args: 名前付き引数の枠(kwonly_start個)を超える位置引数はタプルにまとめる */
                        if (vararg) {
                            if (wrote) write_str(cg, ", ");
                            wrote = true;
                            if (argc > kwonly_start) {
                                write_str(cg, "p2c_tuple_from_array((P2C_Object*[]){");
                                for (size_t i = kwonly_start; i < argc; i++) {
                                    if (i > kwonly_start) write_str(cg, ", ");
                                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i));
                                }
                                char nbuf[32];
                                snprintf(nbuf, sizeof(nbuf), "%zu", argc - kwonly_start);
                                write_str(cg, "}, "); write_str(cg, nbuf); write_str(cg, ")");
                            } else {
                                write_str(cg, "p2c_tuple_from_array(NULL, 0)");
                            }
                        }
                        /* **kwargs: どの名前付きパラメータにも一致しないキーワード引数を辞書にまとめる */
                        if (kwarg) {
                            if (wrote) write_str(cg, ", ");
                            bool *matched_kw = nkw ? (bool*)p2c_alloc(cg->alloc, nkw * sizeof(bool)) : NULL;
                            size_t extra_count = 0;
                            for (size_t k = 0; k < nkw; k++) {
                                P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
                                bool m = false;
                                if (kw && kw->arg) {
                                    for (size_t i = 0; i < total_params; i++) {
                                        P2C_AstArg *pa = (P2C_AstArg*)p2c_vec_get(params, i);
                                        if (pa && strcmp(kw->arg, pa->name) == 0) { m = true; break; }
                                    }
                                }
                                if (matched_kw) matched_kw[k] = m;
                                if (!m) extra_count++;
                            }
                            if (extra_count > 0) {
                                write_str(cg, "p2c_dict_from_pairs((P2C_Object*[]){");
                                bool first = true;
                                for (size_t k = 0; k < nkw; k++) {
                                    if (matched_kw && matched_kw[k]) continue;
                                    P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
                                    if (!first) write_str(cg, ", ");
                                    first = false;
                                    write_str(cg, "p2c_obj_from_str(\""); write_str(cg, kw->arg); write_str(cg, "\")");
                                }
                                write_str(cg, "}, (P2C_Object*[]){");
                                first = true;
                                for (size_t k = 0; k < nkw; k++) {
                                    if (matched_kw && matched_kw[k]) continue;
                                    P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
                                    if (!first) write_str(cg, ", ");
                                    first = false;
                                    gen_expr(cg, kw->value);
                                }
                                char nbuf2[32];
                                snprintf(nbuf2, sizeof(nbuf2), "%zu", extra_count);
                                write_str(cg, "}, "); write_str(cg, nbuf2); write_str(cg, ")");
                            } else {
                                write_str(cg, "p2c_dict_from_pairs(NULL, NULL, 0)");
                            }
                            if (matched_kw) p2c_free(cg->alloc, matched_kw);
                        }
                    }
                    write_str(cg, ")");
                }
            } else if (func->base.type == AST_ATTRIBUTE && func->base.u.attribute.value &&
                       func->base.u.attribute.value->base.type == AST_NAME &&
                       strcmp(func->base.u.attribute.value->base.u.name.name, "dict") == 0 &&
                       strcmp(func->base.u.attribute.attr, "fromkeys") == 0 &&
                       (argc == 1 || argc == 2) && nkw == 0) {
                write_str(cg, "p2c_dict_fromkeys(");
                gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                write_str(cg, ", ");
                if (argc == 2) gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1)); else write_str(cg, "&P2C_None");
                write_str(cg, ")");
            } else if (func->base.type == AST_ATTRIBUTE && func->base.u.attribute.value &&
                       func->base.u.attribute.value->base.type == AST_CALL &&
                       func->base.u.attribute.value->base.u.call.func &&
                       func->base.u.attribute.value->base.u.call.func->base.type == AST_NAME &&
                       strcmp(func->base.u.attribute.value->base.u.call.func->base.u.name.name, "super") == 0) {
                /* super().method(args) : Pythonの継承解決を模倣し、コード生成時に
                 * 「現在のクラスの基底クラス（さらにその基底、…）を、実際に
                 * このメソッドを定義しているクラスが見つかるまで辿った上で、
                 * そのクラスの実装関数を self を束縛して直接呼び出す」C コードに
                 * 変換する（Pythonのsuper()が返す一時的なプロキシオブジェクトは
                 * 生成しない。単一継承のみ対応 — 多重継承のMRO解決は非対応）。
                 * 以前はsuper()自体が未定義関数呼び出しとして生成され、常に
                 * コンパイルエラーになっていた。 */
                /* super().method(args) は、インスタンスの型のMRO上で「このメソッドを
                 * 書いたクラスの次」から解決するようランタイムへ委譲する。
                 * コード生成時に基底チェーンを静的に辿る近似では、多重継承の
                 * ダイヤモンド（class D(B, C)、B(A)、C(A)でBのsuper()はCを指す）で
                 * CPythonと異なる実装を呼んでしまうため。 */
                P2C_AstExpr *super_call = func->base.u.attribute.value;
                const char *method_name = func->base.u.attribute.attr;
                if (!cg->current_class) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                      "super() is supported only inside a class method body");
                    write_str(cg, "((P2C_Object*)0)");
                } else if (p2c_vec_len(super_call->base.u.call.args) > 0) {
                    /* super(Class, self) の2引数形は未対応（0引数形のみ対応）。 */
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                      "only the zero-argument super() form is supported");
                    write_str(cg, "((P2C_Object*)0)");
                } else {
                    if (nkw > 0) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                          "keyword arguments for super() method calls are not supported yet");
                    }
                    write_str(cg, "p2c_super_call_attr(self, "); write_str(cg, cg->current_class);
                    write_str(cg, ", \""); write_str(cg, method_name); write_str(cg, "\", ");
                    if (argc == 0) write_str(cg, "NULL, 0");
                    else {
                        write_str(cg, "(P2C_Object*[]){");
                        for (size_t ai = 0; ai < argc; ai++) {
                            if (ai) write_str(cg, ", ");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, ai));
                        }
                        write_str(cg, "}, "); emit_usize(cg, argc);
                    }
                    write_str(cg, ")");
                }
            } else if (func->base.type == AST_ATTRIBUTE) {
                if (nkw == 0) {
                    write_str(cg, "p2c_call_attr("); gen_expr(cg, func->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, func->base.u.attribute.attr); write_str(cg, "\", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else {
                    write_str(cg, "p2c_call_attr_kw("); gen_expr(cg, func->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, func->base.u.attribute.attr); write_str(cg, "\", "); emit_args_array(cg, n->u.call.args); write_str(cg, ", (const char*[]){");
                    for (size_t ki = 0; ki < nkw; ki++) { if (ki) write_str(cg, ", "); P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki); if (kw && kw->arg) { write_str(cg, "\""); write_str(cg, kw->arg); write_str(cg, "\""); } else write_str(cg, "NULL"); }
                    write_str(cg, "}, (P2C_Object*[]){");
                    for (size_t ki = 0; ki < nkw; ki++) { if (ki) write_str(cg, ", "); P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki); if (kw) gen_expr(cg, kw->value); else write_str(cg, "&P2C_None"); }
                    write_str(cg, "}, "); emit_usize(cg, nkw); write_str(cg, ")");
                }
            } else {
                write_str(cg, "p2c_call("); gen_expr(cg, func); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
            }
            break;
        }
        case AST_ATTRIBUTE:
            write_str(cg, "p2c_getattr("); gen_expr(cg, n->u.attribute.value); write_str(cg, ", \""); write_str(cg, n->u.attribute.attr); write_str(cg, "\")");
            break;
        case AST_SUBSCRIPT:
            write_str(cg, "p2c_subscript_get("); gen_expr(cg, n->u.subscript.value); write_str(cg, ", "); gen_expr(cg, n->u.subscript.slice); write_str(cg, ")");
            break;
        case AST_IFEXP:
            write_str(cg, "(p2c_obj_is_truthy("); gen_expr(cg, n->u.ifexp.test); write_str(cg, ") ? "); gen_expr(cg, n->u.ifexp.body); write_str(cg, " : "); gen_expr(cg, n->u.ifexp.orelse); write_str(cg, ")");
            break;
        case AST_NAMED_EXPR:
            write_str(cg, "("); write_ident(cg, n->u.named_expr.target->base.u.name.name); write_str(cg, " = "); gen_expr(cg, n->u.named_expr.value); write_str(cg, ")");
            break;
        case AST_GENERATOR_EXPRESSION:
            gen_generator_expression(cg, expr);
            break;
        case AST_COMPREHENSION: {
            if (cg->opts.strict_c11) {
                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "comprehensions are unavailable in strict ISO C11 mode");
                write_str(cg, "&P2C_None");
                break;
            }
            int comp_id = ++cg->lambda_counter;
            char list_var[64];
            const bool set_result = n->u.comprehension.set_result;
            const bool dict_result = n->u.comprehension.dict_key != NULL;
            snprintf(list_var, sizeof(list_var), dict_result ? "_p2c_compdict_%d" : (set_result ? "_p2c_compset_%d" : "_p2c_complist_%d"), comp_id);
            write_str(cg, "({ P2C_Object *"); write_str(cg, list_var); write_str(cg, dict_result ? " = p2c_dict_new(); " : (set_result ? " = p2c_set_new(); " : " = p2c_list_new(); "));
            gen_comprehension_body(cg, comp_id, n->u.comprehension.generators, 0, n->u.comprehension.elt, n->u.comprehension.dict_key, list_var, set_result ? "p2c_set_add" : "p2c_list_append");
            write_str(cg, " "); write_str(cg, list_var); write_str(cg, "; })");
            break;
        }
        case AST_LAMBDA: {
            P2C_Map *captures = cg->declared_vars;
            P2C_Map *saved_declared = cg->declared_vars;
            P2C_Map *saved_env_names = cg->closure_env_names;
            const char *saved_env_var = cg->closure_env_var;
            P2C_String *saved_current = cg->current;
            int saved_indent = cg->indent_level;
            char entry_name[64];
            size_t capture_count = 0;
            int id = ++cg->lambda_counter;
            P2C_Vector *largs = n->u.lambda.args;
            snprintf(entry_name, sizeof(entry_name), "_p2c_lambda_entry_%d", id);
            /* ネストしたラムダの定義は forward の末尾（待ち行列の連結時）に現れるため、
             * 先に使っても解決できるよう前方宣言を header へ置く。 */
            p2c_str_append(cg->header, "static P2C_Object *");
            p2c_str_append(cg->header, entry_name);
            p2c_str_append(cg->header, "(P2C_Object *env, P2C_Object **args, size_t nargs);\n");
            if (captures) for (size_t i = 0; i < captures->bucket_count; i++) for (P2C_MapEntry *entry = captures->buckets[i]; entry; entry = entry->next) capture_count++;
            /* forward へ書き込み中に現れたラムダ定義を forward へ直接書くと、
             * 生成中の外側の定義に割り込んで壊れたCになる。
             * その場合は別バッファへ書き、最終組み立てで forward の末尾へ連結する。 */
            const bool nested_def = (cg->current != cg->toplevel && cg->current != cg->body &&
                                     cg->current != cg->header);
            P2C_String *def_buf = nested_def ? p2c_str_new(cg->alloc) : cg->forward;
            if (!def_buf) def_buf = cg->forward;
            cg->current = def_buf;
            cg->indent_level = 0;
            write_str(cg, "static P2C_Object *"); write_str(cg, entry_name); write_str(cg, "(P2C_Object *env, P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
            indent(cg); write_str(cg, "(void)env; (void)args; (void)nargs;"); write_newline(cg);
            cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
            for (size_t i = 0; i < p2c_vec_len(largs); i++) {
                P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(largs, i);
                indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, arg->name); write_str(cg, " = args["); emit_usize(cg, i); write_str(cg, "];"); write_newline(cg);
                remember_declared(cg, arg->name);
            }
            cg->closure_env_names = captures;
            cg->closure_env_var = "env";
            indent(cg); write_str(cg, "return "); gen_expr(cg, n->u.lambda.body); write_str(cg, ";"); write_newline(cg);
            p2c_map_free(cg->declared_vars);
            cg->declared_vars = saved_declared;
            cg->closure_env_names = saved_env_names;
            cg->closure_env_var = saved_env_var;
            pop_indent(cg); write_line(cg, "}"); write_newline(cg);
            cg->current = saved_current;
            cg->indent_level = saved_indent;
            if (nested_def) {
                if (!cg->deferred_defs) cg->deferred_defs = p2c_vec_new(cg->alloc, NULL);
                if (cg->deferred_defs) (void)p2c_vec_push(cg->deferred_defs, def_buf);
            }
            write_str(cg, "p2c_closure_new(\"<lambda>\", "); write_str(cg, entry_name); write_str(cg, ", ");
            if (capture_count == 0) {
                write_str(cg, "p2c_dict_from_pairs(NULL, NULL, 0)");
            } else {
                write_str(cg, "p2c_dict_from_pairs((P2C_Object*[]){");
                bool first = true;
                for (size_t i = 0; i < captures->bucket_count; i++) for (P2C_MapEntry *entry = captures->buckets[i]; entry; entry = entry->next) {
                    if (!first) write_str(cg, ", ");
                    first = false;
                    write_str(cg, "p2c_obj_from_str(\""); write_str(cg, (const char*)entry->key); write_str(cg, "\")");
                }
                write_str(cg, "}, (P2C_Object*[]){");
                first = true;
                for (size_t i = 0; i < captures->bucket_count; i++) for (P2C_MapEntry *entry = captures->buckets[i]; entry; entry = entry->next) {
                    if (!first) write_str(cg, ", ");
                    first = false;
                    if (map_has_name(cg->cell_names, (const char*)entry->key)) { write_str(cg, "_p2c_cell_"); write_ident(cg, (const char*)entry->key); }
                    else { write_str(cg, "p2c_cell_new("); write_ident(cg, (const char*)entry->key); write_str(cg, ")"); }
                }
                write_str(cg, "}, "); emit_usize(cg, capture_count); write_str(cg, ")");
            }
            write_str(cg, ")");
            break;
        }
        case AST_LIST:
            write_str(cg, "p2c_list_from_array(");
            if (p2c_vec_len(n->u.list.elts) == 0) write_str(cg, "NULL, 0");
            else {
                write_str(cg, "(P2C_Object*[]){");
                for (size_t i = 0; i < p2c_vec_len(n->u.list.elts); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.list.elts, i)); }
                write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(n->u.list.elts));
            }
            write_str(cg, ")");
            break;
        case AST_SET:
            write_str(cg, "p2c_set_from_array(");
            if (p2c_vec_len(n->u.list.elts) == 0) write_str(cg, "NULL, 0");
            else {
                write_str(cg, "(P2C_Object*[]){");
                for (size_t i = 0; i < p2c_vec_len(n->u.list.elts); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.list.elts, i)); }
                write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(n->u.list.elts));
            }
            write_str(cg, ")");
            break;
        case AST_TUPLE:
            write_str(cg, "p2c_tuple_from_array(");
            if (p2c_vec_len(n->u.tuple.elts) == 0) write_str(cg, "NULL, 0");
            else {
                write_str(cg, "(P2C_Object*[]){");
                for (size_t i = 0; i < p2c_vec_len(n->u.tuple.elts); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.tuple.elts, i)); }
                write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(n->u.tuple.elts));
            }
            write_str(cg, ")");
            break;
        case AST_DICT: {
            bool has_unpack = false;
            for (size_t i = 0; i < p2c_vec_len(n->u.dict.keys); i++) {
                if (!p2c_vec_get(n->u.dict.keys, i)) { has_unpack = true; break; }
            }
            if (!has_unpack) {
                write_str(cg, "p2c_dict_from_pairs(");
                if (p2c_vec_len(n->u.dict.keys) == 0) write_str(cg, "NULL, NULL, 0");
                else {
                    write_str(cg, "(P2C_Object*[]){");
                    for (size_t i = 0; i < p2c_vec_len(n->u.dict.keys); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.dict.keys, i)); }
                    write_str(cg, "}, (P2C_Object*[]){");
                    for (size_t i = 0; i < p2c_vec_len(n->u.dict.values); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.dict.values, i)); }
                    write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(n->u.dict.keys));
                }
                write_str(cg, ")");
            } else {
                int temp = ++cg->temp_counter;
                write_str(cg, "({ P2C_Object *_p2c_dict_"); emit_usize(cg, (size_t)temp); write_str(cg, " = p2c_dict_new(); ");
                for (size_t i = 0; i < p2c_vec_len(n->u.dict.keys); i++) {
                    P2C_AstExpr *key = (P2C_AstExpr*)p2c_vec_get(n->u.dict.keys, i);
                    P2C_AstExpr *value = (P2C_AstExpr*)p2c_vec_get(n->u.dict.values, i);
                    if (key) {
                        write_str(cg, "p2c_dict_set(_p2c_dict_"); emit_usize(cg, (size_t)temp); write_str(cg, ", "); gen_expr(cg, key); write_str(cg, ", "); gen_expr(cg, value); write_str(cg, "); ");
                    } else {
                        write_str(cg, "p2c_dict_update(_p2c_dict_"); emit_usize(cg, (size_t)temp); write_str(cg, ", "); gen_expr(cg, value); write_str(cg, "); ");
                    }
                }
                write_str(cg, "_p2c_dict_"); emit_usize(cg, (size_t)temp); write_str(cg, "; })");
            }
            break;
        }
        default:
            /* 未対応の式。--fallback ではランタイムスタブ、既定では変換エラー。 */
            emit_unsupported_construct(cg, n, "expression", true);
            break;
    }
}

static void gen_assign_target(P2C_CodeGen *cg, P2C_AstExpr *target, P2C_AstExpr *value) {
    if (!target) return;
    if (target->base.type == AST_NAME) {
        indent(cg);
        if (map_has_name(cg->nonlocal_names, target->base.u.name.name) && cg->closure_env_var) {
            write_str(cg, "p2c_cell_set(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", p2c_obj_from_str(\""); write_str(cg, target->base.u.name.name); write_str(cg, "\")), "); gen_expr(cg, value); write_str(cg, ");");
        } else if (map_has_name(cg->cell_names, target->base.u.name.name)) {
            write_str(cg, "p2c_cell_set(_p2c_cell_"); write_ident(cg, target->base.u.name.name); write_str(cg, ", "); gen_expr(cg, value); write_str(cg, ");");
        } else {
            write_ident(cg, target->base.u.name.name); write_str(cg, " = "); gen_expr(cg, value); write_str(cg, ";");
        }
        write_newline(cg);
    } else if (target->base.type == AST_ATTRIBUTE) {
        indent(cg); write_str(cg, "p2c_setattr("); gen_expr(cg, target->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, target->base.u.attribute.attr); write_str(cg, "\", "); gen_expr(cg, value); write_str(cg, ");"); write_newline(cg);
    } else if (target->base.type == AST_SUBSCRIPT) {
        indent(cg); write_str(cg, "p2c_subscript_set("); gen_expr(cg, target->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, target->base.u.subscript.slice); write_str(cg, ", "); gen_expr(cg, value); write_str(cg, ");"); write_newline(cg);
    } else if (target->base.type == AST_CALL && target->base.u.call.func &&
               target->base.u.call.func->base.type == AST_NAME &&
               strcmp(target->base.u.call.func->base.u.name.name, "p2c_obj_slice") == 0 &&
               target->base.u.call.args && p2c_vec_len(target->base.u.call.args) == 4) {
        indent(cg); write_str(cg, "p2c_slice_assign(");
        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 0)); write_str(cg, ", ");
        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 1)); write_str(cg, ", ");
        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 2)); write_str(cg, ", ");
        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 3)); write_str(cg, ", ");
        gen_expr(cg, value); write_str(cg, ");"); write_newline(cg);
    } else if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        /* タプル/リストへのアンパック代入: a, b = expr
         * 右辺を一度だけ評価してから、各要素へ順にsubscriptで取り出して代入する。 */
        indent(cg); write_str(cg, "{"); write_newline(cg); push_indent(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_unpack_src = "); gen_expr(cg, value); write_str(cg, ";"); write_newline(cg);
        P2C_Vector *elts = target->base.type == AST_TUPLE ? target->base.u.tuple.elts : target->base.u.list.elts;
        size_t elts_len = p2c_vec_len(elts);
        size_t star_index = elts_len;
        for (size_t i = 0; i < elts_len; i++) {
            P2C_AstExpr *candidate = (P2C_AstExpr*)p2c_vec_get(elts, i);
            if (candidate && candidate->base.type == AST_STARRED) {
                if (star_index != elts_len) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "multiple starred assignment targets are not supported");
                    break;
                }
                star_index = i;
            }
        }
        if (star_index != elts_len) {
            P2C_AstExpr *starred = (P2C_AstExpr*)p2c_vec_get(elts, star_index);
            if (!starred->base.u.starred.value || starred->base.u.starred.value->base.type != AST_NAME) {
                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "starred assignment target must be a name");
            }
            size_t trailing = elts_len - star_index - 1;
            indent(cg); write_str(cg, "int64_t _p2c_unpack_len = p2c_len(_p2c_unpack_src);"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_unpack_len < "); emit_usize(cg, elts_len - 1); write_str(cg, ") p2c_raise(p2c_make_exception(\"ValueError\", \"not enough values to unpack\"));"); write_newline(cg);
            for (size_t i = 0; i < elts_len; i++) {
                P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
                if (i == star_index) {
                    if (elt->base.u.starred.value && elt->base.u.starred.value->base.type == AST_NAME) {
                        indent(cg); write_ident(cg, elt->base.u.starred.value->base.u.name.name); write_str(cg, " = p2c_obj_slice(_p2c_unpack_src, p2c_obj_from_int("); emit_usize(cg, i); write_str(cg, "), p2c_obj_from_int(_p2c_unpack_len - "); emit_usize(cg, trailing); write_str(cg, "), &P2C_None);"); write_newline(cg);
                    }
                    continue;
                }
                indent(cg); write_str(cg, "P2C_Object *_p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, " = p2c_subscript_get(_p2c_unpack_src, p2c_obj_from_int(");
                if (i < star_index) emit_usize(cg, i);
                else { write_str(cg, "_p2c_unpack_len - "); emit_usize(cg, trailing); write_str(cg, " + "); emit_usize(cg, i - star_index - 1); }
                write_str(cg, ")); "); write_newline(cg);
                if (elt->base.type == AST_NAME) { indent(cg); write_ident(cg, elt->base.u.name.name); write_str(cg, " = _p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, ";"); write_newline(cg); }
                else if (elt->base.type == AST_ATTRIBUTE) { indent(cg); write_str(cg, "p2c_setattr("); gen_expr(cg, elt->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, elt->base.u.attribute.attr); write_str(cg, "\", _p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg); }
                else if (elt->base.type == AST_SUBSCRIPT) { indent(cg); write_str(cg, "p2c_subscript_set("); gen_expr(cg, elt->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, elt->base.u.subscript.slice); write_str(cg, ", _p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg); }
            }
            pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
            return;
        }
        for (size_t i = 0; i < elts_len; i++) {
            P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
            indent(cg); write_str(cg, "P2C_Object *_p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, " = p2c_subscript_get(_p2c_unpack_src, p2c_obj_from_int(");
            emit_usize(cg, i);
            write_str(cg, "));"); write_newline(cg);
            if (elt->base.type == AST_NAME) { indent(cg); write_ident(cg, elt->base.u.name.name); write_str(cg, " = _p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, ";"); write_newline(cg); }
            else if (elt->base.type == AST_ATTRIBUTE) { indent(cg); write_str(cg, "p2c_setattr("); gen_expr(cg, elt->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, elt->base.u.attribute.attr); write_str(cg, "\", _p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg); }
            else if (elt->base.type == AST_SUBSCRIPT) { indent(cg); write_str(cg, "p2c_subscript_set("); gen_expr(cg, elt->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, elt->base.u.subscript.slice); write_str(cg, ", _p2c_unpack_item_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg); }
        }
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    } else {
        indent(cg); gen_expr(cg, target); write_str(cg, " = "); gen_expr(cg, value); write_str(cg, ";"); write_newline(cg);
    }
}

/* return/break/continueで脱出するまでに通るtryの後処理を、Pythonが規定する
 * 「内側から外側へ」の順序で生成する。min_loop_depthは脱出先のループ深さで、
 * これより浅い位置のtry（脱出後も生存するtry）は対象外とする。
 * 各フレームでは finally本体（あれば）を実行してから p2c_exc_stack を元の
 * フレームへ戻す。これにより、return時に例外フレームが死んだスタックを
 * 指したままになるのを防ぎ、保留中の例外はPythonの規則どおり破棄される。 */
static void emit_exit_cleanups(P2C_CodeGen *cg, int min_loop_depth) {
    for (struct P2C_CleanupFrame *f = cg->cleanup_top; f && f->loop_depth >= min_loop_depth; f = f->prev) {
        /* 生成中のfinally本体（in_finalbody）は再実行しない。finallyの中で
         * return/break/continue した場合は、既に実行中のfinallyを飛ばして
         * 例外フレームの復元だけを行えばPythonと同じ挙動になる。 */
        if (!f->in_finalbody && f->finalbody && p2c_vec_len(f->finalbody) > 0) {
            struct P2C_CleanupFrame *saved_top = cg->cleanup_top;
            cg->cleanup_top = f->prev; /* finally自身のフレームは再実行しない */
            gen_stmt_list(cg, f->finalbody);
            cg->cleanup_top = saved_top;
        }
        indent(cg); write_str(cg, "p2c_exc_stack = _p2c_ef_"); emit_usize(cg, (size_t)f->try_id); write_str(cg, ".prev;"); write_newline(cg);
    }
}

static void gen_try_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) {
    P2C_AstNode *n = &stmt->base;
    int try_id = ++cg->temp_counter;
    indent(cg); write_str(cg, "{"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".prev = p2c_exc_stack;"); write_newline(cg);
    indent(cg); write_str(cg, "_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".exc = NULL;"); write_newline(cg);
    indent(cg); write_str(cg, "p2c_exc_stack = &_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ";"); write_newline(cg);
    /* 式評価中の一時値（binopの左オペランド、f-stringビルダ）の深さを保存する。
     * volatileなのはP2C_SETJMPの後にlongjmpで戻ってきたとき、setjmpの前後で
     * 変更された非volatileなローカルの値が不定になるため。 */
    indent(cg); write_str(cg, "volatile size_t _p2c_expr_depth_"); emit_usize(cg, (size_t)try_id); write_str(cg, " = p2c_binop_depth();"); write_newline(cg);
    indent(cg); write_str(cg, "volatile size_t _p2c_fstr_depth_"); emit_usize(cg, (size_t)try_id); write_str(cg, " = p2c_fstr_depth();"); write_newline(cg);
    indent(cg); write_str(cg, "int _p2c_jmp_"); emit_usize(cg, (size_t)try_id); write_str(cg, " = P2C_SETJMP(_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".env);"); write_newline(cg);
    indent(cg); write_str(cg, "int _p2c_handled_"); emit_usize(cg, (size_t)try_id); write_str(cg, " = 0;"); write_newline(cg);
    indent(cg); write_str(cg, "if (_p2c_jmp_"); emit_usize(cg, (size_t)try_id); write_str(cg, " == 0) {"); write_newline(cg); push_indent(cg);
    /* このtryの本体とexcept節を生成している間、return/break/continueが脱出する
     * ときに実行すべき後処理（finally本体と例外フレームの復元）を登録する。 */
    struct P2C_CleanupFrame _p2c_cf;
    _p2c_cf.prev = cg->cleanup_top;
    _p2c_cf.try_id = try_id;
    _p2c_cf.finalbody = n->u.try_stmt.finalbody;
    _p2c_cf.loop_depth = cg->loop_depth;
    _p2c_cf.in_finalbody = false;
    cg->cleanup_top = &_p2c_cf;
    gen_stmt_list(cg, n->u.try_stmt.body);
    if (p2c_vec_len(n->u.try_stmt.orelse) > 0) gen_stmt_list(cg, n->u.try_stmt.orelse);
    pop_indent(cg);
    for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
        P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
        indent(cg);
        /* 直前のブロック（i==0ならif(_p2c_jmp==0){...}、i>0なら1つ前の
         * except節の{...}）を閉じてから else if を開く。以前は i==0
         * のときだけ閉じ括弧を書いており、except節が2つ以上あると
         * 2つめ以降が1つ前のexcept節のブロックの中に入れ子になってしまい
         * 生成コードが「expected '}' before 'else'」でコンパイルエラーに
         * なるバグがあった。 */
        write_str(cg, "} else if (");
        if (!h->type) {
            write_str(cg, "1");
        } else if (h->type->base.type == AST_NAME) {
            write_str(cg, "p2c_exc_name_match(_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".exc, \""); write_str(cg, h->type->base.u.name.name); write_str(cg, "\")");
        } else if (h->type->base.type == AST_TUPLE) {
            /* except (A, B, ...): 複数の例外型をORで判定する。
             * 以前はこのケースを認識できておらず無条件で"1"（全キャッチ）を
             * 生成していたため、本来は後続のexcept節でマッチすべき例外まで
             * ここで奪ってしまうという静かな誤動作バグがあった。 */
            P2C_Vector *elts = h->type->base.u.tuple.elts;
            size_t n_elts = p2c_vec_len(elts);
            if (n_elts == 0) {
                write_str(cg, "0"); /* 空タプルは何にもマッチしない */
            } else {
                bool wrote_any = false;
                for (size_t ti = 0; ti < n_elts; ti++) {
                    P2C_AstExpr *te = (P2C_AstExpr*)p2c_vec_get(elts, ti);
                    if (!te || te->base.type != AST_NAME) continue;
                    if (wrote_any) write_str(cg, " || ");
                    write_str(cg, "p2c_exc_name_match(_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".exc, \""); write_str(cg, te->base.u.name.name); write_str(cg, "\")");
                    wrote_any = true;
                }
                if (!wrote_any) write_str(cg, "0");
            }
        } else {
            write_str(cg, "1");
        }
        write_str(cg, ") {"); write_newline(cg); push_indent(cg);
        /* 例外がこのtryへ届いた時点で、式評価中の一時値をtry開始時の深さへ戻す。
         * 冪等なので複数のexcept節が並んでいても、最初に一致した節で1回だけ
         * 実際に巻き戻る（式の途中で脱出した左オペランドやf-stringビルダを
         * 取り残さない）。 */
        indent(cg); write_str(cg, "p2c_binop_rewind(_p2c_expr_depth_"); emit_usize(cg, (size_t)try_id); write_str(cg, ");"); write_newline(cg);
        indent(cg); write_str(cg, "p2c_fstr_rewind(_p2c_fstr_depth_"); emit_usize(cg, (size_t)try_id); write_str(cg, ");"); write_newline(cg);
        indent(cg); write_str(cg, "_p2c_handled_"); emit_usize(cg, (size_t)try_id); write_str(cg, " = 1;"); write_newline(cg);
        indent(cg); write_str(cg, "p2c_exc_stack = _p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".prev;"); write_newline(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_saved_exc_"); emit_usize(cg, (size_t)try_id); write_str(cg, " = p2c_active_exception; p2c_active_exception = _p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".exc;"); write_newline(cg);
        if (h->name) { indent(cg); write_str(cg, h->name); write_str(cg, " = _p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".exc;"); write_newline(cg); }
        gen_stmt_list(cg, h->body);
        indent(cg); write_str(cg, "p2c_active_exception = _p2c_saved_exc_"); emit_usize(cg, (size_t)try_id); write_str(cg, ";"); write_newline(cg);
        pop_indent(cg);
    }
    indent(cg); write_str(cg, "}"); write_newline(cg);
    /* finally本体の生成中は、このtryのフレームを「実行中」として残す。
     * finallyの中のreturn/break/continueは、このfinallyを再実行せずに
     * 例外フレームの復元だけを行う必要がある（emit_exit_cleanupsが
     * in_finalbodyを見て本体の再実行を抑止する）。 */
    _p2c_cf.in_finalbody = true;
    if (p2c_vec_len(n->u.try_stmt.finalbody) > 0) gen_stmt_list(cg, n->u.try_stmt.finalbody);
    _p2c_cf.in_finalbody = false;
    cg->cleanup_top = _p2c_cf.prev;
    indent(cg); write_str(cg, "p2c_exc_stack = _p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".prev;"); write_newline(cg);
    /* ハンドラ無しで外側へ再送出する経路でも、このフレームで放棄した一時値を
     * 巻き戻しておく（正常終了時は深さが保存値と等しいので何もしない）。 */
    indent(cg); write_str(cg, "p2c_binop_rewind(_p2c_expr_depth_"); emit_usize(cg, (size_t)try_id); write_str(cg, ");"); write_newline(cg);
    indent(cg); write_str(cg, "p2c_fstr_rewind(_p2c_fstr_depth_"); emit_usize(cg, (size_t)try_id); write_str(cg, ");"); write_newline(cg);
    indent(cg); write_str(cg, "if (_p2c_jmp_"); emit_usize(cg, (size_t)try_id); write_str(cg, " != 0 && !_p2c_handled_"); emit_usize(cg, (size_t)try_id); write_str(cg, ") p2c_raise(_p2c_ef_"); emit_usize(cg, (size_t)try_id); write_str(cg, ".exc);"); write_newline(cg);
    pop_indent(cg); write_line(cg, "}");
}

/* cg->source_textからline番目（1始まり）の行を取り出してoutに書き込む。
 * 見つからない場合はoutを空文字にする。改行文字は含めない。 */
static void emit_match_value_ref(P2C_CodeGen *cg, int value_id) {
    write_str(cg, "_p2c_match_value_");
    emit_usize(cg, (size_t)value_id);
}

static void emit_match_ok_ref(P2C_CodeGen *cg, int state_id) {
    write_str(cg, "_p2c_match_ok_");
    emit_usize(cg, (size_t)state_id);
}

static void gen_match_pattern(P2C_CodeGen *cg, P2C_AstMatchPattern *pattern, int state_id, int value_id) {
    if (!pattern) return;
    switch (pattern->kind) {
        case P2C_MATCH_VALUE:
            indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_obj_is_truthy(p2c_obj_eq("); emit_match_value_ref(cg, value_id); write_str(cg, ", "); gen_expr(cg, pattern->value); write_str(cg, "))) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            break;
        case P2C_MATCH_CAPTURE:
            if (pattern->capture_name) { indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); write_ident(cg, pattern->capture_name); write_str(cg, " = "); emit_match_value_ref(cg, value_id); write_str(cg, ";"); write_newline(cg); }
            break;
        case P2C_MATCH_WILDCARD:
        case P2C_MATCH_STAR:
            break;
        case P2C_MATCH_AS:
            if (pattern->children && p2c_vec_len(pattern->children) == 1) gen_match_pattern(cg, (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, 0), state_id, value_id);
            if (pattern->capture_name) { indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); write_ident(cg, pattern->capture_name); write_str(cg, " = "); emit_match_value_ref(cg, value_id); write_str(cg, ";"); write_newline(cg); }
            break;
        case P2C_MATCH_SEQUENCE: {
            size_t count = pattern->children ? p2c_vec_len(pattern->children) : 0;
            size_t star_index = count;
            size_t fixed_count = count;
            for (size_t i = 0; i < count; i++) {
                P2C_AstMatchPattern *child = (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i);
                if (child && child->kind == P2C_MATCH_STAR) { star_index = i; fixed_count--; break; }
            }
            indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_match_sequence("); emit_match_value_ref(cg, value_id); write_str(cg, ", "); emit_usize(cg, fixed_count); write_str(cg, ", "); write_str(cg, star_index < count ? "true" : "false"); write_str(cg, ")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            for (size_t i = 0; i < count; i++) {
                P2C_AstMatchPattern *child = (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i);
                if (!child) continue;
                if (child->kind == P2C_MATCH_STAR) {
                    if (child->capture_name) {
                        size_t tail = count - i - 1;
                        indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); write_ident(cg, child->capture_name); write_str(cg, " = p2c_match_sequence_rest("); emit_match_value_ref(cg, value_id); write_str(cg, ", "); emit_usize(cg, i); write_str(cg, ", "); emit_usize(cg, tail); write_str(cg, ");"); write_newline(cg);
                    }
                    continue;
                }
                int child_value_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *"); emit_match_value_ref(cg, child_value_id); write_str(cg, " = &P2C_None;"); write_newline(cg);
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); emit_match_value_ref(cg, child_value_id); write_str(cg, " = p2c_match_sequence_item("); emit_match_value_ref(cg, value_id); write_str(cg, ", ");
                if (star_index < count && i > star_index) { write_str(cg, "(size_t)(p2c_len("); emit_match_value_ref(cg, value_id); write_str(cg, ") - "); emit_usize(cg, count - i); write_str(cg, ")"); }
                else emit_usize(cg, i);
                write_str(cg, ");"); write_newline(cg);
                gen_match_pattern(cg, child, state_id, child_value_id);
            }
            break;
        }
        case P2C_MATCH_CLASS:
            if (pattern->value) {
                int class_value_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *"); emit_match_value_ref(cg, class_value_id); write_str(cg, " = "); gen_expr(cg, pattern->value); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_isinstance_of_object("); emit_match_value_ref(cg, value_id); write_str(cg, ", "); emit_match_value_ref(cg, class_value_id); write_str(cg, ")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            } else {
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_isinstance_of_class("); emit_match_value_ref(cg, value_id); write_str(cg, ", \""); write_str(cg, pattern->class_name ? pattern->class_name : ""); write_str(cg, "\")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            }
            {
                size_t positional_index = 0;
                if (pattern->attr_names && pattern->children) for (size_t i = 0; i < p2c_vec_len(pattern->attr_names); i++) {
                    int child_value_id = ++cg->temp_counter;
                    const char *attr_name = (const char*)p2c_vec_get(pattern->attr_names, i);
                    P2C_AstMatchPattern *child = (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i);
                    indent(cg); write_str(cg, "P2C_Object *"); emit_match_value_ref(cg, child_value_id); write_str(cg, " = &P2C_None;"); write_newline(cg);
                    if (attr_name) {
                        indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_hasattr("); emit_match_value_ref(cg, value_id); write_str(cg, ", \""); write_str(cg, attr_name); write_str(cg, "\")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
                        indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); emit_match_value_ref(cg, child_value_id); write_str(cg, " = p2c_getattr("); emit_match_value_ref(cg, value_id); write_str(cg, ", \""); write_str(cg, attr_name); write_str(cg, "\");"); write_newline(cg);
                    } else {
                        indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_match_class_positional("); emit_match_value_ref(cg, value_id); write_str(cg, ", \""); write_str(cg, pattern->class_name ? pattern->class_name : ""); write_str(cg, "\", "); emit_usize(cg, positional_index); write_str(cg, ", &"); emit_match_value_ref(cg, child_value_id); write_str(cg, ")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
                        positional_index++;
                    }
                    gen_match_pattern(cg, child, state_id, child_value_id);
                }
            }
            break;
        case P2C_MATCH_MAPPING: {
            size_t key_count = pattern->keys ? p2c_vec_len(pattern->keys) : 0;
            int rest_keys_id = 0;
            if (pattern->rest_name) {
                rest_keys_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *_p2c_match_keys_"); emit_usize(cg, (size_t)rest_keys_id); write_str(cg, "["); emit_usize(cg, key_count ? key_count : 1); write_str(cg, "]; "); write_newline(cg);
            }
            indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_obj_is_dict("); emit_match_value_ref(cg, value_id); write_str(cg, ")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            if (pattern->keys && pattern->children) for (size_t i = 0; i < key_count; i++) {
                int key_id = ++cg->temp_counter;
                int child_value_id = ++cg->temp_counter;
                P2C_AstExpr *key = (P2C_AstExpr*)p2c_vec_get(pattern->keys, i);
                P2C_AstMatchPattern *child = (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i);
                indent(cg); write_str(cg, "P2C_Object *"); emit_match_value_ref(cg, key_id); write_str(cg, " = "); gen_expr(cg, key); write_str(cg, ";"); write_newline(cg);
                if (pattern->rest_name) { indent(cg); write_str(cg, "_p2c_match_keys_"); emit_usize(cg, (size_t)rest_keys_id); write_str(cg, "["); emit_usize(cg, i); write_str(cg, "] = "); emit_match_value_ref(cg, key_id); write_str(cg, ";"); write_newline(cg); }
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !p2c_match_mapping_has("); emit_match_value_ref(cg, value_id); write_str(cg, ", "); emit_match_value_ref(cg, key_id); write_str(cg, ")) "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *"); emit_match_value_ref(cg, child_value_id); write_str(cg, " = &P2C_None;"); write_newline(cg);
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); emit_match_value_ref(cg, child_value_id); write_str(cg, " = p2c_match_mapping_get("); emit_match_value_ref(cg, value_id); write_str(cg, ", "); emit_match_value_ref(cg, key_id); write_str(cg, ");"); write_newline(cg);
                gen_match_pattern(cg, child, state_id, child_value_id);
            }
            if (pattern->rest_name) {
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); write_ident(cg, pattern->rest_name); write_str(cg, " = p2c_match_mapping_rest("); emit_match_value_ref(cg, value_id); write_str(cg, ", _p2c_match_keys_"); emit_usize(cg, (size_t)rest_keys_id); write_str(cg, ", "); emit_usize(cg, key_count); write_str(cg, ");"); write_newline(cg);
            }
            break;
        }
        case P2C_MATCH_OR: {
            int or_done_id = ++cg->temp_counter;
            indent(cg); write_str(cg, "int _p2c_match_or_"); emit_usize(cg, (size_t)or_done_id); write_str(cg, " = 0;"); write_newline(cg);
            indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
            if (pattern->children) for (size_t i = 0; i < p2c_vec_len(pattern->children); i++) {
                int alternative_state_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "if (!_p2c_match_or_"); emit_usize(cg, (size_t)or_done_id); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
                indent(cg); write_str(cg, "int "); emit_match_ok_ref(cg, alternative_state_id); write_str(cg, " = 1;"); write_newline(cg);
                gen_match_pattern(cg, (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i), alternative_state_id, value_id);
                indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, alternative_state_id); write_str(cg, ") _p2c_match_or_"); emit_usize(cg, (size_t)or_done_id); write_str(cg, " = 1;"); write_newline(cg);
                pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
            }
            pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
            indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, " && !_p2c_match_or_"); emit_usize(cg, (size_t)or_done_id); write_str(cg, ") "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            break;
        }
        default:
            /* 未知のパターン種別は「マッチしない」として扱う（未対応の構文は
             * parse時に診断されるため、ここへは来ない）。 */
            indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 0;"); write_newline(cg);
            break;
    }
}

static void gen_match_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) {
    P2C_AstMatch *match_stmt = &stmt->base.u.match_stmt;
    int match_id = ++cg->temp_counter;
    indent(cg); write_str(cg, "{"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_match_value_"); emit_usize(cg, (size_t)match_id); write_str(cg, " = "); gen_expr(cg, match_stmt->subject); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "int _p2c_match_done_"); emit_usize(cg, (size_t)match_id); write_str(cg, " = 0;"); write_newline(cg);
    for (size_t i = 0; i < p2c_vec_len(match_stmt->cases); i++) {
        P2C_AstMatchCase *match_case = (P2C_AstMatchCase*)p2c_vec_get(match_stmt->cases, i);
        int state_id;
        if (!match_case || !match_case->pattern) continue;
        state_id = ++cg->temp_counter;
        indent(cg); write_str(cg, "if (!_p2c_match_done_"); emit_usize(cg, (size_t)match_id); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
        indent(cg); write_str(cg, "int "); emit_match_ok_ref(cg, state_id); write_str(cg, " = 1;"); write_newline(cg);
        gen_match_pattern(cg, match_case->pattern, state_id, match_id);
        indent(cg); write_str(cg, "if ("); emit_match_ok_ref(cg, state_id); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
        if (match_case->guard) {
            indent(cg); write_str(cg, "if (p2c_obj_is_truthy("); gen_expr(cg, match_case->guard); write_str(cg, ")) {"); write_newline(cg); push_indent(cg);
        }
        indent(cg); write_str(cg, "_p2c_match_done_"); emit_usize(cg, (size_t)match_id); write_str(cg, " = 1;"); write_newline(cg);
        gen_stmt_list(cg, match_case->body);
        if (match_case->guard) { pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg); }
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    }
    pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
}

static void get_source_line(P2C_CodeGen *cg, uint32_t line, char *out, size_t out_sz) {
    out[0] = '\0';
    if (!cg->source_text || line == 0) return;
    const char *p = cg->source_text;
    uint32_t cur = 1;
    while (cur < line && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return;
    const char *start = p;
    while (*start == ' ' || *start == '\t') start++; /* 先頭の空白は除去してコメントを短くする */
    size_t len = 0;
    while (start[len] && start[len] != '\n' && start[len] != '\r' && len < out_sz - 1) len++;
    memcpy(out, start, len);
    out[len] = '\0';
}

/* debug_info有効時、文の直前に元のPython行をコメントとして挿入する。
 * コメント終端記号がソース中に含まれる場合は壊れたコメントにならないよう
 * 安全に無害化する。 */
static void emit_debug_comment(P2C_CodeGen *cg, P2C_AstNode *n) {
    if (!cg->opts.debug_info) return;
    char line_text[256];
    get_source_line(cg, n->line, line_text, sizeof(line_text));
    indent(cg);
    write_str(cg, "/* py:");
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", n->line);
        write_str(cg, buf);
    }
    write_str(cg, ": ");
    if (line_text[0]) {
        for (char *c = line_text; *c; c++) {
            if (c[0] == '*' && c[1] == '/') { write_str(cg, "* /"); c++; }
            else { char one[2] = {*c, '\0'}; write_str(cg, one); }
        }
    }
    write_str(cg, " */");
    write_newline(cg);
}

/* 関数本体内の global/nonlocal 文で宣言されている名前を集める。
 * これらは関数のローカル変数として再宣言してはならない
 * （ファイルスコープの変数、または外側のスコープの変数をそのまま使うため）。 */
static void collect_global_decls(P2C_Vector *stmts, P2C_CodeGen *cg) {
    if (!stmts) return;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt) continue;
        P2C_AstNode *n = &stmt->base;
        if (n->type == AST_GLOBAL) {
            for (size_t j = 0; j < p2c_vec_len(n->u.global.names); j++) {
                const char *name = (const char*)p2c_vec_get(n->u.global.names, j);
                if (name) remember_declared(cg, name);
            }
        } else if (n->type == AST_NONLOCAL) {
            for (size_t j = 0; j < p2c_vec_len(n->u.nonlocal_stmt.names); j++) {
                const char *name = (const char*)p2c_vec_get(n->u.nonlocal_stmt.names, j);
                if (name) map_set_name(cg->nonlocal_names, name);
            }
        } else if (n->type == AST_IF) {
            collect_global_decls(n->u.if_stmt.body, cg);
            collect_global_decls(n->u.if_stmt.orelse, cg);
        } else if (n->type == AST_WHILE) {
            collect_global_decls(n->u.while_stmt.body, cg);
        } else if (n->type == AST_FOR) {
            collect_global_decls(n->u.for_stmt.body, cg);
        } else if (n->type == AST_TRY) {
            collect_global_decls(n->u.try_stmt.body, cg);
            collect_global_decls(n->u.try_stmt.finalbody, cg);
            for (size_t j = 0; j < p2c_vec_len(n->u.try_stmt.handlers); j++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, j);
                if (h) collect_global_decls(h->body, cg);
            }
        }
    }
}

/* 基底クラス式から名前を取り出す。単純な名前(Animal)だけでなく、
 * pygame.sprite.Spriteのようなドット区切りの属性アクセスにも対応する
 * （この場合は末尾の属性名"Sprite"を基底クラス名として扱う）。
 * このコードベースの継承モデルは実体参照ではなく名前文字列ベースのため、
 * 登録されているクラス名と一致してさえいれば、どちらの書き方でも
 * 継承解決が機能する。 */
static const char* extract_base_name(P2C_AstExpr *b) {
    if (!b) return NULL;
    if (b->base.type == AST_NAME) return b->base.u.name.name;
    if (b->base.type == AST_ATTRIBUTE) return b->base.u.attribute.attr;
    return NULL;
}

static bool is_builtin_exception_name(const char *name) {
    static const char *names[] = {
        "Exception", "BaseException", "ValueError", "TypeError", "KeyError", "IndexError",
        "ZeroDivisionError", "RuntimeError", "AttributeError", "NameError", "StopIteration", "StopAsyncIteration",
        "AssertionError", "NotImplementedError", "OverflowError", "ArithmeticError",
        "FileNotFoundError", "IOError", "OSError", "ImportError", "LookupError", NULL
    };
    if (!name) return false;
    for (int i = 0; names[i]; i++) if (strcmp(names[i], name) == 0) return true;
    return false;
}

/* raise ExceptionType(...) や raise ExceptionType のように、組み込み例外型を
 * 直接送出する式を p2c_make_exception(...) 呼び出しへ変換して生成する。
 * それ以外（例: 既にキャッチした例外変数の再raiseなど）はそのまま式として評価する。
 * (以前はこの変換が一切なく、raise ValueError("msg") のような一般的な書き方が
 * 「ValueErrorという未定義関数の呼び出し」としてコンパイルエラーになっていた。) */
static void gen_raise_target(P2C_CodeGen *cg, P2C_AstExpr *exc) {
    if (exc && exc->base.type == AST_CALL && exc->base.u.call.func->base.type == AST_NAME &&
        is_builtin_exception_name(exc->base.u.call.func->base.u.name.name)) {
        write_str(cg, "p2c_make_exception(\"");
        write_str(cg, exc->base.u.call.func->base.u.name.name);
        write_str(cg, "\", ");
        if (p2c_vec_len(exc->base.u.call.args) > 0) {
            write_str(cg, "p2c_obj_as_str(p2c_obj_str(");
            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(exc->base.u.call.args, 0));
            write_str(cg, "))");
        } else {
            write_str(cg, "\"\"");
        }
        write_str(cg, ")");
    } else if (exc && exc->base.type == AST_NAME && is_builtin_exception_name(exc->base.u.name.name)) {
        write_str(cg, "p2c_make_exception(\""); write_str(cg, exc->base.u.name.name); write_str(cg, "\", \"\")");
    } else if (exc) {
        gen_expr(cg, exc);
    } else {
        write_str(cg, "p2c_make_exception(\"RuntimeError\", \"raise\")");
    }
}

static void gen_with_items(P2C_CodeGen *cg, P2C_Vector *items, size_t item_index, P2C_Vector *body) {
    if (item_index >= p2c_vec_len(items)) { gen_stmt_list(cg, body); return; }
    P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(items, item_index);
    int with_id = ++cg->temp_counter;
    indent(cg); write_str(cg, "{"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, " = "); gen_expr(cg, item->context_expr); write_str(cg, ";"); write_newline(cg);
    if (item->optional_vars) {
        indent(cg); write_str(cg, "P2C_Object *_p2c_with_val_"); emit_usize(cg, (size_t)with_id); write_str(cg, " = p2c_has_method(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__enter__\") ? p2c_call_attr(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__enter__\", NULL, 0) : _p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ";"); write_newline(cg);
        if (item->optional_vars->base.type == AST_NAME) {
            indent(cg); write_ident(cg, item->optional_vars->base.u.name.name); write_str(cg, " = _p2c_with_val_"); emit_usize(cg, (size_t)with_id); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "(void)"); write_ident(cg, item->optional_vars->base.u.name.name); write_str(cg, ";"); write_newline(cg);
        } else if (item->optional_vars->base.type == AST_TUPLE || item->optional_vars->base.type == AST_LIST) {
            char with_value_name[64];
            P2C_AstExpr with_value_expr;
            snprintf(with_value_name, sizeof(with_value_name), "_p2c_with_val_%d", with_id);
            memset(&with_value_expr, 0, sizeof(with_value_expr));
            with_value_expr.base.type = AST_NAME;
            with_value_expr.base.u.name.name = with_value_name;
            gen_assign_target(cg, item->optional_vars, &with_value_expr);
        } else {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "with as target must be a name, tuple, or list");
        }
    } else {
        indent(cg); write_str(cg, "(void)(p2c_has_method(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__enter__\") ? p2c_call_attr(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__enter__\", NULL, 0) : _p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ");"); write_newline(cg);
    }
    indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "_p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ".prev = p2c_exc_stack; _p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ".env) == 0) {"); write_newline(cg); push_indent(cg);
    gen_with_items(cg, items, item_index + 1, body);
    indent(cg); write_str(cg, "p2c_exc_stack = _p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ".prev;"); write_newline(cg);
    indent(cg); write_str(cg, "if (p2c_has_method(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__exit__\")) p2c_call_attr(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__exit__\", (P2C_Object*[]){&P2C_None, &P2C_None, &P2C_None}, 3);"); write_newline(cg);
    pop_indent(cg); indent(cg); write_str(cg, "} else {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_with_exc_"); emit_usize(cg, (size_t)with_id); write_str(cg, " = _p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ".exc;"); write_newline(cg);
    indent(cg); write_str(cg, "p2c_exc_stack = _p2c_with_ef_"); emit_usize(cg, (size_t)with_id); write_str(cg, ".prev;"); write_newline(cg);
    indent(cg); write_str(cg, "if (!(p2c_has_method(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__exit__\") && p2c_obj_is_truthy(p2c_call_attr(_p2c_with_ctx_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", \"__exit__\", (P2C_Object*[]){p2c_builtin_type(_p2c_with_exc_"); emit_usize(cg, (size_t)with_id); write_str(cg, "), _p2c_with_exc_"); emit_usize(cg, (size_t)with_id); write_str(cg, ", &P2C_None}, 3)))) p2c_raise(_p2c_with_exc_"); emit_usize(cg, (size_t)with_id); write_str(cg, ");"); write_newline(cg);
    pop_indent(cg); write_line(cg, "}");
    pop_indent(cg); write_line(cg, "}");
}

static bool suspension_expr(P2C_AstExpr *expr) {
    if (!expr) return false;
    return expr->base.type == AST_YIELD || expr->base.type == AST_AWAIT;
}

static P2C_AstExpr *find_await_expr(P2C_AstExpr *expr) {
    if (!expr) return NULL;
    if (expr->base.type == AST_AWAIT) return expr;
    if (expr->base.type == AST_BINOP) {
        P2C_AstExpr *found = find_await_expr(expr->base.u.binop.left);
        return found ? found : find_await_expr(expr->base.u.binop.right);
    }
    if (expr->base.type == AST_UNARYOP) return find_await_expr(expr->base.u.unaryop.operand);
    if (expr->base.type == AST_COMPARE) {
        P2C_AstExpr *found = find_await_expr(expr->base.u.compare.left);
        if (found) return found;
        for (size_t i = 0; i < p2c_vec_len(expr->base.u.compare.comparators); i++) {
            found = find_await_expr((P2C_AstExpr*)p2c_vec_get(expr->base.u.compare.comparators, i));
            if (found) return found;
        }
        return NULL;
    }
    if (expr->base.type == AST_BOOLOP) {
        for (size_t i = 0; i < p2c_vec_len(expr->base.u.boolop.values); i++) {
            P2C_AstExpr *found = find_await_expr((P2C_AstExpr*)p2c_vec_get(expr->base.u.boolop.values, i));
            if (found) return found;
        }
        return NULL;
    }
    if (expr->base.type == AST_IFEXP) {
        P2C_AstExpr *found = find_await_expr(expr->base.u.ifexp.test);
        if (found) return found;
        found = find_await_expr(expr->base.u.ifexp.body);
        return found ? found : find_await_expr(expr->base.u.ifexp.orelse);
    }
    if (expr->base.type == AST_CALL) {
        P2C_AstExpr *found = find_await_expr(expr->base.u.call.func);
        if (found) return found;
        for (size_t i = 0; i < p2c_vec_len(expr->base.u.call.args); i++) {
            found = find_await_expr((P2C_AstExpr*)p2c_vec_get(expr->base.u.call.args, i));
            if (found) return found;
        }
    }
    return NULL;
}

static bool suspension_stmt_list_has_await(P2C_Vector *stmts);

static bool suspension_stmt_has_await(P2C_AstStmt *stmt) {
    P2C_AstNode *n;
    if (!stmt) return false;
    n = &stmt->base;
    switch (n->type) {
        case AST_ASSIGN: return find_await_expr(n->u.assign.value) != NULL;
        case AST_AUGASSIGN: return find_await_expr(n->u.augassign.value) != NULL;
        case AST_ANNASSIGN: return find_await_expr(n->u.annassign.value) != NULL;
        case AST_RETURN: return find_await_expr(n->u.return_stmt.value) != NULL;
        case AST_EXPR_STMT: return find_await_expr(n->u.expr_stmt.value) != NULL || suspension_expr(n->u.expr_stmt.value);
        case AST_RAISE: return find_await_expr(n->u.raise.exc) != NULL || find_await_expr(n->u.raise.cause) != NULL;
        case AST_ASSERT: return find_await_expr(n->u.assert_stmt.test) != NULL || find_await_expr(n->u.assert_stmt.msg) != NULL;
        case AST_IF:
            return find_await_expr(n->u.if_stmt.test) != NULL || suspension_stmt_list_has_await(n->u.if_stmt.body) || suspension_stmt_list_has_await(n->u.if_stmt.orelse);
        case AST_WHILE:
            return find_await_expr(n->u.while_stmt.test) != NULL || suspension_stmt_list_has_await(n->u.while_stmt.body) || suspension_stmt_list_has_await(n->u.while_stmt.orelse);
        case AST_FOR:
        case AST_ASYNC_FOR:
            return n->type == AST_ASYNC_FOR || find_await_expr(n->u.for_stmt.iter) != NULL || suspension_stmt_list_has_await(n->u.for_stmt.body) || suspension_stmt_list_has_await(n->u.for_stmt.orelse);
        case AST_WITH:
            if (n->u.with.is_async) return true;
            for (size_t i = 0; i < p2c_vec_len(n->u.with.items); i++) {
                P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, i);
                if (item && find_await_expr(item->context_expr)) return true;
            }
            return suspension_stmt_list_has_await(n->u.with.body);
        case AST_TRY:
            if (suspension_stmt_list_has_await(n->u.try_stmt.body) || suspension_stmt_list_has_await(n->u.try_stmt.orelse) || suspension_stmt_list_has_await(n->u.try_stmt.finalbody)) return true;
            for (size_t i = 0; i < p2c_vec_len(n->u.try_stmt.handlers); i++) {
                P2C_AstExceptHandler *handler = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, i);
                if (handler && (find_await_expr(handler->type) || suspension_stmt_list_has_await(handler->body))) return true;
            }
            return false;
        default: return false;
    }
}

static bool suspension_stmt_list_has_await(P2C_Vector *stmts) {
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        if (suspension_stmt_has_await((P2C_AstStmt*)p2c_vec_get(stmts, i))) return true;
    }
    return false;
}

/* メソッド種別（生成Cの P2C_METHOD_* と同じ値。codegenはランタイムヘッダを
 * 取り込まないため、ここで同じ値を持つ列挙を定義する）。 */
enum {
    CG_METHOD_INSTANCE = 0,
    CG_METHOD_STATIC = 1,
    CG_METHOD_CLASS = 2,
    CG_METHOD_PROPERTY = 3,
    /* @x.setter: プロパティ x へ代入されたときに呼ばれる setter。
     * メソッド表には同じ名前で PROPERTY の行（ゲッター）と別に行を持ち、
     * ランタイムは種別で区別する（名前引きはゲッターを優先）。 */
    CG_METHOD_PROPERTY_SETTER = 4
};

/* クラス本体内のメソッド定義に付いたデコレータから、メソッド種別を求める。
 *   @staticmethod -> CG_METHOD_STATIC  （selfを渡さない）
 *   @classmethod   -> CG_METHOD_CLASS   （先頭引数にクラスオブジェクト）
 *   @property      -> CG_METHOD_PROPERTY（属性読み出し時のゲッター）
 * 上記以外のデコレータは、クラス本体では意味解析が明示診断する（ここでは
 * 通常のインスタンスメソッドとして扱い、生成そのものは壊さない）。 */
static int class_method_kind(P2C_AstFunctionDef *fd) {
    if (!fd || !fd->decorator_list) return CG_METHOD_INSTANCE;
    for (size_t i = 0; i < p2c_vec_len(fd->decorator_list); i++) {
        P2C_AstExpr *dec = (P2C_AstExpr*)p2c_vec_get(fd->decorator_list, i);
        if (!dec) continue;
        if (dec->base.type == AST_NAME) {
            const char *dn = dec->base.u.name.name;
            if (!dn) continue;
            if (strcmp(dn, "staticmethod") == 0) return CG_METHOD_STATIC;
            if (strcmp(dn, "classmethod") == 0) return CG_METHOD_CLASS;
            if (strcmp(dn, "property") == 0) return CG_METHOD_PROPERTY;
            continue;
        }
        /* @x.setter / @x.deleter の形（属性アクセスをデコレータに使う）。
         * setter のみ対応し、deleter は意味解析側で診断する。 */
        if (dec->base.type == AST_ATTRIBUTE && dec->base.u.attribute.attr &&
            strcmp(dec->base.u.attribute.attr, "setter") == 0) {
            return CG_METHOD_PROPERTY_SETTER;
        }
    }
    return CG_METHOD_INSTANCE;
}

/* メソッド種別を生成Cの P2C_METHOD_* 定数名へ変換する。 */
static const char* class_method_kind_constant(int kind) {
    switch (kind) {
        case CG_METHOD_STATIC: return "P2C_METHOD_STATIC";
        case CG_METHOD_CLASS: return "P2C_METHOD_CLASS";
        case CG_METHOD_PROPERTY: return "P2C_METHOD_PROPERTY";
        case CG_METHOD_PROPERTY_SETTER: return "P2C_METHOD_PROPERTY_SETTER";
        default: return "P2C_METHOD_INSTANCE";
    }
}

/* @x.setter のメソッドはゲッターと同じ Python 名を持つため、そのままでは C の
 * シンボル（関数・アダプタ）が衝突して生成Cがコンパイルできない。クラス本体の
 * 生成前に "<name>__setter" へ改名し、メソッド表の「名前」列だけ元の Python 名
 * （＝プロパティ名）へ戻す。 */
#define P2C_SETTER_C_SUFFIX "__setter"

static void rename_class_setters(P2C_CodeGen *cg, P2C_AstClassDef *cd) {
    for (size_t i = 0; i < p2c_vec_len(cd->body); i++) {
        P2C_AstStmt *m = (P2C_AstStmt*)p2c_vec_get(cd->body, i);
        if (!m || m->base.type != AST_FUNCTIONDEF) continue;
        if (class_method_kind(&m->base.u.functiondef) != CG_METHOD_PROPERTY_SETTER) continue;
        {
            /* snprintf だと -Wformat-truncation が「切れる可能性」を指摘するため、
             * 長さを確定させて memcpy で組み立てる（他の箇所と同じ方針）。 */
            const char *base = m->base.u.functiondef.name;
            size_t base_len = strlen(base);
            size_t suffix_len = sizeof(P2C_SETTER_C_SUFFIX) - 1u;
            char *renamed = (char*)p2c_alloc(cg->alloc, base_len + suffix_len + 1u);
            if (!renamed) continue;
            memcpy(renamed, base, base_len);
            memcpy(renamed + base_len, P2C_SETTER_C_SUFFIX, suffix_len + 1u);
            m->base.u.functiondef.name = renamed;
        }
    }
}

/* メソッド表の「名前」列を出力する。setter は C 名の接尾辞を落として
 * プロパティ名（＝ゲッターと同じ名前）に戻す。 */
static void emit_method_row_name(P2C_CodeGen *cg, const char *c_name, int kind) {
    if (kind == CG_METHOD_PROPERTY_SETTER && c_name) {
        size_t len = strlen(c_name);
        size_t suffix = sizeof(P2C_SETTER_C_SUFFIX) - 1u;
        char buf[256];
        size_t copy;
        if (len > suffix) len -= suffix;
        copy = len < sizeof(buf) - 1u ? len : sizeof(buf) - 1u;
        memcpy(buf, c_name, copy);
        buf[copy] = '\0';
        write_str(cg, buf);
        return;
    }
    if (c_name) write_str(cg, c_name);
}

static bool class_method_requires_suspension(P2C_AstFunctionDef *fd) {
    return fd && fd->is_async && suspension_stmt_list_has_await(fd->body);
}

static bool suspension_function(P2C_AstFunctionDef *fd) {
    if (!fd) return false;
    if (fd->is_async) return true;
    for (size_t i = 0; i < p2c_vec_len(fd->body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, i);
        if (!stmt) continue;
        if (stmt->base.type == AST_EXPR_STMT && suspension_expr(stmt->base.u.expr_stmt.value)) return true;
        if (stmt->base.type == AST_ASSIGN && suspension_expr(stmt->base.u.assign.value)) return true;
        if (stmt->base.type == AST_RETURN && suspension_expr(stmt->base.u.return_stmt.value)) return true;
        if (stmt->base.type == AST_WITH && stmt->base.u.with.is_async) return true;
    }
    return false;
}

static void suspension_collect_local(P2C_Map *locals, P2C_AstExpr *target) {
    if (!target) return;
    if (target->base.type == AST_NAME) {
        map_set_name(locals, target->base.u.name.name);
    } else if (target->base.type == AST_STARRED) {
        suspension_collect_local(locals, target->base.u.starred.value);
    } else if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.type == AST_TUPLE ? target->base.u.tuple.elts : target->base.u.list.elts;
        for (size_t i = 0; i < p2c_vec_len(elts); i++) suspension_collect_local(locals, (P2C_AstExpr*)p2c_vec_get(elts, i));
    }
}

static void suspension_collect_locals(P2C_Map *locals, P2C_AstFunctionDef *fd) {
    for (size_t i = 0; i < p2c_vec_len(fd->args); i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (arg) map_set_name(locals, arg->name);
    }
    for (size_t i = 0; i < p2c_vec_len(fd->body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, i);
        if (!stmt) continue;
        if (stmt->base.type == AST_ASSIGN) {
            for (size_t j = 0; j < p2c_vec_len(stmt->base.u.assign.targets); j++) {
                suspension_collect_local(locals, (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, j));
            }
        } else if (stmt->base.type == AST_AUGASSIGN) {
            suspension_collect_local(locals, stmt->base.u.augassign.target);
        } else if (stmt->base.type == AST_ASYNC_FOR) {
            suspension_collect_local(locals, stmt->base.u.for_stmt.target);
        } else if (stmt->base.type == AST_WITH && stmt->base.u.with.is_async) {
            for (size_t j = 0; j < p2c_vec_len(stmt->base.u.with.items); j++) {
                P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(stmt->base.u.with.items, j);
                if (item) suspension_collect_local(locals, item->optional_vars);
            }
        }
    }
}

static P2C_Map *collect_nested_nonlocal_cell_names(P2C_CodeGen *cg, P2C_Vector *stmts) {
    P2C_Map *names = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!names) return NULL;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt || stmt->base.type != AST_FUNCTIONDEF) continue;
        for (size_t j = 0; j < p2c_vec_len(stmt->base.u.functiondef.body); j++) {
            P2C_AstStmt *child_stmt = (P2C_AstStmt*)p2c_vec_get(stmt->base.u.functiondef.body, j);
            if (!child_stmt || child_stmt->base.type != AST_NONLOCAL) continue;
            for (size_t k = 0; k < p2c_vec_len(child_stmt->base.u.nonlocal_stmt.names); k++) {
                const char *name = (const char*)p2c_vec_get(child_stmt->base.u.nonlocal_stmt.names, k);
                if (name) map_set_name(names, name);
            }
        }
    }
    return names;
}

static void emit_cell_declarations(P2C_CodeGen *cg, P2C_Map *cells) {
    if (!cells) return;
    for (size_t i = 0; i < cells->bucket_count; i++) {
        for (P2C_MapEntry *entry = cells->buckets[i]; entry; entry = entry->next) {
            const char *name = (const char*)entry->key;
            indent(cg); write_str(cg, "P2C_Object *_p2c_cell_"); write_ident(cg, name); write_str(cg, " = p2c_cell_new("); write_ident(cg, name); write_str(cg, ");"); write_newline(cg);
        }
    }
}

/* ジェネレータ式のステップ関数内で式を生成する入口（実体は _impl の直後に定義）。
 * _impl から再帰的に呼び出すため、ここで先に宣言する。 */
static void gen_suspension_expr(P2C_CodeGen *cg, P2C_AstExpr *expr, P2C_Map *locals);

static void gen_suspension_expr_impl(P2C_CodeGen *cg, P2C_AstExpr *expr, P2C_Map *locals) {
    if (!expr) { write_str(cg, "&P2C_None"); return; }
    P2C_AstNode *n = &expr->base;
    if (n->type == AST_NAME) {
        if (map_has_name(locals, n->u.name.name)) {
            write_str(cg, "p2c_generator_local_get(generator, \""); write_str(cg, n->u.name.name); write_str(cg, "\")");
        } else {
            write_ident(cg, n->u.name.name);
        }
        return;
    }
    if (n->type == AST_CONST) { gen_expr(cg, expr); return; }
    if (n->type == AST_AWAIT) { write_str(cg, "p2c_generator_await_result(generator)"); return; }
    if (n->type == AST_BINOP) {
        const char *fn = "p2c_obj_add";
        switch (n->u.binop.op) {
            case OP_ADD: fn = "p2c_obj_add"; break;
            case OP_SUB: fn = "p2c_obj_sub"; break;
            case OP_MULT: fn = "p2c_obj_mul"; break;
            case OP_DIV: fn = "p2c_obj_div"; break;
            case OP_FLOORDIV: fn = "p2c_obj_floordiv"; break;
            case OP_MOD: fn = "p2c_obj_mod"; break;
            case OP_POW: fn = "p2c_obj_pow"; break;
            default: break;
        }
        write_str(cg, fn); write_str(cg, "(");
        gen_suspension_expr(cg, n->u.binop.left, locals); write_str(cg, ", ");
        gen_suspension_expr(cg, n->u.binop.right, locals); write_str(cg, ")");
        return;
    }
    if (n->type == AST_BOOLOP && p2c_vec_len(n->u.boolop.values) == 2) {
        P2C_AstExpr *left = (P2C_AstExpr*)p2c_vec_get(n->u.boolop.values, 0);
        P2C_AstExpr *right = (P2C_AstExpr*)p2c_vec_get(n->u.boolop.values, 1);
        if (n->u.boolop.op == OP_AND) {
            write_str(cg, "(p2c_obj_is_truthy("); gen_suspension_expr(cg, left, locals); write_str(cg, ") ? ");
            gen_suspension_expr(cg, right, locals); write_str(cg, " : "); gen_suspension_expr(cg, left, locals); write_str(cg, ")");
        } else {
            write_str(cg, "(p2c_obj_is_truthy("); gen_suspension_expr(cg, left, locals); write_str(cg, ") ? ");
            gen_suspension_expr(cg, left, locals); write_str(cg, " : "); gen_suspension_expr(cg, right, locals); write_str(cg, ")");
        }
        return;
    }
    if (n->type == AST_UNARYOP) {
        if (n->u.unaryop.op == OP_USUB) {
            write_str(cg, "p2c_obj_mul(p2c_obj_from_int(-1), ");
            gen_suspension_expr(cg, n->u.unaryop.operand, locals); write_str(cg, ")");
        } else {
            gen_suspension_expr(cg, n->u.unaryop.operand, locals);
        }
        return;
    }
    if (n->type == AST_COMPARE && p2c_vec_len(n->u.compare.ops) == 1) {
        P2C_AstOperator *op = (P2C_AstOperator*)p2c_vec_get(n->u.compare.ops, 0);
        P2C_AstExpr *right = (P2C_AstExpr*)p2c_vec_get(n->u.compare.comparators, 0);
        const char *func = "p2c_obj_eq";
        bool negate = false;
        bool swap_args = false;
        switch (*op) {
            case OP_LT: func = "p2c_obj_lt"; break;
            case OP_LE: func = "p2c_obj_le"; break;
            case OP_EQ: func = "p2c_obj_eq"; break;
            case OP_NE: func = "p2c_obj_ne"; break;
            case OP_GT: func = "p2c_obj_gt"; break;
            case OP_GE: func = "p2c_obj_ge"; break;
            case OP_IN: func = "p2c_obj_contains"; swap_args = true; break;
            case OP_NOTIN: func = "p2c_obj_contains"; swap_args = true; negate = true; break;
            case OP_IS: func = "p2c_obj_is"; break;
            case OP_ISNOT: func = "p2c_obj_is"; negate = true; break;
            default: break;
        }
        if (negate) write_str(cg, "p2c_bool_not(");
        write_str(cg, func); write_str(cg, "(");
        if (swap_args) { gen_suspension_expr(cg, right, locals); write_str(cg, ", "); gen_suspension_expr(cg, n->u.compare.left, locals); }
        else { gen_suspension_expr(cg, n->u.compare.left, locals); write_str(cg, ", "); gen_suspension_expr(cg, right, locals); }
        write_str(cg, ")");
        if (negate) write_str(cg, ")");
        return;
    }
    if (n->type == AST_ATTRIBUTE) {
        write_str(cg, "p2c_getattr(");
        gen_suspension_expr(cg, n->u.attribute.value, locals);
        write_str(cg, ", \""); write_str(cg, n->u.attribute.attr); write_str(cg, "\")");
        return;
    }
    if (n->type == AST_CALL && n->u.call.func && n->u.call.func->base.type == AST_ATTRIBUTE) {
        P2C_AstExpr *func = n->u.call.func;
        size_t argc = p2c_vec_len(n->u.call.args);
        write_str(cg, "p2c_call_attr(");
        gen_suspension_expr(cg, func->base.u.attribute.value, locals);
        write_str(cg, ", \""); write_str(cg, func->base.u.attribute.attr); write_str(cg, "\", ");
        if (argc == 0) {
            write_str(cg, "NULL, 0");
        } else {
            write_str(cg, "(P2C_Object*[]){");
            for (size_t i = 0; i < argc; i++) {
                if (i) write_str(cg, ", ");
                gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i), locals);
            }
            write_str(cg, "}, "); emit_usize(cg, argc);
        }
        write_str(cg, ")");
        return;
    }
    if (n->type == AST_CALL && n->u.call.func && n->u.call.func->base.type == AST_NAME) {
        const char *name = n->u.call.func->base.u.name.name;
        P2C_AstFunctionDef *callee = (P2C_AstFunctionDef*)p2c_map_get(cg->func_args, name);
        size_t argc = p2c_vec_len(n->u.call.args);
        if (strcmp(name, "print") == 0) {
            write_str(cg, "p2c_print_multi(");
            if (argc == 0) {
                write_str(cg, "NULL, 0");
            } else {
                write_str(cg, "(P2C_Object*[]){");
                for (size_t i = 0; i < argc; i++) {
                    if (i) write_str(cg, ", ");
                    gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i), locals);
                }
                write_str(cg, "}, "); emit_usize(cg, argc);
            }
            write_str(cg, ")");
        } else if (callee) {
            write_ident(cg, name); write_str(cg, "(");
            for (size_t i = 0; i < argc; i++) {
                if (i) write_str(cg, ", ");
                gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i), locals);
            }
            write_str(cg, ")");
        } else if (strcmp(name, "int") == 0 && argc == 1) {
            write_str(cg, "p2c_obj_from_int(p2c_obj_as_int(");
            gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0), locals); write_str(cg, "))");
        } else if (strcmp(name, "str") == 0 && argc == 1) {
            write_str(cg, "p2c_obj_str(");
            gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0), locals); write_str(cg, ")");
        } else if (is_builtin_callable_name(name)) {
            /* 組込み関数はgen_expr側に完全なディスパッチ（len/range/sum/sorted/
             * isinstance等の引数検査と変換）があるため、そのまま再利用する。
             * cg->genexpr_locals が設定されているので、引数の中のローカル名も
             * ジェネレータのローカルから解決される。 */
            gen_expr(cg, expr);
        } else {
            write_str(cg, "p2c_call("); write_ident(cg, name); write_str(cg, ", ");
            if (argc == 0) write_str(cg, "NULL, 0");
            else {
                write_str(cg, "(P2C_Object*[]){");
                for (size_t i = 0; i < argc; i++) {
                    if (i) write_str(cg, ", ");
                    gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i), locals);
                }
                write_str(cg, "}, "); emit_usize(cg, argc);
            }
            write_str(cg, ")");
        }
        return;
    }
    gen_expr(cg, expr);
}

/* ジェネレータ式・状態機械関数（async/generator）のステップ関数の中で式を生成する
 * 入口。生成の間だけ cg->genexpr_locals を設定し、gen_expr側の名前解決
 * （組込み関数の引数、タプル/辞書表示、添字、f-string等）でもジェネレータの
 * ローカル名を参照できるようにする。 */
static void gen_suspension_expr(P2C_CodeGen *cg, P2C_AstExpr *expr, P2C_Map *locals) {
    P2C_Map *saved_locals = cg->genexpr_locals;
    cg->genexpr_locals = locals;
    gen_suspension_expr_impl(cg, expr, locals);
    cg->genexpr_locals = saved_locals;
}

/* ジェネレータ式（genexpr）のfor節ターゲットが束縛する名前を集める。
 * タプル/リストのアンパック（for x, y in pairs）とその入れ子に対応する。 */
static bool gen_genexp_collect_target(P2C_Map *locals, P2C_Map *targets, P2C_AstExpr *target) {
    if (!target) return false;
    if (target->base.type == AST_NAME) {
        map_set_name(locals, target->base.u.name.name);
        map_set_name(targets, target->base.u.name.name);
        return true;
    }
    if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.type == AST_TUPLE ? target->base.u.tuple.elts : target->base.u.list.elts;
        for (size_t i = 0; i < p2c_vec_len(elts); i++) {
            if (!gen_genexp_collect_target(locals, targets, (P2C_AstExpr*)p2c_vec_get(elts, i))) return false;
        }
        return true;
    }
    return false;
}

/* ジェネレータ式のターゲット束縛。for節のターゲットは単純名だけでなく
 * タプル/リストのアンパック（for x, y in pairs）や入れ子も取り得るため、
 * 要素を添字で取り出してジェネレータのローカルへ書き戻す。nseqは入れ子の
 * 展開で作る一時変数名を一意にするための連番。 */
static void gen_genexp_bind_target(P2C_CodeGen *cg, int id, const char *item_expr, P2C_AstExpr *target, size_t *nseq) {
    if (!target) return;
    if (target->base.type == AST_NAME) {
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, target->base.u.name.name);
        write_str(cg, "\", "); write_str(cg, item_expr); write_str(cg, ");"); write_newline(cg);
        return;
    }
    if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.type == AST_TUPLE ? target->base.u.tuple.elts : target->base.u.list.elts;
        for (size_t i = 0; i < p2c_vec_len(elts); i++) {
            P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
            char item[64];
            if (!elt) continue;
            if (elt->base.type == AST_STARRED || elt->base.type == AST_ATTRIBUTE || elt->base.type == AST_SUBSCRIPT) {
                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                  "generator expression for-targets support names and tuple/list unpacking only");
                continue;
            }
            snprintf(item, sizeof(item), "_p2c_genexp_unpack_%d_%zu", id, (*nseq)++);
            indent(cg); write_str(cg, "P2C_Object *"); write_str(cg, item); write_str(cg, " = p2c_subscript_get(");
            write_str(cg, item_expr); write_str(cg, ", p2c_obj_from_int("); emit_usize(cg, i); write_str(cg, "));"); write_newline(cg);
            gen_genexp_bind_target(cg, id, item, elt, nseq);
        }
        return;
    }
    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                      "generator expression for-targets support names and tuple/list unpacking only");
}

/* ジェネレータ式を、複数のfor節・タプルターゲット・ifフィルタに対応した
 * ジェネレータ（ステップ関数）へ変換する。
 *
 * ステップ関数は入れ子ループを「今どの段（for節）を進めているか」を表す
 * levelローカルとswitchで表現する。
 *   - 段kのイテレータはジェネレータのローカルに保持し、未生成（None）なら
 *     その段に入った時点で作る。Pythonの「最も外側のiterableだけを生成時に
 *     評価し、内側はループ到達時に評価する」規則に対応する。
 *   - 最内段で要素をyieldし、再開時はlevelが最内段のままなので同じ
 *     イテレータから続きを取り出せる。
 *   - 段のイテレータが尽きたら（StopIteration）そのイテレータを捨てて1つ外側の
 *     段へ戻る。最外段が尽きたらジェネレータを終了する。
 */
static void gen_generator_expression(P2C_CodeGen *cg, P2C_AstExpr *expr) {
    P2C_Vector *generators = expr->base.u.comprehension.generators;
    P2C_Map *locals;
    P2C_Map *targets;
    P2C_String *saved_current;
    int saved_indent;
    int id;
    char step_name[64];
    char level_local[64];
    char iter_local[64];
    size_t capture_count = 0;
    size_t clause_count = generators ? p2c_vec_len(generators) : 0;
    if (cg->current == cg->forward && cg->closure_env_var) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "generator expression capture inside nested closures is not yet supported");
        write_str(cg, "&P2C_None");
        return;
    }
    if (clause_count == 0) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "generator expression requires at least one for-clause");
        write_str(cg, "&P2C_None");
        return;
    }
    /* 1節あたり1つのcase節を生成する実装のため、実用上限を設けて明示的に診断する。 */
    if (clause_count > 8) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "generator expression supports at most 8 for-clauses");
        write_str(cg, "&P2C_None");
        return;
    }
    locals = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    targets = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!locals || !targets) {
        codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate generator expression locals");
        free_name_map(locals);
        free_name_map(targets);
        write_str(cg, "&P2C_None");
        return;
    }
    for (size_t k = 0; k < clause_count; k++) {
        P2C_AstComprehensionGen *clause = (P2C_AstComprehensionGen*)p2c_vec_get(generators, k);
        if (!clause || !clause->target ||
            !gen_genexp_collect_target(locals, targets, clause->target)) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                              "generator expression for-targets support names and tuple/list unpacking only");
            free_name_map(locals);
            free_name_map(targets);
            write_str(cg, "&P2C_None");
            return;
        }
    }
    /* 外側スコープの名前（クロージャ変数・セル）を取り込む。ターゲット名は
     * ジェネレータのローカルが優先されるため除外する。 */
    if (cg->declared_vars) for (size_t i = 0; i < cg->declared_vars->bucket_count; i++) for (P2C_MapEntry *entry = cg->declared_vars->buckets[i]; entry; entry = entry->next) {
        const char *name = (const char*)entry->key;
        if (!map_has_name(locals, name)) { map_set_name(locals, name); capture_count++; }
    }
    if (cg->closure_env_names) for (size_t i = 0; i < cg->closure_env_names->bucket_count; i++) for (P2C_MapEntry *entry = cg->closure_env_names->buckets[i]; entry; entry = entry->next) {
        const char *name = (const char*)entry->key;
        if (!map_has_name(locals, name)) { map_set_name(locals, name); capture_count++; }
    }
    id = ++cg->generator_expression_counter;
    snprintf(step_name, sizeof(step_name), "_p2c_genexp_step_%d", id);
    snprintf(level_local, sizeof(level_local), "__p2c_genexp_level_%d", id);
    snprintf(iter_local, sizeof(iter_local), "__p2c_genexp_iter_%d_0", id);
    saved_current = cg->current;
    saved_indent = cg->indent_level;
    cg->current = cg->forward;
    cg->indent_level = 0;
    indent(cg); write_str(cg, "static P2C_Object *"); write_str(cg, step_name); write_str(cg, "(P2C_Object *generator) {"); write_newline(cg);
    push_indent(cg);
    write_line(cg, "for (;;) {");
    push_indent(cg);
    indent(cg); write_str(cg, "int _p2c_genexp_level = (int)p2c_obj_as_int(p2c_generator_local_get(generator, \""); write_str(cg, level_local); write_str(cg, "\"));"); write_newline(cg);
    write_line(cg, "switch (_p2c_genexp_level) {");
    push_indent(cg);
    for (size_t k = 0; k < clause_count; k++) {
        P2C_AstComprehensionGen *clause = (P2C_AstComprehensionGen*)p2c_vec_get(generators, k);
        char iter_name[64];
        char value_name[64];
        size_t bind_seq = 0;
        snprintf(iter_name, sizeof(iter_name), "__p2c_genexp_iter_%d_%zu", id, k);
        snprintf(value_name, sizeof(value_name), "_p2c_genexp_value_%zu", k);
        indent(cg); write_str(cg, "case "); emit_usize(cg, k); write_str(cg, ": {"); write_newline(cg);
        push_indent(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_genexp_iter_"); emit_usize(cg, k);
        write_str(cg, " = p2c_generator_local_get(generator, \""); write_str(cg, iter_name); write_str(cg, "\");"); write_newline(cg);
        indent(cg); write_str(cg, "if (_p2c_genexp_iter_"); emit_usize(cg, k); write_str(cg, " == &P2C_None) { _p2c_genexp_iter_");
        emit_usize(cg, k); write_str(cg, " = p2c_builtin_iter(");
        gen_suspension_expr(cg, clause->iter, locals);
        write_str(cg, "); p2c_generator_local_set(generator, \""); write_str(cg, iter_name);
        write_str(cg, "\", _p2c_genexp_iter_"); emit_usize(cg, k); write_str(cg, "); }"); write_newline(cg);
        /* next()は例外フレームで包む。StopIterationは段の終わりを意味する。 */
        indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_genexp_ef_"); emit_usize(cg, k); write_str(cg, ";"); write_newline(cg);
        indent(cg); write_str(cg, "P2C_Object * volatile _p2c_genexp_value_"); emit_usize(cg, k); write_str(cg, " = NULL;"); write_newline(cg);
        indent(cg); write_str(cg, "_p2c_genexp_ef_"); emit_usize(cg, k); write_str(cg, ".prev = p2c_exc_stack; _p2c_genexp_ef_");
        emit_usize(cg, k); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_genexp_ef_"); emit_usize(cg, k); write_str(cg, ";"); write_newline(cg);
        indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_genexp_ef_"); emit_usize(cg, k); write_str(cg, ".env) == 0) { _p2c_genexp_value_");
        emit_usize(cg, k); write_str(cg, " = p2c_builtin_next(_p2c_genexp_iter_"); emit_usize(cg, k);
        write_str(cg, "); p2c_exc_stack = _p2c_genexp_ef_"); emit_usize(cg, k); write_str(cg, ".prev; } else { P2C_Object *_p2c_genexp_exc_");
        emit_usize(cg, k); write_str(cg, " = _p2c_genexp_ef_"); emit_usize(cg, k); write_str(cg, ".exc; p2c_exc_stack = _p2c_genexp_ef_");
        emit_usize(cg, k); write_str(cg, ".prev; if (p2c_exc_name_match(_p2c_genexp_exc_"); emit_usize(cg, k); write_str(cg, ", \"StopIteration\")) { ");
        if (k == 0) {
            write_str(cg, "return p2c_generator_finish(generator, &P2C_None);");
        } else {
            write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, iter_name);
            write_str(cg, "\", &P2C_None); p2c_generator_local_set(generator, \""); write_str(cg, level_local);
            write_str(cg, "\", p2c_obj_from_int("); emit_usize(cg, k - 1); write_str(cg, ")); continue;");
        }
        write_str(cg, " } p2c_raise(_p2c_genexp_exc_"); emit_usize(cg, k);
        write_str(cg, "); return p2c_generator_finish(generator, &P2C_None); }"); write_newline(cg);
        gen_genexp_bind_target(cg, id, value_name, clause->target, &bind_seq);
        for (size_t i = 0; i < p2c_vec_len(clause->ifs); i++) {
            indent(cg); write_str(cg, "if (!p2c_obj_is_truthy(");
            gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(clause->ifs, i), locals);
            write_str(cg, ")) continue;"); write_newline(cg);
        }
        if (k + 1 < clause_count) {
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, level_local);
            write_str(cg, "\", p2c_obj_from_int("); emit_usize(cg, k + 1); write_str(cg, ")); continue;"); write_newline(cg);
        } else {
            indent(cg); write_str(cg, "return p2c_generator_yield(generator, ");
            gen_suspension_expr(cg, expr->base.u.comprehension.elt, locals);
            write_str(cg, ", 0);"); write_newline(cg);
        }
        pop_indent(cg); write_line(cg, "}");
    }
    pop_indent(cg); write_line(cg, "}");
    pop_indent(cg); write_line(cg, "}");
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
    cg->current = saved_current;
    cg->indent_level = saved_indent;
    /* ジェネレータ本体を作る。ローカルはlevel・最外段のイテレータと、
     * 外側スコープから取り込む名前。最外段のiterableはここで評価する
     * （Pythonでは最も外側のiterableだけが生成時に評価される）。 */
    write_str(cg, "p2c_generator_new_with_locals("); write_str(cg, step_name); write_str(cg, ", false, (const char*[]){\"");
    write_str(cg, level_local); write_str(cg, "\", \""); write_str(cg, iter_local); write_str(cg, "\"");
    for (size_t i = 0; i < locals->bucket_count; i++) for (P2C_MapEntry *entry = locals->buckets[i]; entry; entry = entry->next) {
        const char *name = (const char*)entry->key;
        if (map_has_name(targets, name)) continue;
        write_str(cg, ", \""); write_str(cg, name); write_str(cg, "\"");
    }
    write_str(cg, "}, (P2C_Object*[]){p2c_obj_from_int(0), p2c_builtin_iter(");
    gen_expr(cg, ((P2C_AstComprehensionGen*)p2c_vec_get(generators, 0))->iter);
    write_str(cg, ")");
    for (size_t i = 0; i < locals->bucket_count; i++) for (P2C_MapEntry *entry = locals->buckets[i]; entry; entry = entry->next) {
        const char *name = (const char*)entry->key;
        if (map_has_name(targets, name)) continue;
        write_str(cg, ", ");
        if (map_has_name(cg->closure_env_names, name) && !map_has_name(cg->declared_vars, name) && cg->closure_env_var) {
            write_str(cg, "p2c_cell_get(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", p2c_obj_from_str(\""); write_str(cg, name); write_str(cg, "\")))");
        } else if (map_has_name(cg->cell_names, name)) {
            write_str(cg, "p2c_cell_get(_p2c_cell_"); write_ident(cg, name); write_str(cg, ")");
        } else {
            write_ident(cg, name);
        }
    }
    write_str(cg, "}, "); emit_usize(cg, capture_count + 2); write_str(cg, ")");
    free_name_map(locals);
    free_name_map(targets);
}


static void gen_suspension_local_set(P2C_CodeGen *cg, P2C_AstExpr *target, P2C_AstExpr *value, P2C_Map *locals) {
    if (!target || target->base.type != AST_NAME) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "state-machine functions currently require a simple name assignment target");
        return;
    }
    indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, target->base.u.name.name); write_str(cg, "\", ");
    gen_suspension_expr(cg, value, locals); write_str(cg, ");"); write_newline(cg);
}

static void gen_suspension_inline_stmt_list(P2C_CodeGen *cg, P2C_Vector *stmts, P2C_Map *locals, size_t loop_state, size_t after_state, const char *step_name) {
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt || stmt->base.type == AST_PASS) continue;
        if (stmt->base.type == AST_ASSIGN && p2c_vec_len(stmt->base.u.assign.targets) == 1) {
            gen_suspension_local_set(cg, (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, 0), stmt->base.u.assign.value, locals);
        } else if (stmt->base.type == AST_AUGASSIGN && stmt->base.u.augassign.target && stmt->base.u.augassign.target->base.type == AST_NAME) {
            const char *func = "p2c_obj_add";
            switch (stmt->base.u.augassign.op) {
                case OP_ADD: func = "p2c_obj_add"; break;
                case OP_SUB: func = "p2c_obj_sub"; break;
                case OP_MULT: func = "p2c_obj_mul"; break;
                case OP_DIV: func = "p2c_obj_div"; break;
                case OP_FLOORDIV: func = "p2c_obj_floordiv"; break;
                case OP_MOD: func = "p2c_obj_mod"; break;
                default: codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async for augmented assignment operator is not supported"); break;
            }
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, stmt->base.u.augassign.target->base.u.name.name); write_str(cg, "\", "); write_str(cg, func); write_str(cg, "(p2c_generator_local_get(generator, \""); write_str(cg, stmt->base.u.augassign.target->base.u.name.name); write_str(cg, "\"), "); gen_suspension_expr(cg, stmt->base.u.augassign.value, locals); write_str(cg, "));"); write_newline(cg);
        } else if (stmt->base.type == AST_IF) {
            indent(cg); write_str(cg, "if (p2c_obj_is_truthy("); gen_suspension_expr(cg, stmt->base.u.if_stmt.test, locals); write_str(cg, ")) {"); write_newline(cg); push_indent(cg);
            gen_suspension_inline_stmt_list(cg, stmt->base.u.if_stmt.body, locals, loop_state, after_state, step_name);
            pop_indent(cg); indent(cg); write_str(cg, "} else {"); write_newline(cg); push_indent(cg);
            gen_suspension_inline_stmt_list(cg, stmt->base.u.if_stmt.orelse, locals, loop_state, after_state, step_name);
            pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
        } else if (stmt->base.type == AST_BREAK) {
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, after_state); write_str(cg, "); return "); write_ident(cg, step_name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else if (stmt->base.type == AST_CONTINUE) {
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, loop_state); write_str(cg, "); return "); write_ident(cg, step_name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else if (stmt->base.type == AST_EXPR_STMT) {
            indent(cg); write_str(cg, "(void)"); gen_suspension_expr(cg, stmt->base.u.expr_stmt.value, locals); write_str(cg, ";"); write_newline(cg);
        } else if (stmt->base.type == AST_RAISE) {
            indent(cg);
            if (!stmt->base.u.raise.exc) {
                write_str(cg, "p2c_reraise();");
            } else {
                write_str(cg, "p2c_raise(");
                if (stmt->base.u.raise.cause) {
                    write_str(cg, "p2c_exception_with_cause(");
                    gen_raise_target(cg, stmt->base.u.raise.exc);
                    write_str(cg, ", ");
                    gen_suspension_expr(cg, stmt->base.u.raise.cause, locals);
                    write_str(cg, ")");
                } else {
                    gen_raise_target(cg, stmt->base.u.raise.exc);
                }
                write_str(cg, ");");
            }
            write_newline(cg);
        } else {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "suspension state-machine body currently supports assignment, augmented assignment, if, break, continue, expression, raise, and pass statements");
        }
    }
}

static size_t suspension_stmt_width(P2C_AstStmt *stmt) {
    if (stmt && stmt->base.type == AST_WITH && stmt->base.u.with.is_async) {
        size_t count = p2c_vec_len(stmt->base.u.with.items);
        return count > 0 ? count * 3 + 1 : 1;
    }
    return 3;
}

static size_t suspension_state_base(P2C_AstFunctionDef *fd, size_t index) {
    size_t base = 0;
    for (size_t i = 0; i < index; i++) {
        base += suspension_stmt_width((P2C_AstStmt*)p2c_vec_get(fd->body, i));
    }
    return base;
}

static void gen_async_with_states(P2C_CodeGen *cg, P2C_AstFunctionDef *fd, P2C_Map *locals, size_t index) {
    P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, index);
    P2C_AstWith *with_stmt = &stmt->base.u.with;
    size_t count = p2c_vec_len(with_stmt->items);
    size_t base = suspension_state_base(fd, index);
    size_t body_state = base + count * 2;
    size_t next_state = suspension_state_base(fd, index + 1);

    if (count == 0) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async with requires at least one context manager");
        return;
    }
    for (size_t j = 0; j < count; j++) {
        P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(with_stmt->items, j);
        if (!item || (item->optional_vars && item->optional_vars->base.type != AST_NAME)) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async with state-machine lowering requires simple name targets");
            return;
        }
        size_t enter_state = base + j * 2;
        size_t entered_state = enter_state + 1;
        if (j > 0) {
            indent(cg); write_str(cg, "case "); emit_usize(cg, enter_state); write_str(cg, ": {"); write_newline(cg); push_indent(cg);
            indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, " = p2c_generator_local_get(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, "\");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, " == &P2C_None) { _p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, " = "); gen_suspension_expr(cg, item->context_expr, locals); write_str(cg, "; p2c_generator_local_set(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, "\", _p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, "); }"); write_newline(cg);
            indent(cg); write_str(cg, "return p2c_generator_await(generator, p2c_async_call_attr(_p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, ", \"__aenter__\", NULL, 0), "); emit_usize(cg, entered_state); write_str(cg, ");"); write_newline(cg);
            pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
        }

        indent(cg); write_str(cg, "case "); emit_usize(cg, entered_state); write_str(cg, ": {"); write_newline(cg); push_indent(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_value_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, " = p2c_generator_await_result(generator);"); write_newline(cg);
        if (item->optional_vars) {
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, item->optional_vars->base.u.name.name); write_str(cg, "\", _p2c_async_with_value_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, ");"); write_newline(cg);
        } else {
            indent(cg); write_str(cg, "(void)_p2c_async_with_value_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, ";"); write_newline(cg);
        }
        indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, j + 1 < count ? base + (j + 1) * 2 : body_state); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    }

    indent(cg); write_str(cg, "case "); emit_usize(cg, body_state); write_str(cg, ": {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, "; _p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ".prev = p2c_exc_stack; _p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ".env) == 0) {"); write_newline(cg); push_indent(cg);
    gen_suspension_inline_stmt_list(cg, with_stmt->body, locals, next_state, next_state, fd->name);
    indent(cg); write_str(cg, "p2c_exc_stack = _p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ".prev; p2c_generator_local_set(generator, \"__p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, "\", &P2C_None);"); write_newline(cg);
    pop_indent(cg); indent(cg); write_str(cg, "} else {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, " = _p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ".exc; p2c_exc_stack = _p2c_async_with_ef_"); emit_usize(cg, index); write_str(cg, ".prev; p2c_generator_local_set(generator, \"__p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, "\", _p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, ");"); write_newline(cg);
    pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " = p2c_generator_local_get(generator, \"__p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, "\");"); write_newline(cg);
    indent(cg); write_str(cg, "if (_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " != &P2C_None) return p2c_generator_await(generator, p2c_async_call_attr(p2c_generator_local_get(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, count - 1); write_str(cg, "\"), \"__aexit__\", (P2C_Object*[]){p2c_builtin_type(_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, "), _p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, ", &P2C_None}, 3), "); emit_usize(cg, body_state + 1); write_str(cg, ");"); write_newline(cg);
    indent(cg); write_str(cg, "return p2c_generator_await(generator, p2c_async_call_attr(p2c_generator_local_get(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, count - 1); write_str(cg, "\"), \"__aexit__\", (P2C_Object*[]){&P2C_None, &P2C_None, &P2C_None}, 3), "); emit_usize(cg, body_state + 1); write_str(cg, ");"); write_newline(cg);
    pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);

    for (size_t step = 0; step < count; step++) {
        size_t j = count - 1 - step;
        size_t exit_state = body_state + 1 + step;
        indent(cg); write_str(cg, "case "); emit_usize(cg, exit_state); write_str(cg, ": {"); write_newline(cg); push_indent(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_suppress_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, " = p2c_generator_await_result(generator);"); write_newline(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " = p2c_generator_local_get(generator, \"__p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, "\");"); write_newline(cg);
        indent(cg); write_str(cg, "if (_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " != &P2C_None && p2c_obj_is_truthy(_p2c_async_with_suppress_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j); write_str(cg, ")) { p2c_generator_local_set(generator, \"__p2c_async_with_exc_"); emit_usize(cg, index); write_str(cg, "\", &P2C_None); _p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " = &P2C_None; }"); write_newline(cg);
        if (j > 0) {
            indent(cg); write_str(cg, "if (_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " != &P2C_None) return p2c_generator_await(generator, p2c_async_call_attr(p2c_generator_local_get(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j - 1); write_str(cg, "\"), \"__aexit__\", (P2C_Object*[]){p2c_builtin_type(_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, "), _p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, ", &P2C_None}, 3), "); emit_usize(cg, exit_state + 1); write_str(cg, ");"); write_newline(cg);
            indent(cg); write_str(cg, "return p2c_generator_await(generator, p2c_async_call_attr(p2c_generator_local_get(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, index); write_str(cg, "_"); emit_usize(cg, j - 1); write_str(cg, "\"), \"__aexit__\", (P2C_Object*[]){&P2C_None, &P2C_None, &P2C_None}, 3), "); emit_usize(cg, exit_state + 1); write_str(cg, ");"); write_newline(cg);
        } else {
            indent(cg); write_str(cg, "if (_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, " != &P2C_None) p2c_raise(_p2c_async_with_pending_"); emit_usize(cg, index); write_str(cg, ");"); write_newline(cg);
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, next_state); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        }
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    }
}

static void gen_suspension_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    P2C_Map *locals = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!locals) { codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate state-machine local map"); return; }
    suspension_collect_locals(locals, fd);
    size_t argc = p2c_vec_len(fd->args);

    p2c_str_append(cg->forward, "static P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->name)); p2c_str_append(cg->forward, "__step(P2C_Object *generator);\n");
    p2c_str_append(cg->forward, "static P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->name)); p2c_str_append(cg->forward, "(");
    for (size_t i = 0; i < argc; i++) {
        if (i) p2c_str_append(cg->forward, ", ");
        p2c_str_append(cg->forward, "P2C_Object *");
        p2c_str_append(cg->forward, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    }
    if (argc == 0) p2c_str_append(cg->forward, "void");
    p2c_str_append(cg->forward, ");\n");

    indent(cg); write_str(cg, "static P2C_Object *"); write_ident(cg, fd->name); write_str(cg, "__step(P2C_Object *generator) {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "switch (p2c_generator_state(generator)) {"); write_newline(cg); push_indent(cg);
    for (size_t i = 0; i < p2c_vec_len(fd->body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, i);
        indent(cg); write_str(cg, "case "); emit_usize(cg, suspension_state_base(fd, i)); write_str(cg, ": {"); write_newline(cg); push_indent(cg);
        if (i > 0) {
            P2C_AstStmt *prev = (P2C_AstStmt*)p2c_vec_get(fd->body, i - 1);
            if (prev && prev->base.type == AST_ASSIGN && prev->base.u.assign.value && find_await_expr(prev->base.u.assign.value) && p2c_vec_len(prev->base.u.assign.targets) == 1) {
                gen_suspension_local_set(cg, (P2C_AstExpr*)p2c_vec_get(prev->base.u.assign.targets, 0), prev->base.u.assign.value, locals);
            } else if (prev && prev->base.type == AST_EXPR_STMT && prev->base.u.expr_stmt.value && prev->base.u.expr_stmt.value->base.type == AST_AWAIT) {
                indent(cg); write_str(cg, "(void)p2c_generator_await_result(generator);"); write_newline(cg);
            }
        }
        if (!stmt || stmt->base.type == AST_PASS) {
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else if (stmt->base.type == AST_EXPR_STMT && stmt->base.u.expr_stmt.value && stmt->base.u.expr_stmt.value->base.type == AST_YIELD && stmt->base.u.expr_stmt.value->base.u.yield_expr.from) {
            P2C_AstExpr *value = stmt->base.u.expr_stmt.value->base.u.yield_expr.value;
            indent(cg); write_str(cg, "P2C_Object *_p2c_yield_from_iter_"); emit_usize(cg, i); write_str(cg, " = p2c_generator_local_get(generator, \"__p2c_yield_from_"); emit_usize(cg, i); write_str(cg, "\");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_yield_from_iter_"); emit_usize(cg, i); write_str(cg, " == &P2C_None) { _p2c_yield_from_iter_"); emit_usize(cg, i); write_str(cg, " = p2c_builtin_iter("); gen_suspension_expr(cg, value, locals); write_str(cg, "); p2c_generator_local_set(generator, \"__p2c_yield_from_"); emit_usize(cg, i); write_str(cg, "\", _p2c_yield_from_iter_"); emit_usize(cg, i); write_str(cg, "); }"); write_newline(cg);
            indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, "; _p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ".prev = p2c_exc_stack; _p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ".env) == 0) { P2C_Object *_p2c_yield_from_value_"); emit_usize(cg, i); write_str(cg, " = p2c_builtin_next(_p2c_yield_from_iter_"); emit_usize(cg, i); write_str(cg, "); p2c_exc_stack = _p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ".prev; return p2c_generator_yield(generator, _p2c_yield_from_value_"); emit_usize(cg, i); write_str(cg, ", "); emit_usize(cg, suspension_state_base(fd, i)); write_str(cg, "); }"); write_newline(cg);
            indent(cg); write_str(cg, "P2C_Object *_p2c_yield_from_exc_"); emit_usize(cg, i); write_str(cg, " = _p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ".exc; p2c_exc_stack = _p2c_yield_from_ef_"); emit_usize(cg, i); write_str(cg, ".prev;"); write_newline(cg);
            indent(cg); write_str(cg, "if (!p2c_exc_name_match(_p2c_yield_from_exc_"); emit_usize(cg, i); write_str(cg, ", \"StopIteration\")) p2c_raise(_p2c_yield_from_exc_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg);
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \"__p2c_yield_from_"); emit_usize(cg, i); write_str(cg, "\", &P2C_None); p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else if (stmt->base.type == AST_EXPR_STMT && stmt->base.u.expr_stmt.value && stmt->base.u.expr_stmt.value->base.type == AST_YIELD) {
            P2C_AstExpr *value = stmt->base.u.expr_stmt.value->base.u.yield_expr.value;
            indent(cg); write_str(cg, "return p2c_generator_yield(generator, "); gen_suspension_expr(cg, value, locals); write_str(cg, ", "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, ");"); write_newline(cg);
        } else if (stmt->base.type == AST_ASSIGN && stmt->base.u.assign.value && find_await_expr(stmt->base.u.assign.value) && p2c_vec_len(stmt->base.u.assign.targets) == 1) {
            P2C_AstExpr *_p2c_await = find_await_expr(stmt->base.u.assign.value);
            indent(cg); write_str(cg, "return p2c_generator_await(generator, "); gen_suspension_expr(cg, _p2c_await->base.u.await_expr.value, locals); write_str(cg, ", "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, ");"); write_newline(cg);
        } else if (stmt->base.type == AST_EXPR_STMT && stmt->base.u.expr_stmt.value && stmt->base.u.expr_stmt.value->base.type == AST_AWAIT) {
            indent(cg); write_str(cg, "return p2c_generator_await(generator, "); gen_suspension_expr(cg, stmt->base.u.expr_stmt.value->base.u.await_expr.value, locals); write_str(cg, ", "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, ");"); write_newline(cg);
        } else if (stmt->base.type == AST_ASYNC_FOR) {
            indent(cg); write_str(cg, "P2C_Object *_p2c_async_iter_"); emit_usize(cg, i); write_str(cg, " = p2c_generator_local_get(generator, \"__p2c_async_for_iter_"); emit_usize(cg, i); write_str(cg, "\");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_async_iter_"); emit_usize(cg, i); write_str(cg, " == &P2C_None) { _p2c_async_iter_"); emit_usize(cg, i); write_str(cg, " = p2c_async_iter("); gen_suspension_expr(cg, stmt->base.u.for_stmt.iter, locals); write_str(cg, "); p2c_generator_local_set(generator, \"__p2c_async_for_iter_"); emit_usize(cg, i); write_str(cg, "\", _p2c_async_iter_"); emit_usize(cg, i); write_str(cg, "); }"); write_newline(cg);
            indent(cg); write_str(cg, "return p2c_generator_await(generator, p2c_async_next(_p2c_async_iter_"); emit_usize(cg, i); write_str(cg, "), "); emit_usize(cg, suspension_state_base(fd, i) + 1); write_str(cg, ");"); write_newline(cg);
        } else if (stmt->base.type == AST_WITH && stmt->base.u.with.is_async) {
            size_t item_count = p2c_vec_len(stmt->base.u.with.items);
            if (item_count == 0) {
                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async with requires at least one context manager");
            } else {
                P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(stmt->base.u.with.items, 0);
                bool valid_items = item != NULL;
                for (size_t j = 0; j < item_count; j++) {
                    P2C_AstWithItem *candidate = (P2C_AstWithItem*)p2c_vec_get(stmt->base.u.with.items, j);
                    if (!candidate || (candidate->optional_vars && candidate->optional_vars->base.type != AST_NAME)) valid_items = false;
                }
                if (!valid_items) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async with state-machine lowering requires simple name targets");
                } else {
                    size_t with_base = suspension_state_base(fd, i);
                    indent(cg); write_str(cg, "P2C_Object *_p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0 = p2c_generator_local_get(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0\");"); write_newline(cg);
                    indent(cg); write_str(cg, "if (_p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0 == &P2C_None) { _p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0 = "); gen_suspension_expr(cg, item->context_expr, locals); write_str(cg, "; p2c_generator_local_set(generator, \"__p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0\", _p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0); }"); write_newline(cg);
                    indent(cg); write_str(cg, "return p2c_generator_await(generator, p2c_async_call_attr(_p2c_async_with_ctx_"); emit_usize(cg, i); write_str(cg, "_0, \"__aenter__\", NULL, 0), "); emit_usize(cg, with_base + 1); write_str(cg, ");"); write_newline(cg);
                }
            }
        } else if (stmt->base.type == AST_ASSIGN && p2c_vec_len(stmt->base.u.assign.targets) == 1) {
            gen_suspension_local_set(cg, (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, 0), stmt->base.u.assign.value, locals);
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else if (stmt->base.type == AST_RETURN) {
            indent(cg); write_str(cg, "return p2c_generator_finish(generator, "); gen_suspension_expr(cg, stmt->base.u.return_stmt.value, locals); write_str(cg, ");"); write_newline(cg);
        } else if (stmt->base.type == AST_EXPR_STMT) {
            indent(cg); write_str(cg, "(void)"); gen_suspension_expr(cg, stmt->base.u.expr_stmt.value, locals); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else {
            indent(cg); write_str(cg, "p2c_raise(p2c_make_exception(\"NotImplementedError\", \"suspension inside this statement is not yet supported\"));"); write_newline(cg);
            write_line(cg, "return p2c_generator_finish(generator, &P2C_None);");
        }
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    }
    for (size_t i = 0; i < p2c_vec_len(fd->body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, i);
        if (!stmt || stmt->base.type != AST_ASYNC_FOR) continue;
        if (!stmt->base.u.for_stmt.target || stmt->base.u.for_stmt.target->base.type != AST_NAME) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async for state-machine lowering currently requires a simple name target");
            continue;
        }
        indent(cg); write_str(cg, "case "); emit_usize(cg, suspension_state_base(fd, i) + 1); write_str(cg, ": {"); write_newline(cg); push_indent(cg);
        indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, "; P2C_Object *_p2c_async_for_item_"); emit_usize(cg, i); write_str(cg, " = NULL;"); write_newline(cg);
        indent(cg); write_str(cg, "_p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ".prev = p2c_exc_stack; _p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ";"); write_newline(cg);
        indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ".env) == 0) { _p2c_async_for_item_"); emit_usize(cg, i); write_str(cg, " = p2c_generator_await_result(generator); p2c_exc_stack = _p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ".prev; } else { P2C_Object *_p2c_async_for_exc_"); emit_usize(cg, i); write_str(cg, " = _p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ".exc; p2c_exc_stack = _p2c_async_for_ef_"); emit_usize(cg, i); write_str(cg, ".prev; if (!p2c_exc_name_match(_p2c_async_for_exc_"); emit_usize(cg, i); write_str(cg, ", \"StopAsyncIteration\")) p2c_raise(_p2c_async_for_exc_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg);
        gen_suspension_inline_stmt_list(cg, stmt->base.u.for_stmt.orelse, locals, i * 3, (i + 1) * 3, fd->name);
        indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator); }"); write_newline(cg);
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, stmt->base.u.for_stmt.target->base.u.name.name); write_str(cg, "\", _p2c_async_for_item_"); emit_usize(cg, i); write_str(cg, ");"); write_newline(cg);
        gen_suspension_inline_stmt_list(cg, stmt->base.u.for_stmt.body, locals, i * 3, (i + 1) * 3, fd->name);
        indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    }
    for (size_t i = 0; i < p2c_vec_len(fd->body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, i);
        if (stmt && stmt->base.type == AST_WITH && stmt->base.u.with.is_async) gen_async_with_states(cg, fd, locals, i);
    }
    indent(cg); write_str(cg, "default: return p2c_generator_finish(generator, &P2C_None);"); write_newline(cg);
    pop_indent(cg); write_line(cg, "}");
    write_line(cg, "return p2c_generator_finish(generator, &P2C_None);");
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);

    indent(cg); write_str(cg, "static P2C_Object *"); write_ident(cg, fd->name); write_str(cg, "(");
    for (size_t i = 0; i < argc; i++) {
        if (i) write_str(cg, ", ");
        write_str(cg, "P2C_Object *"); write_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    }
    if (argc == 0) write_str(cg, "void");
    write_str(cg, ") {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *generator = p2c_generator_new("); write_ident(cg, fd->name); write_str(cg, "__step, "); write_str(cg, fd->is_async ? "true" : "false"); write_str(cg, ");"); write_newline(cg);
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, arg->name); write_str(cg, "\", "); write_ident(cg, arg->name); write_str(cg, ");"); write_newline(cg);
    }
    write_line(cg, "return generator;");
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
    free_name_map(locals);
}

static void gen_nested_closure(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    P2C_Map *captures = cg->declared_vars;
    P2C_Map *saved_declared;
    P2C_Map *saved_env_names;
    const char *saved_env_var;
    P2C_String *saved_current;
    int saved_indent;
    int id = ++cg->lambda_counter;
    char entry_name[64];
    size_t capture_count = 0;
    snprintf(entry_name, sizeof(entry_name), "_p2c_closure_entry_%d", id);
    if (captures) {
        for (size_t i = 0; i < captures->bucket_count; i++) {
            for (P2C_MapEntry *e = captures->buckets[i]; e; e = e->next) capture_count++;
        }
    }
    saved_current = cg->current;
    saved_indent = cg->indent_level;
    cg->current = cg->forward;
    cg->indent_level = 0;
    write_str(cg, "static P2C_Object *"); write_str(cg, entry_name); write_str(cg, "(P2C_Object *env, P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "(void)env; (void)args; (void)nargs;"); write_newline(cg);
    size_t argc = p2c_vec_len(fd->args);
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, arg->name); write_str(cg, " = args["); emit_usize(cg, i); write_str(cg, "]; (void)"); write_ident(cg, arg->name); write_str(cg, ";"); write_newline(cg);
    }
    saved_declared = cg->declared_vars;
    saved_env_names = cg->closure_env_names;
    saved_env_var = cg->closure_env_var;
    P2C_Map *saved_nonlocal_names = cg->nonlocal_names;
    P2C_Map *saved_cell_names = cg->cell_names;
    cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    cg->nonlocal_names = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    cg->cell_names = collect_nested_nonlocal_cell_names(cg, fd->body);
    for (size_t i = 0; i < argc; i++) remember_declared(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    collect_global_decls(fd->body, cg);
    predeclare_stmt_list(cg, fd->body);
    emit_cell_declarations(cg, cg->cell_names);
    cg->closure_env_names = captures;
    cg->closure_env_var = "env";
    /* 入れ子関数のreturn/break/continueは外側のtry/finallyを脱出しないため、
     * クリーンアップフレームとループ深さをリセットして生成する。 */
    struct P2C_CleanupFrame *saved_cleanup_top = cg->cleanup_top;
    int saved_loop_depth = cg->loop_depth;
    cg->cleanup_top = NULL;
    cg->loop_depth = 0;
    cg->function_depth++;
    gen_stmt_list(cg, fd->body);
    cg->function_depth--;
    cg->cleanup_top = saved_cleanup_top;
    cg->loop_depth = saved_loop_depth;
    if (p2c_vec_len(fd->body) == 0 || ((P2C_AstStmt*)p2c_vec_get(fd->body, p2c_vec_len(fd->body) - 1))->base.type != AST_RETURN) write_line(cg, "return &P2C_None;");
    p2c_map_free(cg->declared_vars);
    cg->declared_vars = saved_declared;
    free_name_map(cg->nonlocal_names);
    cg->nonlocal_names = saved_nonlocal_names;
    free_name_map(cg->cell_names);
    cg->cell_names = saved_cell_names;
    cg->closure_env_names = saved_env_names;
    cg->closure_env_var = saved_env_var;
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
    cg->current = saved_current;
    cg->indent_level = saved_indent;
    indent(cg); write_ident(cg, fd->name); write_str(cg, " = p2c_closure_new(\""); write_str(cg, fd->name); write_str(cg, "\", "); write_str(cg, entry_name); write_str(cg, ", ");
    if (capture_count == 0) {
        write_str(cg, "p2c_dict_from_pairs(NULL, NULL, 0)");
    } else {
        write_str(cg, "p2c_dict_from_pairs((P2C_Object*[]){");
        bool first = true;
        for (size_t i = 0; i < captures->bucket_count; i++) {
            for (P2C_MapEntry *e = captures->buckets[i]; e; e = e->next) {
                if (!first) write_str(cg, ", ");
                first = false;
                write_str(cg, "p2c_obj_from_str(\""); write_str(cg, (const char*)e->key); write_str(cg, "\")");
            }
        }
        write_str(cg, "}, (P2C_Object*[]){");
        first = true;
        for (size_t i = 0; i < captures->bucket_count; i++) {
            for (P2C_MapEntry *e = captures->buckets[i]; e; e = e->next) {
                if (!first) write_str(cg, ", ");
                first = false;
                if (map_has_name(cg->cell_names, (const char*)e->key)) {
                    write_str(cg, "_p2c_cell_"); write_ident(cg, (const char*)e->key);
                } else {
                    write_str(cg, "p2c_cell_new("); write_ident(cg, (const char*)e->key); write_str(cg, ")");
                }
            }
        }
        write_str(cg, "}, "); emit_usize(cg, capture_count); write_str(cg, ")");
    }
    write_str(cg, ");"); write_newline(cg);
}

static void gen_decorator_callable_adapter(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    if (fd->vararg || fd->kwarg) return;
    size_t argc = p2c_vec_len(fd->args);
    size_t required = 0;
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (!arg->default_val) required++;
    }
    indent(cg); write_str(cg, "static P2C_Object *"); write_ident(cg, fd->name); write_str(cg, "__decorator_adapter(P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
    if (argc == 0) write_line(cg, "(void)args;");
    indent(cg); write_str(cg, "if (");
    if (required > 0) { write_str(cg, "nargs < "); emit_usize(cg, required); write_str(cg, " || "); }
    write_str(cg, "nargs > "); emit_usize(cg, argc); write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"wrong function arity for decorated function\"));"); write_newline(cg);
    indent(cg); write_str(cg, "return "); write_ident(cg, fd->name); write_str(cg, "(");
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (i) write_str(cg, ", ");
        write_str(cg, "(nargs > "); emit_usize(cg, i); write_str(cg, ") ? args["); emit_usize(cg, i); write_str(cg, "] : ");
        if (arg->default_val) gen_expr(cg, arg->default_val); else write_str(cg, "&P2C_None");
    }
    write_str(cg, ");"); write_newline(cg);
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
}

static void gen_decorator_expr(P2C_CodeGen *cg, P2C_AstExpr *expr) {
    if (expr && expr->base.type == AST_NAME && map_has_name(cg->module_function_names, expr->base.u.name.name) && !map_has_name(cg->decorated_names, expr->base.u.name.name)) {
        P2C_AstFunctionDef *decorator_def = (P2C_AstFunctionDef*)p2c_map_get(cg->func_args, expr->base.u.name.name);
        if (decorator_def && (decorator_def->vararg || decorator_def->kwarg)) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "functions with *args or **kwargs cannot be used as decorators yet");
            write_str(cg, "&P2C_None");
        } else {
            write_str(cg, "p2c_function_new(\""); write_str(cg, expr->base.u.name.name); write_str(cg, "\", "); write_ident(cg, expr->base.u.name.name); write_str(cg, "__decorator_adapter)");
        }
    } else {
        gen_expr(cg, expr);
    }
}

static void gen_decorator_application(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    size_t decorator_count = p2c_vec_len(fd->decorator_list);
    if (decorator_count == 0) return;
    int binding_id = ++cg->temp_counter;
    for (size_t i = 0; i < decorator_count; i++) {
        indent(cg); write_str(cg, "P2C_Object *_p2c_decorator_"); emit_usize(cg, (size_t)binding_id); write_str(cg, "_"); emit_usize(cg, i); write_str(cg, " = ");
        gen_decorator_expr(cg, (P2C_AstExpr*)p2c_vec_get(fd->decorator_list, i));
        write_str(cg, ";"); write_newline(cg);
    }
    indent(cg); write_str(cg, "P2C_Object *_p2c_decorated_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, " = p2c_function_new(\""); write_str(cg, fd->name); write_str(cg, "\", "); write_ident(cg, fd->name); write_str(cg, "__decorator_adapter);"); write_newline(cg);
    for (size_t i = decorator_count; i > 0; i--) {
        indent(cg); write_str(cg, "_p2c_decorated_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, " = p2c_call(_p2c_decorator_"); emit_usize(cg, (size_t)binding_id); write_str(cg, "_"); emit_usize(cg, i - 1);
        write_str(cg, ", (P2C_Object*[]){_p2c_decorated_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, "}, 1);"); write_newline(cg);
    }
    indent(cg); write_str(cg, "_p2c_decorated_"); write_ident(cg, fd->name); write_str(cg, " = _p2c_decorated_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, ";"); write_newline(cg);
}

static void gen_class_decorator_application(P2C_CodeGen *cg, P2C_AstClassDef *cd) {
    size_t decorator_count = p2c_vec_len(cd->decorator_list);
    if (decorator_count == 0) return;
    int binding_id = ++cg->temp_counter;
    for (size_t i = 0; i < decorator_count; i++) {
        indent(cg); write_str(cg, "P2C_Object *_p2c_class_decorator_"); emit_usize(cg, (size_t)binding_id); write_str(cg, "_"); emit_usize(cg, i); write_str(cg, " = ");
        gen_decorator_expr(cg, (P2C_AstExpr*)p2c_vec_get(cd->decorator_list, i));
        write_str(cg, ";"); write_newline(cg);
    }
    indent(cg); write_str(cg, "P2C_Object *_p2c_decorated_class_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, " = "); write_ident(cg, cd->name); write_str(cg, ";"); write_newline(cg);
    for (size_t i = decorator_count; i > 0; i--) {
        indent(cg); write_str(cg, "_p2c_decorated_class_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, " = p2c_call(_p2c_class_decorator_"); emit_usize(cg, (size_t)binding_id); write_str(cg, "_"); emit_usize(cg, i - 1);
        write_str(cg, ", (P2C_Object*[]){_p2c_decorated_class_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, "}, 1);"); write_newline(cg);
    }
    indent(cg); write_str(cg, "_p2c_decorated_"); write_ident(cg, cd->name); write_str(cg, " = _p2c_decorated_class_value_"); emit_usize(cg, (size_t)binding_id); write_str(cg, ";"); write_newline(cg);
}

static void gen_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) {
    if (!stmt) return;
    P2C_AstNode *n = &stmt->base;
    emit_debug_comment(cg, n);
    switch (n->type) {
        case AST_ASSIGN:
            if (p2c_vec_len(n->u.assign.targets) > 1) {
                indent(cg); write_str(cg, "P2C_Object *_p2c_assign_tmp = "); gen_expr(cg, n->u.assign.value); write_str(cg, ";"); write_newline(cg);
                for (size_t i = 0; i < p2c_vec_len(n->u.assign.targets); i++) {
                    P2C_AstExpr *t = (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, i);
                    indent(cg);
                    if (t->base.type == AST_NAME) { write_ident(cg, t->base.u.name.name); write_str(cg, " = _p2c_assign_tmp;"); }
                    else if (t->base.type == AST_ATTRIBUTE) { write_str(cg, "p2c_setattr("); gen_expr(cg, t->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, t->base.u.attribute.attr); write_str(cg, "\", _p2c_assign_tmp);"); }
                    else if (t->base.type == AST_SUBSCRIPT) { write_str(cg, "p2c_subscript_set("); gen_expr(cg, t->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, t->base.u.subscript.slice); write_str(cg, ", _p2c_assign_tmp);"); }
                    write_newline(cg);
                }
            } else if (p2c_vec_len(n->u.assign.targets) == 1) {
                gen_assign_target(cg, (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, 0), n->u.assign.value);
            }
            break;
        case AST_ANNASSIGN:
            if (n->u.annassign.value) gen_assign_target(cg, n->u.annassign.target, n->u.annassign.value);
            break;
        case AST_AUGASSIGN: {
            const char *func = "p2c_obj_add";
            switch (n->u.augassign.op) {
                case OP_ADD: func = "p2c_obj_add"; break;
                case OP_SUB: func = "p2c_obj_sub"; break;
                case OP_MULT: func = "p2c_obj_mul"; break;
                case OP_DIV: func = "p2c_obj_div"; break;
                case OP_FLOORDIV: func = "p2c_obj_floordiv"; break;
                case OP_MOD: func = "p2c_obj_mod"; break;
                case OP_LSHIFT: func = "p2c_obj_lshift"; break;
                case OP_RSHIFT: func = "p2c_obj_rshift"; break;
                case OP_BITAND: func = "p2c_obj_bitand"; break;
                case OP_BITOR: func = "p2c_obj_bitor"; break;
                case OP_BITXOR: func = "p2c_obj_bitxor"; break;
                default: break;
            }
            P2C_AstExpr *target = n->u.augassign.target;
            if (target->base.type == AST_NAME) {
                indent(cg);
                if (map_has_name(cg->nonlocal_names, target->base.u.name.name) && cg->closure_env_var) {
                    write_str(cg, "p2c_cell_set(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", p2c_obj_from_str(\""); write_str(cg, target->base.u.name.name); write_str(cg, "\")), ");
                    write_str(cg, func); write_str(cg, "("); gen_expr(cg, target); write_str(cg, ", "); gen_expr(cg, n->u.augassign.value); write_str(cg, "));");
                } else if (map_has_name(cg->cell_names, target->base.u.name.name)) {
                    write_str(cg, "p2c_cell_set(_p2c_cell_"); write_ident(cg, target->base.u.name.name); write_str(cg, ", ");
                    write_str(cg, func); write_str(cg, "("); gen_expr(cg, target); write_str(cg, ", "); gen_expr(cg, n->u.augassign.value); write_str(cg, "));");
                } else {
                    write_ident(cg, target->base.u.name.name); write_str(cg, " = ");
                    write_str(cg, func); write_str(cg, "("); gen_expr(cg, target); write_str(cg, ", "); gen_expr(cg, n->u.augassign.value); write_str(cg, ");");
                }
                write_newline(cg);
            } else if (target->base.type == AST_ATTRIBUTE) {
                int aug_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, target->base.u.attribute.value); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_rhs_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, n->u.augassign.value); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "p2c_setattr(_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", \""); write_str(cg, target->base.u.attribute.attr); write_str(cg, "\", "); write_str(cg, func); write_str(cg, "(p2c_getattr(_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", \""); write_str(cg, target->base.u.attribute.attr); write_str(cg, "\"), _p2c_aug_rhs_"); emit_usize(cg, (size_t)aug_id); write_str(cg, "));"); write_newline(cg);
            } else if (target->base.type == AST_SUBSCRIPT) {
                int aug_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, target->base.u.subscript.value); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_key_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, target->base.u.subscript.slice); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_rhs_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, n->u.augassign.value); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "p2c_subscript_set(_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_key_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", "); write_str(cg, func); write_str(cg, "(p2c_subscript_get(_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_key_"); emit_usize(cg, (size_t)aug_id); write_str(cg, "), _p2c_aug_rhs_"); emit_usize(cg, (size_t)aug_id); write_str(cg, "));"); write_newline(cg);
            } else if (target->base.type == AST_CALL && target->base.u.call.func &&
                       target->base.u.call.func->base.type == AST_NAME &&
                       strcmp(target->base.u.call.func->base.u.name.name, "p2c_obj_slice") == 0 &&
                       target->base.u.call.args && p2c_vec_len(target->base.u.call.args) == 4) {
                int aug_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 0)); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_start_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 1)); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_stop_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 2)); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_step_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(target->base.u.call.args, 3)); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "P2C_Object *_p2c_aug_rhs_"); emit_usize(cg, (size_t)aug_id); write_str(cg, " = "); gen_expr(cg, n->u.augassign.value); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "p2c_slice_assign(_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_start_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_stop_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_step_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", "); write_str(cg, func); write_str(cg, "(p2c_obj_slice(_p2c_aug_obj_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_start_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_stop_"); emit_usize(cg, (size_t)aug_id); write_str(cg, ", _p2c_aug_step_"); emit_usize(cg, (size_t)aug_id); write_str(cg, "), _p2c_aug_rhs_"); emit_usize(cg, (size_t)aug_id); write_str(cg, "));"); write_newline(cg);
            }
            break;
        }
        case AST_RETURN:
            /* Pythonはreturn式を評価してからfinallyを実行するため、戻り値を
             * 一時変数へ確定させてから脱出時の後処理を生成する。tryの外側に
             * あるreturn（大多数）は従来どおり1行で出力する。 */
            if (cg->cleanup_top) {
                int ret_id = ++cg->temp_counter;
                indent(cg); write_str(cg, "P2C_Object *_p2c_ret_"); emit_usize(cg, (size_t)ret_id); write_str(cg, " = ");
                if (n->u.return_stmt.value) gen_expr(cg, n->u.return_stmt.value); else write_str(cg, "&P2C_None");
                write_str(cg, ";"); write_newline(cg);
                emit_exit_cleanups(cg, 0);
                indent(cg); write_str(cg, "return _p2c_ret_"); emit_usize(cg, (size_t)ret_id); write_str(cg, ";"); write_newline(cg);
            } else {
                indent(cg); write_str(cg, "return "); if (n->u.return_stmt.value) gen_expr(cg, n->u.return_stmt.value); else write_str(cg, "&P2C_None"); write_str(cg, ";"); write_newline(cg);
            }
            break;
        case AST_EXPR_STMT:
            indent(cg);
            /* 式文の値は捨てる。ただし `...` や数値リテラルのように「副作用の無い
             * 左辺値」だけの文は -Wunused-value になるため、明示的に void へ
             * キャストする（関数呼び出しには影響しない）。 */
            write_str(cg, "(void)(");
            gen_expr(cg, n->u.expr_stmt.value);
            write_str(cg, ");");
            write_newline(cg);
            break;
        case AST_WITH:
            gen_with_items(cg, n->u.with.items, 0, n->u.with.body);
            break;
        case AST_PASS:
            write_line(cg, "/* pass */");
            break;
        case AST_BLOCK:
            /* セミコロン区切りで同じ行に書かれた複数文を展開する */
            for (size_t bi = 0; bi < p2c_vec_len(n->u.block.stmts); bi++) {
                gen_stmt(cg, (P2C_AstStmt*)p2c_vec_get(n->u.block.stmts, bi));
            }
            break;
        case AST_GLOBAL:
        case AST_NONLOCAL:
            /* global/nonlocal 自体はCコードを生成しない。
             * 対応する名前をローカル変数として再宣言しないようにする処理は
             * 関数のpredeclareパス開始前(collect_global_decls)で済ませてある。 */
            break;
        case AST_BREAK:
            /* breakがtry/finallyを脱出する場合は、ループの外へ出る前に
             * finally本体を実行する（脱出後も生存するtryは対象外）。 */
            emit_exit_cleanups(cg, cg->loop_depth);
            if (cg->active_loop_id > 0) {
                indent(cg); write_str(cg, "_p2c_loop_broken_"); emit_usize(cg, (size_t)cg->active_loop_id); write_str(cg, " = 1;"); write_newline(cg);
            }
            write_line(cg, "break;");
            break;
        case AST_CONTINUE:
            /* continueも同様に、次の反復へ進む前にfinally本体を実行する。 */
            emit_exit_cleanups(cg, cg->loop_depth);
            write_line(cg, "continue;");
            break;
        case AST_IF:
            indent(cg); write_str(cg, "if (p2c_obj_is_truthy("); gen_expr(cg, n->u.if_stmt.test); write_str(cg, ")) {"); write_newline(cg); push_indent(cg);
            gen_stmt_list(cg, n->u.if_stmt.body); pop_indent(cg);
            if (p2c_vec_len(n->u.if_stmt.orelse) > 0) {
                write_line(cg, "} else {"); push_indent(cg); gen_stmt_list(cg, n->u.if_stmt.orelse); pop_indent(cg); write_line(cg, "}");
            } else write_line(cg, "}");
            break;
        case AST_WHILE: {
            bool has_loop_else = p2c_vec_len(n->u.while_stmt.orelse) > 0;
            int loop_id = has_loop_else ? ++cg->temp_counter : 0;
            int saved_loop_id = cg->active_loop_id;
            cg->active_loop_id = loop_id;
            indent(cg); write_str(cg, "{"); write_newline(cg); push_indent(cg);
            if (has_loop_else) { indent(cg); write_str(cg, "int _p2c_loop_broken_"); emit_usize(cg, (size_t)loop_id); write_str(cg, " = 0;"); write_newline(cg); }
            /* ループ後退エッジ。サンドボックスのステップ予算を消費する
             * （未設定なら runtime 側で即 return）。 */
            indent(cg); write_str(cg, "p2c_sandbox_tick();"); write_newline(cg);
            indent(cg); write_str(cg, "while (p2c_obj_is_truthy("); gen_expr(cg, n->u.while_stmt.test); write_str(cg, ")) {"); write_newline(cg); push_indent(cg);
            cg->loop_depth++;
            gen_stmt_list(cg, n->u.while_stmt.body);
            cg->loop_depth--;
            pop_indent(cg); write_line(cg, "}");
            if (has_loop_else) {
                indent(cg); write_str(cg, "if (!_p2c_loop_broken_"); emit_usize(cg, (size_t)loop_id); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
                gen_stmt_list(cg, n->u.while_stmt.orelse);
                pop_indent(cg); write_line(cg, "}");
            }
            pop_indent(cg); write_line(cg, "}");
            cg->active_loop_id = saved_loop_id;
            break;
        }
        case AST_ASYNC_FOR:
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "async for state-machine lowering is not yet enabled");
            break;
        case AST_FOR: {
            /* for x in SomeClass(): のように、for文の反復対象が既知クラスへの
             * 直接のコンストラクタ呼び出しである場合、そのクラスが
             * __iter__は実装しているが__len__+__getitem__（python_code_to_cのfor文が
             * 実際に使うプロトコル）を実装していないケースを検出する。
             * 検出しなければ実行時にTypeErrorが送出されるだけで
             * （p2c_len経由。クラッシュはしないが、分かりにくい失敗になる）、
             * 分かりやすいコンパイルエラーの方が親切なため、ここで先回りする。 */
            P2C_AstExpr *iter_e = n->u.for_stmt.iter;
            if (iter_e && iter_e->base.type == AST_CALL && iter_e->base.u.call.func->base.type == AST_NAME) {
                const char *cls_name = iter_e->base.u.call.func->base.u.name.name;
                P2C_Map *methods = (P2C_Map*)p2c_map_get(cg->class_methods, cls_name);
                if (methods && p2c_map_get(methods, "__iter__") &&
                    !(p2c_map_get(methods, "__len__") && p2c_map_get(methods, "__getitem__"))) {
                    char errbuf[256];
                    snprintf(errbuf, sizeof(errbuf),
                        "for x in %s(): '%s' defines __iter__ but python_code_to_c's for-loop needs __len__ and "
                        "__getitem__ (sequence protocol), not the __iter__/__next__ iterator protocol. "
                        "Add __len__ and __getitem__ to '%s', or iterate manually with iter()/next().",
                        cls_name, cls_name, cls_name);
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, errbuf);
                }
            }
            bool has_loop_else = p2c_vec_len(n->u.for_stmt.orelse) > 0;
            int loop_id = has_loop_else ? ++cg->temp_counter : 0;
            int for_id = ++cg->temp_counter;
            int saved_loop_id = cg->active_loop_id;
            cg->active_loop_id = loop_id;
            indent(cg); write_str(cg, "{"); write_newline(cg); push_indent(cg);
            if (has_loop_else) { indent(cg); write_str(cg, "int _p2c_loop_broken_"); emit_usize(cg, (size_t)loop_id); write_str(cg, " = 0;"); write_newline(cg); }
            indent(cg); write_str(cg, "P2C_Object *_p2c_iter_obj_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = p2c_builtin_iter("); gen_expr(cg, n->u.for_stmt.iter); write_str(cg, ");"); write_newline(cg);
            indent(cg); write_str(cg, "for (;;) {"); write_newline(cg); push_indent(cg);
            indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "P2C_Object * volatile _p2c_iter_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = NULL;"); write_newline(cg);
            indent(cg); write_str(cg, "_p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".prev = p2c_exc_stack; _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".env) == 0) { _p2c_iter_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = p2c_builtin_next(_p2c_iter_obj_"); emit_usize(cg, (size_t)for_id); write_str(cg, "); p2c_exc_stack = _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".prev; } else { P2C_Object *_p2c_iter_exc_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".exc; p2c_exc_stack = _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".prev; if (p2c_exc_name_match(_p2c_iter_exc_"); emit_usize(cg, (size_t)for_id); write_str(cg, ", \"StopIteration\")) break; p2c_raise(_p2c_iter_exc_"); emit_usize(cg, (size_t)for_id); write_str(cg, "); }"); write_newline(cg);
            if (n->u.for_stmt.target && n->u.for_stmt.target->base.type == AST_NAME) {
                indent(cg); write_ident(cg, n->u.for_stmt.target->base.u.name.name); write_str(cg, " = _p2c_iter_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
            } else if (n->u.for_stmt.target && n->u.for_stmt.target->base.type == AST_TUPLE) {
                P2C_Vector *elts = n->u.for_stmt.target->base.u.tuple.elts;
                size_t target_count = p2c_vec_len(elts);
                size_t star_index = target_count;
                size_t star_count = 0;
                for (size_t ei = 0; ei < target_count; ei++) {
                    P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, ei);
                    if (e && e->base.type == AST_STARRED) { star_index = ei; star_count++; }
                }
                if (star_count > 1) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "multiple starred for-loop targets are not supported");
                    break;
                }
                indent(cg); write_str(cg, "P2C_Object *_p2c_for_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = _p2c_iter_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
                indent(cg); write_str(cg, "int64_t _p2c_for_len_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = p2c_len(_p2c_for_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, ");"); write_newline(cg);
                if (star_count == 0) {
                    indent(cg); write_str(cg, "if (_p2c_for_len_"); emit_usize(cg, (size_t)for_id); write_str(cg, " != "); emit_usize(cg, target_count); write_str(cg, ") p2c_raise(p2c_make_exception(\"ValueError\", \"for unpack target has incorrect length\"));"); write_newline(cg);
                } else {
                    size_t fixed_count = target_count - 1;
                    indent(cg); write_str(cg, "if (_p2c_for_len_"); emit_usize(cg, (size_t)for_id); write_str(cg, " < "); emit_usize(cg, fixed_count); write_str(cg, ") p2c_raise(p2c_make_exception(\"ValueError\", \"for unpack target has insufficient values\"));"); write_newline(cg);
                }
                for (size_t ei = 0; ei < target_count; ei++) {
                    P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, ei);
                    if (!e) continue;
                    if (e->base.type == AST_STARRED && e->base.u.starred.value && e->base.u.starred.value->base.type == AST_NAME) {
                        size_t after_count = target_count - ei - 1;
                        indent(cg); write_ident(cg, e->base.u.starred.value->base.u.name.name); write_str(cg, " = p2c_list_new();"); write_newline(cg);
                        indent(cg); write_str(cg, "for (int64_t _p2c_for_rest_i_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = "); emit_usize(cg, ei); write_str(cg, "; _p2c_for_rest_i_"); emit_usize(cg, (size_t)for_id); write_str(cg, " < _p2c_for_len_"); emit_usize(cg, (size_t)for_id); write_str(cg, " - "); emit_usize(cg, after_count); write_str(cg, "; _p2c_for_rest_i_"); emit_usize(cg, (size_t)for_id); write_str(cg, "++) p2c_list_append("); write_ident(cg, e->base.u.starred.value->base.u.name.name); write_str(cg, ", p2c_subscript_get(_p2c_for_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, ", p2c_obj_from_int(_p2c_for_rest_i_"); emit_usize(cg, (size_t)for_id); write_str(cg, "))); "); write_newline(cg);
                    } else if (e->base.type == AST_NAME) {
                        indent(cg); write_ident(cg, e->base.u.name.name); write_str(cg, " = p2c_subscript_get(_p2c_for_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, ", p2c_obj_from_int(");
                        if (star_count > 0 && ei > star_index) {
                            size_t after_count = target_count - star_index - 1;
                            size_t after_offset = ei - star_index - 1;
                            write_str(cg, "_p2c_for_len_"); emit_usize(cg, (size_t)for_id); write_str(cg, " - "); emit_usize(cg, after_count); write_str(cg, " + "); emit_usize(cg, after_offset);
                        } else emit_usize(cg, ei);
                        write_str(cg, "));"); write_newline(cg);
                    }
                }
            }
            cg->loop_depth++;
            gen_stmt_list(cg, n->u.for_stmt.body);
            cg->loop_depth--;
            pop_indent(cg); write_line(cg, "}");
            if (has_loop_else) {
                indent(cg); write_str(cg, "if (!_p2c_loop_broken_"); emit_usize(cg, (size_t)loop_id); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
                gen_stmt_list(cg, n->u.for_stmt.orelse);
                pop_indent(cg); write_line(cg, "}");
            }
            pop_indent(cg); write_line(cg, "}");
            cg->active_loop_id = saved_loop_id;
            break;
        }
        case AST_TRY:
            gen_try_stmt(cg, stmt);
            break;
        case AST_MATCH:
            gen_match_stmt(cg, stmt);
            break;
        case AST_RAISE:
            indent(cg);
            if (!n->u.raise.exc) write_str(cg, "p2c_reraise();");
            else {
                write_str(cg, "p2c_raise(");
                if (n->u.raise.cause) {
                    write_str(cg, "p2c_exception_with_cause(");
                    gen_raise_target(cg, n->u.raise.exc);
                    write_str(cg, ", ");
                    gen_expr(cg, n->u.raise.cause);
                    write_str(cg, ")");
                } else gen_raise_target(cg, n->u.raise.exc);
                write_str(cg, ");");
            }
            write_newline(cg);
            break;
        case AST_ASSERT:
            indent(cg); write_str(cg, "if (!p2c_obj_is_truthy("); gen_expr(cg, n->u.assert_stmt.test); write_str(cg, ")) p2c_raise(p2c_make_exception(\"AssertionError\", ");
            if (n->u.assert_stmt.msg && n->u.assert_stmt.msg->base.type == AST_CONST && n->u.assert_stmt.msg->base.u.constant.token_type == TOK_STR_LITERAL) {
                write_str(cg, "\""); gen_string_literal_contents(cg, n->u.assert_stmt.msg->base.u.constant.value); write_str(cg, "\"));");
            } else write_str(cg, "\"assertion failed\"));");
            write_newline(cg);
            break;
        case AST_FUNCTIONDEF: {
            P2C_AstFunctionDef *fd = &n->u.functiondef;
            if (cg->function_depth > 0) {
                gen_nested_closure(cg, fd);
                break;
            }
            if (suspension_function(fd)) {
                gen_suspension_function(cg, fd);
                if (cg->function_depth == 0 && !fd->vararg && !fd->kwarg &&
                    ((fd->decorator_list && p2c_vec_len(fd->decorator_list) > 0) || map_has_name(cg->decorator_callable_names, fd->name))) {
                    gen_decorator_callable_adapter(cg, fd);
                }
                break;
            }
            size_t nfixed = p2c_vec_len(fd->args);
            p2c_str_append(cg->forward, "P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->name)); p2c_str_append(cg->forward, "(");
            {
                bool wrote = false;
                for (size_t i = 0; i < nfixed; i++) {
                    if (wrote) p2c_str_append(cg->forward, ", ");
                    wrote = true;
                    p2c_str_append(cg->forward, "P2C_Object *");
                    P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
                    p2c_str_append(cg->forward, mangle_ident(arg->name));
                }
                if (fd->vararg) { if (wrote) p2c_str_append(cg->forward, ", "); wrote = true; p2c_str_append(cg->forward, "P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->vararg)); }
                if (fd->kwarg) { if (wrote) p2c_str_append(cg->forward, ", "); p2c_str_append(cg->forward, "P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->kwarg)); }
            }
            if (nfixed == 0 && !fd->vararg && !fd->kwarg) p2c_str_append(cg->forward, "void");
            p2c_str_append(cg->forward, ");\n");
            indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, fd->name); write_str(cg, "(");
            {
                bool wrote = false;
                for (size_t i = 0; i < nfixed; i++) {
                    if (wrote) write_str(cg, ", ");
                    wrote = true;
                    write_str(cg, "P2C_Object *");
                    P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
                    write_ident(cg, arg->name);
                }
                if (fd->vararg) { if (wrote) write_str(cg, ", "); wrote = true; write_str(cg, "P2C_Object *"); write_ident(cg, fd->vararg); }
                if (fd->kwarg) { if (wrote) write_str(cg, ", "); write_str(cg, "P2C_Object *"); write_ident(cg, fd->kwarg); }
            }
            if (nfixed == 0 && !fd->vararg && !fd->kwarg) write_str(cg, "void");
            write_str(cg, ") {"); write_newline(cg); push_indent(cg);
            /* 本体がパラメータを1つも参照しない関数（例: def f(x): return 1）では
             * -Wunused-parameter が -Werror 下でコンパイルエラーになる。
             * 常に(void)キャストしておけば、実際に使われる場合でも無害。 */
            for (size_t i = 0; i < nfixed; i++) {
                indent(cg); write_str(cg, "(void)"); write_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name); write_str(cg, ";"); write_newline(cg);
            }
            if (fd->vararg) { indent(cg); write_str(cg, "(void)"); write_ident(cg, fd->vararg); write_str(cg, ";"); write_newline(cg); }
            if (fd->kwarg) { indent(cg); write_str(cg, "(void)"); write_ident(cg, fd->kwarg); write_str(cg, ";"); write_newline(cg); }
            P2C_Map *saved_declared = cg->declared_vars;
            P2C_Map *saved_nonlocal_names = cg->nonlocal_names;
            P2C_Map *saved_cell_names = cg->cell_names;
            cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
            cg->nonlocal_names = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
            cg->cell_names = collect_nested_nonlocal_cell_names(cg, fd->body);
            for (size_t i = 0; i < nfixed; i++) remember_declared(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
            if (fd->vararg) remember_declared(cg, fd->vararg);
            if (fd->kwarg) remember_declared(cg, fd->kwarg);
            collect_global_decls(fd->body, cg);
            predeclare_stmt_list(cg, fd->body);
            emit_cell_declarations(cg, cg->cell_names);
            /* 入れ子関数は外側のtry/finallyを脱出しないため、クリーンアップ
             * フレームとループ深さをリセットして生成する。 */
            struct P2C_CleanupFrame *saved_cleanup_top = cg->cleanup_top;
            int saved_loop_depth = cg->loop_depth;
            cg->cleanup_top = NULL;
            cg->loop_depth = 0;
            cg->function_depth++;
            gen_stmt_list(cg, fd->body);
            cg->function_depth--;
            cg->cleanup_top = saved_cleanup_top;
            cg->loop_depth = saved_loop_depth;
            if (p2c_vec_len(fd->body) == 0 || ((P2C_AstStmt*)p2c_vec_get(fd->body, p2c_vec_len(fd->body)-1))->base.type != AST_RETURN) write_line(cg, "return &P2C_None;");
            p2c_map_free(cg->declared_vars); cg->declared_vars = saved_declared;
            free_name_map(cg->nonlocal_names); cg->nonlocal_names = saved_nonlocal_names;
            free_name_map(cg->cell_names); cg->cell_names = saved_cell_names;
            pop_indent(cg); write_line(cg, "}"); write_newline(cg);
            if (cg->function_depth == 0 && !fd->vararg && !fd->kwarg &&
                ((fd->decorator_list && p2c_vec_len(fd->decorator_list) > 0) || map_has_name(cg->decorator_callable_names, fd->name))) {
                gen_decorator_callable_adapter(cg, fd);
            }
            break;
        }
        case AST_CLASSDEF: {
            /* ネストしたクラス定義は外側クラス名を前置したC名（Outer__Inner）で
             * シンボルを分離する。モジュール直下のクラスは従来通りクラス名
             * そのものを使う。名前長はソース依存で上限が無いため、切り詰めが
             * 起きないようcodegenアロケータ上に確保する。 */
            const char *cname = n->u.classdef.name;
            if (cg->current_class) {
                size_t nested_cname_len = strlen(cg->current_class) + 2 + strlen(n->u.classdef.name);
                char *nested_cname = (char*)p2c_alloc(cg->alloc, nested_cname_len + 1);
                if (!nested_cname) {
                    codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate nested class C name");
                    break;
                }
                /* 長さは上で確定済み（外側名 + "__" + 内側名）。GCCの
                 * -Wformat-truncation はsnprintfの長さ計算を追えないため、
                 * 明示的な長さで組み立てる。 */
                {
                    size_t outer_len = strlen(cg->current_class);
                    memcpy(nested_cname, cg->current_class, outer_len);
                    nested_cname[outer_len] = '_';
                    nested_cname[outer_len + 1] = '_';
                    memcpy(nested_cname + outer_len + 2, n->u.classdef.name,
                           nested_cname_len - outer_len - 2 + 1);
                }
                cname = nested_cname;
            }
            /* super()解決用に、このクラスのメソッド本体を生成する間だけ
             * current_class/current_class_baseを設定する。 */
            const char *saved_current_class = cg->current_class;
            cg->current_class = cname;
            /* setter の C シンボル衝突を避けるため、生成前に改名する。 */
            rename_class_setters(cg, &n->u.classdef);
            p2c_str_append_fmt(cg->forward, "static P2C_Object *%s = NULL;\n", cname);
            p2c_str_append_fmt(cg->forward, "static P2C_Object* %s__ctor(P2C_Object **args, size_t nargs);\n", cname);
            p2c_str_append_fmt(cg->forward, "static P2C_Object* %s__classobj(void);\n", cname);
            for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_AstStmt *member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                if (member->base.type == AST_FUNCTIONDEF) {
                    P2C_AstFunctionDef *fd = &member->base.u.functiondef;
                    int fwd_kind = class_method_kind(fd);
                    p2c_str_append_fmt(cg->forward, "static P2C_Object* %s__%s(", cname, fd->name);
                    if (fwd_kind == CG_METHOD_STATIC) {
                        /* staticmethodのC関数は、ランタイムが種別に従って先頭へNULLを
                         * 渡すため、未使用の先頭パラメータを1つ受け取る。 */
                        p2c_str_append(cg->forward, "P2C_Object *_p2c_static_self");
                    }
                    for (size_t j = 0; j < p2c_vec_len(fd->args); j++) {
                        if (j || fwd_kind == CG_METHOD_STATIC) p2c_str_append(cg->forward, ", ");
                        p2c_str_append(cg->forward, "P2C_Object *");
                        p2c_str_append(cg->forward, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name);
                    }
                    if (fd->vararg) {
                        if (p2c_vec_len(fd->args) || fwd_kind == CG_METHOD_STATIC) p2c_str_append(cg->forward, ", ");
                        p2c_str_append(cg->forward, "P2C_Object *"); p2c_str_append(cg->forward, fd->vararg);
                    }
                    if (fd->kwarg) {
                        if (p2c_vec_len(fd->args) || fd->vararg || fwd_kind == CG_METHOD_STATIC) p2c_str_append(cg->forward, ", ");
                        p2c_str_append(cg->forward, "P2C_Object *"); p2c_str_append(cg->forward, fd->kwarg);
                    }
                    p2c_str_append(cg->forward, ");\n");
                    p2c_str_append_fmt(cg->forward, "static P2C_Object* %s__%s__adapter(P2C_Object *self, P2C_Object **args, size_t nargs);\n", cname, fd->name);
                    p2c_str_append_fmt(cg->forward, "static P2C_Object* %s__%s__kwadapter(P2C_Object *self, P2C_Object **args, size_t nargs, const char **kw_names, P2C_Object **kw_values, size_t nkw);\n", cname, fd->name);
                }
            }
            /* ネストしたクラスの定義は外側クラスの__classobj()から参照するため、
             * メソッド本体より先にファイルスコープへ生成しておく。 */
            for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_AstStmt *nested_member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                if (nested_member->base.type != AST_CLASSDEF) continue;
                P2C_String *saved_nested_current = cg->current;
                int saved_nested_indent = cg->indent_level;
                cg->current = cg->toplevel;
                cg->indent_level = 0;
                gen_stmt(cg, nested_member);
                write_newline(cg);
                cg->current = saved_nested_current;
                cg->indent_level = saved_nested_indent;
            }
            /* このクラスの本体で可視なネストクラス（Python名 -> C名）。
             * Python仕様ではクラススコープの名前はメソッド本体から見えないため、
             * in_class_bodyがfalseのときの参照は明示的に診断する。 */
            P2C_Map *saved_nested_aliases = cg->nested_class_aliases;
            bool saved_in_class_body = cg->in_class_body;
            P2C_Map *own_nested_aliases = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
            if (!own_nested_aliases) codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate nested class alias map");
            for (size_t i = 0; own_nested_aliases && i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_AstStmt *nested_member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                if (nested_member->base.type != AST_CLASSDEF) continue;
                size_t nested_name_len = strlen(nested_member->base.u.classdef.name);
                size_t alias_len = strlen(cname) + 2 + nested_name_len;
                char *key_dup = p2c_alloc(cg->alloc, nested_name_len + 1);
                char *val_dup = p2c_alloc(cg->alloc, alias_len + 1);
                if (key_dup && val_dup) {
                    strcpy(key_dup, nested_member->base.u.classdef.name);
                    /* C名は 外側C名 + "__" + 内側Python名（alias_lenで長さ確定済み）。 */
                    {
                        size_t outer_len = strlen(cname);
                        memcpy(val_dup, cname, outer_len);
                        val_dup[outer_len] = '_';
                        val_dup[outer_len + 1] = '_';
                        memcpy(val_dup + outer_len + 2, nested_member->base.u.classdef.name,
                               alias_len - outer_len - 2 + 1);
                    }
                    p2c_map_insert(own_nested_aliases, key_dup, val_dup);
                }
            }
            cg->nested_class_aliases = own_nested_aliases;
            cg->in_class_body = false;
            for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_AstStmt *member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                if (member->base.type != AST_FUNCTIONDEF) continue;
                P2C_AstFunctionDef *fd = &member->base.u.functiondef;
                int method_kind = class_method_kind(fd);
                /* staticmethodはselfを受け取らないため、位置引数の対応が1つずれる。 */
                size_t self_count = (method_kind == CG_METHOD_STATIC) ? 0 : 1;
                bool method_is_suspension = class_method_requires_suspension(fd);
                if (method_is_suspension) {
                    P2C_String *method_entry = p2c_str_new(cg->alloc);
                    if (!method_entry) {
                        codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate class async method entry name");
                        continue;
                    }
                    p2c_str_append(method_entry, cname);
                    p2c_str_append(method_entry, "__");
                    p2c_str_append(method_entry, fd->name);
                    /* p2c_str_cstr() はconst文字列を返すため、char* を要求するAST
                     * フィールドへ渡すにはcodegenアロケータ上の書き換え可能な
                     * コピーを作る（const破棄キャストを避ける）。 */
                    char *saved_method_name = fd->name;
                    const char *method_name = p2c_str_cstr(method_entry);
                    size_t method_name_len = strlen(method_name);
                    char *method_name_copy = (char*)p2c_alloc(cg->alloc, method_name_len + 1);
                    if (!method_name_copy) {
                        codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate class async method name");
                        p2c_str_free(method_entry);
                        continue;
                    }
                    memcpy(method_name_copy, method_name, method_name_len + 1);
                    fd->name = method_name_copy;
                    gen_suspension_function(cg, fd);
                    fd->name = saved_method_name;
                    p2c_free(cg->alloc, method_name_copy);
                    p2c_str_free(method_entry);
                } else {
                    indent(cg); write_str(cg, "static P2C_Object* "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "(");
                if (method_kind == CG_METHOD_STATIC) write_str(cg, "P2C_Object *_p2c_static_self");
                for (size_t j = 0; j < p2c_vec_len(fd->args); j++) {
                    if (j || method_kind == CG_METHOD_STATIC) write_str(cg, ", ");
                    write_str(cg, "P2C_Object *"); write_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name);
                }
                if (fd->vararg) { if (p2c_vec_len(fd->args) || method_kind == CG_METHOD_STATIC) write_str(cg, ", "); write_str(cg, "P2C_Object *"); write_ident(cg, fd->vararg); }
                if (fd->kwarg) { if (p2c_vec_len(fd->args) || fd->vararg || method_kind == CG_METHOD_STATIC) write_str(cg, ", "); write_str(cg, "P2C_Object *"); write_ident(cg, fd->kwarg); }
                write_str(cg, ") {"); write_newline(cg); push_indent(cg);
                /* selfを含め、本体で参照されないパラメータがあると
                 * -Wunused-parameter が -Werror 下でコンパイルエラーになる
                 * （例: def __iter__(self): return iter([1,2,3]) のようにselfを
                 * 使わないメソッド）。常に(void)キャストしておく。 */
                if (method_kind == CG_METHOD_STATIC) write_line(cg, "(void)_p2c_static_self;");
                for (size_t j = 0; j < p2c_vec_len(fd->args); j++) {
                    indent(cg); write_str(cg, "(void)"); write_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name); write_str(cg, ";"); write_newline(cg);
                }
                if (fd->vararg) { indent(cg); write_str(cg, "(void)"); write_ident(cg, fd->vararg); write_str(cg, ";"); write_newline(cg); }
                if (fd->kwarg) { indent(cg); write_str(cg, "(void)"); write_ident(cg, fd->kwarg); write_str(cg, ";"); write_newline(cg); }
                P2C_Map *saved_declared = cg->declared_vars;
                cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
                for (size_t j = 0; j < p2c_vec_len(fd->args); j++) remember_declared(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name);
                if (fd->vararg) remember_declared(cg, fd->vararg);
                if (fd->kwarg) remember_declared(cg, fd->kwarg);
                predeclare_stmt_list(cg, fd->body);
                /* メソッド本体は独立したC関数なので、外側のtry/finallyやループの
                 * コンテキストを引き継がない。 */
                struct P2C_CleanupFrame *saved_method_cleanup_top = cg->cleanup_top;
                int saved_method_loop_depth = cg->loop_depth;
                cg->cleanup_top = NULL;
                cg->loop_depth = 0;
                gen_stmt_list(cg, fd->body);
                cg->cleanup_top = saved_method_cleanup_top;
                cg->loop_depth = saved_method_loop_depth;
                if (p2c_vec_len(fd->body) == 0 || ((P2C_AstStmt*)p2c_vec_get(fd->body, p2c_vec_len(fd->body)-1))->base.type != AST_RETURN) write_line(cg, "return &P2C_None;");
                    p2c_map_free(cg->declared_vars); cg->declared_vars = saved_declared;
                    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
                }

                indent(cg); write_str(cg, "static P2C_Object* "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "__adapter(P2C_Object *self, P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
                /* argsが0個のメソッド(selfのみ)ではargs/nargsパラメータが本体で
                 * 一切参照されず、-Wunused-parameter(-Werror下ではエラー)になる。
                 * 常に(void)キャストしておけば、実際に使われる場合でも無害。 */
                write_line(cg, "(void)args; (void)nargs;");
                /* staticmethodのアダプタは self を受け取るが使用しない。 */
                if (self_count == 0) write_line(cg, "(void)self;");
                size_t method_positional_total = (fd->kwonly_start > 0 ? (size_t)fd->kwonly_start : p2c_vec_len(fd->args));
                method_positional_total = method_positional_total > self_count ? method_positional_total - self_count : 0;
                size_t method_required = 0;
                for (size_t j = self_count; j < p2c_vec_len(fd->args); j++) {
                    P2C_AstArg *ma = (P2C_AstArg*)p2c_vec_get(fd->args, j);
                    if ((int)j < fd->kwonly_start && !ma->default_val) method_required++;
                }
                indent(cg); write_str(cg, "if (");
                if (method_required > 0) { write_str(cg, "nargs < "); emit_usize(cg, method_required); if (!fd->vararg) write_str(cg, " || "); }
                if (!fd->vararg) { write_str(cg, "nargs > "); emit_usize(cg, method_positional_total); }
                else if (method_required == 0) write_str(cg, "false");
                write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"wrong method arity\"));"); write_newline(cg);
                for (size_t j = self_count; j < p2c_vec_len(fd->args); j++) {
                    P2C_AstArg *ma = (P2C_AstArg*)p2c_vec_get(fd->args, j);
                    indent(cg); write_str(cg, "P2C_Object *_p2c_method_arg_"); emit_usize(cg, j - self_count); if ((int)j < fd->kwonly_start) { write_str(cg, " = (nargs > "); emit_usize(cg, j - self_count); write_str(cg, ") ? args["); emit_usize(cg, j - self_count); write_str(cg, "] : "); }
                    else write_str(cg, " = ");
                    if (ma->default_val) gen_expr(cg, ma->default_val);
                    else write_str(cg, "&P2C_None");
                    write_str(cg, ";"); write_newline(cg);
                }
                if (fd->vararg) {
                    indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, fd->vararg); write_str(cg, " = p2c_tuple_new(nargs > "); emit_usize(cg, method_positional_total); write_str(cg, " ? nargs - "); emit_usize(cg, method_positional_total); write_str(cg, " : 0);"); write_newline(cg);
                    indent(cg); write_str(cg, "for (size_t _p2c_var_i = "); emit_usize(cg, method_positional_total); write_str(cg, "; _p2c_var_i < nargs; _p2c_var_i++) p2c_tuple_set("); write_ident(cg, fd->vararg); write_str(cg, ", _p2c_var_i - "); emit_usize(cg, method_positional_total); write_str(cg, ", args[_p2c_var_i]);"); write_newline(cg);
                }
                if (fd->kwarg) { indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, fd->kwarg); write_str(cg, " = p2c_dict_new();"); write_newline(cg); }
                indent(cg); write_str(cg, "return "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "(");
                /* 生成Cの関数は常に先頭パラメータ（self スロット）を持つ。
                 * staticmethodはその位置にNULLが渡されるだけで、以降の位置引数は
                 * 先頭のPython引数から順に対応する。 */
                /* 生成Cの関数は常に先頭の self スロットを受け取る（staticmethodでは
                 * 未使用スロット。ランタイムがNULLを渡す）。以降が位置引数。 */
                write_str(cg, "self");
                for (size_t k = 0; k + self_count < p2c_vec_len(fd->args); k++) {
                    write_str(cg, ", _p2c_method_arg_"); emit_usize(cg, k);
                }
                if (fd->vararg) { if (p2c_vec_len(fd->args)) write_str(cg, ", "); write_ident(cg, fd->vararg); }
                if (fd->kwarg) { if (p2c_vec_len(fd->args) || fd->vararg) write_str(cg, ", "); write_ident(cg, fd->kwarg); }
                write_str(cg, ");"); write_newline(cg);
                pop_indent(cg); write_line(cg, "}"); write_newline(cg);

                /* キーワード引数を通常引数、*args、**kwargsへ束縛するadapter。 */
                indent(cg); write_str(cg, "static P2C_Object* "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "__kwadapter(P2C_Object *self, P2C_Object **args, size_t nargs, const char **kw_names, P2C_Object **kw_values, size_t nkw) {"); write_newline(cg); push_indent(cg);
                /* __str__(self)のように追加引数を持たないメソッドのkwadapterでは、
                 * args/kw_names/kw_valuesが本体で一切参照されず -Wunused-parameter
                 * （-Wextra、テストでは-Werror）で生成Cのコンパイルが失敗する。
                 * 使われる場合にも無害なよう常に(void)キャストしておく。 */
                indent(cg); write_str(cg, "(void)self; (void)args; (void)nargs; (void)kw_names; (void)kw_values; (void)nkw;"); write_newline(cg);
                indent(cg); write_str(cg, "if (");
                if (method_required > 0) { write_str(cg, "nargs < "); emit_usize(cg, method_required); if (!fd->vararg) write_str(cg, " || "); }
                if (!fd->vararg) { write_str(cg, "nargs > "); emit_usize(cg, method_positional_total); }
                else if (method_required == 0) write_str(cg, "false");
                write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"wrong method arity\"));"); write_newline(cg);
                for (size_t j = self_count; j < p2c_vec_len(fd->args); j++) {
                    P2C_AstArg *ma = (P2C_AstArg*)p2c_vec_get(fd->args, j);
                    indent(cg); write_str(cg, "P2C_Object *_p2c_kw_arg_"); emit_usize(cg, j - self_count);
                    if ((int)j < fd->kwonly_start) { write_str(cg, " = (nargs > "); emit_usize(cg, j - self_count); write_str(cg, ") ? args["); emit_usize(cg, j - self_count); write_str(cg, "] : "); }
                    else write_str(cg, " = ");
                    if (ma->default_val) gen_expr(cg, ma->default_val); else write_str(cg, "&P2C_None");
                    write_str(cg, ";"); write_newline(cg);
                    indent(cg); write_str(cg, "bool _p2c_kw_have_"); emit_usize(cg, j - self_count);
                    if ((int)j < fd->kwonly_start) { write_str(cg, " = nargs > "); emit_usize(cg, j - self_count); }
                    else write_str(cg, " = false");
                    write_str(cg, ";"); write_newline(cg);
                    indent(cg); write_str(cg, "for (size_t _p2c_kw_i = 0; _p2c_kw_i < nkw; _p2c_kw_i++) if (strcmp(kw_names[_p2c_kw_i], \""); write_str(cg, ma->name); write_str(cg, "\") == 0) { if (_p2c_kw_have_"); emit_usize(cg, j - self_count); write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"multiple values for method argument\")); _p2c_kw_arg_"); emit_usize(cg, j - self_count); write_str(cg, " = kw_values[_p2c_kw_i]; _p2c_kw_have_"); emit_usize(cg, j - self_count); write_str(cg, " = true; }"); write_newline(cg);
                    if (!ma->default_val) { indent(cg); write_str(cg, "if (!_p2c_kw_have_"); emit_usize(cg, j - self_count); write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"missing required method argument\"));"); write_newline(cg); }
                }
                if (fd->vararg) {
                    indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, fd->vararg); write_str(cg, " = p2c_tuple_new(nargs > "); emit_usize(cg, method_positional_total); write_str(cg, " ? nargs - "); emit_usize(cg, method_positional_total); write_str(cg, " : 0);"); write_newline(cg);
                    indent(cg); write_str(cg, "for (size_t _p2c_var_i = "); emit_usize(cg, method_positional_total); write_str(cg, "; _p2c_var_i < nargs; _p2c_var_i++) p2c_tuple_set("); write_ident(cg, fd->vararg); write_str(cg, ", _p2c_var_i - "); emit_usize(cg, method_positional_total); write_str(cg, ", args[_p2c_var_i]);"); write_newline(cg);
                }
                if (fd->kwarg) {
                    indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, fd->kwarg); write_str(cg, " = p2c_dict_new();"); write_newline(cg);
                }
                indent(cg); write_str(cg, "for (size_t _p2c_kw_i = 0; _p2c_kw_i < nkw; _p2c_kw_i++) { bool _p2c_known = false;"); write_newline(cg); push_indent(cg);
                for (size_t j = self_count; j < p2c_vec_len(fd->args); j++) {
                    P2C_AstArg *ma = (P2C_AstArg*)p2c_vec_get(fd->args, j);
                    indent(cg); write_str(cg, "if (strcmp(kw_names[_p2c_kw_i], \""); write_str(cg, ma->name); write_str(cg, "\") == 0) _p2c_known = true;"); write_newline(cg);
                }
                if (fd->kwarg) {
                    indent(cg); write_str(cg, "if (!_p2c_known) p2c_dict_set("); write_ident(cg, fd->kwarg); write_str(cg, ", p2c_obj_from_str(kw_names[_p2c_kw_i]), kw_values[_p2c_kw_i]);"); write_newline(cg);
                } else {
                    indent(cg); write_str(cg, "if (!_p2c_known) p2c_raise(p2c_make_exception(\"TypeError\", \"unexpected method keyword argument\"));"); write_newline(cg);
                }
                pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
                indent(cg); write_str(cg, "return "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "(self");
                /* 呼び出しの先頭 "(self" は既に出力済み（staticでは未使用スロット）。 */
                for (size_t k = 0; k + self_count < p2c_vec_len(fd->args); k++) {
                    write_str(cg, ", _p2c_kw_arg_"); emit_usize(cg, k);
                }
                if (fd->vararg) { write_str(cg, ", "); write_ident(cg, fd->vararg); }
                if (fd->kwarg) { write_str(cg, ", "); write_ident(cg, fd->kwarg); }
                write_str(cg, ");"); write_newline(cg);
                pop_indent(cg); write_line(cg, "}"); write_newline(cg);
            }
            indent(cg); write_str(cg, "static P2C_MethodDef "); write_str(cg, cname); write_str(cg, "__methods[] = {"); write_newline(cg); push_indent(cg);
            for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_AstStmt *member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                if (member->base.type == AST_FUNCTIONDEF) {
                    indent(cg); write_str(cg, "{\""); emit_method_row_name(cg, member->base.u.functiondef.name, class_method_kind(&member->base.u.functiondef)); write_str(cg, "\", "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, member->base.u.functiondef.name); write_str(cg, "__adapter, "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, member->base.u.functiondef.name); write_str(cg, "__kwadapter, "); write_str(cg, class_method_kind_constant(class_method_kind(&member->base.u.functiondef))); write_str(cg, "},"); write_newline(cg);
                }
            }
            write_line(cg, "{NULL, NULL, NULL, P2C_METHOD_INSTANCE}"); pop_indent(cg); write_line(cg, "};");
            indent(cg); write_str(cg, "static P2C_Object* "); write_str(cg, cname); write_str(cg, "__classobj(void) {"); write_newline(cg); push_indent(cg);
            indent(cg); write_str(cg, "if (!"); write_str(cg, cname); write_str(cg, ") "); write_str(cg, cname); write_str(cg, " = p2c_class_new(\""); write_str(cg, n->u.classdef.name); write_str(cg, "\", "); write_str(cg, cname); write_str(cg, "__ctor, "); write_str(cg, cname); write_str(cg, "__methods, ");
            if (p2c_vec_len(n->u.classdef.bases) > 0) {
                write_str(cg, "\"");
                bool wrote_any = false;
                for (size_t i = 0; i < p2c_vec_len(n->u.classdef.bases); i++) {
                    P2C_AstExpr *b = (P2C_AstExpr*)p2c_vec_get(n->u.classdef.bases, i);
                    const char *bn = extract_base_name(b);
                    if (!bn) continue;
                    if (wrote_any) write_str(cg, ",");
                    write_str(cg, bn);
                    wrote_any = true;
                }
                write_str(cg, "\"");
            } else write_str(cg, "NULL");
            write_str(cg, ");"); write_newline(cg);
            /* クラス本体の文はPython同様に上から順に評価する。ネストしたclassの
             * 定義はその場で外側クラスの属性として登録し、クラス本体の式からは
             * その名前で参照できるようにする。 */
            cg->in_class_body = true;
            for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                P2C_AstStmt *member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                if (member->base.type == AST_CLASSDEF) {
                    const char *nested_alias = nested_class_alias(cg, member->base.u.classdef.name);
                    if (!nested_alias) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "could not resolve nested class name");
                        continue;
                    }
                    indent(cg); write_str(cg, "if (!"); write_str(cg, nested_alias); write_str(cg, ") "); write_str(cg, nested_alias); write_str(cg, " = "); write_str(cg, nested_alias); write_str(cg, "__classobj();"); write_newline(cg);
                    indent(cg); write_str(cg, "p2c_setattr("); write_str(cg, cname); write_str(cg, ", \""); write_str(cg, member->base.u.classdef.name); write_str(cg, "\", "); write_str(cg, nested_alias); write_str(cg, ");"); write_newline(cg);
                    continue;
                }
                if (member->base.type == AST_ASSIGN && p2c_vec_len(member->base.u.assign.targets) == 1) {
                    P2C_AstExpr *t = (P2C_AstExpr*)p2c_vec_get(member->base.u.assign.targets, 0);
                    if (t->base.type == AST_NAME) {
                        indent(cg); write_str(cg, "p2c_setattr("); write_str(cg, cname); write_str(cg, ", \""); write_str(cg, t->base.u.name.name); write_str(cg, "\", "); gen_expr(cg, member->base.u.assign.value); write_str(cg, ");"); write_newline(cg);
                    }
                }
            }
            cg->in_class_body = false;
            indent(cg); write_str(cg, "return "); write_str(cg, cname); write_str(cg, ";"); write_newline(cg);
            pop_indent(cg); write_line(cg, "}"); write_newline(cg);
            indent(cg); write_str(cg, "static P2C_Object* "); write_str(cg, cname); write_str(cg, "__ctor(P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
            /* __init__を持たないクラスでは、下でargs/nargsを参照する分岐に
             * 入らないため -Wunused-parameter になりうる。 */
            write_line(cg, "(void)args; (void)nargs;");
            indent(cg); write_str(cg, "P2C_Object *self = p2c_instance_new("); write_str(cg, cname); write_str(cg, "__classobj());"); write_newline(cg);
            {
                /* このクラス自身が__init__を定義していればそれを使う。
                 * 定義していなければ、基底クラスの解決済み__init__（さらにその基底、と
                 * 継承チェーンをたどった結果）を継承する。Pythonの継承と同様、
                 * サブクラスが__init__を省略した場合は基底クラスの__init__がそのまま
                 * 使われる（以前はここが未実装で、サブクラスのインスタンスは
                 * __init__が一切呼ばれず属性が初期化されないバグがあった）。 */
                const char *resolved_init = NULL;
                for (size_t i = 0; i < p2c_vec_len(n->u.classdef.body); i++) {
                    P2C_AstStmt *member = (P2C_AstStmt*)p2c_vec_get(n->u.classdef.body, i);
                    if (member->base.type == AST_FUNCTIONDEF && strcmp(member->base.u.functiondef.name, "__init__") == 0) {
                        static char self_init_buf[256];
                        snprintf(self_init_buf, sizeof(self_init_buf), "%s____init____adapter", cname);
                        resolved_init = self_init_buf;
                        break;
                    }
                }
                static char own_init_name[256];
                if (resolved_init) {
                    snprintf(own_init_name, sizeof(own_init_name), "%s", resolved_init);
                } else {
                    for (size_t bi = 0; bi < p2c_vec_len(n->u.classdef.bases) && !resolved_init; bi++) {
                        P2C_AstExpr *b = (P2C_AstExpr*)p2c_vec_get(n->u.classdef.bases, bi);
                        const char *bn = extract_base_name(b);
                        if (!bn) continue;
                        const char *inherited = (const char*)p2c_map_get(cg->class_init_adapter, bn);
                        if (!inherited && saved_current_class) {
                            /* 基底が同じ外側クラスのネストクラスである場合、
                             * 解決済み__init__は前置済みC名で登録されている。 */
                            char nested_base[512];
                            snprintf(nested_base, sizeof(nested_base), "%s__%s", saved_current_class, bn);
                            inherited = (const char*)p2c_map_get(cg->class_init_adapter, nested_base);
                        }
                        if (inherited) { snprintf(own_init_name, sizeof(own_init_name), "%s", inherited); resolved_init = own_init_name; }
                    }
                }
                if (resolved_init) {
                    indent(cg); write_str(cg, resolved_init); write_str(cg, "(self, args, nargs);"); write_newline(cg);
                    if (!p2c_map_get(cg->class_init_adapter, cname)) {
                        char *key_dup = p2c_alloc(cg->alloc, strlen(cname) + 1);
                        char *val_dup = p2c_alloc(cg->alloc, strlen(resolved_init) + 1);
                        if (key_dup && val_dup) { strcpy(key_dup, cname); strcpy(val_dup, resolved_init); p2c_map_insert(cg->class_init_adapter, key_dup, val_dup); }
                    }
                }
            }
            write_line(cg, "return self;"); pop_indent(cg); write_line(cg, "}"); write_newline(cg);
            cg->current_class = saved_current_class;
            cg->nested_class_aliases = saved_nested_aliases;
            cg->in_class_body = saved_in_class_body;
            if (own_nested_aliases) free_name_map(own_nested_aliases);
            break;
        }
        case AST_IMPORT:
            for (size_t i = 0; i < p2c_vec_len(n->u.import_stmt.names); i++) {
                P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.import_stmt.names, i);
                indent(cg); write_ident(cg, al->asname ? al->asname : al->name); write_str(cg, " = ");
                if (strcmp(al->name, "asyncio") == 0) {
                    write_str(cg, "&P2C_None");
                } else {
                    write_str(cg, "p2c_import_module(\""); write_str(cg, al->name); write_str(cg, "\")");
                }
                write_str(cg, ";"); write_newline(cg);
            }
            break;
        case AST_IMPORTFROM:
            for (size_t i = 0; i < p2c_vec_len(n->u.importfrom.names); i++) {
                P2C_AstAlias *al = (P2C_AstAlias*)p2c_vec_get(n->u.importfrom.names, i);
                indent(cg); write_ident(cg, al->asname ? al->asname : al->name); write_str(cg, " = p2c_getattr(p2c_import_module(\""); if (n->u.importfrom.module) write_str(cg, n->u.importfrom.module); write_str(cg, "\"), \""); write_str(cg, al->name); write_str(cg, "\");"); write_newline(cg);
            }
            break;
        case AST_DELETE:
            /* del文: 対象の種類によって処理が異なる。
             * - 単純名 (del x): 変数を &P2C_None に再束縛するだけでよい。
             *   実際のオブジェクトの解放はGCが到達可能性から判断して行う。
             * - 属性 (del obj.attr): p2c_delattr() でインスタンス/クラス/
             *   モジュールの属性マップから該当エントリを削除する。
             * - 添字 (del x[i], del d[k]): p2c_subscript_delete() で
             *   list なら要素を詰めて削除、dict ならエントリを削除する。
             * （以前は対象の種類を区別せず gen_expr(target) の結果に
             *   `= &P2C_None` を代入しようとしており、属性・添字の場合は
             *   gen_expr が関数呼び出し式を生成するため代入先として
             *   不正な式となり、生成されたCがコンパイルエラーになっていた。） */
            for (size_t i = 0; i < p2c_vec_len(n->u.delete.targets); i++) {
                P2C_AstExpr *dt = (P2C_AstExpr*)p2c_vec_get(n->u.delete.targets, i);
                if (!dt) continue;
                indent(cg);
                if (dt->base.type == AST_ATTRIBUTE) {
                    write_str(cg, "p2c_delattr(");
                    gen_expr(cg, dt->base.u.attribute.value);
                    write_str(cg, ", \""); write_str(cg, dt->base.u.attribute.attr); write_str(cg, "\");");
                } else if (dt->base.type == AST_SUBSCRIPT) {
                    write_str(cg, "p2c_subscript_delete(");
                    gen_expr(cg, dt->base.u.subscript.value);
                    write_str(cg, ", ");
                    gen_expr(cg, dt->base.u.subscript.slice);
                    write_str(cg, ");");
                } else if (dt->base.type == AST_CALL && dt->base.u.call.func &&
                           dt->base.u.call.func->base.type == AST_NAME &&
                           strcmp(dt->base.u.call.func->base.u.name.name, "p2c_obj_slice") == 0 &&
                           dt->base.u.call.args && p2c_vec_len(dt->base.u.call.args) == 4) {
                    write_str(cg, "p2c_slice_delete(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(dt->base.u.call.args, 0)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(dt->base.u.call.args, 1)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(dt->base.u.call.args, 2)); write_str(cg, ", ");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(dt->base.u.call.args, 3)); write_str(cg, ");");
                } else {
                    gen_expr(cg, dt); write_str(cg, " = &P2C_None;");
                }
                write_newline(cg);
            }
            break;
        default:
            /* 未対応の文。--fallback ではランタイムスタブ、既定では変換エラー。 */
            emit_unsupported_construct(cg, n, "statement", false);
            break;
    }
}

static void gen_stmt_list(P2C_CodeGen *cg, P2C_Vector *stmts) {
    if (!stmts) return;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) gen_stmt(cg, (P2C_AstStmt*)p2c_vec_get(stmts, i));
}

/* --embed-entry で指定されたエントリ名が、そのままCの識別子として生成Cへ
 * 埋め込めるかを検査する。スクリプト/ビルドから任意文字列が渡るため、
 * 宣言と定義の両方に使う前に必ず検証する（不正なら診断して生成しない）。 */
static bool is_valid_c_identifier(const char *name) {
    if (!name || !*name) return false;
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_')) return false;
    for (const char *p = name + 1; *p; p++) {
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
    return true;
}

P2C_Result p2c_codegen_generate(P2C_CodeGen *cg, P2C_AstModule *mod, char **out_code) {
    if (!cg || !mod || !out_code) return P2C_ERR_INTERNAL;
    if (cg->opts.embed_entry && !is_valid_c_identifier(cg->opts.embed_entry)) {
        codegen_set_error(cg, P2C_ERR_SYNTAX,
                          "--embed-entry requires a valid C identifier (letters, digits and '_', not starting with a digit)");
        return cg->last_error;
    }
    p2c_str_append(cg->header, "/* Generated by Python Code to C Alpha0.6 */\n");
    p2c_str_append(cg->header, "#include \"runtime/python_code_to_c_runtime.h\"\n\n");
    /* PYTHON_CODE_TO_C_NO_STDLIB でビルドするhobby OS向け：コンパイル時に -DPYTHON_CODE_TO_C_HEAP_SIZE=N を
     * 指定するだけで、この静的バッファがランタイムのヒープとして使われる。
     * 指定しなければ従来通り p2c_runtime_init(NULL, 0) のまま
     * （hosted環境ではこの引数自体を無視して普通のmallocを使うので影響なし）。
     * embed entry (--embed-entry) ではヒープの所有権がカーネル側にあるため
     * この静的バッファは生成しない。 */
    if (!cg->opts.embed_entry) {
        p2c_str_append(cg->header, "#ifdef PYTHON_CODE_TO_C_HEAP_SIZE\n");
        p2c_str_append(cg->header, "static unsigned char p2c_static_heap[PYTHON_CODE_TO_C_HEAP_SIZE];\n");
        p2c_str_append(cg->header, "#endif\n\n");
    } else {
        p2c_str_append(cg->forward, "/* Kernel entry generated by --embed-entry.\n");
        p2c_str_append(cg->forward, " * この関数はカーネルから直接呼び出せる P2C_Object* (*)(void) であり、\n");
        p2c_str_append(cg->forward, " * int main() を生成しないため、baremetal ターゲットでもリンクできる。\n");
        p2c_str_append(cg->forward, " * 呼び出し前にカーネル側で p2c_runtime_init() と p2c_gc_init() を一度実行し、\n");
        p2c_str_append(cg->forward, " * スタック境界を p2c_gc_set_stack_bounds() で登録するか、\n");
        p2c_str_append(cg->forward, " * p2c_embed_start()/p2c_embed_run_program() を利用すること\n");
        p2c_str_append(cg->forward, " * (docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md 参照)。 */\n");
        p2c_str_append(cg->forward, "P2C_Object* ");
        p2c_str_append(cg->forward, cg->opts.embed_entry);
        p2c_str_append(cg->forward, "(void);\n");
    }

    P2C_Vector *mod_body = ((P2C_AstNode*)mod)->u.module.body;
    scan_stmt_list(cg, mod_body);
    collect_module_globals(cg, mod_body);

    /* モジュールトップレベルの変数は、main()内のローカル変数ではなく
     * 実際のCファイルスコープ変数として宣言する。これにより、他の関数から
     * `global x` で参照・再代入できるようになる
     * （以前はここが main() 内のローカル変数になっており、他の関数からは
     * 一切参照できず、`global` 文自体も無視されていた）。 */
    for (size_t i = 0; i < cg->module_globals->bucket_count; i++) {
        for (P2C_MapEntry *e = cg->module_globals->buckets[i]; e; e = e->next) {
            p2c_str_append(cg->forward, "P2C_Object *");
            p2c_str_append(cg->forward, mangle_ident((const char*)e->key));
            p2c_str_append(cg->forward, " = &P2C_None;\n");
        }
    }
    for (size_t i = 0; i < cg->decorated_names->bucket_count; i++) {
        for (P2C_MapEntry *e = cg->decorated_names->buckets[i]; e; e = e->next) {
            p2c_str_append(cg->forward, "P2C_Object *_p2c_decorated_");
            p2c_str_append(cg->forward, mangle_ident((const char*)e->key));
            p2c_str_append(cg->forward, " = &P2C_None;\n");
        }
    }

    if (cg->opts.embed_entry) {
        p2c_str_append(cg->body, "P2C_Object* ");
        p2c_str_append(cg->body, cg->opts.embed_entry);
        p2c_str_append(cg->body, "(void) {\n");
    } else {
        p2c_str_append(cg->body, "int main(void) {\n");
    }
    cg->indent_level = 1;
    reset_declared_vars(cg);
    if (!cg->opts.embed_entry) {
        write_line(cg, "#ifdef PYTHON_CODE_TO_C_HEAP_SIZE");
        write_line(cg, "p2c_runtime_init(p2c_static_heap, sizeof(p2c_static_heap));");
        write_line(cg, "#else");
        write_line(cg, "p2c_runtime_init(NULL, 0);");
        write_line(cg, "#endif");
        /* GC 初期化: Linux では pthread_getattr_np() で OS からスタック境界を
         * 自動取得する。非 Linux（自作OS/ベアメタル）でも、この main() 自身の
         * フレーム内アドレスを「スタック上端のヒント」として渡すことで保守的
         * スタックスキャンが有効になり、ローカル変数からしか到達できない
         * オブジェクトが収集されなくなる（走査は使用中の範囲だけ）。
         * 以前は NULL を渡していたため、非 Linux では自動GCが安全側停止し、
         * ローカル変数の保護も行われなかった。 */
        write_line(cg, "P2C_GC_ENTER_MAIN();");
    } else {
        /* embed entry: ランタイム/GCの初期化はカーネル側の責務。
         * ここで初期化するとカーネルが設定したスタック境界を上書きしてしまい、
         * 保守的スタックスキャンが無効化されるため、意図的に生成しない。 */
        write_line(cg, "/* p2c_runtime_init()/p2c_gc_init()/p2c_gc_set_stack_bounds() は呼び出し側で完了済み。 */");
    }

    /* モジュールグローバル変数をルートとして登録する。
     * これらは C ファイルスコープ変数 (forward セクション) なので
     * 保守的スタックスキャンではカバーされない。 */
    for (size_t _gi = 0; _gi < cg->module_globals->bucket_count; _gi++) {
        for (P2C_MapEntry *_ge = cg->module_globals->buckets[_gi]; _ge; _ge = _ge->next) {
            indent(cg); write_str(cg, "p2c_gc_register_root(&");
            write_str(cg, mangle_ident((const char*)_ge->key));
            write_str(cg, ");"); write_newline(cg);
        }
    }
    for (size_t _di = 0; _di < cg->decorated_names->bucket_count; _di++) {
        for (P2C_MapEntry *_de = cg->decorated_names->buckets[_di]; _de; _de = _de->next) {
            indent(cg); write_str(cg, "p2c_gc_register_root(&_p2c_decorated_");
            write_str(cg, mangle_ident((const char*)_de->key));
            write_str(cg, ");"); write_newline(cg);
        }
    }
    write_newline(cg);

    /* main()のdeclared_varsにモジュールグローバルを先に登録しておくことで、
     * 以降のpredeclare_stmt/declare_name_if_neededがこれらをローカル変数として
     * 再宣言しないようにする（既にファイルスコープで宣言済みのため）。 */
    for (size_t i = 0; i < cg->module_globals->bucket_count; i++) {
        for (P2C_MapEntry *e = cg->module_globals->buckets[i]; e; e = e->next) {
            remember_declared(cg, (const char*)e->key);
        }
    }

    /* predeclare class globals inside main for direct value usage */
    for (size_t i = 0; i < p2c_vec_len(mod_body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(mod_body, i);
        if (stmt->base.type == AST_CLASSDEF) {
            indent(cg); write_str(cg, "if (!"); write_str(cg, stmt->base.u.classdef.name); write_str(cg, ") "); write_str(cg, stmt->base.u.classdef.name); write_str(cg, " = "); write_str(cg, stmt->base.u.classdef.name); write_str(cg, "__classobj();"); write_newline(cg);
            remember_declared(cg, stmt->base.u.classdef.name);
        }
    }
    if (p2c_vec_len(mod_body) > 0) write_newline(cg);

    for (size_t i = 0; i < p2c_vec_len(mod_body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(mod_body, i);
        if (stmt->base.type != AST_FUNCTIONDEF && stmt->base.type != AST_CLASSDEF) predeclare_stmt(cg, stmt);
    }
    if (p2c_vec_len(mod_body) > 0) write_newline(cg);

    int saved_indent = cg->indent_level;
    for (size_t i = 0; i < p2c_vec_len(mod_body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(mod_body, i);
        if (stmt->base.type == AST_FUNCTIONDEF || stmt->base.type == AST_CLASSDEF) {
            P2C_String *saved = cg->current; cg->current = cg->toplevel; cg->indent_level = 0;
            gen_stmt(cg, stmt); write_newline(cg);
            cg->current = saved; cg->indent_level = saved_indent;
            if (stmt->base.type == AST_FUNCTIONDEF && stmt->base.u.functiondef.decorator_list && p2c_vec_len(stmt->base.u.functiondef.decorator_list) > 0) {
                gen_decorator_application(cg, &stmt->base.u.functiondef);
            } else if (stmt->base.type == AST_CLASSDEF && stmt->base.u.classdef.decorator_list && p2c_vec_len(stmt->base.u.classdef.decorator_list) > 0) {
                gen_class_decorator_application(cg, &stmt->base.u.classdef);
            }
        } else {
            gen_stmt(cg, stmt);
        }
    }

    write_newline(cg);
    if (cg->opts.embed_entry) {
        /* カーネルがタスクの生存期間を管理するため、ここでは shutdown しない。
         * モジュールの最終式の値（無ければNone）を返し、呼び出し側が
         * 必要に応じて p2c_embed_stop()/p2c_runtime_shutdown() を実行する。 */
        write_line(cg, "return &P2C_None;");
    } else {
        write_line(cg, "p2c_runtime_shutdown();");
        write_line(cg, "return 0;");
    }
    cg->indent_level = 0;
    p2c_str_append(cg->body, "}\n");

    /* 退避していたネスト定義（ラムダ等）を forward の末尾へ流し込む。
     * いずれもファイルスコープの定義なので、本体より前にあれば順序は問わない。 */
    if (cg->deferred_defs) {
        for (size_t _i = 0; _i < p2c_vec_len(cg->deferred_defs); _i++) {
            P2C_String *_d = (P2C_String*)p2c_vec_get(cg->deferred_defs, _i);
            if (_d) p2c_str_append(cg->forward, p2c_str_cstr(_d));
        }
    }
    size_t total_len = p2c_str_len(cg->header) + p2c_str_len(cg->forward) + p2c_str_len(cg->toplevel) + p2c_str_len(cg->body) + 1;
    char *result = p2c_alloc(cg->alloc, total_len);
    if (!result) return P2C_ERR_NOMEM;
    size_t pos = 0;
    strcpy(result + pos, p2c_str_cstr(cg->header)); pos += strlen(p2c_str_cstr(cg->header));
    strcpy(result + pos, p2c_str_cstr(cg->forward)); pos += strlen(p2c_str_cstr(cg->forward));
    strcpy(result + pos, p2c_str_cstr(cg->toplevel)); pos += strlen(p2c_str_cstr(cg->toplevel));
    strcpy(result + pos, p2c_str_cstr(cg->body));
    /* cg->last_error は生成処理中に codegen_set_error() で設定されることがある
     * （例: 対応していない組み合わせの *args アンパックを検出した場合）。
     * これまでここでチェックしておらず、常に P2C_OK を返してしまっていたため、
     * 呼び出し元(python_to_c)がエラーを検知できず、壊れたコードがそのまま
     * "成功" として返っていた。ここで正しく検査して返すようにする。 */
    if (cg->last_error != P2C_OK) {
        p2c_free(cg->alloc, result);
        *out_code = NULL;
        return cg->last_error;
    }
    *out_code = result;
    return P2C_OK;
}

P2C_Result p2c_codegen_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) { gen_stmt(cg, stmt); return P2C_OK; }
P2C_Result p2c_codegen_expr(P2C_CodeGen *cg, P2C_AstExpr *expr, P2C_String *out) {
    if (!cg || !out) return P2C_ERR_INTERNAL;
    P2C_String *saved = cg->current; cg->current = out; gen_expr(cg, expr); cg->current = saved; return P2C_OK;
}
