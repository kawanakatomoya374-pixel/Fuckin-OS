/*
 * 自作OSへの組み込み例（推奨構成: p2c_embed ファサード）
 * =====================================================
 * このファイルがカーネル側で用意する唯一のファイルです。次のマクロを付けて
 * ビルドし、下記「リンク対象」をリンクしてください。
 *
 *   -DPYTHON_CODE_TO_C_NO_STDLIB -DPYTHON_CODE_TO_C_NO_PYGAME
 *   -DPYTHON_CODE_TO_C_NO_LIBC_STUBS
 *   -DP2C_EMBED_PROVIDE_LIBC_HEAP -DP2C_EMBED_PROVIDE_PLATFORM_COMPAT
 *
 *   リンク対象:
 *     src/runtime/python_code_to_c_runtime.c
 *     src/common/python_code_to_c_common.c
 *     src/platform/python_code_to_c_platform.c
 *     src/platform/python_code_to_c_embed.c
 *     カーネルの malloc/free/realloc/calloc と setjmp/longjmp
 *     変換済みモジュール（--embed-entry kernel_python_program）
 *
 * 各 YOUR_* はカーネルのAPIへ置き換えてください。このファイルは
 * `make test-hobby-os-template` が警告即エラーでコンパイルするため、
 * 常にビルド可能な状態に保たれています。
 */
#include "platform/python_code_to_c_embed.h"
#include "hobby_os_api.h"

/* ── カーネルAPI（各自のものへ置き換える） ────────────────────────── */
extern void uart_write_byte(char c);                    /* シリアル出力 */
extern uint64_t kernel_ticks_ms(void);                  /* 単調増加ミリ秒 */
extern void kernel_halt(void);                          /* 割り込み禁止で停止 */
extern void kernel_read_line(char *buf, unsigned long cap); /* コンソール入力 */

/* タスクごとに用意する領域（静的に確保してよい） */
#define P2C_HOBBY_HEAP_SIZE (64u * 1024u)
#define P2C_HOBBY_CONSOLE_SIZE 512u

static unsigned char p2c_heap_storage[P2C_HOBBY_HEAP_SIZE];
static char p2c_console[P2C_HOBBY_CONSOLE_SIZE];
static P2C_EmbedHeap p2c_heap;

/* ── デバイス接続 ──────────────────────────────────────────────── */
static void p2c_hobby_uart(void *user, const char *data, size_t len) {
    (void)user;
    if (!data) return;
    for (size_t i = 0; i < len; i++) uart_write_byte(data[i]);
}

static uint64_t p2c_hobby_clock(void *user) {
    (void)user;
    return kernel_ticks_ms();
}

static size_t p2c_hobby_input(char *buf, size_t cap, void *user) {
    (void)user;
    if (!buf || cap == 0) return 0;
    kernel_read_line(buf, (unsigned long)cap);
    size_t len = 0;
    while (buf[len] != '\0') len++;
    return len;
}

static void p2c_hobby_panic(const char *reason, void *user) {
    (void)user;
    p2c_embed_panic(reason);
    kernel_halt();
}

/* ── タスク本体 ────────────────────────────────────────────────── */
/* task_stack_lo / task_stack_hi はそのタスクのスタック区間の両端。
 * ここで宣言することで、保守的GCスキャンがタスクのローカル変数を
 * ルートとして認識できる。 */
int p2c_hobby_run_task(void *task_stack_lo, void *task_stack_hi) {
    /* malloc を置き換えるため、他のどの確保よりも先に既定ヒープを有効化する。 */
    if (p2c_embed_heap_init(&p2c_heap, p2c_heap_storage, sizeof(p2c_heap_storage)) != 0) return -1;
    p2c_embed_heap_set_default(&p2c_heap);

    P2C_EmbedConfig cfg;
    p2c_embed_config_init(&cfg);
    p2c_embed_config_use_uart(&cfg, p2c_hobby_uart, NULL);
    p2c_embed_config_use_console(&cfg, p2c_console, sizeof(p2c_console));
    p2c_embed_config_use_heap(&cfg, &p2c_heap, p2c_heap_storage, sizeof(p2c_heap_storage));
    p2c_embed_config_use_stack(&cfg, task_stack_lo, task_stack_hi);
    cfg.clock_ms = p2c_hobby_clock;
    cfg.read_line = p2c_hobby_input;
    cfg.panic = p2c_hobby_panic;
    cfg.enable_gc = true;
    cfg.gc_threshold = 8u * 1024u;
    cfg.raise_memory_error = true;

    if (p2c_embed_start(&cfg) != 0) return -1;

    P2C_Object *result = p2c_embed_run_program(kernel_python_program);
    if (!result) {
        /* 未処理例外: 診断はすでにUART/コンソールへ出力済み。 */
        p2c_embed_stop();
        return -2;
    }

    /* カーネルのログ/メトリクス用の実測値 */
    P2C_EmbedStats stats;
    p2c_embed_stats(&stats);
    if (p2c_embed_heap_check(&p2c_heap) != 0) { /* 開発時の整合性検査 */
        p2c_embed_stop();
        return -3;
    }
    (void)stats;

    p2c_embed_stop(); /* タスク終了。再度 start すれば再実行できる。 */
    return 0;
}
