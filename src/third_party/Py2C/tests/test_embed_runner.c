/*
 * Embedded-integration regression for p2c_embed (kernel-style setup):
 *   - boundary-tagged heap: alignment, split, coalescing, realloc, corruption check
 *   - config/lifecycle: start -> run -> stats -> stop -> restart (task restart)
 *   - GC safety: collection is refused while the stack window is unknown
 *   - OOM policy: notification hook, and MemoryError delivery into try/except
 *
 * Built with PYTHON_CODE_TO_C_NO_STDLIB (no hosted platform), the embed facade
 * supplying the platform hooks plus malloc/free (P2C_EMBED_PROVIDE_LIBC_HEAP /
 * P2C_EMBED_PROVIDE_PLATFORM_COMPAT are supplied by the build), and the x86-64
 * setjmp reference implementation that a kernel must provide.
 */
#include "platform/python_code_to_c_embed.h"
#include "host_stack_bounds.h"

#ifdef P2C_EMBED_TEST_TRACE
/* 段階の進行を1文字で出力する（デバッグ用。カーネル向けコードではない）。 */
extern long write(int fd, const void *buf, unsigned long count);
static void trace_stage(char tag) { (void)write(1, &tag, 1); }
#else
static void trace_stage(char tag) { (void)tag; }
#endif

#define HEAP_STORAGE_SIZE 65536u
#define CONSOLE_SIZE 4096u

static unsigned char heap_storage[HEAP_STORAGE_SIZE + 1u]; /* +1 で非アライン開始も作れる */
/* ヒープ構造体と領域は1対1。単体テストは別領域を使う（同じ領域を別の
 * P2C_EmbedHeapで再初期化すると、以前の構造体の空きリストが無効になる）。 */
static unsigned char probe_storage[HEAP_STORAGE_SIZE + 1u];
static char console_buffer[CONSOLE_SIZE];
static P2C_EmbedHeap embed_heap;
static size_t uart_bytes;

static void test_uart(void *user, const char *data, size_t len) {
    (void)user;
    (void)data;
    uart_bytes += len;
}

static P2C_ExceptFrame *jump_target;
static volatile int panic_seen;
static volatile int panic_reason_ok;

static void test_panic(const char *reason, void *user) {
    trace_stage('!');
    (void)user;
    panic_seen = 1;
    panic_reason_ok = (reason && strstr(reason, "out of memory") != NULL) ? 1 : 0;
    /* カーネルではhaltする。テストでは異常系でも観測できるよう脱出する。
     * OOM中のため例外オブジェクトは作らない（確保できないのが前提）。 */
    if (jump_target) {
        P2C_ExceptFrame *frame = jump_target;
        jump_target = NULL;
        longjmp(frame->env, 1);
    }
    for (;;) { }
}

static void config_for(P2C_EmbedConfig *cfg, void *heap_base, size_t heap_size) {
    p2c_embed_config_init(cfg);
    p2c_embed_config_use_uart(cfg, test_uart, NULL);
    p2c_embed_config_use_console(cfg, console_buffer, sizeof(console_buffer));
    p2c_embed_config_use_heap(cfg, &embed_heap, heap_base, heap_size);
    cfg->panic = test_panic;
}

/* 組込みヒープをグローバルな malloc 供給元として最初に有効化する。
 * P2C_EMBED_PROVIDE_LIBC_HEAP は malloc を置き換えるため、pthread など
 * ホストCライブラリの一部もこのヒープから確保する。境界取得より先に
 * ヒープを用意しておかないと、それらの確保がENOMEMで失敗する。 */
static void prime_default_heap(void) {
    (void)p2c_embed_heap_init(&embed_heap, heap_storage, HEAP_STORAGE_SIZE);
    p2c_embed_heap_set_default(&embed_heap);
}

/* ---------- 1. ヒープ単体 ---------- */
static int check_heap_allocator(void) {
    trace_stage('H');
    P2C_EmbedHeap heap;
    /* わざと1バイトずらした領域を渡し、内部でアラインされることを確認する。 */
    if (p2c_embed_heap_init(&heap, probe_storage + 1u, HEAP_STORAGE_SIZE) != 0) return 10;
    if ((uintptr_t)heap.base % 16u != 0) return 11;

    void *a = p2c_embed_heap_alloc(&heap, 100);
    void *b = p2c_embed_heap_alloc(&heap, 100);
    void *c = p2c_embed_heap_alloc(&heap, 100);
    if (!a || !b || !c) return 12;
    if ((uintptr_t)a % 16u || (uintptr_t)b % 16u || (uintptr_t)c % 16u) return 13;
    if ((unsigned char*)b - (unsigned char*)a < 100) return 14; /* 重なりなし */

    /* 内容が保持されること */
    for (size_t i = 0; i < 100; i++) ((unsigned char*)b)[i] = (unsigned char)(i & 0x7fu);
    p2c_embed_heap_free(&heap, b);
    if (p2c_embed_heap_check(&heap) != 0) return 15;
    void *b2 = p2c_embed_heap_alloc(&heap, 64);
    if (b2 != b) return 16; /* 同じ空きブロックが再利用される */
    for (size_t i = 0; i < 64; i++) if (((unsigned char*)b2)[i] != (unsigned char)(i & 0x7fu)) return 17;

    /* reallocで成長（隣接空きと結合） */
    p2c_embed_heap_free(&heap, c);
    unsigned char *grown = (unsigned char*)p2c_embed_heap_realloc(&heap, b2, 96);
    if (!grown) return 18;
    for (size_t i = 0; i < 64; i++) if (grown[i] != (unsigned char)(i & 0x7fu)) return 19;

    /* 縮小と移動（大きすぎる拡張） */
    unsigned char *shrunk = (unsigned char*)p2c_embed_heap_realloc(&heap, grown, 32);
    if (!shrunk) return 20;
    for (size_t i = 0; i < 32; i++) if (shrunk[i] != (unsigned char)(i & 0x7fu)) return 21;
    /* 隣接空きだけで伸びてしまわないよう、大きな空きを先に埋めてから
     * 「新規確保＋コピー＋解放」の移動経路を強制する。 */
    void *filler = p2c_embed_heap_alloc(&heap, heap.capacity / 2u);
    if (!filler) return 225;
    size_t before_alloc_calls = heap.alloc_calls;
    unsigned char *moved = (unsigned char*)p2c_embed_heap_realloc(&heap, shrunk, 4096);
    if (!moved) return 22;
    if (heap.alloc_calls == before_alloc_calls) return 23; /* 移動したはず */
    for (size_t i = 0; i < 32; i++) if (moved[i] != (unsigned char)(i & 0x7fu)) return 24;
    p2c_embed_heap_free(&heap, filler);

    /* 二重解放は無視され、整合性は保たれる */
    p2c_embed_heap_free(&heap, a);
    p2c_embed_heap_free(&heap, a);
    if (p2c_embed_heap_check(&heap) != 0) return 25;

    /* 全部解放すると単一ブロックへ完全合体する（64KiB確保できる ）。 */
    p2c_embed_heap_free(&heap, moved);
    p2c_embed_heap_free(&heap, b2);
    if (heap.used != 0) return 26;
    if (p2c_embed_heap_free_bytes(&heap) != heap.capacity) return 27;
    void *whole = p2c_embed_heap_alloc(&heap, heap.capacity - 64u);
    if (!whole) return 28;
    if (p2c_embed_heap_check(&heap) != 0) return 281;
    p2c_embed_heap_free(&heap, whole);

    /* 容量超過要求は失敗し、統計に残る */
    size_t failures = heap.failures;
    if (p2c_embed_heap_alloc(&heap, heap.capacity + 1u) != NULL) return 29;
    if (heap.failures != failures + 1u) return 30;

    /* 小さすぎる領域は初期化に失敗する */
    P2C_EmbedHeap tiny;
    if (p2c_embed_heap_init(&tiny, probe_storage, 32u) == 0) return 31;
    return 0;
}


/* ---------- 2. ライフサイクルとGC ---------- */

/* モジュール本体相当: リストを積み上げて表示する（生成コードと同じAPIを使う）。 */
static P2C_Object *program_hello(void) {
    P2C_Object *items = p2c_list_new();
    if (!items) return NULL;
    for (int64_t i = 0; i < 5; i++) p2c_list_append(items, p2c_obj_from_int(i * i));
    p2c_print_multi((P2C_Object*[]){items}, 1);
    return items;
}

/* 使い捨てオブジェクトを大量に作る（GC+ヒープ再利用の検証用）。 */
static P2C_Object *program_churn(void) {
    for (int64_t i = 0; i < 400; i++) {
        P2C_Object *tmp = p2c_list_new();
        if (!tmp) return NULL;
        p2c_list_append(tmp, p2c_obj_from_int(i));
        p2c_list_append(tmp, p2c_obj_from_str("churn"));
    }
    return &P2C_None;
}

static int check_lifecycle_and_gc(void) {
    trace_stage('L');
    P2C_EmbedConfig cfg;
    void *stack_lo = NULL;
    void *stack_hi = NULL;
    p2c_host_stack_bounds(&stack_lo, &stack_hi);
    if (!stack_lo || !stack_hi) return 40; /* ホスト境界が取れない環境では検査不能 */

    config_for(&cfg, heap_storage, HEAP_STORAGE_SIZE);
    p2c_embed_config_use_stack(&cfg, stack_lo, stack_hi);
    cfg.gc_threshold = 4096; /* 小さくして収集を強制する */
    uart_bytes = 0;
    if (p2c_embed_start(&cfg) != 0) return 41;
    if (!p2c_embed_is_active()) return 42;
    if (!p2c_gc_stack_scan_available()) return 43;

    P2C_Object *result = p2c_embed_run_program(program_hello);
    if (!result) return 44;
    if (uart_bytes != p2c_embed_console_len()) return 45; /* UARTと控えが一致 */
    if (uart_bytes == 0) return 46;
    if (console_buffer[0] != '[') return 47;

    /* churn を3回: GC無しでは64KiBを使い切る量を割り当てる。 */
    for (int i = 0; i < 3; i++) {
        if (!p2c_embed_run_program(program_churn)) return 48;
    }
    P2C_EmbedStats stats;
    p2c_embed_stats(&stats);
    if (stats.gc_collections == 0) return 49;
    if (stats.gc_last_freed == 0) return 50;
    if (stats.alloc_failures != 0) return 51;          /* 再利用できていれば失敗しない */
    if (stats.heap_used > stats.heap_size) return 52;
    if (stats.heap_peak == 0) return 53;
    if (p2c_embed_heap_check(&embed_heap) != 0) return 54;

    p2c_embed_stop();
    if (p2c_embed_is_active()) return 55;
    if (p2c_platform_current() != p2c_platform_default()) return 56;

    /* タスク再起動: 同じ静的ヒープを使い回せる。 */
    if (p2c_embed_start(&cfg) != 0) return 57;
    if (!p2c_embed_run_program(program_hello)) return 58;
    p2c_embed_stop();
    return 0;
}

/* ---------- 3. スタック境界未登録時は回収しない（安全側の停止） ---------- */
static int check_collection_is_gated(void) {
    trace_stage('G');
    P2C_EmbedConfig cfg;
    config_for(&cfg, heap_storage, HEAP_STORAGE_SIZE);
    /* 境界を意図的に設定しない */
    if (p2c_embed_start(&cfg) != 0) return 60;
    if (p2c_gc_stack_scan_available()) return 61;
    p2c_embed_console_reset();
    size_t before_objects = p2c_gc_object_count();
    p2c_gc_collect();
    if (p2c_gc_collections_run() != 0) return 62;
    if (p2c_gc_object_count() != before_objects) return 63;
    const char *text = p2c_embed_console_text();
    if (strstr(text, "stack bounds") == NULL) return 64; /* 原因と対処が診断される */

    /* 境界を宣言すれば回収が有効になる。 */
    void *stack_lo = NULL;
    void *stack_hi = NULL;
    p2c_host_stack_bounds(&stack_lo, &stack_hi);
    if (!stack_lo || !stack_hi) return 65;
    p2c_gc_set_stack_bounds(stack_lo, stack_hi);
    if (!p2c_gc_stack_scan_available()) return 66;
    p2c_gc_collect();
    if (p2c_gc_collections_run() == 0) return 67;
    p2c_embed_stop();
    return 0;
}

/* ---------- 4. OOM通知とMemoryError化 ---------- */
static size_t oom_notifications;
static size_t oom_last_request;

static void counting_oom(size_t requested, const char *context, void *user) {
    (void)context;
    (void)user;
    oom_notifications++;
    oom_last_request = requested;
}

/* 小さなヒープをすぐ枯渇させる。NULLが返る前提で扱い、解放はGCに任せる。 */
static P2C_Object *program_exhaust(void) {
    for (int64_t i = 0; i < 20000; i++) {
        P2C_Object *box = p2c_obj_from_str("0123456789012345678901234567890123456789");
        if (!box) return &P2C_None;
    }
    return &P2C_None;
}

static int check_oom_notification(void) {
    trace_stage('O');
    P2C_EmbedConfig cfg;
    /* 組込みモジュール（math等）の登録には数KBのヒープが必要なので、
     * 枯渇テストでも初期化が完了する大きさを確保する（以前は4096で足りていた）。 */
    config_for(&cfg, heap_storage, 32768);
    cfg.raise_memory_error = false;         /* NULLを返すだけの従来動作 */
    cfg.enable_gc = false;                  /* 枯渇を確実にする */
    oom_notifications = 0;
    oom_last_request = 0;
    if (p2c_embed_start(&cfg) != 0) return 70;
    trace_stage('a');
    p2c_runtime_set_oom_handler(counting_oom, NULL);
    trace_stage('b');
    if (!p2c_embed_run_program(program_exhaust)) return 71;
    trace_stage('c');
    if (oom_notifications == 0) return 72;
    if (oom_last_request == 0) return 73;
    if (embed_heap.failures == 0) return 74;
    if (p2c_runtime_heap_used() != 0 || p2c_runtime_heap_size() != 0) return 75; /* 組込みヒープ経由 */
    p2c_runtime_set_oom_handler(NULL, NULL);
    p2c_embed_stop();
    return 0;
}

/* 例外フレームが有効なら MemoryError が raise され、except 側へ制御が移る。 */
static int check_oom_to_memory_error(void) {
    trace_stage('M');
    P2C_EmbedConfig cfg;
    /* 枯渇テストでも初期化（組込みモジュール登録）が完了するヒープ量を与える。 */
    config_for(&cfg, heap_storage, 32768);
    cfg.raise_memory_error = true;
    cfg.enable_gc = false;
    if (p2c_embed_start(&cfg) != 0) return 80;
    /* 事前確保された MemoryError シングルトンが用意されていること。 */
    if (p2c_make_exception("Probe", "probe") == NULL) return 81;

    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    int caught = 0;
    if (setjmp(frame.env) == 0) {
        (void)program_exhaust();
        p2c_exc_stack = frame.prev;
        p2c_embed_stop();
        return 82; /* 枯渇しなかった = テスト前提が崩れている */
    }
    p2c_exc_stack = frame.prev;
    if (p2c_exc_name_match(frame.exc, "MemoryError")) caught = 1;
    if (!caught) return 83;
    p2c_embed_stop();
    return 0;
}

/* パニック経路: 例外フレームが無いのに枯渇したら panic フックへ進む。 */
static int check_panic_path(void) {
    trace_stage('P');
    P2C_EmbedConfig cfg;
    /* panic経路のテストでも、初期化（組込みモジュール登録）が完了する量を与える。
     * 枯渇は program_exhaust() で意図的に起こす。 */
    config_for(&cfg, heap_storage, 32768);
    cfg.raise_memory_error = true;
    cfg.enable_gc = false;
    if (p2c_embed_start(&cfg) != 0) return 90;
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    jump_target = &frame;
    panic_seen = 0;
    panic_reason_ok = 0;
    int status = 0;
    if (setjmp(frame.env) == 0) {
        /* 例外フレームを外してから枯渇させ、panic経路へ入れる。 */
        p2c_exc_stack = NULL;
        (void)program_exhaust();
        status = 91; /* panic しなかった */
    } else {
        p2c_exc_stack = frame.prev;
        if (!panic_seen) status = 92;
        if (!panic_reason_ok) status = 95;
        const char *text = p2c_embed_console_text();
        if (strstr(text, "PANIC:") == NULL) status = 93;
        if (strstr(text, "MemoryError") == NULL) status = 94;
    }
    jump_target = NULL;
    p2c_exc_stack = NULL;
    p2c_embed_stop();
    return status;
}

int main(void) {
    prime_default_heap();
    int status = check_heap_allocator();
    if (status != 0) return status;
    status = check_lifecycle_and_gc();
    if (status != 0) return status;
    status = check_collection_is_gated();
    if (status != 0) return status;
    status = check_oom_notification();
    if (status != 0) return status;
    status = check_oom_to_memory_error();
    if (status != 0) return status;
    status = check_panic_path();
    if (status != 0) return status;
    return 0;
}
