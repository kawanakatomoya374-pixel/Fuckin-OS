#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "core/python_code_to_c.h"
#include "tools/python_code_to_c_runtime_locate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
#endif

/* 自己診断用テストケース */
typedef struct {
    const char *name;
    const char *python_code;
} TestCase;

static const TestCase tests[] = {
    {"Hello World", "print('Hello, World!')\n"},
    {"Variable assignment", "x = 10\ny = 20\nz = x + y\n"},
    {"If statement", "x = 10\nif x > 5:\n    print('big')\nelse:\n    print('small')\n"},
    {"While loop", "i = 0\nwhile i < 5:\n    print(i)\n    i = i + 1\n"},
    {"For loop", "for i in range(0, 5, 1):\n    print(i)\n"},
    {"Function definition", "def add(a, b):\n    return a + b\nresult = add(1, 2)\n"},
    {"String operations", "s = 'hello'\nt = 'world'\nu = s + t\n"},
    {"Comparison operators", "a = 10\nb = 20\nc = a < b\nd = a == b\n"},
    {"Containers", "a = [1, 2]\na[1] = 5\nd = {'x': 1}\nt = (1, 2, 3)\nprint(a)\nprint(d['x'])\nprint(t[2])\n"},
    {"Try Except", "try:\n    x = 1 / 0\nexcept ZeroDivisionError as e:\n    print('caught')\n"},
    {"Class", "class Counter:\n    def __init__(self, start):\n        self.value = start\n    def inc(self):\n        self.value = self.value + 1\n        return self.value\nc = Counter(1)\nprint(c.inc())\n"},
    {"Import", "import math\nfrom math import sqrt\nprint(math.pi)\nprint(sqrt(4))\n"},
    {"Varargs (*args/**kwargs)", "def f(a, *args, **kwargs):\n    return len(args) + len(kwargs)\nprint(f(1, 2, 3, x=1, y=2))\n"},
    {"in/not-in, unpack, format", "def add3(a,b,c):\n    return a+b+c\nx=[1,2,3]\nprint(3 in x, 9 not in x, add3(*x), '{}-{}'.format(1,2))\n"},
    {NULL, NULL}
};

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "python_code_to_c %s\n"
            "Usage:\n"
            "  %s <input.py> [-o output.c] [-v] [--comments] [--c11] [--embed-entry NAME]  ... PythonをCコードに変換\n"
            "  %s run [-v] <input.py>            ... 変換・コンパイル・実行をまとめて1コマンドで行う\n"
            "  %s --dump-ast <input.py>          ... パース結果のASTを木構造で表示する（デバッグ用）\n"
            "  %s --self-test\n"
            "  %s --supported\n"
            "\n"
            "  -v, --verbose                     ... 変換の各段階(字句解析/構文解析/意味解析/\n"
            "                                         コード生成)の所要時間や統計をstderrへ出力する\n"
            "  --comments                        ... 生成Cコードに、対応する元のPython行を\n"
            "                                         コメントとして挿入する\n"
            "  --c11                             ... GNU拡張が必要な構文を拒否し、ISO C11対象を明示する\n"
            "  --embed-entry NAME                ... int main() の代わりにカーネルから呼べる\n"
            "                                         P2C_Object *NAME(void) を生成する（自作OS組込み用）\n"
            "  --fallback                        ... 未対応構文を「実行時にNotImplementedErrorを\n"
            "                                         送出するスタブ」へ置き換え、変換を続行する\n"
            "\n"
            "Examples:\n"
            "  %s sample.py -o sample.c\n"
            "  cc -I./include sample.c src/runtime/python_code_to_c_runtime.c src/common/python_code_to_c_common.c src/platform/python_code_to_c_platform.c src/platform/python_code_to_c_platform_hosted.c src/modules/python_code_to_c_pygame.c -lm -o sample\n"
            "  %s run sample.py               ... 上記と同じことを1コマンドで（ファイルを投げるだけ）\n",
            p2c_version_string(), argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static char *read_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    char *buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    if (size > 0 && fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

static int write_text_file(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, fp) == len;
    fclose(fp);
    return ok;
}

static int run_self_test(void) {
    printf("=======================================\n");
    printf("Python Code to C Alpha0.6 / python_code_to_c v%s\n", p2c_version_string());
    printf("=======================================\n\n");

    int passed = 0;
    int failed = 0;
    for (int i = 0; tests[i].name; i++) {
        printf("Test %d: %s ... ", i + 1, tests[i].name);
        char *c_code = NULL;
        P2C_Result r = python_to_c(tests[i].python_code, NULL, &c_code);
        if (r == P2C_OK && c_code) {
            printf("PASS\n");
            passed++;
            free(c_code);
        } else {
            printf("FAIL (%s: %s)\n", p2c_result_to_string(r), p2c_last_error_details());
            failed++;
        }
    }

    printf("=======================================\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    printf("=======================================\n");
    return failed > 0 ? 1 : 0;
}

/* ============================================================
 * run モード: 「投げるだけ」でPythonファイルをC変換→コンパイル→実行する。
 * ============================================================ */

static int path_is_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* ランタイムのソース(python_code_to_c_runtime.c等)が置かれているディレクトリの探索は
 * tools/python_code_to_c_runtime_locate.h の p2c_locate_runtime() に集約されている
 * （GUI版と探索ロジックが食い違わないようにするため）。 */

static int run_mode(const char *argv0, const char *input_path) {
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
    (void)argv0; (void)input_path;
    fprintf(stderr, "Error: run mode requires a hosted build (PYTHON_CODE_TO_C_NO_STDLIB is defined).\n");
    return 1;
#else
    P2C_RuntimeLocation loc;
    if (!p2c_locate_runtime(argv0, &loc)) {
        fprintf(stderr,
            "Error: ランタイムのソース (python_code_to_c_runtime.c 等) が見つかりませんでした。\n"
            "  - プロジェクトのルートディレクトリから実行するか、\n"
            "  - 環境変数 PYTHON_CODE_TO_C_SRC_DIR に src ディレクトリのパスを指定してください。\n"
            "    例: PYTHON_CODE_TO_C_SRC_DIR=/path/to/project/src %s run %s\n",
            argv0 ? argv0 : "python_code_to_c", input_path);
        return 1;
    }

    char *code = read_text_file(input_path);
    if (!code) {
        fprintf(stderr, "Error: cannot read file '%s'\n", input_path);
        return 1;
    }

    char *c_code = NULL;
    P2C_Result r = python_to_c(code, NULL, &c_code);
    free(code);
    if (r != P2C_OK || !c_code) {
        fprintf(stderr, "Error: %s\n", p2c_last_error_details());
        free(c_code);
        return 1;
    }

    char dir_template[] = "/tmp/python_code_to_c_run_XXXXXX";
    char *tmpdir = mkdtemp(dir_template);
    if (!tmpdir) {
        fprintf(stderr, "Error: failed to create a temporary directory for the build.\n");
        free(c_code);
        return 1;
    }

    char c_path[4200], bin_path[4200];
    snprintf(c_path, sizeof(c_path), "%s/python_code_to_c_out.c", tmpdir);
    snprintf(bin_path, sizeof(bin_path), "%s/python_code_to_c_out", tmpdir);

    bool wrote_ok = write_text_file(c_path, c_code);
    free(c_code);
    if (!wrote_ok) {
        fprintf(stderr, "Error: cannot write temporary file '%s'\n", c_path);
        return 1;
    }

    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "cc";

    /* プロジェクトパスに空白が含まれるケースを考慮し、全パスをクォートする。
     * 各パスは理論上4000バイト超になりうる(P2C_RuntimeLocation参照)ため、
     * 固定長バッファだとGCCのformat-truncation検査(-Werror)を満たせない。
     * 実際の長さから必要サイズを計算して動的確保する。 */
    size_t cmd_cap = strlen(cc) + strlen(loc.include_dir) + strlen(c_path) +
        strlen(loc.runtime_c) + strlen(loc.common_c) + strlen(loc.platform_core_c) +
        strlen(loc.platform_c) + strlen(loc.pygame_c) + strlen(bin_path) + 80;
    char *cmd = (char*)malloc(cmd_cap);
    char compile_log[8192] = {0};
    if (!cmd) {
        fprintf(stderr, "Error: out of memory building compile command.\n");
        return 1;
    }
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
        fprintf(stderr, "Error: generated C code failed to compile.\n");
        if (compile_log[0]) fprintf(stderr, "%s\n", compile_log);
        fprintf(stderr, "(生成されたCコードは残しています: %s)\n", c_path);
        return 1;
    }

    char run_cmd[4300];
    snprintf(run_cmd, sizeof(run_cmd), "\"%s\"", bin_path);
    int rc = system(run_cmd);

    if (!getenv("PYTHON_CODE_TO_C_RUN_KEEP_TMP")) {
        remove(c_path);
        remove(bin_path);
        rmdir(tmpdir);
    }

#ifdef WIFEXITED
    if (rc != -1 && WIFEXITED(rc)) return WEXITSTATUS(rc);
#endif
    return rc;
#endif
}

int main(int argc, char *argv[]) {

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--self-test") == 0) {
        return run_self_test();
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "--supported") == 0) {
        p2c_print_supported_range();
        return 0;
    }
    if (strcmp(argv[1], "run") == 0) {
        const char *run_input = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
                p2c_set_verbose(true);
            } else if (!run_input) {
                run_input = argv[i];
            }
        }
        if (!run_input) {
            fprintf(stderr, "Error: usage: %s run [-v] <input.py>\n", argv[0]);
            return 1;
        }
        return run_mode(argv[0], run_input);
    }
    if (strcmp(argv[1], "--dump-ast") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: usage: %s --dump-ast <input.py>\n", argv[0]);
            return 1;
        }
        char *code = read_text_file(argv[2]);
        if (!code) { fprintf(stderr, "Error: cannot read file '%s'\n", argv[2]); return 1; }
        char *dump = NULL;
        P2C_Result r = python_to_ast_dump(code, &dump);
        free(code);
        if (r != P2C_OK || !dump) {
            fprintf(stderr, "Error: %s\n", p2c_last_error_details());
            free(dump);
            return 1;
        }
        fputs(dump, stdout);
        free(dump);
        return 0;
    }

    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *embed_entry = NULL;
    bool want_comments = false;
    bool want_strict_c11 = false;
    bool want_fallback = false;   /* 未対応構文をランタイムスタブへ置換 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires a path\n");
                return 1;
            }
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--embed-entry") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --embed-entry requires a C identifier\n");
                return 1;
            }
            embed_entry = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            p2c_set_verbose(true);
        } else if (strcmp(argv[i], "--comments") == 0) {
            want_comments = true;
        } else if (strcmp(argv[i], "--c11") == 0) {
            want_strict_c11 = true;
        } else if (strcmp(argv[i], "--fallback") == 0) {
            /* 未対応構文でも変換・ビルドを失敗させない（実行時に
             * NotImplementedError を送出するスタブを生成する）。 */
            want_fallback = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "Error: missing input path\n");
        return 1;
    }

    char *code = read_text_file(input_path);
    if (!code) {
        fprintf(stderr, "Error: cannot read file '%s'\n", input_path);
        return 1;
    }

    char *c_code = NULL;
    P2C_TranspileOptions tr_opts = P2C_DEFAULT_TRANSPILER_OPTIONS;
    tr_opts.debug_comments = want_comments;
    tr_opts.strict_c11 = want_strict_c11;
    tr_opts.fallback_unsupported = want_fallback;
    tr_opts.embed_entry = embed_entry;
    P2C_Result r = python_to_c(code, &tr_opts, &c_code);
    free(code);

    if (r != P2C_OK || !c_code) {
        fprintf(stderr, "Error: %s\n", p2c_last_error_details());
        free(c_code);
        return 1;
    }

    if (output_path) {
        if (!write_text_file(output_path, c_code)) {
            fprintf(stderr, "Error: cannot write file '%s'\n", output_path);
            free(c_code);
            return 1;
        }
    } else {
        fputs(c_code, stdout);
    }

    free(c_code);
    return 0;
}
