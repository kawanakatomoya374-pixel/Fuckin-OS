# HobbyOS integration template (Alpha0.6)

`p2c_embed` は「ヒープ・出力・時計・スタック区間」だけをカーネルから受け取り、
`P2C_Platform`・ランタイム初期化・GCのスタック境界・OOM方針をまとめて設定する
統合ファサードです。自作OSへは**次の2段階**で組み込めます。

## 1. 変換側（開発ホスト）

```sh
python-code-to-c kernel_module.py --embed-entry kernel_python_program -o kernel_module.c
```

`--embed-entry NAME` は `int main()` を生成せず、カーネルから呼べる
`P2C_Object *NAME(void)` だけを生成します。ヒープ・スタック・出力の所有権は
カーネル側に残ります。

## 2. カーネル側

追加するのは次の2ファイルだけです（`embed` 構成）。

| ファイル | 役割 |
|---|---|
| `templates/hobby_os/embed/hobby_os_embed.c` | UART・時計・スタック境界を `P2C_EmbedConfig` へ接続し、タスクとして実行する |
| `src/platform/python_code_to_c_embed.c`（本体に同梱） | 組込みヒープ、シンク、ライフサイクル、OOM/panic、互換フック、malloc/free |

ビルド時に定義するマクロ:

```
-DPYTHON_CODE_TO_C_NO_STDLIB          # 標準Cライブラリなし
-DPYTHON_CODE_TO_C_NO_PYGAME          # GUIモジュールを除外
-DPYTHON_CODE_TO_C_NO_LIBC_STUBS      # カーネルの malloc/free を使う（推奨）
-DP2C_EMBED_PROVIDE_LIBC_HEAP         # embed のヒープを malloc/free へ委譲
-DP2C_EMBED_PROVIDE_PLATFORM_COMPAT   # 互換フックも embed が提供
```

リンクするソース:

```
src/runtime/python_code_to_c_runtime.c
src/common/python_code_to_c_common.c
src/platform/python_code_to_c_platform.c
src/platform/python_code_to_c_embed.c
templates/hobby_os/embed/hobby_os_embed.c
kernel_module.c                        # 変換で生成したモジュール
```

## 3. カーネルが必ず提供するもの

| 契約 | 内容 |
|---|---|
| `malloc/free/realloc/calloc` | `PYTHON_CODE_TO_C_NO_LIBC_STUBS` を定義する場合、カーネルのアロケータ。`p2c_embed_heap_*` をそのまま使い、GCが解放したメモリを再利用できるようにするのが推奨。`p2c_platform_set()` のアロケータは変換器コア（文字列ビルダ等）にも使われるため、同じ領域を `p2c_runtime_init(heap, size)` と二重に渡さないこと。**定義しない場合でも、`p2c_platform_set_allocator()` でカーネルアロケータを注入すれば、ランタイム同梱スタブの `malloc/free` もそのヒープへ委譲される**（回帰: `make test-heap-unification`） |
| `setjmp/longjmp` | `PYTHON_CODE_TO_C_NO_LIBC_STUBS` を定義する場合のみ必要。`runtime.h` の `jmp_buf`（`void *buf[16]`）契約に合わせる。`examples/embed/x86_64_setjmp.c` に x86-64 の参考実装がある。**定義しない場合（既定）はコンパイラ組み込み（`__builtin_setjmp`/`__builtin_longjmp`）で例外が動作するため、実装は不要**。カーネルの実装へ完全に差し替える場合は `P2C_SETJMP`/`P2C_LONGJMP` を定義する（例: `-DP2C_SETJMP(env)=my_setjmp(env)`）。ランタイム本体と生成Cの両方がこのフックを通る |
| タスクスタック区間 | `p2c_embed_config_use_stack()` へ渡す。`P2C_GC_ENTER_TASK()`／`P2C_GC_ENTER_MAIN()` でも境界（上端ヒント）を宣言できる。境界が無い場合のみ回収は安全側に停止する |
| `snprintf`/`strtoll`/`strtod`/`floor`/`fmod`/`pow`/`sqrt`/`sin`/`cos` | 数値・文字列変換の補助契約（単一ヘッダーとfreestandingコアが要求） |

## 4. ヒープ設計の指針

- 既定の線形ヒープは `free` を再利用しないため、長時間走るタスクでは枯渇します。
  `P2C_EMBED_PROVIDE_LIBC_HEAP` + `p2c_embed_heap_set_default()` を最初に呼ぶと、
  GCの解放がそのまま再利用へつながります（境界タグ + 空きリスト + 隣接合体）。
- `malloc` を置き換える場合、ブート直後（他の確保より先）に既定ヒープを
  有効化してください。ホストCライブラリを併用する構成では、その内部確保も
  embed ヒープを通ります。
- 1つの `P2C_EmbedHeap` は1つの領域に対応します。同じ領域を別の構造体で
  再初期化すると、以前の空きリストは無効になります。
- アロケータだけを注入する場合は `p2c_platform_set_allocator(kalloc, krealloc, kfree, ctx)`
  が使えます（`write`/`clock_ms` は既定実装を継承し、カーネルのヒープがランタイムと
  変換器コアの唯一のヒープになります）。`P2C_Allocator` を変換器コアの既定にする場合は
  `p2c_set_default_allocator(p2c_linear_allocator(arena, size))` を使います。
- 変換器コアの静的フォールバック（`P2C_COMPILER_FALLBACK_HEAP_SIZE`、既定64KiB）は、
  アロケータ／共有ヒープが確保できない場合だけ使われます。カーネルのヒープを注入した
  構成では `p2c_core_static_allocator_active()` が `false` になります。静的な確保を
  一切許さない場合は `-DP2C_COMPILER_FALLBACK_HEAP_SIZE=0` で無効化できます。
- 停止順序は「`p2c_embed_stop()`／`p2c_runtime_shutdown()` → プラットフォーム解除」です。

## 5. 実行と後始末

```c
P2C_EmbedConfig cfg;
p2c_embed_config_init(&cfg);
p2c_embed_config_use_uart(&cfg, my_uart_write, NULL);
p2c_embed_config_use_console(&cfg, my_console, sizeof(my_console));
p2c_embed_config_use_heap(&cfg, &my_heap, my_heap_storage, sizeof(my_heap_storage));
p2c_embed_config_use_stack(&cfg, task->stack_base, task->stack_top);
cfg.clock_ms = my_clock_ms;
cfg.panic = my_panic;                 /* 復帰しない前提のハンドラ */
if (p2c_embed_start(&cfg) != 0) return -1;
P2C_Object *result = p2c_embed_run_program(kernel_python_program);
P2C_EmbedStats stats;
p2c_embed_stats(&stats);              /* ヒープ/GCの実測値をログへ */
p2c_embed_stop();                     /* タスク終了。再startで再実行できる */
```

- 捕捉されなかった例外は `p2c_embed_run_program()` がシンクへ診断出力し、NULLを
  返します（カーネルを落としません）。
- メモリ枯渇は `cfg.raise_memory_error` により、例外フレームがあれば
  `MemoryError` として `except` へ、無ければ panic フックへ渡ります。
- `p2c_embed_heap_check()` でヒープ整合性を検証できます（開発時）。

## 6. 品質ゲート

以下のテストがテンプレートと同じ構成を実際にビルド・実行します。

| ターゲット | 検証内容 |
|---|---|
| `make test-embed-runtime` | ヒープ（分割・合体・realloc・破損検出）、ライフサイクル、GCの安全側停止、OOM通知、MemoryError化、panic経路 |
| `make test-freestanding-setjmp` | NO_STDLIB での raise/except/入れ子フレーム |
| `make test-embed-compile` | カーネル相当環境で変換器コア自体を実行（オンデバイス変換） |
| `make test-embed-generated` | `--embed-entry` で生成したモジュールをカーネル相当ドライバで実行しCPythonと差分比較 |
| `make test-hobby-os-template` | テンプレートのコンパイル（警告即エラー） |
| `make test-allocator-injection` | カーネルアロケータの注入（`p2c_platform_set_allocator`／`p2c_set_default_allocator`）と静的フォールバック判定 |
| `make test-setjmp-hook` | `P2C_SETJMP`/`P2C_LONGJMP` の差し替えがランタイムと生成Cに効くこと |
| `make test-heap-unification` | ランタイム同梱スタブの `malloc/free` がカーネルアロケータ（共有ヒープ）へ委譲され、変換器・ランタイムと1つのヒープを共有すること |

詳細は [`../../docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md`](../../docs/spec/HOBBY_OS_EMBEDDING_ALPHA0.6.md) を参照してください。
