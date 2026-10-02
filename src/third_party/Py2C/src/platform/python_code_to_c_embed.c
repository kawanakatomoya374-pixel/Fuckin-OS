#include "platform/python_code_to_c_embed.h"

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdlib.h>
#include <string.h>
#endif

/* ============================================================
 * 組込みヒープ: 境界タグ + アドレス順空きリスト + 隣接合体
 * ============================================================
 * libc の malloc/free を使わず、呼び出し側が用意した固定領域だけを扱う。
 * ブロックは [ヘッダ(32B)][ペイロード] の形で並び、ヘッダに全体サイズを持つ。
 * 空きブロックはアドレス昇順の片方向リストで管理し、解放時に前後と合体する。
 * これにより、GCが解放したオブジェクトのメモリが再利用可能になる
 * （組込みフォールバックの線形アロケータは free を再利用しない）。 */

#define P2C_EMBED_ALIGN 16u
#define P2C_EMBED_MAGIC 0x50324348u /* "P2CH" */
#define P2C_EMBED_FLAG_FREE 1u

struct P2C_EmbedFree {
    size_t size;                 /* ブロック全体（ヘッダ含む） */
    unsigned magic;              /* 破損検出 */
    unsigned flags;              /* P2C_EMBED_FLAG_FREE */
    struct P2C_EmbedFree *next;  /* 空きリスト（アドレス昇順、空きブロックのみ） */
};

typedef struct P2C_EmbedFree P2C_EmbedBlock;

#define P2C_EMBED_HEADER ((sizeof(P2C_EmbedBlock) + (P2C_EMBED_ALIGN - 1u)) & ~(size_t)(P2C_EMBED_ALIGN - 1u))

static size_t embed_align_up(size_t value) {
    return (value + (P2C_EMBED_ALIGN - 1u)) & ~(size_t)(P2C_EMBED_ALIGN - 1u);
}

static unsigned char *embed_block_payload(P2C_EmbedBlock *block) {
    return (unsigned char*)block + P2C_EMBED_HEADER;
}

/* 埋め込みヒープ内のブロック境界はヘッダ長がアライン倍数であるため常に整列している。
 * unsigned char* からの直接キャストは型からアライン要件しか読めない
 * -Wcast-align=strict を誤検出させるため、void* を経由して「整列済みの領域を
 * 型付きビューへ戻す」意図を明示する。 */
static P2C_EmbedBlock *embed_payload_block(void *ptr) {
    return (P2C_EmbedBlock*)(void*)((unsigned char*)ptr - P2C_EMBED_HEADER);
}

static bool embed_block_valid(P2C_EmbedHeap *heap, P2C_EmbedBlock *block) {
    if (!heap || !block || block->magic != P2C_EMBED_MAGIC) return false;
    unsigned char *start = (unsigned char*)block;
    if (start < heap->base) return false;
    if (block->size < P2C_EMBED_HEADER + P2C_EMBED_ALIGN) return false;
    if ((size_t)(start - heap->base) > heap->capacity) return false;
    if (block->size > heap->capacity - (size_t)(start - heap->base)) return false;
    return true;
}

/* 空きリストへアドレス昇順で挿入する（合体は呼び出し側で行う）。 */
static void embed_free_list_insert(P2C_EmbedHeap *heap, P2C_EmbedBlock *block) {
    P2C_EmbedBlock *prev = NULL;
    P2C_EmbedBlock *cur = heap->free_list;
    while (cur && (unsigned char*)cur < (unsigned char*)block) {
        prev = cur;
        cur = cur->next;
    }
    block->next = cur;
    if (prev) prev->next = block;
    else heap->free_list = block;
}

int p2c_embed_heap_init(P2C_EmbedHeap *heap, void *raw, size_t size) {
    if (!heap) return -1;
    heap->base = NULL;
    heap->capacity = 0;
    heap->used = 0;
    heap->peak = 0;
    heap->failures = 0;
    heap->alloc_calls = 0;
    heap->free_calls = 0;
    heap->free_list = NULL;
    if (!raw) return -1;
    uintptr_t start = (uintptr_t)raw;
    uintptr_t aligned = (start + (P2C_EMBED_ALIGN - 1u)) & ~(uintptr_t)(P2C_EMBED_ALIGN - 1u);
    size_t skipped = (size_t)(aligned - start);
    if (size <= skipped) return -1;
    size_t usable = size - skipped;
    usable &= ~(size_t)(P2C_EMBED_ALIGN - 1u);
    /* ヘッダ + 最小ペイロード + もう1ブロック分のヘッダが無いと分割できない。 */
    if (usable < 2u * P2C_EMBED_HEADER + P2C_EMBED_ALIGN) return -1;
    heap->base = (unsigned char*)aligned;
    heap->capacity = usable;
    P2C_EmbedBlock *first = (P2C_EmbedBlock*)(void*)heap->base;
    first->size = usable;
    first->magic = P2C_EMBED_MAGIC;
    first->flags = P2C_EMBED_FLAG_FREE;
    first->next = NULL;
    heap->free_list = first;
    return 0;
}

size_t p2c_embed_heap_free_bytes(P2C_EmbedHeap *heap) {
    if (!heap) return 0;
    size_t total = 0;
    for (P2C_EmbedBlock *b = heap->free_list; b; b = b->next) total += b->size;
    return total;
}

size_t p2c_embed_heap_largest_free(P2C_EmbedHeap *heap) {
    size_t best = 0;
    if (!heap) return 0;
    for (P2C_EmbedBlock *b = heap->free_list; b; b = b->next) {
        size_t payload = b->size > P2C_EMBED_HEADER ? b->size - P2C_EMBED_HEADER : 0;
        if (payload > best) best = payload;
    }
    return best;
}

size_t p2c_embed_heap_block_size(P2C_EmbedHeap *heap, void *ptr) {
    if (!ptr) return 0;
    P2C_EmbedBlock *block = embed_payload_block(ptr);
    if (!embed_block_valid(heap, block)) return 0;
    return block->size - P2C_EMBED_HEADER;
}

void *p2c_embed_heap_alloc(P2C_EmbedHeap *heap, size_t size) {
    if (!heap || !heap->base) return NULL;
    heap->alloc_calls++;
    if (size == 0) size = 1;
    /* ヘッダ込みでALIGN境界へ切り上げる。桁あふれは即失敗。 */
    if (size > heap->capacity) { heap->failures++; return NULL; }
    size_t need = embed_align_up(size);
    if (need < size || need > heap->capacity) { heap->failures++; return NULL; }
    need += P2C_EMBED_HEADER;

    P2C_EmbedBlock *prev = NULL;
    P2C_EmbedBlock *block = heap->free_list;
    while (block) {
        if (block->size >= need) break;
        prev = block;
        block = block->next;
    }
    if (!block) { heap->failures++; return NULL; }

    /* 分割: 残りが「ヘッダ + 最小ペイロード」以上あるときだけ切り分ける。 */
    size_t rest = block->size - need;
    if (rest >= P2C_EMBED_HEADER + P2C_EMBED_ALIGN) {
        P2C_EmbedBlock *tail = (P2C_EmbedBlock*)(void*)((unsigned char*)block + need);
        tail->size = rest;
        tail->magic = P2C_EMBED_MAGIC;
        tail->flags = P2C_EMBED_FLAG_FREE;
        tail->next = block->next;
        if (prev) prev->next = tail;
        else heap->free_list = tail;
        block->size = need;
    } else if (prev) {
        prev->next = block->next;
    } else {
        heap->free_list = block->next;
    }
    block->flags = 0;
    block->next = NULL;
    heap->used += block->size;
    if (heap->used > heap->peak) heap->peak = heap->used;
    return embed_block_payload(block);
}

static void embed_heap_release(P2C_EmbedHeap *heap, P2C_EmbedBlock *block) {
    /* 空きリストへ挿入後、メモリ上で隣接する空きブロックと合体する。 */
    block->flags = P2C_EMBED_FLAG_FREE;
    embed_free_list_insert(heap, block);
    /* 直後のブロックと合体 */
    P2C_EmbedBlock *next = block->next;
    if (next && (unsigned char*)block + block->size == (unsigned char*)next) {
        block->size += next->size;
        block->next = next->next;
    }
    /* 直前のブロックと合体 */
    P2C_EmbedBlock *prev = heap->free_list;
    while (prev && prev->next != block) prev = prev->next;
    if (prev && (unsigned char*)prev + prev->size == (unsigned char*)block) {
        prev->size += block->size;
        prev->next = block->next;
    }
}

void p2c_embed_heap_free(P2C_EmbedHeap *heap, void *ptr) {
    if (!heap || !ptr) return;
    heap->free_calls++;
    P2C_EmbedBlock *block = embed_payload_block(ptr);
    if (!embed_block_valid(heap, block)) return; /* 二重解放・破損は無視（安全側） */
    if (block->flags & P2C_EMBED_FLAG_FREE) return; /* 二重解放 */
    if (heap->used >= block->size) heap->used -= block->size;
    else heap->used = 0;
    embed_heap_release(heap, block);
}

void *p2c_embed_heap_realloc(P2C_EmbedHeap *heap, void *ptr, size_t new_size) {
    if (!heap) return NULL;
    if (!ptr) return p2c_embed_heap_alloc(heap, new_size);
    /* libc契約に合わせ、0は1バイトとして扱う（ランタイムの空文字列確保契約）。 */
    if (new_size == 0) new_size = 1;
    P2C_EmbedBlock *block = embed_payload_block(ptr);
    if (!embed_block_valid(heap, block) || (block->flags & P2C_EMBED_FLAG_FREE)) return NULL;
    size_t old_payload = block->size - P2C_EMBED_HEADER;
    if (new_size <= old_payload) {
        size_t need = P2C_EMBED_HEADER + embed_align_up(new_size);
        size_t rest = block->size - need;
        /* 縮小: 余りが独立ブロックとして成立するなら切り離して返す。 */
        if (rest >= P2C_EMBED_HEADER + P2C_EMBED_ALIGN) {
            P2C_EmbedBlock *tail = (P2C_EmbedBlock*)(void*)((unsigned char*)block + need);
            tail->size = rest;
            tail->magic = P2C_EMBED_MAGIC;
            tail->next = NULL;
            heap->used -= rest;
            embed_heap_release(heap, tail);
            block->size = need;
        }
        return ptr;
    }
    /* 拡張: 直後の空きブロックと結合できるならその場で伸ばす。 */
    size_t need = P2C_EMBED_HEADER + embed_align_up(new_size);
    P2C_EmbedBlock *next = (P2C_EmbedBlock*)(void*)((unsigned char*)block + block->size);
    if (embed_block_valid(heap, next) && (next->flags & P2C_EMBED_FLAG_FREE) &&
        block->size + next->size >= need) {
        size_t combined = block->size + next->size;
        /* 空きリストから next を外す */
        P2C_EmbedBlock *prev = heap->free_list;
        while (prev && prev->next != next) prev = prev->next;
        if (prev) prev->next = next->next; else heap->free_list = next->next;
        size_t rest = combined - need;
        block->size = need;
        /* 拡張で増えた分（need - 旧ブロックサイズ）だけを使用量へ加算する。
         * 合体相手は元から空きで used に含まれていないため、rest は加算しない。 */
        heap->used += need - (old_payload + P2C_EMBED_HEADER);
        if (heap->used > heap->peak) heap->peak = heap->used;
        if (rest >= P2C_EMBED_HEADER + P2C_EMBED_ALIGN) {
            P2C_EmbedBlock *tail = (P2C_EmbedBlock*)(void*)((unsigned char*)block + need);
            tail->size = rest;
            tail->magic = P2C_EMBED_MAGIC;
            tail->next = NULL;
            embed_heap_release(heap, tail);
        } else {
            block->size = combined; /* 端数はそのまま保持（次回のreallocで使える） */
            heap->used += combined - need;
            if (heap->used > heap->peak) heap->peak = heap->used;
        }
        return ptr;
    }
    /* 移動: 新規確保してコピーし、元を解放する。 */
    void *fresh = p2c_embed_heap_alloc(heap, new_size);
    if (!fresh) return NULL;
    size_t copy = old_payload < new_size ? old_payload : new_size;
    for (size_t i = 0; i < copy; i++) ((unsigned char*)fresh)[i] = ((unsigned char*)ptr)[i];
    p2c_embed_heap_free(heap, ptr);
    return fresh;
}

int p2c_embed_heap_check(P2C_EmbedHeap *heap) {
    if (!heap || !heap->base) return 1;
    if ((uintptr_t)heap->base % P2C_EMBED_ALIGN != 0) return 2;
    if (heap->capacity < 2u * P2C_EMBED_HEADER + P2C_EMBED_ALIGN) return 3;
    if ((heap->capacity & (P2C_EMBED_ALIGN - 1u)) != 0) return 4;
    uintptr_t prev_end = 0;
    for (P2C_EmbedBlock *b = heap->free_list; b; b = b->next) {
        if (!embed_block_valid(heap, b)) return 5;
        if ((b->flags & P2C_EMBED_FLAG_FREE) == 0) return 6;
        if (((uintptr_t)b & (P2C_EMBED_ALIGN - 1u)) != 0) return 7;
        if (prev_end != 0 && (uintptr_t)b < prev_end) return 8; /* 空き同士の重なり */
        prev_end = (uintptr_t)b + b->size;
    }
    if (prev_end > (uintptr_t)heap->base + heap->capacity) return 9;
    return 0;
}

#ifdef P2C_EMBED_PROVIDE_LIBC_HEAP
/* ランタイム（PYTHON_CODE_TO_C_NO_LIBC_STUBS）の malloc/free をこのヒープへ
 * 委譲する。GCが解放したブロックが再利用可能になる。 */
static P2C_EmbedHeap *g_default_heap = NULL;

void p2c_embed_heap_set_default(P2C_EmbedHeap *heap) { g_default_heap = heap; }

void *malloc(size_t size) { return p2c_embed_heap_alloc(g_default_heap, size); }
void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return NULL;
    size_t total = nmemb * size;
    unsigned char *out = (unsigned char*)p2c_embed_heap_alloc(g_default_heap, total);
    if (out) for (size_t i = 0; i < total; i++) out[i] = 0;
    return out;
}
void *realloc(void *ptr, size_t size) { return p2c_embed_heap_realloc(g_default_heap, ptr, size); }
void free(void *ptr) { p2c_embed_heap_free(g_default_heap, ptr); }
#endif
/* ============================================================
 * 設定
 * ============================================================ */
static const P2C_EmbedConfig *g_config = NULL;
static bool g_embed_active = false;
static char *g_console = NULL;
static size_t g_console_capacity = 0;
static size_t g_console_len = 0;
static const P2C_Platform *g_embed_platform_ptr = NULL;

void p2c_embed_config_init(P2C_EmbedConfig *config) {
    if (!config) return;
    config->write = NULL;
    config->write_user = NULL;
    config->read_line = NULL;
    config->read_user = NULL;
    config->clock_ms = NULL;
    config->clock_user = NULL;
    config->panic = NULL;
    config->panic_user = NULL;
    config->heap = NULL;
    config->heap_base = NULL;
    config->heap_size = 0;
    config->stack_lo = NULL;
    config->stack_hi = NULL;
    config->console = NULL;
    config->console_capacity = 0;
    config->enable_gc = true;
    config->gc_threshold = 0;
    config->raise_memory_error = true;
}

void p2c_embed_config_use_uart(P2C_EmbedConfig *config,
                               void (*write)(void *user, const char *data, size_t len), void *user) {
    if (!config) return;
    config->write = write;
    config->write_user = user;
}

void p2c_embed_config_use_console(P2C_EmbedConfig *config, char *buffer, size_t capacity) {
    if (!config) return;
    config->console = buffer;
    config->console_capacity = capacity;
}

void p2c_embed_config_use_heap(P2C_EmbedConfig *config, P2C_EmbedHeap *heap, void *base, size_t size) {
    if (!config) return;
    config->heap = heap;
    config->heap_base = base;
    config->heap_size = size;
}

void p2c_embed_config_use_stack(P2C_EmbedConfig *config, void *stack_lo, void *stack_hi) {
    if (!config) return;
    config->stack_lo = stack_lo;
    config->stack_hi = stack_hi;
}

/* ============================================================
 * 出力シンク
 * ============================================================ */
static void embed_console_tap(const char *data, size_t len) {
    if (!g_console || g_console_capacity == 0 || !data || len == 0) return;
    if (g_console_len >= g_console_capacity - 1u) return;
    size_t room = g_console_capacity - 1u - g_console_len;
    if (len > room) len = room;
    for (size_t i = 0; i < len; i++) g_console[g_console_len + i] = data[i];
    g_console_len += len;
    g_console[g_console_len] = '\0';
}

/* p2c_platform_write* からの出力は全てここを通る（コンソールへ複製し、
 * 設定されたUARTシンクへ転送する）。 */
static void embed_write(int stream, const char *data, size_t len, void *user) {
    (void)stream;
    (void)user;
    if (!data || len == 0) return;
    embed_console_tap(data, len);
    if (g_config && g_config->write) g_config->write(g_config->write_user, data, len);
}

const char *p2c_embed_console_text(void) { return g_console ? g_console : ""; }
size_t p2c_embed_console_len(void) { return g_console_len; }
void p2c_embed_console_reset(void) {
    g_console_len = 0;
    if (g_console && g_console_capacity > 0) g_console[0] = '\0';
}

/* パニック時に復帰しなかった場合の待機フラグ（常に0。脱出条件として置く）。
 * 静的解析へ「意図的な待機」であることを示すために volatile で参照する。 */
static volatile int g_embed_halt_spin = 0;

void p2c_embed_panic(const char *reason) {
    static const char prefix[] = "PANIC: ";
    const char *text = reason ? reason : "unknown";
    embed_write(2, prefix, sizeof(prefix) - 1u, NULL);
    size_t len = 0;
    while (text[len] != '\0') len++;
    embed_write(2, text, len, NULL);
    embed_write(2, "\n", 1, NULL);
    if (g_config && g_config->panic) g_config->panic(text, g_config->panic_user);
    /* パニックハンドラが復帰した場合の最終手段。カーネルではここで待機する
     * （ホスト側の検証では panic フックから longjmp して復帰させる）。
     * volatile の脱出条件を介することで「意図的な待機」であることを示し、
     * 静的解析の無限ループ検知（CWE-835 / -fanalyzer）を避ける。
     * カーネルが WFI/HLT を差し込みたい場合はこのループを置き換える。 */
    while (!g_embed_halt_spin) { /* spin (halt) */ }
}

/* ============================================================
 * プラットフォーム/ランタイム初期化
 * ============================================================ */
static void *embed_alloc(size_t size, void *user) {
    P2C_EmbedHeap *heap = (P2C_EmbedHeap*)user;
    if (heap) return p2c_embed_heap_alloc(heap, size);
    return malloc(size);
}
static void *embed_realloc(void *ptr, size_t old_size, size_t new_size, void *user) {
    (void)old_size;
    P2C_EmbedHeap *heap = (P2C_EmbedHeap*)user;
    if (heap) return p2c_embed_heap_realloc(heap, ptr, new_size);
    return realloc(ptr, new_size);
}
static void embed_free(void *ptr, size_t size, void *user) {
    (void)size;
    P2C_EmbedHeap *heap = (P2C_EmbedHeap*)user;
    if (heap) p2c_embed_heap_free(heap, ptr);
    else free(ptr);
}
static uint64_t embed_clock(void *user) {
    const P2C_EmbedConfig *config = (const P2C_EmbedConfig*)user;
    if (config && config->clock_ms) return config->clock_ms(config->clock_user);
    return 0;
}

/* プラットフォーム構造体は1つだけ静的に保持する。 */
static P2C_Platform g_embed_platform;

int p2c_embed_start(const P2C_EmbedConfig *config) {
    if (!config) return -1;
    if (g_embed_active) return -2;
    g_config = config;
    g_console = config->console;
    g_console_capacity = config->console_capacity;
    p2c_embed_console_reset();

    if (config->heap && config->heap_base && config->heap_size) {
        if (p2c_embed_heap_init(config->heap, config->heap_base, config->heap_size) != 0) {
            static const char msg[] = "p2c_embed_start: heap region is too small or unusable\n";
            embed_write(2, msg, sizeof(msg) - 1u, NULL);
            g_config = NULL;
            return -3;
        }
#ifdef P2C_EMBED_PROVIDE_LIBC_HEAP
        p2c_embed_heap_set_default(config->heap);
#endif
        /* 適応GCの上限をヒープ容量に合わせる。小さなヒープでしきい値が
         * 伸びすぎると収集が止まり、枯渇してpanic経路へ入ってしまう
         * （例: 32KiB のヒープで 4MiB まで伸ばすと回収が起きない）。 */
        p2c_gc_set_adaptive_limit(config->heap_size / 4u);
    }

    g_embed_platform.alloc = embed_alloc;
    g_embed_platform.realloc = embed_realloc;
    g_embed_platform.free = embed_free;
    g_embed_platform.write = embed_write;
    g_embed_platform.clock_ms = embed_clock;
    g_embed_platform.user = config->heap;
    g_embed_platform_ptr = &g_embed_platform;
    p2c_platform_set(&g_embed_platform);

    p2c_runtime_init(config->heap_base, config->heap_size);
    /* スタック境界のヒントとしてheap_baseを渡す（アドレス自体は未使用）。
     * 実際の境界は次の set_stack_bounds で宣言する。 */
    p2c_gc_init(config->stack_lo);
    if (config->stack_lo && config->stack_hi) p2c_gc_set_stack_bounds(config->stack_lo, config->stack_hi);
    p2c_gc_set_enabled(config->enable_gc);
    if (config->gc_threshold) p2c_gc_set_threshold(config->gc_threshold);
    if (config->raise_memory_error) p2c_runtime_set_oom_handler(p2c_oom_raise_memory_error, NULL);
    g_embed_active = true;
    return 0;
}

P2C_Object *p2c_embed_run_program(P2C_EmbedProgram program) {
    if (!program) return NULL;
    if (!p2c_runtime_is_active()) return NULL;
    P2C_Object *result = NULL;
    /* catch-all 例外フレームをここで張る。変換済みモジュールのエントリが
     * 未処理例外を送出しても、カーネルを落とさず診断だけを残して戻る。 */
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    if (P2C_SETJMP(frame.env) == 0) {
        result = program();
        p2c_exc_stack = frame.prev;
    } else {
        P2C_Object *exc = frame.exc;
        p2c_exc_stack = frame.prev;
        p2c_active_exception = NULL;
        if (exc && exc->cls && exc->cls->type_tag == OBJ_EXCEPTION) {
            embed_write(2, "Uncaught ", 9, NULL);
            embed_write(2, exc->u.v_exception.type_name ? exc->u.v_exception.type_name : "Exception",
                        strlen(exc->u.v_exception.type_name ? exc->u.v_exception.type_name : "Exception"), NULL);
            embed_write(2, ": ", 2, NULL);
            embed_write(2, exc->u.v_exception.msg ? exc->u.v_exception.msg : "", 
                        strlen(exc->u.v_exception.msg ? exc->u.v_exception.msg : ""), NULL);
            embed_write(2, "\n", 1, NULL);
        } else {
            embed_write(2, "Uncaught exception\n", 19, NULL);
        }
        result = NULL;
    }
    return result;
}

void p2c_embed_stop(void) {
    if (!g_embed_active) return;
    p2c_runtime_shutdown();
    p2c_platform_set(NULL);
    g_config = NULL;
    g_console = NULL;
    g_console_capacity = 0;
    g_console_len = 0;
    g_embed_platform_ptr = NULL;
    g_embed_active = false;
}

bool p2c_embed_is_active(void) { return g_embed_active; }

#ifdef P2C_EMBED_PROVIDE_PLATFORM_COMPAT
/* ============================================================
 * 互換フック（platform.h の委譲API）
 * ============================================================
 * これらを定義すると、カーネルが追加で用意するファイルは embed.c だけで済む。
 * 出力・入力は p2c_embed_start() で設定したシンク/入力フックへ転送される。 */
void p2c_platform_init(void) { }
void p2c_platform_shutdown(void) { }

static void embed_write_compat(int stream, const char *data, size_t len) {
    embed_write(stream, data, len, NULL);
}

void p2c_platform_write(const char *s) {
    if (!s) return;
    size_t len = 0;
    while (s[len] != '\0') len++;
    embed_write_compat(1, s, len);
}

void p2c_platform_write_n(const char *s, size_t len) {
    if (!s || len == 0) return;
    embed_write_compat(1, s, len);
}

size_t p2c_platform_read_line(char *buf, size_t cap) {
    if (!buf || cap == 0) return 0;
    buf[0] = '\0';
    if (g_config && g_config->read_line) return g_config->read_line(buf, cap, g_config->read_user);
    return 0;
}

void p2c_platform_abort(const char *reason) {
    p2c_embed_panic(reason);
}
#endif /* P2C_EMBED_PROVIDE_PLATFORM_COMPAT */

const P2C_Platform *p2c_embed_platform(void) { return g_embed_platform_ptr; }

void p2c_embed_stats(P2C_EmbedStats *out) {
    if (!out) return;
    out->heap_size = 0;
    out->heap_used = 0;
    out->heap_peak = 0;
    out->heap_free = 0;
    out->alloc_failures = 0;
    if (g_config && g_config->heap) {
        P2C_EmbedHeap *heap = g_config->heap;
        out->heap_size = heap->capacity;
        out->heap_used = heap->used;
        out->heap_peak = heap->peak;
        out->heap_free = p2c_embed_heap_free_bytes(heap);
        out->alloc_failures = heap->failures;
    } else {
        out->heap_size = p2c_runtime_heap_size();
        out->heap_used = p2c_runtime_heap_used();
        out->heap_peak = p2c_runtime_heap_peak();
        out->alloc_failures = p2c_runtime_alloc_failures();
    }
    out->gc_objects = p2c_gc_object_count();
    out->gc_collections = p2c_gc_collections_run();
    out->gc_last_freed = p2c_gc_last_freed();
    out->gc_scan_available = p2c_gc_stack_scan_available();
    out->gc_enabled = p2c_gc_is_enabled();
}

