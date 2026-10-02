# Python Code to C Alpha0.6

**Python Code to C Alpha0.6** は、Python 3系の実用的なサブセットをC11へ変換する、移植性重視のトランスパイラです。字句解析、AST構築、意味解析、Cコード生成、ランタイムを独立した層として構成し、通常のホストOSではCLIとして、自作OSや組み込み環境ではライブラリとして利用できます。

> Alpha0.6の設計目標は、ホスト環境での使いやすさを維持しながら、OS依存処理を明示的なプラットフォーム契約へ押し込み、コアをfreestandingビルドできるようにすることです。

## Alpha0.6の主な変更

| 領域 | Alpha0.6での変更 |
|---|---|
| 製品名称 | ディレクトリ、ファイル名、マクロ、CLI名、生成コードコメントを `Python Code to C Alpha0.6` / `python-code-to-c` 系へ統一 |
| 自作OS組み込み | `P2C_Platform` にメモリ、出力、時刻のフックを集約し、OS向けコア静的ライブラリを生成 |
| ビルド・起動 | `make` でHosted CLI、`make gui` でGUI、`make freestanding` でOS向けコアを構築。`make run INPUT=...`で変換・実行を一括化 |
| 構文対応 | 式、関数、クラス、module-level decorator、コンテナ、例外、list/dict/set内包表記、f-string、import、with、複数context managerのasync with、try、可変長引数に加え、クラスメソッドの`*args`・`**kwargs`・名前付き引数・`**mapping`展開、`dict.fromkeys`、pair iterable `dict.update`、setの包含関係・`pop`を対応 |
| GC hardening | shutdown/reinit時のregistry・async queue・pygame static class slot reset、constructor後段allocation失敗のrollback、runtime active状態、式評価中のTLS一時値（二項演算の左オペランド・処理中の例外）の明示ルートと例外脱出時の深さ巻き戻しを追加 |
| 診断 | 未対応構文を不正なCへ変換せず、ソース位置付きの明確なエラーとして報告 |
| スライス操作 | listの`a[b:c:d] = iterable`、`+=`、`del a[b:c:d]`に対応。通常スライスは長さ変更、拡張スライスは要素数一致を検査し、自己参照代入は安全に複製 |
| 移植資料 | `PORTING.md` に最小実装契約、ツールチェーン差し替え手順、Hosted/Freestandingの境界を記載 |

## クイックスタート

```sh
make check-tools
make
make run INPUT=examples/fib.py
make test
```

生成Cをホスト上で実行する場合は、ランタイムとプラットフォーム実装をリンクします。

```sh
cc -I./include /tmp/fib.c \
  src/runtime/python_code_to_c_runtime.c \
  src/common/python_code_to_c_common.c \
  src/platform/python_code_to_c_platform.c \
  src/platform/python_code_to_c_platform_hosted.c \
  src/modules/python_code_to_c_pygame.c -lm -o /tmp/fib
/tmp/fib
```

CLIの実行ファイルは `build/python-code-to-c` です。互換目的で `make` が `bin/python_code_to_c` というシンボリックリンクも生成します。

## 対応構文の範囲

Alpha0.6は、数値・文字列・真偽値・リスト・タプル・辞書・set、`...`（Ellipsis）、添字・スライス・スライス代入・属性アクセス、算術・比較・論理演算、条件式、lambda、関数定義と呼び出し、**module-level function/class decorator**、位置引数・キーワード引数・`*args`・`**kwargs`、if・while・for、break・continue・return、class・継承・**ネストしたクラス定義**（`class Outer: class Inner:`。生成CではC名を`Outer__Inner`に前置して衝突を避け、外側クラスの`__classobj()`が属性として登録するため`Outer.Inner`で参照できる）、try・except・finally・raise、with、複数context managerのasync with、import、list/dict/set内包表記、f-string、`str.format()`、`format`/`divmod`/`pow(a,b,mod)`/`callable`、`\xHH`・`\ooo`・`\uXXXX`・`\UXXXXXXXX`エスケープ、型注釈付き代入、複合代入を扱います。decoratorは式を上から下へ評価して下から上へ適用し、function/class objectをGC root付きcallable slotへ再束縛します。

多重継承（`class D(B, C)`）はPythonと同じC3線形化でMROを決めるため、ダイヤモンド継承（`class D(B, C)`、`class B(A)`、`class C(A)`）でも基底メソッドの選択と`super()`の解決先がCPythonと一致します。基底クラスのクラス属性もサブクラスのインスタンスからMRO順に見えます。メソッドを値として取り出すと束縛メソッド（`m = obj.method`）になり、`sorted(key=...)`や`map()`、コールバック引数へそのまま渡せます。メソッドデコレータは`@staticmethod`（selfを渡さない）・`@classmethod`（先頭にクラスオブジェクト）・`@property`（属性読み出しでゲッター実行。インスタンス属性より優先）と **`@x.setter`**（プロパティへの代入処理。setter未実装の代入は`AttributeError`）に対応し、プロパティはMRO順に継承・オーバーライドされます。`import math`は`floor`/`ceil`/`trunc`/`fabs`/`fmod`/`hypot`/`copysign`/`ldexp`/`degrees`/`radians`/`sin`/`cos`/`tan`/`asin`/`acos`/`atan`/`atan2`/`exp`/`expm1`/`log`/`log2`/`log10`/`log1p`/`sqrt`/`cbrt`/`pow`/`isnan`/`isinf`/`isfinite`/`fsum`/`prod`/`factorial`/`gcd`/`isqrt`/`comb`/`perm`/`erf`/`erfc`/`gamma`/`lgamma`と`pi`/`e`/`tau`/`inf`/`nan`を提供します。ジェネレータ式は複数for節・タプルターゲット・`if`フィルタに対応し、最も外側のiterableだけを生成時に評価するPythonの規則にも従います。`finally`内の`return`/`break`/`continue`は、finally本体を実行したうえで保留中の制御フロー（例外・`return`）を上書きします。`sorted()`/`min()`/`max()`はタプル・リストの辞書式比較に対応し、CPythonと同じく安定ソート（O(n log n)）で並べ替えます。順序を持たない型同士の比較はCPython同様`TypeError`になります。

実際の対応状況は次のコマンドで確認できます。

```sh
./build/python-code-to-c --supported
```

Python標準ライブラリ全体やCPythonの完全互換を目標にはしていません。未対応機能は、不正なCを出力する代わりに、ソース位置付きのエラーとして報告します。現在の主な未対応は、関数本体内の`class`定義、メソッドへのユーザー定義デコレータ、デコレータ関数側の`*args`/`**kwargs`、ネストしたクロージャから捕捉するジェネレータ式、`async for`の状態機械、複数のstarred代入対象、複素数型、`bytes`/`bytearray`です。

## 厳格ビルドと静的検査

コンパイラ本体とfreestandingコアは共通の厳格な警告基準 `WARN_CFLAGS` でビルドされます（すべて`-Werror`）。`-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wconversion -Wsign-conversion -Wcast-qual -Wwrite-strings -Wdouble-promotion -Wvla -Wfloat-equal` に加えて、`-Wcast-align=strict`（組込みで致命的なアラインメント違反）、`-Wlogical-op`/`-Wduplicated-cond`/`-Wduplicated-branches`、`-Wstrict-overflow=2`/`-Wshift-overflow=2`、`-Wformat-overflow=2`/`-Wformat-truncation=2`/`-Wstringop-overflow=4`、`-Wuse-after-free=3`/`-Warray-bounds=2`、`-Wjump-misses-init`/`-Wnested-externs`/`-Wmissing-declarations`、`-Wswitch-default`/`-Wimplicit-fallthrough=5`、`-Wunused-macros`、`-Warith-conversion`、`-Wcast-function-type` などを有効にしています。意図的に外しているフラグ（`-Wswitch-enum`、`-Wdeclaration-after-statement`、`-Wc++-compat`、`-Wnull-dereference`）とその理由は `Makefile` のコメントに明記しています。

ホスト向けビルドには実行時ハードニング（`HOSTED_HARDEN`: `-fstack-protector-strong`、`-fstack-clash-protection`、`-D_FORTIFY_SOURCE=3`）を既定で適用します。組込み構成ではこれらを分離し、代わりに次の検査を用意しています。

```sh
make test-stack-usage   # freestandingランタイムの全フレームが STACK_USAGE_LIMIT(既定4096バイト) 以下か
make test-analyzer      # GCC -fanalyzer で解放後利用・NULL経路・確保失敗の取り違えを検出
```

`test-stack-usage` はカーネルのスタックが数KiBしか無い環境を想定した検査で、`-Wstack-usage` を`-Werror`と併用します（現状の最大は約3.4KiB）。`-fanalyzer` は実行時間が長いため既定の`make test`には含めず、明示的に実行します（指摘があれば`-Werror`で失敗します）。

## 未対応構文のフォールバックと GC

未対応構文を含むコードでもビルドを止めたくない場合は `--fallback` を使います。

```sh
python-code-to-c input.py --fallback -o output.c
```

未対応の箇所（`bytes` リテラル・複素数リテラル・codegenが扱えない式/文など）を
**実行時に `NotImplementedError` を送出するスタブ**へ置き換え、変換とビルドは成功します。
その行を実行しなければ通常どおり動くため、大きなコードの一部だけが未対応でも
ビルドして試せます。既定（指定なし）は従来どおり、位置付きの明確な診断で停止します。

GCは**適応しきい値**を採用しました。収集してもほとんど解放されない状況では自動収集の
しきい値を倍々に伸ばし（上限4MiB）、よく解放できる状況では基準値へ戻すため、生存集合が
大きいプログラムで「収集のしきい値によるスタックスキャンの繰り返し」が起きにくくなります。
`p2c_gc_set_adaptive(false)` で固定しきい値の挙動に戻せ、`p2c_gc_stats()` で収集回数・
ピーク・現在のしきい値などを取得できます（回帰: `make test-gc-adaptive`）。

## 安全に実行する（サンドボックス予算）

信頼できないPythonを組込み環境で走らせるときは、実行予算を設定して暴走を例外で止めます。

```c
P2C_SandboxLimits limits = { .max_ticks = 100000, .max_allocs = 4096 };
p2c_sandbox_set(&limits);   /* 0 は無制限。タスクごとに呼ぶ */
```

- 生成Cの `while` ループ後退エッジには `p2c_sandbox_tick()` が自動で入るため、`while True:` のような無限ループも **`SandboxError`** として送出されます（ハングやクラッシュにはしません）。
- 確保予算（`max_allocs`）はオブジェクト確保のたびに検査し、超過時は同じく `SandboxError` になります。
- `p2c_sandbox_reset()` で消費量をゼロに戻せ、`p2c_sandbox_ticks()` / `p2c_sandbox_allocs()` / `p2c_sandbox_violations()` で実績を読めます。回帰: `make test-sandbox`。

## 文字列の `%` 書式

`"%s(%.2f)" % (name, area)` のような剰余演算子による書式化に対応しました。`%s` `%r` `%a` `%d` `%i` `%u` `%f` `%F` `%e` `%E` `%g` `%G` `%x` `%X` `%o` `%c` `%%` と、フラグ（`- + 空白 # 0`）・幅・精度・`*`（引数から取得）を扱い、右辺はタプル＝位置引数、`%(key)` を含む書式＝辞書、それ以外＝単一の値として解釈します（CPythonと同じ規則）。以前は左辺が文字列でも数値の剰余として扱われ `ZeroDivisionError` で落ちていました（回帰: `make test-conformance` の C556-C576、C577-C588）。
``make test-limits`` 固定上限の診断と -D 上書きの検証

## C99（TinyCC）ビルドとELF生成

既定ビルドはC11＋厳格警告のまま、**C99専用コンパイラ（TinyCC等）向けのビルド経路** と **ELF生成** を追加しました。詳細は [`docs/build/C99_TINYC_AND_ELF_ALPHA0.6.md`](docs/build/C99_TINYC_AND_ELF_ALPHA0.6.md) を参照してください。

```sh
make c99             # コンパイラ本体を -std=c99 -pedantic -Wall -Wextra -Werror でビルド
make test-c99        # C99で変換器と生成Cをビルドし、CPython差分コーパス（749件）を実行
make tcc             # CC=tcc でフル機能ビルド（TCC=/path/to/tcc で指定可）
make test-tcc        # tccで変換器・生成C・単一ヘッダーをビルドして差分コーパスを実行

make test-single-header-c99  # 単一ヘッダーをGCC C99（-pedantic-errors）でビルド・実行
make test-single-header-tcc  # 単一ヘッダーをTinyCCでビルド（hosted実行 + freestanding）

make elf             # 完全版（GCC・静的リンク）ELF + マニフェスト
make hobbyos elf     # HobbyOS向けELF（GCC・リンカスクリプト・W^X）+ マニフェスト
make tcc elf c99     # TinyCC(C99)で作る完全機能ELF（CLI+GUI、動的リンク）+ 実行確認
make tcc elf hobby c99  # TinyCC(C99)からHobbyOS向けELF（hobby は hobbyos の別名）
make hobbyos-elf     # 上と同じ（明示的なターゲット名）
make test-hobbyos-libc  # HobbyOS向けELFが要求する参照libm/libcをホストの実装と比較検証
```

`elf` 系の成果物はすべて **`.elf` 拡張子付き**で出力し、同じ内容の拡張子なしコピーも
互換用に残します（マニフェストは `<名前>.elf.txt`）。

| コマンド | 出力 |
| --- | --- |
| `make elf` | `build/elf/python-code-to-c.elf`（＋ `build/elf/python-code-to-c`） |
| `make tcc elf c99` | `build/elf-tcc/python-code-to-c.elf`（＋ `build/elf-tcc/python-code-to-c`） |
| `make hobbyos elf` / `make hobbyos-elf` | `build/hobbyos/python-code-to-c-hobbyos.elf` |
| `make tcc elf hobby c99` | `build/hobbyos-tcc/python-code-to-c-hobbyos.elf` |

```sh

- **TinyCCでのフル機能ビルドを検証済み**: 変換器本体（CLI・GUI含む）・生成C・単一ヘッダー（hosted/freestanding）のすべてがTinyCCでビルドでき、CLIの機能は通常ビルドと同じです（機能を削る `#ifdef` は入れていません）。tccが無い環境では `apt-get download tcc` + `dpkg-deb -x`（root不要）でも用意できます。
- **C99妥当性の検証**: `make c99` は `-pedantic -Werror` でコアをビルドするため、C11専用構文やGCC拡張が混入すると失敗します。生成Cは内包表記がGNU statement expressionを使うため `-std=c99`（`-pedantic`なし）でコンパイルします。
- **完全版ELF**: 動的ローダに依存しない静的リンクの実行ファイルを作り、`INTERP`セグメントが無いこと・実際に実行できることを自動検査します。
- **HobbyOS向けELF**: `p2c_hobbyos_entry()` をエントリとし、`.text`(R+X)/`.rodata`(R)/`.data`+`.bss`(R+W) の3セグメント（W^X）で構成します。`INTERP`が無く、**未定義シンボルも無い**（ランタイム同梱の最小libcとカーネルフックだけで完結）ことを自動検査します。ロードアドレスは `-DHOBBYOS_LOAD_ADDR=...` で変更できます。
- **参照libm/libc**: libcを持たないターゲット向けに、`floor`/`sqrt`/`sin`/`log`/`exp`/…/`snprintf`/`strtod`/`strtoll` の参照実装を `templates/hobby_os/embed/hobby_os_libc.c` に用意し、`make test-hobbyos-libc` がホストのlibm/libcと比較して精度と境界（±inf/NaN/0/負値）を回帰として固定します。

## 自作OS・組み込み環境への移植

自作OS向けには、まずコア静的ライブラリを生成します。

```sh
make freestanding
```

成果物は `build/freestanding/libpython-code-to-c-core.a` です。対象OSのクロスコンパイラを使う場合は、コンパイラ名やx86固有フラグを既定にしない`templates/toolchains/freestanding-c11.mk`を使い、`CC`、`AR`、`FREESTANDING_ARCH_CFLAGS`、`FREESTANDING_EXTRA_CFLAGS`、リンカ設定を外側のビルドから指定してください。`make -f templates/toolchains/freestanding-c11.mk PROJECT_ROOT=. print-config`で実効設定を確認できます。

OS側の実装は `include/platform/python_code_to_c_platform.h` の `P2C_Platform` に接続します。メモリ確保、再確保、解放、診断出力、時刻を提供し、`python_to_c()`や生成Cの実行より前に`p2c_platform_set()`で登録します。アロケータだけを差し替えたい場合は `p2c_platform_set_allocator(kalloc, krealloc, kfree, ctx)` の1回でカーネルのヒープを唯一のヒープにでき、`P2C_Allocator` を変換器コアの既定として注入する場合は `p2c_set_default_allocator()` を使います。`p2c_core_static_allocator_active()` が `false` なら、変換器はカーネルのヒープだけを使っています（静的フォールバックは `P2C_COMPILER_FALLBACK_HEAP_SIZE` でサイズ変更・無効化でき、既定は64KiB）。非局所脱出（`raise`/`except`/generator/`with`/`finally`）は `P2C_SETJMP`/`P2C_LONGJMP` の2つのマクロだけを通るため、カーネルの実装へ完全に差し替えられます（既定はfreestandingでコンパイラ組み込み、hostedでlibc）。`print`と例外診断は登録済み`write`コールバックへ出力されます。CLI・ファイルシステム・プロセス生成・GUIに依存せず、字句解析からCコード生成までを直接呼び出せます。詳細な段階的手順は [`PORTING.md`](PORTING.md) を、Hosted起動・makeターゲット・クロス構築の実行手順は [`docs/build/BUILD_AND_LAUNCH_ALPHA0.6.md`](docs/build/BUILD_AND_LAUNCH_ALPHA0.6.md) を参照してください。

### 変換済みモジュールをカーネルタスクとして実行する

組込みを最短で行う場合は、`--embed-entry` と統合ファサード `p2c_embed` を使います。

```sh
# 開発ホスト側: main() を持たない、カーネルから呼べるエントリを生成
python-code-to-c kernel_module.py --embed-entry kernel_python_program -o kernel_module.c
```

```c
/* カーネル側: 静的ヒープ/スタック/出力/時計を渡すだけで実行できる */
P2C_EmbedConfig cfg;
p2c_embed_config_init(&cfg);
p2c_embed_config_use_heap(&cfg, &my_heap, my_heap_storage, sizeof(my_heap_storage));
p2c_embed_config_use_uart(&cfg, my_uart_write, NULL);
p2c_embed_config_use_stack(&cfg, task_stack_base, task_stack_top);
cfg.clock_ms = my_clock_ms;
if (p2c_embed_start(&cfg) != 0) return -1;
p2c_embed_run_program(kernel_python_program);
p2c_embed_stop();
```

`p2c_embed` は組込みヒープ（境界タグ＋空きリスト＋隣接合体）、出力シンク、GCのスタック境界宣言、OOM方針（`MemoryError`送出またはpanic）、ヒープ/GC統計、未処理例外の診断を提供します。変換器コア自体も標準Cライブラリ無しでビルド・実行できるため、OS内でPythonソースをCへ変換することもできます。テンプレートは [`templates/hobby_os/`](templates/hobby_os/README.md)、契約の詳細は [`docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md`](docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md) を参照してください。

## ディレクトリ構成

| パス | 役割 |
|---|---|
| `include/` | 公開ヘッダーと層別API |
| `src/lexer` | Pythonソースの字句解析 |
| `src/parser` | ASTと構文解析 |
| `src/semantic` | 名前解決、機能検査、未対応診断 |
| `src/codegen` | Cコード生成 |
| `src/runtime` | オブジェクト・GC・例外ランタイム |
| `src/platform` | HostedおよびOSアダプタの境界 |
| `src/tools` | CLI、GUI、HTTPなどホスト専用ツール |
| `templates/toolchains` | Hosted/Freestanding/Cross設定 |
| `examples` | 構文対応の実例 |
| `tests` | スモークテストと回帰テスト |

## 検証

```sh
make test
make freestanding
```

`make test` は例、コンテナ、クラス、例外、f-string、内包表記、複数context managerのasync with、module-level decorator、import、プラットフォーム出力アダプタ、set comprehensionとdecoratorの期待診断、GC lifecycle、GCスタックスキャンの走査範囲、式評価中のTLS一時値のルートと例外脱出時の巻き戻し、allocator rollback、Hosted・厳格C11・freestanding単一ヘッダー、組込み統合（`p2c_embed` のヒープ・ライフサイクル・GC安全側停止・OOM、カーネル提供setjmp/longjmp、ベアメタルでの例外とスタックスキャン、カーネル相当環境での変換器コア実行、`--embed-entry`生成モジュールのCPython差分、組み込みテンプレートのコンパイル、LF/CRLF/CRの生成C一致、カーネルアロケータの注入と静的フォールバック判定、共有ヒープの一本化（NO_STDLIBスタブの`malloc`と`p2c_heap_*`が同じヒープを指すこと）、`P2C_SETJMP`/`P2C_LONGJMP`の差し替え）を検証し、**460件のCPython差分コーパス**と、決定的dict/set差分ファジングを実行します。`make CC=clang test-gc-leaks`は3回のruntime epochをLeakSanitizerで検査します。`make freestanding` は標準Cライブラリへ依存しないコンパイラコアを `-ffreestanding` でコンパイルします。`make CC=clang test-single-header-freestanding`は単一ヘッダーのfreestanding実装部にHosted allocator・出力・時刻参照が残らないことを検査します。実際のカーネルでのリンクには、対象OSのスタートアップ、リンカスクリプト、メモリアロケータ、必要な文字列・数値変換・数学関数の実装が別途必要です（`setjmp`/`longjmp`は既定でコンパイラ組み込みを使うため必須ではありません。自前実装をリンクする場合は `PYTHON_CODE_TO_C_NO_LIBC_STUBS` を使います。`examples/embed/x86_64_setjmp.c` と `templates/hobby_os/` がひな形になります）。

## バージョン

本リリースの識別子は **Python Code to C Alpha0.6 / 0.6.0** です。公開ヘッダー、ビルド定義、CLIの版番号はこの識別子に同期しています。変更履歴は [`CHANGELOG_ALPHA0.6.md`](CHANGELOG_ALPHA0.6.md)、移植仕様は [`PORTING.md`](PORTING.md)、構文・移植性の詳細は [`docs/spec/SYNTAX_AND_PORTABILITY.md`](docs/spec/SYNTAX_AND_PORTABILITY.md)、[`docs/spec/ASYNC_WITH_AND_SET_COMPREHENSION_ALPHA0.6.md`](docs/spec/ASYNC_WITH_AND_SET_COMPREHENSION_ALPHA0.6.md)、および[`docs/spec/DECORATORS_ALPHA0.6.md`](docs/spec/DECORATORS_ALPHA0.6.md)、単一ヘッダー統合は [`docs/build/SINGLE_HEADER_INTEGRATION_ALPHA0.6.md`](docs/build/SINGLE_HEADER_INTEGRATION_ALPHA0.6.md)、起動・ビルド手順は [`docs/build/BUILD_AND_LAUNCH_ALPHA0.6.md`](docs/build/BUILD_AND_LAUNCH_ALPHA0.6.md)、決定的コンテナファジングは [`docs/testing/CONTAINER_FUZZING_ALPHA0.6.md`](docs/testing/CONTAINER_FUZZING_ALPHA0.6.md) に記載しています。

## GCと自作OS向け描画

GCには重複登録を抑止する明示的ルート表、`p2c_gc_unregister_root()` による寿命管理、回収再入防止、ゼロしきい値の安全化、スタック成長方向に依存しない保守的走査を備えています。保守的スタックスキャンは**現在の関数フレームから宣言されたスタック上端まで**だけを走査するため、大きなスタック区間を宣言しても収集コストは使用量に比例します（語数は `p2c_gc_last_stack_words()` で確認できます）。二項演算の左オペランド（`p2c_binop_begin()` がTLSへ積み、`p2c_binop_finish()` が取り出す値）と処理中の例外もマーク対象なので、右オペランドの評価中に自動収集が走っても解放済みポインタを演算関数へ渡しません。式の途中で例外が脱出する場合に備え、生成Cの `try` は開始時の `p2c_binop_depth()` / `p2c_fstr_depth()` を保存し、例外ハンドラへ到達した時点（および未処理の再送出直前）に `p2c_binop_rewind()` / `p2c_fstr_rewind()` で戻します（f-stringビルダの解放もここで行うため、`try` の反復でリークしません）。メモリ確保は共通層の共有ヒープ抽象（`p2c_heap_alloc`／`p2c_heap_calloc`／`p2c_heap_realloc`／`p2c_heap_free`）へ一本化されており、`p2c_platform_set()` で登録したアロケータは**生成Cのランタイムと変換器コアの両方**で使われます。`p2c_runtime_shutdown()`はruntime所有objectと内部registry・async queue・static module slotをresetするため、終了後の`P2C_Object*`は再利用できません。C側でオブジェクトを保持する場合は、保持期間に対応して登録と解除を行い、shutdown後に外部slotを`NULL`へ更新してください。

自作OS側のグラフィカルUIには `include/platform/python_code_to_c_gui.h` を使用できます。矩形、枠線、線分、RGBAクリア、テキストを固定バッファへ描画コマンドとして記録し、`P2C_GuiBackend` でフレームバッファやGPUドライバへ渡します。GUIライブラリやPOSIXソケットをコアへ持ち込まないため、ホストWeb GUIと自作OSの直接描画を同じUI状態から切り替えられます。

## 複雑回帰テスト

`tests/complex_alpha06.py` は算術、文字列、コンテナ、内包表記、反復、関数、可変長引数、例外、クラス、デフォルト引数、f-string、辞書操作、組み込み関数などを含む60ケースのコーパスです。`make test` は通常のスモークテストに加えて `tests/test_gc_gui.c`、複数runtime epochとroot/cycleを検証する`tests/test_gc_runtime_reinit.c`、決定的allocator失敗rollbackを検証する`tests/test_gc_allocation_failure.c`をビルド・実行します。
