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
    false,
    false   /* unbox_int_locals: AOT アンボクシング（既定 OFF。--unbox で有効化） */
};

static void gen_expr(P2C_CodeGen *cg, P2C_AstExpr *expr);
static void cref_expr(P2C_AstExpr *e, bool ic, P2C_Map *out);
static void cref_expr_list(P2C_Vector *v, bool ic, P2C_Map *out);

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
    /* C11のキーワード。Pythonでは合法な識別子（double/float/int/struct等）を
     * そのまま出力すると生成Cがコンパイルできない（"expected identifier
     * before 'double'"）。libc名と同じく "p2c_user_" を付けて回避する。 */
    static const char *c_keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "inline", "int", "long", "register", "restrict", "return", "short",
        "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while",
        "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic",
        "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local",
        NULL
    };
    for (int i = 0; reserved[i]; i++) {
        if (strcmp(name, reserved[i]) == 0) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "p2c_user_%s", name);
            return buf;
        }
    }
    for (int i = 0; c_keywords[i]; i++) {
        if (strcmp(name, c_keywords[i]) == 0) {
            static char buf[128];
            snprintf(buf, sizeof(buf), "p2c_user_%s", name);
            return buf;
        }
    }
    /* 生成コードがフレーム引数や内部変数に使う名前。ユーザーが同じ名前を
     * 使うと、メソッドadapter等で `args` の再宣言などコンパイルエラーになる
     * （例: def __init__(self, *args, **kw)）。ユーザー側の識別子を一律に
     * マングルして衝突を避ける（参照側も write_ident 経由で一貫する）。 */
    static const char *generated_internal[] = {
        "args", "nargs", "env", "generator",
        "kw_names", "kw_values", "nkw", NULL
    };
    for (int i = 0; generated_internal[i]; i++) {
        if (strcmp(name, generated_internal[i]) == 0) {
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
        "sum", "sorted", "reversed", "enumerate", "zip", "type", "isinstance", "issubclass", "any", "all",
        "map", "filter", "list", "tuple", "dict", "set", "frozenset", "ord", "chr", "bin",
        "oct", "hex", "repr", "ascii", "open", "vars", "bytes", "iter", "next", "input", "hasattr", "getattr", "setattr",
        "delattr", "divmod", "pow", "format", "callable", "hash", NULL
    };
    if (!name) return false;
    for (int i = 0; callables[i]; i++) {
        if (strcmp(name, callables[i]) == 0) return true;
    }
    return false;
}

/* CPython の組込み関数・型の名前（小文字で始まるもの）。この実装が対応していない組込みを
 * 呼んだときに、「未定義の名前」ではなく「未対応の組込み」と診断するために使う。 */
static bool is_python_builtin_name(const char *name) {
    static const char *names[] = {
        "abs", "aiter", "all", "anext", "any", "ascii", "bin", "bool",
        "breakpoint", "bytearray", "bytes", "callable", "chr", "classmethod", "compile", "complex",
        "delattr", "dict", "dir", "divmod", "enumerate", "eval", "exec", "exit",
        "filter", "float", "format", "frozenset", "getattr", "globals", "hasattr", "hash",
        "help", "hex", "id", "input", "int", "isinstance", "issubclass", "iter",
        "len", "list", "locals", "map", "max", "memoryview", "min", "next",
        "object", "oct", "open", "ord", "pow", "print", "property", "quit",
        "range", "repr", "ascii", "open", "vars", "reversed", "round", "set", "setattr", "slice", "sorted",
        "staticmethod", "str", "sum", "super", "tuple", "type", "vars", "zip", NULL
    };
    if (!name) return false;
    for (int i = 0; names[i]; i++) if (strcmp(names[i], name) == 0) return true;
    return false;
}

static void write_ident(P2C_CodeGen *cg, const char *name);
static void write_param_ident(P2C_CodeGen *cg, const char *name);
static void gen_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
static void gen_stmt_list(P2C_CodeGen *cg, P2C_Vector *stmts);
static void predeclare_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
static void predeclare_expr(P2C_CodeGen *cg, P2C_AstExpr *expr);
static void scan_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
static void gen_suspension_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd);
static void gen_generator_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd);
static void gen_generator_expression(P2C_CodeGen *cg, P2C_AstExpr *expr);
static bool is_builtin_exception_name(const char *name);
/* AOT アンボクシング（--unbox）: 定義は後方（literal ヘルパの後）。 */
static bool is_native_int_local(P2C_CodeGen *cg, const char *name);

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

/* ============================================================
 * 既定値引数の「定義時一度だけの評価」
 * ============================================================
 * Python は `def f(a, b=expr)` の expr を def 文の実行時に一度だけ評価し、
 * その値を既定値として保持する（可変既定値の共有・グローバル変更の非反映・
 * `def f(x, i=i)` イディオムがこれに依存する）。
 * 以前は既定値式を呼び出し地点へそのまま展開していたため、これらがすべて
 * CPython と異なり、`i=i` は自己参照（未初期化変数）で不正なCになっていた。
 *
 * モジュール関数/メソッドは Cファイルスコープの隠しグローバル
 *   _p2c_default_<key>_<index>
 * に保存し、ネスト関数/ラムダはクロージャenvの "__p2c_def_<index>" に保存する。
 * key は C 識別子として安全な一意名（モジュール関数は関数名、メソッドは
 * "クラス名__メソッド名"）。
 * ============================================================ */
static void default_global_name(char *buf, size_t cap, const char *key, size_t index) {
    snprintf(buf, cap, "_p2c_default_%s_%zu", key ? key : "fn", index);
}

/* 既定値を参照する式を出力する。key が非NULLなら隠しグローバルから読む。 */
static void emit_default_value(P2C_CodeGen *cg, const char *key, size_t index, P2C_AstExpr *dflt) {
    if (key && dflt) {
        char gname[256];
        default_global_name(gname, sizeof(gname), key, index);
        write_str(cg, gname);
    } else if (dflt) {
        gen_expr(cg, dflt);
    } else {
        write_str(cg, "&P2C_None");
    }
}

/* 隠しグローバルを宣言し、現在の出力位置で既定値を評価して代入＋GCルート登録する。
 * key/fd が無い場合は何もしない。 */
static void emit_default_initializers(P2C_CodeGen *cg, const char *key, P2C_AstFunctionDef *fd) {
    if (!key || !fd || !fd->args) return;
    size_t n = p2c_vec_len(fd->args);
    for (size_t i = 0; i < n; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (!arg || !arg->default_val) continue;
        char gname[256];
        default_global_name(gname, sizeof(gname), key, i);
        p2c_str_append(cg->forward, "P2C_Object *");
        p2c_str_append(cg->forward, gname);
        p2c_str_append(cg->forward, " = &P2C_None;\n");
        indent(cg); write_str(cg, gname); write_str(cg, " = "); gen_expr(cg, arg->default_val); write_str(cg, ";"); write_newline(cg);
        indent(cg); write_str(cg, "p2c_gc_register_root(&"); write_str(cg, gname); write_str(cg, ");"); write_newline(cg);
    }
}

/* ネスト関数/ラムダのクロージャenvへ既定値を保存するためのキー名。 */
static void default_env_key(char *buf, size_t cap, size_t index) {
    snprintf(buf, cap, "__p2c_def_%zu", index);
}

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
    cg->in_suspension_body = false;
    cg->declared_vars = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->module_scope_declared = cg->declared_vars;
    cg->global_decl_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->synthetic_locals = NULL;
    cg->known_classes = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->module_globals = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->class_init_adapter = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->func_args = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->decorated_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->module_function_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->decorator_callable_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->function_value_names = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->class_methods = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->class_init_defs = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->comprehension_targets = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->current_class = NULL;
    cg->ord_frames = NULL;
    cg->closure_env_names = NULL;
    cg->nonlocal_names = NULL;
    cg->cell_names = NULL;
    cg->closure_env_var = NULL;
    cg->genexpr_locals = NULL;
    cg->lit_strs = p2c_map_new(a, p2c_hash_str, p2c_eq_str);
    cg->lit_str_counter = 0;
    cg->native_int_locals = NULL;
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

/* 文字列リテラルの共有表（キー＝エスケープ済みテキスト、値＝共有実体の名前）は
 * キーと値が別々の確保なので、両方を解放する。 */
static void free_lit_str_map(P2C_Map *map) {
    if (!map) return;
    for (size_t i = 0; i < map->bucket_count; i++) {
        for (P2C_MapEntry *entry = map->buckets[i]; entry; entry = entry->next) {
            if (entry->key) p2c_free(map->alloc, entry->key);
            if (entry->val) p2c_free(map->alloc, entry->val);
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
    free_name_map(cg->function_value_names);
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
    if (cg->class_init_defs) free_name_map(cg->class_init_defs);
    free_name_map(cg->comprehension_targets);
    free_lit_str_map(cg->lit_strs);
    free_name_map(cg->native_int_locals);
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

/* 位置つきの診断。未対応構文は「どの行の何が」を必ず示す
 * （"unsupported construct" だけでは、どの構文を避ければよいか分からない）。 */
static void codegen_set_error_at(P2C_CodeGen *cg, P2C_Result err, uint32_t line, uint32_t col,
                                 const char *what, const char *detail) {
    if (!cg || cg->last_error != P2C_OK) return;
    char buf[512];
    if (line > 0u) {
        snprintf(buf, sizeof(buf), "line %u, col %u: unsupported construct: %s%s%s%s",
                 line, col, what ? what : "?",
                 detail ? " (" : "", detail ? detail : "", detail ? ")" : "");
    } else {
        snprintf(buf, sizeof(buf), "unsupported construct: %s%s%s%s",
                 what ? what : "?",
                 detail ? " (" : "", detail ? detail : "", detail ? ")" : "");
    }
    codegen_set_error(cg, err, buf);
}

/* 診断メッセージ用の文種別ラベル。 */
static const char *stmt_kind_label(P2C_AstStmt *stmt) {
    if (!stmt) return "empty statement";
    switch (stmt->base.type) {
        case AST_FOR: return "for statement";
        case AST_ASYNC_FOR: return "async for statement";
        case AST_WHILE: return "while statement";
        case AST_IF: return "if statement";
        case AST_WITH: return "with statement";
        case AST_TRY: return "try statement";
        case AST_MATCH: return "match statement";
        case AST_RAISE: return "raise statement";
        case AST_RETURN: return "return statement";
        case AST_ASSERT: return "assert statement";
        case AST_DELETE: return "del statement";
        case AST_IMPORT: return "import statement";
        case AST_IMPORTFROM: return "from-import statement";
        case AST_GLOBAL: return "global statement";
        case AST_NONLOCAL: return "nonlocal statement";
        case AST_FUNCTIONDEF: return "nested def statement";
        case AST_CLASSDEF: return "nested class statement";
        default: return "statement";
    }
}


static void reset_declared_vars(P2C_CodeGen *cg) {
    if (cg->declared_vars) p2c_map_free(cg->declared_vars);
    cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    cg->synthetic_locals = NULL;
}
static void remember_declared(P2C_CodeGen *cg, const char *name) { map_set_name(cg->declared_vars, name); }
/* コード生成が導入した一時ローカル（入れ子アンパック等）を「宣言済みの名前」として
 * 扱いつつ、クロージャ捕捉の候補からは外す。declared_vars に入れると、後続の
 * ラムダ/クロージャ生成がその名前を「外側の変数」として取り込んでしまい、
 * 存在しない変数を捕捉する壊れたCになる。 */
static void remember_synthetic_local(P2C_CodeGen *cg, const char *name) {
    if (!cg->synthetic_locals) cg->synthetic_locals = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    map_set_name(cg->synthetic_locals, name);
}
static bool is_synthetic_local(P2C_CodeGen *cg, const char *name) {
    return name && cg->synthetic_locals && map_has_name(cg->synthetic_locals, name);
}
static bool is_declared(P2C_CodeGen *cg, const char *name) { return map_has_name(cg->declared_vars, name); }

static void declare_name_if_needed(P2C_CodeGen *cg, const char *name) {
    if (!name || is_declared(cg, name) || map_has_name(cg->nonlocal_names, name)) return;
    /* 先に登録する: 宣言の名前と、以後の参照（write_ident）の名前を一致させるため */
    remember_declared(cg, name);
    /* AOT アンボクシング: int しか入らないと証明できたローカルは C の int64_t にする。 */
    if (is_native_int_local(cg, name)) {
        indent(cg); write_str(cg, "int64_t "); write_ident(cg, name); write_str(cg, " = 0;"); write_newline(cg);
    } else {
        indent(cg); write_str(cg, "P2C_Object *"); write_ident(cg, name); write_str(cg, cg->in_suspension_body ? " = &P2C_None;" : " = &P2C_Unbound;"); write_newline(cg);
    }
}

/* 代入・for のターゲットが束縛する名前を再帰的に訪問する（入れ子のタプル/リストと
 * 先頭 '*' に対応）。以前は1段しか見ておらず、`(a, b), c = ...` のような
 * 入れ子アンパックで内側の名前が宣言されず NameError になっていた。 */
static void for_each_target_name(P2C_AstExpr *target, void (*fn)(P2C_CodeGen*, const char*), P2C_CodeGen *cg) {
    if (!target) return;
    switch (target->base.type) {
        case AST_NAME:
            fn(cg, target->base.u.name.name);
            break;
        case AST_STARRED:
            for_each_target_name(target->base.u.starred.value, fn, cg);
            break;
        case AST_TUPLE: case AST_LIST: {
            P2C_Vector *elts = target->base.u.list.elts;
            for (size_t i = 0; elts && i < p2c_vec_len(elts); i++)
                for_each_target_name((P2C_AstExpr*)p2c_vec_get(elts, i), fn, cg);
            break;
        }
        default:
            break;   /* 属性・添字ターゲットは宣言不要 */
    }
}

static void declare_name_if_needed_cb(P2C_CodeGen *cg, const char *name) { declare_name_if_needed(cg, name); }

/* except ... as NAME の束縛変数は setjmp/longjmp をまたいで書き込まれ、
 * longjmp 後に読まれる。C11 6.13.2.1 により、そのような自動変数は volatile
 * でなければ値が不定になりうる（-Wclobbered が指摘するレジスタ退避の最適化が
 * まさにその状況）。他と同様に宣言済み管理へ登録しつつ volatile を付ける。 */
static void declare_volatile_name_if_needed(P2C_CodeGen *cg, const char *name) {
    if (!name || is_declared(cg, name) || map_has_name(cg->nonlocal_names, name)) return;
    remember_declared(cg, name);
    indent(cg); write_str(cg, "P2C_Object * volatile "); write_ident(cg, name); write_str(cg, cg->in_suspension_body ? " = &P2C_None;" : " = &P2C_Unbound;"); write_newline(cg);
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

/* ── 文字列リテラルの共有実体 ─────────────────────────────────────────
 * コンパイル時に内容が確定する文字列は、確保を伴う p2c_obj_from_str("...")
 * ではなく、モジュール内で共有する静的オブジェクト（P2C_STATIC_STR）を参照する。
 * 内容が同じものは1回だけ定義するので、リテラルを使うループでも
 * 「初回から」確保もGC負荷も発生しない。
 *
 * escaped_text は C の文字列リテラルの「中身」（エスケープ済み・引用符なし）。
 * text_len は Python 文字列表現としてのバイト長（strlen 相当）。
 * 定義は header（本体より前に出力される）へ追記する。
 *
 * 表の値は共有実体の名前（char*）で、キーはエスケープ済みテキストのコピー。
 * どちらもコンパイラ寿命なので p2c_codegen_free() でまとめて解放する。
 * ポインタ⇔整数のキャストを使わないのは、フリースタンディング構成で
 * <stdint.h>（intptr_t）が無い場合にもビルドできるようにするため。 */
static void write_static_str_ref(P2C_CodeGen *cg, const char *escaped_text, size_t text_len) {
    char *name = (char*)p2c_map_get(cg->lit_strs, escaped_text);
    if (!name) {
        char buf[40];
        snprintf(buf, sizeof(buf), "_p2c_lit_str_%d", ++cg->lit_str_counter);
        size_t text_n = strlen(escaped_text);
        char *key   = (char*)p2c_alloc(cg->alloc, text_n + 1);
        char *fresh = (char*)p2c_alloc(cg->alloc, strlen(buf) + 1);
        if (!key || !fresh) {
            /* 記録できないときは、その場で生成する式へ安全に落とす。 */
            if (key) p2c_free(cg->alloc, key);
            if (fresh) p2c_free(cg->alloc, fresh);
            write_str(cg, "p2c_obj_from_str(\"");
            write_str(cg, escaped_text);
            write_str(cg, "\")");
            return;
        }
        memcpy(key, escaped_text, text_n + 1);
        memcpy(fresh, buf, strlen(buf) + 1);
        p2c_map_insert(cg->lit_strs, key, fresh);
        name = fresh;
        p2c_str_append(cg->header, "P2C_STATIC_STR(");
        p2c_str_append(cg->header, name);
        p2c_str_append(cg->header, ", \"");
        p2c_str_append(cg->header, escaped_text);
        p2c_str_append(cg->header, "\", ");
        p2c_str_append_fmt(cg->header, "%zu", text_len);
        p2c_str_append(cg->header, ");\n");
    }
    write_str(cg, "(&");
    write_str(cg, name);
    write_str(cg, ")");
}

/* 識別子・キーワード名・辞書キーなど、エスケープ不要な名前の共有リテラル。
 * Python の識別子は `"` と `\` を含めないため、そのままC文字列の中身になる。 */
static void write_name_lit_str(P2C_CodeGen *cg, const char *name) {
    if (!name) { write_str(cg, "(&P2C_None)"); return; }
    write_static_str_ref(cg, name, strlen(name));
}

/* ユーザーの文字列リテラル（任意内容）。エスケープしてから共有実体にする。 */
static void write_user_lit_str(P2C_CodeGen *cg, const char *value) {
    if (!value) { write_str(cg, "(&P2C_None)"); return; }
    P2C_String *scratch = p2c_str_new(cg->alloc);
    if (!scratch) { write_str(cg, "(&P2C_None)"); return; }
    P2C_String *saved = cg->current;
    cg->current = scratch;                /* gen_string_literal_contents は current へ書く */
    gen_string_literal_contents(cg, value);
    cg->current = saved;
    write_static_str_ref(cg, p2c_str_cstr(scratch), strlen(value));
    p2c_str_free(scratch);
}

/* ── AOT アンボクシング（int 型付きローカル。--unbox）─────────────────────
 * 「int しか入らないと証明できた」関数ローカルを C の int64_t で持ち、
 * 算術・比較・代入を P2C_Object を作らずに計算する。解析は関数単位の前処理
 * （analyze_native_int_locals）で行い、結果を cg->native_int_locals に入れる。
 *
 * 読み出しは既定でボックス化する（p2c_obj_from_int）。そのため、生の int64 が
 * P2C_Object* の要る場所へ漏れた場合は生成Cの**コンパイルエラー**になり、
 * 静かな誤動作にはならない。 */

static bool is_native_int_local(P2C_CodeGen *cg, const char *name) {
    return cg && name && cg->native_int_locals && map_has_name(cg->native_int_locals, name);
}

/* int リテラルが int64 に収まるか。桁数だけで判定する（19 桁以上は対象外。
 * C の定数としてはみ出すと未定義動作になるため、確実に収まる範囲に限る）。 */
static bool int_literal_fits_i64(const char *text) {
    if (!text || !*text) return false;
    const char *p = text;
    if (*p == '+' || *p == '-') p++;
    size_t digits = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;   /* 0x/0o/アンダースコア等は対象外 */
        digits++;
    }
    return digits > 0 && digits <= 18;
}

/* 生の int64 演算で扱える二項演算子（/ と ** は int に閉じないので対象外）。 */
static const char *raw_binop_func(P2C_AstOperator op) {
    switch (op) {
        case OP_ADD: return "p2c_int_add_raw";
        case OP_SUB: return "p2c_int_sub_raw";
        case OP_MULT: return "p2c_int_mul_raw";
        case OP_FLOORDIV: return "p2c_int_floordiv_raw";
        case OP_MOD: return "p2c_int_mod_raw";
        case OP_BITAND: return "p2c_int_bitand_raw";
        case OP_BITOR: return "p2c_int_bitor_raw";
        case OP_BITXOR: return "p2c_int_bitxor_raw";
        case OP_LSHIFT: return "p2c_int_lshift_raw";
        case OP_RSHIFT: return "p2c_int_rshift_raw";
        default: return NULL;
    }
}

static bool name_is_int_candidate(P2C_Map *cand, P2C_Map *bad, const char *name) {
    return cand && name && map_has_name(cand, name) && !(bad && map_has_name(bad, name));
}

/* 比較: int 同士は C の演算子、相手が P2C_Object* の混在はヘルパを使う。 */
static const char *c_cmp_symbol(P2C_AstOperator op) {
    switch (op) {
        case OP_LT: return "<";
        case OP_LE: return "<=";
        case OP_GT: return ">";
        case OP_GE: return ">=";
        case OP_EQ: return "==";
        case OP_NE: return "!=";
        default: return NULL;
    }
}
static const char *raw_cmp_helper(P2C_AstOperator op) {
    switch (op) {
        case OP_LT: return "p2c_int_lt_obj";
        case OP_LE: return "p2c_int_le_obj";
        case OP_GT: return "p2c_int_gt_obj";
        case OP_GE: return "p2c_int_ge_obj";
        case OP_EQ: return "p2c_int_eq_obj";
        case OP_NE: return "p2c_int_ne_obj";
        default: return NULL;
    }
}
/* a OP b ⇔ b swapped(OP) a（生の比較は左辺を int64 にする必要があるため）。 */
static P2C_AstOperator swapped_cmp(P2C_AstOperator op) {
    switch (op) {
        case OP_LT: return OP_GT;
        case OP_LE: return OP_GE;
        case OP_GT: return OP_LT;
        case OP_GE: return OP_LE;
        default: return op;
    }
}

/* 式が「int に閉じている」と言えるか。解析（cand/bad）と生成
 * （cg->native_int_locals / bad=NULL）で同じ判定を使う。 */
static bool expr_is_int_typed_with(P2C_CodeGen *cg, P2C_AstExpr *e, P2C_Map *cand, P2C_Map *bad) {
    if (!e) return false;
    switch (e->base.type) {
        case AST_CONST:
            return e->base.u.constant.token_type == TOK_INT_LITERAL &&
                   int_literal_fits_i64(e->base.u.constant.value);
        case AST_NAME:
            return name_is_int_candidate(cand, bad, e->base.u.name.name);
        case AST_UNARYOP:
            if (e->base.u.unaryop.op == OP_USUB || e->base.u.unaryop.op == OP_UADD)
                return expr_is_int_typed_with(cg, e->base.u.unaryop.operand, cand, bad);
            return false;
        case AST_BINOP:
            if (e->base.u.binop.is_fstring_concat) return false;
            if (!raw_binop_func(e->base.u.binop.op)) return false;
            return expr_is_int_typed_with(cg, e->base.u.binop.left, cand, bad) &&
                   expr_is_int_typed_with(cg, e->base.u.binop.right, cand, bad);
        case AST_IFEXP:
            return expr_is_int_typed_with(cg, e->base.u.ifexp.body, cand, bad) &&
                   expr_is_int_typed_with(cg, e->base.u.ifexp.orelse, cand, bad);
        default:
            return false;
    }
}

static bool expr_is_int_typed(P2C_CodeGen *cg, P2C_AstExpr *e) {
    return expr_is_int_typed_with(cg, e, cg->native_int_locals, NULL);
}

/* int64_t 型の C 式を出力する（expr_is_int_typed が真の式にだけ使う）。 */
static const char *ord_lookup(P2C_CodeGen *cg, const P2C_AstExpr *e);
static void gen_int_expr(P2C_CodeGen *cg, P2C_AstExpr *e) {
    if (!e) { write_str(cg, "0"); return; }
    {
        const char *tmp = ord_lookup(cg, e);
        if (tmp) { write_str(cg, "p2c_obj_as_int("); write_str(cg, tmp); write_str(cg, ")"); return; }
    }
    switch (e->base.type) {
        case AST_CONST:
            if (e->base.u.constant.token_type == TOK_INT_LITERAL) {
                write_str(cg, "((int64_t)"); write_str(cg, e->base.u.constant.value); write_str(cg, ")");
                return;
            }
            break;
        case AST_NAME:
            if (is_native_int_local(cg, e->base.u.name.name)) {
                write_ident(cg, e->base.u.name.name);
                return;
            }
            break;
        case AST_UNARYOP:
            if (e->base.u.unaryop.op == OP_USUB) {
                write_str(cg, "p2c_int_neg_raw("); gen_int_expr(cg, e->base.u.unaryop.operand); write_str(cg, ")");
                return;
            }
            if (e->base.u.unaryop.op == OP_UADD) { gen_int_expr(cg, e->base.u.unaryop.operand); return; }
            if (e->base.u.unaryop.op == OP_INVERT) {
                write_str(cg, "p2c_int_invert_raw("); gen_int_expr(cg, e->base.u.unaryop.operand); write_str(cg, ")");
                return;
            }
            break;
        case AST_BINOP: {
            const char *f = e->base.u.binop.is_fstring_concat ? NULL : raw_binop_func(e->base.u.binop.op);
            if (f) {
                write_str(cg, f); write_str(cg, "(");
                gen_int_expr(cg, e->base.u.binop.left); write_str(cg, ", ");
                gen_int_expr(cg, e->base.u.binop.right); write_str(cg, ")");
                return;
            }
            break;
        }
        case AST_IFEXP:
            write_str(cg, "(p2c_obj_is_truthy("); gen_expr(cg, e->base.u.ifexp.test); write_str(cg, ") ? ");
            gen_int_expr(cg, e->base.u.ifexp.body); write_str(cg, " : ");
            gen_int_expr(cg, e->base.u.ifexp.orelse); write_str(cg, ")");
            return;
        default:
            break;
    }
    /* 解析で int と確定しているが生で書けない形（呼び出し・添字など）:
     * 従来の式を出して as_int で取り出す（値は int なので安全）。 */
    write_str(cg, "p2c_obj_as_int(");
    gen_expr(cg, e);
    write_str(cg, ")");
}


/* ── 解析（対象にできる式かどうか / 束縛名の収集）──────────────────────
 * 入れ子スコープ（関数・クラス・内包表記・ラムダ）と、追加の束縛を作る式
 * （walrus）や制御移動（yield/await）を含む関数は対象外にする（保守的）。 */
static bool native_expr_ok(P2C_CodeGen *cg, P2C_AstExpr *e) {
    if (!e) return true;
    switch (e->base.type) {
        case AST_LAMBDA:
        case AST_COMPREHENSION:
        case AST_GENERATOR_EXPRESSION:
        case AST_YIELD:
        case AST_AWAIT:
        case AST_NAMED_EXPR:
            return false;
        case AST_BINOP:
            return native_expr_ok(cg, e->base.u.binop.left) && native_expr_ok(cg, e->base.u.binop.right);
        case AST_UNARYOP:
            return native_expr_ok(cg, e->base.u.unaryop.operand);
        case AST_IFEXP:
            return native_expr_ok(cg, e->base.u.ifexp.test) &&
                   native_expr_ok(cg, e->base.u.ifexp.body) &&
                   native_expr_ok(cg, e->base.u.ifexp.orelse);
        case AST_BOOLOP: {
            for (size_t i = 0; i < p2c_vec_len(e->base.u.boolop.values); i++)
                if (!native_expr_ok(cg, (P2C_AstExpr*)p2c_vec_get(e->base.u.boolop.values, i))) return false;
            return true;
        }
        case AST_COMPARE: {
            if (!native_expr_ok(cg, e->base.u.compare.left)) return false;
            for (size_t i = 0; i < p2c_vec_len(e->base.u.compare.comparators); i++)
                if (!native_expr_ok(cg, (P2C_AstExpr*)p2c_vec_get(e->base.u.compare.comparators, i))) return false;
            return true;
        }
        case AST_CALL: {
            if (!native_expr_ok(cg, e->base.u.call.func)) return false;
            for (size_t i = 0; i < p2c_vec_len(e->base.u.call.args); i++)
                if (!native_expr_ok(cg, (P2C_AstExpr*)p2c_vec_get(e->base.u.call.args, i))) return false;
            for (size_t i = 0; i < p2c_vec_len(e->base.u.call.keywords); i++) {
                P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(e->base.u.call.keywords, i);
                if (kw && !native_expr_ok(cg, kw->value)) return false;
            }
            return true;
        }
        case AST_ATTRIBUTE:
            return native_expr_ok(cg, e->base.u.attribute.value);
        case AST_SUBSCRIPT:
            return native_expr_ok(cg, e->base.u.subscript.value) && native_expr_ok(cg, e->base.u.subscript.slice);
        case AST_LIST:
        case AST_TUPLE: {
            P2C_Vector *elts = e->base.type == AST_TUPLE ? e->base.u.tuple.elts : e->base.u.list.elts;
            for (size_t i = 0; i < p2c_vec_len(elts); i++)
                if (!native_expr_ok(cg, (P2C_AstExpr*)p2c_vec_get(elts, i))) return false;
            return true;
        }
        case AST_DICT: {
            for (size_t i = 0; i < p2c_vec_len(e->base.u.dict.values); i++)
                if (!native_expr_ok(cg, (P2C_AstExpr*)p2c_vec_get(e->base.u.dict.values, i))) return false;
            for (size_t i = 0; i < p2c_vec_len(e->base.u.dict.keys); i++)
                if (!native_expr_ok(cg, (P2C_AstExpr*)p2c_vec_get(e->base.u.dict.keys, i))) return false;
            return true;
        }
        case AST_STARRED:
            return native_expr_ok(cg, e->base.u.starred.value);
        case AST_CONST:
        case AST_NAME:
            return true;
        default:
            return false;   /* 未知の式・set などは対象外（保守的） */
    }
}

/* 代入ターゲットが束縛する名前を「int 以外が入りうる」として記録する。 */
static void mark_target_names_bad(P2C_Map *bad, P2C_AstExpr *target) {
    if (!bad || !target) return;
    if (target->base.type == AST_NAME) {
        map_set_name(bad, target->base.u.name.name);
    } else if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.type == AST_TUPLE ? target->base.u.tuple.elts : target->base.u.list.elts;
        for (size_t i = 0; i < p2c_vec_len(elts); i++)
            mark_target_names_bad(bad, (P2C_AstExpr*)p2c_vec_get(elts, i));
    } else if (target->base.type == AST_STARRED) {
        mark_target_names_bad(bad, target->base.u.starred.value);
    }
    /* 属性・添字・呼び出しは名前を束縛しない */
}

/* import のエイリアスが束縛する名前。 */
static void mark_alias_bad(P2C_Map *bad, P2C_AstAlias *alias) {
    if (!bad || !alias || !alias->name) return;
    if (alias->asname && *alias->asname) {
        map_set_name(bad, alias->asname);
        return;
    }
    /* `import a.b` は a を束縛する。モジュール名の先頭要素を使う。 */
    const char *dot = strchr(alias->name, '.');
    size_t len = dot ? (size_t)(dot - alias->name) : strlen(alias->name);
    char tmp[128];
    if (len < sizeof(tmp)) {
        memcpy(tmp, alias->name, len);
        tmp[len] = '\0';
        map_set_name(bad, tmp);
    }
}

/* 文を走査して「int と確定した代入先」(int_ok) と「int 以外が入りうる名前」(bad)
 * を集める。対象外の構文を見つけたら false（その関数は unboxing しない）。 */
static bool native_collect_stmts(P2C_CodeGen *cg, P2C_Vector *stmts, P2C_Map *int_ok, P2C_Map *bad) {
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *st = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!st) continue;
        P2C_AstNode *n = &st->base;
        switch (n->type) {
            case AST_ASSIGN: {
                size_t ntargets = p2c_vec_len(n->u.assign.targets);
                for (size_t t = 0; t < ntargets; t++) {
                    P2C_AstExpr *tg = (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, t);
                    if (!tg) continue;
                    /* 連鎖代入 (a = b = 5) は一時オブジェクト経由で代入されるため対象外。 */
                    if (ntargets == 1 && tg->base.type == AST_NAME) {
                        if (expr_is_int_typed_with(cg, n->u.assign.value, int_ok, bad))
                            map_set_name(int_ok, tg->base.u.name.name);
                        else
                            map_set_name(bad, tg->base.u.name.name);
                    } else {
                        mark_target_names_bad(bad, tg);
                    }
                }
                if (!native_expr_ok(cg, n->u.assign.value)) return false;
                break;
            }
            case AST_AUGASSIGN: {
                P2C_AstExpr *tg = n->u.augassign.target;
                if (tg && tg->base.type == AST_NAME && raw_binop_func(n->u.augassign.op) &&
                    expr_is_int_typed_with(cg, n->u.augassign.value, int_ok, bad)) {
                    map_set_name(int_ok, tg->base.u.name.name);
                } else {
                    mark_target_names_bad(bad, tg);
                }
                if (!native_expr_ok(cg, n->u.augassign.value)) return false;
                break;
            }
            case AST_ANNASSIGN:
                mark_target_names_bad(bad, n->u.annassign.target);
                if (!native_expr_ok(cg, n->u.annassign.value)) return false;
                if (!native_expr_ok(cg, n->u.annassign.annotation)) return false;
                break;
            case AST_RETURN:
                if (!native_expr_ok(cg, n->u.return_stmt.value)) return false;
                break;
            case AST_EXPR_STMT:
                if (!native_expr_ok(cg, n->u.expr_stmt.value)) return false;
                break;
            case AST_BLOCK:
                if (!native_collect_stmts(cg, n->u.block.stmts, int_ok, bad)) return false;
                break;
            case AST_PASS:
            case AST_BREAK:
            case AST_CONTINUE:
                break;
            case AST_IF:
                if (!native_expr_ok(cg, n->u.if_stmt.test)) return false;
                if (!native_collect_stmts(cg, n->u.if_stmt.body, int_ok, bad)) return false;
                if (!native_collect_stmts(cg, n->u.if_stmt.orelse, int_ok, bad)) return false;
                break;
            case AST_WHILE:
                if (!native_expr_ok(cg, n->u.while_stmt.test)) return false;
                if (!native_collect_stmts(cg, n->u.while_stmt.body, int_ok, bad)) return false;
                if (!native_collect_stmts(cg, n->u.while_stmt.orelse, int_ok, bad)) return false;
                break;
            case AST_FOR:
                mark_target_names_bad(bad, n->u.for_stmt.target);
                if (!native_expr_ok(cg, n->u.for_stmt.iter)) return false;
                if (!native_collect_stmts(cg, n->u.for_stmt.body, int_ok, bad)) return false;
                if (!native_collect_stmts(cg, n->u.for_stmt.orelse, int_ok, bad)) return false;
                break;
            case AST_TRY:
                if (!native_collect_stmts(cg, n->u.try_stmt.body, int_ok, bad)) return false;
                if (!native_collect_stmts(cg, n->u.try_stmt.orelse, int_ok, bad)) return false;
                if (!native_collect_stmts(cg, n->u.try_stmt.finalbody, int_ok, bad)) return false;
                for (size_t h = 0; h < p2c_vec_len(n->u.try_stmt.handlers); h++) {
                    P2C_AstExceptHandler *hd = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, h);
                    if (!hd) continue;
                    if (hd->name) map_set_name(bad, hd->name);
                    if (!native_expr_ok(cg, hd->type)) return false;
                    if (!native_collect_stmts(cg, hd->body, int_ok, bad)) return false;
                }
                break;
            case AST_RAISE:
                if (!native_expr_ok(cg, n->u.raise.exc)) return false;
                if (!native_expr_ok(cg, n->u.raise.cause)) return false;
                break;
            case AST_ASSERT:
                if (!native_expr_ok(cg, n->u.assert_stmt.test)) return false;
                if (!native_expr_ok(cg, n->u.assert_stmt.msg)) return false;
                break;
            case AST_WITH:
                for (size_t w = 0; w < p2c_vec_len(n->u.with.items); w++) {
                    P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, w);
                    if (!item) continue;
                    mark_target_names_bad(bad, item->optional_vars);   /* with ... as x */
                    if (!native_expr_ok(cg, item->context_expr)) return false;
                }
                if (!native_collect_stmts(cg, n->u.with.body, int_ok, bad)) return false;
                break;
            case AST_DELETE:
                for (size_t d = 0; d < p2c_vec_len(n->u.delete.targets); d++)
                    mark_target_names_bad(bad, (P2C_AstExpr*)p2c_vec_get(n->u.delete.targets, d));
                break;
            case AST_GLOBAL:
            case AST_NONLOCAL: {
                P2C_Vector *names = n->type == AST_GLOBAL ? n->u.global.names : n->u.nonlocal_stmt.names;
                for (size_t g = 0; g < p2c_vec_len(names); g++)
                    map_set_name(bad, (const char*)p2c_vec_get(names, g));
                break;
            }
            case AST_IMPORT:
                for (size_t a = 0; a < p2c_vec_len(n->u.import_stmt.names); a++)
                    mark_alias_bad(bad, (P2C_AstAlias*)p2c_vec_get(n->u.import_stmt.names, a));
                break;
            case AST_IMPORTFROM:
                for (size_t a = 0; a < p2c_vec_len(n->u.importfrom.names); a++)
                    mark_alias_bad(bad, (P2C_AstAlias*)p2c_vec_get(n->u.importfrom.names, a));
                break;
            default:
                /* 入れ子スコープ（def/class）・match・async for などは対象外。 */
                return false;
        }
    }
    return true;
}

/* 関数本体を解析して cg->native_int_locals を設定する（対象外なら空のまま）。
 * 代入の型判定は候補集合に依存するため、収束するまで数回繰り返す
 * （例: `x = y` より後で `y = 1` と書かれていても正しく判定する）。 */
static void analyze_native_int_locals(P2C_CodeGen *cg, P2C_Vector *stmts, P2C_Vector *args) {
    if (cg->native_int_locals) { free_name_map(cg->native_int_locals); cg->native_int_locals = NULL; }
    if (!cg->opts.unbox_int_locals || !stmts) return;
    P2C_Map *cand = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!cand) return;
    for (int round = 0; round < 4; round++) {
        P2C_Map *next = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
        P2C_Map *bad = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
        if (!next || !bad) { if (next) free_name_map(next); if (bad) free_name_map(bad); break; }
        if (!native_collect_stmts(cg, stmts, next, bad)) {
            free_name_map(next); free_name_map(bad); free_name_map(cand);
            return;   /* 対象外の構文があった */
        }
        /* int 以外が入りうる名前を候補から除く。 */
        for (size_t b = 0; b < bad->bucket_count; b++)
            for (P2C_MapEntry *e = bad->buckets[b]; e; e = e->next)
                if (e->key) p2c_map_remove(next, e->key);
        bool same = (p2c_map_len(next) == p2c_map_len(cand));
        if (same) {
            for (size_t b = 0; b < next->bucket_count && same; b++)
                for (P2C_MapEntry *e = next->buckets[b]; e; e = e->next)
                    if (e->key && !map_has_name(cand, (const char*)e->key)) { same = false; break; }
        }
        free_name_map(cand);
        cand = next;
        free_name_map(bad);
        if (same) break;
    }
    /* 引数・セル・nonlocal・グローバル・クラス名・合成ローカルは対象外に落とす。 */
    for (size_t a = 0; a < (args ? p2c_vec_len(args) : 0); a++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(args, a);
        if (arg && arg->name) p2c_map_remove(cand, arg->name);
    }
    P2C_Map *final_map = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (final_map) {
        for (size_t b = 0; b < cand->bucket_count; b++) {
            for (P2C_MapEntry *e = cand->buckets[b]; e; e = e->next) {
                const char *name = e->key ? (const char*)e->key : NULL;
                if (!name) continue;
                if (map_has_name(cg->module_globals, name)) continue;
                if (map_has_name(cg->cell_names, name)) continue;
                if (map_has_name(cg->nonlocal_names, name)) continue;
                if (map_has_name(cg->known_classes, name)) continue;
                if (map_has_name(cg->closure_env_names, name)) continue;
                if (is_synthetic_local(cg, name)) continue;
                map_set_name(final_map, name);
            }
        }
    }
    free_name_map(cand);
    cg->native_int_locals = final_map;
}

/* AOT アンボクシング: 関数本体の解析（宣言より前）と、生成後の復帰をひと組にする。
 * 入れ子スコープの生成で集合が入れ替わるため、外側の集合を戻り値で受け渡す。 */
static P2C_Map *native_begin_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    P2C_Map *saved = cg->native_int_locals;
    cg->native_int_locals = NULL;
    analyze_native_int_locals(cg, fd->body, fd->args);
    return saved;
}
static void native_end_function(P2C_CodeGen *cg, P2C_Map *saved) {
    free_name_map(cg->native_int_locals);
    cg->native_int_locals = saved;
}

/* モジュールのトップレベル文（関数/クラス定義の中は除く）を走査し、
 * 単純名への代入・複合代入・forループ変数を module_globals に登録する。
 * これらは実際のC言語のファイルスコープ変数として宣言され、関数内から
 * `global x` で参照・再代入できるようにする。 */
static void collect_module_level_name(P2C_CodeGen *cg, const char *name) {
    map_set_name(cg->module_globals, name);
}
static void collect_module_level_name_cb(P2C_CodeGen *cg, const char *name) { collect_module_level_name(cg, name); }
/* 関数・クラスの本体（入れ子を含む）にある `global x` の名前を module_globals に登録する。 */
static void collect_global_decl_names(P2C_CodeGen *cg, P2C_Vector *stmts) {
    if (!stmts) return;
    for (size_t i = 0; i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt) continue;
        P2C_AstNode *n = &stmt->base;
        switch (n->type) {
            case AST_GLOBAL:
                for (size_t k = 0; n->u.global.names && k < p2c_vec_len(n->u.global.names); k++) {
                    const char *nm = (const char*)p2c_vec_get(n->u.global.names, k);
                    if (nm) collect_module_level_name(cg, nm);
                }
                break;
            case AST_FUNCTIONDEF: collect_global_decl_names(cg, n->u.functiondef.body); break;
            case AST_CLASSDEF: collect_global_decl_names(cg, n->u.classdef.body); break;
            case AST_IF: collect_global_decl_names(cg, n->u.if_stmt.body); collect_global_decl_names(cg, n->u.if_stmt.orelse); break;
            case AST_WHILE: collect_global_decl_names(cg, n->u.while_stmt.body); break;
            case AST_FOR: case AST_ASYNC_FOR: collect_global_decl_names(cg, n->u.for_stmt.body); break;
            case AST_WITH: collect_global_decl_names(cg, n->u.with.body); break;
            case AST_TRY:
                collect_global_decl_names(cg, n->u.try_stmt.body);
                collect_global_decl_names(cg, n->u.try_stmt.finalbody);
                for (size_t j = 0; j < p2c_vec_len(n->u.try_stmt.handlers); j++) {
                    P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, j);
                    if (h) collect_global_decl_names(cg, h->body);
                }
                break;
            default: break;
        }
    }
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
                    for_each_target_name(t, collect_module_level_name_cb, cg);
                }
                break;
            case AST_AUGASSIGN:
                if (n->u.augassign.target && n->u.augassign.target->base.type == AST_NAME) collect_module_level_name(cg, n->u.augassign.target->base.u.name.name);
                break;
            case AST_ANNASSIGN:
                /* x: int = 5 も単純名への代入として module 変数になる */
                if (n->u.annassign.target && n->u.annassign.target->base.type == AST_NAME)
                    collect_module_level_name(cg, n->u.annassign.target->base.u.name.name);
                break;
            case AST_FUNCTIONDEF:
                /* 関数内の `global x` で宣言された名前は、モジュール変数として存在させる
                 * （モジュール直下で代入されていなくても、関数が代入すれば C の大域変数が要る）。 */
                collect_global_decl_names(cg, n->u.functiondef.body);
                break;
            case AST_CLASSDEF:
                collect_global_decl_names(cg, n->u.classdef.body);
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
                if (n->u.for_stmt.target) for_each_target_name(n->u.for_stmt.target, collect_module_level_name_cb, cg);
                collect_module_globals(cg, n->u.for_stmt.body);
                collect_module_globals(cg, n->u.for_stmt.orelse);
                break;
            case AST_WITH:
                /* with ... as x: の x も module 変数（タプル代入も含む） */
                for (size_t j = 0; n->u.with.items && j < p2c_vec_len(n->u.with.items); j++) {
                    P2C_AstWithItem *wi = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, j);
                    if (wi && wi->optional_vars) for_each_target_name(wi->optional_vars, collect_module_level_name_cb, cg);
                }
                collect_module_globals(cg, n->u.with.body);
                break;
            case AST_WHILE:
                collect_module_globals(cg, n->u.while_stmt.body);
                collect_module_globals(cg, n->u.while_stmt.orelse);
                break;
            case AST_IF:
                collect_module_globals(cg, n->u.if_stmt.body);
                collect_module_globals(cg, n->u.if_stmt.orelse);
                break;
            case AST_TRY:
                collect_module_globals(cg, n->u.try_stmt.body);
                collect_module_globals(cg, n->u.try_stmt.finalbody);
                collect_module_globals(cg, n->u.try_stmt.orelse);
                for (size_t j = 0; j < p2c_vec_len(n->u.try_stmt.handlers); j++) {
                    P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, j);
                    if (!h) continue;
                    if (h->name) collect_module_level_name(cg, h->name);
                    collect_module_globals(cg, h->body);
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
                if (member->base.type == AST_FUNCTIONDEF) {
                    map_set_name(methods, member->base.u.functiondef.name);
                    /* クラス生成時のキーワード引数解決用に __init__ の定義を控える。 */
                    if (!cg->scan_class_cname && strcmp(member->base.u.functiondef.name, "__init__") == 0) {
                        map_set_func_args(cg->class_init_defs, n->u.classdef.name, &member->base.u.functiondef);
                    }
                }
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
            /* 関数本体内のネスト def はモジュール関数ではない。scan 中は
             * function_depth を進めて、内側の def を func_args /
             * module_function_names へ登録しない（登録すると、クロージャ変数の
             * 呼び出しをモジュール関数呼び出しと誤認し、既定値グローバルや
             * 直接C呼び出しを生成してしまう）。 */
            cg->function_depth++;
            scan_stmt_list(cg, n->u.functiondef.body);
            cg->function_depth--;
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
                for_each_target_name(t, declare_name_if_needed_cb, cg);
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
            else for_each_target_name(n->u.for_stmt.target, declare_name_if_needed_cb, cg);
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

/* 関数の引数・*args・**kwargs の C 名。宣言する場所（外側の関数の表が有効な場合を含む）に
 * よらず、モジュール変数と同名なら局所の名前へ変える。本体の参照（write_ident）も同じ結果になる。 */
static void write_param_ident(P2C_CodeGen *cg, const char *name) {
    if (cg->module_globals && map_has_name(cg->module_globals, name) &&
        !(cg->global_decl_names && map_has_name(cg->global_decl_names, name))) write_str(cg, "p2c_loc_");
    write_str(cg, mangle_ident(name));
}

static void write_ident(P2C_CodeGen *cg, const char *name) {
    /* 関数（ラムダ・メソッド・入れ子の def を含む）の局所が、モジュール変数と同じ名前のとき、
     * C の -Wshadow（局所が大域を隠す）に触れないように局所の C 名だけを変える。
     * 関数の中でも、局所でない名前（モジュール変数の参照）は変えない。 */
    if (cg->declared_vars && cg->declared_vars != cg->module_scope_declared &&
        cg->module_globals && map_has_name(cg->declared_vars, name) && map_has_name(cg->module_globals, name) &&
        !(cg->global_decl_names && map_has_name(cg->global_decl_names, name))) {
        write_str(cg, "p2c_loc_");
    }
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
    /* 内包表記の対象変数が、要素・条件の中のラムダから参照される場合、Python と同じく
     * 1つの変数（セル）を全反復で共有する。反復ごとの値のコピーにはしない。 */
    bool target_cell = false;
    char target_cell_name[256] = {0};
    if (gen->target && gen->target->base.type == AST_NAME && gen->target->base.u.name.name &&
        !(cg->cell_names && map_has_name(cg->cell_names, gen->target->base.u.name.name))) {
        P2C_Map *refs = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
        if (refs) {
            cref_expr(elt, false, refs);
            cref_expr(dict_key, false, refs);
            for (size_t gi = gen_idx; gi < p2c_vec_len(generators); gi++) {
                P2C_AstComprehensionGen *g2 = (P2C_AstComprehensionGen*)p2c_vec_get(generators, gi);
                if (!g2) continue;
                cref_expr_list(g2->ifs, false, refs);
                if (gi > gen_idx) cref_expr(g2->iter, false, refs);
            }
            target_cell = map_has_name(refs, gen->target->base.u.name.name);
            p2c_map_free(refs);
        }
        if (target_cell) {
            snprintf(target_cell_name, sizeof(target_cell_name), "%s", gen->target->base.u.name.name);
            if (!cg->cell_names) cg->cell_names = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
            if (cg->cell_names) {
                write_str(cg, "P2C_Object *_p2c_cell_"); write_ident(cg, target_cell_name);
                write_str(cg, " = p2c_cell_new(&P2C_None); ");
                map_set_name(cg->cell_names, target_cell_name);
            } else {
                target_cell = false;
            }
        }
    }
    char iter_var[64], n_var[64], i_var[64], item_var[64];
    snprintf(iter_var, sizeof(iter_var), "_p2c_citer_%d_%zu", comp_id, gen_idx);
    snprintf(n_var, sizeof(n_var), "_p2c_cn_%d_%zu", comp_id, gen_idx);
    snprintf(i_var, sizeof(i_var), "_p2c_ci_%d_%zu", comp_id, gen_idx);
    snprintf(item_var, sizeof(item_var), "_p2c_citem_%d_%zu", comp_id, gen_idx);

    write_str(cg, "{ P2C_Object *"); write_str(cg, iter_var); write_str(cg, " = p2c_iter_source("); gen_expr(cg, gen->iter); write_str(cg, "); ");
    write_str(cg, "size_t "); write_str(cg, n_var); write_str(cg, " = (size_t)p2c_len("); write_str(cg, iter_var); write_str(cg, "); ");
    write_str(cg, "for (size_t "); write_str(cg, i_var); write_str(cg, " = 0; "); write_str(cg, i_var); write_str(cg, " < "); write_str(cg, n_var); write_str(cg, "; "); write_str(cg, i_var); write_str(cg, "++) { ");
    write_str(cg, "P2C_Object *"); write_str(cg, item_var); write_str(cg, " = p2c_iter_at("); write_str(cg, iter_var); write_str(cg, ", (int64_t)"); write_str(cg, i_var); write_str(cg, "); ");

    /* 内包表記ターゲットはこの内包表記の中だけの名前。declared_vars を汚すと
     * 外側の式（genexp の capture 等）へ漏れるため、専用マップでスコープを切る。 */
    P2C_Map *saved_targets = cg->comprehension_targets;
    P2C_Map *scoped_targets = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (scoped_targets) {
        if (saved_targets) {
            for (size_t bi = 0; bi < saved_targets->bucket_count; bi++)
                for (P2C_MapEntry *be = saved_targets->buckets[bi]; be; be = be->next)
                    map_set_name(scoped_targets, (const char*)be->key);
        }
        cg->comprehension_targets = scoped_targets;
    }
    if (gen->target->base.type == AST_NAME) {
        if (target_cell) {
            write_str(cg, "p2c_cell_set(_p2c_cell_"); write_ident(cg, gen->target->base.u.name.name);
            write_str(cg, ", "); write_str(cg, item_var); write_str(cg, "); ");
        } else {
            write_str(cg, "P2C_Object *"); write_ident(cg, gen->target->base.u.name.name); write_str(cg, " = "); write_str(cg, item_var); write_str(cg, "; ");
        }
        map_set_name(cg->comprehension_targets, gen->target->base.u.name.name);
    } else if (gen->target->base.type == AST_TUPLE) {
        P2C_Vector *elts = gen->target->base.u.tuple.elts;
        for (size_t k = 0; k < p2c_vec_len(elts); k++) {
            P2C_AstExpr *te = (P2C_AstExpr*)p2c_vec_get(elts, k);
            if (te->base.type != AST_NAME) continue;
            write_str(cg, "P2C_Object *"); write_ident(cg, te->base.u.name.name); write_str(cg, " = p2c_subscript_get(");
            write_str(cg, item_var); write_str(cg, ", p2c_obj_from_int("); emit_usize(cg, k); write_str(cg, ")); ");
            map_set_name(cg->comprehension_targets, te->base.u.name.name);
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
    /* この内包表記のセルは、ここで終わる（以降の同名の変数と混ざらないように外す） */
    if (target_cell && cg->cell_names) p2c_map_remove(cg->cell_names, target_cell_name);
    if (scoped_targets) {
        cg->comprehension_targets = saved_targets;
        free_name_map(scoped_targets);
    }
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
    codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, line, n ? n->col : 0u, what,
                         "rerun with --fallback to replace it with a runtime stub");
    if (as_expr) {
        write_str(cg, "&P2C_None");
    } else {
        indent(cg);
        write_str(cg, "/* unsupported statement */");
        write_newline(cg);
    }
}

/* ── 入れ子の関数・ラムダから参照される局所の検出（遅延束縛のためのセル化） ──────
 * Python のクロージャは変数そのものを捕捉する（後から代入された値が見える）。
 * 値を作成時にコピーすると、ループ変数や後の再代入が反映されない。そこで、入れ子の
 * 関数・ラムダの本体で参照される外側の局所は、共有セル（p2c_cell）に置く。
 * 本体の中の名前を集める（この式・文の中で入れ子の関数・ラムダの内側にある名前）。
 * in_closure は「現在の位置が入れ子の関数・ラムダの内側か」。 */
static void cref_stmts(P2C_Vector *stmts, bool ic, P2C_Map *out);

static void cref_expr_list(P2C_Vector *v, bool ic, P2C_Map *out) {
    for (size_t i = 0; v && i < p2c_vec_len(v); i++) cref_expr((P2C_AstExpr*)p2c_vec_get(v, i), ic, out);
}

static void cref_pattern(P2C_AstMatchPattern *p, bool ic, P2C_Map *out) {
    if (!p) return;
    cref_expr(p->value, ic, out);
    if (p->capture_name && ic) map_set_name(out, p->capture_name);
    if (p->rest_name && ic) map_set_name(out, p->rest_name);
    cref_expr_list(p->keys, ic, out);
    for (size_t i = 0; p->children && i < p2c_vec_len(p->children); i++)
        cref_pattern((P2C_AstMatchPattern*)p2c_vec_get(p->children, i), ic, out);
}

static void cref_expr(P2C_AstExpr *e, bool ic, P2C_Map *out) {
    if (!e) return;
    P2C_AstNode *n = &e->base;
    switch (n->type) {
        case AST_NAME:
            if (ic && n->u.name.name) map_set_name(out, n->u.name.name);
            break;
        case AST_BINOP: cref_expr(n->u.binop.left, ic, out); cref_expr(n->u.binop.right, ic, out); break;
        case AST_UNARYOP: cref_expr(n->u.unaryop.operand, ic, out); break;
        case AST_COMPARE: cref_expr(n->u.compare.left, ic, out); cref_expr_list(n->u.compare.comparators, ic, out); break;
        case AST_BOOLOP: cref_expr_list(n->u.boolop.values, ic, out); break;
        case AST_CALL:
            cref_expr(n->u.call.func, ic, out);
            cref_expr_list(n->u.call.args, ic, out);
            for (size_t i = 0; n->u.call.keywords && i < p2c_vec_len(n->u.call.keywords); i++) {
                P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, i);
                if (kw) cref_expr(kw->value, ic, out);
            }
            break;
        case AST_ATTRIBUTE: cref_expr(n->u.attribute.value, ic, out); break;
        case AST_SUBSCRIPT: cref_expr(n->u.subscript.value, ic, out); cref_expr(n->u.subscript.slice, ic, out); break;
        case AST_IFEXP:
            cref_expr(n->u.ifexp.test, ic, out); cref_expr(n->u.ifexp.body, ic, out); cref_expr(n->u.ifexp.orelse, ic, out);
            break;
        case AST_LAMBDA:
            /* 既定値は外側で評価される。本体は入れ子の内側。 */
            for (size_t i = 0; n->u.lambda.args && i < p2c_vec_len(n->u.lambda.args); i++) {
                P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(n->u.lambda.args, i);
                if (a) cref_expr(a->default_val, ic, out);
            }
            cref_expr(n->u.lambda.body, true, out);
            break;
        case AST_LIST: case AST_TUPLE: case AST_SET:
            cref_expr_list(n->u.list.elts, ic, out);
            break;
        case AST_DICT:
            cref_expr_list(n->u.dict.keys, ic, out);
            cref_expr_list(n->u.dict.values, ic, out);
            break;
        case AST_COMPREHENSION:
            /* 内包表記はこの関数の中で展開される（入れ子の関数ではない）。 */
            cref_expr(n->u.comprehension.elt, ic, out);
            cref_expr(n->u.comprehension.dict_key, ic, out);
            for (size_t i = 0; n->u.comprehension.generators && i < p2c_vec_len(n->u.comprehension.generators); i++) {
                P2C_AstComprehensionGen *g = (P2C_AstComprehensionGen*)p2c_vec_get(n->u.comprehension.generators, i);
                if (!g) continue;
                cref_expr(g->target, ic, out);
                cref_expr(g->iter, ic, out);
                cref_expr_list(g->ifs, ic, out);
            }
            break;
        case AST_GENERATOR_EXPRESSION:
            /* ジェネレータ式は内側で評価される（外側の変数を捕捉する）。 */
            cref_expr(n->u.comprehension.elt, true, out);
            cref_expr(n->u.comprehension.dict_key, true, out);
            for (size_t i = 0; n->u.comprehension.generators && i < p2c_vec_len(n->u.comprehension.generators); i++) {
                P2C_AstComprehensionGen *g = (P2C_AstComprehensionGen*)p2c_vec_get(n->u.comprehension.generators, i);
                if (!g) continue;
                cref_expr(g->target, true, out);
                cref_expr(g->iter, true, out);
                cref_expr_list(g->ifs, true, out);
            }
            break;
        case AST_STARRED: cref_expr(n->u.starred.value, ic, out); break;
        case AST_NAMED_EXPR: cref_expr(n->u.named_expr.target, ic, out); cref_expr(n->u.named_expr.value, ic, out); break;
        case AST_YIELD: cref_expr(n->u.yield_expr.value, ic, out); break;
        case AST_AWAIT: cref_expr(n->u.await_expr.value, ic, out); break;
        default:
            break;
    }
}

static void cref_stmts(P2C_Vector *stmts, bool ic, P2C_Map *out) {
    for (size_t i = 0; stmts && i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *s = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!s) continue;
        P2C_AstNode *n = &s->base;
        switch (n->type) {
            case AST_ASSIGN: cref_expr_list(n->u.assign.targets, ic, out); cref_expr(n->u.assign.value, ic, out); break;
            case AST_AUGASSIGN: cref_expr(n->u.augassign.target, ic, out); cref_expr(n->u.augassign.value, ic, out); break;
            case AST_ANNASSIGN: cref_expr(n->u.annassign.target, ic, out); cref_expr(n->u.annassign.value, ic, out); break;
            case AST_RETURN: cref_expr(n->u.return_stmt.value, ic, out); break;
            case AST_EXPR_STMT: cref_expr(n->u.expr_stmt.value, ic, out); break;
            case AST_BLOCK: cref_stmts(n->u.block.stmts, ic, out); break;
            case AST_IF:
                cref_expr(n->u.if_stmt.test, ic, out); cref_stmts(n->u.if_stmt.body, ic, out); cref_stmts(n->u.if_stmt.orelse, ic, out);
                break;
            case AST_WHILE:
                cref_expr(n->u.while_stmt.test, ic, out); cref_stmts(n->u.while_stmt.body, ic, out); cref_stmts(n->u.while_stmt.orelse, ic, out);
                break;
            case AST_FOR: case AST_ASYNC_FOR:
                cref_expr(n->u.for_stmt.target, ic, out); cref_expr(n->u.for_stmt.iter, ic, out);
                cref_stmts(n->u.for_stmt.body, ic, out); cref_stmts(n->u.for_stmt.orelse, ic, out);
                break;
            case AST_TRY:
                cref_stmts(n->u.try_stmt.body, ic, out);
                for (size_t h = 0; n->u.try_stmt.handlers && h < p2c_vec_len(n->u.try_stmt.handlers); h++) {
                    P2C_AstExceptHandler *eh = (P2C_AstExceptHandler*)p2c_vec_get(n->u.try_stmt.handlers, h);
                    if (!eh) continue;
                    cref_expr(eh->type, ic, out);
                    if (eh->name && ic) map_set_name(out, eh->name);
                    cref_stmts(eh->body, ic, out);
                }
                cref_stmts(n->u.try_stmt.orelse, ic, out);
                cref_stmts(n->u.try_stmt.finalbody, ic, out);
                break;
            case AST_RAISE: cref_expr(n->u.raise.exc, ic, out); cref_expr(n->u.raise.cause, ic, out); break;
            case AST_ASSERT: cref_expr(n->u.assert_stmt.test, ic, out); cref_expr(n->u.assert_stmt.msg, ic, out); break;
            case AST_MATCH:
                cref_expr(n->u.match_stmt.subject, ic, out);
                for (size_t c = 0; n->u.match_stmt.cases && c < p2c_vec_len(n->u.match_stmt.cases); c++) {
                    P2C_AstMatchCase *mc = (P2C_AstMatchCase*)p2c_vec_get(n->u.match_stmt.cases, c);
                    if (!mc) continue;
                    cref_pattern(mc->pattern, ic, out);
                    cref_expr(mc->guard, ic, out);
                    cref_stmts(mc->body, ic, out);
                }
                break;
            case AST_FUNCTIONDEF:
                cref_expr_list(n->u.functiondef.decorator_list, ic, out);
                for (size_t a = 0; n->u.functiondef.args && a < p2c_vec_len(n->u.functiondef.args); a++) {
                    P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(n->u.functiondef.args, a);
                    if (arg) cref_expr(arg->default_val, ic, out);
                }
                cref_stmts(n->u.functiondef.body, true, out);
                break;
            case AST_CLASSDEF:
                cref_expr_list(n->u.classdef.bases, ic, out);
                for (size_t k = 0; n->u.classdef.keywords && k < p2c_vec_len(n->u.classdef.keywords); k++) {
                    P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.classdef.keywords, k);
                    if (kw) cref_expr(kw->value, ic, out);
                }
                cref_expr_list(n->u.classdef.decorator_list, ic, out);
                cref_stmts(n->u.classdef.body, ic, out);
                break;
            case AST_GLOBAL: case AST_NONLOCAL:
                for (size_t k = 0; ic && n->u.global.names && k < p2c_vec_len(n->u.global.names); k++) {
                    const char *nm = (const char*)p2c_vec_get(n->u.global.names, k);
                    if (nm) map_set_name(out, nm);
                }
                break;
            case AST_WITH:
                for (size_t k = 0; n->u.with.items && k < p2c_vec_len(n->u.with.items); k++) {
                    P2C_AstWithItem *wi = (P2C_AstWithItem*)p2c_vec_get(n->u.with.items, k);
                    if (!wi) continue;
                    cref_expr(wi->context_expr, ic, out);
                    cref_expr(wi->optional_vars, ic, out);
                }
                cref_stmts(n->u.with.body, ic, out);
                break;
            case AST_DELETE: cref_expr_list(n->u.delete.targets, ic, out); break;
            case AST_MODULE: cref_stmts(n->u.module.body, ic, out); break;
            default:
                break;
        }
    }
}

/* 関数本体（入れ子の関数・ラムダの内側）で参照される名前の集合を返す（呼び出し側で解放） */
static P2C_Map *collect_closure_refs(P2C_CodeGen *cg, P2C_Vector *body) {
    P2C_Map *out = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (out) cref_stmts(body, false, out);
    return out;
}

/* 参照された名前のうち、この関数の局所（C変数として宣言されたもの）をセルへ加える。
 * 局所でない名前（組込み・モジュール変数）は対象外（それらは元々生きた値として読まれる）。 */
static void add_closure_cells(P2C_CodeGen *cg, P2C_Map *refs) {
    if (!refs) return;
    for (size_t i = 0; i < refs->bucket_count; i++) {
        for (P2C_MapEntry *e = refs->buckets[i]; e; e = e->next) {
            const char *name = (const char*)e->key;
            /* 関数の局所（モジュール変数と同名でも、関数の中では局所）だけをセルにする */
            if (!name || !map_has_name(cg->declared_vars, name)) continue;
            map_set_name(cg->cell_names, name);
        }
    }
}

/* ── 評価順の固定 ──────────────────────────────────────────────────────────
 * C11 では、関数引数・初期化子リストの評価順は不定である（§6.5.2.2p10, §6.7.9p23）。
 * そのため f(a(), b()) や辞書 {k(): v()} を C の引数・複合リテラルへそのまま写すと、
 * コンパイラの選択で後ろの式が先に評価されうる（GCC は多くの場合 右から左）。
 * 副作用を持つ部分式が2つ以上あるとき（星引数の値が副作用を持つ場合を含む）は、
 * GNU 文式 ({ ... }) の中で Python の順（左から右）に一時変数へ評価し、元の式は
 * その一時変数を参照して出す。置き換えは「元の AST ノード → 変数名」の対応表で行い、
 * AST 自体は書き換えない。これにより、星引数の式が展開のたびに再評価される問題も
 * 同時に防ぐ。strict C11 モードでは文式が使えないため、診断を出して拒否する。 */
typedef struct P2C_OrdFrame {
    const P2C_AstExpr *node;
    char name[48];
    struct P2C_OrdFrame *next;
} P2C_OrdFrame;

static void gen_expr_core(P2C_CodeGen *cg, P2C_AstExpr *expr);

static const char *ord_lookup(P2C_CodeGen *cg, const P2C_AstExpr *e) {
    for (P2C_OrdFrame *f = (P2C_OrdFrame*)cg->ord_frames; f; f = f->next) {
        if (f->node == e) return f->name;
    }
    return NULL;
}

/* 副作用が無く、評価順が結果に影響しない式か。
 * 名前は「値を持つ変数」（ローカル・モジュール変数）だけを順序の対象にする。組込み関数・型・
 * クラス・関数の名前は値ではなく静的な参照として扱われる（isinstance(x, frozenset) の型引数など）。 */
static bool ord_is_pure(P2C_CodeGen *cg, const P2C_AstExpr *e) {
    if (!e || e->base.type == AST_CONST) return true;
    /* 要素がすべて副作用の無い式のリスト・タプル・集合（例: isinstance(x, (list, set))）は純粋。
     * 一時変数へ分けると、組込み名を値として書くことになり、特別扱いの形が壊れるため。 */
    if (e->base.type == AST_TUPLE || e->base.type == AST_LIST || e->base.type == AST_SET) {
        P2C_Vector *elts = e->base.u.list.elts;
        for (size_t i = 0; elts && i < p2c_vec_len(elts); i++) {
            if (!ord_is_pure(cg, (const P2C_AstExpr*)p2c_vec_get(elts, i))) return false;
        }
        return true;
    }
    if (e->base.type != AST_NAME || !e->base.u.name.name) return e->base.type == AST_NAME;
    const char *n = e->base.u.name.name;
    if (cg->declared_vars && map_has_name(cg->declared_vars, n)) return false;
    if (cg->module_globals && map_has_name(cg->module_globals, n)) return false;
    return true;
}

/* 評価順が問題になる部分式を、Python の評価順で items へ集める。戻り値は個数。
 * starred_effect は、星引数の値が副作用を持つ（展開で再評価される）ときに true。 */
static size_t ord_collect(P2C_CodeGen *cg, P2C_AstExpr *e, P2C_AstExpr **items, bool *starred_effect) {
    size_t n = 0;
    switch (e->base.type) {
        case AST_CALL: {
            P2C_AstExpr *func = e->base.u.call.func;
            /* レシーバ（obj.m(...) の obj）は対象にしない。super().m(...) や型名への属性呼び出し
             * などは、レシーバを式として評価せず専用の形で出すため、一時変数へ分けると壊れる。 */
            if (func && func->base.type != AST_NAME && func->base.type != AST_ATTRIBUTE) {
                items[n++] = func;
            }
            P2C_Vector *args = e->base.u.call.args;
            for (size_t i = 0; args && i < p2c_vec_len(args); i++) {
                P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(args, i);
                if (!a) continue;
                if (a->base.type == AST_STARRED) {
                    P2C_AstExpr *v = a->base.u.starred.value;
                    if (v) {
                        items[n++] = v;
                        if (!ord_is_pure(cg, v)) *starred_effect = true;
                    }
                } else {
                    items[n++] = a;
                }
            }
            P2C_Vector *kws = e->base.u.call.keywords;
            for (size_t i = 0; kws && i < p2c_vec_len(kws); i++) {
                const P2C_AstKeyword *kw = (const P2C_AstKeyword*)p2c_vec_get(kws, i);
                if (kw && kw->value) items[n++] = kw->value;
            }
            return n;
        }
        case AST_DICT: {
            P2C_Vector *keys = e->base.u.dict.keys;
            P2C_Vector *vals = e->base.u.dict.values;
            size_t count = vals ? p2c_vec_len(vals) : 0;
            for (size_t i = 0; i < count; i++) {
                P2C_AstExpr *k = keys && i < p2c_vec_len(keys) ? (P2C_AstExpr*)p2c_vec_get(keys, i) : NULL;
                if (k) items[n++] = k;      /* 辞書は キー → 値 の順に評価する */
                P2C_AstExpr *v = (P2C_AstExpr*)p2c_vec_get(vals, i);
                if (v) items[n++] = v;
            }
            return n;
        }
        case AST_LIST:
        case AST_TUPLE:
        case AST_SET: {
            P2C_Vector *elts = e->base.u.list.elts;
            for (size_t i = 0; elts && i < p2c_vec_len(elts); i++) {
                P2C_AstExpr *x = (P2C_AstExpr*)p2c_vec_get(elts, i);
                if (!x) continue;
                if (x->base.type == AST_STARRED) {
                    P2C_AstExpr *v = x->base.u.starred.value;
                    if (v) {
                        items[n++] = v;
                        if (!ord_is_pure(cg, v)) *starred_effect = true;
                    }
                } else {
                    items[n++] = x;
                }
            }
            return n;
        }
        default:
            return 0;
    }
}

/* ord_collect が書き込む候補数の上限（要素の数から求める。候補配列の確保サイズに使う） */
static size_t ord_element_count(const P2C_AstExpr *e) {
    size_t n = 0;
    switch (e->base.type) {
        case AST_CALL:
            n += e->base.u.call.args ? p2c_vec_len(e->base.u.call.args) : 0;
            n += e->base.u.call.keywords ? p2c_vec_len(e->base.u.call.keywords) : 0;
            return n;
        case AST_DICT:
            return e->base.u.dict.values ? p2c_vec_len(e->base.u.dict.values) : 0;
        case AST_LIST:
        case AST_TUPLE:
        case AST_SET:
            return e->base.u.list.elts ? p2c_vec_len(e->base.u.list.elts) : 0;
        default:
            return 0;
    }
}

/* 評価順を固定して e を出す（必要な場合だけ呼ぶ）。 */
static void gen_expr_ordered(P2C_CodeGen *cg, P2C_AstExpr *e, P2C_AstExpr **items, size_t count) {
    if (cg->opts.strict_c11) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
            "a call, dict or list with more than one side-effecting element needs a statement "
            "expression to keep Python's evaluation order; it is not available in strict ISO C11 "
            "mode (--c11). Assign the parts to variables first.");
        gen_expr_core(cg, e);
        return;
    }
    P2C_OrdFrame *saved = (P2C_OrdFrame*)cg->ord_frames;
    write_str(cg, "({ ");
    for (size_t i = 0; i < count; i++) {
        P2C_AstExpr *it = items[i];
        if (!it || ord_is_pure(cg, it)) continue; /* 副作用の無い式（定数・組込みの名前など）は直接書く */
        P2C_OrdFrame *f = (P2C_OrdFrame*)p2c_alloc(cg->alloc, sizeof(P2C_OrdFrame));
        if (!f) break;
        snprintf(f->name, sizeof(f->name), "_p2c_seq_%d", ++cg->temp_counter);
        write_str(cg, "P2C_Object *"); write_str(cg, f->name); write_str(cg, " = ");
        gen_expr(cg, it);
        /* 出力の形によっては一時変数を参照しない（クラス名を文字列で渡す等）。未使用の警告を避ける */
        write_str(cg, "; (void)"); write_str(cg, f->name); write_str(cg, "; ");
        f->node = it;
        f->next = (P2C_OrdFrame*)cg->ord_frames;
        cg->ord_frames = f;
    }
    gen_expr_core(cg, e);
    write_str(cg, "; })");
    /* この式の対応表だけを外し、確保した対応を解放する（外側の式の対応は残す） */
    while (cg->ord_frames && cg->ord_frames != saved) {
        P2C_OrdFrame *top = (P2C_OrdFrame*)cg->ord_frames;
        cg->ord_frames = top->next;
        p2c_free(cg->alloc, top);
    }
}

/* 捕捉の対象: モジュール直下で作る関数・ラムダでは、モジュール変数は捕捉しない。
 * モジュール変数は生きた C の大域変数として読まれる（Python の遅延束縛と同じ）。
 * 捕捉すると、ループ後の値や再代入が反映されなくなる。 */
static P2C_Map *closure_capture_scope(P2C_CodeGen *cg) {
    if (!cg->declared_vars) return cg->declared_vars;
    const bool at_module = cg->function_depth == 0 && cg->module_globals;
    P2C_Map *m = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!m) return cg->declared_vars;
    for (size_t i = 0; i < cg->declared_vars->bucket_count; i++) {
        for (P2C_MapEntry *e = cg->declared_vars->buckets[i]; e; e = e->next) {
            const char *name = (const char*)e->key;
            if (name && !(at_module && map_has_name(cg->module_globals, name))) map_set_name(m, name);
        }
    }
    /* 生成中の内包表記の対象変数は、その内包表記の中の C 局所として存在する。捕捉する。 */
    if (cg->comprehension_targets) {
        for (size_t i = 0; i < cg->comprehension_targets->bucket_count; i++) {
            for (P2C_MapEntry *e = cg->comprehension_targets->buckets[i]; e; e = e->next) {
                if (e->key) map_set_name(m, (const char*)e->key);
            }
        }
    }
    return m;
}

/* 呼び出し先が変数・属性（メソッド・モジュールの関数）で、*args / **kwargs を含む呼び出し。
 * 呼び出し先のシグネチャは実行時にしか分からないため、引数を p2c_call_starred で展開する。
 * 既知の関数・組込み・クラス・例外は従来の経路（コンパイル時に展開できる）を使う。 */
static bool call_uses_starred_dispatch(P2C_CodeGen *cg, const P2C_AstNode *n) {
    const P2C_AstExpr *func = n->u.call.func;
    if (!func) return false;
    bool has_star = false;
    for (size_t i = 0; i < p2c_vec_len(n->u.call.args); i++) {
        const P2C_AstExpr *a = (const P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
        if (a && a->base.type == AST_STARRED) has_star = true;
    }
    for (size_t k = 0; k < p2c_vec_len(n->u.call.keywords); k++) {
        const P2C_AstKeyword *kw = (const P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
        if (kw && kw->arg == NULL) has_star = true;
    }
    if (!has_star) return false;
    if (func->base.type == AST_NAME) {
        const char *name = func->base.u.name.name;
        if (!name || strncmp(name, "p2c_", 4) == 0) return false;
        if (p2c_map_get(cg->func_args, name) || map_has_name(cg->known_classes, name)) return false;
        if (is_builtin_callable_name(name) || is_builtin_exception_name(name) || is_python_builtin_name(name)) return false;
        return true;
    }
    return func->base.type == AST_ATTRIBUTE;
}

/* 装飾された関数（値として包まれたもの）へのキーワード引数の呼び出し。
 * 元の def の仮引数順に、位置引数・キーワード引数・既定値を並べ替えて、位置引数の配列として渡す。
 * デコレータの wrapper は位置引数で受け取り、元の関数へ位置引数で渡す（CPython の結果と一致する
 * 一般的な形）。**kwargs へ入るキーワード、キーワード専用引数は、実行時のキーワード経路が無いため
 * 明示的な診断にする。 */
static void gen_decorated_keyword_call(P2C_CodeGen *cg, const char *name, P2C_Vector *args, P2C_Vector *call_keywords) {
    P2C_AstFunctionDef *fdef = (P2C_AstFunctionDef*)p2c_map_get(cg->func_args, name);
    if (!fdef) {
        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "keyword arguments for this decorated function are not supported");
        write_str(cg, "NULL");
        return;
    }
    size_t argc = p2c_vec_len(fdef->args);
    size_t npos = (fdef->kwonly_start >= 0 && (size_t)fdef->kwonly_start <= argc) ? (size_t)fdef->kwonly_start : argc;
    size_t A = p2c_vec_len(args);
    for (size_t i = 0; i < A; i++) {
        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(args, i);
        if (a && a->base.type == AST_STARRED) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "*args together with keyword arguments for a decorated function is not supported yet");
            write_str(cg, "NULL");
            return;
        }
    }
    /* キーワードの対応先（名前付きの仮引数）を求める */
    P2C_AstExpr **kwval = (P2C_AstExpr**)p2c_alloc(cg->alloc, (argc > 0 ? argc : 1) * sizeof(P2C_AstExpr*));
    if (!kwval) { write_str(cg, "NULL"); return; }
    for (size_t i = 0; i < argc; i++) kwval[i] = NULL;
    for (size_t k = 0; k < p2c_vec_len(call_keywords); k++) {
        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(call_keywords, k);
        if (!kw) continue;
        if (!kw->arg) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "**kwargs at a call to a decorated function is not supported yet");
            write_str(cg, "NULL");
            return;
        }
        size_t found = argc;
        for (size_t i = 0; i < argc; i++) {
            P2C_AstArg *pa = (P2C_AstArg*)p2c_vec_get(fdef->args, i);
            if (pa && pa->name && strcmp(pa->name, kw->arg) == 0) { found = i; break; }
        }
        if (found == argc) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                "keyword arguments that go into **kwargs or to keyword-only parameters of a decorated function are not supported yet");
            write_str(cg, "NULL");
            return;
        }
        if (found >= npos) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                "keyword arguments for keyword-only parameters of a decorated function are not supported yet");
            write_str(cg, "NULL");
            return;
        }
        if (found < A) {
            codegen_set_error(cg, P2C_ERR_SEMANTIC, "got multiple values for a parameter of a decorated function");
            write_str(cg, "NULL");
            return;
        }
        kwval[found] = kw->value;
    }
    size_t count = (A > npos) ? A : npos;
    if (count == 0) { write_str(cg, "NULL, 0"); return; }
    char dk[192];
    snprintf(dk, sizeof(dk), "%s", mangle_ident(name));
    write_str(cg, "(P2C_Object*[]){");
    for (size_t i = 0; i < count; i++) {
        if (i) write_str(cg, ", ");
        if (i < A) {
            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(args, i));
        } else if (kwval[i]) {
            gen_expr(cg, kwval[i]);
        } else {
            P2C_AstArg *pa = (P2C_AstArg*)p2c_vec_get(fdef->args, i);
            if (!pa->default_val) {
                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                    "missing required argument in a call to a decorated function");
                write_str(cg, "&P2C_None");
            } else {
                emit_default_value(cg, dk, i, pa->default_val);
            }
        }
    }
    write_str(cg, "}, "); emit_usize(cg, count);
}

/* max / min / sum / zip（配列を受け取る組込み）へ *args を渡す呼び出し。キーワードは対象外。 */
static bool call_uses_starred_builtin(P2C_CodeGen *cg, const P2C_AstNode *n) {
    const P2C_AstExpr *func = n->u.call.func;
    if (!func || func->base.type != AST_NAME || !func->base.u.name.name) return false;
    const char *name = func->base.u.name.name;
    if (strcmp(name, "max") != 0 && strcmp(name, "min") != 0 && strcmp(name, "sum") != 0 && strcmp(name, "zip") != 0) return false;
    if (p2c_map_get(cg->func_args, name) || is_declared(cg, name)) return false;
    if (p2c_vec_len(n->u.call.keywords) > 0) return false;
    for (size_t i = 0; i < p2c_vec_len(n->u.call.args); i++) {
        const P2C_AstExpr *a = (const P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
        if (a && a->base.type == AST_STARRED) return true;
    }
    return false;
}

/* vars()（引数なし）: 現在の関数の局所（代入前のものは辞書に含まれない）。
 * 名前は整列して出力し、生成物を決定的にする。セル・環境・nonlocal・global の名前は対象外。 */
static int vars_name_cmp(const char *const *a, const char *const *b) {
    return strcmp(*a, *b);
}
static void gen_vars_locals(P2C_CodeGen *cg) {
    size_t cap = cg->declared_vars ? cg->declared_vars->bucket_count * 4u + 4u : 4u, n = 0;
    const char **names = (const char**)p2c_alloc(cg->alloc, cap * sizeof(char*));
    if (!names) { write_str(cg, "NULL, NULL, 0"); return; }
    if (cg->declared_vars && cg->declared_vars->buckets) {
        for (size_t i = 0; i < cg->declared_vars->bucket_count; i++) {
            for (P2C_MapEntry *e = cg->declared_vars->buckets[i]; e; e = e->next) {
                const char *nm = (const char*)e->key;
                if (!nm || n >= cap) continue;
                if (strncmp(nm, "_p2c", 4) == 0 || is_synthetic_local(cg, nm)) continue;
                if (map_has_name(cg->cell_names, nm) || map_has_name(cg->closure_env_names, nm)) continue;
                if (map_has_name(cg->nonlocal_names, nm) || map_has_name(cg->global_decl_names, nm)) continue;
                names[n++] = nm;
            }
        }
    }
    /* 挿入整列（名前の数は少ないので十分） */
    for (size_t i = 1; i < n; i++) {
        const char *key = names[i];
        size_t j = i;
        while (j > 0 && vars_name_cmp(&names[j - 1], &key) > 0) { names[j] = names[j - 1]; j--; }
        names[j] = key;
    }
    if (n == 0) { write_str(cg, "NULL, NULL, 0"); return; }
    write_str(cg, "p2c_builtin_vars_locals((const char*[]){");
    for (size_t i = 0; i < n; i++) { if (i) write_str(cg, ", "); write_str(cg, "\""); write_str(cg, names[i]); write_str(cg, "\""); }
    write_str(cg, "}, (P2C_Object*[]){");
    for (size_t i = 0; i < n; i++) {
        if (i) write_str(cg, ", ");
        if (is_native_int_local(cg, names[i])) { write_str(cg, "p2c_obj_from_int("); write_ident(cg, names[i]); write_str(cg, ")"); }
        else write_ident(cg, names[i]);
    }
    write_str(cg, "}, "); emit_usize(cg, n); write_str(cg, ")");
}

static void gen_starred_builtin_call(P2C_CodeGen *cg, const P2C_AstNode *n) {
    size_t argc = p2c_vec_len(n->u.call.args);
    const char *name = n->u.call.func->base.u.name.name;
    write_str(cg, "p2c_call_builtin_starred(\""); write_str(cg, name); write_str(cg, "\", ");
    write_str(cg, "(P2C_Object*[]){");
    for (size_t i = 0; i < argc; i++) {
        if (i) write_str(cg, ", ");
        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
        if (a && a->base.type == AST_STARRED) gen_expr(cg, a->base.u.starred.value);
        else gen_expr(cg, a);
    }
    write_str(cg, "}, (const unsigned char[]){");
    for (size_t i = 0; i < argc; i++) {
        if (i) write_str(cg, ", ");
        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
        write_str(cg, (a && a->base.type == AST_STARRED) ? "1" : "0");
    }
    write_str(cg, "}, "); emit_usize(cg, argc); write_str(cg, ")");
}

static void gen_starred_dispatch_call(P2C_CodeGen *cg, const P2C_AstNode *n) {
    size_t argc = p2c_vec_len(n->u.call.args);
    const P2C_AstKeyword *dstar = NULL;
    for (size_t k = 0; k < p2c_vec_len(n->u.call.keywords); k++) {
        const P2C_AstKeyword *kw = (const P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
        if (!kw) continue;
        if (kw->arg == NULL) dstar = kw;
        else {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                "keyword arguments together with *args at a call to a callable variable are not supported");
        }
    }
    write_str(cg, "p2c_call_starred(");
    gen_expr(cg, n->u.call.func);
    write_str(cg, ", ");
    if (argc == 0) {
        write_str(cg, "NULL, NULL, 0");
    } else {
        write_str(cg, "(P2C_Object*[]){");
        for (size_t i = 0; i < argc; i++) {
            if (i) write_str(cg, ", ");
            P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
            if (a && a->base.type == AST_STARRED) gen_expr(cg, a->base.u.starred.value);
            else gen_expr(cg, a);
        }
        write_str(cg, "}, (const unsigned char[]){");
        for (size_t i = 0; i < argc; i++) {
            if (i) write_str(cg, ", ");
            P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i);
            write_str(cg, (a && a->base.type == AST_STARRED) ? "1" : "0");
        }
        write_str(cg, "}, ");
        emit_usize(cg, argc);
    }
    write_str(cg, ", ");
    if (dstar && dstar->value) gen_expr(cg, dstar->value);
    else write_str(cg, "NULL");
    write_str(cg, ")");
}

static void gen_expr(P2C_CodeGen *cg, P2C_AstExpr *expr) {
    if (!expr) { write_str(cg, "&P2C_None"); return; }
    /* 一時変数へ評価済みの部分式は、その変数を参照する */
    const char *tmp = ord_lookup(cg, expr);
    if (tmp) { write_str(cg, tmp); return; }
    if (expr->base.type == AST_CALL || expr->base.type == AST_DICT ||
        expr->base.type == AST_LIST || expr->base.type == AST_TUPLE || expr->base.type == AST_SET) {
        /* 候補は要素数の2倍＋1を超えない（辞書は キーと値の2つ、呼び出しは関数とレシーバの分が余分） */
        size_t bound = 2 * ord_element_count(expr) + 2;
        P2C_AstExpr **items = (P2C_AstExpr**)p2c_alloc(cg->alloc, bound * sizeof(*items));
        if (!items) { gen_expr_core(cg, expr); return; }
        bool starred_effect = false;
        size_t count = ord_collect(cg, expr, items, &starred_effect);
        /* 順序が問題になるのは、副作用を持つ部分式が2つ以上ある場合と、副作用を持つ部分式と
         * 可変な変数の読み出しが同じ式にある場合だけ（変数だけなら評価順は結果に影響しない）。 */
        size_t effects = 0, readers = 0;
        for (size_t i = 0; i < count; i++) {
            const P2C_AstExpr *it = items[i];
            if (!it || ord_is_pure(cg, it)) continue;
            if (it->base.type == AST_NAME) readers++;
            else effects++;
        }
        if (starred_effect || effects >= 2 || (effects >= 1 && readers >= 1)) {
            gen_expr_ordered(cg, expr, items, count);
        } else {
            gen_expr_core(cg, expr);
        }
        p2c_free(cg->alloc, (void*)items);
        return;
    }
    gen_expr_core(cg, expr);
}

static void gen_expr_core(P2C_CodeGen *cg, P2C_AstExpr *expr) {
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
            } else if (is_synthetic_local(cg, n->u.name.name)) {
                /* コード生成が導入した一時ローカル（unpack の受け皿等）。
                 * declared_vars には入れないので、ここで素の識別子として解決する。 */
                write_ident(cg, n->u.name.name);
            } else if (map_has_name(cg->closure_env_names, n->u.name.name) && !map_has_name(cg->declared_vars, n->u.name.name) && cg->closure_env_var) {
                write_str(cg, "p2c_cell_get(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", "); write_name_lit_str(cg, n->u.name.name); write_str(cg, "))");
            } else if (map_has_name(cg->cell_names, n->u.name.name)) {
                write_str(cg, "p2c_cell_get(_p2c_cell_"); write_ident(cg, n->u.name.name); write_str(cg, ")");
            } else if (map_has_name(cg->decorated_names, n->u.name.name)) {
                write_str(cg, "_p2c_decorated_"); write_ident(cg, n->u.name.name);
            } else if (n->u.name.name && map_has_name(cg->module_function_names, n->u.name.name) &&
                       !is_declared(cg, n->u.name.name) &&
                       !map_has_name(cg->closure_env_names, n->u.name.name) &&
                       !map_has_name(cg->cell_names, n->u.name.name)) {
                /* モジュール関数を「値」として使う場合（g = f, sorted(key=f), map(f, ...)）。
                 * 生のC関数名（P2C_Object* (*)(...)）を出力するとP2C_Object*の位置で
                 * 型が合わずコンパイルできないため、callable adapterを包んだ
                 * P2C_Objectへ解決する。adapter本体は本体生成の最後に、実際に
                 * 値参照された関数だけへ生成する（未使用警告を避けるため）。 */
                P2C_AstFunctionDef *vfd = (P2C_AstFunctionDef*)p2c_map_get(cg->func_args, n->u.name.name);
                if (vfd) {
                    map_set_name(cg->function_value_names, n->u.name.name);
                    write_str(cg, "p2c_function_new(\""); write_str(cg, n->u.name.name); write_str(cg, "\", ");
                    write_ident(cg, n->u.name.name); write_str(cg, "__decorator_adapter)");
                } else {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                        "functions with *args or **kwargs cannot be used as values yet");
                    write_str(cg, "&P2C_None");
                }
            } else if (n->u.name.name && is_builtin_callable_name(n->u.name.name) &&
                       !is_declared(cg, n->u.name.name) &&
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
                } else if (n->u.name.name && !is_declared(cg, n->u.name.name) &&
                           !map_has_name(cg->known_classes, n->u.name.name) &&
                           !map_has_name(cg->module_globals, n->u.name.name) &&
                           !map_has_name(cg->nonlocal_names, n->u.name.name) &&
                           !map_has_name(cg->module_function_names, n->u.name.name) &&
                           !map_has_name(cg->comprehension_targets, n->u.name.name) &&
                           !is_builtin_exception_name(n->u.name.name)) {
                    /* どのスコープにも見つからない名前。Python と同じく実行時
                     * NameError にする（生の C 識別子を出力すると生成Cが
                     * 「undeclared identifier」でコンパイルできない）。 */
                    write_str(cg, "p2c_name_error_ref(\"");
                    write_str(cg, n->u.name.name);
                    write_str(cg, "\")");
                } else if (is_native_int_local(cg, n->u.name.name)) {
                    /* AOT アンボクシング: 型付きローカルは値を P2C_Object* の
                     * 文脈へ出すときにボックス化する。 */
                    write_str(cg, "p2c_obj_from_int("); write_ident(cg, n->u.name.name); write_str(cg, ")");
                } else if (is_declared(cg, n->u.name.name) && !cg->in_suspension_body &&
                           !map_has_name(cg->cell_names, n->u.name.name) &&
                           !map_has_name(cg->closure_env_names, n->u.name.name)) {
                    /* 代入前に読むと UnboundLocalError（CPython と同じ） */
                    write_str(cg, "p2c_local_checked("); write_ident(cg, n->u.name.name);
                    write_str(cg, ", \""); write_str(cg, n->u.name.name); write_str(cg, "\")");
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
                    write_user_lit_str(cg, n->u.constant.value);
                    break;
                case TOK_BYTES_LITERAL:
                    /* 16進の文字列からバイト列を作る（中身は ASCII の16進だけなので直接書ける） */
                    write_str(cg, "p2c_bytes_from_hex(\""); write_str(cg, n->u.constant.value); write_str(cg, "\")");
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
            const char *rawf = cg->opts.unbox_int_locals ? raw_binop_func(n->u.binop.op) : NULL;
            /* AOT アンボクシング: 両辺が int と確定しているなら生の int64 で計算し、
             * 結果だけをボックス化する（オペランドのボックス化を省く）。 */
            if (rawf && expr_is_int_typed(cg, n->u.binop.left) && expr_is_int_typed(cg, n->u.binop.right)) {
                write_str(cg, "p2c_obj_from_int("); write_str(cg, rawf); write_str(cg, "(");
                gen_int_expr(cg, n->u.binop.left); write_str(cg, ", ");
                gen_int_expr(cg, n->u.binop.right); write_str(cg, "))");
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
                write_str(cg, "p2c_obj_neg("); gen_expr(cg, n->u.unaryop.operand); write_str(cg, ")");
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
                /* AOT アンボクシング: 順序・等値比較は、両辺が int なら生比較、
                 * 片側だけが型付き int なら混在ヘルパで比較する
                 * （型付きローカルをボックス化しない）。 */
                if (cg->opts.unbox_int_locals && c_cmp_symbol(*op) && raw_cmp_helper(*op) &&
                    (expr_is_int_typed(cg, n->u.compare.left) || expr_is_int_typed(cg, right))) {
                    bool left_raw = expr_is_int_typed(cg, n->u.compare.left);
                    bool right_raw = expr_is_int_typed(cg, right);
                    if (left_raw && right_raw) {
                        write_str(cg, "(("); gen_int_expr(cg, n->u.compare.left); write_str(cg, ") ");
                        write_str(cg, c_cmp_symbol(*op)); write_str(cg, " (");
                        gen_int_expr(cg, right); write_str(cg, ") ? &P2C_True : &P2C_False)");
                    } else if (left_raw) {
                        write_str(cg, "("); write_str(cg, raw_cmp_helper(*op)); write_str(cg, "(");
                        gen_int_expr(cg, n->u.compare.left); write_str(cg, ", ");
                        gen_expr(cg, right); write_str(cg, ") ? &P2C_True : &P2C_False)");
                    } else {
                        write_str(cg, "("); write_str(cg, raw_cmp_helper(swapped_cmp(*op))); write_str(cg, "(");
                        gen_int_expr(cg, right); write_str(cg, ", ");
                        gen_expr(cg, n->u.compare.left); write_str(cg, ") ? &P2C_True : &P2C_False)");
                    }
                    break;
                }
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
            if (n->u.call.func && n->u.call.func->base.type == AST_NAME && n->u.call.func->base.u.name.name &&
                strncmp(n->u.call.func->base.u.name.name, "_gui_", 5) == 0 &&
                !p2c_map_get(cg->func_args, n->u.call.func->base.u.name.name) &&
                !is_declared(cg, n->u.call.func->base.u.name.name) && p2c_vec_len(n->u.call.keywords) == 0) {
                /* 描画の低レベル関数（_gui_open など）はランタイムへ渡す */
                write_str(cg, "p2c_gui_prim_call(\""); write_str(cg, n->u.call.func->base.u.name.name); write_str(cg, "\", ");
                size_t gn = p2c_vec_len(n->u.call.args);
                if (gn == 0) write_str(cg, "NULL, 0");
                else {
                    write_str(cg, "(P2C_Object*[]){");
                    for (size_t gi = 0; gi < gn; gi++) { if (gi) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, gi)); }
                    write_str(cg, "}, "); emit_usize(cg, gn);
                }
                write_str(cg, ")");
            } else if (call_uses_starred_builtin(cg, n)) {
                gen_starred_builtin_call(cg, n);
            } else if (call_uses_starred_dispatch(cg, n)) {
                gen_starred_dispatch_call(cg, n);
            } else if (func->base.type == AST_ATTRIBUTE && func->base.u.attribute.value &&
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
                } else if (strcmp(name, "hash") == 0) {
                    /* hash(obj): int を返す。以前は未対応で生の hash(...) を出力し、
                     * 生成Cがコンパイルできなかった。 */
                    write_str(cg, "p2c_builtin_hash(");
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
                } else if (strcmp(name, "enumerate") == 0 && (argc >= 1)) {
                    P2C_AstExpr *start_expr = (argc >= 2) ? (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1) : NULL;
                    /* start= キーワードも受け付ける（以前は位置引数のみで、
                     * enumerate(x, start=1) が 0 始まりになっていた）。 */
                    for (size_t ki = 0; ki < nkw; ki++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                        if (kw && kw->arg && strcmp(kw->arg, "start") == 0 && !start_expr) start_expr = kw->value;
                    }
                    write_str(cg, "p2c_builtin_enumerate(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                    write_str(cg, ", ");
                    if (start_expr) gen_expr(cg, start_expr); else write_str(cg, "p2c_obj_from_int(0)");
                    write_str(cg, ")");
                } else if (strcmp(name, "zip") == 0) {
                    if (argc == 1 && ((P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0))->base.type == AST_STARRED) {
                        /* zip(*m): 展開対象が引数の列（転置の定番）なので、
                         * 実行時に実体化してから zip へ渡す。 */
                        write_str(cg, "p2c_builtin_zip_star(");
                        gen_expr(cg, ((P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0))->base.u.starred.value);
                        write_str(cg, ")");
                    } else {
                        write_str(cg, "p2c_builtin_zip("); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                    }
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
                                if (strcmp(tn,"int")==0) ck="p2c_obj_is_int_like";
                                else if (strcmp(tn,"float")==0) ck="p2c_obj_is_float";
                                else if (strcmp(tn,"str")==0) ck="p2c_obj_is_str";
                                else if (strcmp(tn,"bytes")==0) ck="p2c_obj_is_bytes";
                                else if (strcmp(tn,"bool")==0) ck="p2c_obj_is_bool";
                                else if (strcmp(tn,"list")==0) ck="p2c_obj_is_list";
                                else if (strcmp(tn,"dict")==0) ck="p2c_obj_is_dict";
                                else if (strcmp(tn,"set")==0) ck="p2c_obj_is_plain_set";
                                else if (strcmp(tn,"frozenset")==0) ck="p2c_obj_is_frozenset";
                                else if (strcmp(tn,"tuple")==0) ck="p2c_obj_is_tuple";
                            }
                            if (tn && strcmp(tn, "object") == 0) { write_str(cg, "true"); }
                            else if (ck) { write_str(cg, ck); write_str(cg, "("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")"); }
                            else { write_str(cg, "false"); }
                        }
                        write_str(cg, ")");
                    } else {
                        const char *type_name = (type_arg->base.type == AST_NAME) ? type_arg->base.u.name.name : NULL;
                        const char *checker = NULL;
                        if (type_name) {
                            if (strcmp(type_name, "int") == 0) checker = "p2c_obj_is_int_like";
                            else if (strcmp(type_name, "float") == 0) checker = "p2c_obj_is_float";
                            else if (strcmp(type_name, "str") == 0) checker = "p2c_obj_is_str";
        else if (strcmp(type_name, "bytes") == 0) checker = "p2c_obj_is_bytes";
                            else if (strcmp(type_name, "bool") == 0) checker = "p2c_obj_is_bool";
                            else if (strcmp(type_name, "list") == 0) checker = "p2c_obj_is_list";
                            else if (strcmp(type_name, "dict") == 0) checker = "p2c_obj_is_dict";
                            else if (strcmp(type_name, "set") == 0) checker = "p2c_obj_is_plain_set";
                            else if (strcmp(type_name, "frozenset") == 0) checker = "p2c_obj_is_frozenset";
                            else if (strcmp(type_name, "tuple") == 0) checker = "p2c_obj_is_tuple";
                        }
                        if (type_name && strcmp(type_name, "object") == 0) {
                            /* isinstance(x, object) は常に True（object は最上位）。 */
                            write_str(cg, "p2c_obj_from_bool(true)");
                        } else if (checker) {
                            write_str(cg, "p2c_obj_from_bool("); write_str(cg, checker); write_str(cg, "(");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                            write_str(cg, "))");
                        } else if (type_name && map_has_name(cg->known_classes, type_name)) {
                            write_str(cg, "p2c_obj_from_bool(p2c_isinstance_of_class(");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                            write_str(cg, ", \""); write_str(cg, type_name); write_str(cg, "\"))");
                        } else {
                            /* クラス名がbare nameでない場合（Outer.Innerのような属性経由や、
                             * クラスオブジェクトを保持した変数、type(None) のような
                             * type()呼び出し）は第2引数を評価して判定する。
                             * type() は "<class 'Name'>" 形式の文字列を返すため、
                             * p2c_isinstance_of_typeobj がクラスオブジェクトと
                             * その文字列の両方を受け付ける。以前はここが常にfalseに
                             * なり、isinstance(x, ns.Class) が黙ってFalseを返し、
                             * isinstance(x, type(None)) も誤っていた。 */
                            write_str(cg, "p2c_obj_from_bool(p2c_isinstance_of_typeobj(");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                            write_str(cg, ", ");
                            gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                            write_str(cg, "))");
                        }
                    }
                } else if (strcmp(name, "issubclass") == 0 && argc >= 2) {
                    /* issubclass(cls, Base)。Base が既知クラス名なら名前でMROを辿り、
                     * それ以外はクラスオブジェクトとして判定する。以前は未知関数として
                     * 生の issubclass(...) を出力し、生成Cがコンパイルできなかった。 */
                    P2C_AstExpr *base_arg = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1);
                    const char *base_name = (base_arg->base.type == AST_NAME) ? base_arg->base.u.name.name : NULL;
                    if (base_name && (map_has_name(cg->known_classes, base_name) || strcmp(base_name, "object") == 0)) {
                        write_str(cg, "p2c_obj_from_bool(p2c_issubclass_of_class(");
                        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                        write_str(cg, ", \""); write_str(cg, base_name); write_str(cg, "\"))");
                    } else {
                        write_str(cg, "p2c_obj_from_bool(p2c_issubclass_of_typeobj(");
                        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0));
                        write_str(cg, ", ");
                        gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1));
                        write_str(cg, "))");
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
                            write_str(cg, " p2c_dict_set(_p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, ", "); write_name_lit_str(cg, kw->arg); write_str(cg, ", "); gen_expr(cg, kw->value); write_str(cg, ");");
                        } else {
                            write_str(cg, " p2c_dict_update(_p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, ", "); gen_expr(cg, kw->value); write_str(cg, ");");
                        }
                    }
                    write_str(cg, " _p2c_dict_"); emit_usize(cg, (size_t)dict_id); write_str(cg, "; })");
                } else if (strcmp(name, "dict") == 0 && argc == 0) {
                    write_str(cg, "p2c_dict_new()");
                } else if (strcmp(name, "dict") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_dict("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "set") == 0 && argc == 0) {
                    write_str(cg, "p2c_set_new()");
                } else if (strcmp(name, "frozenset") == 0 && argc == 0) {
                    write_str(cg, "p2c_frozenset_new()");
                } else if (strcmp(name, "frozenset") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_frozenset("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "set") == 0 && argc == 1) {
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
                } else if (strcmp(name, "bytes") == 0 && p2c_vec_len(n->u.call.keywords) == 0) {
                    /* bytes(...): 引数は配列で渡す。encoding= は UTF-8 のときだけ（ここでは受け取らない） */
                    write_str(cg, "p2c_builtin_bytes((P2C_Object*[]){");
                    for (size_t bi = 0; bi < argc; bi++) { if (bi) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, bi)); }
                    if (argc == 0) write_str(cg, "NULL");
                    write_str(cg, "}, "); emit_usize(cg, argc); write_str(cg, ")");
                } else if (strcmp(name, "vars") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_vars_obj("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "vars") == 0 && argc == 0) {
                    gen_vars_locals(cg);
                } else if (strcmp(name, "ascii") == 0 && argc == 1) {
                    write_str(cg, "p2c_builtin_ascii("); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ")");
                } else if (strcmp(name, "open") == 0 && argc >= 1 && argc <= 2) {
                    /* open(path, mode, encoding=..., mode=...): キーワードは mode と encoding のみ。
                     * 以前はキーワードを黙って無視していた（open(p, mode="w") が読み取りになる）。 */
                    P2C_AstExpr *mode_expr = argc == 2 ? (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 1) : NULL;
                    P2C_AstExpr *enc_expr = NULL;
                    for (size_t k = 0; k < p2c_vec_len(n->u.call.keywords); k++) {
                        P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, k);
                        if (!kw) continue;
                        if (kw->arg && strcmp(kw->arg, "mode") == 0 && !mode_expr) { mode_expr = kw->value; continue; }
                        if (kw->arg && strcmp(kw->arg, "encoding") == 0 && !enc_expr) { enc_expr = kw->value; continue; }
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                            "open() accepts only the mode and encoding arguments in this implementation (each given once)");
                    }
                    write_str(cg, "p2c_builtin_open_ex(");
                    gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, 0)); write_str(cg, ", ");
                    if (mode_expr) gen_expr(cg, mode_expr); else write_str(cg, "p2c_obj_from_str(\"r\")");
                    write_str(cg, ", ");
                    if (enc_expr) gen_expr(cg, enc_expr); else write_str(cg, "&P2C_None");
                    write_str(cg, ")");
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
                    write_str(cg, "p2c_call(_p2c_decorated_"); write_ident(cg, name); write_str(cg, ", ");
                    if (nkw != 0) gen_decorated_keyword_call(cg, name, n->u.call.args, n->u.call.keywords);
                    else emit_args_array(cg, n->u.call.args);
                    write_str(cg, ")");
                } else if (nested_class_alias(cg, name)) {
                    /* クラス本体内でのネストクラス呼び出し（例: factory = Inner()）は
                     * 前置済みC名のクラスオブジェクトを呼び出す。 */
                    if (!cg->in_class_body) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                            "nested class names are not visible inside method bodies; refer to the class through its enclosing class (Outer.Inner)");
                    }
                    write_str(cg, "p2c_call("); write_str(cg, nested_class_alias(cg, name)); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else if (map_has_name(cg->known_classes, name)) {
                    P2C_AstFunctionDef *init_def = (P2C_AstFunctionDef*)p2c_map_get(cg->class_init_defs, name);
                    /* __init__ の既定値はクラス本体の実行時に評価済みの隠しグローバルから読む。 */
                    char cdefkey[384] = {0};
                    if (init_def) snprintf(cdefkey, sizeof(cdefkey), "%s__%s", name, mangle_ident(init_def->name));
                    if (nkw > 0 && init_def) {
                        /* クラス生成のキーワード引数を __init__ のパラメータ順へ解決する。
                         * 以前はキーワードが黙って無視され、A(y=9) が既定値のまま
                         * になっていた。 */
                        size_t nparams = p2c_vec_len(init_def->args);
                        size_t startp = 0;
                        if (nparams > 0) {
                            P2C_AstArg *a0 = (P2C_AstArg*)p2c_vec_get(init_def->args, 0);
                            if (a0 && a0->name && (strcmp(a0->name, "self") == 0 || strcmp(a0->name, "cls") == 0)) startp = 1;
                        }
                        if (init_def->vararg || init_def->kwarg) {
                            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                "keyword arguments for a class whose __init__ takes *args/**kwargs are not supported");
                        }
                        write_str(cg, "p2c_call("); write_ident(cg, name); write_str(cg, ", (P2C_Object*[]){");
                        size_t emitted = 0;
                        for (size_t pi = startp; pi < nparams; pi++) {
                            P2C_AstArg *p = (P2C_AstArg*)p2c_vec_get(init_def->args, pi);
                            P2C_AstExpr *pos = (pi - startp < argc) ? (P2C_AstExpr*)p2c_vec_get(n->u.call.args, pi - startp) : NULL;
                            if (emitted) write_str(cg, ", ");
                            emitted++;
                            if (pos) { gen_expr(cg, pos); continue; }
                            P2C_AstExpr *kwval = NULL;
                            for (size_t ki = 0; ki < nkw; ki++) {
                                P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(n->u.call.keywords, ki);
                                if (kw && kw->arg && p->name && strcmp(kw->arg, p->name) == 0) { kwval = kw->value; break; }
                            }
                            if (kwval) gen_expr(cg, kwval);
                            else emit_default_value(cg, cdefkey, pi, p->default_val);
                        }
                        write_str(cg, "}, "); emit_usize(cg, emitted); write_str(cg, ")");
                    } else {
                        if (nkw > 0) {
                            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                "keyword arguments for this constructor are not supported");
                        }
                        write_str(cg, "p2c_call("); write_ident(cg, name); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                    }
                } else if (is_declared(cg, name) || map_has_name(cg->comprehension_targets, name)) {
                    write_str(cg, "p2c_call("); write_ident(cg, name); write_str(cg, ", "); emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                } else if (is_builtin_exception_name(name)) {
                    /* 式の位置での組み込み例外の構築（e = ValueError("v") や
                     * raise X from ValueError("cause")）。以前は未定義関数呼び出し
                     * ValueError(...) として不正なCを出力していた。
                     * 引数はタプルとして保持し、`.args` が CPython と同じ値になる
                     * ようにする（表示用メッセージだけ KeyError は repr を使う）。 */
                    if (argc >= 1) {
                        write_str(cg, "p2c_make_exception_with_args(\""); write_str(cg, name); write_str(cg, "\", ");
                        emit_args_array(cg, n->u.call.args);
                        write_str(cg, ")");
                    } else {
                        write_str(cg, "p2c_make_exception(\""); write_str(cg, name); write_str(cg, "\", \"\")");
                    }
                } else if (name && !map_has_name(cg->declared_vars, name) &&
                           (map_has_name(cg->closure_env_names, name) || map_has_name(cg->cell_names, name))) {
                    /* 捕捉変数/セルに入っている callable を呼ぶ（高階関数の f(x)、
                     * デコレータ内の fn(...) など）。以前は生の識別子 f(...) を
                     * 出力していたため、生成Cが「implicit declaration of function 'f'」
                     * でコンパイルできなかった。 */
                    bool callable_starred = false;
                    for (size_t ai = 0; ai < argc; ai++) {
                        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, ai);
                        if (a && a->base.type == AST_STARRED) { callable_starred = true; break; }
                    }
                    if (callable_starred || nkw > 0) {
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                            "*args/**kwargs at a call to a callable variable is not supported; pass arguments explicitly");
                    }
                    write_str(cg, "p2c_call(");
                    if (map_has_name(cg->closure_env_names, name) && cg->closure_env_var) {
                        write_str(cg, "p2c_cell_get(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", ");
                        write_name_lit_str(cg, name); write_str(cg, "))");
                    } else {
                        write_str(cg, "p2c_cell_get(_p2c_cell_"); write_ident(cg, name); write_str(cg, ")");
                    }
                    write_str(cg, ", ");
                    if (callable_starred || nkw > 0) write_str(cg, "NULL, 0)");
                    else { emit_args_array(cg, n->u.call.args); write_str(cg, ")"); }
                } else if (name && strncmp(name, "p2c_", 4) != 0 && !p2c_map_get(cg->func_args, name) &&
                           !map_has_name(cg->known_classes, name) && !map_has_name(cg->closure_env_names, name)) {
                    /* 呼び出し先が、def・クラス・局所・捕捉変数のどれでもない。
                     * 以前は名前をそのまま C の関数として書いていたため、モジュール変数に入った
                     * callable（from math import sqrt を関数の中で呼ぶ）は「P2C_Object は関数では
                     * ない」でコンパイルできず、未定義名や libc と同名の open() などはリンクエラーや
                     * 実行時クラッシュ（libc の open(2) を P2C_Object* で呼ぶ）になっていた。 */
                    size_t cargc = p2c_vec_len(n->u.call.args);
                    bool cstar = false;
                    for (size_t ai = 0; ai < cargc; ai++) {
                        P2C_AstExpr *a = (P2C_AstExpr*)p2c_vec_get(n->u.call.args, ai);
                        if (a && a->base.type == AST_STARRED) { cstar = true; break; }
                    }
                    if (cg->module_globals && map_has_name(cg->module_globals, name)) {
                        if (cstar || nkw > 0) {
                            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                "*args/**kwargs or keyword arguments at a call to a callable variable are not supported; pass arguments explicitly");
                        }
                        write_str(cg, "p2c_call("); write_ident(cg, name); write_str(cg, ", ");
                        emit_args_array(cg, n->u.call.args); write_str(cg, ")");
                    } else if (is_python_builtin_name(name)) {
                        char msg[160];
                        snprintf(msg, sizeof(msg), "the Python builtin '%s' is not supported by this implementation", name);
                        codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, msg);
                        write_str(cg, "p2c_name_error_ref(\""); write_str(cg, name); write_str(cg, "\")");
                    } else {
                        /* 未定義名: CPython は引数を評価する前に NameError を送出する */
                        write_str(cg, "p2c_name_error_ref(\""); write_str(cg, name); write_str(cg, "\")");
                    }
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
                    /* モジュール関数の既定値は def 時に評価済みの隠しグローバルから読む。 */
                    char defkey[192];
                    const char *dkey = NULL;
                    if (fdef && map_has_name(cg->module_function_names, name)) {
                        snprintf(defkey, sizeof(defkey), "%s", mangle_ident(name));
                        dkey = defkey;
                    }
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
                                    write_str(cg, ", ");
                                    write_name_lit_str(cg, pa->name);
                                    write_str(cg, ", ");
                                    emit_default_value(cg, dkey, i, pa->default_val);
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
                            /* 複数混合 or unknown: ベストエフォートでそのまま渡す。
                             * 既知の固定シグネチャ関数に対して *args と **kwargs を
                             * 混在させた場合は、正しい引数を組み立てられないため
                             * 不正なCを出さず明示的に診断する。 */
                            if (fdef && (has_starred || has_dstar)) {
                                codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                                    "mixing *args/**kwargs unpacking at a call site is not supported; pass arguments explicitly");
                            }
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
                            else emit_default_value(cg, dkey, i, pa ? pa->default_val : NULL);
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
                                    write_name_lit_str(cg, kw->arg);
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
            if (map_has_name(cg->cell_names, n->u.named_expr.target->base.u.name.name)) {
                /* セル: 値を一度だけ評価して保存し、その値を式の値にする */
                write_str(cg, "({ P2C_Object *_p2c_walrus = "); gen_expr(cg, n->u.named_expr.value);
                write_str(cg, "; p2c_cell_set(_p2c_cell_"); write_ident(cg, n->u.named_expr.target->base.u.name.name);
                write_str(cg, ", _p2c_walrus); _p2c_walrus; })");
            } else {
                write_str(cg, "("); write_ident(cg, n->u.named_expr.target->base.u.name.name); write_str(cg, " = "); gen_expr(cg, n->u.named_expr.value); write_str(cg, ")");
            }
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
            P2C_Map *captures = closure_capture_scope(cg);
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
            cg->synthetic_locals = NULL;
            /* ラムダは独自のセルを持たない（外側のセルは env 経由で読む）。 */
            P2C_Map *saved_lambda_cells = cg->cell_names;
            cg->cell_names = NULL;
            /* 既定値の式は外側の捕捉変数を参照しうるため、env を先に設定する。 */
            cg->closure_env_names = captures;
            cg->closure_env_var = "env";
            /* 位置引数はキーワード専用引数より前。キーワード専用は既定値だけで束縛する。 */
            size_t lam_npos = (n->u.lambda.kwonly_start >= 0) ? (size_t)n->u.lambda.kwonly_start : p2c_vec_len(largs);
            for (size_t i = 0; i < p2c_vec_len(largs); i++) {
                P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(largs, i);
                indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, arg->name);
                if (i < lam_npos) {
                    write_str(cg, " = (nargs > "); emit_usize(cg, i); write_str(cg, ") ? args["); emit_usize(cg, i); write_str(cg, "] : ");
                } else {
                    write_str(cg, " = ");
                }
                if (arg->default_val) {
                    /* 既定値は lambda 生成時に評価され env に保存されている。 */
                    char ekey[64];
                    default_env_key(ekey, sizeof(ekey), i);
                    write_str(cg, "p2c_dict_get(env, "); write_name_lit_str(cg, ekey); write_str(cg, ")");
                } else {
                    write_str(cg, "&P2C_None");
                }
                write_str(cg, ";"); write_newline(cg);
                remember_declared(cg, arg->name);
            }
            /* *args（残りの位置引数のタプル）と **kwargs（呼び出しはキーワードを渡さないので空の辞書） */
            /* 余分な位置引数は TypeError（CPython と同じ）。*args があれば受け取る。 */
            if (!n->u.lambda.vararg && lam_npos < 0x7fffffffu) {
                indent(cg); write_str(cg, "if (nargs > "); emit_usize(cg, lam_npos); write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"<lambda>() takes "); emit_usize(cg, lam_npos); write_str(cg, " positional arguments but more were given\"));"); write_newline(cg);
            }
            if (n->u.lambda.vararg) {
                remember_declared(cg, n->u.lambda.vararg);
                indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, n->u.lambda.vararg);
                write_str(cg, " = (nargs > "); emit_usize(cg, lam_npos); write_str(cg, ") ? p2c_tuple_from_array(args + ");
                emit_usize(cg, lam_npos); write_str(cg, ", nargs - "); emit_usize(cg, lam_npos);
                write_str(cg, ") : p2c_tuple_from_array(NULL, 0);"); write_newline(cg);
            }
            if (n->u.lambda.kwarg) {
                remember_declared(cg, n->u.lambda.kwarg);
                indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, n->u.lambda.kwarg);
                write_str(cg, " = p2c_dict_new();"); write_newline(cg);
            }
            cg->closure_env_names = captures;
            cg->closure_env_var = "env";
            indent(cg); write_str(cg, "return "); gen_expr(cg, n->u.lambda.body); write_str(cg, ";"); write_newline(cg);
            p2c_map_free(cg->declared_vars);
    p2c_map_free(cg->synthetic_locals);
            cg->cell_names = saved_lambda_cells;
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
            {
            size_t lndefaults = 0;
            for (size_t i = 0; i < p2c_vec_len(largs); i++) {
                P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(largs, i);
                if (a && a->default_val) lndefaults++;
            }
            size_t ltotal = capture_count + lndefaults;
            if (ltotal == 0) {
                write_str(cg, "p2c_dict_from_pairs(NULL, NULL, 0)");
            } else {
                write_str(cg, "p2c_dict_from_pairs((P2C_Object*[]){");
                bool first = true;
                for (size_t i = 0; i < captures->bucket_count; i++) for (P2C_MapEntry *entry = captures->buckets[i]; entry; entry = entry->next) {
                    if (!first) write_str(cg, ", ");
                    first = false;
                    write_name_lit_str(cg, (const char*)entry->key);
                }
                for (size_t i = 0; i < p2c_vec_len(largs); i++) {
                    P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(largs, i);
                    if (!a || !a->default_val) continue;
                    char ekey[64];
                    default_env_key(ekey, sizeof(ekey), i);
                    if (!first) write_str(cg, ", ");
                    first = false;
                    write_name_lit_str(cg, ekey);
                }
                write_str(cg, "}, (P2C_Object*[]){");
                first = true;
                for (size_t i = 0; i < captures->bucket_count; i++) for (P2C_MapEntry *entry = captures->buckets[i]; entry; entry = entry->next) {
                    if (!first) write_str(cg, ", ");
                    first = false;
                    if (map_has_name(cg->cell_names, (const char*)entry->key)) { write_str(cg, "_p2c_cell_"); write_ident(cg, (const char*)entry->key); }
                    else { write_str(cg, "p2c_cell_new("); write_ident(cg, (const char*)entry->key); write_str(cg, ")"); }
                }
                /* 既定値は lambda 生成時に一度だけ評価して保存する。 */
                for (size_t i = 0; i < p2c_vec_len(largs); i++) {
                    P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(largs, i);
                    if (!a || !a->default_val) continue;
                    if (!first) write_str(cg, ", ");
                    first = false;
                    gen_expr(cg, a->default_val);
                }
                write_str(cg, "}, "); emit_usize(cg, ltotal); write_str(cg, ")");
            }
            }
            write_str(cg, ")");
            break;
        }
        case AST_LIST:
        case AST_SET:
        case AST_TUPLE: {
            P2C_Vector *elts = (n->type == AST_TUPLE) ? n->u.tuple.elts : n->u.list.elts;
            bool has_starred = false;
            for (size_t i = 0; elts && i < p2c_vec_len(elts); i++) {
                P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
                if (elt && elt->base.type == AST_STARRED) { has_starred = true; break; }
            }
            if (has_starred) {
                /* [*a, b] / (*a,) / {*a, b}: いったん list を組み立ててから
                 * 目的の型へ変換する（要素の展開は list.extend と同じ経路）。 */
                if (cg->opts.strict_c11) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                        "starred unpacking inside a list/tuple/set literal is unavailable in strict ISO C11 mode");
                    write_str(cg, "&P2C_None");
                    break;
                }
                int temp = ++cg->temp_counter;
                write_str(cg, "({ P2C_Object *_p2c_seq_");
                emit_usize(cg, (size_t)temp);
                write_str(cg, " = p2c_list_new(); ");
                for (size_t i = 0; elts && i < p2c_vec_len(elts); i++) {
                    P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
                    if (elt && elt->base.type == AST_STARRED) {
                        write_str(cg, "(void)p2c_call_attr(_p2c_seq_");
                        emit_usize(cg, (size_t)temp);
                        write_str(cg, ", \"extend\", (P2C_Object*[]){");
                        gen_expr(cg, elt->base.u.starred.value);
                        write_str(cg, "}, 1); ");
                    } else {
                        write_str(cg, "p2c_list_append(_p2c_seq_");
                        emit_usize(cg, (size_t)temp);
                        write_str(cg, ", ");
                        gen_expr(cg, elt);
                        write_str(cg, "); ");
                    }
                }
                if (n->type == AST_TUPLE) {
                    write_str(cg, "p2c_builtin_tuple(_p2c_seq_");
                    emit_usize(cg, (size_t)temp);
                    write_str(cg, "); })");
                } else if (n->type == AST_SET) {
                    write_str(cg, "p2c_builtin_set(_p2c_seq_");
                    emit_usize(cg, (size_t)temp);
                    write_str(cg, "); })");
                } else {
                    write_str(cg, "_p2c_seq_");
                    emit_usize(cg, (size_t)temp);
                    write_str(cg, "; })");
                }
                break;
            }
            if (n->type == AST_TUPLE) {
                write_str(cg, "p2c_tuple_from_array(");
                if (p2c_vec_len(elts) == 0) write_str(cg, "NULL, 0");
                else {
                    write_str(cg, "(P2C_Object*[]){");
                    for (size_t i = 0; i < p2c_vec_len(elts); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(elts, i)); }
                    write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(elts));
                }
                write_str(cg, ")");
            } else if (n->type == AST_SET) {
                write_str(cg, "p2c_set_from_array(");
                if (p2c_vec_len(elts) == 0) write_str(cg, "NULL, 0");
                else {
                    write_str(cg, "(P2C_Object*[]){");
                    for (size_t i = 0; i < p2c_vec_len(elts); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(elts, i)); }
                    write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(elts));
                }
                write_str(cg, ")");
            } else {
                write_str(cg, "p2c_list_from_array(");
                if (p2c_vec_len(elts) == 0) write_str(cg, "NULL, 0");
                else {
                    write_str(cg, "(P2C_Object*[]){");
                    for (size_t i = 0; i < p2c_vec_len(elts); i++) { if (i) write_str(cg, ", "); gen_expr(cg, (P2C_AstExpr*)p2c_vec_get(elts, i)); }
                    write_str(cg, "}, "); emit_usize(cg, p2c_vec_len(elts));
                }
                write_str(cg, ")");
            }
            break;
        }
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
            /* 未対応の式。--fallback ではランタイムスタブ、既定では変換エラー。
             * yield/await がここへ来るのは「状態機械へ落とせない位置」に
             * ある場合（例: クロージャ内の入れ子関数として定義された
             * ジェネレータ）なので、原因が分かる診断にする。 */
            if (n->type == AST_YIELD || n->type == AST_AWAIT) {
                emit_unsupported_construct(cg, n,
                    (n->type == AST_YIELD) ? "yield in this position"
                                           : "await in this position", true);
            } else {
                emit_unsupported_construct(cg, n, "expression", true);
            }
            break;
    }
}

static void gen_assign_target(P2C_CodeGen *cg, P2C_AstExpr *target, P2C_AstExpr *value) {
    if (!target) return;
    if (target->base.type == AST_NAME) {
        indent(cg);
        if (map_has_name(cg->nonlocal_names, target->base.u.name.name) && cg->closure_env_var) {
            write_str(cg, "p2c_cell_set(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", "); write_name_lit_str(cg, target->base.u.name.name); write_str(cg, "), "); gen_expr(cg, value); write_str(cg, ");");
        } else if (map_has_name(cg->cell_names, target->base.u.name.name)) {
            write_str(cg, "p2c_cell_set(_p2c_cell_"); write_ident(cg, target->base.u.name.name); write_str(cg, ", "); gen_expr(cg, value); write_str(cg, ");");
        } else if (is_native_int_local(cg, target->base.u.name.name)) {
            /* AOT アンボクシング: 型付きローカルへの代入は生の int64 のまま入れる。 */
            write_ident(cg, target->base.u.name.name); write_str(cg, " = "); gen_int_expr(cg, value); write_str(cg, ";");
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
        /* 入れ子になった代入でも名前が衝突しないよう、一時変数名に一意な番号を付ける
         * （以前は固定名だったため、`(a, b), c = ...` で内側の宣言が外側を
         * 影にする -Wshadow 警告が生成Cに出ていた）。 */
        int unpack_id = ++cg->temp_counter;
        char unpack_src[96];
        snprintf(unpack_src, sizeof(unpack_src), "_p2c_unpack_src_%d", unpack_id);
        remember_synthetic_local(cg, unpack_src);
        indent(cg); write_str(cg, "P2C_Object *"); write_str(cg, unpack_src); write_str(cg, " = p2c_unpack_source("); gen_expr(cg, value); write_str(cg, ");"); write_newline(cg);
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
            indent(cg); write_str(cg, "int64_t _p2c_unpack_len = p2c_len("); write_str(cg, unpack_src); write_str(cg, ");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_unpack_len < "); emit_usize(cg, elts_len - 1); write_str(cg, ") p2c_raise(p2c_make_exception(\"ValueError\", \"not enough values to unpack\"));"); write_newline(cg);
            for (size_t i = 0; i < elts_len; i++) {
                P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
                if (i == star_index) {
                    if (elt->base.u.starred.value && elt->base.u.starred.value->base.type == AST_NAME) {
                        indent(cg); write_ident(cg, elt->base.u.starred.value->base.u.name.name); write_str(cg, " = p2c_builtin_list(p2c_obj_slice("); write_str(cg, unpack_src); write_str(cg, ", p2c_obj_from_int("); emit_usize(cg, i); write_str(cg, "), p2c_obj_from_int(_p2c_unpack_len - "); emit_usize(cg, trailing); write_str(cg, "), &P2C_None));"); write_newline(cg);
                    }
                    continue;
                }
                indent(cg); write_str(cg, "P2C_Object *_p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, " = p2c_subscript_get("); write_str(cg, unpack_src); write_str(cg, ", p2c_obj_from_int(");
                if (i < star_index) emit_usize(cg, i);
                else { write_str(cg, "_p2c_unpack_len - "); emit_usize(cg, trailing); write_str(cg, " + "); emit_usize(cg, i - star_index - 1); }
                write_str(cg, ")); "); write_newline(cg);
                if (elt->base.type == AST_NAME) { indent(cg); write_ident(cg, elt->base.u.name.name); write_str(cg, " = _p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, ";"); write_newline(cg); }
                else if (elt->base.type == AST_ATTRIBUTE) { indent(cg); write_str(cg, "p2c_setattr("); gen_expr(cg, elt->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, elt->base.u.attribute.attr); write_str(cg, "\", _p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, ");"); write_newline(cg); }
                else if (elt->base.type == AST_SUBSCRIPT) { indent(cg); write_str(cg, "p2c_subscript_set("); gen_expr(cg, elt->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, elt->base.u.subscript.slice); write_str(cg, ", _p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, ");"); write_newline(cg); }
                else if (elt->base.type == AST_TUPLE || elt->base.type == AST_LIST) {
                    /* 入れ子のアンパック ((a, b), c = ...)。以前は黙って無視され、
                     * 内側の名前が None のままになっていた。 */
                    char nested_tmp[96];
                    P2C_AstExpr synth;
                    snprintf(nested_tmp, sizeof(nested_tmp), "_p2c_unpack_item_%d_%zu", unpack_id, i);
                    memset(&synth, 0, sizeof(synth));
                    synth.base.type = AST_NAME;
                    synth.base.u.name.name = nested_tmp;
                    remember_synthetic_local(cg, nested_tmp);
                    gen_assign_target(cg, elt, &synth);
                }
            }
            pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
            return;
        }
        for (size_t i = 0; i < elts_len; i++) {
            P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
            indent(cg); write_str(cg, "P2C_Object *_p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, " = p2c_subscript_get("); write_str(cg, unpack_src); write_str(cg, ", p2c_obj_from_int(");
            emit_usize(cg, i);
            write_str(cg, "));"); write_newline(cg);
            if (elt->base.type == AST_NAME) { indent(cg); write_ident(cg, elt->base.u.name.name); write_str(cg, " = _p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, ";"); write_newline(cg); }
            else if (elt->base.type == AST_ATTRIBUTE) { indent(cg); write_str(cg, "p2c_setattr("); gen_expr(cg, elt->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, elt->base.u.attribute.attr); write_str(cg, "\", _p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, ");"); write_newline(cg); }
            else if (elt->base.type == AST_SUBSCRIPT) { indent(cg); write_str(cg, "p2c_subscript_set("); gen_expr(cg, elt->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, elt->base.u.subscript.slice); write_str(cg, ", _p2c_unpack_item_"); emit_usize(cg, (size_t)unpack_id); write_str(cg, "_"); emit_usize(cg, (size_t)i); write_str(cg, ");"); write_newline(cg); }
            else if (elt->base.type == AST_TUPLE || elt->base.type == AST_LIST) {
                /* 入れ子のアンパック ((a, b), c = ...)。 */
                char nested_tmp2[96];
                P2C_AstExpr synth2;
                snprintf(nested_tmp2, sizeof(nested_tmp2), "_p2c_unpack_item_%d_%zu", unpack_id, i);
                memset(&synth2, 0, sizeof(synth2));
                synth2.base.type = AST_NAME;
                synth2.base.u.name.name = nested_tmp2;
                remember_synthetic_local(cg, nested_tmp2);
                gen_assign_target(cg, elt, &synth2);
            }
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
                if (name) {
                    remember_declared(cg, name);
                    /* `global` 名は関数の局所ではない。改名（p2c_loc_）すると大域変数を指せなくなる。 */
                    if (cg->global_decl_names) map_set_name(cg->global_decl_names, name);
                }
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
        /* 引数はタプルとして保持し、`.args` が CPython と同じ値になるようにする
         * （表示用メッセージは KeyError だけ repr になる）。 */
        if (p2c_vec_len(exc->base.u.call.args) > 0) {
            write_str(cg, "p2c_make_exception_with_args(\"");
            write_str(cg, exc->base.u.call.func->base.u.name.name);
            write_str(cg, "\", ");
            emit_args_array(cg, exc->base.u.call.args);
            write_str(cg, ")");
        } else {
            write_str(cg, "p2c_make_exception(\"");
            write_str(cg, exc->base.u.call.func->base.u.name.name);
            write_str(cg, "\", \"\")");
        }
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
            /* 合成した名前は宣言済みとして登録する（未定義名の NameError 化から除外）。 */
            remember_declared(cg, with_value_name);
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

/* ── 汎用ジェネレータ状態機械のための事前解析 ───────────────────────────
 * 既存の gen_suspension_function は「本体のトップレベル文」しか落とせず、
 * for/while/if の内側にある yield を扱えない。木を歩いて
 *   1. yield が制御構造の内側にあるか（＝汎用低減が必要か）
 *   2. その文を状態機械へ落とせるか
 * を判定し、必要な場合だけ gen_generator_function へ回す。
 * lambda・内包表記は別スコープなので内側へは入らない。 */

static bool expr_has_suspension(P2C_AstExpr *e) {
    if (!e) return false;
    switch (e->base.type) {
        case AST_YIELD:
        case AST_AWAIT:
            return true;
        case AST_BINOP:
            return expr_has_suspension(e->base.u.binop.left) || expr_has_suspension(e->base.u.binop.right);
        case AST_UNARYOP:
            return expr_has_suspension(e->base.u.unaryop.operand);
        case AST_COMPARE: {
            if (expr_has_suspension(e->base.u.compare.left)) return true;
            P2C_Vector *cs = e->base.u.compare.comparators;
            for (size_t i = 0; cs && i < p2c_vec_len(cs); i++)
                if (expr_has_suspension((P2C_AstExpr*)p2c_vec_get(cs, i))) return true;
            return false;
        }
        case AST_BOOLOP: {
            P2C_Vector *vs = e->base.u.boolop.values;
            for (size_t i = 0; vs && i < p2c_vec_len(vs); i++)
                if (expr_has_suspension((P2C_AstExpr*)p2c_vec_get(vs, i))) return true;
            return false;
        }
        case AST_CALL: {
            if (expr_has_suspension(e->base.u.call.func)) return true;
            P2C_Vector *as = e->base.u.call.args;
            for (size_t i = 0; as && i < p2c_vec_len(as); i++)
                if (expr_has_suspension((P2C_AstExpr*)p2c_vec_get(as, i))) return true;
            P2C_Vector *ks = e->base.u.call.keywords;
            for (size_t i = 0; ks && i < p2c_vec_len(ks); i++) {
                P2C_AstKeyword *kw = (P2C_AstKeyword*)p2c_vec_get(ks, i);
                if (kw && expr_has_suspension(kw->value)) return true;
            }
            return false;
        }
        case AST_ATTRIBUTE:
            return expr_has_suspension(e->base.u.attribute.value);
        case AST_SUBSCRIPT:
            return expr_has_suspension(e->base.u.subscript.value) || expr_has_suspension(e->base.u.subscript.slice);
        case AST_IFEXP:
            return expr_has_suspension(e->base.u.ifexp.test) || expr_has_suspension(e->base.u.ifexp.body) ||
                   expr_has_suspension(e->base.u.ifexp.orelse);
        case AST_STARRED:
            return expr_has_suspension(e->base.u.starred.value);
        case AST_NAMED_EXPR:
            return expr_has_suspension(e->base.u.named_expr.value);
        case AST_LIST: case AST_TUPLE: case AST_SET: {
            P2C_Vector *elts = e->base.u.list.elts;
            for (size_t i = 0; elts && i < p2c_vec_len(elts); i++)
                if (expr_has_suspension((P2C_AstExpr*)p2c_vec_get(elts, i))) return true;
            return false;
        }
        case AST_DICT: {
            P2C_Vector *ks = e->base.u.dict.keys;
            P2C_Vector *vs = e->base.u.dict.values;
            for (size_t i = 0; ks && i < p2c_vec_len(ks); i++)
                if (expr_has_suspension((P2C_AstExpr*)p2c_vec_get(ks, i))) return true;
            for (size_t i = 0; vs && i < p2c_vec_len(vs); i++)
                if (expr_has_suspension((P2C_AstExpr*)p2c_vec_get(vs, i))) return true;
            return false;
        }
        default:
            return false;
    }
}

static bool stmt_list_has_suspension(P2C_Vector *stmts);

static bool stmt_has_suspension(P2C_AstStmt *stmt) {
    if (!stmt) return false;
    switch (stmt->base.type) {
        case AST_EXPR_STMT:  return expr_has_suspension(stmt->base.u.expr_stmt.value);
        case AST_ASSIGN:     return expr_has_suspension(stmt->base.u.assign.value);
        case AST_AUGASSIGN:  return expr_has_suspension(stmt->base.u.augassign.value);
        case AST_RETURN:     return expr_has_suspension(stmt->base.u.return_stmt.value);
        case AST_ANNASSIGN:  return expr_has_suspension(stmt->base.u.annassign.value);
        case AST_IF:
            return expr_has_suspension(stmt->base.u.if_stmt.test) ||
                   stmt_list_has_suspension(stmt->base.u.if_stmt.body) ||
                   stmt_list_has_suspension(stmt->base.u.if_stmt.orelse);
        case AST_WHILE:
            return expr_has_suspension(stmt->base.u.while_stmt.test) ||
                   stmt_list_has_suspension(stmt->base.u.while_stmt.body) ||
                   stmt_list_has_suspension(stmt->base.u.while_stmt.orelse);
        case AST_FOR:
        case AST_ASYNC_FOR:
            return expr_has_suspension(stmt->base.u.for_stmt.iter) ||
                   stmt_list_has_suspension(stmt->base.u.for_stmt.body) ||
                   stmt_list_has_suspension(stmt->base.u.for_stmt.orelse);
        case AST_WITH:
            return stmt_list_has_suspension(stmt->base.u.with.body);
        case AST_TRY: {
            if (stmt_list_has_suspension(stmt->base.u.try_stmt.body)) return true;
            if (stmt_list_has_suspension(stmt->base.u.try_stmt.orelse)) return true;
            if (stmt_list_has_suspension(stmt->base.u.try_stmt.finalbody)) return true;
            P2C_Vector *hs = stmt->base.u.try_stmt.handlers;
            for (size_t i = 0; hs && i < p2c_vec_len(hs); i++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(hs, i);
                if (h && stmt_list_has_suspension(h->body)) return true;
            }
            return false;
        }
        case AST_BLOCK:
            return stmt_list_has_suspension(stmt->base.u.block.stmts);
        default:
            return false;
    }
}

static bool stmt_list_has_suspension(P2C_Vector *stmts) {
    for (size_t i = 0; stmts && i < p2c_vec_len(stmts); i++)
        if (stmt_has_suspension((P2C_AstStmt*)p2c_vec_get(stmts, i))) return true;
    return false;
}

/* yield が制御構造（for/while/if/try/with）の内側にある同期ジェネレータは、
 * 汎用状態機械（gen_generator_function）で低減する必要がある。 */
static bool generator_needs_state_machine(P2C_AstFunctionDef *fd) {
    if (!fd || fd->is_async || !fd->body) return false;
    for (size_t i = 0; i < p2c_vec_len(fd->body); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(fd->body, i);
        if (!stmt) continue;
        switch (stmt->base.type) {
            case AST_FOR: case AST_ASYNC_FOR: case AST_WHILE: case AST_IF:
            case AST_TRY: case AST_WITH: case AST_BLOCK:
                if (stmt_has_suspension(stmt)) return true;
                break;
            case AST_ASSIGN:
            case AST_AUGASSIGN:
                /* x = yield v / x += (yield v) は既存のトップレベル状態機械が
                 * 扱えない（await 専用の経路しか無い）ため汎用側へ回す。 */
                if (stmt_has_suspension(stmt)) return true;
                break;
            default:
                break;
        }
    }
    return false;
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

/* yield を含まない if の本体（入れ子を含む）で代入される名前も、ジェネレータの局所に入れる */
static void suspension_collect_if_locals(P2C_Map *locals, P2C_Vector *stmts);
static void suspension_collect_one_local(P2C_Map *locals, P2C_AstStmt *stmt) {
    if (!stmt) return;
        if (stmt->base.type == AST_ASSIGN) {
            for (size_t j = 0; j < p2c_vec_len(stmt->base.u.assign.targets); j++) {
                suspension_collect_local(locals, (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, j));
            }
        } else if (stmt->base.type == AST_AUGASSIGN) {
            suspension_collect_local(locals, stmt->base.u.augassign.target);
        } else if (stmt->base.type == AST_IF) {
            suspension_collect_if_locals(locals, stmt->base.u.if_stmt.body);
            suspension_collect_if_locals(locals, stmt->base.u.if_stmt.orelse);
        } else if (stmt->base.type == AST_FOR && !stmt_has_suspension(stmt)) {
            suspension_collect_local(locals, stmt->base.u.for_stmt.target);
            suspension_collect_if_locals(locals, stmt->base.u.for_stmt.body);
        } else if (stmt->base.type == AST_WHILE && !stmt_has_suspension(stmt)) {
            suspension_collect_if_locals(locals, stmt->base.u.while_stmt.body);
        } else if (stmt->base.type == AST_TRY && !stmt_has_suspension(stmt)) {
            suspension_collect_if_locals(locals, stmt->base.u.try_stmt.body);
            for (size_t k = 0; k < p2c_vec_len(stmt->base.u.try_stmt.handlers); k++) {
                P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(stmt->base.u.try_stmt.handlers, k);
                if (!h) continue;
                if (h->name) map_set_name(locals, h->name);
                suspension_collect_if_locals(locals, h->body);
            }
        }
}

static void suspension_collect_if_locals(P2C_Map *locals, P2C_Vector *stmts) {
    for (size_t i = 0; stmts && i < p2c_vec_len(stmts); i++) {
        suspension_collect_one_local(locals, (P2C_AstStmt*)p2c_vec_get(stmts, i));
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
        } else if ((stmt->base.type == AST_IF || stmt->base.type == AST_FOR || stmt->base.type == AST_WHILE || stmt->base.type == AST_TRY) && !stmt_has_suspension(stmt)) {
            suspension_collect_one_local(locals, stmt);
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
        /* 単項演算子を落とさない（以前は not / ~ を無視して被演算子だけを出していた） */
        if (n->u.unaryop.op == OP_USUB) {
            write_str(cg, "p2c_obj_neg(");
            gen_suspension_expr(cg, n->u.unaryop.operand, locals); write_str(cg, ")");
        } else if (n->u.unaryop.op == OP_NOT) {
            write_str(cg, "p2c_obj_from_bool(!p2c_obj_is_truthy(");
            gen_suspension_expr(cg, n->u.unaryop.operand, locals); write_str(cg, "))");
        } else if (n->u.unaryop.op == OP_INVERT) {
            write_str(cg, "p2c_obj_invert(");
            gen_suspension_expr(cg, n->u.unaryop.operand, locals); write_str(cg, ")");
        } else if (n->u.unaryop.op == OP_UADD) {
            gen_suspension_expr(cg, n->u.unaryop.operand, locals);
        } else {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, n->line, n->col, "unary operator", "this unary operator is not supported in a generator body");
            write_str(cg, "&P2C_None");
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
        } else if (strncmp(name, "p2c_", 4) == 0) {
            /* パーサが合成した内部ランタイム呼び出し（p2c_obj_slice 等）は、
             * そのままC関数呼び出しとして出力する。p2c_call で包むと関数
             * ポインタを第1引数に渡す形になり、生成Cが型エラーで落ちる。 */
            write_ident(cg, name); write_str(cg, "(");
            for (size_t i = 0; i < argc; i++) {
                if (i) write_str(cg, ", ");
                gen_suspension_expr(cg, (P2C_AstExpr*)p2c_vec_get(n->u.call.args, i), locals);
            }
            write_str(cg, ")");
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
    /* ステップ関数は後方（forward の末尾）へ回すことがあるため、前方宣言を置く
     * （入れ子のジェネレータ式で未宣言参照にならないように）。 */
    p2c_str_append(cg->header, "static P2C_Object *");
    p2c_str_append(cg->header, step_name);
    p2c_str_append(cg->header, "(P2C_Object *generator);\n");
    snprintf(level_local, sizeof(level_local), "__p2c_genexp_level_%d", id);
    snprintf(iter_local, sizeof(iter_local), "__p2c_genexp_iter_%d_0", id);
    saved_current = cg->current;
    saved_indent = cg->indent_level;
    /* 入れ子のジェネレータ式（x for x in (y for y in ...)）では、内側の
     * ステップ関数を forward へ直接書くと外側の関数定義の途中へ割り込んで
     * 壊れたCになる。一時バッファへ退避し、最後に forward の末尾へ連結する。 */
    bool deferred_step = (cg->current == cg->forward);
    P2C_String *step_buf = deferred_step ? p2c_str_new(cg->alloc) : cg->forward;
    if (!step_buf) step_buf = cg->forward;
    cg->current = step_buf;
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
            write_str(cg, "p2c_cell_get(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", "); write_name_lit_str(cg, name); write_str(cg, "))");
        } else if (map_has_name(cg->cell_names, name)) {
            write_str(cg, "p2c_cell_get(_p2c_cell_"); write_ident(cg, name); write_str(cg, ")");
        } else {
            write_ident(cg, name);
        }
    }
    write_str(cg, "}, "); emit_usize(cg, capture_count + 2); write_str(cg, ")");
    if (deferred_step && step_buf != cg->forward) {
        /* 退避したステップ関数は、外側の定義が書き終わってから forward の末尾へ
         * 連結する（deferred_defs へ積み、generate() 側でまとめて連結する）。 */
        if (!cg->deferred_defs) cg->deferred_defs = p2c_vec_new(cg->alloc, NULL);
        if (cg->deferred_defs) (void)p2c_vec_push(cg->deferred_defs, step_buf);
        else p2c_str_free(step_buf);
    }
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
                case OP_ADD: func = "p2c_obj_iadd"; break;
                case OP_SUB: func = "p2c_obj_isub"; break;
                case OP_MULT: func = "p2c_obj_imul"; break;
                case OP_DIV: func = "p2c_obj_div"; break;
                case OP_FLOORDIV: func = "p2c_obj_floordiv"; break;
                case OP_MOD: func = "p2c_obj_mod"; break;
                case OP_POW: func = "p2c_obj_pow"; break;
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

/* yield を含まない if 文を、状態機械の1状態として下ろす。本体の代入・式は、
 * ジェネレータの局所へ保存する形（gen_suspension_local_set / gen_suspension_expr）で書く。
 * 本体に yield や、対応しない文（return など）がある場合は、変換時に診断する。 */
static void gen_plain_suspension_stmts(P2C_CodeGen *cg, P2C_Vector *stmts, P2C_Map *locals);
/* yield を含まない for / while を、C のループとして下ろす。反復は StopIteration で終わり、
 * break / continue は C の制御構造がそのまま扱う（本体は普通の C の文）。 */
static void gen_plain_suspension_for(P2C_CodeGen *cg, P2C_AstStmt *stmt, P2C_Map *locals) {
    if (p2c_vec_len(stmt->base.u.for_stmt.orelse) > 0) {
        codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col, "for statement with else", "for-else inside a generator body is not supported yet");
        return;
    }
    P2C_AstExpr *target = stmt->base.u.for_stmt.target;
    if (!target || target->base.type != AST_NAME) {
        codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col, "for statement target", "only a simple name target is supported in a generator body");
        return;
    }
    char id[48];
    snprintf(id, sizeof(id), "%u_%u", (unsigned)stmt->base.line, (unsigned)stmt->base.col);
    indent(cg); write_str(cg, "{ P2C_Object *_p2c_gfor_it_"); write_str(cg, id); write_str(cg, " = p2c_builtin_iter("); gen_suspension_expr(cg, stmt->base.u.for_stmt.iter, locals); write_str(cg, ");"); write_newline(cg);
    indent(cg); write_str(cg, "for (;;) {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_gfor_ef_"); write_str(cg, id); write_str(cg, "; _p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ".prev = p2c_exc_stack; _p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_gfor_item_"); write_str(cg, id); write_str(cg, " = &P2C_None;"); write_newline(cg);
    indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ".env) == 0) { _p2c_gfor_item_"); write_str(cg, id); write_str(cg, " = p2c_builtin_next(_p2c_gfor_it_"); write_str(cg, id); write_str(cg, "); p2c_exc_stack = _p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ".prev; }"); write_newline(cg);
    indent(cg); write_str(cg, "else { P2C_Object *_p2c_gfor_exc_"); write_str(cg, id); write_str(cg, " = _p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ".exc; p2c_exc_stack = _p2c_gfor_ef_"); write_str(cg, id); write_str(cg, ".prev; if (!p2c_exc_name_match(_p2c_gfor_exc_"); write_str(cg, id); write_str(cg, ", \"StopIteration\")) p2c_raise(_p2c_gfor_exc_"); write_str(cg, id); write_str(cg, "); break; }"); write_newline(cg);
    indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, target->base.u.name.name); write_str(cg, "\", _p2c_gfor_item_"); write_str(cg, id); write_str(cg, ");"); write_newline(cg);
    gen_plain_suspension_stmts(cg, stmt->base.u.for_stmt.body, locals);
    pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    indent(cg); write_str(cg, "}"); write_newline(cg);
}
static void gen_plain_suspension_while(P2C_CodeGen *cg, P2C_AstStmt *stmt, P2C_Map *locals) {
    if (p2c_vec_len(stmt->base.u.while_stmt.orelse) > 0) {
        codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col, "while statement with else", "while-else inside a generator body is not supported yet");
        return;
    }
    indent(cg); write_str(cg, "while (p2c_obj_is_truthy("); gen_suspension_expr(cg, stmt->base.u.while_stmt.test, locals); write_str(cg, ")) {"); write_newline(cg);
    push_indent(cg); gen_plain_suspension_stmts(cg, stmt->base.u.while_stmt.body, locals); pop_indent(cg);
    indent(cg); write_str(cg, "}"); write_newline(cg);
}
/* yield を含まない try / except を下ろす。本体は C の例外フレームの中で実行する（ジェネレータの
 * 状態機械は再開のたびに C のフレームを失うため、yield をまたぐ try はここでは扱わない）。 */
static void gen_plain_suspension_try(P2C_CodeGen *cg, P2C_AstStmt *stmt, P2C_Map *locals) {
    P2C_AstTry *t = &stmt->base.u.try_stmt;
    if (p2c_vec_len(t->finalbody) > 0 || p2c_vec_len(t->orelse) > 0) {
        codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col, "try statement with finally or else", "finally and else clauses of a try inside a generator body are not supported yet");
        return;
    }
    if (p2c_vec_len(t->handlers) == 0) {
        codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col, "try statement without except", "a try inside a generator body needs an except clause");
        return;
    }
    char id[48];
    snprintf(id, sizeof(id), "%u_%u", (unsigned)stmt->base.line, (unsigned)stmt->base.col);
    indent(cg); write_str(cg, "{ P2C_ExceptFrame _p2c_gtry_ef_"); write_str(cg, id); write_str(cg, "; _p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ".prev = p2c_exc_stack; _p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ";"); write_newline(cg);
    indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ".env) == 0) {"); write_newline(cg); push_indent(cg);
    gen_plain_suspension_stmts(cg, t->body, locals);
    indent(cg); write_str(cg, "p2c_exc_stack = _p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ".prev;"); write_newline(cg);
    pop_indent(cg); indent(cg); write_str(cg, "} else {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *_p2c_gtry_exc_"); write_str(cg, id); write_str(cg, " = _p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ".exc; p2c_exc_stack = _p2c_gtry_ef_"); write_str(cg, id); write_str(cg, ".prev;"); write_newline(cg);
    bool first = true;
    for (size_t k = 0; k < p2c_vec_len(t->handlers); k++) {
        P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(t->handlers, k);
        if (!h) continue;
        indent(cg); write_str(cg, first ? "if (" : "else if (");
        first = false;
        if (!h->type) {
            write_str(cg, "true");
        } else if (h->type->base.type == AST_NAME) {
            write_str(cg, "p2c_exc_name_match(_p2c_gtry_exc_"); write_str(cg, id); write_str(cg, ", \""); write_str(cg, h->type->base.u.name.name); write_str(cg, "\")");
        } else if (h->type->base.type == AST_TUPLE) {
            bool any = false;
            for (size_t j = 0; j < p2c_vec_len(h->type->base.u.tuple.elts); j++) {
                P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(h->type->base.u.tuple.elts, j);
                if (!e || e->base.type != AST_NAME) continue;
                if (any) write_str(cg, " || ");
                any = true;
                write_str(cg, "p2c_exc_name_match(_p2c_gtry_exc_"); write_str(cg, id); write_str(cg, ", \""); write_str(cg, e->base.u.name.name); write_str(cg, "\")");
            }
            if (!any) write_str(cg, "false");
        } else {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, h->type->base.line, h->type->base.col, "except clause type", "only names and tuples of names are supported in a generator body");
            write_str(cg, "false");
        }
        write_str(cg, ") {"); write_newline(cg); push_indent(cg);
        if (h->name) {
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, h->name); write_str(cg, "\", _p2c_gtry_exc_"); write_str(cg, id); write_str(cg, ");"); write_newline(cg);
        }
        gen_plain_suspension_stmts(cg, h->body, locals);
        pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    }
    indent(cg); write_str(cg, first ? "p2c_raise(_p2c_gtry_exc_" : "else p2c_raise(_p2c_gtry_exc_"); write_str(cg, id); write_str(cg, ");"); write_newline(cg);
    pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
    indent(cg); write_str(cg, "}"); write_newline(cg);
}
static void gen_plain_suspension_if(P2C_CodeGen *cg, P2C_AstStmt *stmt, P2C_Map *locals) {
    indent(cg); write_str(cg, "if (p2c_obj_is_truthy("); gen_suspension_expr(cg, stmt->base.u.if_stmt.test, locals); write_str(cg, ")) {"); write_newline(cg);
    push_indent(cg); gen_plain_suspension_stmts(cg, stmt->base.u.if_stmt.body, locals); pop_indent(cg);
    indent(cg); write_str(cg, "} else {"); write_newline(cg);
    push_indent(cg); gen_plain_suspension_stmts(cg, stmt->base.u.if_stmt.orelse, locals); pop_indent(cg);
    indent(cg); write_str(cg, "}"); write_newline(cg);
}
static void gen_plain_suspension_stmts(P2C_CodeGen *cg, P2C_Vector *stmts, P2C_Map *locals) {
    for (size_t i = 0; stmts && i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *s = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!s || s->base.type == AST_PASS) continue;
        if (s->base.type == AST_ASSIGN && p2c_vec_len(s->base.u.assign.targets) == 1 && !stmt_has_suspension(s)) {
            gen_suspension_local_set(cg, (P2C_AstExpr*)p2c_vec_get(s->base.u.assign.targets, 0), s->base.u.assign.value, locals);
        } else if (s->base.type == AST_EXPR_STMT && !stmt_has_suspension(s)) {
            indent(cg); write_str(cg, "(void)"); gen_suspension_expr(cg, s->base.u.expr_stmt.value, locals); write_str(cg, ";"); write_newline(cg);
        } else if (s->base.type == AST_IF && !stmt_has_suspension(s)) {
            gen_plain_suspension_if(cg, s, locals);
        } else if (s->base.type == AST_FOR && !stmt_has_suspension(s)) {
            gen_plain_suspension_for(cg, s, locals);
        } else if (s->base.type == AST_WHILE && !stmt_has_suspension(s)) {
            gen_plain_suspension_while(cg, s, locals);
        } else if (s->base.type == AST_TRY && !stmt_has_suspension(s)) {
            gen_plain_suspension_try(cg, s, locals);
        } else if (s->base.type == AST_BREAK) {
            write_line(cg, "break;");
        } else if (s->base.type == AST_CONTINUE) {
            write_line(cg, "continue;");
        } else {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, s->base.line, s->base.col, stmt_kind_label(s),
                                 "this statement inside an if in a generator body cannot be lowered yet");
            write_line(cg, "p2c_raise(p2c_make_exception(\"NotImplementedError\", \"unsupported statement in generator\"));");
        }
    }
}

static void gen_suspension_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    P2C_Map *locals = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!locals) { codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate state-machine local map"); return; }
    suspension_collect_locals(locals, fd);
    size_t argc = p2c_vec_len(fd->args);

    p2c_str_append(cg->forward, "static P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->name)); p2c_str_append(cg->forward, "__step(P2C_Object *generator);\n");
    /* クラスメソッドはクラス側のプロトタイプ宣言と重複するため出さない
     * （-Wredundant-decls 対策）。 */
    if (!cg->current_class) {
    p2c_str_append(cg->forward, "static P2C_Object *"); p2c_str_append(cg->forward, mangle_ident(fd->name)); p2c_str_append(cg->forward, "(");
    for (size_t i = 0; i < argc; i++) {
        if (i) p2c_str_append(cg->forward, ", ");
        p2c_str_append(cg->forward, "P2C_Object *");
        p2c_str_append(cg->forward, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    }
    if (argc == 0) p2c_str_append(cg->forward, "void");
    p2c_str_append(cg->forward, ");\n");
    }

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
        } else if ((stmt->base.type == AST_IF || stmt->base.type == AST_FOR || stmt->base.type == AST_WHILE || stmt->base.type == AST_TRY) && !stmt_has_suspension(stmt)) {
            /* yield を含まない if / for / while / try: 1状態として下ろし、終わったら次の状態へ進む */
            if (stmt->base.type == AST_IF) gen_plain_suspension_if(cg, stmt, locals);
            else if (stmt->base.type == AST_FOR) gen_plain_suspension_for(cg, stmt, locals);
            else if (stmt->base.type == AST_TRY) gen_plain_suspension_try(cg, stmt, locals);
            else gen_plain_suspension_while(cg, stmt, locals);
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, suspension_state_base(fd, i + 1)); write_str(cg, "); return "); write_ident(cg, fd->name); write_str(cg, "__step(generator);"); write_newline(cg);
        } else {
            /* 状態機械へ落とせない文。以前はここで「実行時に
             * NotImplementedError を投げるC」を生成していたため、
             * 未対応構文が変換時に検出されず、生成物が実行時に落ちていた。
             * 変換時点の位置つき診断へ変更する。 */
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED,
                                 stmt ? stmt->base.line : 0u, stmt ? stmt->base.col : 0u,
                                 stmt_kind_label(stmt),
                                 "a yield/await inside it cannot be lowered to the state machine yet");
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
        write_str(cg, "P2C_Object *"); write_param_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    }
    if (argc == 0) write_str(cg, "void");
    write_str(cg, ") {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *generator = p2c_generator_new("); write_ident(cg, fd->name); write_str(cg, "__step, "); write_str(cg, fd->is_async ? "true" : "false"); write_str(cg, ");"); write_newline(cg);
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, arg->name); write_str(cg, "\", "); write_param_ident(cg, arg->name); write_str(cg, ");"); write_newline(cg);
    }
    write_line(cg, "return generator;");
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
    free_name_map(locals);
}

/* ============================================================
 * 汎用ジェネレータ状態機械（ループ内 yield 対応）
 *
 * 生成するステップ関数の骨格:
 *   static P2C_Object *NAME__step(P2C_Object *generator) {
 *       for (;;) {
 *           switch (p2c_generator_state(generator)) {
 *               case 0: { ... }        // 各状態
 *               ...
 *               default: return p2c_generator_finish(generator, &P2C_None);
 *           }
 *       }
 *   }
 * 状態遷移は「p2c_generator_set_state(generator, N); continue;」で行う。
 * return step(...) を連ねる再帰方式だと、yield を含まないループを長く回した
 * ときに C スタックを消費し尽くす（テールコール最適化に依存してしまう）ため、
 * ドライバループ方式にしてスタック使用量を状態数に依存させない。
 * yield は値を返すためにいったん関数から戻る（再開時は状態 N から）。
 * ============================================================ */

typedef struct {
    P2C_CodeGen *cg;
    P2C_Map     *locals;
    /* break で脱出するとき、最内ループの for イテレータを破棄するためのローカル名。
     * 未使用なら NULL（for ループ以外）。 */
    const char *break_iter_local;
} P2C_GenLoop;

static void genloop_goto(P2C_GenLoop *gl, size_t state) {
    P2C_CodeGen *cg = gl->cg;
    indent(cg); write_str(cg, "p2c_generator_set_state(generator, ");
    emit_usize(cg, state); write_str(cg, "); continue;"); write_newline(cg);
}

static void genloop_open_case(P2C_GenLoop *gl, size_t state) {
    P2C_CodeGen *cg = gl->cg;
    indent(cg); write_str(cg, "case "); emit_usize(cg, state); write_str(cg, ": {");
    write_newline(cg); push_indent(cg);
}

static void genloop_close_case(P2C_GenLoop *gl) {
    pop_indent(gl->cg);
    indent(gl->cg); write_str(gl->cg, "}"); write_newline(gl->cg);
}

/* 各文の状態数（entry から exit までに消費する状態数）。
 * エミッタ（genloop_emit_stmt_list）と同じ規則で数える必要がある。 */
static size_t genloop_width_stmt_list(P2C_Vector *stmts);

static size_t genloop_width_stmt(P2C_AstStmt *stmt) {
    if (!stmt) return 1;
    switch (stmt->base.type) {
        case AST_PASS: case AST_IMPORT: case AST_IMPORTFROM: case AST_GLOBAL:
        case AST_NONLOCAL: case AST_DELETE: case AST_BREAK: case AST_CONTINUE:
        case AST_ANNASSIGN:
            return 1;
        case AST_ASSIGN:    return expr_has_suspension(stmt->base.u.assign.value) ? 2u : 1u;
        case AST_AUGASSIGN: return expr_has_suspension(stmt->base.u.augassign.value) ? 2u : 1u;
        case AST_EXPR_STMT: return expr_has_suspension(stmt->base.u.expr_stmt.value) ? 2u : 1u;
        case AST_RETURN:    return expr_has_suspension(stmt->base.u.return_stmt.value) ? 2u : 1u;
        case AST_RAISE:     return 1u;
        case AST_BLOCK:     return genloop_width_stmt_list(stmt->base.u.block.stmts);
        case AST_IF:
            if (expr_has_suspension(stmt->base.u.if_stmt.test)) return 1u;
            return 1u + genloop_width_stmt_list(stmt->base.u.if_stmt.body) +
                        genloop_width_stmt_list(stmt->base.u.if_stmt.orelse);
        case AST_WHILE:
            if (expr_has_suspension(stmt->base.u.while_stmt.test)) return 1u;
            return 1u + genloop_width_stmt_list(stmt->base.u.while_stmt.body) +
                        genloop_width_stmt_list(stmt->base.u.while_stmt.orelse);
        case AST_FOR:
            if (expr_has_suspension(stmt->base.u.for_stmt.iter)) return 1u;
            return 2u + genloop_width_stmt_list(stmt->base.u.for_stmt.body) +
                        genloop_width_stmt_list(stmt->base.u.for_stmt.orelse);
        default:
            return 1u;
    }
}

static size_t genloop_width_stmt_list(P2C_Vector *stmts) {
    size_t total = 0;
    for (size_t i = 0; stmts && i < p2c_vec_len(stmts); i++)
        total += genloop_width_stmt((P2C_AstStmt*)p2c_vec_get(stmts, i));
    return total;
}

/* ジェネレータのローカルとして保持すべき名前を、ループ/分岐の内側も含めて集める。 */
static void genloop_collect_names(P2C_Map *locals, P2C_AstExpr *target) {
    if (!locals || !target) return;
    if (target->base.type == AST_NAME) {
        map_set_name(locals, target->base.u.name.name);
    } else if (target->base.type == AST_STARRED) {
        genloop_collect_names(locals, target->base.u.starred.value);
    } else if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.u.list.elts;
        for (size_t i = 0; elts && i < p2c_vec_len(elts); i++)
            genloop_collect_names(locals, (P2C_AstExpr*)p2c_vec_get(elts, i));
    }
}

static void genloop_collect_stmt_list(P2C_Map *locals, P2C_Vector *stmts) {
    for (size_t i = 0; stmts && i < p2c_vec_len(stmts); i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        if (!stmt) continue;
        switch (stmt->base.type) {
            case AST_ASSIGN:
                for (size_t j = 0; j < p2c_vec_len(stmt->base.u.assign.targets); j++)
                    genloop_collect_names(locals, (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, j));
                break;
            case AST_AUGASSIGN: genloop_collect_names(locals, stmt->base.u.augassign.target); break;
            case AST_ANNASSIGN: genloop_collect_names(locals, stmt->base.u.annassign.target); break;
            case AST_FOR:
            case AST_ASYNC_FOR:
                genloop_collect_names(locals, stmt->base.u.for_stmt.target);
                genloop_collect_stmt_list(locals, stmt->base.u.for_stmt.body);
                genloop_collect_stmt_list(locals, stmt->base.u.for_stmt.orelse);
                break;
            case AST_IF:
                genloop_collect_stmt_list(locals, stmt->base.u.if_stmt.body);
                genloop_collect_stmt_list(locals, stmt->base.u.if_stmt.orelse);
                break;
            case AST_WHILE:
                genloop_collect_stmt_list(locals, stmt->base.u.while_stmt.body);
                genloop_collect_stmt_list(locals, stmt->base.u.while_stmt.orelse);
                break;
            case AST_WITH:
                for (size_t j = 0; j < p2c_vec_len(stmt->base.u.with.items); j++) {
                    P2C_AstWithItem *item = (P2C_AstWithItem*)p2c_vec_get(stmt->base.u.with.items, j);
                    if (item) genloop_collect_names(locals, item->optional_vars);
                }
                genloop_collect_stmt_list(locals, stmt->base.u.with.body);
                break;
            case AST_TRY:
                genloop_collect_stmt_list(locals, stmt->base.u.try_stmt.body);
                genloop_collect_stmt_list(locals, stmt->base.u.try_stmt.orelse);
                genloop_collect_stmt_list(locals, stmt->base.u.try_stmt.finalbody);
                for (size_t j = 0; j < p2c_vec_len(stmt->base.u.try_stmt.handlers); j++) {
                    P2C_AstExceptHandler *h = (P2C_AstExceptHandler*)p2c_vec_get(stmt->base.u.try_stmt.handlers, j);
                    if (!h) continue;
                    if (h->name) map_set_name(locals, h->name);
                    genloop_collect_stmt_list(locals, h->body);
                }
                break;
            case AST_BLOCK:
                genloop_collect_stmt_list(locals, stmt->base.u.block.stmts);
                break;
            default:
                break;
        }
    }
}

static void genloop_emit_simple(P2C_GenLoop *gl, P2C_AstStmt *stmt, size_t entry, size_t exit,
                                size_t continue_state, size_t break_state);

/* ジェネレータのローカルへ代入する（単純名・タプル/リスト展開に対応）。 */
static void genloop_bind_target(P2C_GenLoop *gl, P2C_AstExpr *target, const char *item_var) {
    P2C_CodeGen *cg = gl->cg;
    if (!target) return;
    if (target->base.type == AST_NAME) {
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \"");
        write_str(cg, target->base.u.name.name); write_str(cg, "\", "); write_str(cg, item_var);
        write_str(cg, ");"); write_newline(cg);
        return;
    }
    if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.u.list.elts;
        for (size_t i = 0; elts && i < p2c_vec_len(elts); i++) {
            P2C_AstExpr *elt = (P2C_AstExpr*)p2c_vec_get(elts, i);
            char tmp[80];
            snprintf(tmp, sizeof(tmp), "_p2c_unpack_%zu", i);
            indent(cg); write_str(cg, "P2C_Object *"); write_str(cg, tmp); write_str(cg, " = p2c_subscript_get(");
            write_str(cg, item_var); write_str(cg, ", p2c_obj_from_int("); emit_usize(cg, i); write_str(cg, "));");
            write_newline(cg);
            genloop_bind_target(gl, elt, tmp);
        }
        return;
    }
    codegen_set_error_at(gl->cg, P2C_ERR_NOT_IMPLEMENTED, target->base.line, target->base.col,
                         "assignment/unpacking target",
                         "generator state machine supports names and tuple/list targets only");
}

/* 文リストを再帰的に状態機械へ落とす。
 *   entry          : このリストの先頭状態
 *   exit           : リストを最後まで実行したあとに遷移する状態
 *   continue_state : 内側の continue が飛ぶ先（最内ループの再開状態）
 *   break_state    : 内側の break が飛ぶ先（最内ループの直後） */
static void genloop_emit_stmt_list(P2C_GenLoop *gl, P2C_Vector *stmts, size_t entry, size_t exit,
                                   size_t continue_state, size_t break_state) {
    size_t cursor = entry;
    size_t count = p2c_vec_len(stmts);
    if (count == 0) {
        /* 空リストでは entry == exit のことが多い（例: else 節が空）。
         * その場合に case を出すと、次の文と同じ状態番号になって
         * "duplicate case value" の生成Cになる（＝コンパイル不能）。
         * 遷移が本当に必要なときだけ case を出す。 */
        if (entry != exit) {
            genloop_open_case(gl, entry);
            genloop_goto(gl, exit);
            genloop_close_case(gl);
        }
        return;
    }
    for (size_t i = 0; i < count; i++) {
        P2C_AstStmt *stmt = (P2C_AstStmt*)p2c_vec_get(stmts, i);
        size_t next = cursor + genloop_width_stmt(stmt);
        size_t stmt_exit = (i + 1u < count) ? next : exit;
        genloop_emit_simple(gl, stmt, cursor, stmt_exit, continue_state, break_state);
        cursor = next;
    }
}

/* 代入・複合代入を状態機械へ落とす。 */
static void genloop_yield_from(P2C_GenLoop *gl, P2C_AstExpr *inner, size_t entry, size_t exit, const char *bind);
static void genloop_emit_assign(P2C_GenLoop *gl, P2C_AstStmt *stmt, size_t entry, size_t exit) {
    P2C_CodeGen *cg = gl->cg;
    P2C_Map *locals = gl->locals;
    if (stmt->base.type == AST_AUGASSIGN) {
        P2C_AstExpr *target = stmt->base.u.augassign.target;
        const char *func = "p2c_obj_iadd";
        switch (stmt->base.u.augassign.op) {
            case OP_ADD: func = "p2c_obj_iadd"; break;
            case OP_SUB: func = "p2c_obj_isub"; break;
            case OP_MULT: func = "p2c_obj_imul"; break;
            case OP_DIV: func = "p2c_obj_div"; break;
            case OP_FLOORDIV: func = "p2c_obj_floordiv"; break;
            case OP_MOD: func = "p2c_obj_mod"; break;
            case OP_POW: func = "p2c_obj_pow"; break;
            case OP_BITAND: func = "p2c_obj_ibitand"; break;
            case OP_BITOR: func = "p2c_obj_ibitor"; break;
            case OP_BITXOR: func = "p2c_obj_ibitxor"; break;
            default:
                codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col,
                                     "augmented assignment operator", "unsupported in a generator loop");
                func = "p2c_obj_iadd";
                break;
        }
        if (!target || expr_has_suspension(stmt->base.u.augassign.value)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col,
                                 "augmented assignment",
                                 "generator state machine supports 'target op= value' only");
            genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
            return;
        }
        if (target->base.type == AST_ATTRIBUTE || target->base.type == AST_SUBSCRIPT) {
            /* self.total += v のような属性/添字への複合代入（ジェネレータメソッドで
             * よく使う形）。以前は「name op= value のみ」として診断していた。 */
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg);
            if (target->base.type == AST_ATTRIBUTE) {
                write_str(cg, "p2c_setattr(");
                gen_suspension_expr(cg, target->base.u.attribute.value, locals);
                write_str(cg, ", \""); write_str(cg, target->base.u.attribute.attr); write_str(cg, "\", ");
            } else {
                write_str(cg, "p2c_subscript_set(");
                gen_suspension_expr(cg, target->base.u.subscript.value, locals);
                write_str(cg, ", ");
                gen_suspension_expr(cg, target->base.u.subscript.slice, locals);
                write_str(cg, ", ");
            }
            write_str(cg, func); write_str(cg, "(");
            if (target->base.type == AST_ATTRIBUTE) {
                write_str(cg, "p2c_getattr(");
                gen_suspension_expr(cg, target->base.u.attribute.value, locals);
                write_str(cg, ", \""); write_str(cg, target->base.u.attribute.attr); write_str(cg, "\")");
            } else {
                write_str(cg, "p2c_subscript_get(");
                gen_suspension_expr(cg, target->base.u.subscript.value, locals);
                write_str(cg, ", ");
                gen_suspension_expr(cg, target->base.u.subscript.slice, locals);
                write_str(cg, ")");
            }
            write_str(cg, ", ");
            gen_suspension_expr(cg, stmt->base.u.augassign.value, locals);
            write_str(cg, "));"); write_newline(cg);
            pop_indent(cg);
            genloop_goto(gl, exit);
            genloop_close_case(gl);
            return;
        }
        if (target->base.type != AST_NAME) return;
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \"");
        write_str(cg, target->base.u.name.name); write_str(cg, "\", ");
        write_str(cg, func); write_str(cg, "(p2c_generator_local_get(generator, \"");
        write_str(cg, target->base.u.name.name); write_str(cg, "\"), ");
        gen_suspension_expr(cg, stmt->base.u.augassign.value, locals);
        write_str(cg, "));"); write_newline(cg);
        pop_indent(cg);
        genloop_goto(gl, exit);
        genloop_close_case(gl);
        return;
    }
    {
        P2C_AstExpr *value = stmt->base.u.assign.value;
        size_t ntargets = p2c_vec_len(stmt->base.u.assign.targets);
        if (value && value->base.type == AST_YIELD && !value->base.u.yield_expr.from && ntargets == 1) {
            /* x = yield v : 再開時に None を束縛する（本ランタイムは send() を持たないため、
             * next() で進める通常の反復では Python と同じく None が入る）。 */
            P2C_AstExpr *target = (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, 0);
            if (!target || target->base.type != AST_NAME) {
                codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col,
                                     "yield assignment target", "use a simple name target");
                genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
                return;
            }
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg); write_str(cg, "return p2c_generator_yield(generator, ");
            if (value->base.u.yield_expr.value) gen_suspension_expr(cg, value->base.u.yield_expr.value, locals);
            else write_str(cg, "&P2C_None");
            write_str(cg, ", "); emit_usize(cg, entry + 1u); write_str(cg, ");"); write_newline(cg);
            pop_indent(cg);
            genloop_close_case(gl);
            genloop_open_case(gl, entry + 1u);
            push_indent(cg);
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \"");
            write_str(cg, target->base.u.name.name);
            write_str(cg, "\", p2c_generator_sent_value(generator));"); write_newline(cg);
            pop_indent(cg);
            genloop_goto(gl, exit);
            genloop_close_case(gl);
            return;
        }
        if (value && value->base.type == AST_YIELD && value->base.u.yield_expr.from && ntargets == 1) {
            /* v = yield from X: 委譲の戻り値を v へ束縛する */
            P2C_AstExpr *target = (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, 0);
            if (!target || target->base.type != AST_NAME) {
                codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col,
                                     "yield from assignment target", "use a simple name target");
                genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
                return;
            }
            genloop_yield_from(gl, value->base.u.yield_expr.value, entry, exit, target->base.u.name.name);
            return;
        }
        if (value && expr_has_suspension(value)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, value->base.line, value->base.col,
                                 "yield/await inside an assignment expression",
                                 "only 'x = yield v' with a simple name target is supported");
            genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
            return;
        }
        if (ntargets == 1) {
            P2C_AstExpr *t0 = (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, 0);
            if (t0 && (t0->base.type == AST_ATTRIBUTE || t0->base.type == AST_SUBSCRIPT)) {
                /* self.x = v / d[k] = v（ジェネレータメソッドでの代入）。 */
                genloop_open_case(gl, entry);
                push_indent(cg);
                indent(cg);
                if (t0->base.type == AST_ATTRIBUTE) {
                    write_str(cg, "p2c_setattr(");
                    gen_suspension_expr(cg, t0->base.u.attribute.value, locals);
                    write_str(cg, ", \""); write_str(cg, t0->base.u.attribute.attr); write_str(cg, "\", ");
                } else {
                    write_str(cg, "p2c_subscript_set(");
                    gen_suspension_expr(cg, t0->base.u.subscript.value, locals);
                    write_str(cg, ", ");
                    gen_suspension_expr(cg, t0->base.u.subscript.slice, locals);
                    write_str(cg, ", ");
                }
                gen_suspension_expr(cg, value, locals);
                write_str(cg, ");"); write_newline(cg);
                pop_indent(cg);
                genloop_goto(gl, exit);
                genloop_close_case(gl);
                return;
            }
        }
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg); write_str(cg, "P2C_Object *_p2c_value = ");
        gen_suspension_expr(cg, value, locals); write_str(cg, ";"); write_newline(cg);
        for (size_t j = 0; j < ntargets; j++) {
            P2C_AstExpr *target = (P2C_AstExpr*)p2c_vec_get(stmt->base.u.assign.targets, j);
            genloop_bind_target(gl, target, "_p2c_value");
        }
        pop_indent(cg);
        genloop_goto(gl, exit);
        genloop_close_case(gl);
    }
}

/* yield / yield from / return / raise を状態機械へ落とす。 */
/* yield from inner: 反復子をジェネレータのローカルに保持し、要素ごとに yield する。
 * 再開後は同じ状態へ戻って次の要素を取り出す。bind が NULL でなければ、委譲が終わったとき
 * サブジェネレータの戻り値（StopIteration の値）をその名前のローカルに束縛する（PEP 380）。 */
static void genloop_yield_from(P2C_GenLoop *gl, P2C_AstExpr *inner, size_t entry, size_t exit, const char *bind) {
    P2C_CodeGen *cg = gl->cg;
    P2C_Map *locals = gl->locals;
            /* yield from iterable: 反復子をジェネレータのローカルに保持し、要素ごとに
             * yield する。再開後は同じ状態へ戻って次の要素を取り出す。 */
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg); write_str(cg, "P2C_Object *_p2c_yf_iter = p2c_generator_local_get(generator, \"__p2c_yield_from_");
            emit_usize(cg, entry); write_str(cg, "\");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_yf_iter == &P2C_None) { _p2c_yf_iter = p2c_builtin_iter(");
            gen_suspension_expr(cg, inner, locals);
            write_str(cg, "); p2c_generator_local_set(generator, \"__p2c_yield_from_");
            emit_usize(cg, entry); write_str(cg, "\", _p2c_yf_iter); }"); write_newline(cg);
            indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_yf_ef; P2C_Object *_p2c_yf_value = NULL;"); write_newline(cg);
            indent(cg); write_str(cg, "_p2c_yf_ef.prev = p2c_exc_stack; _p2c_yf_ef.exc = NULL; p2c_exc_stack = &_p2c_yf_ef;"); write_newline(cg);
            indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_yf_ef.env) == 0) { _p2c_yf_value = p2c_builtin_next(_p2c_yf_iter); p2c_exc_stack = _p2c_yf_ef.prev; }"); write_newline(cg);
            indent(cg); write_str(cg, "else { P2C_Object *_p2c_yf_exc = _p2c_yf_ef.exc; p2c_exc_stack = _p2c_yf_ef.prev; if (!p2c_exc_name_match(_p2c_yf_exc, \"StopIteration\")) p2c_raise(_p2c_yf_exc); "); if (bind) { write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, bind); write_str(cg, "\", p2c_stop_iteration_value(_p2c_yf_exc)); "); } write_str(cg, "p2c_generator_local_set(generator, \"__p2c_yield_from_");
            emit_usize(cg, entry); write_str(cg, "\", &P2C_None); p2c_generator_set_state(generator, ");
            emit_usize(cg, exit); write_str(cg, "); continue; }"); write_newline(cg);
            indent(cg); write_str(cg, "return p2c_generator_yield(generator, _p2c_yf_value, ");
            emit_usize(cg, entry); write_str(cg, ");"); write_newline(cg);
            pop_indent(cg);
            genloop_close_case(gl);
            return;
}

static void genloop_emit_suspension(P2C_GenLoop *gl, P2C_AstStmt *stmt, size_t entry, size_t exit) {
    P2C_CodeGen *cg = gl->cg;
    P2C_Map *locals = gl->locals;
    if (stmt->base.type == AST_EXPR_STMT) {
        P2C_AstExpr *value = stmt->base.u.expr_stmt.value;
        if (value && value->base.type == AST_YIELD && value->base.u.yield_expr.from) {
            genloop_yield_from(gl, value->base.u.yield_expr.value, entry, exit, NULL);
            return;
        }
        if (value && value->base.type == AST_YIELD) {
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg); write_str(cg, "return p2c_generator_yield(generator, ");
            if (value->base.u.yield_expr.value) gen_suspension_expr(cg, value->base.u.yield_expr.value, locals);
            else write_str(cg, "&P2C_None");
            write_str(cg, ", "); emit_usize(cg, entry + 1u); write_str(cg, ");"); write_newline(cg);
            pop_indent(cg);
            genloop_close_case(gl);
            genloop_open_case(gl, entry + 1u);
            genloop_goto(gl, exit);
            genloop_close_case(gl);
            return;
        }
        if (value && expr_has_suspension(value)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, value->base.line, value->base.col,
                                 "yield/await nested inside an expression",
                                 "assign it to a name on its own statement first");
            genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
            return;
        }
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg); write_str(cg, "(void)"); gen_suspension_expr(cg, value, locals); write_str(cg, ";"); write_newline(cg);
        pop_indent(cg);
        genloop_goto(gl, exit);
        genloop_close_case(gl);
        return;
    }
    if (stmt->base.type == AST_RETURN) {
        P2C_AstExpr *value = stmt->base.u.return_stmt.value;
        if (value && expr_has_suspension(value)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col,
                                 "yield/await inside a return expression", "not supported in a generator");
        }
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg); write_str(cg, "return p2c_generator_finish(generator, ");
        if (value && !expr_has_suspension(value)) gen_suspension_expr(cg, value, locals);
        else write_str(cg, "&P2C_None");
        write_str(cg, ");"); write_newline(cg);
        pop_indent(cg);
        genloop_close_case(gl);
        return;
    }
    if (stmt->base.type == AST_RAISE) {
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg);
        if (!stmt->base.u.raise.exc) {
            write_str(cg, "p2c_reraise();");
        } else if (stmt->base.u.raise.cause) {
            write_str(cg, "p2c_raise(p2c_exception_with_cause(");
            gen_raise_target(cg, stmt->base.u.raise.exc);
            write_str(cg, ", ");
            gen_suspension_expr(cg, stmt->base.u.raise.cause, locals);
            write_str(cg, "));");
        } else {
            write_str(cg, "p2c_raise(");
            gen_raise_target(cg, stmt->base.u.raise.exc);
            write_str(cg, ");");
        }
        write_newline(cg);
        indent(cg); write_str(cg, "return p2c_generator_finish(generator, &P2C_None);"); write_newline(cg);
        pop_indent(cg);
        genloop_close_case(gl);
        return;
    }
    genloop_open_case(gl, entry);
    genloop_goto(gl, exit);
    genloop_close_case(gl);
}

static void genloop_emit_simple(P2C_GenLoop *gl, P2C_AstStmt *stmt, size_t entry, size_t exit,
                                size_t continue_state, size_t break_state) {
    P2C_CodeGen *cg = gl->cg;
    P2C_Map *locals = gl->locals;
    if (!stmt) { genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl); return; }
    switch (stmt->base.type) {
        case AST_PASS:
            genloop_open_case(gl, entry);
            genloop_goto(gl, exit);
            genloop_close_case(gl);
            return;
        case AST_BREAK:
            genloop_open_case(gl, entry);
            push_indent(cg);
            if (gl->break_iter_local) {
                /* for ループを break で抜けるときはイテレータを破棄する。残すと
                 * 入れ子ループを外側から再入したときに前回の続きから回り、
                 * 要素を取りこぼす（例: for row ...: for v in row: break）。 */
                indent(cg); write_str(cg, "p2c_generator_local_set(generator, \"");
                write_str(cg, gl->break_iter_local); write_str(cg, "\", &P2C_None);"); write_newline(cg);
            }
            pop_indent(cg);
            genloop_goto(gl, break_state);
            genloop_close_case(gl);
            return;
        case AST_CONTINUE:
            genloop_open_case(gl, entry);
            genloop_goto(gl, continue_state);
            genloop_close_case(gl);
            return;
        case AST_BLOCK:
            /* セミコロン区切りの複数文。 */
            genloop_emit_stmt_list(gl, stmt->base.u.block.stmts, entry, exit, continue_state, break_state);
            return;
        case AST_ASSIGN:
        case AST_AUGASSIGN:
            genloop_emit_assign(gl, stmt, entry, exit);
            return;
        case AST_EXPR_STMT:
        case AST_RETURN:
        case AST_RAISE:
            genloop_emit_suspension(gl, stmt, entry, exit);
            return;
        default:
            break;
    }
    if (stmt->base.type == AST_IF) {
        P2C_AstExpr *test = stmt->base.u.if_stmt.test;
        P2C_Vector *body = stmt->base.u.if_stmt.body;
        P2C_Vector *orelse = stmt->base.u.if_stmt.orelse;
        if (expr_has_suspension(test)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, test->base.line, test->base.col,
                                 "yield/await in an if condition", "not supported in a generator");
            genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
            return;
        }
        {
            size_t body_entry = entry + 1u;
            size_t orelse_entry = body_entry + genloop_width_stmt_list(body);
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg); write_str(cg, "if (p2c_obj_is_truthy(");
            gen_suspension_expr(cg, test, locals);
            write_str(cg, ")) { p2c_generator_set_state(generator, "); emit_usize(cg, body_entry);
            write_str(cg, "); continue; }"); write_newline(cg);
            indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, orelse_entry);
            write_str(cg, "); continue;"); write_newline(cg);
            pop_indent(cg);
            genloop_close_case(gl);
            genloop_emit_stmt_list(gl, body, body_entry, exit, continue_state, break_state);
            genloop_emit_stmt_list(gl, orelse, orelse_entry, exit, continue_state, break_state);
        }
        return;
    }
    if (stmt->base.type == AST_WHILE) {
        P2C_AstExpr *test = stmt->base.u.while_stmt.test;
        P2C_Vector *body = stmt->base.u.while_stmt.body;
        P2C_Vector *orelse = stmt->base.u.while_stmt.orelse;
        if (expr_has_suspension(test)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, test->base.line, test->base.col,
                                 "yield/await in a while condition", "not supported in a generator");
            genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
            return;
        }
        size_t body_entry = entry + 1u;
        size_t orelse_entry = body_entry + genloop_width_stmt_list(body);
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg); write_str(cg, "if (p2c_obj_is_truthy(");
        gen_suspension_expr(cg, test, locals);
        write_str(cg, ")) { p2c_generator_set_state(generator, "); emit_usize(cg, body_entry);
        write_str(cg, "); continue; }"); write_newline(cg);
        indent(cg); write_str(cg, "p2c_generator_set_state(generator, "); emit_usize(cg, orelse_entry);
        write_str(cg, "); continue;"); write_newline(cg);
        pop_indent(cg);
        genloop_close_case(gl);
        /* 本体を終えたら条件判定（entry）へ戻る。break は else を飛ばして exit へ。 */
        genloop_emit_stmt_list(gl, body, body_entry, entry, entry, exit);
        genloop_emit_stmt_list(gl, orelse, orelse_entry, exit, entry, exit);
        return;
    }
    if (stmt->base.type == AST_FOR) {
        P2C_AstExpr *iter = stmt->base.u.for_stmt.iter;
        P2C_AstExpr *target = stmt->base.u.for_stmt.target;
        P2C_Vector *body = stmt->base.u.for_stmt.body;
        P2C_Vector *orelse = stmt->base.u.for_stmt.orelse;
        if (expr_has_suspension(iter)) {
            codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, iter->base.line, iter->base.col,
                                 "yield/await in a for iterable", "not supported in a generator");
            genloop_open_case(gl, entry); genloop_goto(gl, exit); genloop_close_case(gl);
            return;
        }
        {
            size_t body_entry = entry + 1u;
            size_t orelse_entry = body_entry + genloop_width_stmt_list(body);
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg); write_str(cg, "P2C_Object *_p2c_it = p2c_generator_local_get(generator, \"__p2c_iter_");
            emit_usize(cg, entry); write_str(cg, "\");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_it == &P2C_None) { _p2c_it = p2c_builtin_iter(");
            gen_suspension_expr(cg, iter, locals);
            write_str(cg, "); p2c_generator_local_set(generator, \"__p2c_iter_");
            emit_usize(cg, entry); write_str(cg, "\", _p2c_it); }"); write_newline(cg);
            indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_ef; P2C_Object *volatile _p2c_item = NULL; volatile bool _p2c_done = false;"); write_newline(cg);
            indent(cg); write_str(cg, "_p2c_ef.prev = p2c_exc_stack; _p2c_ef.exc = NULL; p2c_exc_stack = &_p2c_ef;"); write_newline(cg);
            indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_ef.env) == 0) { _p2c_item = p2c_builtin_next(_p2c_it); p2c_exc_stack = _p2c_ef.prev; }"); write_newline(cg);
            indent(cg); write_str(cg, "else { P2C_Object *_p2c_exc = _p2c_ef.exc; p2c_exc_stack = _p2c_ef.prev; if (!p2c_exc_name_match(_p2c_exc, \"StopIteration\")) p2c_raise(_p2c_exc); _p2c_done = true; }"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_done) { p2c_generator_local_set(generator, \"__p2c_iter_");
            emit_usize(cg, entry); write_str(cg, "\", &P2C_None); p2c_generator_set_state(generator, ");
            emit_usize(cg, orelse_entry); write_str(cg, "); continue; }"); write_newline(cg);
            genloop_bind_target(gl, target, "_p2c_item");
            pop_indent(cg);
            genloop_goto(gl, body_entry);
            genloop_close_case(gl);
            /* 本体を終えたら次の要素（entry）へ。break は else を飛ばして exit へ。 */
            {
                char iter_local[64];
                const char *saved_break_iter = gl->break_iter_local;
                snprintf(iter_local, sizeof(iter_local), "__p2c_iter_%zu", entry);
                gl->break_iter_local = iter_local;
                genloop_emit_stmt_list(gl, body, body_entry, entry, entry, exit);
                gl->break_iter_local = saved_break_iter;
            }
            genloop_emit_stmt_list(gl, orelse, orelse_entry, exit, entry, exit);
        }
        return;
    }
    if (stmt->base.type == AST_ASSERT) {
        /* assert は「条件が偽なら AssertionError」を1状態で表現する。 */
        genloop_open_case(gl, entry);
        push_indent(cg);
        indent(cg); write_str(cg, "if (!p2c_obj_is_truthy(");
        gen_suspension_expr(cg, stmt->base.u.assert_stmt.test, locals);
        write_str(cg, ")) p2c_raise(p2c_make_exception_with_args(\"AssertionError\", (P2C_Object*[]){");
        if (stmt->base.u.assert_stmt.msg &&
            stmt->base.u.assert_stmt.msg->base.type == AST_CONST &&
            stmt->base.u.assert_stmt.msg->base.u.constant.token_type == TOK_STR_LITERAL) {
            write_user_lit_str(cg, stmt->base.u.assert_stmt.msg->base.u.constant.value);
        } else if (stmt->base.u.assert_stmt.msg) {
            /* リテラル以外のメッセージ式も評価して args に載せる。 */
            gen_suspension_expr(cg, stmt->base.u.assert_stmt.msg, locals);
        } else {
            write_str(cg, "&P2C_None");
        }
        write_str(cg, "}, 1));"); write_newline(cg);
        pop_indent(cg);
        genloop_goto(gl, exit);
        genloop_close_case(gl);
        return;
    }
    if (stmt->base.type == AST_ANNASSIGN && stmt->base.u.annassign.simple &&
        stmt->base.u.annassign.target && stmt->base.u.annassign.target->base.type == AST_NAME) {
        /* 型注釈付き代入（x: int = v）。注釈は消去して代入だけを行う。
         * 値が無い `x: int` は宣言のみなので何もしない。 */
        if (stmt->base.u.annassign.value) {
            genloop_open_case(gl, entry);
            push_indent(cg);
            indent(cg); write_str(cg, "p2c_generator_local_set(generator, \"");
            write_str(cg, stmt->base.u.annassign.target->base.u.name.name); write_str(cg, "\", ");
            gen_suspension_expr(cg, stmt->base.u.annassign.value, locals);
            write_str(cg, ");"); write_newline(cg);
            pop_indent(cg);
        } else {
            genloop_open_case(gl, entry);
        }
        genloop_goto(gl, exit);
        genloop_close_case(gl);
        return;
    }
    codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, stmt->base.line, stmt->base.col,
                         stmt_kind_label(stmt),
                         "this statement is not supported inside a generator loop yet");
    genloop_open_case(gl, entry);
    genloop_goto(gl, exit);
    genloop_close_case(gl);
}

/* ジェネレータ関数（ループ内 yield 対応）をステップ関数＋生成関数へ変換する。
 * gen_suspension_function と同じく、呼び出し側は名前の衝突を避けるため
 * メソッドでは fd->name を "Class__method" に置き換えて呼ぶ。 */
static void gen_generator_function(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    bool saved_in_susp = cg->in_suspension_body;
    cg->in_suspension_body = true;
    if (cg->closure_env_var) {
        codegen_set_error_at(cg, P2C_ERR_NOT_IMPLEMENTED, 0u, 0u,
                             "generator containing a loop inside a closure",
                             "captured variables are not supported yet; move the generator to module level");
        return;
    }
    P2C_Map *locals = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    if (!locals) { codegen_set_error(cg, P2C_ERR_NOMEM, "could not allocate generator local map"); return; }
    size_t argc = p2c_vec_len(fd->args);
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (arg) map_set_name(locals, arg->name);
    }
    genloop_collect_stmt_list(locals, fd->body);
    size_t total_states = genloop_width_stmt_list(fd->body);

    /* 前方宣言（生成関数からステップ関数を参照するため）。
     * クラスメソッドの場合はクラスのプロトタイプ宣言群が先に同じ宣言を
     * 出しているため、ここでは重複させない（-Wredundant-decls 対策）。 */
    p2c_str_append(cg->forward, "static P2C_Object *");
    p2c_str_append(cg->forward, mangle_ident(fd->name));
    p2c_str_append(cg->forward, "__step(P2C_Object *generator);\n");
    if (!cg->current_class) {
        p2c_str_append(cg->forward, "static P2C_Object *");
        p2c_str_append(cg->forward, mangle_ident(fd->name));
        p2c_str_append(cg->forward, "(");
        {
            bool wrote = false;
            for (size_t i = 0; i < argc; i++) {
                if (wrote) p2c_str_append(cg->forward, ", ");
                wrote = true;
                p2c_str_append(cg->forward, "P2C_Object *");
                p2c_str_append(cg->forward, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
            }
            if (!wrote) p2c_str_append(cg->forward, "void");
        }
        p2c_str_append(cg->forward, ");\n");
    }

    P2C_GenLoop gl;
    gl.cg = cg;
    gl.locals = locals;
    gl.break_iter_local = NULL;

    indent(cg); write_str(cg, "static P2C_Object *"); write_ident(cg, fd->name);
    write_str(cg, "__step(P2C_Object *generator) {"); write_newline(cg);
    push_indent(cg);
    write_line(cg, "for (;;) {");
    push_indent(cg);
    write_line(cg, "switch (p2c_generator_state(generator)) {");
    push_indent(cg);
    genloop_emit_stmt_list(&gl, fd->body, 0u, total_states, 0u, 0u);
    indent(cg); write_str(cg, "default: return p2c_generator_finish(generator, &P2C_None);"); write_newline(cg);
    pop_indent(cg); write_line(cg, "}");   /* switch */
    pop_indent(cg); write_line(cg, "}");   /* for */
    write_line(cg, "return p2c_generator_finish(generator, &P2C_None);");
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);

    /* 生成関数: 引数をジェネレータのローカルへ複製して generator を返す。 */
    indent(cg); write_str(cg, "static P2C_Object *"); write_ident(cg, fd->name); write_str(cg, "(");
    for (size_t i = 0; i < argc; i++) {
        if (i) write_str(cg, ", ");
        write_str(cg, "P2C_Object *"); write_param_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    }
    if (argc == 0) write_str(cg, "void");
    write_str(cg, ") {"); write_newline(cg);
    push_indent(cg);
    indent(cg); write_str(cg, "P2C_Object *generator = p2c_generator_new(");
    write_ident(cg, fd->name); write_str(cg, "__step, false);"); write_newline(cg);
    for (size_t i = 0; i < argc; i++) {
        const char *aname = ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name;
        indent(cg); write_str(cg, "p2c_generator_local_set(generator, \""); write_str(cg, aname);
        write_str(cg, "\", "); write_ident(cg, aname); write_str(cg, ");"); write_newline(cg);
    }
    write_line(cg, "return generator;");
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
    free_name_map(locals);
    cg->in_suspension_body = saved_in_susp;
}

static void gen_nested_closure(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    P2C_Map *captures = closure_capture_scope(cg);
    P2C_Map *merged_captures = NULL;
    /* クロージャ内でさらに定義するネスト関数は、外側の関数のローカルだけでなく
     * 外側が捕捉している変数（多段 capture）も参照できる必要がある。
     * 外側の closure env の名前を捕捉集合へ加える（セルは共有する）。 */
    if (cg->declared_vars && cg->closure_env_names && cg->closure_env_var) {
        merged_captures = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
        if (merged_captures) {
            for (size_t i = 0; i < cg->closure_env_names->bucket_count; i++)
                for (P2C_MapEntry *e = cg->closure_env_names->buckets[i]; e; e = e->next)
                    map_set_name(merged_captures, (const char*)e->key);
            for (size_t i = 0; i < cg->declared_vars->bucket_count; i++)
                for (P2C_MapEntry *e = cg->declared_vars->buckets[i]; e; e = e->next)
                    map_set_name(merged_captures, (const char*)e->key);
            captures = merged_captures;
        }
    }
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
    /* forward へ書き込み中に現れたネスト関数定義を forward へ直接書くと、
     * 生成中の外側の定義に割り込んで壊れたCになる（例: クロージャ内の
     * さらに入れ子の def、ラムダ内の def）。別バッファへ退避し、最後に
     * forward の末尾へ連結する。前方宣言は header へ置く。 */
    p2c_str_append(cg->header, "static P2C_Object *");
    p2c_str_append(cg->header, entry_name);
    p2c_str_append(cg->header, "(P2C_Object *env, P2C_Object **args, size_t nargs);\n");
    saved_current = cg->current;
    saved_indent = cg->indent_level;
    const bool nested_def = (cg->current != cg->toplevel && cg->current != cg->body &&
                             cg->current != cg->header);
    P2C_String *def_buf = nested_def ? p2c_str_new(cg->alloc) : cg->forward;
    if (!def_buf) def_buf = cg->forward;
    cg->current = def_buf;
    cg->indent_level = 0;
    write_str(cg, "static P2C_Object *"); write_str(cg, entry_name); write_str(cg, "(P2C_Object *env, P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
    indent(cg); write_str(cg, "(void)env; (void)args; (void)nargs;"); write_newline(cg);
    size_t argc = p2c_vec_len(fd->args);
    /* env を書き換える前に、呼び出し元の状態を退避する（順序を誤ると
     * 解放済みマップへの参照が残り、コンパイラ本体が落ちる）。 */
    saved_declared = cg->declared_vars;
    saved_env_names = cg->closure_env_names;
    saved_env_var = cg->closure_env_var;
    /* 既定値の式は外側の捕捉変数を参照しうるため、env を先に設定する。 */
    cg->closure_env_names = captures;
    cg->closure_env_var = "env";
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, arg->name);
        write_str(cg, " = (nargs > "); emit_usize(cg, i); write_str(cg, ") ? args["); emit_usize(cg, i); write_str(cg, "] : ");
        if (arg->default_val) {
            /* 既定値は def 時に評価され env に保存されている。 */
            char ekey[64];
            default_env_key(ekey, sizeof(ekey), i);
            write_str(cg, "p2c_dict_get(env, "); write_name_lit_str(cg, ekey); write_str(cg, ")");
        } else {
            write_str(cg, "&P2C_None");
        }
        write_str(cg, "; (void)"); write_param_ident(cg, arg->name); write_str(cg, ";"); write_newline(cg);
    }
    /* *args / **kwargs をタプル/辞書として束縛する。クロージャ呼び出しは
     * p2c_call 経由の位置引数のみのため、**kwargs は常に空辞書になる
     * （キーワード引数を伴う可変長デコレータは未対応だが、少なくとも
     * 不正なCを出さずコンパイルできるようにする）。 */
    if (fd->vararg) {
        indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->vararg);
        write_str(cg, " = p2c_tuple_from_array((nargs > "); emit_usize(cg, argc); write_str(cg, " && args) ? args + "); emit_usize(cg, argc);
        write_str(cg, " : NULL, nargs > "); emit_usize(cg, argc); write_str(cg, " ? nargs - "); emit_usize(cg, argc); write_str(cg, " : 0);");
        write_str(cg, " (void)"); write_param_ident(cg, fd->vararg); write_str(cg, ";"); write_newline(cg);
    }
    if (fd->kwarg) {
        indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->kwarg);
        write_str(cg, " = p2c_dict_new(); (void)"); write_param_ident(cg, fd->kwarg); write_str(cg, ";"); write_newline(cg);
    }
    P2C_Map *saved_nonlocal_names = cg->nonlocal_names;
    P2C_Map *saved_cell_names = cg->cell_names;
    cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    cg->synthetic_locals = NULL;
    cg->nonlocal_names = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
    cg->cell_names = collect_nested_nonlocal_cell_names(cg, fd->body);
    for (size_t i = 0; i < argc; i++) remember_declared(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name);
    if (fd->vararg) remember_declared(cg, fd->vararg);
    if (fd->kwarg) remember_declared(cg, fd->kwarg);
    collect_global_decls(fd->body, cg);
    /* AOT アンボクシング: 宣言より前に解析する（--unbox）。 */
    P2C_Map *saved_native_int_locals = native_begin_function(cg, fd);
    P2C_Map *closure_refs = collect_closure_refs(cg, fd->body);
    predeclare_stmt_list(cg, fd->body);
    add_closure_cells(cg, closure_refs);
    p2c_map_free(closure_refs);
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
    /* AOT アンボクシング: この関数の解析結果を外側の集合へ戻す。 */
    native_end_function(cg, saved_native_int_locals);
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
    if (nested_def) {
        if (!cg->deferred_defs) cg->deferred_defs = p2c_vec_new(cg->alloc, NULL);
        if (cg->deferred_defs) (void)p2c_vec_push(cg->deferred_defs, def_buf);
    }
    indent(cg); write_ident(cg, fd->name); write_str(cg, " = p2c_closure_new(\""); write_str(cg, fd->name); write_str(cg, "\", "); write_str(cg, entry_name); write_str(cg, ", ");
    {
    size_t ndefaults = 0;
    for (size_t i = 0; i < argc; i++) {
        P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (a && a->default_val) ndefaults++;
    }
    size_t total_env = capture_count + ndefaults;
    if (total_env == 0) {
        write_str(cg, "p2c_dict_from_pairs(NULL, NULL, 0)");
    } else {
        write_str(cg, "p2c_dict_from_pairs((P2C_Object*[]){");
        bool first = true;
        for (size_t i = 0; i < captures->bucket_count; i++) {
            for (P2C_MapEntry *e = captures->buckets[i]; e; e = e->next) {
                if (!first) write_str(cg, ", ");
                first = false;
                write_name_lit_str(cg, (const char*)e->key);
            }
        }
        for (size_t i = 0; i < argc; i++) {
            P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(fd->args, i);
            if (!a || !a->default_val) continue;
            char ekey[64];
            default_env_key(ekey, sizeof(ekey), i);
            if (!first) write_str(cg, ", ");
            first = false;
            write_name_lit_str(cg, ekey);
        }
        write_str(cg, "}, (P2C_Object*[]){");
        first = true;
        for (size_t i = 0; i < captures->bucket_count; i++) {
            for (P2C_MapEntry *e = captures->buckets[i]; e; e = e->next) {
                if (!first) write_str(cg, ", ");
                first = false;
                if (map_has_name(cg->declared_vars, (const char*)e->key)) {
                    if (map_has_name(cg->cell_names, (const char*)e->key)) {
                        write_str(cg, "_p2c_cell_"); write_ident(cg, (const char*)e->key);
                    } else {
                        write_str(cg, "p2c_cell_new("); write_ident(cg, (const char*)e->key); write_str(cg, ")");
                    }
                } else {
                    /* 多段 capture: 外側の closure env が持つセルをそのまま共有する。 */
                    write_str(cg, "p2c_dict_get("); write_str(cg, cg->closure_env_var ? cg->closure_env_var : "env");
                    write_str(cg, ", "); write_name_lit_str(cg, (const char*)e->key); write_str(cg, ")");
                }
            }
        }
        /* 既定値はこの def 文の実行時（closure生成時）に一度だけ評価して保存する。 */
        for (size_t i = 0; i < argc; i++) {
            P2C_AstArg *a = (P2C_AstArg*)p2c_vec_get(fd->args, i);
            if (!a || !a->default_val) continue;
            if (!first) write_str(cg, ", ");
            first = false;
            gen_expr(cg, a->default_val);
        }
        write_str(cg, "}, "); emit_usize(cg, total_env); write_str(cg, ")");
    }
    }
    write_str(cg, ");"); write_newline(cg);
    if (merged_captures) free_name_map(merged_captures);
}

static void gen_decorator_callable_adapter(P2C_CodeGen *cg, P2C_AstFunctionDef *fd) {
    /* 関数の C 形式は [全引数（位置とキーワード専用の順）, *args のタプル, **kwargs の辞書]。
     * アダプタは引数配列から位置引数を取り、キーワード専用引数は既定値で埋める。
     * *args には残りの位置引数、**kwargs には空の辞書を渡す（呼び出しでキーワードは渡らない）。 */
    size_t argc = p2c_vec_len(fd->args);
    size_t npos = (fd->kwonly_start >= 0 && (size_t)fd->kwonly_start <= argc) ? (size_t)fd->kwonly_start : argc;
    size_t required = 0;
    for (size_t i = 0; i < npos; i++) {
        P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
        if (!arg->default_val) required++;
    }
    indent(cg); write_str(cg, "static P2C_Object *"); write_ident(cg, fd->name); write_str(cg, "__decorator_adapter(P2C_Object **args, size_t nargs) {"); write_newline(cg); push_indent(cg);
    write_line(cg, "(void)args;");
    indent(cg); write_str(cg, "if (");
    if (required > 0) { write_str(cg, "nargs < "); emit_usize(cg, required); if (!fd->vararg) write_str(cg, " || "); }
    if (!fd->vararg) { write_str(cg, "nargs > "); emit_usize(cg, npos); }
    else if (required == 0) write_str(cg, "0");
    write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"wrong function arity for decorated function\"));"); write_newline(cg);
    indent(cg); write_str(cg, "return "); write_ident(cg, fd->name); write_str(cg, "(");
    {
        char dk[192];
        snprintf(dk, sizeof(dk), "%s", mangle_ident(fd->name));
        bool wrote = false;
        for (size_t i = 0; i < argc; i++) {
            P2C_AstArg *arg = (P2C_AstArg*)p2c_vec_get(fd->args, i);
            if (wrote) write_str(cg, ", ");
            wrote = true;
            if (i < npos) {
                write_str(cg, "(nargs > "); emit_usize(cg, i); write_str(cg, ") ? args["); emit_usize(cg, i); write_str(cg, "] : ");
            }
            emit_default_value(cg, dk, i, arg->default_val);
        }
        if (fd->vararg) {
            if (wrote) write_str(cg, ", ");
            wrote = true;
            write_str(cg, "p2c_tuple_from_array(nargs > "); emit_usize(cg, npos);
            write_str(cg, " ? args + "); emit_usize(cg, npos);
            write_str(cg, " : NULL, nargs > "); emit_usize(cg, npos);
            write_str(cg, " ? nargs - "); emit_usize(cg, npos); write_str(cg, " : 0)");
        }
        if (fd->kwarg) {
            if (wrote) write_str(cg, ", ");
            wrote = true;
            write_str(cg, "p2c_dict_new()");
        }
    }
    write_str(cg, ");"); write_newline(cg);
    pop_indent(cg); write_line(cg, "}"); write_newline(cg);
}

static void gen_decorator_expr(P2C_CodeGen *cg, P2C_AstExpr *expr) {
    if (expr && expr->base.type == AST_NAME && map_has_name(cg->module_function_names, expr->base.u.name.name) && !map_has_name(cg->decorated_names, expr->base.u.name.name)) {
        {
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

/* for 文のターゲット束縛（入れ子のタプル/リストと先頭 '*' に対応）。 */
static void gen_for_bind_from_expr(P2C_CodeGen *cg, P2C_AstExpr *target, const char *value_expr, int for_id) {
    if (!target || !value_expr) return;
    if (target->base.type == AST_NAME) {
        indent(cg);
        if (map_has_name(cg->cell_names, target->base.u.name.name)) {
            write_str(cg, "p2c_cell_set(_p2c_cell_"); write_ident(cg, target->base.u.name.name);
            write_str(cg, ", "); write_str(cg, value_expr); write_str(cg, ");");
        } else {
            write_ident(cg, target->base.u.name.name);
            write_str(cg, " = "); write_str(cg, value_expr); write_str(cg, ";");
        }
        write_newline(cg);
        return;
    }
    if (target->base.type == AST_TUPLE || target->base.type == AST_LIST) {
        P2C_Vector *elts = target->base.u.list.elts;
        size_t n = p2c_vec_len(elts);
        size_t star_index = n, star_count = 0;
        for (size_t i = 0; i < n; i++) {
            P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, i);
            if (e && e->base.type == AST_STARRED) { star_index = i; star_count++; }
        }
        if (star_count > 1) {
            codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "multiple starred for-loop targets are not supported");
            return;
        }
        int sub_id = ++cg->temp_counter;
        char sub[64], sublen[64];
        snprintf(sub, sizeof(sub), "_p2c_for_sub_%d", sub_id);
        snprintf(sublen, sizeof(sublen), "_p2c_for_sublen_%d", sub_id);
        indent(cg); write_str(cg, "P2C_Object *"); write_str(cg, sub); write_str(cg, " = "); write_str(cg, value_expr); write_str(cg, ";"); write_newline(cg);
        indent(cg); write_str(cg, "int64_t "); write_str(cg, sublen); write_str(cg, " = p2c_len("); write_str(cg, sub); write_str(cg, ");"); write_newline(cg);
        if (star_count == 0) {
            indent(cg); write_str(cg, "if ("); write_str(cg, sublen); write_str(cg, " != "); emit_usize(cg, n);
            write_str(cg, ") p2c_raise(p2c_make_exception(\"ValueError\", \"for unpack target has incorrect length\"));"); write_newline(cg);
        } else {
            indent(cg); write_str(cg, "if ("); write_str(cg, sublen); write_str(cg, " < "); emit_usize(cg, n - 1);
            write_str(cg, ") p2c_raise(p2c_make_exception(\"ValueError\", \"for unpack target has insufficient values\"));"); write_newline(cg);
        }
        for (size_t i = 0; i < n; i++) {
            P2C_AstExpr *e = (P2C_AstExpr*)p2c_vec_get(elts, i);
            if (!e) continue;
            if (e->base.type == AST_STARRED) {
                P2C_AstExpr *inner = e->base.u.starred.value;
                if (!inner || inner->base.type != AST_NAME) {
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "starred for-loop target must be a name");
                    continue;
                }
                size_t after = n - i - 1;
                indent(cg); write_ident(cg, inner->base.u.name.name); write_str(cg, " = p2c_list_new();"); write_newline(cg);
                indent(cg); write_str(cg, "for (int64_t _p2c_for_rest_i_"); emit_usize(cg, (size_t)sub_id); write_str(cg, " = "); emit_usize(cg, i);
                write_str(cg, "; _p2c_for_rest_i_"); emit_usize(cg, (size_t)sub_id); write_str(cg, " < "); write_str(cg, sublen); write_str(cg, " - "); emit_usize(cg, after);
                write_str(cg, "; _p2c_for_rest_i_"); emit_usize(cg, (size_t)sub_id); write_str(cg, "++) p2c_list_append("); write_ident(cg, inner->base.u.name.name);
                write_str(cg, ", p2c_subscript_get("); write_str(cg, sub); write_str(cg, ", p2c_obj_from_int(_p2c_for_rest_i_"); emit_usize(cg, (size_t)sub_id);
                write_str(cg, "))); "); write_newline(cg);
                continue;
            }
            char item_expr[256];
            if (star_count > 0 && i > star_index) {
                snprintf(item_expr, sizeof(item_expr), "p2c_subscript_get(%s, p2c_obj_from_int(%s - %zu + %zu))",
                         sub, sublen, n - star_index - 1, i - star_index - 1);
            } else {
                snprintf(item_expr, sizeof(item_expr), "p2c_subscript_get(%s, p2c_obj_from_int(%zu))", sub, i);
            }
            (void)for_id;
            gen_for_bind_from_expr(cg, e, item_expr, for_id);
        }
        return;
    }
    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED,
                      "for-loop targets support names and tuple/list unpacking only");
}
static void gen_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt) {
    if (!stmt) return;
    P2C_AstNode *n = &stmt->base;
    emit_debug_comment(cg, n);
    switch (n->type) {
        case AST_ASSIGN:
            if (p2c_vec_len(n->u.assign.targets) > 1) {
                /* 連鎖代入 a = b = expr。一時変数名は文ごとに一意にする
                 * （固定名だと同じスコープに2つ目の連鎖代入が来たとき
                 * 再定義エラーで生成Cがコンパイルできない）。各ターゲットは
                 * gen_assign_target に委ねる（タプル/属性/添字ターゲットにも対応。
                 * 以前は名前/属性/添字しか処理せず、`a, b = c, d = 1, 2` の
                 * タプルターゲットが黙って無視され None のままになっていた）。 */
                int assign_tmp_id = ++cg->temp_counter;
                char assign_tmp[96];
                P2C_AstExpr synth_tmp;
                indent(cg); write_str(cg, "P2C_Object *_p2c_assign_tmp_");
                emit_usize(cg, (size_t)assign_tmp_id);
                write_str(cg, " = "); gen_expr(cg, n->u.assign.value); write_str(cg, ";"); write_newline(cg);
                snprintf(assign_tmp, sizeof(assign_tmp), "_p2c_assign_tmp_%d", assign_tmp_id);
                memset(&synth_tmp, 0, sizeof(synth_tmp));
                synth_tmp.base.type = AST_NAME;
                synth_tmp.base.u.name.name = assign_tmp;
                remember_synthetic_local(cg, assign_tmp);
                for (size_t i = 0; i < p2c_vec_len(n->u.assign.targets); i++) {
                    P2C_AstExpr *t = (P2C_AstExpr*)p2c_vec_get(n->u.assign.targets, i);
                    if (!t) continue;
                    if (t->base.type == AST_NAME || t->base.type == AST_ATTRIBUTE || t->base.type == AST_SUBSCRIPT) {
                        indent(cg);
                        if (t->base.type == AST_NAME) { write_ident(cg, t->base.u.name.name); write_str(cg, " = _p2c_assign_tmp_"); emit_usize(cg, (size_t)assign_tmp_id); write_str(cg, ";"); }
                        else if (t->base.type == AST_ATTRIBUTE) { write_str(cg, "p2c_setattr("); gen_expr(cg, t->base.u.attribute.value); write_str(cg, ", \""); write_str(cg, t->base.u.attribute.attr); write_str(cg, "\", _p2c_assign_tmp_"); emit_usize(cg, (size_t)assign_tmp_id); write_str(cg, ");"); }
                        else { write_str(cg, "p2c_subscript_set("); gen_expr(cg, t->base.u.subscript.value); write_str(cg, ", "); gen_expr(cg, t->base.u.subscript.slice); write_str(cg, ", _p2c_assign_tmp_"); emit_usize(cg, (size_t)assign_tmp_id); write_str(cg, ");"); }
                        write_newline(cg);
                    } else {
                        gen_assign_target(cg, t, &synth_tmp);
                    }
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
                case OP_ADD: func = "p2c_obj_iadd"; break;
                case OP_SUB: func = "p2c_obj_isub"; break;
                case OP_MULT: func = "p2c_obj_imul"; break;
                case OP_DIV: func = "p2c_obj_div"; break;
                case OP_FLOORDIV: func = "p2c_obj_floordiv"; break;
                case OP_MOD: func = "p2c_obj_mod"; break;
                case OP_POW: func = "p2c_obj_pow"; break;
                case OP_LSHIFT: func = "p2c_obj_lshift"; break;
                case OP_RSHIFT: func = "p2c_obj_rshift"; break;
                case OP_BITAND: func = "p2c_obj_ibitand"; break;
                case OP_BITOR: func = "p2c_obj_ibitor"; break;
                case OP_BITXOR: func = "p2c_obj_ibitxor"; break;
                default:
                    codegen_set_error(cg, P2C_ERR_NOT_IMPLEMENTED, "unsupported augmented assignment operator");
                    break;
            }
            P2C_AstExpr *target = n->u.augassign.target;
            if (target->base.type == AST_NAME) {
                indent(cg);
                if (map_has_name(cg->nonlocal_names, target->base.u.name.name) && cg->closure_env_var) {
                    write_str(cg, "p2c_cell_set(p2c_dict_get("); write_str(cg, cg->closure_env_var); write_str(cg, ", "); write_name_lit_str(cg, target->base.u.name.name); write_str(cg, "), ");
                    write_str(cg, func); write_str(cg, "("); gen_expr(cg, target); write_str(cg, ", "); gen_expr(cg, n->u.augassign.value); write_str(cg, "));");
                } else if (map_has_name(cg->cell_names, target->base.u.name.name)) {
                    write_str(cg, "p2c_cell_set(_p2c_cell_"); write_ident(cg, target->base.u.name.name); write_str(cg, ", ");
                    write_str(cg, func); write_str(cg, "("); gen_expr(cg, target); write_str(cg, ", "); gen_expr(cg, n->u.augassign.value); write_str(cg, "));");
                } else if (is_native_int_local(cg, target->base.u.name.name) &&
                           raw_binop_func(n->u.augassign.op)) {
                    /* AOT アンボクシング: 型付きローカルの複合代入は生の int64 で計算する。 */
                    write_ident(cg, target->base.u.name.name); write_str(cg, " = ");
                    write_str(cg, raw_binop_func(n->u.augassign.op)); write_str(cg, "(");
                    write_ident(cg, target->base.u.name.name); write_str(cg, ", ");
                    gen_int_expr(cg, n->u.augassign.value); write_str(cg, ");");
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
            indent(cg); write_str(cg, "P2C_Object *_p2c_iter_tmp_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = NULL;"); write_newline(cg);
            /* 高速路: list/tuple/str/range では例外が起きないので setjmp を挟まない
             * （setjmp は毎要素では高価）。カスタム __next__/ジェネレータのときだけ
             * 下の例外経路へ落ちる。受領用の一時変数は non-volatile にし、
             * volatile な本体用変数へ写してから使う（volatile を cast で
             * 落とすと -Wcast-qual に掛かるため）。 */
            indent(cg); write_str(cg, "int _p2c_iter_fast_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = p2c_iter_try_next(_p2c_iter_obj_"); emit_usize(cg, (size_t)for_id); write_str(cg, ", &_p2c_iter_tmp_"); emit_usize(cg, (size_t)for_id); write_str(cg, ");"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_iter_fast_"); emit_usize(cg, (size_t)for_id); write_str(cg, " == 0) break;"); write_newline(cg);
            indent(cg); write_str(cg, "P2C_Object * volatile _p2c_iter_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = _p2c_iter_tmp_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "if (_p2c_iter_fast_"); emit_usize(cg, (size_t)for_id); write_str(cg, " < 0) {"); write_newline(cg); push_indent(cg);
            indent(cg); write_str(cg, "P2C_ExceptFrame _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "_p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".prev = p2c_exc_stack; _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".exc = NULL; p2c_exc_stack = &_p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ";"); write_newline(cg);
            indent(cg); write_str(cg, "if (P2C_SETJMP(_p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".env) == 0) { _p2c_iter_item_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = p2c_builtin_next(_p2c_iter_obj_"); emit_usize(cg, (size_t)for_id); write_str(cg, "); p2c_exc_stack = _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".prev; } else { P2C_Object *_p2c_iter_exc_"); emit_usize(cg, (size_t)for_id); write_str(cg, " = _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".exc; p2c_exc_stack = _p2c_iter_ef_"); emit_usize(cg, (size_t)for_id); write_str(cg, ".prev; if (p2c_exc_name_match(_p2c_iter_exc_"); emit_usize(cg, (size_t)for_id); write_str(cg, ", \"StopIteration\")) break; p2c_raise(_p2c_iter_exc_"); emit_usize(cg, (size_t)for_id); write_str(cg, "); }"); write_newline(cg);
            pop_indent(cg); write_line(cg, "}");
            if (n->u.for_stmt.target) {
                char for_item_var[64];
                snprintf(for_item_var, sizeof(for_item_var), "_p2c_iter_item_%d", for_id);
                gen_for_bind_from_expr(cg, n->u.for_stmt.target, for_item_var, for_id);
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
            /* assert は「条件が偽なら AssertionError」。メッセージは (1) 文字列
             * リテラルならそのまま、(2) それ以外の式なら評価して args に載せる、
             * (3) 無指定なら "assertion failed"。 */
            if (n->u.assert_stmt.msg && n->u.assert_stmt.msg->base.type == AST_CONST && n->u.assert_stmt.msg->base.u.constant.token_type == TOK_STR_LITERAL) {
                indent(cg); write_str(cg, "if (!p2c_obj_is_truthy("); gen_expr(cg, n->u.assert_stmt.test); write_str(cg, ")) p2c_raise(p2c_make_exception(\"AssertionError\", \"");
                gen_string_literal_contents(cg, n->u.assert_stmt.msg->base.u.constant.value);
                write_str(cg, "\"));"); write_newline(cg);
            } else if (n->u.assert_stmt.msg) {
                /* リテラル以外のメッセージ式も評価して args に載せる（以前は
                 * 「assertion failed」に置き換わり、原因が分からないうえ
                 * メッセージ式の副作用も落ちていた）。p2c_make_exception_with_args は
                 * 文字列をスタックへ写してから確保するため収集中でも安全。 */
                indent(cg); write_str(cg, "if (!p2c_obj_is_truthy("); gen_expr(cg, n->u.assert_stmt.test); write_str(cg, ")) p2c_raise(p2c_make_exception_with_args(\"AssertionError\", (P2C_Object*[]){");
                gen_expr(cg, n->u.assert_stmt.msg);
                write_str(cg, "}, 1));"); write_newline(cg);
            } else {
                indent(cg); write_str(cg, "if (!p2c_obj_is_truthy("); gen_expr(cg, n->u.assert_stmt.test); write_str(cg, ")) p2c_raise(p2c_make_exception(\"AssertionError\", \"assertion failed\"));"); write_newline(cg);
            }
            break;
        case AST_FUNCTIONDEF: {
            P2C_AstFunctionDef *fd = &n->u.functiondef;
            if (cg->function_depth > 0) {
                gen_nested_closure(cg, fd);
                break;
            }
            if (generator_needs_state_machine(fd)) {
                /* ループの内側に yield があるジェネレータ（汎用状態機械）。 */
                gen_generator_function(cg, fd);
                if (cg->function_depth == 0 &&
                    ((fd->decorator_list && p2c_vec_len(fd->decorator_list) > 0) || map_has_name(cg->decorator_callable_names, fd->name))) {
                    gen_decorator_callable_adapter(cg, fd);
                }
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
                    write_param_ident(cg, arg->name);
                }
                if (fd->vararg) { if (wrote) write_str(cg, ", "); wrote = true; write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->vararg); }
                if (fd->kwarg) { if (wrote) write_str(cg, ", "); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->kwarg); }
            }
            if (nfixed == 0 && !fd->vararg && !fd->kwarg) write_str(cg, "void");
            write_str(cg, ") {"); write_newline(cg); push_indent(cg);
            /* 本体がパラメータを1つも参照しない関数（例: def f(x): return 1）では
             * -Wunused-parameter が -Werror 下でコンパイルエラーになる。
             * 常に(void)キャストしておけば、実際に使われる場合でも無害。 */
            for (size_t i = 0; i < nfixed; i++) {
                indent(cg); write_str(cg, "(void)"); write_param_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, i))->name); write_str(cg, ";"); write_newline(cg);
            }
            if (fd->vararg) { indent(cg); write_str(cg, "(void)"); write_param_ident(cg, fd->vararg); write_str(cg, ";"); write_newline(cg); }
            if (fd->kwarg) { indent(cg); write_str(cg, "(void)"); write_param_ident(cg, fd->kwarg); write_str(cg, ";"); write_newline(cg); }
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
            /* AOT アンボクシング: 宣言の前に解析する（--unbox）。 */
            P2C_Map *saved_native_int_locals = native_begin_function(cg, fd);
            P2C_Map *closure_refs = collect_closure_refs(cg, fd->body);
            predeclare_stmt_list(cg, fd->body);
            add_closure_cells(cg, closure_refs);
            p2c_map_free(closure_refs);
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
            /* AOT アンボクシング: この関数の解析結果を外側の集合へ戻す。 */
            native_end_function(cg, saved_native_int_locals);
            cg->cleanup_top = saved_cleanup_top;
            cg->loop_depth = saved_loop_depth;
            if (p2c_vec_len(fd->body) == 0 || ((P2C_AstStmt*)p2c_vec_get(fd->body, p2c_vec_len(fd->body)-1))->base.type != AST_RETURN) write_line(cg, "return &P2C_None;");
            p2c_map_free(cg->declared_vars); cg->declared_vars = saved_declared;
            free_name_map(cg->nonlocal_names); cg->nonlocal_names = saved_nonlocal_names;
            free_name_map(cg->cell_names); cg->cell_names = saved_cell_names;
            pop_indent(cg); write_line(cg, "}"); write_newline(cg);
            if (cg->function_depth == 0 &&
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
                bool method_is_generator_loop = !fd->is_async && stmt_list_has_suspension(fd->body);
                if (method_is_generator_loop) method_is_suspension = true;
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
                    if (method_is_generator_loop) gen_generator_function(cg, fd);
                    else gen_suspension_function(cg, fd);
                    fd->name = saved_method_name;
                    p2c_free(cg->alloc, method_name_copy);
                    p2c_str_free(method_entry);
                } else {
                    indent(cg); write_str(cg, "static P2C_Object* "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "(");
                if (method_kind == CG_METHOD_STATIC) write_str(cg, "P2C_Object *_p2c_static_self");
                for (size_t j = 0; j < p2c_vec_len(fd->args); j++) {
                    if (j || method_kind == CG_METHOD_STATIC) write_str(cg, ", ");
                    write_str(cg, "P2C_Object *"); write_param_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name);
                }
                if (fd->vararg) { if (p2c_vec_len(fd->args) || method_kind == CG_METHOD_STATIC) write_str(cg, ", "); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->vararg); }
                if (fd->kwarg) { if (p2c_vec_len(fd->args) || fd->vararg || method_kind == CG_METHOD_STATIC) write_str(cg, ", "); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->kwarg); }
                write_str(cg, ") {"); write_newline(cg); push_indent(cg);
                /* selfを含め、本体で参照されないパラメータがあると
                 * -Wunused-parameter が -Werror 下でコンパイルエラーになる
                 * （例: def __iter__(self): return iter([1,2,3]) のようにselfを
                 * 使わないメソッド）。常に(void)キャストしておく。 */
                if (method_kind == CG_METHOD_STATIC) write_line(cg, "(void)_p2c_static_self;");
                for (size_t j = 0; j < p2c_vec_len(fd->args); j++) {
                    indent(cg); write_str(cg, "(void)"); write_param_ident(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name); write_str(cg, ";"); write_newline(cg);
                }
                if (fd->vararg) { indent(cg); write_str(cg, "(void)"); write_param_ident(cg, fd->vararg); write_str(cg, ";"); write_newline(cg); }
                if (fd->kwarg) { indent(cg); write_str(cg, "(void)"); write_param_ident(cg, fd->kwarg); write_str(cg, ";"); write_newline(cg); }
                P2C_Map *saved_declared = cg->declared_vars;
                /* メソッドも関数と同じく、入れ子の関数から参照される局所とnonlocalをセルにする */
                P2C_Map *saved_method_cells = cg->cell_names;
                cg->cell_names = collect_nested_nonlocal_cell_names(cg, fd->body);
                cg->declared_vars = p2c_map_new(cg->alloc, p2c_hash_str, p2c_eq_str);
                for (size_t j = 0; j < p2c_vec_len(fd->args); j++) remember_declared(cg, ((P2C_AstArg*)p2c_vec_get(fd->args, j))->name);
                if (fd->vararg) remember_declared(cg, fd->vararg);
                if (fd->kwarg) remember_declared(cg, fd->kwarg);
                /* AOT アンボクシング: メソッド本体も宣言の前に解析する（--unbox）。 */
                P2C_Map *saved_native_int_locals = native_begin_function(cg, fd);
                P2C_Map *method_closure_refs = collect_closure_refs(cg, fd->body);
                predeclare_stmt_list(cg, fd->body);
                add_closure_cells(cg, method_closure_refs);
                p2c_map_free(method_closure_refs);
                emit_cell_declarations(cg, cg->cell_names);
                /* メソッド本体は独立したC関数なので、外側のtry/finallyやループの
                 * コンテキストを引き継がない。 */
                struct P2C_CleanupFrame *saved_method_cleanup_top = cg->cleanup_top;
                int saved_method_loop_depth = cg->loop_depth;
                cg->cleanup_top = NULL;
                cg->loop_depth = 0;
                gen_stmt_list(cg, fd->body);
                native_end_function(cg, saved_native_int_locals);
                cg->cleanup_top = saved_method_cleanup_top;
                cg->loop_depth = saved_method_loop_depth;
                if (p2c_vec_len(fd->body) == 0 || ((P2C_AstStmt*)p2c_vec_get(fd->body, p2c_vec_len(fd->body)-1))->base.type != AST_RETURN) write_line(cg, "return &P2C_None;");
                    p2c_map_free(cg->declared_vars); cg->declared_vars = saved_declared;
                    cg->cell_names = saved_method_cells;
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
                /* メソッドの既定値はクラス本体の実行時に評価済みの隠しグローバルから読む。 */
                char mdefkey[384];
                snprintf(mdefkey, sizeof(mdefkey), "%s__%s", cname, mangle_ident(fd->name));
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
                    if (ma->default_val) emit_default_value(cg, mdefkey, j, ma->default_val);
                    else write_str(cg, "&P2C_None");
                    write_str(cg, ";"); write_newline(cg);
                }
                if (fd->vararg) {
                    indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->vararg); write_str(cg, " = p2c_tuple_new(nargs > "); emit_usize(cg, method_positional_total); write_str(cg, " ? nargs - "); emit_usize(cg, method_positional_total); write_str(cg, " : 0);"); write_newline(cg);
                    indent(cg); write_str(cg, "for (size_t _p2c_var_i = "); emit_usize(cg, method_positional_total); write_str(cg, "; _p2c_var_i < nargs; _p2c_var_i++) p2c_tuple_set("); write_param_ident(cg, fd->vararg); write_str(cg, ", _p2c_var_i - "); emit_usize(cg, method_positional_total); write_str(cg, ", args[_p2c_var_i]);"); write_newline(cg);
                }
                if (fd->kwarg) { indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->kwarg); write_str(cg, " = p2c_dict_new();"); write_newline(cg); }
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
                if (fd->vararg) { if (p2c_vec_len(fd->args)) write_str(cg, ", "); write_param_ident(cg, fd->vararg); }
                if (fd->kwarg) { if (p2c_vec_len(fd->args) || fd->vararg) write_str(cg, ", "); write_param_ident(cg, fd->kwarg); }
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
                    if (ma->default_val) emit_default_value(cg, mdefkey, j, ma->default_val); else write_str(cg, "&P2C_None");
                    write_str(cg, ";"); write_newline(cg);
                    indent(cg); write_str(cg, "bool _p2c_kw_have_"); emit_usize(cg, j - self_count);
                    if ((int)j < fd->kwonly_start) { write_str(cg, " = nargs > "); emit_usize(cg, j - self_count); }
                    else write_str(cg, " = false");
                    write_str(cg, ";"); write_newline(cg);
                    indent(cg); write_str(cg, "for (size_t _p2c_kw_i = 0; _p2c_kw_i < nkw; _p2c_kw_i++) if (strcmp(kw_names[_p2c_kw_i], \""); write_str(cg, ma->name); write_str(cg, "\") == 0) { if (_p2c_kw_have_"); emit_usize(cg, j - self_count); write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"multiple values for method argument\")); _p2c_kw_arg_"); emit_usize(cg, j - self_count); write_str(cg, " = kw_values[_p2c_kw_i]; _p2c_kw_have_"); emit_usize(cg, j - self_count); write_str(cg, " = true; }"); write_newline(cg);
                    if (!ma->default_val) { indent(cg); write_str(cg, "if (!_p2c_kw_have_"); emit_usize(cg, j - self_count); write_str(cg, ") p2c_raise(p2c_make_exception(\"TypeError\", \"missing required method argument\"));"); write_newline(cg); }
                }
                if (fd->vararg) {
                    indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->vararg); write_str(cg, " = p2c_tuple_new(nargs > "); emit_usize(cg, method_positional_total); write_str(cg, " ? nargs - "); emit_usize(cg, method_positional_total); write_str(cg, " : 0);"); write_newline(cg);
                    indent(cg); write_str(cg, "for (size_t _p2c_var_i = "); emit_usize(cg, method_positional_total); write_str(cg, "; _p2c_var_i < nargs; _p2c_var_i++) p2c_tuple_set("); write_param_ident(cg, fd->vararg); write_str(cg, ", _p2c_var_i - "); emit_usize(cg, method_positional_total); write_str(cg, ", args[_p2c_var_i]);"); write_newline(cg);
                }
                if (fd->kwarg) {
                    indent(cg); write_str(cg, "P2C_Object *"); write_param_ident(cg, fd->kwarg); write_str(cg, " = p2c_dict_new();"); write_newline(cg);
                }
                indent(cg); write_str(cg, "for (size_t _p2c_kw_i = 0; _p2c_kw_i < nkw; _p2c_kw_i++) { bool _p2c_known = false;"); write_newline(cg); push_indent(cg);
                for (size_t j = self_count; j < p2c_vec_len(fd->args); j++) {
                    P2C_AstArg *ma = (P2C_AstArg*)p2c_vec_get(fd->args, j);
                    indent(cg); write_str(cg, "if (strcmp(kw_names[_p2c_kw_i], \""); write_str(cg, ma->name); write_str(cg, "\") == 0) _p2c_known = true;"); write_newline(cg);
                }
                if (fd->kwarg) {
                    indent(cg); write_str(cg, "if (!_p2c_known) p2c_dict_set("); write_param_ident(cg, fd->kwarg); write_str(cg, ", p2c_obj_from_str(kw_names[_p2c_kw_i]), kw_values[_p2c_kw_i]);"); write_newline(cg);
                } else {
                    indent(cg); write_str(cg, "if (!_p2c_known) p2c_raise(p2c_make_exception(\"TypeError\", \"unexpected method keyword argument\"));"); write_newline(cg);
                }
                pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
                indent(cg); write_str(cg, "return "); write_str(cg, cname); write_str(cg, "__"); write_str(cg, fd->name); write_str(cg, "(self");
                /* 呼び出しの先頭 "(self" は既に出力済み（staticでは未使用スロット）。 */
                for (size_t k = 0; k + self_count < p2c_vec_len(fd->args); k++) {
                    write_str(cg, ", _p2c_kw_arg_"); emit_usize(cg, k);
                }
                if (fd->vararg) { write_str(cg, ", "); write_param_ident(cg, fd->vararg); }
                if (fd->kwarg) { write_str(cg, ", "); write_param_ident(cg, fd->kwarg); }
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
            indent(cg); write_str(cg, "if (!"); write_str(cg, cname); write_str(cg, ") {"); write_newline(cg); push_indent(cg);
            indent(cg); write_str(cg, cname); write_str(cg, " = p2c_class_new(\""); write_str(cg, n->u.classdef.name); write_str(cg, "\", "); write_str(cg, cname); write_str(cg, "__ctor, "); write_str(cg, cname); write_str(cg, "__methods, ");
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
                if (member->base.type == AST_FUNCTIONDEF) {
                    /* メソッドの既定値をクラス定義時（ここ）で一度だけ評価する。 */
                    P2C_AstFunctionDef *mfd = &member->base.u.functiondef;
                    char mkey[384];
                    snprintf(mkey, sizeof(mkey), "%s__%s", cname, mangle_ident(mfd->name));
                    emit_default_initializers(cg, mkey, mfd);
                    continue;
                }
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
            pop_indent(cg); indent(cg); write_str(cg, "}"); write_newline(cg);
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
                } else {
                    /* Exception/BaseException を直接継承し、自前の __init__ を
                     * 持たないクラスは、Python の Exception.__init__ と同様に
                     * コンストラクタ引数を self.args へタプルで保存する。
                     * これにより str(e) / print(e) がメッセージを表示できる
                     * （以前はインスタンスに引数が残らず <MyError object> に
                     * なっていた）。 */
                    bool is_exception_base = false;
                    for (size_t bi = 0; bi < p2c_vec_len(n->u.classdef.bases); bi++) {
                        P2C_AstExpr *b = (P2C_AstExpr*)p2c_vec_get(n->u.classdef.bases, bi);
                        const char *bn = extract_base_name(b);
                        if (bn && (strcmp(bn, "Exception") == 0 || strcmp(bn, "BaseException") == 0)) { is_exception_base = true; break; }
                    }
                    if (is_exception_base) {
                        indent(cg); write_str(cg, "p2c_setattr(self, \"args\", p2c_tuple_from_array(args, nargs));"); write_newline(cg);
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
    p2c_str_append(cg->header, "/* Generated by Python Code to C Beta0.1 (Py2C B0.1) */\n");
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
        p2c_str_append(cg->forward, " * (docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md 参照)。 */\n");
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
    cg->module_scope_declared = cg->declared_vars;
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
            if (stmt->base.type == AST_FUNCTIONDEF) {
                /* 既定値を def 文の実行時（ここ）で一度だけ評価して保存する。 */
                char dk[192];
                snprintf(dk, sizeof(dk), "%s", mangle_ident(stmt->base.u.functiondef.name));
                emit_default_initializers(cg, dk, &stmt->base.u.functiondef);
            }
            if (stmt->base.type == AST_FUNCTIONDEF && stmt->base.u.functiondef.decorator_list && p2c_vec_len(stmt->base.u.functiondef.decorator_list) > 0) {
                gen_decorator_application(cg, &stmt->base.u.functiondef);
            } else if (stmt->base.type == AST_CLASSDEF && stmt->base.u.classdef.decorator_list && p2c_vec_len(stmt->base.u.classdef.decorator_list) > 0) {
                gen_class_decorator_application(cg, &stmt->base.u.classdef);
            }
        } else {
            gen_stmt(cg, stmt);
        }
    }

    /* 値として参照されたモジュール関数の callable adapter を生成する。
     * 定義本体（cg->toplevel）の末尾へ追加し、本体（main）から参照できる
     * ようにする。decorator として使われた関数は定義直後に adapter を
     * 出しているため、ここでは除外する（二重定義防止）。 */
    if (cg->function_value_names) {
        for (size_t _vi = 0; _vi < cg->function_value_names->bucket_count; _vi++) {
            for (P2C_MapEntry *_ve = cg->function_value_names->buckets[_vi]; _ve; _ve = _ve->next) {
                const char *fname = (const char*)_ve->key;
                if (map_has_name(cg->decorator_callable_names, fname)) continue;
                P2C_AstFunctionDef *vfd = (P2C_AstFunctionDef*)p2c_map_get(cg->func_args, fname);
                if (!vfd) continue;
                P2C_String *saved = cg->current; cg->current = cg->toplevel; cg->indent_level = 0;
                gen_decorator_callable_adapter(cg, vfd);
                cg->current = saved; cg->indent_level = saved_indent;
            }
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
