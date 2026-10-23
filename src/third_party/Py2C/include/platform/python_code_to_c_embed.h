#ifndef PYTHON_CODE_TO_C_EMBED_H
#define PYTHON_CODE_TO_C_EMBED_H

#include "platform/python_code_to_c_platform.h"
#include "runtime/python_code_to_c_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 自作OS・組込みホスト向け統合ファサード (p2c_embed)
 * ============================================================
 * 目的: 「静的ヒープ・1つの出力シンク・1つの時計」さえカーネルが用意すれば、
 *       変換済みPythonモジュールをタスクとして走らせられるようにする。
 *
 * 典型的な使い方（詳細は docs/spec/HOBBY_OS_EMBEDDING_ALPHA1.0.md）:
 *
 *   static P2C_EmbedHeap heap;
 *   static unsigned char heap_storage[64 * 1024];
 *   static char console[512];
 *
 *   int my_kernel_task(void) {
 *       P2C_EmbedConfig cfg;
 *       p2c_embed_config_init(&cfg);
 *       p2c_embed_config_use_heap(&cfg, &heap, heap_storage, sizeof(heap_storage));
 *       p2c_embed_config_use_console(&cfg, console, sizeof(console));
 *       p2c_embed_config_use_uart(&cfg, my_uart_write, NULL);
 *       p2c_embed_config_use_stack(&cfg, task_stack_base, task_stack_top);
 *       if (p2c_embed_start(&cfg) != 0) return -1;
 *       p2c_embed_run_program(p2c_embed_program);  // --embed-entry で生成
 *       p2c_embed_stop();
 *       return 0;
 *   }
 *
 * p2c_embed_start() は次を一度に行う:
 *   1. 委譲シンク/時計/入力から P2C_Platform を組み立てて登録
 *   2. p2c_runtime_init(heap, heap_size)
 *   3. p2c_gc_init() + p2c_gc_set_stack_bounds()
 *   4. OOMハンドラの登録（既定: 例外フレームがあれば MemoryError、無ければ panic）
 * したがってカーネル側にランタイム初期化の順序知識は不要である。 */

typedef P2C_Object* (*P2C_EmbedProgram)(void);

/* ---------- 組込みヒープ（境界タグ + 空きリスト合体） ----------
 * 組込みフォールバックアロケータ（線形）はfreeを再利用しないため、
 * 長時間走るタスクではメモリが枯渇する。このヒープは
 *   - 呼び出し側が用意した固定領域だけを使い（libc/OS非依存）
 *   - freeしたブロックをアドレス順の空きリストへ戻し、隣接ブロックと合体する
 *   - アラインメント、破損検出、使用量/ピーク/失敗回数の統計を持つ
 * ことで、GCの「解放」が実際にメモリ再利用へつながる。 */
typedef struct P2C_EmbedFree P2C_EmbedFree;

typedef struct {
    unsigned char *base;   /* 16バイト境界へ切り上げ済みの先頭 */
    size_t capacity;       /* 切り上げ後の総容量 */
    size_t used;           /* 現在割当済み（ヘッダ込み） */
    size_t peak;           /* used の最大値 */
    size_t failures;       /* 確保失敗の累計 */
    size_t alloc_calls;
    size_t free_calls;
    P2C_EmbedFree *free_list; /* アドレス昇順の空きブロック */
} P2C_EmbedHeap;

/* raw/storage はカーネルの静的配列でよい（アラインされていなくてよい）。
 * 先頭を内部で16バイト境界へ切り上げる。capacity がヘッダ2個分に満たない
 * 場合は 0 を返す。 */
int p2c_embed_heap_init(P2C_EmbedHeap *heap, void *raw, size_t size);
void *p2c_embed_heap_alloc(P2C_EmbedHeap *heap, size_t size);
void *p2c_embed_heap_realloc(P2C_EmbedHeap *heap, void *ptr, size_t new_size);
void p2c_embed_heap_free(P2C_EmbedHeap *heap, void *ptr);
size_t p2c_embed_heap_block_size(P2C_EmbedHeap *heap, void *ptr);
size_t p2c_embed_heap_free_bytes(P2C_EmbedHeap *heap);
size_t p2c_embed_heap_largest_free(P2C_EmbedHeap *heap);
/* 空きリストと境界の整合性を検査する（テスト/デバッグ用、0=正常）。 */
int p2c_embed_heap_check(P2C_EmbedHeap *heap);

/* ---------- 設定 ---------- */
typedef struct {
    /* 出力シンク。print・例外診断・panic の全てがここへ流れる。 */
    void (*write)(void *user, const char *data, size_t len);
    void *write_user;
    /* 入力（input()）。NULLなら入力なし（常にEOF）。 */
    size_t (*read_line)(char *buf, size_t cap, void *user);
    void *read_user;
    /* 単調増加ミリ秒。NULLなら常に0。 */
    uint64_t (*clock_ms)(void *user);
    void *clock_user;
    /* 回復不能なエラー時に呼ぶ。このフックが復帰した場合、p2c_embed は
     * 無限ループで停止する（カーネルでは halt、テストではlongjmpで復帰）。 */
    void (*panic)(const char *reason, void *user);
    void *panic_user;

    /* ヒープ。heap が NULL なら OS/libc の malloc を使う（Hosted向け）。 */
    P2C_EmbedHeap *heap;
    void *heap_base;
    size_t heap_size;

    /* タスクスタック区間。指定するとGCの保守的スタックスキャンが有効になり、
     * 自動GCが実際に回収する。未指定なら回収しない（安全側の停止）。 */
    void *stack_lo;
    void *stack_hi;

    /* 出力の傍受用コンソール（省略可）。指定すると、シンクへ流れる全出力を
     * ここにも追記する（UART送信と同時に直前のログを検査したい場合に使う）。 */
    char *console;
    size_t console_capacity;

    /* GC。enable_gc=false で自動回収を止める（アリーナ運用）。 */
    bool enable_gc;
    size_t gc_threshold;      /* 0ならランタイム既定(256KiB) */

    /* 確保失敗時の扱い。trueなら try/except へ MemoryError を送出し、
     * 例外フレームが無ければ panic フックへ進む。falseなら従来どおり
     * NULLを返すだけ（呼び出し側が処理する）。 */
    bool raise_memory_error;

    /* 例外処理に使う setjmp/longjmp。NO_STDLIB でカーネルが提供する場合は
     * 何もしなくてよい（runtime.h の契約）。 */
} P2C_EmbedConfig;

void p2c_embed_config_init(P2C_EmbedConfig *config);
void p2c_embed_config_use_console(P2C_EmbedConfig *config, char *buffer, size_t capacity);
void p2c_embed_config_use_uart(P2C_EmbedConfig *config, void (*write)(void *user, const char *data, size_t len), void *user);
void p2c_embed_config_use_heap(P2C_EmbedConfig *config, P2C_EmbedHeap *heap, void *base, size_t size);
void p2c_embed_config_use_stack(P2C_EmbedConfig *config, void *stack_lo, void *stack_hi);

/* ---------- ライフサイクル ---------- */
/* 0=成功。失敗時は config のシンクへ理由を書く（シンクが無ければ無言）。 */
int p2c_embed_start(const P2C_EmbedConfig *config);
/* 変換済みモジュールのエントリを、catch-all 例外フレーム付きで実行する。
 * 捕捉されなかった例外は診断出力してNULLを返す（カーネルを落とさない）。
 * 戻り値はモジュールの実行結果（例外時はNULL）。 */
P2C_Object *p2c_embed_run_program(P2C_EmbedProgram program);
/* ランタイムを停止し、既定プラットフォームへ戻す。再startでタスクを再実行できる。 */
void p2c_embed_stop(void);
/* 回復不能エラー。設定済みシンクへ理由を書き、panicフックを呼ぶ。 */
void p2c_embed_panic(const char *reason);
bool p2c_embed_is_active(void);
const P2C_Platform *p2c_embed_platform(void);

/* ---------- 統計とコンソール ---------- */
typedef struct {
    size_t heap_size;
    size_t heap_used;
    size_t heap_peak;
    size_t heap_free;
    size_t alloc_failures;
    size_t gc_objects;
    size_t gc_collections;
    size_t gc_last_freed;
    bool gc_scan_available;
    bool gc_enabled;
} P2C_EmbedStats;

void p2c_embed_stats(P2C_EmbedStats *out);
const char *p2c_embed_console_text(void);
size_t p2c_embed_console_len(void);
void p2c_embed_console_reset(void);

#ifdef P2C_EMBED_PROVIDE_LIBC_HEAP
/* P2C_EMBED_PROVIDE_LIBC_HEAP を定義すると、malloc/calloc/realloc/free を
 * p2c_embed_heap_set_default() で登録したヒープへ委譲する実装を提供する
 * （malloc等の宣言は common.h が既に提供している）。
 * runtime.h の PYTHON_CODE_TO_C_NO_LIBC_STUBS と併用すると、
 * 「GCが解放したメモリが実際に再利用される」ヒープをカーネルが持てる
 * （線形ヒープのままだと長時間タスクで枯渇する）。
 * 標準ライブラリの malloc と同時にリンクしてはならない。 */
void p2c_embed_heap_set_default(P2C_EmbedHeap *heap);
#endif

#ifdef P2C_EMBED_PROVIDE_PLATFORM_COMPAT
/* P2C_EMBED_PROVIDE_PLATFORM_COMPAT を定義すると、変換済みコードが要求する
 * 互換フック（platform.h の p2c_platform_init/shutdown/write/write_n/
 * read_line/abort）もこのファイルが提供する。カーネルの追加ファイルは
 * embed.c だけで済む（プロトタイプは platform.h が提供済み）。
 * platform_hosted.c・examples/baremetal のボード実装・templates/hobby_os の
 * platform_user.c のいずれかと同時にリンクするとシンボルが重複するため、
 * それらを使う構成では定義しないこと。 */
#endif

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_EMBED_H */
