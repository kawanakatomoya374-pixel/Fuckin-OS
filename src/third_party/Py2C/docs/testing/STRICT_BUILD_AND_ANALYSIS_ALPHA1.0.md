# 厳格ビルド・静的検査・組込みリソース検査 (Alpha1.0)

このドキュメントは、Python Code to C Alpha1.0 が **コード品質を機械的に担保する**
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
  `-fstack-clash-protection`, `-D_FORTIFY_SOURCE=3`,
  `-ftrivial-auto-var-init=zero`）を既定で含める。
- `-D_FORTIFY_SOURCE=3` は `-O2` と併用されるため有効。バッファ操作の
  コンパイル時検査が強化される。
- `-ftrivial-auto-var-init=zero` は自動変数を常にゼロ初期化する。未初期化値が
  分岐やポインタへ流れる「再現しない不具合」を決定的に潰す（GCC12+/clang8+）。
  `-ftrivial-auto-var-init=pattern` に差し替えると未初期化読み出しを 0xFE で
  露出させられ、`tests/avinit_differential_alpha10.sh` の差分検出に使える。
- freestanding/組込み構成ではこれらを使えないため、`EMBED_CFLAGS` 側では
  付与しない（`-ffreestanding -fno-builtin -fno-stack-protector`）。

## 2b. 第二の警告源・第二のコンパイラ（clang）

`WARN_CFLAGS` は GCC 基準なので、GCC が持たない軸は clang で補完します。

| ターゲット | 内容 |
| --- | --- |
| `make test-warn-clang` | 全ソースを clang の `-Wall -Wextra -Werror` + `-Wshorten-64-to-32 -Wconditional-uninitialized -Wenum-conversion -Wimplicit-int-conversion -Wshadow-all -Wunreachable-code -Wassign-enum -Wcomma -Wabsolute-value -Wloop-analysis -Wsizeof-array-decay -Wformat-non-iso` でコンパイルする |
| `make test-clang-build` | clang で CLI 本体を `-Werror` 込みの厳格基準でビルドし、その変換器でコーパスとプローブを走らせる |

この 2 つは「警告が 0 件」「clang 産の変換器でも CPython と一致」を継続的に
担保します（本ラウンドで clang が検出した `-Wunreachable-code` と、GCC が
検出した `-Wbad-function-cast` はいずれも `src/platform/python_code_to_c_platform.c`
の `host_clock_ms()` で、死コードと関数呼び出し結果の直接キャストを解消しました）。


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
make full-build           # CLI/GUI/freestanding/単一ヘッダーを -Werror で構築
make test                 # ホスト・GC・baremetal・embed・conformance(770)・fuzz・audit
make test-sanitizers      # ASan/UBSan で parser/GC/conformance
make test-gc-leaks        # LeakSanitizer で複数epochのリーク検査
make test-stack-usage     # スタックフレーム上限
make test-analyzer        # GCC静的解析（任意・長時間）
make test-warn-clang      # clang 第二意見の警告（GCCが見ない軸）
make test-strict-profiles # 6章の厳格プロファイルを一括実行（長時間）
```

## 6. 厳格プロファイル（動的解析の深掘り）

既定ビルドは移植性（freestanding/TinyCC/C99/静的ELF）を守るため、追加の
サニタイザや硬化オプションを既定へは入れられません。そこで **専用ターゲット**
として外付けし、生成プログラム + ランタイムを対象に動的解析を深掘りします。
各プロファイルは `CPython差分コーパス`（`tests/conformance_regression.sh`）と
`意味論プローブ`（`tests/semantic_probe_alpha10.sh`）の両方を走らせます。

| ターゲット | 主眼 | 主なフラグ |
| --- | --- | --- |
| `make test-asan-strict` | ヒープ/スタック破壊 | ASan + `-fsanitize-address-use-after-scope`、`ASAN_OPTIONS=strict_string_checks=1`（`ASAN_UAR=1` で `detect_stack_use_after_return=1`、`ASAN_PTR_PAIRS=1` で `detect_invalid_pointer_pairs=2` を追加。いずれも実行が数倍遅くなるため opt-in） |
| `make test-ubsan-deep` | 未定義動作の網羅 | `-fsanitize=undefined,bounds-strict,object-size,builtin,float-cast-overflow,float-divide-by-zero,nonnull-attribute,returns-nonnull-attribute,pointer-overflow,shift,bool,enum,unreachable,vla-bound,function,return,alignment,integer-divide-by-zero` + `-fno-sanitize-recover=all` |
| `make test-msan` | 未初期化メモリの読み出し | clang `-fsanitize=memory -fsanitize-memory-track-origins=2`（ASan が見えない領域） |
| `make test-lsan-generated` | 解放漏れ | ASan + LSan（`detect_leaks=1`）を生成プログラムへ適用 |
| `make test-clang-integer` | 整数・暗黙変換 | clang `-fsanitize=address,integer,implicit-conversion,unsigned-integer-overflow,local-bounds` |
| `make test-harden-generated` | 生成コードの硬化 | `-O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection -fstrict-flex-arrays=3 -fcf-protection=full` |
| `-O3` プロファイル（バッチ専用） | 最適化依存の不具合 | `-O3 -fno-strict-aliasing`（未初期化値や longjmp 跨ぎの値が最適化で初めて表面化する） |
| `lsan` プロファイル（バッチ専用） | 解放漏れ | ASan + LSan（`detect_leaks=1`）を生成プログラムへ適用 |
| `make test-hardened-core` | 変換器本体の総合硬化 | GCC `-fhardened -fstrict-flex-arrays=3` + `-Wl,-z,relro,-z,now,-z,noexecstack` |
| `make test-avinit-differential` | 未初期化スタックの影響 | 変換器を `-ftrivial-auto-var-init=pattern` でも作り、生成Cと実行結果を通常ビルドと差分する |

補助スクリプト:

- `tests/strict_dynamic_alpha10.sh <profile>` … プロファイル定義の本体。
  `P2C_TEST_CFLAGS` / `P2C_TEST_LDFLAGS` / `P2C_TEST_ENV` を両ハーネスへ注入する。
- `tests/strict_batch_alpha10.sh [stage ...]` … 全プロファイルを順に実行し、
  `build/strict/summary.txt` に PASS/FAIL を集約する（失敗しても続行して全結果を出す）。
- `tests/avinit_differential_alpha10.sh <通常変換器> <pattern変換器>` …
  `AVINIT_RUN=1` で生成プログラムの実行結果まで比較する。
- `tests/clang_build_alpha10.sh` … clang ビルド + コーパス（`make test-clang-build`）。

ハーネス側の厳格化（通常実行にも効く）:

- 生成プログラムの **終了コード** を検査する（`run_differential_case`）。標準出力が
  一致していても SIGABRT などで落ちていれば失敗にする。
- サニタイザプロファイル時は、**サニタイザのレポート自体を失敗として扱う**
  （`AddressSanitizer` / `runtime error:` / `SUMMARY:` を検出したら即失敗）。

### 6.1 このラウンドで見つけて直したもの

| 種別 | 内容 | 対応 |
| --- | --- | --- |
| 回帰（実バグ） | 組込み名を「値」として解決する変更が、`c_identifier_collision` の **パラメータ名 `abs`/`round`** を奪い、加算が関数オブジェクトになり `TypeError` で異常終了していた | 名前解決に `is_declared()` を追加（宣言済みの名前はユーザー変数として扱う）。終了コード検査を先に入れたことで検出できた |
| 死コード | `src/platform/python_code_to_c_platform.c: host_clock_ms()` の `(CLOCKS_PER_SEC ? CLOCKS_PER_SEC : 1)` は clang `-Wunreachable-code` が指摘する常に真の三項演算 | 0除算ガードを削除（C11 7.27.1 が正の定数と規定）。あわせて `clock() * 1000` の桁あふれを避けるため浮動小数で換算 |
| 警告（GCC） | 同関数の `(uint64_t)((double)clock() * ...)` が `-Wbad-function-cast`（関数呼び出し結果の直接キャスト）に該当 | いったん `clock_t` 変数へ受けてから変換 |
| FP 例外耐性 | `math.inf` / `math.nan` を `1.0 / 0.0` / `0.0 / 0.0` で作っていた。IEEE-754 では正しいが、FP 例外をトラップする環境では停止しうる（深い UBSan の `float-divide-by-zero` が `p2c_runtime_init` で検出） | C99 の `INFINITY` / `NAN` を優先し、無い処理系だけ従来の除算へフォールバック（`#ifdef` ガード） |
| 設定バグ（ハーネス） | GCC 版プロファイルに clang 専用の `-fsanitize=function,return` を含めていた／clang が `-Wno-clobbered` を知らず `-Werror` で失敗していた | GCC の UBSan リストを GCC が解釈できる最大構成へ修正し、生成コード側は `-Wno-unknown-warning-option` を併記 |
| 差分ファザが検出（実バグ） | 空白モードの `str.rsplit()` が、空文字列・空白のみの文字列で `['']` を返していた（CPython は `[]`）。`tests/string_fuzz_diff_alpha10.py`（シード付き乱数 × 150 ケース）が `case_098` として検出 | ランタイムで「空白モードかつ右端の空白を除くと空」なら空リストを返すよう修正。`tests/empty_split_alpha10.py`（C764-C770）として回帰へ登録 |
| サニタイザ検出（意図的なラップ） | clang の `-fsanitize=integer` が djb2 / FNV ハッシュの 32bit ラップを `left shift ... cannot be represented in type 'uint32_t'` として報告し、`-fno-sanitize-recover=all` のため全 61 プローブが停止していた（C の仕様どおりの剰余演算で**未定義動作ではない**） | 意図的なラップを行う `p2c_hash_str` / `p2c_obj_hash` に clang 限定の `P2C_INTENTIONAL_WRAP`（`no_sanitize("integer","shift","implicit-conversion")`）を付与し、「32bit の剰余が仕様」とコメントで明示。GCC では空定義のため影響なし |

### 6.2 解析上の限界（既知の抑制）

- **未処理例外の出力先（既知の差）**: 実行時に例外が未処理のまま終わると、ランタイムは
  `型: メッセージ` を **標準出力** へ書いてから abort する（CPython は標準エラーへ
  トレースバックを出し、標準出力には何も書かない）。差分ハーネスの検出力を上げるため
  現状はこのままにしているが、標準出力の完全一致を求める用途では差になる。本筋の修正は
  platform 層へ stderr 書き込み API を足すこと（`p2c_platform_write` は stdout 固定）。

- **`-Wclobbered`（生成コード側で抑制）**: 生成コードは `for` ループごとに
  イテレータ用 `setjmp` を置くため、GCC は「setjmp と longjmp の間で変更された
  変数」すべてに警告を出す。longjmp をまたいで実際に読まれる値（イテレータの
  item、`(void)self` のキャスト対象など）は codegen が `volatile` を付けており、
  また longjmp 後に読まれない変数は不定値でも意味に影響しない。したがって
  厳格プロファイルの生成コード側でのみ `-Wno-clobbered` を付ける
  （`tests/strict_dynamic_alpha10.sh` に理由を明記）。
- **`-m32`（未使用）**: 32bit はコンパイルは通るが、この環境に 32bit 用 `libgcc`
  が無くリンクできないため、意味論の差分検証には使えない。
- **`-Wnull-dereference`**: 1章の表のとおり、単一ヘッダー構成で誤検出が混在する
  ため無効のまま（NULL 契約監査が前提）。



CPython差分（`tests/conformance_regression.sh`）は `run_differential_case` ごとに
Python実行結果と生成Cの実行結果を `diff` し、さらに期待行数と一致することを
検査します。新しい構文を追加するときは、必ずこの表へケースを追加します。
