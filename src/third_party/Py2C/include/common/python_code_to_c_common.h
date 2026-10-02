#ifndef PYTHON_CODE_TO_C_COMMON_H
#define PYTHON_CODE_TO_C_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef P2C_THREAD_LOCAL
#if defined(PYTHON_CODE_TO_C_NO_TLS)
#define P2C_THREAD_LOCAL
#elif defined(__cplusplus)
#define P2C_THREAD_LOCAL thread_local
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define P2C_THREAD_LOCAL _Thread_local
#else
#define P2C_THREAD_LOCAL
#endif
#endif

/* ベアメタル/組み込み環境向け設定 */
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#else
/* 標準ライブラリなし環境用の最小限の型定義。
 *
 * ただしコンパイラが提供する型定義ヘッダ（stdint.h/stddef.h/stdbool.h）が
 * 使える環境（TinyCCなど）では、そちらを使わないと typedef の再定義が
 * エラーになる（例: tcc の stddef.h は int64_t も定義する）。
 *   - TinyCC (__TINYC__) では既定でコンパイラのヘッダを使う
 *   - 明示的に切り替える場合は PYTHON_CODE_TO_C_USE_COMPILER_TYPES（使う）/
 *     PYTHON_CODE_TO_C_NO_COMPILER_HEADERS（使わない）を定義する
 *   - どちらのヘッダも無いカーネル（-nostdinc 等）は既定（自前定義）のまま */
#if defined(__TINYC__) && !defined(PYTHON_CODE_TO_C_NO_COMPILER_HEADERS)
#  ifndef PYTHON_CODE_TO_C_USE_COMPILER_TYPES
#    define PYTHON_CODE_TO_C_USE_COMPILER_TYPES 1
#  endif
#endif

#ifdef PYTHON_CODE_TO_C_USE_COMPILER_TYPES
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#else
typedef unsigned long size_t;
typedef signed long ptrdiff_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef signed short int16_t;
typedef signed int int32_t;
typedef signed long long int64_t;
typedef unsigned long uintptr_t;
typedef uint8_t bool;
#define true 1
#define false 0
#define NULL ((void*)0)
#endif

/* Allocator ABI supplied by the hosted runtime or target OS adapter. */
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void free(void *ptr);


/* <stdarg.h> はコンパイラ組み込みで、libc/OSに依存せず使えるので
 * NO_STDLIB でも問題なくincludeできる（va_list等はコンパイラが提供する）。 */
#include <stdarg.h>

size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
char *strstr(const char *haystack, const char *needle);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strchr(const char *s, int c);
int atoi(const char *s);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
int isspace(int c);
int isdigit(int c);
int tolower(int c);
int toupper(int c);

/* ここから下は、正しい実装には浮動小数点の書式化・文字列→数値変換が
 * 必要で、簡易実装では丸め誤差や桁数の不一致などpython出力との
 * 不一致を招く危険が大きいため、あえて自前実装を用意していない。
 * NO_STDLIB環境（hobby OS等）へ移植する際は、自OSのlibc/libm相当か、
 * 最小限の代替実装をこちらで用意してリンクしてください。
 *   - strtoll / strtod : int()/float() などでの文字列→数値変換
 *   - floor / fmod / pow / sqrt / sin / cos: //, % 演算子、math モジュール
 *   - snprintf / vsnprintf: 数値→文字列変換（print、str() 等）全般
 * 詳しくは README.md の「hobby OS への持ち込み」を参照してください。 */
long long strtoll(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
double floor(double x);
double ceil(double x);
double trunc(double x);
double fabs(double x);
double fmod(double x, double y);
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double copysign(double x, double y);
double ldexp(double x, int exp);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double erf(double x);
double erfc(double x);
double tgamma(double x);
double lgamma(double x);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
#endif

/* 8バイト境界へ切り上げる。`(n + 7) & ~7` の `~7` はint定数(-8)のため
 * 符号反転を伴う変換になり、-Wsign-conversion の指摘対象になる。
 * size_t型のマスクを明示し、64bit環境でもマスク幅を暗黙に狭めない。
 * ホスト実行時とfreestandingカーネルの両方のアロケータが共有する。 */
#define P2C_ALIGN_UP8(n) (((n) + (size_t)7) & ~(size_t)7)

/* エラーコード */
typedef enum {
    P2C_OK = 0,
    P2C_ERR_NOMEM = -1,
    P2C_ERR_SYNTAX = -2,
    P2C_ERR_SEMANTIC = -3,
    P2C_ERR_IO = -4,
    P2C_ERR_INTERNAL = -5,
    P2C_ERR_NOT_IMPLEMENTED = -6
} P2C_Result;

/* 前方宣言 */
typedef struct P2C_Allocator P2C_Allocator;
typedef struct P2C_String P2C_String;
typedef struct P2C_Vector P2C_Vector;
typedef struct P2C_Map P2C_Map;

/* ========================================
 * メモリアロケータ（ベアメタル対応）
 * ======================================== */

typedef void* (*P2C_AllocFn)(void *ctx, size_t size);
typedef void (*P2C_FreeFn)(void *ctx, void *ptr);
typedef void* (*P2C_ReallocFn)(void *ctx, void *ptr, size_t old_size, size_t new_size);

struct P2C_Allocator {
    void *ctx;
    P2C_AllocFn alloc;
    P2C_FreeFn free;
    P2C_ReallocFn realloc;
};

/* デフォルトアロケータ（stdlib使用時） */
P2C_Allocator* p2c_default_allocator(void);

/* リニアアロケータ（固定バッファ、ベアメタル用） */
P2C_Allocator* p2c_linear_allocator(void *buffer, size_t size);
void p2c_linear_reset(P2C_Allocator *a);

/* プールアロケータ（固定サイズオブジェクト用） */
P2C_Allocator* p2c_pool_allocator(void *buffer, size_t buf_size, size_t obj_size);
void p2c_pool_reset(P2C_Allocator *a);

/* アロケータヘルパ */
#define p2c_alloc(a, sz) ((a) && (a)->alloc ? (a)->alloc((a)->ctx, (sz)) : p2c_default_allocator()->alloc(NULL, (sz)))
/* p2c_free: (a)がNULL(または(a)->freeが無い)場合、p2c_allocと同じ既定
 * アロケータにフォールバックして解放する。以前はここにフォールバックが
 * 無く、p2c_alloc(NULL, ...)（多くの箇所で「とりあえず既定アロケータを
 * 使う」という意味で使われる一般的なパターン）で確保したメモリを
 * p2c_free(NULL, ...)で解放しようとしても実質的に何もしない
 * （何もfreeされない）という、確保側とのつじつまが合わない挙動になって
 * いた。NO_STDLIB（組込み）でも既定アロケータは malloc/free へ委譲する
 * 実装を提供するため、ホストと同じく確保と解放が対になる。 */
#define p2c_free(a, p) do { \
        P2C_Allocator *_p2c_free_alloc = (a) ? (a) : p2c_default_allocator(); \
        if (_p2c_free_alloc && _p2c_free_alloc->free) _p2c_free_alloc->free(_p2c_free_alloc->ctx, (p)); \
    } while(0)
#define p2c_realloc(a, p, osz, nsz) ((a) && (a)->realloc ? (a)->realloc((a)->ctx, (p), (osz), (nsz)) : p2c_default_allocator()->realloc(NULL, (p), (osz), (nsz)))

/* ========================================
 * 共有ヒープ抽象（P2C_Platform アロケータの優先）
 * ========================================
 * 変換器コア（文字列ビルダ・AST・コード生成バッファ）とランタイム
 * （GCオブジェクト・コンテナ）の両方がこの経路でメモリを確保する。
 * 確保元は次のうち「1つ」だけが選ばれる（1つのヒープ契約）。
 *
 *   1. P2C_Platform の alloc/realloc/free（p2c_platform_set[_allocator]）
 *   2. PYTHON_CODE_TO_C_NO_LIBC_STUBS のカーネル malloc 系
 *   3. PYTHON_CODE_TO_C_NO_STDLIB のランタイム同梱スタブ（線形ヒープ）
 *   4. それ以外は libc の malloc 系
 *
 * さらに NO_STDLIB のランタイム同梱スタブが公開する malloc/calloc/realloc/free
 * は、この層（p2c_heap_*）へ委譲する。したがってカーネルが
 * p2c_platform_set_allocator() を設定した構成では、raw な malloc() 呼び出し
 * （python_to_c() の返却バッファ等）を含めてヒープが1つに揃う。
 * Hosted 構成（NO_STDLIB なし）では raw malloc は libc のままなので、
 * python_to_c() の返却バッファは呼び出し側の free() と対にする。
 *
 * プラットフォームの free()/realloc() はサイズを受け取る契約なので、
 * プラットフォーム確保ブロックには 16 バイトのヘッダを前置して
 * 「ペイロード長」と「マジック値」を保持する。解放時はマジック値で
 * プラットフォーム確保かどうかを判定するため、プラットフォーム設定前に
 * malloc で確保したブロック（および設定解除後の確保）が混在していても
 * 正しい解放先を選べる。
 *
 * 注意: これらの関数は「1つのヒープ」を仮定している。同じメモリ領域を
 * 指すアロケータを複数（例: プラットフォームと P2C_Allocator）同時に
 * 注入しないこと。NO_STDLIB のランタイム同梱スタブはこの層へ委譲する
 * ため、プラットフォームと衝突しない。 */
void *p2c_heap_alloc(size_t size);
void *p2c_heap_calloc(size_t nmemb, size_t size);
void *p2c_heap_realloc(void *ptr, size_t new_size);
void p2c_heap_free(void *ptr);
/* 現在のヒープ実体がプラットフォーム提供かどうか（診断用）。 */
bool p2c_heap_uses_platform(void);

/* 確保ブロックのヘッダ（内部契約）。
 *
 * 提供元（プラットフォーム／NO_STDLIB の線形スタブ）によらず同じ形式を
 * 前置するため、p2c_heap_free()/p2c_heap_realloc() は提供元を問わず
 * ペイロード長とマジックを読める。magic は次の2値:
 *
 *   P2C_HEAP_BLOCK_MAGIC        … プラットフォームのアロケータ由来
 *   P2C_HEAP_BLOCK_MAGIC_LINEAR … NO_STDLIB の線形ヒープ由来
 *
 * 線形ヒープはバンプのみで個別解放しないため、後者のブロックは
 * p2c_heap_free() が領域に触れずに戻る（旧サイズは realloc のコピーに使う）。 */
#define P2C_HEAP_BLOCK_MAGIC        (((uint64_t)0x50324348u) | (((uint64_t)0x4C4C4148u) << 32))
#define P2C_HEAP_BLOCK_MAGIC_LINEAR (((uint64_t)0x5032434Cu) | (((uint64_t)0x4E45494Eu) << 32))
typedef struct {
    size_t   size;  /* ペイロードのバイト数（プラットフォーム free/realloc へ渡す） */
    uint64_t magic; /* P2C_HEAP_BLOCK_MAGIC / P2C_HEAP_BLOCK_MAGIC_LINEAR */
} P2C_HeapBlockHeader;

/* 提供元の生確保（内部用）。p2c_heap_* のフォールバックだけが呼ぶ。
 *
 *   - Hosted / NO_LIBC_STUBS … libc またはカーネルの malloc/realloc/free
 *   - NO_STDLIB のスタブ構成 … 線形ヒープ（p2c_heap_free_raw は何もしない）
 *
 * 生確保はブロックヘッダを付けない場合があるため、外部からは直接使わず
 * p2c_heap_* を通すこと。 */
void *p2c_heap_alloc_raw(size_t size);
void *p2c_heap_realloc_raw(void *ptr, size_t new_size);
void p2c_heap_free_raw(void *ptr);

/* 既定アロケータ（P2C_Allocator）の明示注入。
 *
 * カーネル/自作OSが kmalloc/kfree や arena/slab を「変換器コアの既定
 * アロケータ」として使うための入口。ここで注入したアロケータは
 * ランタイムと変換器コア（文字列ビルダ・AST・コード生成バッファ）の
 * 両方で最優先に使われる。
 *
 *   p2c_set_default_allocator(p2c_linear_allocator(arena, size));
 *   p2c_set_default_allocator(NULL);   // 注入を解除して既定へ戻す
 *
 * 共有ヒープ（p2c_heap_*）と同じ領域を指すアロケータを注入しないこと
 * （1つのヒープだけを使う）。プラットフォームのアロケータを使いたい場合は
 * p2c_platform_set() / p2c_platform_set_allocator() を使う。 */
void p2c_set_default_allocator(P2C_Allocator *allocator);

/* 共有ヒープ（p2c_heap_*）が実際に確保を行えるかどうか。
 *
 *   - プラットフォーム（カーネル/embed）が設定済み      → true
 *   - NO_LIBC_STUBS（カーネルが malloc 系を提供）        → true
 *   - ホスト（libc の malloc）                          → true
 *   - NO_STDLIB 既定で p2c_runtime_init() 前            → false
 *
 * 変換器コアは false のとき静的フォールバックヒープを使う。 */
bool p2c_heap_usable(void);

/* NO_STDLIB 既定の線形ヒープが実体を供給できるようになったことを
 * ランタイム（p2c_runtime_init/shutdown）から通知する内部フック。 */
void p2c_heap_note_stub_ready(bool ready);

/* ========================================
 * 動的文字列（String Builder）
 * ======================================== */

struct P2C_String {
    P2C_Allocator *alloc;
    char *data;
    size_t len;
    size_t cap;
};

P2C_String* p2c_str_new(P2C_Allocator *a);
P2C_String* p2c_str_new_from(P2C_Allocator *a, const char *s);
P2C_String* p2c_str_new_from_n(P2C_Allocator *a, const char *s, size_t n);
void p2c_str_free(P2C_String *str);
void p2c_str_clear(P2C_String *str);
P2C_Result p2c_str_append(P2C_String *str, const char *s);
P2C_Result p2c_str_append_n(P2C_String *str, const char *s, size_t n);
P2C_Result p2c_str_append_char(P2C_String *str, char c);
P2C_Result p2c_str_append_fmt(P2C_String *str, const char *fmt, ...);
const char* p2c_str_cstr(P2C_String *str);
size_t p2c_str_len(P2C_String *str);
P2C_String* p2c_str_clone(P2C_String *str);

/* ========================================
 * 動的配列（Vector）
 * ======================================== */

/* 重要: この関数ポインタ型は「アロケータを受け取る2引数の解放関数」
 * （例: p2c_ast_stmt_free(item, allocator)）を想定しています。
 * 以前は1引数のシグネチャで、呼び出し側では(P2C_VectorFreeFn)キャストで
 * p2c_ast_stmt_free（2引数）を渡していたため、実際の呼び出し時に
 * 2つ目の引数（アロケータ）に不定値が渡り、解放処理内でその不定な
 * アロケータ経由の関数ポインタを呼び出してクラッシュする重大なバグが
 * ありました。P2C_Vectorは元々アロケータ(v->alloc)を保持しているため、
 * ここでその値を正しく渡すように修正しています。 */
typedef void (*P2C_VectorFreeFn)(void *item, P2C_Allocator *alloc);

struct P2C_Vector {
    P2C_Allocator *alloc;
    void **data;
    size_t len;
    size_t cap;
    P2C_VectorFreeFn free_fn;
};

P2C_Vector* p2c_vec_new(P2C_Allocator *a, P2C_VectorFreeFn free_fn);
void p2c_vec_free(P2C_Vector *vec);
void p2c_vec_clear(P2C_Vector *vec);
P2C_Result p2c_vec_push(P2C_Vector *vec, void *item);
void* p2c_vec_get(P2C_Vector *vec, size_t idx);
void* p2c_vec_pop(P2C_Vector *vec);
size_t p2c_vec_len(P2C_Vector *vec);
void* p2c_vec_last(P2C_Vector *vec);

/* ========================================
 * ハッシュマップ（シンボルテーブル用）
 * ======================================== */

typedef uint32_t (*P2C_HashFn)(const void *key);
typedef bool (*P2C_KeyEqFn)(const void *a, const void *b);
typedef void (*P2C_MapKeyFreeFn)(void *key);
typedef void (*P2C_MapValFreeFn)(void *val);

typedef struct P2C_MapEntry {
    void *key;
    void *val;
    struct P2C_MapEntry *next;
} P2C_MapEntry;

struct P2C_Map {
    P2C_Allocator *alloc;
    P2C_MapEntry **buckets;
    size_t bucket_count;
    size_t len;
    P2C_HashFn hash_fn;
    P2C_KeyEqFn eq_fn;
    P2C_MapKeyFreeFn key_free;
    P2C_MapValFreeFn val_free;
};

P2C_Map* p2c_map_new(P2C_Allocator *a, P2C_HashFn hash_fn, P2C_KeyEqFn eq_fn);
void p2c_map_free(P2C_Map *map);
P2C_Result p2c_map_insert(P2C_Map *map, void *key, void *val);
void* p2c_map_get(P2C_Map *map, const void *key);
bool p2c_map_remove(P2C_Map *map, const void *key);
size_t p2c_map_len(P2C_Map *map);

/* 文字列キー用ハッシュ関数 */
uint32_t p2c_hash_str(const void *key);
bool p2c_eq_str(const void *a, const void *b);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_COMMON_H */
