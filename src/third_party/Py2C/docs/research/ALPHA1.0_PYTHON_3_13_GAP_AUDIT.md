# Python Code to C Alpha1.0 — Python 3.13構文ギャップ監査

## 参照した公式仕様

Python 3.13の公式言語リファレンスでは、複合文として`if`、`while`、`for`、`try`、`with`、`match`、関数・クラス定義、`async with`、`async for`、`async def`を定義している。[1] 式仕様は、非同期内包表記、generator expression、yield、lambda、比較、呼出し、starred/double-starred引数などの意味論を規定する。[2] 完全文法は`except*`、decorator、async with、行列乗算`@`、ellipsis、bytes、complex、async comprehensionを含む構文表面を示す。[3]

## Alpha1.0の実装優先順位

| 優先度 | 機能群 | 根拠 | 今回の対象 |
|---|---|---|---|
| 完了 | `async with` | 既存async for・遅延例外・state machineを再利用し、Python 3.13の複合文を補完した | 構文受理、`__aenter__`/`__aexit__`待機、例外抑止、元例外再送出、class protocol method内await |
| 完了 | 関数ローカルgenerator expression capture | 既存closure cellとgenerator localsの結合で実装でき、公式仕様の遅延評価契約に近づく | free variableをclosure環境から読取るgenerator expression |
| P2 | 多段・再帰closure | `nonlocal`共有cellの自然な拡張であり、関数スコープの実用性を高める | 親closure環境のcell転送、自己参照のlate binding |
| P3 | 複数awaitと副作用順保存 | await仕様が要求する左から右の評価順を満たすために必要 | A-normal form型のcontinuation lowering |
| P4 | `except*` / ExceptionGroup、async comprehension、async generator | 専用の例外群・iterator protocol・複数継続点を要するため、影響範囲が大きい | 設計・明示的互換フォールバックを継続 |

Alpha1.0では、async withと関数ローカルgenerator expression captureを完了範囲へ昇格した。次候補はdecorator、多段・再帰closure、複数awaitの評価順保存である。すべての追加機能はCPython差分テスト、Clang/GCCの警告即エラー、freestanding、単一ヘッダー再生成を必須品質ゲートとする。

## 既知の互換境界

現在のAlpha1.0では`async for`、単一awaitを含む一部代入式、direct child `nonlocal`、C11 lambda closure、関数ローカルgenerator expression capture、単一context managerの`async with`を実装済みである。set comprehensionは同期`for`・filter・重複排除をCPython差分で確認し、strict ISO C11では明示診断する。一方、async for body/else内のawait、複数await、深い多段capture、再帰closure、async withの複数context/複合target、async comprehension、async generator、`except*`は未対応または限定的な互換フォールバック対象である。

## 参考資料

[1]: https://docs.python.org/3.13/reference/compound_stmts.html "Python 3.13 Language Reference — Compound statements"
[2]: https://docs.python.org/3.13/reference/expressions.html "Python 3.13 Language Reference — Expressions"
[3]: https://docs.python.org/3.13/reference/grammar.html "Python 3.13 Language Reference — Full Grammar specification"

## 2026-08-22 再監査: 公式文法との対応差分

Python 3.13の完全文法は、既存の`if`、`while`、`for`、通常`with`、`try`、`match`、`def`、`class`、`async def`、`async for`に加えて、`async with`、decorator、`except*`、非同期内包表記、行列乗算`@`、ellipsis、bytes、complex、完全なstarred target・starred expressionを規定する。[1] [2] Alpha1.0のparser/semantic/codegen監査では、通常のlist/set/dict comprehension、generator expression、dictionary unpack、type parameter、位置専用引数、通常のexception cause、direct-child `nonlocal`、async forの初期状態機械が存在する一方、下表の項目が未実装または限定的であることを確認した。

| 優先度 | 公式構文または意味論 | 現状 | 実装方針 |
|---|---|---|---|
| 完了 | `async with` | `AST_WITH.is_async`、`__aenter__`/`__aexit__`待機state、例外抑止・再送出、class protocol method内awaitを実装 | 複数context、複合target、複雑な停止点は明示的互換境界 |
| P0 | decorator | `@`で明示診断 | 関数・class objectをdecorator callableへ左から右へ適用するbinding経路を追加 |
| P1 | `except*` / `ExceptionGroup` | parserには入口があるが例外群意味論なし | dedicated object、split/merge、handler制御を設計後に追加 |
| P1 | async comprehension / async generator expression | comprehension generatorは同期のみ | async iterator protocolと継続変換を共有して追加 |
| P1 | 多段capture・recursive closure | direct-child captureのみ | cell owner/parent env transferとlate bindingを追加 |
| P2 | 複数await・条件/loop/body内await | 単一awaitを含む一部代入式のみ | A-normal formで評価順を固定する継続lowering |
| P2 | bytes / complex / ellipsis / `@` | 構文・値モデル・演算意味論が未完 | object typeとruntime演算を追加後に受理 |
| 完了 | parenthesized `with` / target一般化 | 括弧付き複数context、末尾カンマ、tuple/list `as` targetを実装 | async withの複数context・複合targetへ安全に拡張する際に再利用 |

今後は、各構文を**パース受理だけで完了扱いにしない**。AST解放、意味解析、生成C、runtime、GCトラバーサル、C11単一ヘッダー、freestanding、CPython差分、ASan/UBSanを同時に満たした時点で対応済みに昇格する。

## References

[1]: https://docs.python.org/3.13/reference/grammar.html "Python 3.13 Full Grammar specification"
[2]: https://docs.python.org/3.13/reference/compound_stmts.html "Python 3.13 Compound statements"
[3]: https://docs.python.org/3.13/reference/expressions.html "Python 3.13 Expressions"

## 2026-09-27 監査: try/finally脱出の意味論修正と厳格警告基線

### 判明した誤生成

`try`本体または`except`節から`return`/`break`/`continue`で脱出するとき、生成Cは`finally`本体を実行せずに脱出していた。CPythonは`finally`を先に実行するため、これは明示診断のない**無言の意味論違反**である。

```python
def f():
    try:
        return 1
    finally:
        print("cleanup")

print(f())      # CPython: cleanup → 1 / 修正前の生成C: 1（cleanupが消える）
```

```python
def g():
    for x in [1, 2, 3]:
        try:
            if x == 2:
                break
        finally:
            print("cleanup", x)   # CPython: cleanup 1, cleanup 2 / 修正前: cleanup 1のみ
```

`with`本体の`return`/`break`/`continue`は既に明示診断されていたが、`try/finally`には同等の保護がなく、脱出時に例外フレームが関数を抜けたスタックを指したままになる問題も同時に存在した。

### 実装

- コード生成中のみ有効なクリーンアップフレーム（`try_id`、`finally`本体、ループ入れ子数）をCスタックへ連結し、`return`は全ての、`break`/`continue`は脱出後も生存する`try`を除いたフレームを内側から外側へ処理する。
- 各フレームで`finally`本体を生成してから`p2c_exc_stack = <frame>.prev`で例外フレームを復元する。これにより保留中例外がPython規則どおり破棄され、戻り先で死んだフレームを参照しない。
- `return`式は一時変数へ確定してから`finally`を生成する（CPythonは戻り値評価→finally実行→returnの順）。
- `finally`本体の生成中は自分自身のフレームを外すため、入れ子`try/finally`は外側だけを再実行する。入れ子関数・メソッド本体・closure entryはフレームとループ深さをリセットして生成し、外側の`try`へ脱出しない。
- `finally`内の`return`/`break`/`continue`は、保留中の制御を上書きする場合の状態機械上の曖昧さを残さないよう引き続き明示診断する（`tests/audit_semantics.py`のC77が固定）。

回帰は`tests/finally_unwind_alpha10.py`（戻り値評価順、`break`/`continue`/for-else/while、`except`ハンドラ内`return`、例外伝播、入れ子`def`、メソッド、モジュールレベルループ）を追加し、`tests/conformance_regression.sh`のC384–C402としてCPython差分で固定した。

### 同時に修正した既存不具合

追加引数を持たないメソッド（例: `def __str__(self)`）の`__kwadapter`は、`args`/`kw_names`/`kw_values`を本体で参照しないため、生成Cが`-Wunused-parameter`（`-Wextra`、テストでは`-Werror`）でコンパイルできなかった。生成側で常に`(void)`キャストを出力して解消した。変更前のビルドでも再現する既存不具合である。

### 厳格警告基線

`Makefile`の`WARN_CFLAGS`と`templates/toolchains/{freestanding-c11,baremetal-example}.mk`へ`-Wold-style-definition`、`-Wredundant-decls`、`-Wundef`、`-Wconversion`、`-Wsign-conversion`、`-Wcast-qual`、`-Wwrite-strings`、`-Wdouble-promotion`、`-Wvla`、`-Wfloat-equal`を追加し、`-Werror`のまま0警告で通した。既存コードは次のとおり修正した。

| 警告 | 箇所 | 修正 |
|---|---|---|
| `-Wsign-conversion` | リニア/プールアロケータとNO_STDLIBヒープの`(n + 7) & ~7` | `P2C_ALIGN_UP8`で`size_t`マスクを明示 |
| `-Wconversion` | バケット添字を`uint32_t h`で受けるハッシュ経路 | `size_t h`へ変更 |
| `-Wredundant-decls` | `runtime.h`の演算子宣言重複、`runtime.c`の前方宣言・`extern`再宣言 | 重複を削除 |
| `-Wcast-qual` | `p2c_str_cstr()`結果のconst破棄、NO_STDLIB版`strstr`/`strchr` | 書き換え可能なコピー生成、標準API契約の変換を1か所へ集約 |
| `-Wwrite-strings` | 組み込み型名テーブルの文字列リテラル | 書き換え可能な静的配列へ |
| `-Wfloat-equal` | Python仕様上「正確な一致」が正しいfloat比較 | `p2c_float_eq`/`p2c_float_ne`へ集約し意図を明記 |

`-Wswitch-enum`はASTノード種別の`switch`が`default:`で未対応種別を診断する設計であり、`-Wnull-dereference`は単一ヘッダー構成（全ソース=1TU）で関数間解析が効き未チェック確保と誤検出が混在した約300件を報告するため、いずれも除外し理由を`Makefile`へ明記した。

### 残る互換境界

suspension（`async def`・generator）経路は`try`文をstep関数へlowerする際に`NotImplementedError: suspension inside this statement is not yet supported`を実行時に送出する。`try/finally`と`return`を併用した`async def`は現状出力を生成しない（要state machine拡張）。同期関数とモジュールレベルの`try/finally`は本修正で対応済みである。

### References

[1]: https://docs.python.org/3.13/reference/compound_stmts.html#the-try-statement "Python 3.13 Language Reference — The try statement"
[2]: https://docs.python.org/3.13/reference/expressions.html "Python 3.13 Language Reference — Expressions (evaluation order)"

## 2026-09-27 監査: 組込み統合と、受理済み構文の無言誤変換

組込み統合（`--embed-entry` と `p2c_embed`）を追加する過程で、CPython差分だけでは
見つからない無言の誤変換と、組込み構成にだけ現れる不具合を検出した。いずれも
再現テストを追加し、修正後に固定した。

| 分類 | 症状 | 原因 | 修正 |
|---|---|---|---|
| 字句 | CRLFで保存したソースで、class本体のメソッドがクラスに登録されず実行時に `AttributeError`（LFソースでは正常） | 行末の `\r` を改行として扱わず未知トークンにしていたため、インデント計算とsuite判定が崩れていた | 入力ソースの改行を正規化（`\r\n`/`\r` → `\n`）。`make test-crlf` で固定 |
| 字句 | `"caf\u00e9"` が `"cafu00e9"` に化ける（未知エスケープでバックスラッシュが消える） | `\u`/`\x`/8進/行継続が未実装で、default節がバックスラッシュを捨てていた | エスケープ解釈を共通ヘルパへ集約しCPython準拠にした。`\N{...}` は明示診断 |
| 変換 | `divmod(...)` と3引数 `pow(...)` を受理しつつ、未定義のC関数呼び出し（`divmod`、`p2c_user_pow`）を生成し、生成Cがコンパイルできない | 組込み名テーブルには載っていたがコード生成の分岐が無く、callable名の経路へ落ちていた | ランタイム実装（床除算/剰余の符号規則、モジュラべき乗と逆元）とコード生成分岐を追加。C403–C428 で固定 |
| 意味解析 | `for k, v in ...` のループ本体で `k` を参照すると「undefined name」で誤って失敗 | for文のターゲットが単純名のときだけシンボル登録していた | タプル/リスト/starredを再帰的に束縛（with文と同じhelperを共用） |
| 組込み | `PYTHON_CODE_TO_C_NO_STDLIB` で `p2c_default_allocator()` がNULLを返し、`p2c_alloc(NULL, ...)`（文字列ビルダ、f-string構築、例外の文字列化、変換器コアの既定アロケータ）が未定義動作。最適化ビルドではGCCが到達不能と判断してトラップ命令を生成し、`str(exc)` を含むNO_STDLIBプログラムが実行時にクラッシュ | 組込みでは既定アロケータを「使わない前提」にしていたが、ランタイムと変換器コアの両方が実際に使っていた | 組込みでも `malloc/free/realloc` へ委譲する既定アロケータを提供。`make test-freestanding-setjmp` と `make test-embed-compile` で固定 |
| GC | スタック境界が不明な環境（非pthread・組込み）で、保守的スキャンを行わずに回収しており、ローカル変数からのみ到達可能なオブジェクトを解放しうる | スキャンの可否を判定せず、境界が無い場合は「スキャンをスキップして回収」していた | 境界が不明な間は収集を実行しない（安全側停止）。`p2c_gc_set_stack_bounds()` を追加し、対処を一度だけ診断する |
| 組込み | 組込みヒープの `realloc` 拡張で使用量がアンダーフロー | 隣接空きとの合体時に、加算済みの解放分をさらに減算していた | 合体相手は `used` に含まれないため減算を削除。`make test-embed-runtime` が検出 |
| 生成C | 内包表記の `size_t n = p2c_len(...)` が `-Wsign-conversion`、`except ... as` 変数が `-Wclobbered`、`...` だけの式文が `-Wunused-value` | 生成側の型・修飾の不足 | 明示キャスト、`volatile` 宣言、`(void)(...)` で固定 |

これらのうち、CPython差分だけでは検出できないものは、ソースの改行コード、組込み構成、
オブジェクトの生存、生成Cのコンパイル可否に依存する。そのため本監査では
「受理した構文は必ずコンパイル可能なCを生成する」「組込み構成でも実行時に
クラッシュしない」を独立した品質ゲート（H01–H06）として追加した
（[`../testing/CONFORMANCE_TEST_MATRIX_ALPHA1.0.md`](../testing/CONFORMANCE_TEST_MATRIX_ALPHA1.0.md) 参照）。

## References

[1]: https://docs.python.org/3.13/reference/lexical_analysis.html "Python 3.13 Language Reference — Lexical analysis (string literals, universal newlines)"
[2]: https://docs.python.org/3/library/functions.html "Python 3.13 Library Reference — Built-in functions (divmod, pow, format, callable)"

