# Python Code to C Alpha0.6 — 起動・ビルド手順

## 1. 目的

この手順書は、Hosted CLI、ローカルGUI、CPython差分テスト、単一ヘッダー、および自作OS向けfreestandingコアを、**同一のMakefile契約**から構築・起動する方法を定義します。プロジェクトルートは `Python Code to C Alpha0.6` です。

> **原則**として、ホスト環境では `make check-tools` を最初に実行し、クロス環境では `CC`、`AR`、必要なABIフラグをmake変数として渡します。ソースやテンプレートを環境ごとに書き換える必要はありません。

## 2. 必要なツール

| 用途 | 必要なコマンド | 備考 |
|---|---|---|
| Hostedビルド | C11コンパイラ、`make`、`ar` | GCCまたはClangを検証済み。任意のISO C11対応コンパイラを指定可能 |
| 差分テスト | `python3` | CPythonを基準出力の取得に使用 |
| Hostedリンク | 数学ライブラリ | 標準設定では`-lm`を利用 |
| 自作OSビルド | 対象ABIのC11クロスコンパイラとアーカイバ | OS側のランタイム補助・スタートアップ・リンクスクリプトは別途提供 |

プロジェクトルートで、現在の指定が有効か確認します。

```sh
make check-tools
```

Clangのみを使うホストでは次のように指定できます。

```sh
make CC=clang AR=ar check-tools
```

## 3. Hosted CLIのビルドと起動

通常ビルドはHosted CLIを `build/python-code-to-c` に生成します。互換用の `bin/python_code_to_c` シンボリックリンクも生成されます。

```sh
make all
./build/python-code-to-c --self-test
./build/python-code-to-c --supported
```

PythonファイルをCへ変換する最小手順は次のとおりです。

```sh
./build/python-code-to-c examples/fib.py -o build/fib.c
```

`make run` はCLIビルド、変換、生成Cのホストコンパイル、実行を一括で行います。対象は `INPUT` で切り替えます。

```sh
make run INPUT=examples/fib.py
make CC=clang run INPUT=tests/string_partition_alpha06.py
```

## 4. ローカルGUIのビルドと起動

GUIはHosted開発環境向けのフロントエンドです。自作OSの直接描画にはこの実行形式ではなく、`P2C_GuiBackend` をOS側のフレームバッファまたはGPUドライバへ接続します。

```sh
make gui
./build/python-code-to-c-gui
```

次のターゲットはビルド後にGUIを起動します。

```sh
make run-gui
```

## 5. 単一ヘッダーの再生成と検証

`include/python_code_to_c_single.h` は生成物です。正本の `include/` または `src/` を変更した後は必ず再生成します。

```sh
make single-header
make test-single-header
```

GCCなしの厳格ISO C11経路と、freestanding実装部の翻訳契約はそれぞれ次で検証します。

```sh
make CC=clang test-single-header-c11
make CC=clang test-single-header-freestanding
```

後者は、Hostedのアロケータ・出力・時刻シンボルを要求しないことを検査します。`snprintf`、`strtoll`、`strtod`、`floor`、`fmod`、`pow`、`sqrt`、`sin`、`cos`は、移植先OSまたは接続する最小Cライブラリが提供する明示的な補助契約です。

## 6. 自作OS向けfreestandingコア

標準のfreestanding成果物は次で作成します。

```sh
make freestanding
```

成果物は `build/freestanding/libpython-code-to-c-core.a` です。テンプレートは `templates/toolchains/freestanding-c11.mk` であり、コンパイラ名やCPU固有フラグを固定しません。クロス構築では対象ツールチェーンをそのまま指定します。

```sh
make freestanding \
  CC=x86_64-elf-clang \
  AR=llvm-ar \
  FREESTANDING_ARCH_CFLAGS='-mno-red-zone' \
  FREESTANDING_EXTRA_CFLAGS='-DP2C_GC_ROOT_CAPACITY=1024'
```

独自ビルドシステムからテンプレートだけを呼び出す場合は、次の形を使用します。

```sh
make -f templates/toolchains/freestanding-c11.mk \
  PROJECT_ROOT=. \
  CC="$CROSS_CC" \
  AR="$CROSS_AR" \
  BUILD=build/target-core \
  FREESTANDING_ARCH_CFLAGS="$TARGET_ABI_FLAGS" \
  EXTRA_CFLAGS="$KERNEL_EXTRA_CFLAGS"
```

現在のテンプレート変数は次で確認できます。

```sh
make -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. print-config
```

OS側では、`P2C_Platform`を `p2c_platform_set()` で登録してからコンパイラまたは生成Cを実行します。`print`と例外診断は登録済みの `write` コールバックへ到達します。GC管理オブジェクトを割込みキュー、タスク状態、GUI状態など保守的スタック走査の外へ保持する場合は、対応する保持スロットを `p2c_gc_register_root()` と `p2c_gc_unregister_root()` で管理してください。詳細なアダプタ実装順は [構文・移植性仕様](../spec/SYNTAX_AND_PORTABILITY.md) を参照してください。

## 7. 品質ゲート

| 目的 | コマンド |
|---|---|
| Hosted CLI・GUI・freestanding・単一ヘッダーのクリーン構築 | `make CC=clang full-build` |
| 全回帰 | `make CC=clang test` |
| GCCクロスチェック | `make CC=gcc full-build && make CC=gcc test` |
| CPython差分だけを実行 | `make test-conformance` |
| 単一ヘッダー3経路 | `make test-single-header && make test-single-header-c11 && make test-single-header-freestanding` |
| 自作OS統合（組込みAPI） | `make test-embed-runtime` |
| 自作OS統合（カーネル提供setjmp） | `make test-freestanding-setjmp` |
| 自作OS統合（オンデバイス変換） | `make test-embed-compile` |
| 自作OS統合（`--embed-entry`生成モジュールの実行） | `make test-embed-generated` |
| ベアメタル（自作libcスタブ）例外とスタックスキャン | `make test-baremetal-exceptions` |
| アロケータ注入（カーネルヒープ/`P2C_Allocator`） | `make test-allocator-injection` |
| `P2C_SETJMP`/`P2C_LONGJMP`のOS差し替え | `make test-setjmp-hook` |
| 共有ヒープの一本化（スタブ`malloc`＝カーネルヒープ） | `make test-heap-unification` |
| GCスタックスキャンの走査範囲 | `make test-gc-stack-scan-scope` |
| GCの式評価中一時値（TLS）と例外脱出時の巻き戻し | `make test-gc-temp-roots` |
| 組み込みテンプレートの検証 | `make test-hobby-os-template` |
| 改行コード回帰（LF/CRLF/CR） | `make test-crlf` |
| テンプレートの掃除 | `make freestanding-clean` |
| 全生成物の掃除 | `make clean` |

`make test-crlf` と `make test-embed-generated` は生成Cのコンパイルを含むため、GNU拡張を含む生成コード基準（`-std=gnu11`）でビルドします。ISO C11のみを対象にする場合は `--c11` を使って内包表記などを変換時に拒否してください。

`make help` はターゲットの短い一覧を表示します。ビルド失敗時は、まず `make check-tools`、次に `make -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. print-config` を実行し、実際のコンパイラ、アーカイバ、CFLAGS、CPPFLAGSを確認してください。

## 8. dict・set差分ファジング

`make test`は通常のCPython差分に加え、固定seedによるdict・set操作列の差分ファジングを実行します。単独で実行するには次を使います。

```sh
make test-container-fuzz
```

既定では3 seed、各48操作を実行します。変更前後の深掘りには、seedと操作数を明示して実行できます。

```sh
make CC=clang \
  P2C_FUZZ_CASES=64 \
  P2C_FUZZ_SEEDS='1 7 42 99 31337 65537 104729 2147483647' \
  test-container-fuzz
```

差分、変換失敗、または生成Cのコンパイル失敗が起きた場合、ランナーは失敗する最小操作prefixを `build/tests/container_fuzz/repro-seed-<seed>-cases-<count>/` に保存し、再現コマンドを出力します。seed、操作数、失敗最小化、固定回帰への昇格の詳細は [`../testing/CONTAINER_FUZZING_ALPHA0.6.md`](../testing/CONTAINER_FUZZING_ALPHA0.6.md) を参照してください。

| 種別 | コマンド | 目的 |
|---|---|---|
| 標準ファジング | `make test-container-fuzz` | 通常品質ゲートと同一の3 seed・144操作 |
| 拡張ファジング | `make P2C_FUZZ_CASES=64 P2C_FUZZ_SEEDS='...' test-container-fuzz` | 追加seedで状態遷移を深掘り |
| 失敗の再現 | 標準エラーに出た`CC`、`P2C_FUZZ_SEEDS`、`P2C_FUZZ_CASES`を指定 | 同じ入力・同じ差分を再生成 |
