# Alpha1.0 リリースノート

Python Code to C Alpha1.0 は、Python 3 系の実用的なサブセットを **C11** へ変換する
トランスパイラです。本ノートは Alpha1.0 時点の到達点・品質基準・既知の制限をまとめます。
記載内容はコードと `./build/py2c --supported` の出力を正とします。

## 1. ハイライト

| 領域 | 到達点 |
|---|---|
| 変換対象 | Python の実用サブセット（制御構文・関数・クラス・例外・内包表記・f-string・ジェネレータ・`match`・`with`・async）。詳細は [`../spec/FEATURE_REFERENCE_ALPHA1.0.md`](../spec/FEATURE_REFERENCE_ALPHA1.0.md) |
| 生成物 | 単一の C11 ファイル。ランタイムと組み合わせてビルド |
| 移植性 | ホスト（CLI/GUI）、単一ヘッダー（C11/C99/freestanding/TinyCC）、freestanding コア、自作OS 組み込みテンプレート |
| ランタイム | 保守的 GC（動的ルート表・反復マーク・実体化アンカー）、setjmp ベースの例外、ジェネレータ状態機械（ドライバループ型） |
| 診断契約 | 未対応構文は**位置付き変換エラー**。不正な C の出力や実行時 abort を出さない。`--fallback` で実行時 `NotImplementedError` へ置換して続行 |
| システムテスト | CPython 差分 **868 アサーション**、各種 C 回帰、単一ヘッダー、組込み、サニタイザ、静的解析 |
| CLI | `py2c help` で初心者向けの使い方（オプション・はじめの一歩・ビルド手順・切り分け）を表示 |

## 2. バージョン体系とディレクトリ

- 製品名・ファイル名・マクロ・CLI 名・生成コードのコメントは
  `Python Code to C Alpha1.0` / `python_code_to_c` / `p2c_` に統一。
- 主要ディレクトリ: `src/`（lexer/parser/ast/semantic/codegen/runtime/common/platform/modules/tools）、
  `include/`（公開ヘッダーと単一ヘッダー）、`tests/`（回帰・差分コーパス・サニタイザ）、
  `docs/`（仕様・レビュー・リリース）、`templates/`（組込み/自作OS のビルドひな形）。

## 3. 品質基準（CI が保証するもの）

- `make full-build` と `make test` を **GCC と Clang の両方**で通すこと。
- 本体は `-Werror` 付きの厳しい警告基準（`-Wconversion`/`-Wsign-conversion`/`-Wcast-align=strict`/
  `-Wclobbered`/`-Wstrict-aliasing=2`/`-Wbidi-chars`/`-Wstack-usage` など。除外理由は `Makefile` に明記）。
- **生成 C も成果物**として同等の基準（`GENERATED_CFLAGS`）でコンパイルする。
- 組込みコアのスタック予算（1 フレーム 4096 バイト以内）を `make test-stack-budget` が検証。
- サニタイザ（ASan/UBSan/LSan 相当）と GCC `-fanalyzer`・clang 静的解析を回帰に含める。

## 4. 主な機能追加（開発ラウンド別の要約）

| ラウンド | 内容 |
|---|---|
| Round 2 | 内包表記・f-string・`str.format` の書式、`*args`/`**kwargs`、`match`/`case`、class 継承と `super()`、`with`、dict/set の各メソッド、`math`/`pygame` ヘッドレス |
| Round 3 | ループ本体内の `yield`（ドライバループ型状態機械）、generator メソッド、`str.format`/f-string のフィールド（`{0[1]}`・`{d[k]}`・`{p.x}`・`!r`・入れ子書式）、クラスメソッド抽出、`type(None)` 等の `isinstance`、GC 強化（動的ルート・反復マーク・実体化アンカー） |
| Round 4 | リテラル内展開 `[*a]`/`(*a,)`/`{*a}`、タプル値/連鎖代入、ジェネレータ `send()` と `x = yield v`、PEP 380 の戻り値、組み込み例外 `.args`、`type(x).__name__`、組み込み例外基底の `super().__init__`、反射演算子と `__neg__`、`zip(*m)`、`enumerate(start=)` |
| Round 5 | ビルド基準の強化（`-Wclobbered` 等）と、そこで検出した生成 C の `volatile` 漏れ・宣言重複・影・巨大スタックフレームの修正。生成 C の品質ゲート化 |
| Round 6 | `src/`・`include/` の全数精査：式評価スタックの動的化（深い再帰での誤 `RuntimeError` を解消）、入れ子 for ターゲットの無言 `None`、括弧付き for ターゲット、`test-sanitizers` が既定ビルドを壊す問題。`py2c help`・`--version`・入門ガイド・`make py2c` を追加 |

修正の詳細と再現条件は [`../review/BUGFIX_ALPHA1.0.md`](../review/BUGFIX_ALPHA1.0.md) に
Round 別の表として記録しています。

## 5. 検証結果（Alpha1.0 時点）

| 検証 | 結果 |
|---|---|
| CPython 差分コンフォーマンス | **868 アサーション一致**（C01–C770、F01–F33、R01–R65） |
| `make test` | 成功（ホスト CLI/GUI、GC 系、単一ヘッダー、組込み、自作OS、サニタイザ、ファジング等） |
| 生成 C の厳格ビルド | コーパス全体で `GENERATED_CFLAGS`（`-Wshadow=local`/`-Wclobbered` 等）警告 0 |
| 組込みスタック予算 | コア全ソースが 1 フレーム 4096 バイト以内（実測最大 3456 バイト） |
| 静的解析 | GCC `-fanalyzer` 警告 0、clang 静的解析 欠陥 0 |
| サニタイザ | ASan/UBSan/LSan 相当の回帰が成功（`str.format(**d)` の use-after-free 等を過去に検出し修正済み） |

## 6. 既知の制限

- `try`/`finally`・`with` をまたぐ `yield`、クロージャ内で定義したジェネレータ
- `bytes`/`bytearray`、任意精度整数（64 ビット固定）
- callable 変数への `*args`/`**kwargs` 可変長呼び出し
- クロージャの捕獲は値（Python の遅延束縛とは非互換。`lambda i=i: ...` で固定）

いずれも、不正な C を出力せず**位置付きの変換エラー**（または `--fallback` 時の実行時
`NotImplementedError`）として扱います。

## 7. 移行のヒント

- まず `./build/py2c run your_script.py` で動かし、エラーが出たらメッセージの行・列を確認してください。
- 未対応構文に当たった場合は `./build/py2c --supported` で代替手段（明示的なループ・
  `lambda i=i:` など）を検討してください。
- 組込みへ持っていく場合は `docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md` の手順と
  `templates/` のひな形を利用してください。
