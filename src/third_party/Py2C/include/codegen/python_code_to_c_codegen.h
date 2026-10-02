#ifndef PYTHON_CODE_TO_C_CODEGEN_H
#define PYTHON_CODE_TO_C_CODEGEN_H

#include "semantic/python_code_to_c_semantic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool baremetal;
    bool use_gc;
    bool use_exceptions;
    bool debug_info;
    bool strict_c11;
    int indent_width;
    const char *runtime_prefix;
    /* NULL以外ならint main(void)の代わりに、その名前のカーネル呼び出し可能な
     * エントリ "P2C_Object *<name>(void)" を生成する（--embed-entry）。
     * ランタイム初期化・GC初期化・shutdownは呼び出し側(カーネル)の責務になり、
     * 生成エントリはモジュールグローバルのルート登録と本体実行だけを行う。 */
    const char *embed_entry;
    /* 未対応構文を「実行時に NotImplementedError を送出するスタブ」へ
     * 置き換えて変換を続行する（--fallback）。既定falseは変換エラー。 */
    bool fallback_unsupported;
} P2C_CodeGenOptions;

extern const P2C_CodeGenOptions P2C_DEFAULT_OPTIONS;

typedef struct P2C_CodeGen P2C_CodeGen;

/* return/break/continueが脱出する途中で実行すべき後処理（try/finallyの
 * finally本体と例外フレームの復元）を表す。定義はコード生成側にある。 */
struct P2C_CleanupFrame;

struct P2C_CodeGen {
    P2C_Allocator *alloc;
    P2C_CodeGenOptions opts;
    P2C_SymbolTable *symtab;
    P2C_String *header;
    P2C_String *forward;
    /* forward 書き込み中（関数本体や外側ラムダの生成中）に現れたラムダ等の定義を
     * 退避しておく待ち行列。最終組み立てで forward の末尾へ連結する。 */
    P2C_Vector *deferred_defs;
    P2C_String *body;
    P2C_String *toplevel;
    P2C_String *current;
    int indent_level;
    int temp_counter;
    int active_loop_id;
    struct P2C_CleanupFrame *cleanup_top; /* 生成中の脱出対象try/finallyの連結リスト */
    int loop_depth;                       /* ループ入れ子数（break/continueの脱出範囲判定） */
    int function_depth;
    P2C_Map *declared_vars;
    P2C_Map *known_classes;
    P2C_Map *module_globals; /* モジュールトップレベルで代入される単純名の集合。global文の解決に使う。 */
    P2C_Map *class_init_adapter; /* クラス名 -> 解決済み__init__アダプタ関数名（自身 or 継承元）。値なしはNULLエントリ扱い。 */
    P2C_Map *func_args; /* 関数名 -> P2C_AstFunctionDef*（デフォルト引数・*args / **kwargsの補完に使用） */
    P2C_Map *decorated_names; /* decorator適用後にP2C callable objectへ再束縛されるmodule-level定義名の集合 */
    P2C_Map *module_function_names; /* direct module-level function名の集合。decorator expressionをP2C callable adapterへ解決する */
    P2C_Map *decorator_callable_names; /* bare-name decoratorとして実際に参照されるmodule-level function名の集合 */
    P2C_Map *class_methods; /* クラス名 -> (メソッド名 -> 1) のP2C_Map。super()がどのクラスにメソッドが実際に定義されているか調べるのに使用 */
    const char *current_class;      /* 現在コード生成中のメソッドが属するクラス名（トップレベル関数ではNULL）。super()解決に使用 */
    int lambda_counter; /* lambda式ごとに一意なC関数名を振るためのカウンタ */
    int generator_expression_counter; /* generator expressionごとに一意なC step関数名を振るためのカウンタ */
    /* ネストクラス定義（class本体の直下に置いたclass）の解決用状態。
     * C名は外側クラスを前置した "Outer__Inner" にして衝突を避ける。
     * scan_class_cnameはscan_stmt走査中の現在クラスのC名（トップレベルではNULL）、
     * nested_class_aliasesは生成中のクラスで可視な「Python名 -> C名」マップ、
     * in_class_bodyはそのマップをクラス本体の文（__classobj内）で評価しているかを表す。
     * Python仕様ではクラススコープの名前はメソッド本体から見えないため、
     * in_class_bodyがfalseのときに名前を参照したら明示的な診断を出す。 */
    const char *scan_class_cname;
    P2C_Map *nested_class_aliases;
    bool in_class_body;
    P2C_Map *closure_env_names;
    P2C_Map *nonlocal_names;
    P2C_Map *cell_names;
    const char *closure_env_var;
    /* ジェネレータ式・状態機械関数（async/generator）のステップ関数を生成して
     * いる間だけ設定される、Pythonのローカル名 -> ジェネレータのローカル辞書。
     * gen_exprの名前解決がこの表を優先することで、組込み関数呼び出しや
     * タプル/辞書表示の中でも genexpr のローカル名を正しく解決できる。 */
    P2C_Map *genexpr_locals;
    P2C_Result last_error;
    char *error_msg;
    const char *source_text; /* debug_info有効時に元のPython行をコメント挿入するために使う（NULL可） */
};

P2C_CodeGen* p2c_codegen_new(P2C_Allocator *a, P2C_CodeGenOptions *opts, P2C_SymbolTable *symtab);
void p2c_codegen_free(P2C_CodeGen *cg);
const char* p2c_codegen_error_msg(P2C_CodeGen *cg);
P2C_Result p2c_codegen_generate(P2C_CodeGen *cg, P2C_AstModule *mod, char **out_code);
P2C_Result p2c_codegen_stmt(P2C_CodeGen *cg, P2C_AstStmt *stmt);
P2C_Result p2c_codegen_expr(P2C_CodeGen *cg, P2C_AstExpr *expr, P2C_String *out);

/**
 * debug_info有効時に生成Cへ挿入するコメントの元となる、Pythonソース全文を設定する。
 * 呼ばなければ従来通りコメントは行番号のみになる。
 */
void p2c_codegen_set_source(P2C_CodeGen *cg, const char *source_text);

#ifdef __cplusplus
}
#endif

#endif
