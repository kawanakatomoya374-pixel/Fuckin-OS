#!/bin/sh
BD=${P2C_BUILD_DIR:-build}
set -eu

CC=${CC:-cc}
# 出力先（build/tests）は他のターゲットの clean で消えることがあるため、
# 呼び出し側に依存せずここで用意する。
mkdir -p $BD/tests
P2C_COMPILER=${P2C_COMPILER:-./$BD/python-code-to-c}
P2C_TEST_CFLAGS=${P2C_TEST_CFLAGS:--Wall -Wextra -Werror -std=gnu11}
P2C_TEST_LDFLAGS=${P2C_TEST_LDFLAGS:-}
# 実行時の環境変数（ASAN_OPTIONS=... など）。厳格プロファイルが設定する。
P2C_TEST_ENV=${P2C_TEST_ENV:-}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p $BD/tests

run_differential_case() {
    case_id="$1"
    pyfile="$2"
    expected_lines="$3"
    stem=$(basename "$pyfile" .py)
    cfile="$BD/tests/${stem}.c"
    pyout="$BD/tests/${stem}.python.out"
    cout="$BD/tests/${stem}.c.out"
    bin="$BD/tests/${stem}"

    python3 "$pyfile" > "$pyout"
    "$P2C_COMPILER" "$pyfile" -o "$cfile"
    $CC -I./include $P2C_TEST_CFLAGS "$cfile" \
        src/runtime/python_code_to_c_runtime.c \
        src/common/python_code_to_c_common.c src/common/python_code_to_c_unicode_tables.c \
        src/platform/python_code_to_c_platform.c \
        src/platform/python_code_to_c_platform_hosted.c \
        src/platform/python_code_to_c_gui.c \
        src/modules/python_code_to_c_pygame.c $P2C_TEST_LDFLAGS -lm -o "$bin"
    # shellcheck disable=SC2086
    if ! env $P2C_TEST_ENV "$bin" > "$cout" 2> "$cout.stderr"; then
        printf '%s\n' "${case_id}: generated program exited non-zero" >&2
        head -20 "$cout.stderr" | sed 's/^/    /' >&2
        exit 1
    fi
    # サニタイザ実行時は、レポート自体を失敗として扱う（CPython差分より先に検出する）。
    if [ -n "$P2C_TEST_ENV" ] && grep -qE 'Sanitizer|runtime error:|SUMMARY: ' "$cout.stderr"; then
        printf '%s\n' "${case_id}: sanitizer report" >&2
        grep -m4 -oE '[A-Za-z]*Sanitizer[^:]*|runtime error: [^ ]+ [^ ]+|SUMMARY: [^ ]+' "$cout.stderr" | sed 's/^/    /' >&2
        exit 1
    fi
    if [ -n "$P2C_TEST_ENV" ] && [ -s "$cout.stderr" ]; then
        printf '%s\n' "${case_id}: stderr under sanitizer profile:" >&2
        head -10 "$cout.stderr" | sed 's/^/    /' >&2
    fi
    diff -u "$pyout" "$cout"

    actual_lines=$(wc -l < "$cout" | tr -d ' ')
    if [ "$actual_lines" != "$expected_lines" ]; then
        printf '%s\n' "${case_id}: expected ${expected_lines} assertions, got ${actual_lines}" >&2
        exit 1
    fi
    printf '%s\n' "${case_id}: ${actual_lines} CPython-differential assertions passed"
}

run_differential_case C01-C60 tests/complex_alpha10.py 60
run_differential_case C61-C76 tests/semantic_conformance_alpha10.py 16
run_differential_case C81-C83 tests/dict_comprehension_alpha10.py 3
run_differential_case C84-C85 tests/multi_with_alpha10.py 2
run_differential_case C86-C92 tests/dict_builtin_alpha10.py 7
run_differential_case C93-C97 tests/empty_and_reversed_alpha10.py 5
run_differential_case C98-C100 tests/enumerate_start_alpha10.py 3
run_differential_case C101-C108 tests/dict_order_mutation_alpha10.py 8
run_differential_case C109-C117 tests/set_methods_alpha10.py 9
run_differential_case C118-C126 tests/set_algebra_alpha10.py 9
run_differential_case C127-C128 tests/dict_equality_alpha10.py 2
run_differential_case C129-C132 tests/numeric_hash_contract_alpha10.py 4
run_differential_case C133-C144 tests/unhashable_keys_alpha10.py 12
run_differential_case C145-C148 tests/tuple_hash_contract_alpha10.py 4
run_differential_case C149-C161 tests/slice_and_index_alpha10.py 13
run_differential_case C162-C167 tests/integer_operator_semantics_alpha10.py 6
run_differential_case C168-C173 tests/fstring_evaluation_alpha10.py 6
run_differential_case C174 tests/type_annotations_alpha10.py 1
run_differential_case C175-C178 tests/walrus_alpha10.py 4
run_differential_case C179-C189 tests/container_methods_alpha10.py 11
run_differential_case C190-C194 tests/match_case_alpha10.py 5
run_differential_case C195-C197 tests/dict_merge_alpha10.py 3
run_differential_case C198-C199 tests/bare_raise_alpha10.py 2
run_differential_case C200-C203 tests/string_methods_alpha10.py 4
run_differential_case C204-C207 tests/for_starred_unpack_alpha10.py 4
run_differential_case C208-C211 tests/print_keywords_alpha10.py 4
run_differential_case C212-C215 tests/string_index_alpha10.py 4
run_differential_case C216-C220 tests/string_replace_count_alpha10.py 5
run_differential_case C221-C224 tests/list_index_bounds_alpha10.py 4
run_differential_case C225-C234 tests/string_search_bounds_alpha10.py 10
run_differential_case C235-C240 tests/string_split_maxsplit_alpha10.py 6
run_differential_case C241-C250 tests/string_edge_methods_alpha10.py 10
run_differential_case C251-C256 tests/string_partition_alpha10.py 6
run_differential_case C257-C268 tests/string_classification_and_lines_alpha10.py 12
run_differential_case C269-C274 tests/string_expandtabs_alpha10.py 7
run_differential_case C275-C282 tests/dict_set_advanced_alpha10.py 8
run_differential_case C283-C287 tests/async_generator_alpha10.py 5
run_differential_case C288-C299 tests/numeric_text_builtins_alpha10.py 12
run_differential_case C300-C302 tests/posonly_args_alpha10.py 3
run_differential_case C303 tests/yield_from_alpha10.py 1
run_differential_case C304 tests/raise_from_alpha10.py 1
run_differential_case C305-C314 tests/match_patterns_alpha10.py 10
run_differential_case C315-C322 tests/class_patterns_alpha10.py 8
run_differential_case C323-C328 tests/class_positional_patterns_alpha10.py 6
run_differential_case C329-C331 tests/dotted_value_patterns_alpha10.py 3
run_differential_case C332-C334 tests/dotted_class_patterns_alpha10.py 3
run_differential_case C335-C339 tests/generator_expression_alpha10.py 5
run_differential_case C340 tests/await_compare_bool_alpha10.py 1
run_differential_case C341 tests/async_for_alpha10.py 1
run_differential_case C342 tests/async_for_control_alpha10.py 1
run_differential_case C343-C346 tests/nonlocal_capture_alpha10.py 4
run_differential_case C347 tests/lambda_capture_alpha10.py 1
run_differential_case C348 tests/generator_expression_capture_alpha10.py 1
run_differential_case C349 tests/generator_expression_call_asan_alpha10.py 1
run_differential_case C350-C353 tests/builtin_key_semantics_alpha10.py 4
run_differential_case C354 tests/exception_cause_alpha10.py 1
run_differential_case C355 tests/with_parenthesized_alpha10.py 1
run_differential_case C356-C357 tests/with_unpack_target_alpha10.py 2
run_differential_case C358-C362 tests/async_with_alpha10.py 5
run_differential_case C363-C365 tests/set_comprehension_alpha10.py 3
run_differential_case C366-C372 tests/decorator_alpha10.py 7
run_differential_case C373-C377 tests/async_with_multiple_alpha10.py 5
run_differential_case C378-C383 tests/async_with_multiple_exception_alpha10.py 6
run_differential_case C384-C402 tests/finally_unwind_alpha10.py 19
run_differential_case C403-C428 tests/builtin_gap_alpha10.py 26
run_differential_case C429-C443 tests/ellipsis_alpha10.py 15
run_differential_case C444-C460 tests/string_escapes_alpha10.py 17
run_differential_case C461-C479 tests/nested_class_alpha10.py 19
run_differential_case C480-C486 tests/c_identifier_collision_alpha10.py 7
run_differential_case C487-C493 tests/multiple_inheritance_alpha10.py 7
run_differential_case C494-C499 tests/bound_method_alpha10.py 6
run_differential_case C500-C505 tests/finally_control_flow_alpha10.py 6
run_differential_case C506-C515 tests/generator_expression_multi_alpha10.py 10
run_differential_case C516-C525 tests/tuple_ordering_alpha10.py 10
run_differential_case C526-C528 tests/super_mro_alpha10.py 3
run_differential_case C529-C538 tests/method_decorators_alpha10.py 10
run_differential_case C539-C549 tests/math_module_alpha10.py 11
run_differential_case C550-C555 tests/property_setter_alpha10.py 6
run_differential_case C556-C576 tests/percent_format_alpha10.py 21
run_differential_case C577-C588 tests/complex_program_alpha10.py 12
run_differential_case C589-C593 tests/repro_super3_alpha10.py 5
run_differential_case C594-C615 tests/strict_convert_alpha10.py 22
run_differential_case C616-C647 tests/unicode_codepoint_alpha10.py 32
run_differential_case C648-C657 tests/dict_set_resize_alpha10.py 10
run_differential_case C658-C667 tests/range_lazy_alpha10.py 23
run_differential_case C668-C681 tests/range_slice_hash_alpha10.py 14
run_differential_case C682-C691 tests/slice_assign_alpha10.py 10
run_differential_case C692-C702 tests/unicode_width_alpha10.py 11
run_differential_case C703-C712 tests/number_literals_alpha10.py 10
run_differential_case C713-C722 tests/format_spec_alpha10.py 10
run_differential_case C723-C732 tests/semicolon_block_alpha10.py 13
run_differential_case C733-C740 tests/rsplit_alpha10.py 8
run_differential_case C741-C746 tests/format_kw_alpha10.py 6
run_differential_case C747-C749 tests/key_builtin_alpha10.py 3
run_differential_case C750-C763 tests/stress_runtime_alpha10.py 14
run_differential_case C764-C770 tests/empty_split_alpha10.py 7
# バグ修正の回帰（C11予約語、連鎖代入、関数の値参照、in-place拡張代入、
# tuple連結、__bool__/__len__、isinstance(True,int)、round負桁、min/max空、
# repr引用符、range表示/属性、例外args、format %、starred代入、lambda既定引数、
# 高階関数、str.istitle）
run_differential_case F01-F33 tests/fixes_alpha10.py 33
# Round-3 で追加した構文の回帰（format フィールド/入れ子書式、クラスからの
# メソッド取り出し、type(None) 判定、ループ内 yield を持つジェネレータ、
# ジェネレータメソッド、x = yield v）。
run_differential_case R01-R29 tests/round3_alpha10.py 29
# Round-4 で追加・修正した機能の回帰（リテラル展開、send()、PEP380 の return 値、
# ジェネレータ内 assert/型注釈、入れ子アンパック、__lt__ によるソート、例外 .args、
# type(x).__name__、組み込み例外基底の super().__init__）。
run_differential_case R30-R44 tests/round4_alpha10.py 15
# Round-5: assert メッセージ式の評価、ジェネレータ内 assert、dict の `|`/`|=`、
# str.removeprefix/removesuffix、zip(*m)、enumerate(start=)、タプル値/連鎖代入。
run_differential_case R45-R55 tests/round5_alpha10.py 11
# Round-6: 括弧付き/入れ子/starred の for ターゲット、深い再帰を含む二項演算式
# （式評価スタックの動的化）、数値の符号・丸め。
run_differential_case R56-R65 tests/round6_alpha10.py 10
# 評価順の固定: 関数引数・辞書の要素・星引数の式は Python の順に1回ずつ評価される。
run_differential_case SEM01-SEM13 tests/eval_order_alpha10.py 13
# クロージャの遅延束縛: 外側の変数（ループ変数・後からの再代入・nonlocal・内包表記の対象）を共有する。
run_differential_case SEM14-SEM25 tests/closure_capture_alpha10.py 12
# frozenset: 不変（変更系は AttributeError）、型名・repr・ハッシュ・isinstance・集合演算の型。
run_differential_case SEM26-SEM45 tests/frozenset_alpha10.py 20
# Unicode: CPython の str.is* と大小変換（完全な写像、最終シグマ、タイトルケース）。
run_differential_case SEM46-SEM57 tests/unicode_semantics_alpha10.py 12
# 入れ子の for 対象（組・リスト・*名前）。`in` を比較演算子として読まないこと。
run_differential_case SEM58-SEM62 tests/for_nested_targets_alpha10.py 5
# ラムダの *args、**kwargs、キーワード専用引数。
run_differential_case SEM63-SEM65 tests/lambda_star_alpha10.py 3
# 変数・属性・クロージャへの *args / **kwargs 展開（デコレータの f(*a, **k) を含む）。
run_differential_case SEM66-SEM71 tests/starred_calls_alpha10.py 6
# 組込みメソッドを値として使う（max(d, key=d.get)、sorted(key=str.lower)、束縛メソッド）。
run_differential_case SEM72-SEM73 tests/method_values_alpha10.py 2
# 集合の順序比較（包含関係）と、repr の制御文字・非 ASCII の表記。
run_differential_case SEM74-SEM75 tests/set_order_repr_alpha10.py 2
# モジュール変数の登録（注釈つき代入、関数内の global、for/with/except の対象、while-else）。
run_differential_case SEM76-SEM78 tests/module_globals_alpha10.py 3
# f 文字列の書式（桁区切りと幅、c 指定、{expr=}、入れ子の書式指定、既定の右寄せ）。
run_differential_case SEM79-SEM83 tests/format_fstring_alpha10.py 5
# 装飾された関数の *args / **kwargs / キーワード専用引数（デコレータ経由で値になる関数）。
run_differential_case SEM84-SEM95 tests/decorated_varargs_regression.py 12
# 装飾された関数へのキーワード引数（仮引数名への対応、既定値、可変長の元関数）。
run_differential_case SEM96-SEM100 tests/decorated_keyword_args_regression.py 5
# 代入前の局所を読むと UnboundLocalError（NameError の子）。組込み例外の親子関係（except LookupError など）。
run_differential_case SEM101-SEM108 tests/unbound_local_regression.py 8
# ジェネレータ本体の yield を含まない if（代入・式・入れ子の if）と、単項演算子 not/~ の下ろし方。
run_differential_case SEM109-SEM113 tests/generator_plain_if_regression.py 5
run_differential_case SEM114-SEM116 tests/generator_unary_not_regression.py 3
# ジェネレータ本体の yield を含まない for / while（break・continue・入れ子）。
run_differential_case SEM117-SEM118 tests/generator_plain_loop_regression.py 2
# ジェネレータ本体の yield を含まない try / except（名前・タプルの一致、名前の束縛、未一致の伝播）。
run_differential_case SEM119-SEM120 tests/generator_plain_try_regression.py 2
# ascii()（ASCII 以外を \x / \u / \U で表示）。
run_differential_case SEM121-SEM122 tests/ascii_builtin_regression.py 2
# sys モジュール（argv、stdout.write、exit）。
run_differential_case SEM123-SEM125 tests/sys_module_regression.py 3
# open()（テキストの読み書き、with、readline / readlines）。
run_differential_case SEM126-SEM134 tests/file_io_regression.py 9
# for line in f:（ファイル反復）。
run_differential_case SEM135-SEM136 tests/file_iter_regression.py 2
# sys の版情報・環境属性（CPython と共通に確かめられる性質だけ）。
run_differential_case SEM137-SEM141 tests/sys_attrs_regression.py 5
# open() のキーワード引数 mode / encoding（UTF-8 の表記）。
run_differential_case SEM142-SEM143 tests/open_kw_regression.py 2
# 組込み（max / min / sum / zip）への *args 展開、単一の非反復値は TypeError。
run_differential_case SEM143-SEM148 tests/builtin_starred_regression.py 6
# sum / max / min に反復できない値を渡すと TypeError。
run_differential_case SEM149-SEM151 tests/sum_type_regression.py 3
# vars(obj)（属性の写し）と vars()（関数の局所。代入前の名前は含まない）。
run_differential_case SEM152-SEM156 tests/vars_regression.py 5
# bytes（段階1）: リテラル、長さ、添字、反復、等値、表記、print、list()。
run_differential_case SEM157-SEM163 tests/bytes_basic_regression.py 7
# bytes の大量の生成（回収で正しく解放されること）。
run_differential_case SEM164-SEM164 tests/bytes_gc_regression.py 1
# bytes（段階2）: bytes()、連結、encode / decode（UTF-8）、isinstance、復号の不正検出。
run_differential_case SEM165-SEM170 tests/bytes_stage2_regression.py 6
# bytes のスライス。
run_differential_case SEM171-SEM173 tests/bytes_slice_regression.py 3
# bytes の辞書のキー・集合の要素（内容によるハッシュと等値）。
run_differential_case SEM174-SEM177 tests/bytes_hash_regression.py 4
# 符号化（UTF-8、ASCII、Latin-1 の名前の表記、エラー、ファイルの読み書き）。
run_differential_case SEM178-SEM185 tests/encodings_regression.py 8
# cp1252（Windows-1252）の符号化（表の文字、未定義バイトのエラー、ファイルの読み書き）。
run_differential_case SEM186-SEM190 tests/cp1252_regression.py 5
printf '%s\n' 'conformance_regression_ok: 1059 semantic assertions passed'
