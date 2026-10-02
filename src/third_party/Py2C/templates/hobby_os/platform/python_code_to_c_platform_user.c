/*
 * 自作OSへの組み込み（フルコントロール構成）
 * ==========================================
 * p2c_embed を使わず、カーネル自身が P2C_Platform と互換フックの全てを実装する
 * 場合のひな形です（既に自前のアロケータ・コンソール・パニック経路があり、
 * それらへ直接配線したい場合に選びます）。p2c_embed を使う推奨構成は
 * ../embed/hobby_os_embed.c と ../README.md を参照してください。
 *
 * 実装が必要なもの（platform.h / runtime.h の契約）:
 *   - P2C_Platform（alloc/realloc/free/write/clock_ms）
 *   - p2c_platform_init/shutdown/write/write_n/read_line/abort
 *   - malloc/realloc/calloc/free（PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義する場合）
 *   - setjmp/longjmp（PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義する場合）
 *
 * このファイルは `make test-hobby-os-template` が警告即エラーでコンパイルします。
 */
#include "platform/python_code_to_c_platform.h"
#include "runtime/python_code_to_c_runtime.h"
#include "hobby_os_api.h"

/* ── カーネルAPI（各自のものへ置き換える） ────────────────────────── */
extern void *kmalloc(unsigned long size);
extern void *krealloc(void *ptr, unsigned long size);
extern void kfree(void *ptr);
extern void uart_write_buf(const char *data, unsigned long len);
extern void uart_write_string(const char *s);
extern unsigned long long kernel_ticks_ms(void);

static void hobby_alloc_hook(size_t requested, const char *context, void *user) {
    (void)requested;
    (void)context;
    (void)user;
    /* ここでカーネルのログへ枯渇を記録し、必要なら halt する。
     * （例外フレームが有効な状態で MemoryError を送出したい場合は
     *  p2c_oom_raise_memory_error() をそのまま登録する。） */
}

static void *platform_alloc(size_t size, void *user) {
    (void)user;
    return kmalloc((unsigned long)size);
}

static void *platform_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)old_size;
    (void)user;
    return krealloc(ptr, (unsigned long)new_size);
}

static void platform_free(void *ptr, size_t size, void *user) {
    (void)size;
    (void)user;
    kfree(ptr);
}

static void platform_write_fn(int stream, const char *data, size_t len, void *user) {
    (void)stream;
    (void)user;
    uart_write_buf(data, (unsigned long)len);
}

static uint64_t platform_clock(void *user) {
    (void)user;
    return (uint64_t)kernel_ticks_ms();
}

static P2C_Platform hobby_platform(void) {
    P2C_Platform p;
    p.alloc = platform_alloc;
    p.realloc = platform_realloc;
    p.free = platform_free;
    p.write = platform_write_fn;
    p.clock_ms = platform_clock;
    p.user = NULL;
    return p;
}

/* ── 互換フック ───────────────────────────────────────────────── */
void p2c_platform_init(void) {
}

void p2c_platform_shutdown(void) {
}

void p2c_platform_write(const char *s) {
    if (s) uart_write_string(s);
}

void p2c_platform_write_n(const char *s, size_t len) {
    if (s && len) uart_write_buf(s, (unsigned long)len);
}

size_t p2c_platform_read_line(char *buf, size_t cap) {
    if (buf && cap > 0) buf[0] = '\0';
    /* 実機ではコンソールの行入力をエコーなしで読み、長さを返す。 */
    return 0;
}

void p2c_platform_abort(const char *reason) {
    if (reason) {
        uart_write_string(reason);
        uart_write_string("\n");
    }
    for (;;) {
        /* 割り込み禁止で停止（カーネル側の halt を呼ぶ） */
    }
}

/* ── タスク起動例 ─────────────────────────────────────────────── */
/* 生成モジュールのエントリは hobby_os_api.h が宣言する
 * （-Wredundant-decls のため再宣言しない）。 */

int p2c_hobby_run_task_full(void *heap, size_t heap_size, void *stack_lo, void *stack_hi) {
    P2C_Platform platform = hobby_platform();
    p2c_platform_set(&platform);
    p2c_runtime_init(heap, heap_size);
    /* スタック区間を宣言して初めて保守的GCスキャンが有効になる。 */
    P2C_GC_ENTER_TASK(stack_lo, stack_hi);
    p2c_gc_set_threshold(8u * 1024u);
    /* 例外フレームが無い状態の枯渇を診断するハンドラ（raiseはしない）。 */
    p2c_runtime_set_oom_handler(hobby_alloc_hook, NULL);

    P2C_Object *result = kernel_python_program(); /* 生成モジュール本体 */
    if (!result) return -1;

    p2c_runtime_shutdown();
    p2c_platform_set(NULL);
    return 0;
}

