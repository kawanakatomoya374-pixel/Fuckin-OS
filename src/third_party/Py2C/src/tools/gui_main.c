/*
 * gui_main.c - "Python Code to C" 統合GUI
 *
 * 組み込み利用を想定し、依存関係を最小限にしています:
 *   - 標準Cライブラリのみ（ncurses等の外部GUIライブラリ不使用）
 *   - libpython_code_to_c.a (python_to_c) と c2py.c (c_to_python) を直接リンクして
 *     サブプロセスを起動せずに変換する（fork/execを使わないため、
 *     プロセス生成が制限された組み込み環境にも移植しやすい）
 *   - 例外として、任意機能の「その場でコンパイル・実行」だけは
 *     Cコンパイラ(cc)をpopen(3)/system(3)経由で呼び出す。この機能は
 *     コンパイラとプロセス生成が使える環境でのみ利用可能で、使わなければ
 *     変換処理自体はサブプロセス不要のまま動作する。
 *
 * 画面遷移はシンプルなテキストメニュー方式です。ANSIエスケープに
 * 対応した端末であれば見出し部分を色付けしますが、対応していない
 * 端末でも問題なく動作するよう出力はプレーンテキストにフォールバック
 * 可能な作りにしています。
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "tools/python_code_to_c_gui_web.h"
#include "tools/python_code_to_c_httpd.h"
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "core/python_code_to_c.h"
#include "tools/c2py.h"
#include "tools/python_code_to_c_runtime_locate.h"

#define MAX_SRC (1024 * 1024) /* 1MB まで */

static const char *g_argv0 = "python_code_to_c_gui";

static int path_is_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* ランタイムのソース(python_code_to_c_runtime.c等)が置かれているディレクトリの探索は
 * tools/python_code_to_c_runtime_locate.h の p2c_locate_runtime() に集約されている
 * （CLI版と探索ロジックが食い違わないようにするため）。 */

/* 生成済みのCコード文字列をコンパイルして実行する（this requires a C compiler
 * とfork/execが使える環境であることが前提。GUI本体の変換処理自体は
 * サブプロセスを使わないが、この「実行」機能だけは例外的にcc/system(3)を使う）。 */
static void compile_and_run_c(const char *c_code) {
    P2C_RuntimeLocation loc;
    if (!p2c_locate_runtime(g_argv0, &loc)) {
        printf("ランタイムのソース (python_code_to_c_runtime.c 等) が見つかりませんでした。\n");
        printf("環境変数 PYTHON_CODE_TO_C_SRC_DIR に src ディレクトリのパスを指定するか、\n");
        printf("プロジェクトのルートディレクトリから実行してください。\n");
        return;
    }
    char dir_template[] = "/tmp/python_code_to_c_gui_run_XXXXXX";
    char *tmpdir = mkdtemp(dir_template);
    if (!tmpdir) { printf("一時ディレクトリの作成に失敗しました。\n"); return; }

    char c_path[4200], bin_path[4200];
    snprintf(c_path, sizeof(c_path), "%s/python_code_to_c_out.c", tmpdir);
    snprintf(bin_path, sizeof(bin_path), "%s/python_code_to_c_out", tmpdir);

    FILE *f = fopen(c_path, "wb");
    if (!f) { printf("一時ファイルの書き込みに失敗しました。\n"); return; }
    fwrite(c_code, 1, strlen(c_code), f);
    fclose(f);

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";
    /* 各パスは理論上4000バイト超になりうる(P2C_RuntimeLocation参照)ため、
     * 固定長バッファだとGCCのformat-truncation検査(-Werror)を満たせない。
     * 実際の長さから必要サイズを計算して動的確保する。 */
    size_t cmd_cap = strlen(cc) + strlen(loc.include_dir) + strlen(c_path) +
        strlen(loc.runtime_c) + strlen(loc.common_c) + strlen(loc.platform_core_c) +
        strlen(loc.platform_c) + strlen(loc.pygame_c) + strlen(bin_path) + 80;
    char *cmd = (char*)malloc(cmd_cap);
    char compile_log[8192] = {0};
    if (!cmd) { printf("コマンド生成用のメモリ確保に失敗しました。\n"); return; }
#if defined(__GNUC__) && !defined(__clang__)
/* バッファ長は直上のstrlen計算から求めているため実際に切り詰めは起きないが、

 * GCCの範囲解析は「char[4096]のフィールドは最大4095バイト」と仮定するため

 * -Wformat-truncation を誤検出する。この呼び出しに限り明示的に抑止する。 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
    snprintf(cmd, cmd_cap,
        "%s -I\"%s\" -std=c11 \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" -lm -o \"%s\" 2>&1",
        cc, loc.include_dir, c_path, loc.runtime_c, loc.common_c, loc.platform_core_c, loc.platform_c, loc.pygame_c, bin_path);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    FILE *cc_out = popen(cmd, "r");
    free(cmd);
    if (cc_out) {
        size_t n = fread(compile_log, 1, sizeof(compile_log) - 1, cc_out);
        compile_log[n] = '\0';
        pclose(cc_out);
    }
    if (!path_is_file(bin_path)) {
        printf("コンパイルに失敗しました:\n%s\n", compile_log);
        printf("(生成されたCコードは残しています: %s)\n", c_path);
        return;
    }
    printf("--- 実行結果 ---\n");
    fflush(stdout);
    char run_cmd[4300];
    snprintf(run_cmd, sizeof(run_cmd), "\"%s\"", bin_path);
    if (system(run_cmd) == -1) {
        printf("(実行コマンドの起動に失敗しました)\n");
    }
    printf("--- 実行終了 ---\n");

    if (!getenv("PYTHON_CODE_TO_C_RUN_KEEP_TMP")) { remove(c_path); remove(bin_path); rmdir(tmpdir); }
}

static char *read_line_dyn(void); /* 後方で定義されている既存ヘルパーを再利用する */

static int prompt_yes_no(const char *question) {
    printf("%s (y/N): ", question);
    fflush(stdout);
    char *sel = read_line_dyn();
    int yes = (sel && sel[0] && (sel[0] == 'y' || sel[0] == 'Y'));
    free(sel);
    return yes;
}

static int g_use_color = 1;

static void hr(void) {
    printf("--------------------------------------------------------------\n");
}

static void banner(void) {
    if (g_use_color) printf("\x1b[1;36m");
    printf("============================================================\n");
    printf("  Python Code to C / C to Python - Converter GUI (Alpha0.6)\n");
    printf("============================================================\n");
    if (g_use_color) printf("\x1b[0m");
}

static void print_version(void) {
    printf("python_code_to_c core version: %s\n", p2c_version_string());
}

static char *read_line_dyn(void) {
    size_t cap = 128, len = 0;
    char *buf = (char*)malloc(cap);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (len + 1 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    buf[len] = '\0';
    if (c == EOF && len == 0) { free(buf); return NULL; }
    return buf;
}

/* ファイルパスを尋ねて全内容を読み込む。失敗時はNULL。 */
static char *prompt_read_file(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char *path = read_line_dyn();
    if (!path || path[0] == '\0') { free(path); return NULL; }
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("ファイルを開けませんでした: %s\n", path);
        free(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > MAX_SRC) {
        printf("ファイルサイズが対応範囲外です。\n");
        fclose(f); free(path);
        return NULL;
    }
    char *buf = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    free(path);
    return buf;
}

/* 標準入力から複数行を読み、単独行の "." で入力終了とする貼り付けモード */
static char *prompt_paste_code(void) {
    printf("コードを貼り付けてください。入力終了は単独行で '.' とだけ入力してください。\n");
    size_t cap = 4096, len = 0;
    char *buf = (char*)malloc(cap);
    buf[0] = '\0';
    char *line;
    while ((line = read_line_dyn()) != NULL) {
        if (strcmp(line, ".") == 0) { free(line); break; }
        size_t ll = strlen(line);
        if (len + ll + 2 >= cap) { while (len + ll + 2 >= cap) cap *= 2; buf = (char*)realloc(buf, cap); }
        memcpy(buf + len, line, ll);
        len += ll;
        buf[len++] = '\n';
        buf[len] = '\0';
        free(line);
    }
    return buf;
}

static void save_to_file(const char *content) {
    printf("保存先ファイルパス（空Enterで保存をスキップ）: ");
    fflush(stdout);
    char *path = read_line_dyn();
    if (!path || path[0] == '\0') { free(path); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("保存に失敗しました: %s\n", path); free(path); return; }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    printf("保存しました: %s\n", path);
    free(path);
}

static char *get_source_input(void) {
    printf("1) ファイルから読み込む\n");
    printf("2) その場に貼り付ける\n");
    printf("選択: ");
    fflush(stdout);
    char *sel = read_line_dyn();
    char *src = NULL;
    if (sel && strcmp(sel, "1") == 0) {
        src = prompt_read_file("入力ファイルパス: ");
    } else {
        src = prompt_paste_code();
    }
    free(sel);
    return src;
}

static void do_python_to_c(void) {
    hr();
    printf("[ Python -> C 変換 ]\n");
    char *src = get_source_input();
    if (!src || src[0] == '\0') { printf("入力が空です。中止します。\n"); free(src); return; }

    char *c_code = NULL;
    P2C_Result r = python_to_c(src, NULL, &c_code);
    free(src);
    if (r != P2C_OK) {
        printf("変換に失敗しました: %s\n", p2c_result_to_string(r));
        const char *detail = p2c_last_error_details();
        if (detail && detail[0]) printf("詳細: %s\n", detail);
        free(c_code);
        return;
    }
    hr();
    printf("%s\n", c_code);
    hr();
    if (prompt_yes_no("この場でコンパイルして実行しますか？（Cコンパイラが必要です）")) {
        compile_and_run_c(c_code);
        hr();
    } else {
        printf("生成されたCコードを手動でコンパイル・実行するには、以下のようにしてください:\n");
        printf("  cc -I<include> <output.c> src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c -lm -o a.out\n\n");
    }
    save_to_file(c_code);
    free(c_code);
}

static void do_c_to_python(void) {
    hr();
    printf("[ C -> Python 変換（簡易版） ]\n");
    printf("注意: これはトークンベースの簡易変換です。ポインタ演算・構造体・\n");
    printf("マクロ等は完全には変換されません。生成結果は必ず確認してください。\n\n");
    char *src = get_source_input();
    if (!src || src[0] == '\0') { printf("入力が空です。中止します。\n"); free(src); return; }

    char *py_code = NULL;
    int r = c_to_python(src, &py_code);
    free(src);
    if (r != 0 || !py_code) {
        printf("変換に失敗しました。\n");
        free(py_code);
        return;
    }
    hr();
    printf("%s\n", py_code);
    hr();
    save_to_file(py_code);
    free(py_code);
}

static void show_menu(void) {
    banner();
    print_version();
    hr();
    printf("1) Python -> C 変換\n");
    printf("2) C -> Python 変換（簡易版）\n");
    printf("3) 対応構文・機能一覧を表示\n");
    printf("4) 終了\n");
    hr();
    printf("選択: ");
    fflush(stdout);
}

/* デフォルト動作（テキストメニューUI）。
 * Webサーバー式GUIは --web / --port=N / --host=... オプションで起動する。 */
static void run_tui(void) {
    for (;;) {
        show_menu();
        char *sel = read_line_dyn();
        if (!sel) { printf("\n"); break; }
        if (strcmp(sel, "1") == 0) do_python_to_c();
        else if (strcmp(sel, "2") == 0) do_c_to_python();
        else if (strcmp(sel, "3") == 0) { hr(); p2c_print_supported_range(); }
        else if (strcmp(sel, "4") == 0 || strcmp(sel, "q") == 0 || strcmp(sel, "exit") == 0) { free(sel); break; }
        else printf("不正な選択です。\n");
        free(sel);
        printf("\n");
    }
    printf("終了します。\n");
}

int main(int argc, char **argv) {
    g_argv0 = argv[0];

    bool web_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--web") == 0) web_mode = true;
        if (strcmp(argv[i], "--no-color") == 0) g_use_color = 0;
        /* --port=N / --host=... があれば暗黙的にWebモードとみなす（利便性のため）*/
        if (strncmp(argv[i], "--port=", 7) == 0) web_mode = true;
        if (strncmp(argv[i], "--host=", 7) == 0) web_mode = true;
        if (strcmp(argv[i], "--no-browser") == 0) web_mode = true;
    }

    if (web_mode) {
        return p2c_gui_web_main(argc, argv);
    }

    /* デフォルト: 元のテキストメニューUI */
    run_tui();
    return 0;
}
