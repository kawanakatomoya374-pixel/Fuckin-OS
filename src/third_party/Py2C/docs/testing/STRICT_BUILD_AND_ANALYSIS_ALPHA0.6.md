# 厳格ビルド・静的検査・組込みリソース検査 (Alpha0.6)

このドキュメントは、Python Code to C Alpha0.6 が **コード品質を機械的に担保する**
しくみ（警告基準、実行時ハードニング、静的解析、スタック使用量検査）をまとめた
ものです。対象読者は変換器本体を保守する人、および生成コードをカーネルへ
組み込む人です。

## 1. 警告基準 (`WARN_CFLAGS`)

コンパイラ本体・freestandingコア・単一ヘッダー検査は、すべて同じ `WARN_CFLAGS`
でビルドされます。`-Werror` が付くため、警告はビルド失敗として扱われます。

| カテゴリ | フラグ | 主に防ぐもの |
| --- | --- | --- |
| 基本 | `-Wall -Wextra -Wpedantic -Wshadow` | 未使用、影の宣言、ISO C11 からの逸脱 |
| 変換 | `-Wconversion -Wsign-conversion -Warith-conversion` | 暗黙の切り詰め、符号の取り違え |
| const契約 | `-Wcast-qual -Wwrite-strings` | 文字列リテラルへの書き込み、const破棄 |
| 組込み | `-Wvla -Wcast-align=strict -Wdouble-promotion -Wfloat-equal` | 可変長スタック配列、整列違反、暗黙のdouble化 |
| 書式 | `-Wformat=2 -Wformat-overflow=2 -Wformat-truncation=2 -Wformat-signedness -Wstringop-overflow=4 -Wstringop-truncation` | 書式と引数の不一致、snprintf の切り詰め・領域外書き込み |
| 制御フロー | `-Wswitch-default -Wimplicit-fallthrough=5 -Wjump-misses-init -Wlogical-op -Wduplicated-cond -Wduplicated-branches` | case漏れ、意図しないfall-through、宣言の飛び越し |
| 最適化前提 | `-Wstrict-overflow=2 -Wshift-overflow=2` | 符号付きオーバーフローを前提にした最適化 |
| メモリ | `-Wuse-after-free=3 -Warray-bounds=2 -Wrestrict` | 解放後利用、境界外アクセス、重なる引数 |
| 宣言 | `-Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition -Wredundant-decls -Wmissing-declarations -Wnested-externs` | プロトタイプ欠落、暗黙の関数宣言 |
| その他 | `-Wundef -Wunused-macros -Wmissing-parameter-type -Wcast-function-type -Wmultistatement-macros -Wsizeof-pointer-memaccess -Wsizeof-array-argument` | 未定義マクロ、使われない定義、誤ったsizeof |

### 意図的に外しているフラグ

| フラグ | 外している理由 |
| --- | --- |
| `-Wswitch-enum` | AST種別のswitchは `default:` で未対応種別を診断する設計。全列挙 (1200件超) に見合う安全性を生まない。網羅性は `tests/` のCPython差分回帰で担保する。 |
| `-Wdeclaration-after-statement` | C11の混合宣言は本コードベースの意図した書き方。C89配置への書き換えは646箇所の移動になり、初期化式の評価順も変わりうる。オブジェクト寿命とGC可視性はPredeclareパスとテストで担保する。 |
| `-Wc++-compat` | C++互換は対象外（C11専用）。`void*` からの暗黙変換だけで108件出る。 |
| `-Wnull-dereference` | TU毎の通常ビルドでは0件だが、単一ヘッダー構成では全ソースが1TUになりGCCの関数間解析が効くため、未チェック確保と誤検出が混在した約300件を報告する。有効化には全域のNULL契約監査が前提。 |

## 2. 実行時ハードニング

- ホスト向け `CFLAGS` には `HOSTED_HARDEN`（`-fstack-protector-strong`,
  `-fstack-clash-protection`, `-D_FORTIFY_SOURCE=3`）を既定で含める。
- `-D_FORTIFY_SOURCE=3` は `-O2` と併用されるため有効。バッファ操作の
  コンパイル時検査が強化される。
- freestanding/組込み構成ではこれらを使えないため、`EMBED_CFLAGS` 側では
  付与しない（`-ffreestanding -fno-builtin -fno-stack-protector`）。

## 3. 静的解析 (`make test-analyzer`)

GCCの `-fanalyzer` をコンパイラコアとランタイムへ適用します。
`-Wno-analyzer-too-complex` で解析時間を抑えつつ、次のような欠陥を検出します。

- 解放後利用・二重解放
- 確保失敗経路でのNULL参照
- 初期化されない値の使用
- ファイル記述子・リソースの取り違え

実行時間が長いため既定の `make test` には含めず、明示的に実行します。
指摘が出た場合は `-Werror` により失敗するため、`main` へ取り込む前に必ず
解消します（本リリースでは、例外オブジェクト生成に失敗した場合のNULL参照を
1件検出し、`p2c_raise` で安全に停止するよう修正しました）。

## 4. 組込みリソース検査 (`make test-stack-usage`)

カーネルのスタックは数KiBしか無いことが多く、1フレームが大きい関数はそのまま
スタックオーバーフローになります。`-Wstack-usage=$(STACK_USAGE_LIMIT)` を
`-Werror` と併用し、freestandingのランタイム（`python_code_to_c_runtime.c`）と
共通層（`python_code_to_c_common.c`）のすべてのフレームが上限以下であることを
検査します。既定の上限は4096バイトです。

主要なフレーム（2025年時点の測定）:

| 関数 | 使用量 | 備考 |
| --- | --- | --- |
| `p2c_class_mro` ほか MRO線形化 | 約3.4KiB | クラス階層のC3線形化。アリーナ3072バイト + 作業配列。初回のみ実行し結果をキャッシュする。 |
| `p2c_input` | 約1.0KiB | カーネル側 `read_line` 用の入力バッファ。 |

設計上の上限:

- `P2C_MRO_MAX_NAMES` = 32（1つのMROに載る名前の数）
- `P2C_MRO_MAX_BASES` = 8（1クラスの直接基底の数）
- `P2C_MRO_MAX_DEPTH` = 12（線形化の再帰段数）
- `P2C_MRO_ARENA_BYTES` = 3072

上限を超える階層ではC3を諦め、従来と同じ深さ優先の訪問順へフォールバックして
動作を継続します（静かに壊れることはなく、解決順は以前の実装と一致します）。

## 5. 回帰の実行順序

```sh
make full-build          # CLI/GUI/freestanding/単一ヘッダーを -Werror で構築
make test                # ホスト・GC・baremetal・embed・conformance(555)・fuzz・audit
make test-sanitizers     # ASan/UBSan で parser/GC/conformance
make test-gc-leaks       # LeakSanitizer で複数epochのリーク検査
make test-stack-usage    # スタックフレーム上限
make test-analyzer       # GCC静的解析（任意・長時間）
```

CPython差分（`tests/conformance_regression.sh`）は `run_differential_case` ごとに
Python実行結果と生成Cの実行結果を `diff` し、さらに期待行数と一致することを
検査します。新しい構文を追加するときは、必ずこの表へケースを追加します。
