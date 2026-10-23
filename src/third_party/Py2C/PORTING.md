# Python Code to C Alpha1.0 — 自作OS・freestanding移植ガイド

## 1. 目的と移植境界

**Python Code to C Alpha1.0** は、変換器コア、ランタイム、OS依存のプラットフォーム層を分離しています。自作OS側が実装するのは、メモリ、出力、時刻を接続する小さな`P2C_Platform`アダプタです。CLI、ホストGUI、プロセス起動、ファイル入出力はfreestandingコアの必須要件ではありません。

> freestandingコアは、OSアダプタを登録するまで確保・出力を行わない設計です。`python_to_c()`、生成Cの実行、またはGCの利用より**前**に、必ず有効な`P2C_Platform`を登録してください。

| 層 | 自作OS側の責務 | Alpha1.0側の責務 |
|---|---|---|
| メモリ | `alloc`、`realloc`、`free`をカーネルヒープ、arena、ページ割当て器へ接続 | コンパイラ・ランタイムの全確保をプラットフォーム契約へ委譲（共有ヒープ。§4.1） |
| 出力 | `write`をシリアル、カーネルログ、画面コンソール等へ接続 | `print`、診断、例外メッセージの出力経路を提供 |
| 時刻 | 必要に応じて単調ミリ秒時計を提供 | GUI・時間依存APIの時刻接続点を提供 |
| C実行基盤 | 整数演算、必要なコンパイラランタイム、必要なら`setjmp`/`longjmp`（§4.2）を提供 | 例外フレーム、GC、明示cause、generator状態機械、コード生成を提供 |
| GUI | 必要時に描画コマンドをフレームバッファまたはGPUへ実行 | デバイス非依存の`P2C_GuiCommand`列を生成 |

## 2. 推奨する移植順序

移植は、一度にCLIやGUIを動かすのではなく、まず静的ライブラリのリンク、次にプラットフォーム、変換、生成C実行、GC、GUIの順で進めます。各段階の受入条件を満たしてから次へ進むことで、メモリ不具合とコード生成不具合を混同しません。

| 段階 | 実施内容 | 最低限の受入条件 |
|---:|---|---|
| 1 | クロスCコンパイラと`ar`を決定し、freestandingコアを構築 | `libpython-code-to-c-core.a`が生成される |
| 2 | `P2C_Platform`の5コールバックを実装・登録 | 小さな確保、解放、出力がOS上で動く |
| 3 | `python_to_c()`で固定文字列を変換 | `P2C_OK`と生成C文字列を取得できる |
| 4 | 生成Cとランタイムを同じOSアダプタでリンク | `print`、算術、list、例外の最小プログラムが動く |
| 5 | GCルート登録を非同期所有者へ導入 | 明示ルートのオブジェクトが収集されない |
| 6 | 必要時だけGUIバックエンドを接続 | コマンド列が期待する順序で描画される |

## 3. コンパイラ非固定のfreestandingビルド

標準のテンプレートは`templates/toolchains/freestanding-c11.mk`です。GCCという名前やx86固有フラグを既定にしません。`CC`、`AR`、`CFLAGS`、`CPPFLAGS`、`BUILD`を外側のビルドから置き換えられます。

```sh
cd 'Python Code to C Alpha1.0'
make -f templates/toolchains/freestanding-c11.mk \
  PROJECT_ROOT=. CC=clang AR=llvm-ar
```

x86_64カーネルでred zoneを無効化するABIなら、対象固有フラグを明示します。これは汎用テンプレートの既定値ではありません。

```sh
make -f templates/toolchains/freestanding-c11.mk \
  PROJECT_ROOT=. CC=clang AR=llvm-ar \
  FREESTANDING_ARCH_CFLAGS='-mno-red-zone'
```

| 変数 | 用途 | 例 |
|---|---|---|
| `CC` | ターゲットCコンパイラ | `clang --target=x86_64-unknown-none` |
| `AR` | ターゲットアーカイバ | `llvm-ar` |
| `FREESTANDING_ARCH_CFLAGS` | CPU・ABI固有フラグ | `-mno-red-zone` |
| `CFLAGS` | 既定のfreestandingフラグを完全に置換 | 最適化・sanitizer無効化など |
| `CPPFLAGS` | ヘッダー探索と設定マクロ | `-I... -DPYTHON_CODE_TO_C_NO_STDLIB` |
| `BUILD` | 出力ディレクトリ | `out/kernel/p2c` |

通常の`make freestanding`も同じ`freestanding-c11.mk`を呼び出します。旧`freestanding-gcc.mk`はx86_64の既存利用者向けの互換入口として残されていますが、新規移植では使用しません。

## 4. 必須プラットフォームアダプタ

`P2C_Platform`はアロケータ、再確保、解放、バイト列出力、ミリ秒時計だけを要求します。`write`の`stream`は通常出力が`1`、診断出力が`2`です。非同期文脈から呼ぶ可能性があるOSでは、アロケータと出力の再入性・ロック規約をOS側で定義してください。

```c
#include "python_code_to_c_single.h"

typedef struct {
    void *heap;
    unsigned long long boot_millis;
} KernelP2CContext;

static void *kernel_alloc(size_t size, void *user) {
    KernelP2CContext *ctx = (KernelP2CContext *)user;
    return kernel_heap_alloc(ctx->heap, size);
}

static void *kernel_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    KernelP2CContext *ctx = (KernelP2CContext *)user;
    return kernel_heap_realloc(ctx->heap, ptr, old_size, new_size);
}

static void kernel_free(void *ptr, size_t size, void *user) {
    KernelP2CContext *ctx = (KernelP2CContext *)user;
    kernel_heap_free(ctx->heap, ptr, size);
}

static void kernel_write(int stream, const char *data, size_t len, void *user) {
    (void)stream;
    (void)user;
    serial_write_bytes(data, len);
}

static uint64_t kernel_clock_ms(void *user) {
    KernelP2CContext *ctx = (KernelP2CContext *)user;
    return kernel_monotonic_millis() - ctx->boot_millis;
}

static P2C_Platform kernel_platform;

void p2c_kernel_init(KernelP2CContext *ctx) {
    kernel_platform.alloc = kernel_alloc;
    kernel_platform.realloc = kernel_realloc;
    kernel_platform.free = kernel_free;
    kernel_platform.write = kernel_write;
    kernel_platform.clock_ms = kernel_clock_ms;
    kernel_platform.user = ctx;
    p2c_platform_set(&kernel_platform);
}
```

この例の`kernel_heap_*`、`serial_write_bytes`、`kernel_monotonic_millis`はOS側の関数です。アダプタの関数ポインタ、`user`、アダプタ構造体は、`p2c_platform_set()`後も有効な静的または長寿命の領域に置いてください。

### 4.1 単一ヒープの契約（変換器コアとランタイムの共有）

`p2c_platform_set()`で登録した`alloc`/`realloc`/`free`は、**生成Cのランタイム（GC object、list/dict/set/tuple、文字列）と、変換器コア（字句解析・AST・文字列ビルダ・コード生成バッファ）の両方**が使います。Alpha1.0では共通層の共有ヒープ抽象（`p2c_heap_alloc`/`p2c_heap_calloc`/`p2c_heap_realloc`/`p2c_heap_free`）へ一本化しており、変換器コアの内部確保も同じヒープを通ります（例外は`python_to_c()`の戻り値である生成C文字列で、これは`malloc`/`free`契約のままなので呼び出し側が`free()`で解放します）。

| 事項 | 契約 |
|---|---|
| プラットフォーム確保ブロック | 16バイトのヘッダ（ペイロード長＋マジック値）を前置して`alloc`へ渡す。`free`/`realloc`にはそのヘッダを含むサイズを渡す |
| `realloc(void *ptr, size_t old_size, size_t new_size, void *user)` | `old_size`はヘッダ込みの実サイズ。伸長できない場合は新規確保＋コピーでもよい（OS側の実装方針に委ねる） |
| `free(void *ptr, size_t size, void *user)` | サイズを利用してリーク／再利用を管理できる。サイズを無視する実装でも動作する |
| 同一領域の二重使用 | **禁止**。プラットフォームアロケータと、`p2c_runtime_init(heap, size)`に渡すヒープ（NO_STDLIBスタブの線形ヒープ）を同じ領域に向けると、2つの独立したバンプポインタが同じアドレスを配り、ヒープが壊れる。どちらか一方だけを使う |
| 停止順序 | `p2c_runtime_shutdown()`／`p2c_embed_stop()`でランタイムを止めてから`p2c_platform_set(NULL)`／`p2c_platform_set_allocator(NULL,…)`で解除する。解除後に残ったプラットフォームブロックはブロック先頭から`free`へ委譲されるが、kmalloc等のlibc非互換アロケータでは解除前に全解放しておくこと |
| `p2c_heap_uses_platform()` | 現在のヒープ実体がプラットフォーム提供かどうかを診断目的で返す |
| `p2c_heap_usable()` | 共有ヒープが実際に確保できるか。プラットフォーム設定済み／`NO_LIBC_STUBS`／ホストは`true`。NO_STDLIB既定で`p2c_runtime_init()`前だけ`false` |

#### 4.1.1 アロケータの注入（`P2C_Platform` / `P2C_Allocator`）

`P2C_Platform`一式（`write`/`clock_ms`を含む）を用意しなくても、アロケータだけを1回で注入できます。

```c
/* カーネルの kmalloc/krealloc/kfree を唯一のヒープにする。
 * これ1回で、ランタイムと変換器コアの内部確保がすべてこのヒープを通る。 */
p2c_platform_set_allocator(kernel_alloc, kernel_realloc, kernel_free, kernel_ctx);
...
p2c_platform_set_allocator(NULL, NULL, NULL, NULL);  /* 解除 */

/* P2C_Allocator を明示注入する場合（arena/slabを変換器の既定にする）。 */
p2c_set_default_allocator(p2c_linear_allocator(arena, arena_size));
p2c_set_default_allocator(NULL);                     /* 組み込み既定へ戻す */
```

解除は注入した側に応じて`p2c_platform_set_allocator(NULL, NULL, NULL, NULL)`または`p2c_set_default_allocator(NULL)`を呼びます（専用のリセット関数はありません）。

#### 4.1.2 変換器コアの静的フォールバック（既定64KiB）

変換器コアは、注入アロケータ／共有ヒープのどちらも確保に失敗する場合に限り、`static char fallback_buf[P2C_COMPILER_FALLBACK_HEAP_SIZE]`（既定65536）をリニアアロケータとして使います（呼び出しごとにリセットするため、常駐サービスでも2回目以降の変換が`P2C_ERR_INTERNAL`になっていた旧挙動は解消済み）。

| 事項 | 内容 |
|---|---|
| 判定 | 変換開始時にアロケータへ8バイトのプローブを1回行う。成功すれば注入アロケータ／共有ヒープをそのまま使う（`p2c_core_static_allocator_active() == false`） |
| サイズ変更 | `-DP2C_COMPILER_FALLBACK_HEAP_SIZE=1048576`（1MiB）など |
| 無効化 | `-DP2C_COMPILER_FALLBACK_HEAP_SIZE=0`。この場合アロケータ未注入の構成は`P2C_ERR_NOMEM`と注入方法を説明するメッセージを返す |
| 確認 | `p2c_core_static_allocator_active()`が`false`なら、変換器はOSのヒープだけを使っている（常駐サービスでの推奨状態） |

> 自力でメモリを確保できる構成（カーネルの`malloc`実装、`P2C_EMBED_PROVIDE_LIBC_HEAP`、プラットフォームアロケータ）では、静的フォールバックは使われません。組込みでの推奨構成は「`p2c_platform_set_allocator()`でカーネルのヒープを注入し、`P2C_COMPILER_FALLBACK_HEAP_SIZE=0`で静的バッファを完全に排除する」です。

プラットフォームを登録しない（既定プラットフォームのまま）場合、共有ヒープはlibcの`malloc`系（NO_STDLIBでは`src/runtime`同梱の線形ヒープ、またはカーネル提供`malloc`）へ委譲されます。既定プラットフォームは「アロケータ未設定」として扱われるため、ホスト上での従来挙動（直接`malloc`）は変わりません。

### 4.2 `setjmp`/`longjmp`の選択肢

例外機構（`raise`/`except`、generatorの`StopIteration`、未処理例外の診断）は非局所脱出に`setjmp`/`longjmp`を使います。Alpha1.0は次の順で実装を選びます。

| 構成 | 実装 | 備考 |
|---|---|---|
| Hosted（`PYTHON_CODE_TO_C_NO_STDLIB`なし） | libc `<setjmp.h>` | 従来どおり |
| freestanding 既定 | コンパイラ組み込み（`__builtin_setjmp`/`__builtin_longjmp`） | `jmp_buf`は`void *buf[16]`。`__builtin_longjmp`は常に擬似値1を返すため、`longjmp(env, val)`の`val`は1に固定される（ランタイム／生成コードは`== 0`判定のみを使う） |
| `PYTHON_CODE_TO_C_NO_LIBC_STUBS` | カーネル提供の`setjmp`/`longjmp` | `examples/embed/x86_64_setjmp.c`がx86-64参考実装 |
| `PYTHON_CODE_TO_C_NO_COMPILER_SETJMP` | カーネル提供（組み込みマクロを無効化） | 独自実装をリンクしたい場合 |
| `P2C_SETJMP`/`P2C_LONGJMP`の定義 | 上記すべてを上書きする差し替え点 | `-DP2C_SETJMP(env)=my_setjmp(env)`／`-DP2C_LONGJMP(env,val)=my_longjmp((env),(val))` |

> Alpha1.0以前の組込みスタブは`longjmp`が無限ループであり、`raise`した瞬間にタスクが停止していました。現在はコンパイラ組み込みにより**libcなしでも例外が実際に機能します**。コンパイラ組み込みもカーネル実装も無い場合は、黙ってハングせず`p2c_platform_abort()`で診断します。

#### 4.2.1 `P2C_SETJMP`/`P2C_LONGJMP`による完全差し替え

ランタイム本体と生成Cの例外機構（`raise`/`except`、`with`、`for`、generator、`finally`、`async for`）は、**`P2C_SETJMP`/`P2C_LONGJMP`の2つのマクロだけ**を通ります（`src/runtime`の内部も、コード生成が出力するコードも同じ）。したがってカーネル側はこの2つを定義するだけで、非局所脱出を自前実装（コンテキストスイッチ、syscall、割込み安全な実装）へ置き換えられます。

```c
/* カーネルの実装を使う（マクロとして定義すること）。 */
#define P2C_SETJMP(env)        kernel_setjmp((env))
#define P2C_LONGJMP(env, val)  kernel_longjmp((env), (val))
```

| 契約 | 内容 |
|---|---|
| バッファ | `jmp_buf`（既定は`typedef struct { void *buf[16]; } jmp_buf[1]`。libcの`jmp_buf`を渡してもよい） |
| 戻り値 | `P2C_SETJMP`は最初の戻りで`0`、`P2C_LONGJMP`で戻ったときは非`0`（ランタイムは`== 0`判定のみを使う） |
| マクロ必須 | 組み込み`setjmp`は「呼び出し元の関数が二度戻る」前提でフレームを保存するため、関数でラップすると未定義動作になる。既定実装がマクロなのはこの理由 |
| 回帰 | `make test-setjmp-hook`が、フックを差し替えた状態でランタイムと生成Cの両方がフックを通ることを検証する（`tests/setjmp_hook_override.h`が実例） |

### 4.3 GCスタック境界の契約

`p2c_gc_collect()`は保守的スタックスキャンでスタック上のローカル変数をルートとして扱います。走査対象は**「現在の関数フレームから、宣言されたスタック上端まで」**だけです（未使用のスタック領域は走査しません。8MiBスタック全体を毎回走査すると、GCしきい値を小さくしたときに収集回数×スタック長で劣化するため）。

| API | 意味 |
|---|---|
| `p2c_gc_init(stack_hint)` | Linuxでは`pthread_getattr_np()`から実境界を取得。それ以外の環境では`stack_hint`（呼び出し元フレーム内のローカル変数アドレス）をスタック上端として使い、`P2C_GC_ENTER_MAIN()`／`P2C_GC_ENTER_TASK()`経由で渡す。`NULL`のときだけ境界不明として自動GCを安全側停止 |
| `p2c_gc_set_stack_bounds(lo, hi)` | タスクスタック区間を明示。順序は不問。大きな区間（例: 8MiB）を宣言しても走査コストは使用量に比例する |
| `p2c_gc_stack_scan_available()` | スキャン可能か。false の間は収集を行わない（生存オブジェクトを解放しないための安全側停止） |
| `p2c_gc_last_stack_words()` | 直近の収集で走査したスタック語数（診断用）。走査範囲がスタック全体へ広がっていないことの確認に使える |

カーネルが直接自前のタスクスタックを管理する場合（スレッドごとにスタックを切り替える等）は、タスク切り替え時に`p2c_gc_set_stack_bounds()`を呼び直してください。生成Cの`main()`は`P2C_GC_ENTER_MAIN()`を使うため、追加設定なしでローカル変数が保護されます。

## 5. 初期化・変換・生成C実行

コンパイラを使うカーネルタスクまたはサービスの最上位フレームで、アダプタを登録してからGCのスタック基点を設定します。`P2C_GC_ENTER_MAIN()`は呼出し元フレームをGC走査の基点にするため、そのタスクが終了しない、またはGC利用中に基点フレームを失わない場所で一度だけ実行します。

```c
void kernel_py2c_service(KernelP2CContext *ctx) {
    p2c_kernel_init(ctx);
    P2C_GC_ENTER_MAIN();

    const char *source = "values = [1, 2, 3]\nprint(sum(values))\n";
    char *generated = NULL;
    P2C_Result result = python_to_c(source, NULL, &generated);
    if (result != P2C_OK) {
        p2c_platform_write(p2c_last_error_details());
        return;
    }

    kernel_store_generated_c(generated);
    kernel_heap_free(ctx->heap, generated, 0);
}
```

生成CをOS上で実行する場合は、同じ`PYTHON_CODE_TO_C_NO_STDLIB`設定と同じプラットフォームアダプタで、生成物とランタイムをリンクします。GNU statement expressionを使用する内包表記・lambdaを避ける移植性優先の生成では、変換オプションの`strict_c11`を有効にするか、CLIの`--c11`を使います。

### 5.1 構造的pattern matchingのOS境界

sequence、mapping、star、as、OR、guard、mapping `**rest`、bare/keyword属性class patternを使う生成Cは、同梱ランタイムの`p2c_match_sequence()`、`p2c_match_sequence_item()`、`p2c_match_sequence_rest()`、`p2c_match_mapping_has()`、`p2c_match_mapping_get()`、`p2c_match_mapping_rest()`、`p2c_isinstance_of_class()`、`p2c_hasattr()`、`p2c_getattr()`だけを使用します。これらは新しいOSコールバックを要求しません。`**rest`は入力dictから要求済みキーを除いて新しいdictを確保し、class patternは既存instance属性を読むだけなので、OS側は通常のPythonコンテナ・instanceと同じアロケータ・GC契約を提供するだけで足ります。属性が存在しない場合は例外を送出せずcase失敗となるため、OS固有の例外抑止フックも不要です。生成中のキー値と残余dictはいずれもランタイムオブジェクトであり、生成Cのフレームが生存する間は通常のGC走査経路で到達可能です。

`match`のsubjectは一度だけ評価され、patternが成功してからguardが評価されます。したがって、OSアダプタに副作用の特別な抑制を追加する必要はありません。自作OS向けに新しいpattern機能を使う場合も、`make CC=clang test-single-header-freestanding`とターゲット`CC`での`freestanding-c11.mk`を先に通し、対象ABIにおけるC11コンパイルを確認してください。

## 6. GCと非同期所有権

GCは到達可能なコンテナ・インスタンス・モジュール・イテレータ・例外causeを走査します。CまたはOSの非同期構造が`P2C_Object*`を保持するときは、**ポインタ値ではなくポインタを置くスロット**をルートとして登録します。

```c
static P2C_Object *pending_message;

void queue_python_object(P2C_Object *obj) {
    pending_message = obj;
    p2c_gc_register_root(&pending_message);
}

void release_python_object(void) {
    p2c_gc_unregister_root(&pending_message);
    pending_message = NULL;
}
```

| 状況 | 必要な操作 |
|---|---|
| Cの長寿命グローバルが`P2C_Object*`を保持 | `p2c_gc_register_root(&slot)` |
| キュー・ドライバ・割込み後処理がオブジェクトを保持 | 保持開始前に登録し、所有権解放後に解除 |
| OS再初期化・テスト環境のリセット | `p2c_gc_reset_roots()`をOSのライフサイクルに合わせて呼ぶ |
| 一時ローカルだけがオブジェクトを保持 | `P2C_GC_ENTER_MAIN()`で設定した長寿命フレームの走査規約を守る |

GCルートの重複登録は無視されますが、解除漏れは不要なオブジェクトの生存につながります。解除後にスロットを`NULL`へ戻す規約を採用すると、OS側の所有権監査が容易になります。

### 6.1 runtime epochとshutdown

`p2c_runtime_shutdown()`は、runtime所有objectを解放し、module registry、class registry、協調async queue、明示root table、pygame互換moduleの静的class slotをresetします。終了後の`P2C_Object*`はすべて無効であり、OS側はキュー、割込み後処理、デバイス状態、GUI状態に残る参照を使用してはいけません。次の`p2c_runtime_init()`は新しいruntime epochを開始し、以前のobjectを再利用しません。

| ライフサイクル段階 | OS側の必須操作 | Alpha1.0の保証 |
|---|---|---|
| 起動前 | platform adapterと長寿命root slotを用意 | runtime objectは未生成 |
| 初期化後 | `p2c_runtime_init()`、`P2C_GC_ENTER_MAIN()`、必要なroot登録を順に実行 | registryとasync queueは新しいepochとして空から開始 |
| 実行中 | OS管理slotをroot登録またはpinし、不要時に解除 | container/closure/generator/exception cause/runtime registryを走査 |
| 終了前 | OSのqueueと割込み処理を停止し、外部slotを使い終える | shutdown前にruntime APIを安全に呼出せる |
| 終了 | `p2c_runtime_shutdown()`後に全外部slotを`NULL`へ更新 | runtime内registry、root、queue、GC統計をresetし、旧objectを無効化 |

OSのallocatorが失敗する場合、runtime constructorは後段確保に失敗した新規objectをGC追跡リストからrollbackする。これはGC内部のダングリング参照を防ぐための契約であり、OS側は依然として`alloc`/`realloc`失敗を安全に返し、枯渇後に古いpointerを再利用しない必要がある。

## 7. 単一ヘッダーでの組込み

単一翻訳単位で組み込む場合は、次の3マクロを使用します。`P2C_SINGLE_HEADER_IMPLEMENTATION`はプログラム全体で一度だけ定義します。

```c
#define PYTHON_CODE_TO_C_NO_STDLIB
#define P2C_SINGLE_HEADER_NO_HOSTED
#define P2C_SINGLE_HEADER_IMPLEMENTATION
#include "python_code_to_c_single.h"
```

ヘッダーが正本と一致することは`make single-header`で保証します。ホスト上では、GCCなしでも次のコマンドでClangだけを使う厳格C11自己完結検証ができます。

```sh
make CC=clang test-single-header-c11
```

このテストはヘッダー以外のPy2c `.c`をリンクせず、`-std=c11 -pedantic-errors`と警告即エラーでコンパイルします。実際の自作OSでは、ターゲット固有のリンカスクリプト、スタートアップ、`setjmp`/`longjmp`実装、必要なコンパイラランタイムをOSのビルドへ追加してください。

## 8. GUIバックエンドの接続

`P2C_GuiContext`は固定長の描画コマンド配列とテキストアリーナへ記録します。OS側は`P2C_GuiBackend`を使い、順番に来るコマンドをフレームバッファ、GPU、独自コンポジタ、またはシリアル描画器へ渡します。

| コマンド | OS側の処理 |
|---|---|
| `P2C_GUI_CLEAR` | RGBA色で描画面を初期化 |
| `P2C_GUI_FILL_RECT` | クリッピング後に塗りつぶし矩形を描画 |
| `P2C_GUI_STROKE_RECT` | 線幅を考慮して枠線を描画 |
| `P2C_GUI_LINE` | 座標系・クリッピング規約に従い線分を描画 |
| `P2C_GUI_TEXT` | UTF-8/ASCII方針を明示してフォント描画器へ渡す |

コマンド数またはテキストアリーナが上限を超えた場合、Alpha1.0は安全にドロップを記録します。OS側はフレーム終了時にドロップ数を監視し、バッファ容量の増加またはUI簡略化を判断してください。

## 9. 検証と障害切り分け

移植済み成果物は、最小の変換・生成C・例外・GC・GUIを分けて検証します。最初からユーザー入力やファイルシステムを有効にしないことが重要です。

| 症状 | 先に確認すること | 典型的な原因 |
|---|---|---|
| 最初の変換で停止 | `p2c_platform_set()`の呼出し順 | 未設定アロケータ、短命な`P2C_Platform`構造体 |
| 出力が出ない | `write`コールバックと`stream`の処理 | シリアル初期化前の出力、stderr破棄 |
| 例外でハングする（旧バージョンの挙動） | `PYTHON_CODE_TO_C_NO_LIBC_STUBS`／`PYTHON_CODE_TO_C_NO_COMPILER_SETJMP`の有無 | 旧スタブの`longjmp`は無限ループだった。現在はコンパイラ組み込み（§4.2）で動作し、実装が無い場合は`p2c_platform_abort()`で停止する |
| ヒープが壊れる／確保した覚えのない領域が書き換わる | ヒープを二重に有効化していないか | プラットフォームアロケータと`p2c_runtime_init(heap, size)`に同じ領域を渡している（§4.1）。どちらか一方だけを使う |
| スタック上のローカル変数がGCで回収される | `p2c_gc_stack_scan_available()` | `p2c_gc_init(NULL)`のまま／境界未宣言。`P2C_GC_ENTER_MAIN()`または`p2c_gc_set_stack_bounds()`を使う（§4.3） |
| しきい値を小さくするとGCが極端に遅い | `p2c_gc_last_stack_words()` | 走査範囲がスタック全体へ広がっていないか（本来は使用中の範囲のみ）。境界を8MiB等で宣言しても走査コストは使用量に比例する |
| 2回目の変換が`P2C_ERR_INTERNAL`（"fallback allocator"） | 既定アロケータの有無と再入 | 静的フォールバック使用中に、変換中から変換を呼ぶ再入だけが拒否される。通常の連続呼び出しは呼び出しごとにリセットされる（§4.1.2） |
| 変換が`P2C_ERR_NOMEM`（"no allocator available"） | `p2c_heap_usable()`と注入の有無 | `P2C_COMPILER_FALLBACK_HEAP_SIZE=0`で静的フォールバックを無効化した構成では、`p2c_platform_set_allocator()`／`p2c_set_default_allocator()`による注入が必須（§4.1.1） |
| 変換器が自分の静的バッファを使っている気がする | `p2c_core_static_allocator_active()` | `true`なら注入アロケータのプローブ（8バイト確保）が失敗している。`p2c_platform_set_allocator()`の`alloc`が`p2c_runtime_init()`前に`NULL`を返していないか確認する |
| プラットフォーム解除後にヒープが壊れる | 停止順序 | `p2c_runtime_shutdown()`／`p2c_embed_stop()`より先に解除した。順序は「ランタイム停止 → プラットフォーム解除」（§4.1） |
| GC後に値が壊れる | 非同期所有者のルート登録 | `P2C_Object*`スロットの未登録・早期解除 |
| 再起動後にクラッシュ | runtime epoch境界と外部slotの初期化 | shutdown後の`P2C_Object*`再利用、OS queueの未停止 |
| ヒープ枯渇後に不安定 | allocator失敗経路 | OS allocatorが失敗を返さない、または外部所有pointerを再利用 |
| クロスコンパイル失敗 | `FREESTANDING_ARCH_CFLAGS`とABI | x86固有フラグを別アーキテクチャへ流用 |
| 例外時に復帰しない | `setjmp`/`longjmp`実装とタスク境界 | 割込み境界をまたぐ例外フレーム、壊れたスタック |
| GUIが欠ける | ドロップ数とバックエンドのクリッピング | 固定バッファ不足、座標系の不一致 |

> freestandingビルドは「標準Cライブラリを必要としない」構成ですが、リンクまで含めて完全自立という意味ではありません。OS側で **スタートアップ、リンカスクリプト、コンパイラランタイム（必要なら`__udivdi3`等）、`sqrt`/`sin`/`cos`/`pow`/`floor`/`fmod`等の数学関数、必要な文字列／数値変換関数** を用意してください（`tests/test_baremetal_runtime.c`・`templates/hobby_os/` が最小構成の例です）。触らない機能の数学関数はリンク時に落とせます。

> **サニタイザと保守的スタックスキャン**: ASanの`detect_stack_use_after_return=1`（GCC libasanの既定）は
> ローカル変数をヒープ上の「fake stack」に置くため、保守的スタックスキャンからは見えません
> （実スタック区間外のアドレスになる）。生成CをASanで実行して検証する場合は
> `ASAN_OPTIONS=detect_stack_use_after_return=0` を付けてください。通常ビルドおよび
> 自作OS上の実行では影響しません。

最終的なホスト品質基準は`make CC=clang full-build && make CC=clang test`および`make CC=gcc full-build && make CC=gcc test`です。自作OS固有の最終受入では、それに加えてターゲットコンパイラで`freestanding-c11.mk`を実行し、OSアダプタをリンクした最小変換・最小生成C実行・GCルート・GUIバックエンドの4種類を確認してください。

## 10. 追加済みベアメタル実行ハーネス

Alpha1.0には、移植手順そのものを検証可能にする最小ベアメタル資産が含まれます。`examples/baremetal/python_code_to_c_baremetal.c`は、標準ライブラリを使わない静的バンプヒープ、再確保、UART相当のバイト列出力、単調tick、platform互換フックを実装します。`p2c_baremetal_uart_write()`だけをターゲットOSのUART、カーネルログ、または画面コンソールへ接続してください。テスト時には同じ出力が固定バッファにも記録されるため、UARTドライバが未接続でも出力契約を検証できます。

| 資産 | 役割 | 受入条件 |
|---|---|---|
| `include/baremetal/python_code_to_c_baremetal.h` | 静的ボード、プラットフォーム、起動の公開契約 | OS側の入口・ヒープ・コンソール領域を静的に保持できる |
| `examples/baremetal/python_code_to_c_baremetal.c` | freestandingアダプタと`p2c_baremetal_run()` | `P2C_Platform`登録、`p2c_runtime_init()`、GC基点、終了処理を順に実行する |
| `examples/baremetal/baremetal_start.c` | `p2c_baremetal_entry()` | ブート後に呼び出せる最小C入口を提供する |
| `examples/baremetal/baremetal_hello_program.c` | 手書き状態機械の実行例 | generatorの`7, 9`とawait結果`22`をplatform出力へ書く |
| `examples/baremetal/baremetal_hello.py` | 実際の変換対象 | generator反復と`asyncio.run`を含む生成Cのfreestandingコンパイル対象 |
| `tests/test_baremetal_runtime.c` | 実行ハーネス | 静的ヒープ上のGC、platform write、tick、協調await結果`22`を検証する |

次のターゲットはホスト上でターゲット向け契約を実行またはコンパイルします。前者はホストのプロセスを使う**検証ハーネス**であり、実機ブートの代替ではありません。最終的なカーネルリンク、リンカスクリプト、初期スタック、割込み、UARTデバイス初期化は各OSの責務です。

```sh
make CC=clang test-baremetal-runtime
make CC=clang test-baremetal-build
make CC=clang test-baremetal-generated
```

`test-baremetal-runtime`は`PYTHON_CODE_TO_C_NO_STDLIB`、`-ffreestanding`、`-fno-builtin`、警告即エラーで直接実行します。`test-baremetal-build`は起動・アダプタ・手書き状態機械を`libpython-code-to-c-baremetal-example.a`へアーカイブし、`test-baremetal-generated`は`baremetal_hello.py`を実際に変換してから同じC11 freestanding条件でオブジェクト化します。

## 11. 協調ジェネレータ・asyncのOS組込み規約

`yield [value]`または`yield from iterable`を持つ関数は`OBJ_ITERATOR`の状態機械として生成され、再開位置、局所値、委譲中iteratorはジェネレータ自身のGC到達可能領域に保持されます。通常の`for`は`iter()`、`next()`、`StopIteration`を使う汎用経路で生成されるため、生成器をそのまま反復できます。`async def`はコルーチン生成器となり、`await coroutine_call()`は子コルーチン完了まで協調的に再スケジュールされます。`asyncio.run(coroutine)`は固定長FIFO実行器へ変換されます。

> この実装はI/O多重化やプリエンプションを行わない協調実行モデルです。OSの割込みハンドラ、別CPU、別スレッドから同じジェネレータまたはスケジューラを同時に操作してはいけません。

OSキュー、割込み後処理、デバイス待機表へ`P2C_Object*`を保存する場合は、保存先スロットを`p2c_gc_register_root()`で登録し、解放時に登録解除します。await実行中のコルーチン内部参照はランタイムが走査しますが、OSが新たに所有する参照は自動登録されません。

| 対応範囲 | 生成・実行規約 |
|---|---|
| generator | `yield [value]`、`yield from iterable`、`next()`、for反復、収集型組込み反復、停止時`StopIteration` |
| async | `async def`、単純な`await coroutine_call()`、`asyncio.run()`、単純name targetの`async for`、単一context managerの`async with` |
| async with | context、`__aenter__`結果、body例外をgenerator localsへ保存し、`__aexit__`待機後にtruthy値で抑止、falsy値で元例外を再送出 |
| 非対応 | `yield from`のsend/throw/close・委譲戻り値、async generator、async comprehension、async withの複数context/複合`as` target/複雑な停止点、Task、キャンセル、I/O待機 |
| 停止点制約 | 順次本体を対象とし、awaitは独立した式文または単純名への代入の右辺に置く。async with bodyの複数停止点や複雑な`try/finally`横断は明示的互換境界 |

## 12. Python 3.13型構文の型消去規約

Python 3.13の`type Alias[params] = expression`、型パラメータ既定値、TypeVarTuple、ParamSpecは、Alpha1.0では**コンパイル時の型専用情報**として受理して消去します。したがってターゲットOSは`typing`、`TypeAliasType`、frame proxy、`exec`/`eval`を提供する必要がありません。aliasを実行時の値として使用するコードは移植対象外です。型構文を含む変換器・生成CのC11契約は、ホストで`make test-py313-syntax`により先に確認してください。詳細は[`docs/PYTHON_3_13_COMPATIBILITY_ALPHA1.0.md`](docs/PYTHON_3_13_COMPATIBILITY_ALPHA1.0.md)を参照します。

## 13. ターゲットOSへの最終リンク

`templates/toolchains/baremetal-example.mk`は、ターゲットの`CC`と`AR`だけを受け取るC11オブジェクト・アーカイブ生成テンプレートです。OSのトップレベルMakefileから次のように呼び出し、生成されたアーカイブをカーネルの通常のリンク集合へ加えます。

```sh
make -f templates/toolchains/baremetal-example.mk \
  PROJECT_ROOT=. CC='clang --target=x86_64-unknown-none' AR=llvm-ar \
  BAREMETAL_ARCH_CFLAGS='-mno-red-zone'
```

このテンプレートは最終ELFやブートイメージを作りません。OS側で`p2c_baremetal_entry()`をカーネル初期化後に呼び出し、`p2c_baremetal_uart_write()`を実デバイスへ置換または接続し、ターゲットの`setjmp`/`longjmp`と必要なコンパイラランタイムをリンクしてください。CPU ABI、リンカスクリプト、割込み禁止区間、メモリマップはテンプレートの外部契約です。

## 14. 今回の品質基準

今回の変更では、ClangとGCCでクリーンな`full-build`と`test`を完了し、C01–C322のCPython差分、sequence・mapping・as・star・OR・guard・mapping `**rest`・bare/keyword属性class pattern、Python 3.13型パラメータ・`type`文の厳格C11型消去検証、位置専用引数、`yield from`、`raise from`、UTF-8 `ord`/`chr`、`bin`/`oct`/`hex`、開始値付き`sum`、既定3 seed×48操作のコンテナ差分ファジング、8 seed×64操作の拡張ファジング、単一ヘッダーのHosted・厳格C11・freestanding検証、ベアメタル実行ハーネス、生成済みベアメタルCのfreestandingコンパイルを確認しています。さらに、変更したparser、AST、codegen、runtimeに対するClang静的解析は診断0件であり、class patternのtoken寿命不具合はAddressSanitizer/UndefinedBehaviorSanitizerで検出・修正済みです。
