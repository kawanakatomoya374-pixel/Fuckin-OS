#ifndef PYTHON_CODE_TO_C_RUNTIME_LOCATE_H
#define PYTHON_CODE_TO_C_RUNTIME_LOCATE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `python_code_to_c run <file.py>` や GUI の「実行」機能が、生成したCコードとリンクする
 * ランタイム本体 (python_code_to_c_runtime.c 等) と include/ を見つけるための共通ヘルパー。
 *
 * src/ は機能ごとにサブディレクトリ分けされているため（runtime/, common/,
 * platform/, modules/ ...）、単一のファイル名では探索できない。このヘルパーは
 * その前提を踏まえて4つのソースファイルとincludeディレクトリへのフルパスを
 * まとめて解決する（CLIとGUIで探索ロジックが食い違うのを防ぐため、
 * 実装は1箇所に集約している）。
 */
/* パスは argv0 -> buf -> candidate -> (runtime_c等) の順に、その都度
 * 固定のサブパス文字列を追記しながら組み立てられる。GCCのformat-truncation
 * 検査は「追記元の文字列が、その配列サイズいっぱいの理論上の最大長だった
 * 場合」を仮定して警告するため、各段階の出力バッファは
 * 「一つ前の段階の配列サイズ + 追記されうる最長のサブパス + 余裕」を
 * 確保しておく必要がある（そうしないと、実際のパス長にかかわらず
 * 各段階でtruncation警告が発生する）。 */
#define P2C_RTLOC_TIER1 4096                     /* argv0 をそのまま保持する分 */
#define P2C_RTLOC_TIER2 (P2C_RTLOC_TIER1 + 64)    /* + "/../src" 等の追記分 */
#define P2C_RUNTIME_LOCATE_BUFSZ (P2C_RTLOC_TIER2 + 64) /* + "/platform/python_code_to_c_platform_hosted.c" 等の追記分 */

typedef struct {
    char runtime_c[P2C_RUNTIME_LOCATE_BUFSZ];
    char common_c[P2C_RUNTIME_LOCATE_BUFSZ];
    char platform_core_c[P2C_RUNTIME_LOCATE_BUFSZ];
    char platform_c[P2C_RUNTIME_LOCATE_BUFSZ];
    char pygame_c[P2C_RUNTIME_LOCATE_BUFSZ];
    char include_dir[P2C_RUNTIME_LOCATE_BUFSZ];
} P2C_RuntimeLocation;

/*
 * 探索優先順位:
 *   1. 環境変数 PYTHON_CODE_TO_C_SRC_DIR
 *   2. 実行ファイル(argv0)の隣の ../src
 *   3. カレントディレクトリの src
 * argv0 は NULL でもよい（その場合は2を飛ばす）。
 * 見つかった場合は out を埋めて 1 を返す。見つからなければ 0 を返す。
 */
int p2c_locate_runtime(const char *argv0, P2C_RuntimeLocation *out);

#ifdef __cplusplus
}
#endif

#endif
