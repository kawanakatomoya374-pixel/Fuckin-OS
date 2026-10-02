#include "core/python_code_to_c.h"
#include "lexer/python_code_to_c_lexer.h"
#include "parser/python_code_to_c_parser.h"
#include "semantic/python_code_to_c_semantic.h"
#include "codegen/python_code_to_c_codegen.h"
#include "parser/python_code_to_c_astdump.h"
#include <stddef.h>

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#endif

/* デフォルトオプション */
/* 指定初期化子（C99）で書く。フィールドを追加しても順序に依存せず、
     * -Wmissing-field-initializers も出ない（未指定は0/NULLで初期化される）。 */
const P2C_TranspileOptions P2C_DEFAULT_TRANSPILER_OPTIONS = {
    .baremetal = true,
    .include_runtime = true,
    .debug_comments = false,
    .strict_c11 = false,
    .fallback_unsupported = false,
    .indent_spaces = 4,
    .embed_entry = NULL
};

/* スレッドローカルエラーバッファ（エラー行のソース表示・キャレット表示を
 * 含められるよう、メッセージ本文だけの頃より余裕を持たせてある） */
static P2C_THREAD_LOCAL char last_error_buf[2048] = {0};

/* ========================================
 * --verbose 用ロギング
 * ======================================== */
static bool g_verbose = false;

void p2c_set_verbose(bool enabled) {
    g_verbose = enabled;
}

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
static double vlog_elapsed_ms(clock_t start) {
    return (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
}
static void p2c_vlog(clock_t start, const char *fmt, ...) {
    if (!g_verbose) return;
    fprintf(stderr, "[python_code_to_c][%6.1fms] ", vlog_elapsed_ms(start));
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}
#define VLOG(start, ...) p2c_vlog((start), __VA_ARGS__)
#else
#define VLOG(start, ...) do { (void)(start); } while (0)
#endif

const char* p2c_result_to_string(P2C_Result result) {
    switch (result) {
        case P2C_OK: return "Success";
        case P2C_ERR_NOMEM: return "Out of memory";
        case P2C_ERR_SYNTAX: return "Syntax error";
        case P2C_ERR_SEMANTIC: return "Semantic error";
        case P2C_ERR_IO: return "I/O error";
        case P2C_ERR_INTERNAL: return "Internal error";
        case P2C_ERR_NOT_IMPLEMENTED: return "Not implemented";
        default: return "Unknown error";
    }
}

const char* p2c_last_error_details(void) {
    return last_error_buf;
}

/* エラーメッセージを「該当行のソースコード + 該当列を指す^」付きで整形する
 * （GCC/Rustのような表示）。src が NULL、該当行が見つからない、または
 * バッファが足りない場合は、通常の1行サマリのみを書いて安全側に倒す。 */
static void format_error_with_source(char *out, size_t out_sz, const char *kind,
                                      const char *src, uint32_t line, uint32_t col,
                                      const char *msg) {
    int n = snprintf(out, out_sz, "%s at line %u, col %u: %s", kind, line, col,
                      msg ? msg : "unknown error");
    if (n < 0 || (size_t)n >= out_sz || !src || line == 0) return;

    /* srcからline行目（1始まり）を探す */
    const char *p = src;
    uint32_t cur = 1;
    while (cur < line && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return; /* 行が見つからない(EOF等) */
    const char *line_start = p;
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n') line_end++;
    size_t line_len = (size_t)(line_end - line_start);
    if (line_len > 200) line_len = 200; /* 極端に長い行は安全のため切り詰める */

    size_t used = strlen(out);
    /* 出力は out_sz で有界なので、切り詰めは仕様どおり（-Wformat-truncation の
     * 誤検出を避けるため、snprintfを使わず明示的な長さで書き込む）。 */
    {
        const char *prefix = "\n\n    ";
        const char *suffix = "\n    ";
        for (size_t i = 0; prefix[i] && used + 1 < out_sz; i++) out[used++] = prefix[i];
        for (size_t i = 0; i < line_len && used + 1 < out_sz; i++) out[used++] = line_start[i];
        for (size_t i = 0; suffix[i] && used + 1 < out_sz; i++) out[used++] = suffix[i];
        out[used] = '\0';
    }
    /* colは1始まり。範囲外なら行頭に^を置く */
    size_t caret_pos = (col >= 1 && (size_t)(col - 1) <= line_len) ? (size_t)(col - 1) : 0;
    for (size_t i = 0; i < caret_pos && used + 1 < out_sz; i++) out[used++] = ' ';
    if (used + 1 < out_sz) { out[used++] = '^'; out[used] = '\0'; }
}

const char* p2c_version_string(void) {
    return PYTHON_CODE_TO_C_VERSION_STRING;
}

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
/* 固定文字列を容量確認つきで連結する。ISO C99が保証する1つの文字列リテラルは
 * 4095バイトまでなので、対応一覧は複数のリテラルへ分割してここで連結する
 * （末尾NULを含めて初めて書き込み、入りきらない場合は何もしない）。
 * freestanding（PYTHON_CODE_TO_C_NO_STDLIB）では機能一覧を生成しないため、
 * 使われない関数にならないよう同じ条件で囲む。 */
static size_t p2c_append_literal(char *dst, size_t cap, size_t used, const char *src, size_t len) {
    if (!dst || !src || used + len + 1u > cap) return used;
    memcpy(dst + used, src, len + 1u);
    return used + len;
}
#endif

const char* p2c_supported_range_string(void) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    /* 機能一覧は分割リテラルで追記するため、合計（約6KB）を収める容量を確保する。
     * 溢れた場合は p2c_append_literal が静かに追記を止めるので、配列はここで
     * 十分な大きさを確保しておく。 */
    static char buf[8192];
    static bool built = false;
    if (!built) {
        snprintf(buf, sizeof(buf),
            "python_code_to_c %s - 対応構文・機能一覧\n"
            "============================================================\n"
            "\n"
            "[対応済み]\n"
            "  文:\n"
            "    if / elif / else, while (while/for else節含む), for <var> in range(...), for <var> in <list/tuple/str>、for starred unpack\n"
            "    def（デフォルト引数・キーワード引数・*args・**kwargs・キーワード専用引数対応）, return（複数値のタプル戻り値含む）, class, try / except / else / finally（try本体・except節から脱出するreturn/break/continueはfinallyを実行してから脱出）, bare raise\n"
            "    try / except / else / finally の finally 内 return/break/continue（保留中の例外や return を上書きし、finally を実行してから脱出する）\n"
            "    match / case（literal、None、capture、wildcard、sequence/mapping/class/as/star、or-pattern、if guard）, import <mod>, from <mod> import <name>, pass, break, continue, assert, global\n"
            "    代入 (=), 代入式 (name := value), 複合代入 (+= -= *= /= //= %%=、属性・添字ターゲット含む), タプル/Starred unpack代入 (a, *mid, z = seq)、for (a, *mid, z) in seq\n"
            "    複数代入 (a = b = c = 1), セミコロン区切りの複数文 (a=1; b=2)\n"
            "  式:\n"
            "    数値(int/float)・文字列・bool・None, list/dict/tuple/set リテラル, list/dict/set内包表記、隣接文字列リテラルの暗黙連結\n"
            "    算術・比較・論理・集合演算子、dictマージ (d1 | d2, d1 |= d2), 三項式 (x if c else y), f-string (f\"...\"), lambda (lambda x, y: x + y)\n"
            "    添字・スライス・属性アクセス、listスライスの代入・+=・del\n"
            "    ジェネレータ式 (x for x in ys): 複数for節・タプルターゲット・ifフィルタに対応し、\n"
            "      最も外側のiterableだけを生成時に評価するPythonの規則にも従う\n"
            "    class継承（メソッド・__init__の継承、多段階継承、明示的な基底クラス呼び出し ClassName.method(self,...)）\n"
            "    関数呼び出しでのキーワード引数 (foo(a=1, b=2))\n"
            "    *args（可変長位置引数）・**kwargs（可変長キーワード引数）: 関数・ネスト関数・クラスメソッドに対応\n"
            "      裸の * によるキーワード専用引数: def f(a, *, b, c=1)、メソッド呼び出しの名前付き引数・**mapping展開に対応\n"
            "    呼び出し側での *args / **kwargs アンパック (f(*lst), f(**d))（既知の関数に対して）\n"
            "    in / not in（list/tuple/str/dictキー）, is / is not, 連鎖比較 (1 < x < 10)\n"
            "  組み込み関数:\n"
            "    print (sep=/end=対応), len, range, input, str, int, float, bool, abs, round, min, max, sum, sorted (reverse=対応)\n"
            "    enumerate, zip, isinstance（型のタプル対応: isinstance(x,(int,str))、クラスオブジェクトや Outer.Inner も可）, type\n"
            "    any, all, map, filter, list, tuple, divmod, pow(base, exp, mod), format(value, spec), callable\n",
            p2c_version_string());
        /* ISO C99が保証する1つの文字列リテラルは4095バイトまで（-Wpedantic の
         * -Woverlength-strings が上限超過を診断する）。対応一覧はそれを超えるため
         * 複数のリテラルに分割し、残り容量を確認しながら連結する。 */
        static const char features_mid[] =
            "  単一値:\n"
            "    ... (Ellipsis) と Ellipsis（is/==/repr/type()/コンテナ要素に対応。def f(): ... のスタブ本体も可）\n"
            "  特殊メソッド:\n"
            "    __init__, __str__, __repr__, __eq__（オーバーライドとメソッドデフォルト引数に対応）\n"
            "  組み込みメソッド:\n"
            "    list:  append, pop, insert, remove, count, index(value, start, stop), extend, clear, reverse, sort, copy\n"
            "    dict:  get, keys, values, items, update, pop, popitem, setdefault, clear, copy\n"
            "    set:   add, discard, remove, clear, copy, union, intersection, difference, symmetric_difference,\n"
            "           update, intersection_update, difference_update, symmetric_difference_update, isdisjoint\n"
            "    str:   upper, lower, casefold, capitalize, swapcase, strip, lstrip, rstrip, split(sep, maxsplit), join, replace(old, new, count),\n"
            "           find/index/rfind/rindex (start, stop対応), count(sub, start, stop), startswith, endswith, removeprefix, removesuffix, format, title, center, ljust, rjust, zfill,\n"
            "           isalpha/isdigit/isalnum/isspace/islower/isupper/isidentifier/isascii/isprintable\n"
            "    f-string / str.format() / format() の書式指定: {:05d} {:.2f} {:>10} {:x} {:b} {:,} {:.0%%} など主要な書式に対応（括弧内の複数f-string連結を含む）\n"
            "  組み込みモジュール: math (pi/e/tau/inf/nan, floor, ceil, trunc, fabs, fmod, hypot, copysign, ldexp,\n"
            "            degrees, radians, sin, cos, tan, asin, acos, atan, atan2, exp, expm1, log, log2, log10, log1p,\n"
            "            sqrt, cbrt, pow, isnan, isinf, isfinite, fsum, prod, factorial, gcd, isqrt, comb, perm, erf, erfc, gamma, lgamma)\n"
            "    pygame (ヘッドレス版: 実際の描画/音声/入力なし。init/display/time/\n"
            "            event/draw/key/sprite/Surface/Rect等、ゲームロジック検証用)\n";
        /* 追記部分はそれぞれ4095バイト以下のリテラルに分割し、残り容量を
         * 確認してからコピーする（静かな切り詰めを起こさない）。 */
        static const char features_tail[] =
            "\n"
            "  クラス:\n"
            "    ネストしたクラス定義 (class Outer: class Inner: ...): クラス本体からはその名前で参照可、\n"
            "      外部からは Outer.Inner 経由。生成CではC名を Outer__Inner に前置して衝突を避け、\n"
            "      外側クラスの __classobj() が属性として登録する。クラスオブジェクトはGC管理下で\n"
            "      新しいランタイムAPIを必要としないため、NO_STDLIB/freestandingでもそのまま動く。\n"
            "    多重継承 (class D(B, C)): Pythonと同じC3線形化でMROを求め、ダイヤモンド継承でも基底メソッドの選択がCPythonと一致する。\n"
            "      基底クラスのクラス属性はサブクラスのインスタンスからも見える（MRO順に探索）。\n"
            "    束縛メソッド: m = obj.method でメソッドを取り出し、コールバック（sorted(key=...)、map()等）へ渡せる。\n"
            "    @property の setter: @x.setter でプロパティへ代入されたときの処理を定義できる。\n"
            "      setterが無いプロパティへの代入はAttributeError（Python同様）。\n"
            "    メソッドデコレータ: @staticmethod（selfを渡さない）、@classmethod（先頭にクラスオブジェクト）、\n"
            "      @property（属性読み出しでゲッター実行・インスタンス属性より優先・setter無しの代入はAttributeError）。\n"
            "      プロパティはMRO順に継承・オーバーライドされ、hasattr()も真になる。\n"
            "  文字列エスケープ:\n"
            "    \\n \\t \\r \\v \\f \\b \\a \\\\ \\\" \\', \\ooo, \\xHH, \\uXXXX, \\UXXXXXXXX, 行継続\n"
            "    （未知のエスケープはバックスラッシュごと保持。\\N{...}はUnicode名前表が無いため診断）\n"
            "\n"
            "[未対応（診断エラーになります）]\n"
            "  関数本体内のclass定義、デコレータ関数の *args / **kwargs、メソッドへのユーザー定義デコレータ\n"
            "  ネストクロージャ捕捉を行うジェネレータ式、async forの状態機械（一部のasync/awaitは対応）\n"
            "  複数のstarred代入対象、複素数型、bytes/bytearray\n"
            "\n"
            "  --fallback: 未対応構文を「実行時にNotImplementedErrorを送出するスタブ」へ\n"
            "            置き換えて変換を続行する（到達しなければそのまま動く）。\n"
            "  GC: 適応しきい値（収集しても解放が少なければしきい値を伸ばす。上限4MiB）。\n"
            "      p2c_gc_set_adaptive()/p2c_gc_stats() で制御と統計取得ができる。\n"
            "  詳細と既知の制限は README.md を参照してください。\n";
        {
            size_t used = strlen(buf);
            used = p2c_append_literal(buf, sizeof(buf), used, features_mid, sizeof(features_mid) - 1u);
            used = p2c_append_literal(buf, sizeof(buf), used, features_tail, sizeof(features_tail) - 1u);
            (void)used;
        }
        built = true;
    }
    return buf;
#else
    return "";
#endif
}

void p2c_print_supported_range(void) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    printf("%s", p2c_supported_range_string());
#endif
}

/* ========================================
 * メイン変換関数
 * ======================================== */

/* 既定アロケータが使えない構成（カーネルが既定アロケータを持たない場合）向けの
 * フォールバック。64KiBの静的バッファをリニアアロケータとして使い、変換呼び出し
 * ごとに確保・解放（リセット）する。
 *
 * 以前は「使用中」フラグを立てたまま戻していたため、同じプロセスで2回目の変換が
 * 必ず P2C_ERR_INTERNAL になっていた（自作OS上でオンデバイス変換を繰り返す用途や
 * サービスとして常駐させる使い方を塞いでいた）。変換結果のC文字列はこの
 * バッファとは別に malloc されるため、呼び出し終了時に解放して問題ない。 */
#ifndef P2C_COMPILER_FALLBACK_HEAP_SIZE
#  define P2C_COMPILER_FALLBACK_HEAP_SIZE 65536
#endif

#if P2C_COMPILER_FALLBACK_HEAP_SIZE > 0
static char fallback_buf[P2C_COMPILER_FALLBACK_HEAP_SIZE];
#endif
static P2C_Allocator *fallback_allocator = NULL;
/* 直近の変換で静的フォールバックを使ったかどうか（診断用）。 */
static bool g_core_static_allocator_active = false;

static void p2c_release_fallback_allocator(void) {
    if (!fallback_allocator) return;
    p2c_linear_reset(fallback_allocator);
    fallback_allocator = NULL;
}

/* 直近の変換が静的フォールバックヒープを使ったかどうか。
 * 自作OSでは「アロケータ注入が効いている（=false が期待値）」ことの
 * 確認に使える。 */
bool p2c_core_static_allocator_active(void) {
    return g_core_static_allocator_active;
}

static P2C_Result p2c_python_to_c_impl(const char *python_code, P2C_TranspileOptions *options, char **out_c_code) {
    if (!python_code || !out_c_code) return P2C_ERR_INTERNAL;
    *out_c_code = NULL;
    
    P2C_TranspileOptions opts = options ? *options : P2C_DEFAULT_TRANSPILER_OPTIONS;
    P2C_Result result = P2C_OK;
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    clock_t t0 = clock();
#else
    int t0 = 0;
#endif
    
    /* アロケータ選択:
     *   1. 明示注入された既定アロケータ（p2c_set_default_allocator）
     *   2. 共有ヒープ（カーネルのP2C_Platform、または libc/カーネルのmalloc）
     *   3. 静的フォールバック（上のどれも確保できない場合の最後の手段）
     * 「実際に確保できるか」は小さなプローブ1回で判定する。NO_STDLIB の既定
     * 構成で p2c_runtime_init() 前に呼ばれた場合など、既定アロケータは
     * 存在しても実体が無い（malloc が NULL を返す）ことがあるため。 */
    P2C_Allocator *alloc = p2c_default_allocator();
    bool usable = alloc && p2c_heap_usable();
    if (usable) {
        void *probe = p2c_alloc(alloc, sizeof(void*));
        if (probe) p2c_free(alloc, probe);
        else usable = false;
    }
    g_core_static_allocator_active = false;
    if (!usable) {
#if P2C_COMPILER_FALLBACK_HEAP_SIZE > 0
        /* フォールバック：静的バッファ（呼び出しごとに再利用する） */
        if (fallback_allocator) {
            /* 再入（変換中に変換を呼ぶ）は静的一式を壊すため拒否する。 */
            strcpy(last_error_buf, "fallback allocator is already in use (reentrant transpile)");
            return P2C_ERR_INTERNAL;
        }
        fallback_allocator = p2c_linear_allocator(fallback_buf, sizeof(fallback_buf));
        if (!fallback_allocator) {
            strcpy(last_error_buf, "failed to create fallback allocator");
            return P2C_ERR_NOMEM;
        }
        alloc = fallback_allocator;
        g_core_static_allocator_active = true;
#else
        /* P2C_COMPILER_FALLBACK_HEAP_SIZE=0: 静的フォールバックを無効化した
         * 構成では、アロケータの注入が必須であることを明示的に知らせる。 */
        strcpy(last_error_buf,
               "no allocator available: inject one with p2c_set_default_allocator() "
               "or p2c_platform_set_allocator(), or define P2C_COMPILER_FALLBACK_HEAP_SIZE>0");
        return P2C_ERR_NOMEM;
#endif
    }
    
    /* 1. 字句解析 */
    size_t code_len = strlen(python_code);
    VLOG(t0, "input: %zu bytes", code_len);
    P2C_Lexer *lexer = p2c_lexer_new(alloc, python_code, code_len);
    if (!lexer) {
        strcpy(last_error_buf, "failed to create lexer");
        return P2C_ERR_NOMEM;
    }
    VLOG(t0, "lexer initialized");
    
    /* 2. 構文解析 */
    P2C_Parser *parser = p2c_parser_new(alloc, lexer);
    if (!parser) {
        p2c_lexer_free(lexer);
        strcpy(last_error_buf, "failed to create parser");
        return P2C_ERR_NOMEM;
    }
    
    /* --fallback 指定時は、未対応構文をスタブへ置換して変換を続行する。 */
    /* この関数はオプションを受け取らない（既定の strict 動作）。 */
    p2c_parser_set_fallback(opts.fallback_unsupported);
    P2C_AstModule *module = p2c_parser_parse_module(parser, &result);
    if (result != P2C_OK || !module) {
        const char *msg = p2c_parser_error_msg(parser);
        if (msg) {
            format_error_with_source(last_error_buf, sizeof(last_error_buf), "Parse error",
                                      python_code, parser->error_line, parser->error_col, msg);
        } else {
            strcpy(last_error_buf, "Unknown parse error");
        }
        VLOG(t0, "parse failed: %s", last_error_buf);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        return result != P2C_OK ? result : P2C_ERR_SYNTAX;
    }
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    {
        P2C_AstNode *mn = (P2C_AstNode*)module;
        VLOG(t0, "parse done: %zu top-level statements", p2c_vec_len(mn->u.module.body));
    }
#endif
    
    /* 3. 意味解析 */
    P2C_Semantic *semantic = p2c_semantic_new(alloc);
    if (!semantic) {
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        strcpy(last_error_buf, "failed to create semantic analyzer");
        return P2C_ERR_NOMEM;
    }
    
    result = p2c_semantic_analyze(semantic, module);
    if (result != P2C_OK) {
        const char *msg = p2c_semantic_error_msg(semantic);
        if (msg) {
            format_error_with_source(last_error_buf, sizeof(last_error_buf), "Semantic error",
                                      python_code, semantic->error_line, semantic->error_col, msg);
        } else {
            strcpy(last_error_buf, "Unknown semantic error");
        }
        VLOG(t0, "semantic analysis failed: %s", last_error_buf);
        p2c_semantic_free(semantic);
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        return result;
    }
    VLOG(t0, "semantic analysis done");
    
    /* 4. コード生成 */
    P2C_CodeGenOptions cg_opts = P2C_DEFAULT_OPTIONS;
    cg_opts.baremetal = opts.baremetal;
    cg_opts.debug_info = opts.debug_comments;
    cg_opts.strict_c11 = opts.strict_c11;
    cg_opts.fallback_unsupported = opts.fallback_unsupported;
    cg_opts.indent_width = opts.indent_spaces;
    cg_opts.embed_entry = opts.embed_entry;
    
    P2C_CodeGen *codegen = p2c_codegen_new(alloc, &cg_opts, semantic->symtab);
    if (!codegen) {
        p2c_semantic_free(semantic);
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        strcpy(last_error_buf, "failed to create code generator");
        return P2C_ERR_NOMEM;
    }
    if (opts.debug_comments) p2c_codegen_set_source(codegen, python_code);
    
    char *generated_code = NULL;
    result = p2c_codegen_generate(codegen, module, &generated_code);
    if (result != P2C_OK || !generated_code) {
        const char *msg = p2c_codegen_error_msg(codegen);
        if (msg) {
            snprintf(last_error_buf, sizeof(last_error_buf), "Code generation error: %s", msg);
        } else {
            strcpy(last_error_buf, "Unknown code generation error");
        }
        VLOG(t0, "code generation failed: %s", last_error_buf);
        p2c_codegen_free(codegen);
        p2c_semantic_free(semantic);
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        return result != P2C_OK ? result : P2C_ERR_INTERNAL;
    }
    VLOG(t0, "code generation done: %zu bytes of C emitted", strlen(generated_code));
    
    /* 5. 結果を出力（strdupして返す） */
    size_t code_size = strlen(generated_code) + 1;
    char *output = (char*)malloc(code_size);
    if (!output) {
        strcpy(last_error_buf, "failed to allocate output buffer");
        p2c_codegen_free(codegen);
        p2c_semantic_free(semantic);
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        return P2C_ERR_NOMEM;
    }
    memcpy(output, generated_code, code_size);
    *out_c_code = output;
    
    /* クリーンアップ */
    p2c_codegen_free(codegen);
    p2c_semantic_free(semantic);
    p2c_ast_free((P2C_AstNode*)module, alloc);
    p2c_parser_free(parser);
    p2c_lexer_free(lexer);
    
    VLOG(t0, "total time");
    return P2C_OK;
}

/* 公開エントリ。フォールバックアロケータ（既定アロケータが無い構成のみ使用）を
 * 呼び出し終了時に必ず解放し、同じプロセスで何度でも変換できるようにする。 */
P2C_Result python_to_c(const char *python_code, P2C_TranspileOptions *options, char **out_c_code) {
    P2C_Result result = p2c_python_to_c_impl(python_code, options, out_c_code);
    p2c_release_fallback_allocator();
    return result;
}

/* ========================================
 * --dump-ast 用: 字句解析・構文解析のみを行い、ASTを木構造テキストとして返す。
 * コード生成や意味解析は行わないため、意味解析エラーで弾かれるコードでも
 * 構文的に読める範囲までのASTを確認できる（デバッグ用途を優先した挙動）。
 * ======================================== */
P2C_Result python_to_ast_dump(const char *python_code, char **out_dump) {
    if (!python_code || !out_dump) return P2C_ERR_INTERNAL;
    *out_dump = NULL;

    P2C_Result result = P2C_OK;
    P2C_Allocator *alloc = p2c_default_allocator();
    if (!alloc) {
        strcpy(last_error_buf, "failed to create allocator");
        return P2C_ERR_NOMEM;
    }

    size_t code_len = strlen(python_code);
    P2C_Lexer *lexer = p2c_lexer_new(alloc, python_code, code_len);
    if (!lexer) {
        strcpy(last_error_buf, "failed to create lexer");
        return P2C_ERR_NOMEM;
    }

    P2C_Parser *parser = p2c_parser_new(alloc, lexer);
    if (!parser) {
        p2c_lexer_free(lexer);
        strcpy(last_error_buf, "failed to create parser");
        return P2C_ERR_NOMEM;
    }

    /* --fallback 指定時は、未対応構文をスタブへ置換して変換を続行する。 */
    /* この関数はオプションを受け取らない（既定の strict 動作）。 */
    p2c_parser_set_fallback(false);
    P2C_AstModule *module = p2c_parser_parse_module(parser, &result);
    if (result != P2C_OK || !module) {
        const char *msg = p2c_parser_error_msg(parser);
        if (msg) {
            format_error_with_source(last_error_buf, sizeof(last_error_buf), "Parse error",
                                      python_code, parser->error_line, parser->error_col, msg);
        } else {
            strcpy(last_error_buf, "Unknown parse error");
        }
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        return result != P2C_OK ? result : P2C_ERR_SYNTAX;
    }

    P2C_String *buf = p2c_str_new(alloc);
    if (!buf) {
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        strcpy(last_error_buf, "failed to allocate dump buffer");
        return P2C_ERR_NOMEM;
    }
    p2c_ast_dump_module(module, buf);

    size_t dump_size = p2c_str_len(buf) + 1;
    char *output = (char*)malloc(dump_size);
    if (!output) {
        p2c_str_free(buf);
        p2c_ast_free((P2C_AstNode*)module, alloc);
        p2c_parser_free(parser);
        p2c_lexer_free(lexer);
        strcpy(last_error_buf, "failed to allocate output buffer");
        return P2C_ERR_NOMEM;
    }
    memcpy(output, p2c_str_cstr(buf), dump_size);
    *out_dump = output;

    p2c_str_free(buf);
    p2c_ast_free((P2C_AstNode*)module, alloc);
    p2c_parser_free(parser);
    p2c_lexer_free(lexer);
    return P2C_OK;
}
