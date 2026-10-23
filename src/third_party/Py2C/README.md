# Python Code to C Beta0.1

**Python Code to C Beta0.1** は、Python 3 系の実用的なサブセットを **C11** へ変換する
移植性重視のトランスパイラです。ホスト CLI・単一ヘッダー・freestanding コア・自作OS 組み込み
の 4 通りの使い方を、同じ変換器でカバーします。

> 設計目標は、ホスト環境での使いやすさを維持しながら、OS 依存処理を明示的なプラットフォーム
> フック（`P2C_Platform`）へ集約し、カーネルしか無い環境でも同じ変換結果を動かせることです。

**はじめて使う方は [`docs/GETTING_STARTED_ALPHA1.0.md`](docs/GETTING_STARTED_ALPHA1.0.md) から**読んでください。
最短手順は次の 3 行です。

```sh
make                      # ビルド（build/python-code-to-c と build/py2c ができます）
./build/py2c help         # 使い方の表示（Py2C help / --help / -h でも同じ）
./build/py2c run hello.py # 変換 → Cコンパイル → 実行 をまとめて実行
```

## 主な特徴

| 領域 | 内容 |
|---|---|
| 変換 | 字句解析 → 構文解析 → 意味解析 → コード生成 の 4 段階。`-v` で各段の時間と統計を表示 |
| 構文対応 | 式、関数、クラス、module-level decorator、コンテナ、例外、list/dict/set 内包表記、f-string、import、with、try、可変長引数（`*args`/`**kwargs`）、`match`/`case`、ジェネレータ（ループ本体内の `yield`・`yield from`・`send()`）、リテラル内展開（`[*a, *b]`・`(*a,)`・`{*a, b}`・`{**d}`）、タプル値/連鎖代入、`zip(*m)`、`enumerate(..., start=n)`、dict の `|`/`|=` |
| 生成物 | 単一の `.c` ファイル。ランタイム（`src/runtime`）と組み合わせてビルドする |
| ランタイム | 保守的 GC、`try`/`except`/`finally`（setjmp ベース）、ジェネレータ状態機械、Python 風の数値・文字列・コンテナ |
| 組込み | `P2C_Platform` でメモリ・出力・時刻を差し替え。単一ヘッダー（`include/python_code_to_c_single.h`）と freestanding コア、自作OS 組み込みテンプレートを同梱 |
| 診断 | 未対応構文は**位置付きの変換エラー**として報告（不正な C や実行時 abort を出さない）。`--fallback` で「実行時 NotImplementedError を送出するコード」へ置換して続行 |
| 品質基準 | `-Werror` 付きの非常に厳しい警告基準（`WARN_CFLAGS`）でコンパイルし、生成 C も成果物として同等の基準（`GENERATED_CFLAGS`）で検証。CPython 差分コーパス **925 アサーション**が `make test` に含まれる |

## ビルドと実行

| コマンド | 説明 |
|---|---|
| `make` | ホスト CLI（`build/python-code-to-c`）をビルド |
| `make py2c` | 上に加えて `build/py2c` と `bin/py2c` を用意（初心者向けの短い入口） |
| `make full-build` | CLI + GUI + freestanding + 単一ヘッダーを一括ビルド |
| `make run INPUT=x.py` | ビルドして、`x.py` を変換・コンパイル・実行まで一括で行う |
| `make single-header` | `include/python_code_to_c_single.h` を再生成 |
| `make test` | 全テスト（下記）を実行 |
| `make tcc-full` | TinyCC だけで全工程（ビルド・C99 コーパス・freestanding・CLI ELF・HobbyOS ELF）を実行。GCC/Clang/binutils は不要 |

## 厳格ビルドと静的検査

コンパイラ本体と freestanding コアは、共通の厳格な警告基準 `WARN_CFLAGS` でビルドされます
（すべて `-Werror`）。`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion -Wsign-conversion
-Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla -Wfloat-equal` に加えて、
`-Wcast-align=strict`、`-Wlogical-op`/`-Wduplicated-cond`/`-Wduplicated-branches`、
`-Wstrict-overflow=2`/`-Wshift-overflow=2`、`-Wformat-overflow=2`/`-Wformat-truncation=2`/
`-Wstringop-overflow=4`、`-Wuse-after-free=3`/`-Warray-bounds=2`、
`-Wjump-misses-init`/`-Wnested-externs`/`-Wmissing-declarations`、
`-Wswitch-default`/`-Wimplicit-fallthrough=5`、`-Wunused-macros`、`-Warith-conversion`、
`-Wcast-function-type` などを有効にしています。

さらに、setjmp/longjmp をまたぐ変数を検出する `-Wclobbered`（本ランタイムの例外・ジェネレータ
実装で必須）、`-Wformat-security`、双方向制御文字を検出する `-Wbidi-chars=any,ucn`、
実行可能スタックを禁じる `-Wtrampolines`、`-Wstrict-aliasing=2`、
`-Wpointer-to-int-cast`/`-Wint-to-pointer-cast`、`-Wstringop-overread`/`-Warray-compare`/
`-Wsizeof-pointer-div`/`-Wmemset-*`/`-Wzero-length-bounds`/`-Wflex-array-member-not-at-end`、
`-Wtautological-compare`/`-Winit-self`/`-Wshift-*`/`-Wabsolute-value`/`-Wenum-conversion`、
`-Wvla-parameter`/`-Woverlength-strings`/`-Wnormalized=nfkc`、およびスタック使用量の上限
`-Wstack-usage=$(STACK_USAGE_BUDGET)`（既定 16384 バイト。組込みコアは
`make test-stack-budget` が 4096 バイトで検証）を追加しています。

生成される C も成果物として同じ「実バグを捕まえる」基準（`GENERATED_CFLAGS`）でコンパイルし、
`make test-conformance` がその基準でコーパス全体を検証します。意図的に外しているフラグ
（`-Wc++-compat`、`-Wnull-dereference`、`-Wuseless-cast`、`-Wdeclaration-after-statement`）と
その理由、ならびに性能ヒントを分離した `make test-opt-hints` の位置づけは `Makefile` の
コメントに明記しています。

ホスト向けビルドには実行時ハードニング（`HOSTED_HARDEN`: `-fstack-protector-strong`、
`-fstack-clash-protection`、`-D_FORTIFY_SOURCE=3`、`-ftrivial-auto-var-init=zero`）と
リンク時硬化（Linux では `-Wl,-z,relro,-z,now,-z,noexecstack` を自動適用）を既定で施します。
組込み構成ではこれらを分離し、代わりに静的検査とサニタイザを用意しています。

## テスト

`make test` は次を一括実行します（GCC と Clang の両方で `make full-build` と `make test` を
通すことを品質基準としています）。

- Hosted CLI のスモーク、GC/GUI の C 回帰、GC ライフサイクル・スタック走査範囲・一時ルート
- **CPython 差分コンフォーマンス 925 アサーション**（`tests/conformance_regression.sh`）。評価順、クロージャの遅延束縛、frozenset、Unicode の回帰を含む
- c2py（C→Python 変換器）の構造検査（`make test-c2py`）と、Web GUI のセキュリティ検査（`make test-gui-web-security`）
- 単一ヘッダーの C11 / C99 / freestanding / TinyCC ビルドと実行
- 組込み（baremetal / freestanding setjmp / heap 統合 / 自作OS テンプレート）
- 生成 C の厳格ビルド（`GENERATED_CFLAGS`）、スタック予算（組込みコア 4096 バイト）
- dict/set の差分ファジング、期待診断（strict C11・decorator・set comprehension）
- サニタイザ（`make test-sanitizers` / `test-asan-strict` / `test-ubsan-deep`）、
  GCC `-fanalyzer`（`make test-analyzer`）、clang 静的解析（`make test-analyzer-clang`）

## 既知の制限（明示的に診断します）

- `try`/`finally` や `with` を**またぐ** `yield`（ジェネレータの中断点として未対応）
- クロージャ内で定義したジェネレータ、ネスト closure 内の generator expression の capture
- `bytes` / `bytearray` リテラルと型
- callable 変数（関数を値として持つ変数）への `*args`/`**kwargs` 可変長呼び出し
- 任意精度整数（64 ビット範囲外は `OverflowError`）

**クロージャは変数を共有します（Python と同じ遅延束縛）**。関数の局所、内包表記の対象変数、
モジュール変数を、ラムダ・入れ子の `def`・ジェネレータ式から参照すると、後の再代入が見えます。
作成時点の値を固定したい場合は、従来どおり `lambda i=i: ...` のように既定引数で束縛できます。

- `except ... as 名前` の名前は、節を抜けても残ります（CPython は節の終わりに削除します。未対応）
- クラス本体のメソッド内で捕捉される局所は、メソッドの本体と同じく共有セルになります

## ドキュメント

| ドキュメント | 内容 |
|---|---|
| [`docs/GETTING_STARTED_ALPHA1.0.md`](docs/GETTING_STARTED_ALPHA1.0.md) | はじめの一歩（初心者向け） |
| [`docs/spec/FEATURE_REFERENCE_ALPHA1.0.md`](docs/spec/FEATURE_REFERENCE_ALPHA1.0.md) | 対応構文・機能の一覧と既知の制限 |
| [`docs/release/ALPHA1.0_RELEASE_NOTES.md`](docs/release/ALPHA1.0_RELEASE_NOTES.md) | リリースノートと検証結果 |
| [`docs/review/BUGFIX_ALPHA1.0.md`](docs/review/BUGFIX_ALPHA1.0.md) | 発見した不具合と修正の記録（Round 別） |
| [`docs/review/PERF_ALPHA1.0.md`](docs/review/PERF_ALPHA1.0.md) | 性能の実測と最適化（Round-7: GC・確保・反復、Round-8: 不変リテラルの静的化、Round-9: ランタイムインスタンス化） |
| [`benchmarks/bench_alpha10.py`](benchmarks/bench_alpha10.py) | CPython との比較に使うベンチマーク（9 種） |
| [`docs/CONFORMANCE_TEST_MATRIX_ALPHA1.0.md`](docs/CONFORMANCE_TEST_MATRIX_ALPHA1.0.md) | コンフォーマンスケースの目的と ID |
| [`docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md`](docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md) | 自作OS への組み込み手順 |

> 補足: 本 README と上記 2 つの要約ドキュメント（機能一覧・リリースノート）は Round-6 で
> 内容を再構成しました。記載内容は常に**コードと `--supported` の出力を正**とします。
