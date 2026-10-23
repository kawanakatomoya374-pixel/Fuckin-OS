# Python Code to C Alpha0.6 単一ヘッダー統合ガイド

## 目的

`include/python_code_to_c_single.h` は、**Python Code to C Alpha0.6** の公開API、字句解析器、構文解析器、意味解析器、Cコード生成器、実行時オブジェクト系、GC、集合・辞書ランタイム、GUI抽象化を一つのヘッダーに展開した配布物です。追加のPy2cソースファイルをコンパイルまたはリンクせず、ホストアプリケーションへ変換器を組み込めます。

このヘッダーは手編集しません。`tools/generate_single_header.sh` が `include/` と `src/` の正本から生成します。ソース修正後は `make single-header` を実行してください。

## 利用形態

| 利用形態 | 定義するマクロ | リンク要件 | 用途 |
|---|---|---|---|
| Hosted埋込み | `P2C_SINGLE_HEADER_IMPLEMENTATION` | 一つの翻訳単位だけで`-lm` | Linux等でPy2cをアプリケーションへ直接組み込む |
| 宣言のみ参照 | なし | 実装を定義した別翻訳単位 | 複数Cファイルのプロジェクト |
| 自作OS・freestanding | `P2C_SINGLE_HEADER_IMPLEMENTATION`、`PYTHON_CODE_TO_C_NO_STDLIB` | OS側の`P2C_Platform`登録、数値/数学補助 | 標準Cライブラリを持たないOSへの統合 |
| 自作OS・独自互換層 | 上記に加え`P2C_SINGLE_HEADER_NO_HOSTED` | OS側が`p2c_platform_write`、`read_line`、`abort`も実装 | I/O互換フックを完全に差替える統合 |

> `P2C_SINGLE_HEADER_IMPLEMENTATION` は**プログラム全体で一度だけ**定義します。複数の翻訳単位で定義すると、実装シンボルが重複します。

## Hostedでの最小例

```c
#define P2C_SINGLE_HEADER_IMPLEMENTATION
#include "python_code_to_c_single.h"

int main(void) {
    char *generated = NULL;
    P2C_Result result = python_to_c("print(dict(answer=42))\n", NULL, &generated);
    if (result != P2C_OK) return 1;
    puts(generated);
    free(generated);
    return 0;
}
```

GCCまたはClangでは、次のようにコンパイルします。GCCを導入しない環境では、Clangだけで同じISO C11経路を利用できます。

```sh
clang -std=c11 -pedantic-errors -O2 -Wall -Wextra -Werror -Wpedantic \
  -Wshadow -Wformat=2 -Wno-format-nonliteral \
  -Wstrict-prototypes -Wmissing-prototypes \
  -I./include embed.c -lm -o embed
```

## 自作OSでの統合境界

freestanding統合では、まず`PYTHON_CODE_TO_C_NO_STDLIB`を定義し、OS側がメモリ確保、再確保、解放、出力、時刻を`P2C_Platform`へ接続します。標準の単一ヘッダーはfreestanding時も互換出力アダプタを含み、登録済み`P2C_Platform.write`へ`print`と例外診断を転送します。`P2C_SINGLE_HEADER_NO_HOSTED`は、OS側が互換出力・入力・停止関数も完全に提供するときだけ定義します。OSは文字列/数値変換と数学補助をリンクまたは提供します。

単一ヘッダー冒頭の`_GNU_SOURCE`/`_POSIX_C_SOURCE`は**Hosted構成でのみ**定義します。`PYTHON_CODE_TO_C_NO_STDLIB`を定義したビルド（自作OS・freestanding）ではfeature macroを一切追加しないため、単一ヘッダーを取り込んだだけでPOSIX/GNU拡張が暗黙に有効化されることはなく、ISO C11・freestandingの前提が保たれます。Hostedでこれらの拡張が必要な場合だけ、`PYTHON_CODE_TO_C_NO_STDLIB`を定義せずに取り込みます。

```c
#define PYTHON_CODE_TO_C_NO_STDLIB
#define P2C_SINGLE_HEADER_IMPLEMENTATION
#include "python_code_to_c_single.h"

/* この後にターゲットOSのP2C_Platform接続を実装する。 */
```

単一ヘッダーはコンパイラ・ランタイムの実装本体を提供しますが、**OS固有I/Oやメモリの意味論を代替するものではありません**。GC管理オブジェクトをOSの非同期構造へ保存する場合は、既存の`p2c_gc_register_root()`／`p2c_gc_unregister_root()`契約を守ります。

## 生成内容と非包含物

| 含まれるもの | 含まれないもの |
|---|---|
| 公開ヘッダー、コンパイラコア、ランタイム、GC、コンテナ、集合・辞書実装、pygame互換モジュール、GUI抽象化、Hostedプラットフォーム実装 | CLIの`main()`、対話GUIの`main()`、HTTP GUIサーバー、C-to-Python補助ツール、外部OS実装 |

この境界により、ヘッダーを埋め込んだプログラムがアプリケーション固有のエントリポイントを自由に持てます。CLIやローカルWeb GUIをそのまま配布する場合は、通常の`make`／`make gui`を利用します。

## 品質契約

`make test-single-header` は生成直後の単一ヘッダーだけを含む`tests/test_single_header.c`をコンパイル・実行します。追加のPy2c `.c` ファイルはリンクしません。GCCとClangの両方で、ISO C11、拡張警告、警告即エラーを用いて検証します。さらに、`make CC=clang test-single-header-c11`はGCCなしでClangだけを使い、`-std=c11 -pedantic-errors`の自己完結ビルドを強制します。`make CC=clang test-single-header-freestanding`は`PYTHON_CODE_TO_C_NO_STDLIB`の実装部をコンパイルし、`malloc`、`realloc`、`free`、`fwrite`、`fputs`、`clock`へのHosted libc参照がないことを検査します。`snprintf`、`strtoll`、`strtod`、`floor`、`fmod`、`pow`、`sqrt`、`sin`、`cos`は、正確な数値・文字列意味論のためOSまたはリンクする最小Cライブラリが提供する明示的な補助契約です。

単一ヘッダーはコンパイラ本体に対してISO C11を守ります。一方、変換対象のPythonがlambdaや内包表記などの式位置一時構築を必要とする場合、生成Cは通常モードでGNU statement expressionを利用します。移植性を最優先する生成対象には`--c11`を指定し、非C11経路を変換時に拒否してください。

## 再生成と確認

| コマンド | 保証する内容 |
|---|---|
| `make single-header` | 正本ソースから`python_code_to_c_single.h`を再生成する |
| `make test-single-header` | 単一ヘッダー単独の自己完結ビルドを確認する |
| `make CC=clang test-single-header-c11` | GCCなしでClangだけを使い、単一ヘッダーをISO C11の`-pedantic-errors`で自己完結ビルド・実行する |
| `make CC=clang test-single-header-freestanding` | `PYTHON_CODE_TO_C_NO_STDLIB`の実装部を翻訳し、Hosted allocator・出力・時刻参照が残らないことを検査する。数値/数学補助はOS提供契約 |
| `make full-build` | CLI、GUI、freestanding、単一ヘッダーをクリーン状態から生成する |
| `make test` | 単一ヘッダー試験、CPython差分、GC/GUI、strict C11診断、移植性成果物を検証する |

単一ヘッダーは生成物であるため、レビューでは生成スクリプト、正本ヘッダー、正本ソース、単一ヘッダー試験をセットで確認します。
