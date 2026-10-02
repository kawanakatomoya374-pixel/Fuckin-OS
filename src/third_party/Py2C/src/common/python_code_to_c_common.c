#include "common/python_code_to_c_common.h"
#include "platform/python_code_to_c_platform.h"

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#endif

/* ========================================
 * 標準ライブラリなし環境用の最小限関数
 * ======================================== */

#if defined(PYTHON_CODE_TO_C_NO_STDLIB) && !defined(PYTHON_CODE_TO_C_NO_LIBC_STUBS)
size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    return n ? ((unsigned char)*s1 - (unsigned char)*s2) : 0;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0') { }
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') { }
    return dest;
}

/* C標準の strstr()/strchr() は、const引数から非constポインタを返す契約で
 * 定義されている（引数側にconstが付いていても戻り値はchar*）。この契約上の
 * 不整合を、整数型を1度経由する変換としてここに集約する。直接 (char*) へ
 * キャストすると -Wcast-qual が「const破棄」として指摘する。 */
static char *p2c_std_const_result(const char *s) {
    return (char*)(uintptr_t)s;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return p2c_std_const_result(haystack);
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return p2c_std_const_result(haystack);
    }
    return NULL;
}

/* python_code_to_cのランタイム内では基底クラス名のカンマ区切りリスト("A,B,C")の分割にのみ
 * 使われる（区切り文字は常に1文字）。POSIXのstrtok_rと同じ引数・戻り値の
 * 約束に従う最小実装。 */
char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *s = str ? str : *saveptr;
    if (!s) return NULL;
    while (*s) {
        const char *d = delim; int is_delim = 0;
        while (*d) { if (*s == *d) { is_delim = 1; break; } d++; }
        if (!is_delim) break;
        s++;
    }
    if (!*s) { *saveptr = NULL; return NULL; }
    char *tok_start = s;
    while (*s) {
        const char *d = delim; int is_delim = 0;
        while (*d) { if (*s == *d) { is_delim = 1; break; } d++; }
        if (is_delim) { *s = '\0'; s++; break; }
        s++;
    }
    *saveptr = *s ? s : NULL;
    return tok_start;
}

char *strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return p2c_std_const_result(s); s++; }
    return (c == '\0') ? p2c_std_const_result(s) : NULL;
}

int atoi(const char *s) {
    int sign = 1;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    int result = 0;
    while (*s >= '0' && *s <= '9') { result = result * 10 + (*s - '0'); s++; }
    return sign * result;
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}
#endif

/* ========================================
 * 共有ヒープ抽象（P2C_Platform アロケータの優先）
 * ======================================== */

/* 確保ブロックの形式（ペイロード長 + magic）は common.h の
 * P2C_HeapBlockHeader / P2C_HEAP_BLOCK_MAGIC(_LINEAR) が正本。
 * 提供元（プラットフォーム／NO_STDLIB の線形スタブ）が同じ形式を使うため、
 * この層はブロックの由来を問わずサイズとマジックを読める。 */

/* カーネル/埋め込みホストが明示的に設定したアロケータだけを返す。
 * 既定プラットフォーム（ホストの malloc 委譲、あるいはNO_STDLIBでの
 * 未設定スタブ）は「アロケータ未設定」として扱い、libc/スタブへ委譲する。 */
static const P2C_Platform *p2c_heap_platform(void) {
    const P2C_Platform *plat = p2c_platform_current();
    if (!plat || plat == p2c_platform_default()) return NULL;
    if (!plat->alloc || !plat->realloc || !plat->free) return NULL;
    return plat;
}

bool p2c_heap_uses_platform(void) {
    return p2c_heap_platform() != NULL;
}

/* プラットフォーム確保ブロック（magic付き）が何個生きているか。
 * プラットフォーム解除後に残ったブロックでも正しく解放できるように、
 * 「magic を確認すべきか」の判定に使う。 */
static size_t g_platform_blocks_live = 0;

/* NO_STDLIB 既定（ランタイム同梱の線形ヒープ）が実体を供給できるか。
 * p2c_runtime_init(heap, size) が実際のヒープを受け取ったときに true になる。 */
static bool g_heap_stub_ready = false;

void p2c_heap_note_stub_ready(bool ready) {
    g_heap_stub_ready = ready;
}

bool p2c_heap_usable(void) {
    if (p2c_heap_uses_platform()) return true;
#ifdef PYTHON_CODE_TO_C_NO_LIBC_STUBS
    /* カーネルが malloc 系を提供する。 */
    return true;
#elif defined(PYTHON_CODE_TO_C_NO_STDLIB)
    /* ランタイムの線形ヒープが p2c_runtime_init() で初期化済みかどうか。 */
    return g_heap_stub_ready;
#else
    /* libc の malloc。 */
    return true;
#endif
}

void *p2c_heap_alloc(size_t size) {
    if (size == 0) size = 1;
    const P2C_Platform *plat = p2c_heap_platform();
    /* プラットフォーム未設定なら提供元の生確保へ委譲する
     * （libc / カーネル malloc / NO_STDLIB の線形ヒープ）。 */
    if (!plat) return p2c_heap_alloc_raw(size);
    P2C_HeapBlockHeader *hdr = (P2C_HeapBlockHeader*)plat->alloc(size + sizeof(P2C_HeapBlockHeader), plat->user);
    if (!hdr) return NULL;
    hdr->size  = size;
    hdr->magic = P2C_HEAP_BLOCK_MAGIC;
    g_platform_blocks_live++;
    return (void*)(hdr + 1);
}

void *p2c_heap_calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > ((size_t)-1) / nmemb) return NULL;
    size_t total = nmemb * size;
    void *out = p2c_heap_alloc(total);
    if (out) memset(out, 0, total ? total : 1u);
    return out;
}

void p2c_heap_free(void *ptr) {
    if (!ptr) return;
    const P2C_Platform *plat = p2c_heap_platform();
    /* プラットフォームブロックが1つでも生きている間は magic を確認する。
     * プラットフォームを解除した後に残ったブロック（ランタイムの
     * シャットダウン順序など）でも、ペイロードではなくブロック先頭を
     * 正しく解放するため。純粋な libc ブロックしか存在しない場合は
     * magic を読まない（malloc ブロックの手前を読まないため）。 */
    if (plat || g_platform_blocks_live > 0) {
        P2C_HeapBlockHeader *hdr = ((P2C_HeapBlockHeader*)ptr) - 1;
        if (hdr->magic == P2C_HEAP_BLOCK_MAGIC) {
            hdr->magic = 0;
            if (g_platform_blocks_live > 0) g_platform_blocks_live--;
            if (plat) {
                plat->free(hdr, hdr->size + sizeof(P2C_HeapBlockHeader), plat->user);
                return;
            }
            /* プラットフォーム解除後に残ったブロックは提供元の free へ委譲する。
             * プラットフォームが libc 非互換（kmalloc等）の場合は、解除前に
             * すべて解放してから p2c_platform_set(NULL) を呼ぶこと
             * （標準の停止順序: p2c_embed_stop()/p2c_runtime_shutdown() →
             *   プラットフォーム解除）。NO_STDLIB のスタブ構成では線形ヒープの
             * 解放（何もしない）へ委譲される。 */
            p2c_heap_free_raw(hdr);
            return;
        }
        if (hdr->magic == P2C_HEAP_BLOCK_MAGIC_LINEAR) {
            /* NO_STDLIB の線形ヒープ由来。バンプのみで個別解放しないため
             * 領域には触れずに戻る（プラットフォームの free へ渡してはいけない:
             * 別のヒープのポインタになる）。 */
            hdr->magic = 0;
            return;
        }
        /* プラットフォーム設定前に確保されたブロックはマジックが無い */
    }
    p2c_heap_free_raw(ptr);
}

void *p2c_heap_realloc(void *ptr, size_t new_size) {
    if (!ptr) return p2c_heap_alloc(new_size);
    if (new_size == 0) new_size = 1;
    const P2C_Platform *plat = p2c_heap_platform();
    if (plat || g_platform_blocks_live > 0) {
        P2C_HeapBlockHeader *hdr = ((P2C_HeapBlockHeader*)ptr) - 1;
        if (hdr->magic == P2C_HEAP_BLOCK_MAGIC) {
            size_t old_size = hdr->size;
            if (plat) {
                P2C_HeapBlockHeader *grown = (P2C_HeapBlockHeader*)plat->realloc(
                    hdr, old_size + sizeof(P2C_HeapBlockHeader), new_size + sizeof(P2C_HeapBlockHeader), plat->user);
                if (!grown) return NULL;
                grown->size  = new_size;
                grown->magic = P2C_HEAP_BLOCK_MAGIC;
                return (void*)(grown + 1);
            }
            /* プラットフォーム解除後: 提供元の新しいブロックへ移し替えて、
             * 元のプラットフォームブロックはブロック先頭から解放する
             * （旧ペイロード長はヘッダに記録されている）。 */
            void *moved = p2c_heap_alloc_raw(new_size);
            if (!moved) return NULL;
            memcpy(moved, ptr, old_size < new_size ? old_size : new_size);
            hdr->magic = 0;
            if (g_platform_blocks_live > 0) g_platform_blocks_live--;
            p2c_heap_free_raw(hdr);
            return moved;
        }
        if (hdr->magic == P2C_HEAP_BLOCK_MAGIC_LINEAR) {
            /* NO_STDLIB の線形ヒープ由来。旧ペイロード長はヘッダに記録されて
             * いるため、旧ブロックを越えて読むことはない。 */
            size_t old_size = hdr->size;
            if (!plat) return p2c_heap_realloc_raw(ptr, new_size);
            /* プラットフォームが設定されたので、カーネルのヒープへ移し替える
             * （元の線形ブロックはバンプ領域に残る）。 */
            void *moved = p2c_heap_alloc(new_size);
            if (!moved) return NULL;
            memcpy(moved, ptr, old_size < new_size ? old_size : new_size);
            hdr->magic = 0;
            return moved;
        }
        /* プラットフォーム設定前のブロック: プラットフォーム側へ移し替える。
         * 旧ペイロード長は不明なので、新しいブロックを確保してから
         * 呼び出し側が渡した新サイズぶんコピーする（安全側。元ブロックは
         * free() されずに残る）。 */
        void *migrated = p2c_heap_alloc(new_size);
        if (!migrated) return NULL;
        memcpy(migrated, ptr, new_size);
        return migrated;
    }
    return p2c_heap_realloc_raw(ptr, new_size);
}

/* ========================================
 * 提供元の生確保（p2c_heap_* のフォールバック専用）
 * ========================================
 * Hosted（libc）と、カーネルが malloc 系を提供する構成
 * （PYTHON_CODE_TO_C_NO_LIBC_STUBS）では libc/カーネルへそのまま委譲する。
 * NO_STDLIB のランタイム同梱スタブ構成では runtime.c が線形ヒープ版を
 * 定義する（この翻訳単位では定義しない）。 */
#if !defined(PYTHON_CODE_TO_C_NO_STDLIB) || defined(PYTHON_CODE_TO_C_NO_LIBC_STUBS)
void *p2c_heap_alloc_raw(size_t size) { return malloc(size); }
void *p2c_heap_realloc_raw(void *ptr, size_t new_size) { return realloc(ptr, new_size); }
void p2c_heap_free_raw(void *ptr) { free(ptr); }
#endif

/* ========================================
 * デフォルトアロケータ
 * ======================================== */

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
/* 既定アロケータも共有ヒープ（p2c_heap_*）へ委譲する。プラットフォームが
 * 未設定なら結局 libc の malloc/free/realloc になるため挙動は従来どおり。
 * プラットフォームが設定されている場合は、変換器コア（文字列ビルダ、AST、
 * コード生成バッファ）も同じヒープを使い、ランタイムと確保先が一致する。 */
static void* default_alloc(void *ctx, size_t size) {
    (void)ctx;
    return p2c_heap_alloc(size);
}
static void default_free(void *ctx, void *ptr) {
    (void)ctx;
    p2c_heap_free(ptr);
}
static void* default_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx; (void)old_size;
    return p2c_heap_realloc(ptr, new_size);
}
#else
/* NO_STDLIBでも既定アロケータを提供する。
 *
 * ここで使う malloc/realloc/free は common.h が宣言しており、組込み構成では
 *   - ランタイム同梱の線形ヒープ（PYTHON_CODE_TO_C_NO_STDLIB 既定）
 *   - カーネルのアロケータ（PYTHON_CODE_TO_C_NO_LIBC_STUBS）
 *   - p2c_embed のヒープ（P2C_EMBED_PROVIDE_LIBC_HEAP）
 * のいずれかが実体を供給する。libcヘッダは必要としない。
 *
 * 以前はここでNULLを返していたため、p2c_alloc(NULL, ...)（文字列ビルダ
 * p2c_str_new(NULL)、f-string構築、例外の文字列化、そして変換器コアの
 * 既定アロケータ）が未定義動作になり、最適化された組込みビルドでは
 * GCCが「到達不能」と判断してトラップ命令を生成していた。変換器コアを
 * 自作OS内で動かす（PythonをオンデバイスでCへ変換する）用途はまさに
 * この経路を使うため、既定アロケータはNO_STDLIBでも機能する必要がある。 */
static void* default_alloc_stub(void *ctx, size_t size) {
    (void)ctx;
    return p2c_heap_alloc(size);
}
static void default_free_stub(void *ctx, void *ptr) {
    (void)ctx;
    p2c_heap_free(ptr);
}
static void* default_realloc_stub(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    (void)ctx; (void)old_size;
    return p2c_heap_realloc(ptr, new_size);
}
#endif

static P2C_Allocator default_allocator = {
#ifdef PYTHON_CODE_TO_C_NO_STDLIB
    NULL, default_alloc_stub, default_free_stub, default_realloc_stub
#else
    NULL, default_alloc, default_free, default_realloc
#endif
};

/* 明示注入された既定アロケータ（カーネル/自作OS向け）。NULL なら組み込み既定。 */
static P2C_Allocator *g_injected_default_allocator = NULL;

void p2c_set_default_allocator(P2C_Allocator *allocator) {
    g_injected_default_allocator = allocator;
}

P2C_Allocator* p2c_default_allocator(void) {
    if (g_injected_default_allocator) return g_injected_default_allocator;
    return &default_allocator;
}

/* ========================================
 * リニアアロケータ
 * ======================================== */

typedef struct {
    char *buffer;
    size_t size;
    size_t used;
} LinearCtx;

static void* linear_alloc(void *ctx, size_t size) {
    LinearCtx *lc = (LinearCtx*)ctx;
    if (!lc || !size) return NULL;
    /* アライメント：8バイト境界 */
    size_t aligned = P2C_ALIGN_UP8(size);
    if (lc->used + aligned > lc->size) return NULL;
    void *p = lc->buffer + lc->used;
    lc->used += aligned;
    return p;
}

static void linear_free(void *ctx, void *ptr) {
    /* リニアアロケータは個別解放しない */
    (void)ctx; (void)ptr;
}

static void* linear_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    LinearCtx *lc = (LinearCtx*)ctx;
    if (!lc || !new_size) return NULL;
    /* 末尾のブロックなら拡張可能 */
    if (ptr && (char*)ptr + P2C_ALIGN_UP8(old_size) == lc->buffer + lc->used) {
        size_t aligned_new = P2C_ALIGN_UP8(new_size);
        size_t aligned_old = P2C_ALIGN_UP8(old_size);
        size_t diff = aligned_new - aligned_old;
        if (lc->used + diff <= lc->size) {
            lc->used += diff;
            return ptr;
        }
    }
    /* 新規割り当て */
    void *new_ptr = linear_alloc(ctx, new_size);
    if (new_ptr && ptr && old_size > 0) {
        memmove(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

P2C_Allocator* p2c_linear_allocator(void *buffer, size_t size) {
    if (!buffer || size < sizeof(LinearCtx) + sizeof(P2C_Allocator)) return NULL;
    
    LinearCtx *lc = (LinearCtx*)buffer;
    lc->buffer = (char*)buffer + sizeof(LinearCtx) + sizeof(P2C_Allocator);
    lc->size = size - sizeof(LinearCtx) - sizeof(P2C_Allocator);
    lc->used = 0;
    
    /* bufferはP2C_ALIGN_UP8済みで、sizeof(LinearCtx)もポインタ整列の倍数なので
     * 実体は整列している。char*からの直接キャストは型情報から整列要件しか
     * 読めない-Wcast-align=strictが誤検出するため、void*を経由して意図を明示する。 */
    P2C_Allocator *a = (P2C_Allocator*)(void*)((char*)buffer + sizeof(LinearCtx));
    a->ctx = lc;
    a->alloc = linear_alloc;
    a->free = linear_free;
    a->realloc = linear_realloc;
    
    return a;
}

void p2c_linear_reset(P2C_Allocator *a) {
    if (!a || !a->ctx) return;
    LinearCtx *lc = (LinearCtx*)a->ctx;
    lc->used = 0;
}

/* ========================================
 * プールアロケータ
 * ======================================== */

typedef struct PoolChunk {
    struct PoolChunk *next;
} PoolChunk;

typedef struct {
    size_t obj_size;
    PoolChunk *free_list;
    char *buffer;
    size_t buf_size;
    size_t obj_count;
} PoolCtx;

static void* pool_alloc(void *ctx, size_t size) {
    PoolCtx *pc = (PoolCtx*)ctx;
    (void)size;
    if (!pc) return NULL;
    if (pc->free_list) {
        PoolChunk *c = pc->free_list;
        pc->free_list = c->next;
        return c;
    }
    /* 新規割り当て */
    if ((pc->obj_count + 1) * pc->obj_size > pc->buf_size) return NULL;
    void *p = pc->buffer + pc->obj_count * pc->obj_size;
    pc->obj_count++;
    return p;
}

static void pool_free(void *ctx, void *ptr) {
    PoolCtx *pc = (PoolCtx*)ctx;
    if (!pc || !ptr) return;
    PoolChunk *c = (PoolChunk*)ptr;
    c->next = pc->free_list;
    pc->free_list = c;
}

static void* pool_realloc(void *ctx, void *ptr, size_t old_size, size_t new_size) {
    /* プールアロケータは固定サイズなので、新規割り当て */
    (void)old_size;
    void *new_ptr = pool_alloc(ctx, new_size);
    if (new_ptr && ptr && old_size > 0) {
        memmove(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    return new_ptr;
}

P2C_Allocator* p2c_pool_allocator(void *buffer, size_t buf_size, size_t obj_size) {
    if (!buffer || buf_size < sizeof(PoolCtx) + sizeof(P2C_Allocator) || obj_size < sizeof(void*)) return NULL;
    
    PoolCtx *pc = (PoolCtx*)buffer;
    pc->obj_size = P2C_ALIGN_UP8(obj_size);
    pc->free_list = NULL;
    pc->buffer = (char*)buffer + sizeof(PoolCtx) + sizeof(P2C_Allocator);
    pc->buf_size = buf_size - sizeof(PoolCtx) - sizeof(P2C_Allocator);
    pc->obj_count = 0;
    
    /* 上と同じ理由（pool用の整列済みバッファ、sizeof(PoolCtx)はポインタ整列の倍数）。 */
    P2C_Allocator *a = (P2C_Allocator*)(void*)((char*)buffer + sizeof(PoolCtx));
    a->ctx = pc;
    a->alloc = pool_alloc;
    a->free = pool_free;
    a->realloc = pool_realloc;
    
    return a;
}

void p2c_pool_reset(P2C_Allocator *a) {
    if (!a || !a->ctx) return;
    PoolCtx *pc = (PoolCtx*)a->ctx;
    pc->free_list = NULL;
    pc->obj_count = 0;
}

/* ========================================
 * 動的文字列（String Builder）
 * ======================================== */

#define P2C_STR_INIT_CAP 64

P2C_String* p2c_str_new(P2C_Allocator *a) {
    P2C_String *s = p2c_alloc(a, sizeof(P2C_String));
    if (!s) return NULL;
    s->alloc = a;
    s->len = 0;
    s->cap = P2C_STR_INIT_CAP;
    s->data = p2c_alloc(a, s->cap);
    if (!s->data) { p2c_free(a, s); return NULL; }
    s->data[0] = '\0';
    return s;
}

P2C_String* p2c_str_new_from(P2C_Allocator *a, const char *str) {
    return p2c_str_new_from_n(a, str, strlen(str));
}

P2C_String* p2c_str_new_from_n(P2C_Allocator *a, const char *str, size_t n) {
    P2C_String *s = p2c_str_new(a);
    if (!s) return NULL;
    if (n > 0 && p2c_str_append_n(s, str, n) != P2C_OK) {
        p2c_str_free(s);
        return NULL;
    }
    return s;
}

void p2c_str_free(P2C_String *s) {
    if (!s) return;
    if (s->data) p2c_free(s->alloc, s->data);
    p2c_free(s->alloc, s);
}

void p2c_str_clear(P2C_String *s) {
    if (!s) return;
    s->len = 0;
    if (s->data) s->data[0] = '\0';
}

static P2C_Result p2c_str_ensure(P2C_String *s, size_t need) {
    if (need <= s->cap) return P2C_OK;
    size_t new_cap = s->cap;
    while (new_cap < need) new_cap *= 2;
    char *new_data = p2c_realloc(s->alloc, s->data, s->cap, new_cap);
    if (!new_data) return P2C_ERR_NOMEM;
    s->data = new_data;
    s->cap = new_cap;
    return P2C_OK;
}

P2C_Result p2c_str_append(P2C_String *s, const char *str) {
    return p2c_str_append_n(s, str, strlen(str));
}

P2C_Result p2c_str_append_n(P2C_String *s, const char *str, size_t n) {
    if (!s || !str) return P2C_ERR_INTERNAL;
    if (n == 0) return P2C_OK;
    if (p2c_str_ensure(s, s->len + n + 1) != P2C_OK) return P2C_ERR_NOMEM;
    memcpy(s->data + s->len, str, n);
    s->len += n;
    s->data[s->len] = '\0';
    return P2C_OK;
}

P2C_Result p2c_str_append_char(P2C_String *s, char c) {
    return p2c_str_append_n(s, &c, 1);
}

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
P2C_Result p2c_str_append_fmt(P2C_String *s, const char *fmt, ...) {
    if (!s || !fmt) return P2C_ERR_INTERNAL;
    va_list args, args2;
    va_start(args, fmt);
    va_copy(args2, args);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0) { va_end(args2); return P2C_ERR_INTERNAL; }
    size_t need = (size_t)n + 1;
    if (p2c_str_ensure(s, s->len + need) != P2C_OK) { va_end(args2); return P2C_ERR_NOMEM; }
    vsnprintf(s->data + s->len, need, fmt, args2);
    va_end(args2);
    s->len += (size_t)n;
    return P2C_OK;
}
#else
/* stdlibなし環境では簡易実装 */
P2C_Result p2c_str_append_fmt(P2C_String *s, const char *fmt, ...) {
    /* 簡易版：%s, %d, %u, %x, %c, %p のみ対応 */
    if (!s || !fmt) return P2C_ERR_INTERNAL;
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1)) {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char *v = va_arg(args, const char*);
                    if (v) p2c_str_append(s, v); else p2c_str_append(s, "(null)");
                    break;
                }
                case 'd': {
                    int v = va_arg(args, int);
                    char buf[32];
                    int i = 0, neg = 0;
                    if (v < 0) { neg = 1; v = -v; }
                    do { buf[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
                    if (neg) buf[i++] = '-';
                    /* 反転 */
                    for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t; }
                    buf[i] = '\0';
                    p2c_str_append(s, buf);
                    break;
                }
                case 'u': {
                    unsigned int v = va_arg(args, unsigned int);
                    char buf[32]; int i = 0;
                    do { buf[i++] = (char)('0' + (v % 10)); v /= 10; } while (v);
                    for (int j = 0; j < i / 2; j++) { char t = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = t; }
                    buf[i] = '\0';
                    p2c_str_append(s, buf);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    p2c_str_append_char(s, c);
                    break;
                }
                case '%':
                    p2c_str_append_char(s, '%');
                    break;
                default:
                    p2c_str_append_char(s, '%');
                    p2c_str_append_char(s, *fmt);
                    break;
            }
        } else {
            p2c_str_append_char(s, *fmt);
        }
        fmt++;
    }
    va_end(args);
    return P2C_OK;
}
#endif

const char* p2c_str_cstr(P2C_String *s) {
    return s ? s->data : NULL;
}

size_t p2c_str_len(P2C_String *s) {
    return s ? s->len : 0;
}

P2C_String* p2c_str_clone(P2C_String *s) {
    if (!s) return NULL;
    return p2c_str_new_from_n(s->alloc, s->data, s->len);
}

/* ========================================
 * 動的配列（Vector）
 * ======================================== */

#define P2C_VEC_INIT_CAP 16

P2C_Vector* p2c_vec_new(P2C_Allocator *a, P2C_VectorFreeFn free_fn) {
    P2C_Vector *v = p2c_alloc(a, sizeof(P2C_Vector));
    if (!v) return NULL;
    v->alloc = a;
    v->len = 0;
    v->cap = P2C_VEC_INIT_CAP;
    v->free_fn = free_fn;
    v->data = p2c_alloc(a, sizeof(void*) * v->cap);
    if (!v->data) { p2c_free(a, v); return NULL; }
    return v;
}

void p2c_vec_free(P2C_Vector *v) {
    if (!v) return;
    p2c_vec_clear(v);
    p2c_free(v->alloc, v->data);
    p2c_free(v->alloc, v);
}

void p2c_vec_clear(P2C_Vector *v) {
    if (!v) return;
    if (v->free_fn) {
        for (size_t i = 0; i < v->len; i++) {
            v->free_fn(v->data[i], v->alloc);
        }
    }
    v->len = 0;
}

P2C_Result p2c_vec_push(P2C_Vector *v, void *item) {
    if (!v) return P2C_ERR_INTERNAL;
    if (v->len >= v->cap) {
        size_t new_cap = v->cap * 2;
        void **new_data = p2c_realloc(v->alloc, v->data, sizeof(void*) * v->cap, sizeof(void*) * new_cap);
        if (!new_data) return P2C_ERR_NOMEM;
        v->data = new_data;
        v->cap = new_cap;
    }
    v->data[v->len++] = item;
    return P2C_OK;
}

void* p2c_vec_get(P2C_Vector *v, size_t idx) {
    if (!v || idx >= v->len) return NULL;
    return v->data[idx];
}

void* p2c_vec_pop(P2C_Vector *v) {
    if (!v || v->len == 0) return NULL;
    return v->data[--v->len];
}

size_t p2c_vec_len(P2C_Vector *v) {
    return v ? v->len : 0;
}

void* p2c_vec_last(P2C_Vector *v) {
    if (!v || v->len == 0) return NULL;
    return v->data[v->len - 1];
}

/* ========================================
 * ハッシュマップ
 * ======================================== */

#define P2C_MAP_INIT_BUCKETS 32

static P2C_MapEntry* p2c_map_entry_new(P2C_Allocator *a, void *key, void *val) {
    P2C_MapEntry *e = p2c_alloc(a, sizeof(P2C_MapEntry));
    if (!e) return NULL;
    e->key = key;
    e->val = val;
    e->next = NULL;
    return e;
}

P2C_Map* p2c_map_new(P2C_Allocator *a, P2C_HashFn hash_fn, P2C_KeyEqFn eq_fn) {
    if (!hash_fn || !eq_fn) return NULL;
    P2C_Map *m = p2c_alloc(a, sizeof(P2C_Map));
    if (!m) return NULL;
    m->alloc = a;
    m->hash_fn = hash_fn;
    m->eq_fn = eq_fn;
    m->key_free = NULL;
    m->val_free = NULL;
    m->bucket_count = P2C_MAP_INIT_BUCKETS;
    m->len = 0;
    m->buckets = p2c_alloc(a, sizeof(P2C_MapEntry*) * m->bucket_count);
    if (!m->buckets) { p2c_free(a, m); return NULL; }
    memset(m->buckets, 0, sizeof(P2C_MapEntry*) * m->bucket_count);
    return m;
}

void p2c_map_free(P2C_Map *m) {
    if (!m) return;
    for (size_t i = 0; i < m->bucket_count; i++) {
        P2C_MapEntry *e = m->buckets[i];
        while (e) {
            P2C_MapEntry *next = e->next;
            if (m->key_free) m->key_free(e->key);
            if (m->val_free) m->val_free(e->val);
            p2c_free(m->alloc, e);
            e = next;
        }
    }
    p2c_free(m->alloc, m->buckets);
    p2c_free(m->alloc, m);
}

P2C_Result p2c_map_insert(P2C_Map *m, void *key, void *val) {
    if (!m) return P2C_ERR_INTERNAL;
    size_t h = m->hash_fn(key) % m->bucket_count;
    P2C_MapEntry *e = m->buckets[h];
    while (e) {
        if (m->eq_fn(e->key, key)) {
            /* 上書き */
            if (m->val_free) m->val_free(e->val);
            e->val = val;
            return P2C_OK;
        }
        e = e->next;
    }
    /* 新規挿入 */
    P2C_MapEntry *new_e = p2c_map_entry_new(m->alloc, key, val);
    if (!new_e) return P2C_ERR_NOMEM;
    new_e->next = m->buckets[h];
    m->buckets[h] = new_e;
    m->len++;
    return P2C_OK;
}

void* p2c_map_get(P2C_Map *m, const void *key) {
    if (!m) return NULL;
    size_t h = m->hash_fn(key) % m->bucket_count;
    P2C_MapEntry *e = m->buckets[h];
    while (e) {
        if (m->eq_fn(e->key, key)) return e->val;
        e = e->next;
    }
    return NULL;
}

bool p2c_map_remove(P2C_Map *m, const void *key) {
    if (!m) return false;
    size_t h = m->hash_fn(key) % m->bucket_count;
    P2C_MapEntry *e = m->buckets[h];
    P2C_MapEntry *prev = NULL;
    while (e) {
        if (m->eq_fn(e->key, key)) {
            if (prev) prev->next = e->next;
            else m->buckets[h] = e->next;
            if (m->key_free) m->key_free(e->key);
            if (m->val_free) m->val_free(e->val);
            p2c_free(m->alloc, e);
            m->len--;
            return true;
        }
        prev = e;
        e = e->next;
    }
    return false;
}

size_t p2c_map_len(P2C_Map *m) {
    return m ? m->len : 0;
}

/* ---- 文字列キー用 ---- */

uint32_t p2c_hash_str(const void *key) {
    const char *s = (const char*)key;
    uint32_t h = 5381;
    while (*s) {
        h = ((h << 5) + h) + (unsigned char)*s++;
    }
    return h;
}

bool p2c_eq_str(const void *a, const void *b) {
    if (!a || !b) return false;
    return strcmp((const char*)a, (const char*)b) == 0;
}
