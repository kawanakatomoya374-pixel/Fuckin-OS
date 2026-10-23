# Python Code to C Alpha1.0 — レビュー修正の記録（第2回）

第1回（`REVIEW_FIXES_ALPHA1.0.md`）の後に、CPython との差分プローブで見つけたバグを直した記録です。
「直したこと」「検証したこと」「まだ残っていること」を分けて書いています。

## 1. 検証結果

| 項目 | 結果 |
|---|---|
| `make test`（GCC 13.3.0、クリーンなコピー） | 終了コード 0 |
| 差分コーパス（CPython との比較） | `conformance_regression_ok: 951 semantic assertions passed` |
| `make tcc-full`（TinyCC 0.9.27） | 終了コード 0 |
| 単一ヘッダー・Unicode 表の鮮度 | 一致（`single_header_fresh_ok`、`unicode_tables_fresh_ok`） |
| 追加した回帰（26 件、CPython と完全一致） | 下の §2 の 7 ファイル |

差分プローブ（`probes`）の結果: `dict1`、`fmt1`、`list1` は一致。`set1`、`str1`、`bigint`、`num1` に残りの差分（§4 を参照）。`clos1`、`cls1`、`exc1`、`gen1`、`misc1` は明示的な未対応の診断で停止します。

## 2. 直したこと

### 構文（パーサ・字句解析）
- 1 行の節（`try: x` の後の `except:`、`else:`、`finally:`）が、次の節を認識できるように NEWLINE を消費する。
- 16 進数の `_` 区切り（`0xFF_FF`）、末尾の小数点（`5.`、`5..real`）、指数部の判定を CPython の規則に合わせた。
- `for` 文の対象で、2 番目以降の要素に入れ子の組・リスト（`for a, (b, c) in ...`）を書けるようにした。式の文法は使わず、対象専用の解析を使う（式として読むと、後ろの `in` を比較演算子として取り込んでしまうため）。
- ラムダに `*args`、キーワード専用引数、`**kwargs` を書けるようにした。
- f 文字列の自己文書化式 `{expr=}` に対応した（式の原文と `=` を出し、既定で `repr`）。

### コード生成
- 呼び出し先が変数・属性（クロージャ、メソッド、モジュールの関数）で、`*args` / `**kwargs` を含む呼び出しを、実行時に展開する `p2c_call_starred` を使うようにした（デコレータの `f(*a, **k)` など）。既知の関数・組込みは従来の経路のまま。
- ラムダの `*args` / `**kwargs` / キーワード専用引数を束縛するようにした。余分な位置引数は `TypeError`。
- `v = yield from g()` で、委譲先の戻り値（PEP 380）を `v` に束縛する。
- モジュール直下の `x: int = 5`、関数内の `global` だけで現れる名前、`for p, q in`、`with ... as`、`except ... as`、`while ... else` の中の代入を、モジュール変数として登録する。
- 未定義の名前を、解析時のエラーではなく実行時の `NameError` にした（CPython と同じ。評価されない枝にあっても正しい Python が動く）。
- 関数呼び出しの解決を直した。モジュール変数に入った callable（`from math import sqrt` を関数内で呼ぶ）が動く。未対応の組込みは、リンクエラーや実行時クラッシュではなく、明示的な診断になる。
- `global` で宣言された名前は、局所として改名しない（大域変数を指し続ける）。

### ランタイム
- 組込み型のメソッドを値として使えるようにした（束縛メソッドと、`str.lower` のような型からの非束縛メソッド）。未実装の名前は `AttributeError`。
- 組込みメソッドの呼び出しが、未実装の名前で無限に再帰していた問題を直した（フォールバックは値を経由せず、元の属性解決を使う）。
- `int.bit_length`、`int.conjugate`、`float.conjugate`、`float.is_integer`、`tuple.count`、`tuple.index` を実装した。`float.is_integer` は freestanding 構成でも動くよう、libm を使わずに判定する。
- `x.__len__()`、`x.__str__()`、`x.__repr__()` の呼び出しに対応した。
- 集合の `<`、`<=`、`>`、`>=` を包含関係で判定するようにした（以前は誤った結果を返していた）。
- 文字列の `repr` は、印字できない文字を `\xNN` / `\uNNNN` / `\UNNNNNNNN` で書き、印字可能な文字はそのまま書く（CPython と同じ規則）。
- 書式（f 文字列と `format`）: `c` 指定（整数の文字）、桁区切りは幅の適用より前、ゼロ埋めより前に適用する（`{n:010,}`）、数値の既定の揃えは右寄せ（`g`、`%` を含む）。
- `setjmp` / `longjmp` の `__builtin` ラッパのキャストを `void *` を経由させた（警告は残るが、ヘッダのコメントどおり freestanding 構成では許容される）。

## 3. 追加した回帰テスト（すべて CPython と完全一致）

| ファイル | 内容 | 件数 |
|---|---|---|
| `tests/for_nested_targets_alpha10.py` | 入れ子の `for` 対象 | 5 |
| `tests/lambda_star_alpha10.py` | ラムダの `*args`、キーワード専用引数、`**kwargs` | 3 |
| `tests/starred_calls_alpha10.py` | 変数・属性・クロージャへの `*args` / `**kwargs` 展開 | 6 |
| `tests/method_values_alpha10.py` | 組込みメソッドを値として使う | 2 |
| `tests/set_order_repr_alpha10.py` | 集合の順序比較、`repr` の制御文字 | 2 |
| `tests/module_globals_alpha10.py` | モジュール変数の登録 | 3 |
| `tests/format_fstring_alpha10.py` | f 文字列の書式、`{expr=}` | 5 |

## 4. まだ残っていること（明示的な制限）

1. **64 ビットを超える整数**は `OverflowError`（任意精度整数は未実装）。
2. **代入前の局所**を読むと `None` になる（CPython は `UnboundLocalError`）。
3. **ジェネレータ内の `try` / `with` と `yield`** は未対応（明示的な診断）。
4. **組込みと標準ライブラリ**: `open`、`vars`、`bytes`、`ascii`、`sys` は未対応（明示的な診断、または実行時の `ImportError`）。
5. **装飾された関数が `*args` / `**kwargs` を持つ場合**は、意味解析で拒否される（明示的な診断）。
6. **組込み関数への `*args` 展開**（`max(*xs)` など）は未対応（明示的な診断）。`print(*xs)` と `zip(*m)` は従来どおり動く。
7. **星引数の呼び出し**で、引数の評価順は固定していない（星引数の中の副作用の順序は保証しない）。
8. **文字列の途中の NUL**（`"\x00"`）は、C の文字列表現のため切り詰められる。
9. **`except ... as 名前`** の名前は、節の後も残る（CPython は削除する）。
10. **型検査の不足**: `str.count` / `str.index` に整数を渡しても `TypeError` にならない。`dict.pop()` を引数なしで呼ぶと `AttributeError`（CPython は `TypeError`）。
11. **`str.encode`** は、バイト型が無いため未対応（属性として取り出すと `AttributeError`）。
12. **ラムダの引数個数の診断**の文言は CPython と異なる（例外の型は一致）。

## 5. 修正の場所（主なファイル）

| 領域 | ファイル |
|---|---|
| 字句解析 | `src/lexer/python_code_to_c_lexer.c` |
| パーサ（for 対象、ラムダ、f 文字列） | `src/parser/python_code_to_c_parser.c`、`include/parser/python_code_to_c_ast.h` |
| 意味解析（未定義の名前） | `src/semantic/python_code_to_c_semantic.c` |
| コード生成（呼び出し、ラムダ、yield from、モジュール変数、書式） | `src/codegen/python_code_to_c_codegen.c` |
| ランタイム（メソッド値、数値・組のメソッド、集合の順序、repr、書式、starred 呼び出し） | `src/runtime/python_code_to_c_runtime.c`、`include/runtime/python_code_to_c_runtime.h` |
| 回帰テスト | `tests/*_alpha10.py`、`tests/conformance_regression.sh` |
