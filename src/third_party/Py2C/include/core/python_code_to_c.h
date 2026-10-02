#ifndef PYTHON_CODE_TO_C_H
#define PYTHON_CODE_TO_C_H

#include "common/python_code_to_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * python_code_to_c メインAPI
 * Pythonコードの文字列をCコードの文字列に変換
 * ======================================== */

/* バージョン情報 */
#define PYTHON_CODE_TO_C_VERSION_MAJOR 0
#define PYTHON_CODE_TO_C_VERSION_MINOR 4
#define PYTHON_CODE_TO_C_VERSION_PATCH 0
#define PYTHON_CODE_TO_C_VERSION_STRING "Alpha0.6"

/* 変換オプション */
typedef struct {
    bool baremetal;         /* ベアメタルモード */
    bool include_runtime;   /* ランタイムコードを含める */
    bool debug_comments;    /* 元のPythonコードをコメントとして挿入 */
    bool strict_c11;        /* GNU拡張を使う構文を拒否するISO C11モード */
    /* 未対応構文のフォールバック（--fallback）。
     * true のとき、codegenが扱えない構文を「実行時に NotImplementedError を
     * 送出するスタブ」へ置き換えて変換を続行する（ビルドは失敗させない）。
     * false（既定）では、黙って不正な値を作らず変換エラーとして報告する。 */
    bool fallback_unsupported;
    int indent_spaces;      /* 出力Cコードのインデント */
    /* NULL以外なら int main(void) の代わりに、カーネルから呼び出せる
     * "P2C_Object *<name>(void)" を生成する（CLI: --embed-entry <name>）。
     * ランタイム/GCの初期化はカーネル側の責務になる。
     * docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md 参照。 */
    const char *embed_entry;
} P2C_TranspileOptions;

/* デフォルトオプション */
extern const P2C_TranspileOptions P2C_DEFAULT_TRANSPILER_OPTIONS;

/* ========================================
 * メイン変換関数
 * ======================================== */

/**
 * PythonコードをCコードに変換
 * @param python_code 入力Pythonソースコード（NULL終端文字列）
 * @param options 変換オプション（NULLでデフォルト使用）
 * @param out_c_code 出力Cコード（malloc/freeで管理、呼び出し側で解放）
 * @return 成功時P2C_OK、失敗時はエラーコード
 *
 * 使用例:
 *   char *c_code = NULL;
 *   P2C_Result r = python_to_c("print('Hello')", NULL, &c_code);
 *   if (r == P2C_OK) {
 *       printf("%s\n", c_code);
 *       free(c_code);
 *   }
 */
P2C_Result python_to_c(const char *python_code, P2C_TranspileOptions *options, char **out_c_code);

/**
 * Pythonコードを字句解析・構文解析し、ASTを木構造テキストとして返す
 * （--dump-ast オプション用）。意味解析・コード生成は行わない。
 * 呼び出し側は使用後 free() すること。
 */
P2C_Result python_to_ast_dump(const char *python_code, char **out_dump);

/**
 * 変換エラーメッセージを取得
 * @param result 変換関数の戻り値
 * @return 人間可読なエラーメッセージ
 */
const char* p2c_result_to_string(P2C_Result result);

/**
 * 最後のエラーの詳細メッセージを取得（行番号・列番号付き）
 * @return エラーメッセージ文字列（内部バッファ、解放不要）
 */
const char* p2c_last_error_details(void);

/**
 * バージョン文字列を取得
 */
const char* p2c_version_string(void);

/**
 * 変換過程を段階ごとにstderrへログ出力するかどうかを設定する
 * （--verbose オプション用）。既定は無効。プロセスグローバルな設定。
 */
void p2c_set_verbose(bool enabled);

/**
 * 対応構文・機能一覧を標準出力に表示する。
 * CLI(--supported)とGUIの両方から共通で呼び出される。
 */
void p2c_print_supported_range(void);

/**
 * 対応構文・機能一覧を文字列として取得する（printfせず取得したい場合用。
 * 内部の静的バッファを返すため、呼び出し側でのfreeは不要）。
 */
const char* p2c_supported_range_string(void);

/**
 * 直近の変換が静的フォールバックヒープ（P2C_COMPILER_FALLBACK_HEAP_SIZE）を
 * 使ったかどうか。
 *
 * 自作OS/組込みでは、p2c_set_default_allocator() または
 * p2c_platform_set_allocator() で OS のヒープを注入しておき、ここが
 * false であることを確認する（true なら注入が効いておらず、変換器が
 * 変換器自身の静的バッファを使っている）。
 */
bool p2c_core_static_allocator_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_H */
