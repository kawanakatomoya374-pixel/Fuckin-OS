# Python Code to C Alpha0.6 — 自作OS組み込みガイド

## 1. この文書の目的

自作OS（HobbyOS）へ **Python Code to C** を組み込むための、現行APIの契約と手順を
一箇所にまとめます。対象は次の2つです。

| 組み込むもの | 目的 | 追加するカーネルコード |
|---|---|---|
| **変換器コア** | OS内でPythonソースをCへ変換する（オンデバイス変換） | なし（APIを呼ぶだけ） |
| **実行時 + 生成モジュール** | 変換済みモジュールをカーネルタスクとして実行する | `templates/hobby_os/embed/hobby_os_embed.c` の1ファイル |

組み込みは次の3層で構成されています。カーネルが触れるのは表層だけです。

```
[カーネル]  UART / 時計 / スタック区間 / アロケータ / setjmp
     │  P2C_EmbedConfig
     ▼
[p2c_embed] 組込みヒープ・出力シンク・OOM/panic・ライフサイクル・互換フック
     │  P2C_Platform / runtime API
     ▼
[ランタイム] GC・オブジェクト・例外・コンテナ・生成コード
```

## 2. 生成側: `--embed-entry`

```sh
python-code-to-c kernel_module.py --embed-entry kernel_python_program -o kernel_module.c
```

- 生成されるのは `P2C_Object *kernel_python_program(void)` だけで、`int main()` は
  生成されません（カーネルには `main` が無いため）。
- ヒープ・GC・出力・スタックはカーネルが所有します。生成エントリは
  モジュールグローバルのGCルート登録と本体実行だけを行います。
- 名前はCの識別子である必要があり、不正な場合は変換時に診断されます
  （`--embed-entry` は C のソースへ埋め込まれるため、検証を省略しません）。
- 通常の `main` 生成が必要な場合は従来どおり `--embed-entry` を付けません。

## 3. カーネル側: `p2c_embed`

```c
#include "platform/python_code_to_c_embed.h"

extern P2C_Object *kernel_python_program(void);

static unsigned char heap_storage[64 * 1024];
static char console[512];
static P2C_EmbedHeap heap;

int kernel_task(void *task_stack_lo, void *task_stack_hi) {
    /* malloc を置き換える場合は、他のどの確保より先に既定ヒープを有効化する */
    if (p2c_embed_heap_init(&heap, heap_storage, sizeof(heap_storage)) != 0) return -1;
    p2c_embed_heap_set_default(&heap);

    P2C_EmbedConfig cfg;
    p2c_embed_config_init(&cfg);
    p2c_embed_config_use_uart(&cfg, my_uart_write, NULL);          /* 出力シンク */
    p2c_embed_config_use_console(&cfg, console, sizeof(console));  /* 出力の控え */
    p2c_embed_config_use_heap(&cfg, &heap, heap_storage, sizeof(heap_storage));
    p2c_embed_config_use_stack(&cfg, task_stack_lo, task_stack_hi);/* GCの走査範囲 */
    cfg.clock_ms = my_clock_ms;
    cfg.panic = my_panic;                                          /* 復帰しない */

    if (p2c_embed_start(&cfg) != 0) return -1;   /* platform登録 + runtime/GC初期化 + OOM方針 */
    if (!p2c_embed_run_program(kernel_python_program)) { p2c_embed_stop(); return -2; }

    P2C_EmbedStats stats;
    p2c_embed_stats(&stats);         /* heap_used/peak, gc_collections, gc_last_freed ... */
    p2c_embed_stop();                /* 再startで再実行できる（タスク再起動） */
    return 0;
}
```

`p2c_embed_start()` は次を一度に済ませます。

1. 委譲シンク/時計/入力から `P2C_Platform` を組み立てて `p2c_platform_set()`
2. `p2c_runtime_init(heap, heap_size)`
3. `p2c_gc_init()` と `p2c_gc_set_stack_bounds()`
4. OOMハンドラの登録（例外フレームがあれば `MemoryError`、無ければ panic）


## 4. カーネルが満たすべき契約

| 契約 | 必須/任意 | 内容 |
|---|---|---|
| 出力シンク | 必須 | `void (*)(void *user, const char *data, size_t len)`。`print`・例外診断・panicが全てここへ流れる |
| スタック区間 | 強く推奨 | `p2c_embed_config_use_stack()`。未指定でも生成Cの `P2C_GC_ENTER_MAIN()` が「スタック上端のヒント」を渡すためスキャンは有効になる（走査は使用中の範囲だけ）。明示するとカーネル管理のタスクスタックを正確に反映できる |
| `malloc/free/realloc/calloc` | `PYTHON_CODE_TO_C_NO_LIBC_STUBS` 定義時 | カーネルのアロケータ。`p2c_embed_heap_*` をそのまま使うと GC の解放が再利用に繋がる |
| `malloc/free/realloc/calloc`（スタブ既定） | 不要 | `PYTHON_CODE_TO_C_NO_LIBC_STUBS` を定義しない場合、ランタイムが線形ヒープのスタブを提供する。このとき `p2c_platform_set_allocator()` を設定していれば、スタブの `malloc` 系はカーネルのアロケータへ委譲され、変換器コア・ランタイム・スタブのすべてが1つのヒープを共有する（`p2c_runtime_init()` に同じ領域を渡す必要はない。回帰: `make test-heap-unification`） |
| `setjmp/longjmp` | `PYTHON_CODE_TO_C_NO_LIBC_STUBS` 定義時のみ | 例外処理に必須。`jmp_buf` は `struct { void *buf[16]; }[1]`（`runtime.h`）。x86-64 の参考実装が `examples/embed/x86_64_setjmp.c`。既定の freestanding 構成ではコンパイラ組み込み（`__builtin_setjmp`/`__builtin_longjmp`）を使うため、カーネル実装は不要（`PYTHON_CODE_TO_C_NO_COMPILER_SETJMP` で無効化） |
| アロケータ注入 | 任意 | `p2c_platform_set_allocator()` でカーネルの kmalloc/kfree を唯一のヒープにできる（`write`/`clock_ms` は既定実装を継承）。`p2c_set_default_allocator()` で `P2C_Allocator` を変換器コアの既定にできる。`p2c_core_static_allocator_active()` が `false` ならOSのヒープだけを使っている |
| setjmp差し替え | 任意 | `P2C_SETJMP`/`P2C_LONGJMP` を定義すると、ランタイム本体と生成Cの例外機構がその実装だけを使う（例: `-DP2C_SETJMP(env)=my_setjmp(env)`）。既定はコンパイラ組み込み（freestanding）／libc（hosted） |
| 式評価中の脱出（自前 `longjmp`） | 任意 | カーネルやアプリが例外フレームへ直接 `longjmp` する場合、到達した時点で保存した深さへ `p2c_binop_rewind()`／`p2c_fstr_rewind()` で戻す。戻さないと式評価中の一時値（二項演算の左オペランド、f-stringビルダ）が残り、反復でネスト上限を誤発火してビルダがリークする。生成C（`--embed-entry` を含む）の `try` は `p2c_binop_depth()`／`p2c_fstr_depth()` を保存して自動でこれを行う |
| 時計 | 任意 | 単調増加ミリ秒。未指定なら0 |
| 入力 | 任意 | `input()` 用の行読み。未指定なら常にEOF |
| panic | 任意 | OOM/回復不能時のフック。復帰しない前提（復帰した場合ライブラリは停止する） |
| 数値/文字列補助 | 必須 | `snprintf`/`strtoll`/`strtod` と `floor`/`fmod`/`pow`/`sqrt`/`sin`/`cos`。単一ヘッダーとfreestandingコアが要求する明示的な契約 |

## 5. ヒープとメモリ再利用

| 方式 | 解放の再利用 | 用途 |
|---|---|---|
| `P2C_Platform` の `alloc/realloc/free`（推奨） | カーネル次第 | カーネルヒープをそのまま使う。**ランタイムと変換器コア**、およびランタイム同梱スタブの `malloc` 系がこのヒープを使う（共有ヒープ）。`p2c_runtime_init(heap, size)` へ同じ領域を二重に渡さないこと |
| ランタイム同梱の線形ヒープ（既定） | しない（`p2c_runtime_shutdown()` で全解放） | 短命のタスク、デモ、単発実行。カーネルアロケータを注入していない場合にだけ唯一のヒープになる（`realloc` はブロックヘッダの旧サイズぶんだけコピーする） |
| `P2C_EMBED_PROVIDE_LIBC_HEAP` + `p2c_embed_heap_set_default()` | する（境界タグ + 空きリスト + 隣接合体） | 長時間走るタスク、常駐サービス |
| カーネル自前のアロケータ | カーネル次第 | 既にkmalloc等がある場合はこれを直接使う |

- `p2c_embed_heap_*` は libc を使わず固定領域だけを扱います。`p2c_embed_heap_check()`
  が空きリストと境界の整合性を検証します（開発時）。
- `p2c_embed_heap_init()` で渡す領域はアライン不要です（内部で16バイト境界へ切り上げ）。
- 1つの `P2C_EmbedHeap` は1つの領域に対応します。同じ領域を再初期化すると、
  以前のヒープ構造体の空きリストは無効になります。

### 5-1. ヒープサイズの見積り（実測）

ランタイムの初期化（`p2c_embed_start`）は、組込みモジュール（`math` など）の登録を
含むため、**起動直後に数KB〜十数KBを消費**します。既定構成（`math` 登録込み、
`PYTHON_CODE_TO_C_NO_PYGAME`）での実測値は次のとおりです
（`make test-embed-baseline` で再測定できます）。

| 項目 | 値 |
| --- | --- |
| 初期化直後の使用量（peak、math込み） | 約 17.7 KB（17,728 バイト） |
| `-DPYTHON_CODE_TO_C_NO_MATH_MODULE` を定義した場合 | **704 バイト**（約25分の1。mathを使わないカーネル向け） |
| 推奨ヒープ | 32 KB 以上（タスクの作業用に余裕を持たせる） |
| テンプレート既定 | 64 KB（`templates/hobby_os`）、HobbyOS向けELFは128 KB |

初期化に必要なメモリが確保できない場合、`p2c_embed` は設定の `panic` フックへ進みます
（カーネルでは halt、テストでは `longjmp` で復帰）。ヒープを小さくしすぎると
この経路に入るため、上の表を目安に確保してください。
- `P2C_EMBED_PROVIDE_LIBC_HEAP` は `malloc` を置き換えます。ホストCライブラリを
  併用する場合は、その内部確保もこのヒープを通るため、起動直後に既定ヒープを
  有効化してください。
- `p2c_platform_set()` で登録したアロケータは、変換器コア（字句解析・AST・文字列
  ビルダ・生成C文字列）の確保にも使われます（共通層の共有ヒープ抽象
  `p2c_heap_alloc`/`p2c_heap_realloc`/`p2c_heap_free`）。プラットフォーム確保ブロックには
  16バイトのヘッダが前置され、`free`/`realloc` へ実サイズが渡されます。
- **同じ領域をプラットフォームアロケータと `p2c_runtime_init(heap, size)` の
  両方に渡さないでください**（バンプポインタが二重になりヒープが壊れます）。
  どちらか一方だけを使います。
- アロケータだけを注入したい場合は `p2c_platform_set_allocator(kalloc, krealloc, kfree, ctx)`
  を使います（`write`/`clock_ms` は既定実装を継承）。`P2C_Allocator` を変換器の
  既定アロケータとして注入する場合は `p2c_set_default_allocator(p2c_linear_allocator(arena, size))`
  を使います。解除はどちらも `NULL` を渡します。
- 停止順序は「`p2c_embed_stop()`／`p2c_runtime_shutdown()` でランタイムを止める
  → プラットフォームを解除する」です。解除後に残ったプラットフォームブロックは
  ブロック先頭から `free` へ委譲されますが、kmalloc 等の libc 非互換アロケータでは
  解除前に全解放しておいてください。
- `p2c_heap_uses_platform()`（プラットフォーム提供か）と `p2c_heap_usable()`
  （実際に確保できるか）で共有ヒープの状態を診断できます。

## 6. GCの安全側設計

- 保守的スタックスキャンの走査範囲は「**現在の関数フレームから、宣言されたスタック上端まで**」です。
  未使用のスタック領域は走査しないため、8MiBのスタック区間を宣言しても収集コストは
  使用量に比例します（`p2c_gc_last_stack_words()` で語数を確認できます）。
- 境界が不明な間は `p2c_gc_collect()` としきい値による自動収集は**何もしません**
  （生存オブジェクトを誤って解放しないため）。この状態では一度だけ診断を出力します。
- `p2c_gc_init()` に渡した「スタック上のアドレス」は上端のヒントとして扱われ、
  それだけでスキャンが有効になります（生成Cの `P2C_GC_ENTER_MAIN()` がこの経路）。
  正確な区間を宣言したい場合は次のどちらかを使います。
  - `p2c_gc_set_stack_bounds(lo, hi)`
  - `P2C_GC_ENTER_TASK(lo, hi)` マクロ（`p2c_gc_init` + 境界宣言）
- `p2c_gc_set_stack_bounds()` はスタックの成長方向を仮定しません（上下端を正規化して扱います）。
- 非Linuxのホステッド環境で区間を問い合わせるAPIが無い場合は、
  `-DP2C_GC_STACK_HINT_RANGE=<bytes>` で「ヒントから最大Nバイト下」を宣言できます
  （カーネルではタスク構造体の値を渡す方が確実です）。
- スタックスキャンは収集ごとに構築するアドレス索引で判定するため、
  従来の線形探索より高速です（索引の確保に失敗した場合のみ線形へフォールバック）。

## 7. メモリ枯渇（OOM）の扱い

| 設定 | 挙動 |
|---|---|
| `cfg.raise_memory_error = true`（既定） | 例外フレームがあれば `MemoryError` を送出し、`except MemoryError` で受けられる。無ければシンクへ診断し panic フックへ |
| `cfg.raise_memory_error = false` | 従来どおり確保失敗をNULLで返す（呼び出し側が処理する） |
| `p2c_runtime_set_oom_handler(fn, user)` | 任意のハンドラを登録。`p2c_oom_raise_memory_error` が標準実装（事前確保済みの `MemoryError` を使うため、枯渇中でも再確保しない） |

## 7-1. 実行予算（サンドボックス）— 信頼できないコードを走らせる

カーネル内で「利用者が持ち込んだPython」を動かす場合、無限ループやメモリ爆発を
**ハングやクラッシュではなく例外**として止められる必要があります。そのための予算APIを
ランタイムに用意しました。

```c
P2C_SandboxLimits limits = { .max_ticks = 200000, .max_allocs = 8192 };
p2c_sandbox_set(&limits);   /* 0 は無制限。タスク開始時に呼ぶ */
p2c_embed_start(...);       /* 変換済みモジュールを実行 */
p2c_sandbox_set(NULL);      /* 実行後に無制限へ戻す */

uint64_t ticks  = p2c_sandbox_ticks();       /* 消費したループ後退エッジ数 */
uint64_t allocs = p2c_sandbox_allocs();      /* 消費した確保数 */
uint64_t viol   = p2c_sandbox_violations();  /* 予算超過で例外を出した回数 */
p2c_sandbox_reset();                         /* 消費量だけをゼロに戻す */
```

- **ステップ予算** (`max_ticks`) はループの後退エッジで消費します。生成Cの `while` には
  `p2c_sandbox_tick()` が自動で入るため、`while True: pass` のような確保を伴わない
  無限ループも検出できます。`for` も反復ごとにランタイム側の反復経路を通ります。
- **確保予算** (`max_allocs`) は `p2c_obj_new()` で検査し、超過時は確保を失敗させます。
- 超過時は `SandboxError` を送出します。生成Cの `except` で捕捉できるので、
  「予算内で終わらなかった」ことをOS側・Python側の両方で扱えます。
- 予算超過後は内部で予算を無効化し、例外処理中の二次的な超過で例外が連鎖しないようにします。
- 予算が未設定（0）のときの追加コストは分岐1回だけで、通常の実行性能に影響しません。
- HobbyOS では `write` フック経由で `SandboxError` の内容をログに出し、タスクを終了させます。
- 回帰: `make test-sandbox`（ステップ／確保の各予算が例外になること、無制限時に何も起きないこと、
  生成ループに計装が入ること）。

## 8. OS内で変換する（オンデバイス変換）

```c
#include "core/python_code_to_c.h"

char *generated = NULL;
P2C_Result r = python_to_c(python_source, NULL, &generated);
/* 成功時 generated は malloc 系で確保される。使い終わったら free()。 */
```

- 変換器コアは `templates/toolchains/freestanding-c11.mk` と同じ構成で
  標準Cライブラリ無しにビルドできます（`make freestanding`）。
- `PYTHON_CODE_TO_C_NO_STDLIB` でも既定アロケータが `malloc/free` へ委譲するため、
  カーネルは `p2c_runtime_init()`（または embed のヒープ）を用意するだけで使えます。
  `p2c_platform_set()` を登録した場合は、変換器コアの確保も含めて
  そのアロケータ（共有ヒープ）へ一本化されます。
- 既定アロケータを持たないカーネル構成では、変換器コアは静的バッファ
  （`P2C_COMPILER_FALLBACK_HEAP_SIZE`、既定64KiB）へフォールバックし、呼び出しごとに
  解放されるため同じプロセスで何度でも実行できます。判定は変換開始時の8バイトの
  プローブで行うため、`p2c_core_static_allocator_active()` が `false` ならOSのヒープ
  だけを使っています。`-DP2C_COMPILER_FALLBACK_HEAP_SIZE=0` で静的バッファを完全に
  排除できます（未注入なら `P2C_ERR_NOMEM` と注入方法を返します）。
- `make test-embed-compile` がこの経路をカーネル相当環境で実行します。

## 9. 品質ゲート

| コマンド | 検証内容 |
|---|---|
| `make test-embed-runtime` | 組込みヒープ（分割・合体・realloc・破損検出）、ライフサイクル、タスク再起動、GCの安全側停止と有効化、OOM通知、MemoryError化、panic経路 |
| `make test-freestanding-setjmp` | カーネル提供 setjmp/longjmp の契約（raise/except、入れ子フレーム、深いフレームからの longjmp） |
| `make test-baremetal-exceptions` | 自作libcスタブ構成での例外機構（コンパイラ組み込みsetjmp）、スタック上の生存ローカル保護、プラットフォームアロケータが実際に使われること |
| `make test-gc-temp-roots` | 式評価中のTLS一時値（二項演算の左オペランド、処理中の例外）が収集のルートに入ること、式の途中で例外が脱出しても深さが漏れず、その後の式が正しいこと、停止後に深さが0へ戻ること |
| `make test-gc-stack-scan-scope` | 保守的スタックスキャンが使用中の範囲だけを走査すること（`p2c_gc_last_stack_words()`）と、ローカルのみ到達可能なオブジェクトの生存 |
| `make test-embed-compile` | カーネル相当環境での変換器コア実行（`--embed-entry` 相当APIと不正名の拒否も検証） |
| `make test-embed-generated` | `--embed-entry` 生成モジュールをカーネル相当ドライバで実行し、CPythonと出力差分比較 |
| `make test-hobby-os-template` | テンプレートのコンパイル（警告即エラー）とライブラリ化 |
| `make test-crlf` | 改行コード（LF/CRLF/CR）に対して生成Cが完全一致すること |
| `make test-baremetal-generated` | ベアメタル例（ネストしたクラス定義を含む `examples/baremetal/baremetal_hello.py`）が `PYTHON_CODE_TO_C_NO_STDLIB` のfreestandingで変換・オブジェクト化できること |
| `make test-allocator-injection` | カーネルアロケータの注入（`p2c_platform_set_allocator`／`p2c_set_default_allocator`）が変換器コアとランタイムの両方へ効くこと、注入が失敗する場合だけ静的フォールバックへ落ちること |
| `make test-setjmp-hook` | `P2C_SETJMP`/`P2C_LONGJMP` を差し替えた状態で、ランタイムと生成Cの例外機構がフックを通ること |
| `make CC=clang test-single-header-freestanding` | 単一ヘッダの freestanding 実装部がHosted参照を持たないこと |

## 10. 参考ファイル

| ファイル | 内容 |
|---|---|
| `include/platform/python_code_to_c_embed.h` | 公開API（設定・ヒープ・ライフサイクル・統計） |
| `src/platform/python_code_to_c_embed.c` | 実装（ヒープ、シンク、互換フック、`malloc`委譲） |
| `examples/embed/` | 実行例（`embed_boot.py`、カーネル相当ドライバ、スタック境界、x86-64 setjmp） |
| `templates/hobby_os/` | 組み込みテンプレート（推奨構成とフルコントロール構成、Makefile断片） |
| `tests/test_embed_runner.c` | 組込みAPIの回帰 |
| `tests/test_freestanding_setjmp.c` | 例外機構の回帰 |
| `tests/test_setjmp_hook.c` + `tests/setjmp_hook_override.h` | `P2C_SETJMP`/`P2C_LONGJMP` のOS差し替え回帰（フック呼び出し回数で検証） |
| `tests/test_allocator_injection.c` | アロケータ注入（`p2c_platform_set_allocator`／`p2c_set_default_allocator`）と静的フォールバック判定の回帰 |
| `tests/test_embed_transpile.c` | オンデバイス変換の回帰 |
