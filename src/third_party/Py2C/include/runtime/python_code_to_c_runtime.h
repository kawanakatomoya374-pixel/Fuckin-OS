#ifndef PYTHON_CODE_TO_C_RUNTIME_H
#define PYTHON_CODE_TO_C_RUNTIME_H

#include "common/python_code_to_c_common.h"
#include "platform/python_code_to_c_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * Portable runtime core
 * ======================================== */

typedef enum {
    OBJ_NONE,
    OBJ_BOOL,
    OBJ_INT,
    OBJ_FLOAT,
    OBJ_STR,
    OBJ_LIST,
    OBJ_DICT,
    OBJ_SET,
    OBJ_TUPLE,
    OBJ_FUNCTION,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_MODULE,
    OBJ_EXCEPTION,
    OBJ_ITERATOR,  /* iter()/next() が返す反復子オブジェクト */
    OBJ_CELL,      /* closureが共有する可変束縛セル */
    OBJ_ELLIPSIS,  /* Pythonの単一値 ... (Ellipsis)。リーフ型。 */
    OBJ_RANGE      /* range(start, stop, step)。要素を作らない遅延オブジェクト。 */
} P2C_ObjType;

typedef struct P2C_Object P2C_Object;
typedef P2C_Object* (*P2C_CallableFn)(P2C_Object **args, size_t nargs);
typedef P2C_Object* (*P2C_ClosureFn)(P2C_Object *env, P2C_Object **args, size_t nargs);
typedef P2C_Object* (*P2C_GeneratorStepFn)(P2C_Object *generator);
typedef P2C_Object* (*P2C_BinaryOpFn)(P2C_Object *left, P2C_Object *right);

typedef void (*P2C_ObjFreeFn)(P2C_Object *obj);
typedef P2C_Object* (*P2C_ObjStrFn)(P2C_Object *obj);

/* GC (ガベージコレクタ) がオブジェクトグラフを辿るためのコールバック。
 * list/dict/tuple/instance/class/moduleのように他のP2C_Object*を保持しうる
 * 型は、自分が直接指しているP2C_Object*それぞれについて visit(child, ctx) を
 * 呼び出す gc_traverse を用意する。int/float/str/exceptionのように他の
 * P2C_Object*を保持しない「葉」の型はNULLのままでよい（何も辿らない）。 */
typedef void (*P2C_GcVisitFn)(P2C_Object *child, void *ctx);
typedef void (*P2C_ObjGcTraverseFn)(P2C_Object *obj, P2C_GcVisitFn visit, void *ctx);

typedef struct P2C_ClassDef {
    const char *name;
    P2C_ObjType type_tag;
    P2C_ObjFreeFn free_fn;
    P2C_ObjStrFn str_fn;
    struct P2C_MethodDef *methods;
    struct P2C_ClassDef *base;
    /* 末尾に追加: 既存の位置指定初期化子 {"str", OBJ_STR, free_none, str_none,
     * no_methods, NULL} 等の並びを崩さないため、新しいフィールドは必ず
     * 構造体の最後に足す（Cは省略された末尾メンバをNULL/0で埋めるため、
     * gc_traverseを書いていない既存の初期化子はそのままNULL=葉型として
     * 動作し続ける）。 */
    P2C_ObjGcTraverseFn gc_traverse;
} P2C_ClassDef;

typedef P2C_Object* (*P2C_MethodKwFn)(P2C_Object *self, P2C_Object **args, size_t nargs,
                                      const char **kw_names, P2C_Object **kw_values, size_t nkw);

/* メソッドの種類。インスタンスメソッドは先頭引数に self（インスタンス）を、
 * staticmethodは引数を受け取らず（selfを渡さない）、classmethodは先頭引数に
 * クラスオブジェクトを受け取り、propertyは属性読み出し時に自己を束縛して
 * 呼び出されるゲッターとして扱う。
 * P2C_MethodDef の末尾メンバなので、既存の位置指定初期化子
 * {"name", func, kwfunc} は自動的に P2C_METHOD_INSTANCE になる。 */
enum {
    P2C_METHOD_INSTANCE = 0,
    P2C_METHOD_STATIC = 1,
    P2C_METHOD_CLASS = 2,
    P2C_METHOD_PROPERTY = 3,
    /* @x.setter（プロパティ x の setter）。名前はゲッターと同じで、
     * 種別で区別する（名前引きはゲッターを優先する）。 */
    P2C_METHOD_PROPERTY_SETTER = 4
};

typedef struct P2C_MethodDef {
    const char *name;
    P2C_Object* (*func)(P2C_Object *self, P2C_Object **args, size_t nargs);
    /* NULLならキーワード引数は受理しない既存メソッド。 */
    P2C_MethodKwFn kwfunc;
    /* P2C_METHOD_* のいずれか（末尾に追加したフィールド）。 */
    int kind;
} P2C_MethodDef;

typedef struct P2C_DictEntry {
    P2C_Object *key;
    P2C_Object *val;
    struct P2C_DictEntry *next;
    struct P2C_DictEntry *order_next;
} P2C_DictEntry;

struct P2C_Object {
    P2C_ClassDef *cls;
    int refcount;
    union {
        bool v_bool;
        int64_t v_int;
        double v_float;
        struct { char *data; size_t len; } v_str;
        struct { int64_t start; int64_t stop; int64_t step; } v_range;
        struct { P2C_Object **items; size_t len; size_t cap; } v_list;
        struct { P2C_DictEntry **buckets; P2C_DictEntry *order_head; P2C_DictEntry *order_tail; size_t bucket_count; size_t len; } v_dict;
        struct { P2C_Object **items; size_t len; } v_tuple;
        struct { char *name; P2C_CallableFn func; P2C_ClosureFn closure_func; P2C_Object *env; } v_function;
        /* mro: クラス階層のC3線形化キャッシュ（自分以外の基底クラス名を解決順に
         * カンマで連結した文字列）。初回のメソッド・属性解決時に遅延計算する。
         * 継承関係はbase_nameの名前文字列で保持しているが、多重継承のダイヤモンド
         * （class D(B, C), B(A), C(A)）でPythonと同じ解決順にするにはC3線形化が
         * 必要になるため、その結果をここへ保持する。 */
        struct { char *name; P2C_CallableFn ctor; P2C_MethodDef *methods; P2C_Map *attrs; char *base_name; char *mro; } v_class;
        struct { P2C_Object *klass; P2C_Map *attrs; } v_instance;
        struct { char *name; P2C_Map *attrs; } v_module;
        struct { char *msg; char *type_name; P2C_Object *cause; } v_exception;
        struct { P2C_Object *value; } v_cell;
        /* iter()/next() 用。sequenceベース（list/tuple/str/dict や
         * __len__+__getitem__ を実装するインスタンス）を反復する場合は
         * seq+posを使う。__iter__が返したカスタムオブジェクト（自前で
         * __next__ を実装するインスタンス）をラップする場合はcustomを使う。 */
        struct {
            P2C_Object *seq;
            int64_t pos;
            P2C_Object *custom;
            P2C_GeneratorStepFn step;
            P2C_Object *locals;
            P2C_Object *awaiting;
            P2C_Object *result;
            P2C_Object *exception;
            uint32_t state;
            bool is_coroutine;
            bool running;
            bool done;
            bool yielded;
            bool queued;
        } v_iterator;
    } u;
    /* GC用の内部管理フィールド（末尾に追加、理由はP2C_ClassDefのgc_traverseと同様）。
     * ユーザーコードやcodegenが直接読み書きするものではない。 */
    P2C_Object *gc_next;   /* GCが追跡する全オブジェクトの侵入型連結リスト */
    uint8_t gc_marked;     /* mark-and-sweepのmarkフェーズで使うビット */
};

extern P2C_ClassDef P2C_Class_NoneType;
extern P2C_ClassDef P2C_Class_Bool;
extern P2C_ClassDef P2C_Class_Int;
extern P2C_ClassDef P2C_Class_Float;
extern P2C_ClassDef P2C_Class_Str;
extern P2C_ClassDef P2C_Class_List;
extern P2C_ClassDef P2C_Class_Dict;
extern P2C_ClassDef P2C_Class_Set;
extern P2C_ClassDef P2C_Class_Tuple;
extern P2C_ClassDef P2C_Class_Function;
extern P2C_ClassDef P2C_Class_ClassObject;
extern P2C_ClassDef P2C_Class_Instance;
extern P2C_ClassDef P2C_Class_Module;
extern P2C_ClassDef P2C_Class_Exception;
extern P2C_ClassDef P2C_Class_Iterator;
extern P2C_ClassDef P2C_Class_Cell;
extern P2C_ClassDef P2C_Class_Ellipsis;

extern P2C_Object P2C_None;
extern P2C_Object P2C_True;
extern P2C_Object P2C_False;
/* Pythonの単一値 `...`。式のプレースホルダやスタブ本体（def f(): ...）に使う。 */
extern P2C_Object P2C_Ellipsis;

P2C_Object* p2c_obj_new(P2C_ClassDef *cls);
/* p2c_obj_incref/decref: 参照カウンタの増減のみを行う（解放は行わない）。
 * オブジェクトの実際の解放はp2c_gc_collect()が担う（下記GCセクション参照）。
 * 生成コード自身がこれらを呼ぶ必要はない。ホスト側のC/組み込みコード
 * （pygame等のネイティブモジュール実装や、ランタイムを直接叩く埋め込み用途）
 * が、P2C_Objectグラフの外側（Cのグローバル変数やヒープ上の自前構造体等、
 * GCがスキャンできない場所）に一時的にP2C_Object*を保持する必要がある場合に
 * p2c_obj_incref()で「ピン留め」しておくと、GCはrefcount > 0のオブジェクトを
 * 追加のルートとして扱い回収しない。使い終わったらp2c_obj_decref()で解除する。 */
void p2c_obj_incref(P2C_Object *obj);
void p2c_obj_decref(P2C_Object *obj);

P2C_Object* p2c_obj_from_bool(bool v);
P2C_Object* p2c_obj_from_int(int64_t v);
P2C_Object* p2c_obj_from_float(double v);
P2C_Object* p2c_obj_from_str(const char *s);
P2C_Object* p2c_obj_from_str_n(const char *s, size_t len);
#define p2c_obj_is_none(o) ((o) == &P2C_None || (o) == NULL)
P2C_Object* p2c_str_format_py(const char *fmt, P2C_Object **args, size_t nargs);

bool p2c_obj_is_int(P2C_Object *obj);
bool p2c_obj_is_float(P2C_Object *obj);
bool p2c_obj_is_str(P2C_Object *obj);
bool p2c_obj_is_list(P2C_Object *obj);
bool p2c_obj_is_dict(P2C_Object *obj);
bool p2c_obj_is_set(P2C_Object *obj);
bool p2c_obj_is_bool(P2C_Object *obj);
bool p2c_obj_is_tuple(P2C_Object *obj);
bool p2c_obj_is_truthy(P2C_Object *obj);

int64_t p2c_obj_as_int(P2C_Object *obj);
double p2c_obj_as_float(P2C_Object *obj);
const char* p2c_obj_as_str(P2C_Object *obj);

P2C_Object* p2c_obj_add(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_sub(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_mul(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_div(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_floordiv(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_mod(P2C_Object *a, P2C_Object *b);
/* Python の str % 書式（"%s(%.2f)" % (name, x)）。p2c_obj_mod から呼ばれる。 */
P2C_Object* p2c_percent_format(const char *fmt, P2C_Object *rhs);
P2C_Object* p2c_obj_pow(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_lshift(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_rshift(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_bitand(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_bitor(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_bitxor(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_invert(P2C_Object *a);

P2C_Object* p2c_obj_eq(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_ne(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_lt(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_le(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_gt(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_obj_ge(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_bool_and(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_bool_or(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_bool_not(P2C_Object *a);
P2C_Object* p2c_obj_is(P2C_Object *container, P2C_Object *item);
P2C_Object* p2c_obj_contains(P2C_Object *container, P2C_Object *item);

P2C_Object* p2c_list_new(void);
P2C_Object* p2c_list_from_array(P2C_Object **items, size_t len);
void p2c_list_append(P2C_Object *list, P2C_Object *item);
P2C_Object* p2c_list_get(P2C_Object *list, size_t idx);
void p2c_list_set(P2C_Object *list, size_t idx, P2C_Object *val);
void p2c_list_delete_at(P2C_Object *list, int64_t idx); /* del list[idx] */
size_t p2c_list_len(P2C_Object *list);
P2C_Object* p2c_list_pop(P2C_Object *list);

P2C_Object* p2c_dict_new(void);
P2C_Object* p2c_set_new(void);
P2C_Object* p2c_set_from_array(P2C_Object **items, size_t len);
void p2c_set_add(P2C_Object *set, P2C_Object *item);
P2C_Object* p2c_dict_from_pairs(P2C_Object **keys, P2C_Object **vals, size_t len);
P2C_Object* p2c_dict_fromkeys(P2C_Object *iterable, P2C_Object *value);
void p2c_dict_set(P2C_Object *dict, P2C_Object *key, P2C_Object *val);
void p2c_dict_update(P2C_Object *dict, P2C_Object *mapping);
void p2c_dict_remove(P2C_Object *dict, P2C_Object *key); /* del dict[key] */
P2C_Object* p2c_dict_exclude_keys(P2C_Object *src, const char **exclude, size_t n_exclude);
P2C_Object* p2c_dict_get(P2C_Object *dict, P2C_Object *key);
P2C_Object* p2c_dict_get_with_default(P2C_Object *dict, P2C_Object *key, P2C_Object *default_val);
size_t p2c_dict_len(P2C_Object *dict);

P2C_Object* p2c_tuple_new(size_t len);
P2C_Object* p2c_tuple_from_array(P2C_Object **items, size_t len);
void p2c_tuple_set(P2C_Object *tuple, size_t idx, P2C_Object *val);
P2C_Object* p2c_tuple_get(P2C_Object *tuple, size_t idx);
size_t p2c_tuple_len(P2C_Object *tuple);

P2C_Object* p2c_str_concat(P2C_Object *a, P2C_Object *b);
size_t p2c_obj_str_len(P2C_Object *s);
P2C_Object* p2c_str_format(const char *fmt, ...);

P2C_Object* p2c_function_new(const char *name, P2C_CallableFn func);
P2C_Object* p2c_builtin_function_object(const char *name);
P2C_Object* p2c_closure_new(const char *name, P2C_ClosureFn func, P2C_Object *env);
P2C_Object* p2c_cell_new(P2C_Object *value);
P2C_Object* p2c_cell_get(P2C_Object *cell);
void p2c_cell_set(P2C_Object *cell, P2C_Object *value);
P2C_Object* p2c_class_new(const char *name, P2C_CallableFn ctor, P2C_MethodDef *methods, const char *base_name);
P2C_Object* p2c_instance_new(P2C_Object *klass);
P2C_Object* p2c_module_new(const char *name);
void p2c_module_set_attr(P2C_Object *module, const char *name, P2C_Object *val);
P2C_Object* p2c_import_module(const char *name);
void p2c_register_module(P2C_Object *module);
P2C_Object* p2c_call(P2C_Object *callable, P2C_Object **args, size_t nargs);
P2C_Object* p2c_call_attr(P2C_Object *obj, const char *name, P2C_Object **args, size_t nargs);
/* super().method(...) の動的解決（selfの型のMRO上で、defining_classの次から探索する）。
 * コード生成は super().m(...) をこの呼び出しへlowerする。 */
P2C_Object* p2c_super_call_attr(P2C_Object *self, P2C_Object *defining_class, const char *name, P2C_Object **args, size_t nargs);
P2C_Object* p2c_call_attr_kw(P2C_Object *obj, const char *name, P2C_Object **args, size_t nargs,
                              const char **kw_names, P2C_Object **kw_values, size_t nkw);

/* ランタイムを初期化する（1プロセス1回。再初期化は p2c_runtime_shutdown() の後）。
 *
 * heap_base/heap_size は PYTHON_CODE_TO_C_NO_STDLIB のランタイム同梱スタブ
 * （線形ヒープ）が使う領域。ただし共有ヒープ（p2c_heap_*）の実体は次の順で
 * 1つだけ選ばれるため、
 *
 *   - p2c_platform_set_allocator()/p2c_platform_set() でカーネルのアロケータを
 *     注入済み → この引数は使われない（カーネルのヒープが唯一のヒープ。
 *     NULL, 0 を渡してよい）
 *   - PYTHON_CODE_TO_C_NO_LIBC_STUBS → 引数は無視される（カーネルの malloc 系）
 *   - それ以外（Hosted）→ 引数は無視される（libc の malloc 系）
 *
 * p2c_runtime_shutdown() はモジュール/クラスレジストリを reset して再利用する
 * （少数のブロックはヒープに残る。リーク検査は test-gc-leaks が担当する）。 */
void p2c_runtime_init(void *heap_base, size_t heap_size);
void p2c_runtime_shutdown(void);
bool p2c_runtime_is_active(void);
/* 現在のランタイムコンテキストに登録されているクラス数（観測用）。
 * p2c_runtime_shutdown() で 0 に戻る（再初期化の検証に使う）。 */
size_t p2c_runtime_class_count(void);
void* p2c_runtime_alloc(size_t size);
void* p2c_runtime_realloc(void *ptr, size_t old_size, size_t new_size);
void p2c_runtime_free(void *ptr);

/* ========================================
 * 組み込みホスト向けのヒープ統計と確保失敗通知
 * ======================================== */
/* 以下3つは、組込みフォールバックアロケータ（PYTHON_CODE_TO_C_NO_STDLIB で
 * libcスタブを同梱している場合の線形ヒープ）の実測値。
 * カーネルが自前の malloc を提供する構成や、Hosted(malloc)構成では
 * heap_size/peak は0を返す（OSヒープの使用量はここでは追跡しない）。 */
size_t p2c_runtime_heap_size(void);
size_t p2c_runtime_heap_used(void);
size_t p2c_runtime_heap_peak(void);
/* 組込みフォールバックアロケータがNULLを返した累計回数。
 * ヒープ枯渇をカーネルのログ/メトリクスへ出すために使う。 */
size_t p2c_runtime_alloc_failures(void);

/* メモリ確保に失敗したとき（NULLを返す直前）に呼ばれるフック。
 * context は "p2c_obj_new" のような確保主体の短い名前で、静的領域を指す。
 * ハンドラは何もせず戻ってもよい（呼び出し側は従来どおりNULLを受け取る）が、
 * 例外フレームが有効なら MemoryError を送出して Python の except へ
 * 制御を移すこともできる。標準実装が p2c_oom_raise_memory_error()。 */
typedef void (*P2C_OomHandler)(size_t requested, const char *context, void *user);
void p2c_runtime_set_oom_handler(P2C_OomHandler handler, void *user);
/* 現在のハンドラを呼ぶ。ハンドラ未設定なら何もしない。 */
void p2c_runtime_notify_oom(size_t requested, const char *context);
/* すぐに使える標準OOMハンドラ:
 *   - 例外フレーム(p2c_exc_stack)が有効なら MemoryError を送出する
 *   - そうでなければ確保サイズと主体を診断出力し、p2c_platform_abort() へ進む
 * カーネルはこれを set するか、自前の「ログして停止」ハンドラを渡す。 */
void p2c_oom_raise_memory_error(size_t requested, const char *context, void *user);


#ifndef P2C_GC_ROOT_CAPACITY
#define P2C_GC_ROOT_CAPACITY 1024
#endif

/* ========================================
 * GC (トレーシング・ガベージコレクタ)
 * ========================================
 * p2c_obj_new()で確保されたP2C_Objectを自動的に回収する、stop-the-world
 * mark-and-sweep方式のGC。ルート（生きているオブジェクトの起点）は次の
 * 組み合わせで見つける:
 *   1. p2c_gc_register_root()で明示的に登録されたスロット
 *      （生成コードのモジュールレベル変数・クラスオブジェクト等、
 *      Cのファイルスコープ変数として存在するもの）
 *   2. ランタイム内部の永続レジストリ（import済みモジュール、クラス名
 *      レジストリ）
 *   3. 現在のCコールスタック全体の保守的（conservative）スキャン
 *      （ローカル変数・関数引数・一時オブジェクト・例外フレーム等、
 *      名前を付けて個別登録していないものはすべてここでカバーされる）
 * 3.のおかげで、codegenは通常の代入・関数呼び出し・ループ・comprehension・
 * with/try文などについて一切GC専用のコードを生成する必要がない。 */

/* 生成コードの main() 先頭、または埋め込みホストの初期化で呼ぶ。
 * Linux では pthread_getattr_np() によりOSスタック境界を自動取得する。
 * それ以外の環境（自作OS/ベアメタル）では stack_hint を「スタック上端の
 * 実アドレス」として扱い、収集時に「現在のフレームから hint まで」を
 * 保守的に走査する。hint に NULL を渡した場合だけ境界不明となり、
 * p2c_gc_set_stack_bounds() が呼ばれるまで自動GCは安全側に停止する。 */
void p2c_gc_init(void *stack_hint);

/* カーネル/埋め込みホストが明示的にスタック境界を宣言する。
 * stack_lo / stack_hi はタスクのスタック区間の両端（順序は不問、NULL不可）。
 * スタックが下方成長か上方成長かは仮定しない。実際の走査は
 * 「現在の関数フレーム（使用中のスタック）から区間の上端まで」に限定される
 * ため、8MiB のような大きな区間を宣言しても収集コストは使用量に比例する。 */
void p2c_gc_set_stack_bounds(void *stack_lo, void *stack_hi);

/* 保守的スタックスキャンが可能か（=自動GCが安全に実行できるか）。
 * false の間、p2c_gc_collect() としきい値による自動収集は何もしない。
 * スタックを走査できないのに回収すると、ローカル変数からしか到達できない
 * 生きたオブジェクトを解放してしまうため、これを「安全側の停止」とする。
 * p2c_gc_init() にスタック上のアドレスを渡した場合は true になる。 */
bool p2c_gc_stack_scan_available(void);

/* このプロセスが生きている間ずっとルートとして扱いたいP2C_Object*スロットを
 * 登録する。スロットの「アドレス」を渡す点に注意（中身は後で変わってよい）。
 * generated main()がモジュールレベル変数・トップレベルクラスの静的変数を
 * 登録するのに使う。 */
void p2c_gc_register_root(P2C_Object **slot);
void p2c_gc_unregister_root(P2C_Object **slot);
void p2c_gc_reset_roots(void);

/* mark-and-sweepを1回実行する。p2c_obj_new()の呼び出しに伴って必要に応じて
 * 自動的にも呼ばれるが、明示的に呼んでもよい。 */
void p2c_gc_collect(void);

void p2c_gc_set_enabled(bool enabled);
bool p2c_gc_is_enabled(void);
/* 自動発火のしきい値（前回の収集からの累積確保バイト数）を設定する。 */
void p2c_gc_set_threshold(size_t bytes);
/* しきい値の自動調整（適応GC）。有効時は「収集してもほとんど解放されない」
 * 状況でしきい値を倍々に伸ばし（〜上限 4MiB）、よく解放される状況では
 * 基準値へ戻していく。長時間動くタスクでの「収集のしすぎ」を避ける。
 * p2c_gc_set_threshold() は基準値と現在値の両方を設定し、成長回数を0に戻す。
 * 既定は有効（無効化する場合は false）。 */
void p2c_gc_set_adaptive(bool enabled);
/* 適応GCがしきい値を伸ばせる上限（バイト）。0で既定（4MiB）へ戻す。
 * 組込みではヒープ容量の1/4程度に設定すると枯渇しにくい
 * （p2c_embed_start() が自動設定する）。 */
void p2c_gc_set_adaptive_limit(size_t max_threshold);
size_t p2c_gc_adaptive_limit(void);
bool p2c_gc_is_adaptive(void);
/* GC統計。診断・カーネルのメモリ見積り・回帰テスト用。 */
typedef struct {
    size_t collections;         /* 累計収集回数 */
    size_t objects;             /* 現在追跡中のオブジェクト数 */
    size_t peak_objects;        /* 追跡オブジェクト数の最大値 */
    size_t last_freed;          /* 直近の収集で解放した数 */
    size_t threshold;           /* 現在の自動収集しきい値（バイト） */
    size_t base_threshold;      /* 基準しきい値（バイト） */
    size_t threshold_growths;   /* 適応GCがしきい値を伸ばした回数 */
    size_t oom_resets;          /* OOMでしきい値を基準値へ戻した回数 */
    size_t scanned_words;       /* 直近の収集で走査したスタック語数 */
    size_t temp_roots;          /* 直近の収集でルート化したTLS一時値の数 */
} P2C_GcStats;
void p2c_gc_stats(P2C_GcStats *out);
size_t p2c_gc_object_count(void);      /* 現在GCが追跡中のオブジェクト数 */
size_t p2c_gc_collections_run(void);   /* これまでに実行された収集回数 */
size_t p2c_gc_last_freed(void);        /* 直近の収集で解放されたオブジェクト数 */
size_t p2c_gc_last_stack_words(void);  /* 直近の収集で走査したスタック語数（診断用。
                                        * 使用中のスタック範囲だけを走査している
                                        * ことをカーネル/テストが確認できる） */
size_t p2c_gc_last_temp_roots(void);   /* 直近の収集でルート化したTLS一時値の数
                                        * （式評価中の左オペランドと処理中の例外）。
                                        * 0なら式の途中から参照できる一時値がない */
size_t p2c_gc_root_count(void);

/* ── 未対応構文のフォールバック (--fallback) ─────────────────────
 * 変換器が対応していない構文の代わりに生成されるスタブ。この式/文が
 * 実際に実行されたときだけ NotImplementedError になり、到達しなければ
 * プログラムはそのまま動く（＝未対応構文を含んでいてもビルドは通る）。
 * where は "line 12" のような位置、what は "expression" 等の種別。 */
/* ── サンドボックス（安全な実行上限） ─────────────────────────────
 * 組込み（HobbyOS等）で「信頼できないPython」を走らせるときに、暴走を
 * 例外として止めるための予算。0 は「無制限」。
 *   max_ticks : ループ後退エッジの実行回数（while/for の各反復）
 *   max_allocs: ランタイムが確保したオブジェクト数の上限
 * 予算超過時は SandboxError を送出する（クラッシュやハングにしない）。
 * 期限（ミリ秒）で止めたいカーネルは p2c_sandbox_ticks() を見て
 * 自分のタイマと組み合わせることもできる。 */
typedef struct {
    uint64_t max_ticks;
    uint64_t max_allocs;
} P2C_SandboxLimits;

void p2c_sandbox_set(const P2C_SandboxLimits *limits);
void p2c_sandbox_reset(void);
void p2c_sandbox_tick(void);           /* ループ後退エッジで呼ばれる */
uint64_t p2c_sandbox_ticks(void);      /* 消費したステップ数 */
uint64_t p2c_sandbox_allocs(void);     /* 消費した確保数 */
uint64_t p2c_sandbox_violations(void); /* 予算超過で例外を出した回数 */

P2C_Object* p2c_fallback_expr(const char *where, const char *what);
void p2c_fallback_stmt(const char *where, const char *what);
size_t p2c_gc_root_capacity(void);
bool p2c_gc_is_collecting(void);        /* 回収処理の再入を検査する */

/* main()の先頭でだけ使う。do/whileでラップした「文」であって式ではないので
 * 呼び出し側は末尾にセミコロンを付けること。ここでローカル変数のアドレスを
 * 取ることで、main()自身のスタックフレーム内をGCのスキャン基点にできる
 * （p2c_runtime_init()等、一段深い呼び出し先の中でアドレスを取ってしまうと、
 * main()自身のローカル変数がスキャン範囲から漏れてしまう）。 */
#define P2C_GC_ENTER_MAIN() do { int _p2c_gc_stack_mark_; p2c_gc_init(&_p2c_gc_stack_mark_); } while (0)

/* カーネルのタスクエントリ向け。スタック区間を明示してGCを初期化する。
 * 例: P2C_GC_ENTER_TASK(task->stack_base, task->stack_base + task->stack_size);
 * これを呼ぶと保守的スタックスキャンが有効になり、自動GCと
 * p2c_gc_collect() が実際に回収を行うようになる。 */
#define P2C_GC_ENTER_TASK(stack_lo, stack_hi) \
    do { int _p2c_gc_stack_mark_; p2c_gc_init(&_p2c_gc_stack_mark_); \
         p2c_gc_set_stack_bounds((stack_lo), (stack_hi)); } while (0)

#ifdef PYTHON_CODE_TO_C_NO_STDLIB
/* 自作OS向けの最小libc契約。
 *
 * malloc/calloc/realloc/free は common.h が（NO_STDLIB時に）宣言済み。
 * setjmp/longjmp はここで宣言する。
 *
 * 既定では、ランタイム本体(src/runtime/python_code_to_c_runtime.c)が
 * 「線形ヒープのmalloc/free」を同梱し、setjmp/longjmp には
 * コンパイラ組み込み (__builtin_setjmp / __builtin_longjmp) を使う。
 * これにより libc なしでも try/except が実際に機能する
 * （以前はスタブが無限ループしており、例外が送出されるとハングしていた）。
 *
 * カーネルが自前のアロケータや setjmp/longjmp を持っている場合は
 * PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義すると、それらは一切定義されず
 * 上記の宣言だけが残るため、カーネル実装へリンクされる
 * （参考実装: examples/embed/x86_64_setjmp.c）。
 * このとき p2c_runtime_init() の heap/heap_size 引数は無視される
 * （ヒープの所有権はカーネルのアロケータにある）。
 * コンパイラ組み込みを使いたくない場合（独自実装をリンクしたい場合）は
 * PYTHON_CODE_TO_C_NO_COMPILER_SETJMP を定義する。 */
typedef struct { void *buf[16]; } jmp_buf[1];
int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

/* コンパイラ組み込みによる本物の非局所脱出。
 * __builtin_setjmp は「呼び出し元の関数が二度戻る」ことを前提にした
 * 実装なので、必ずマクロで各呼び出し地点へ展開する（関数でラップすると
 * 保存したフレームが既に無効になっていて未定義動作になる）。
 * __builtin_longjmp は常に 1 を返すため、longjmp(env, val) の val は
 * 1 に固定される（ランタイム/生成コードは == 0 判定のみを使う）。
 * tcc (__TINYC__) はこれらの組み込みを持たないため対象外とし、libc の
 * setjmp/longjmp（ホスト構成）またはカーネル実装（NO_LIBC_STUBS）を使う。 */
#if !defined(setjmp) && !defined(PYTHON_CODE_TO_C_NO_COMPILER_SETJMP) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(__TINYC__)
#  define setjmp(env) __builtin_setjmp((void**)(env))
#  define longjmp(env, val) ((void)(val), __builtin_longjmp((void**)(env), 1))
#  define P2C_HAVE_COMPILER_SETJMP 1
#endif
#else
#include <setjmp.h>
#endif

/* ── OS差し替え点: 非局所脱出 (P2C_SETJMP / P2C_LONGJMP) ─────────────
 * ランタイム本体と生成コードの例外機構（raise/except/with/for/generator/
 * finally）は、この2つのマクロだけを通る。カーネル/自作OSは次のように
 * 定義するだけで、setjmp/longjmp の実装（libc / コンパイラ組み込み /
 * 独自のコンテキストスイッチ / syscall）を完全に差し替えられる。
 *
 *   -DP2C_SETJMP(env)=my_setjmp(env)
 *   -DP2C_LONGJMP(env,val)=my_longjmp((env),(val))
 *
 * 既定は setjmp/longjmp。NO_STDLIB では上のコンパイラ組み込み
 * (__builtin_setjmp/__builtin_longjmp) が、ホストでは libc が実体になる。
 * 契約は「P2C_SETJMP は 0 を返し、P2C_LONGJMP は P2C_SETJMP が 0 を返した
 * 呼び出し地点へ val(!=0) を持って戻る」こと。バッファは jmp_buf
 * （本ヘッダの定義、または libc の jmp_buf。いずれも void* 16語以上）。
 * 実装をマクロにする必要があるのは、組み込み setjmp が
 * 「呼び出し元の関数が二度戻る」前提でフレームを保存するため
 * （関数でラップすると保存済みフレームが無効になり未定義動作）。 */
#ifndef P2C_SETJMP
#  define P2C_SETJMP(env) setjmp(env)
#endif
#ifndef P2C_LONGJMP
#  define P2C_LONGJMP(env, val) longjmp((env), (val))
#endif

typedef struct P2C_ExceptFrame {
    jmp_buf env;
    P2C_Object *exc;
    struct P2C_ExceptFrame *prev;
} P2C_ExceptFrame;

extern P2C_THREAD_LOCAL P2C_ExceptFrame *p2c_exc_stack;
extern P2C_THREAD_LOCAL P2C_Object *p2c_active_exception;

void p2c_raise(P2C_Object *exc);
void p2c_reraise(void);
P2C_Object* p2c_make_exception(const char *type_name, const char *msg);
P2C_Object* p2c_exception_with_cause(P2C_Object *exc, P2C_Object *cause);
P2C_Object* p2c_posonly_keyword_error(const char *function_name, const char *parameter_name);
bool p2c_exc_match(P2C_Object *exc, P2C_ClassDef *cls);
bool p2c_exc_name_match(P2C_Object *exc, const char *type_name);

void p2c_print(P2C_Object *obj);
P2C_Object* p2c_print_multi(P2C_Object **args, size_t nargs);
P2C_Object* p2c_print_multi_opts(P2C_Object **args, size_t nargs, P2C_Object *sep, P2C_Object *end);
P2C_Object* p2c_obj_str(P2C_Object *obj);
P2C_Object* p2c_obj_repr(P2C_Object *obj);
void p2c_binop_begin(P2C_Object *left);
P2C_Object* p2c_binop_finish(P2C_BinaryOpFn op, P2C_Object *right);
P2C_Object* p2c_format_fixed(P2C_Object *obj, P2C_Object *ndigits);
P2C_Object* p2c_fstr_fmt(P2C_Object *obj, P2C_Object *spec);
void p2c_fstr_begin(void);
void p2c_fstr_append(P2C_Object *obj);
P2C_Object* p2c_fstr_finish(void);

/* 式評価中の一時値の深さ。生成Cのtryは開始時にこの深さを保存し、例外ハンドラへ
 * 到達した時点で p2c_binop_rewind()/p2c_fstr_rewind() により元の深さへ戻す。
 * 戻さないと、式の途中で例外が脱出するたびに一時値が残り、反復すると
 * p2c_binop_begin() が「binary expression nesting limit exceeded」を誤発火する
 * （f-stringのビルダはネイティブ確保なのでリークする）。巻き戻しは冪等なので
 * ハンドラごとに呼んでよい。カーネル等が自前でlongjmpする場合も、
 * 例外フレームへ到達した時点で同じ保存値へ戻す契約とする。 */
size_t p2c_binop_depth(void);
void p2c_binop_rewind(size_t depth);
size_t p2c_fstr_depth(void);
void p2c_fstr_rewind(size_t depth);
P2C_Object* p2c_obj_slice(P2C_Object *obj, P2C_Object *start, P2C_Object *stop, P2C_Object *step);
void p2c_slice_assign(P2C_Object *obj, P2C_Object *start, P2C_Object *stop, P2C_Object *step, P2C_Object *value);
void p2c_slice_delete(P2C_Object *obj, P2C_Object *start, P2C_Object *stop, P2C_Object *step);
P2C_Object* p2c_builtin_enumerate(P2C_Object *iterable, P2C_Object *start);
P2C_Object* p2c_builtin_zip(P2C_Object **args, size_t nargs);
bool p2c_isinstance_of_class(P2C_Object *obj, const char *class_name);
bool p2c_isinstance_of_object(P2C_Object *obj, P2C_Object *class_obj);
bool p2c_has_method(P2C_Object *obj, const char *name);
P2C_Object* p2c_builtin_type(P2C_Object *obj);
P2C_Object* p2c_obj_abs(P2C_Object *obj);
P2C_Object* p2c_builtin_min(P2C_Object **args, size_t nargs);
P2C_Object* p2c_builtin_min_key(P2C_Object **args, size_t nargs, P2C_Object *key);
P2C_Object* p2c_builtin_max(P2C_Object **args, size_t nargs);
P2C_Object* p2c_builtin_max_key(P2C_Object **args, size_t nargs, P2C_Object *key);
/* divmod(a, b) / pow(base, exp, mod) / format(value, spec) / callable(obj)。
 * pow_modは整数専用のモジュラべき乗（負の指数は法における逆元を要求する）。 */
P2C_Object* p2c_builtin_divmod(P2C_Object *a, P2C_Object *b);
P2C_Object* p2c_builtin_pow_mod(P2C_Object *base, P2C_Object *exp, P2C_Object *mod);
P2C_Object* p2c_builtin_format(P2C_Object *value, P2C_Object *spec);
P2C_Object* p2c_builtin_callable(P2C_Object *obj);
P2C_Object* p2c_builtin_sum(P2C_Object **args, size_t nargs);
P2C_Object* p2c_builtin_ord(P2C_Object *obj);
P2C_Object* p2c_builtin_chr(P2C_Object *obj);
P2C_Object* p2c_builtin_int_from_base(P2C_Object *obj, P2C_Object *base_obj);
/* 組込み関数を値として取り出す（sorted(key=len) など）。 */
P2C_Object* p2c_builtin_ref(const char *name);
P2C_Object* p2c_builtin_ref_checked(const char *name);
P2C_Object* p2c_builtin_int_base(P2C_Object *obj, unsigned base, const char *prefix);
P2C_Object* p2c_builtin_sorted(P2C_Object *iterable);
P2C_Object* p2c_builtin_sorted_key(P2C_Object *iterable, P2C_Object *key, P2C_Object *reverse);
P2C_Object* p2c_builtin_sorted_rev(P2C_Object *iterable, P2C_Object *reverse);
P2C_Object* p2c_builtin_reversed(P2C_Object *iterable);
P2C_Object* p2c_builtin_round(P2C_Object *x, P2C_Object *ndigits);
P2C_Object* p2c_builtin_any(P2C_Object *iterable);
P2C_Object* p2c_builtin_all(P2C_Object *iterable);
P2C_Object* p2c_builtin_map(P2C_Object *fn, P2C_Object *iterable);
P2C_Object* p2c_builtin_filter(P2C_Object *fn, P2C_Object *iterable);
P2C_Object* p2c_builtin_list(P2C_Object *iterable);
P2C_Object* p2c_builtin_dict(P2C_Object *iterable);
P2C_Object* p2c_builtin_set(P2C_Object *iterable);
P2C_Object* p2c_builtin_tuple(P2C_Object *iterable);
void p2c_print_str(const char *s);
P2C_Object* p2c_input(void);
int64_t p2c_len(P2C_Object *obj);
P2C_Object* p2c_range(P2C_Object *start, P2C_Object *stop, P2C_Object *step);
bool p2c_hasattr(P2C_Object *obj, const char *name);
P2C_Object* p2c_getattr(P2C_Object *obj, const char *name);
P2C_Object* p2c_getattr_default(P2C_Object *obj, const char *name, P2C_Object *default_val);
P2C_Object* p2c_builtin_iter(P2C_Object *obj);
P2C_Object* p2c_builtin_next(P2C_Object *it);
P2C_Object* p2c_async_iter(P2C_Object *obj);
P2C_Object* p2c_async_next(P2C_Object *iter);
P2C_Object* p2c_async_call_attr(P2C_Object *obj, const char *name, P2C_Object **args, size_t nargs);

/* Generator and cooperative coroutine API. Generated state machines use this
 * API instead of preserving C stack frames, so it remains C11/freestanding
 * compatible. A coroutine is an iterator that can only be progressed by the
 * cooperative scheduler. */
P2C_Object* p2c_generator_new(P2C_GeneratorStepFn step, bool is_coroutine);
P2C_Object* p2c_generator_new_with_local(P2C_GeneratorStepFn step, bool is_coroutine, const char *name, P2C_Object *value);
P2C_Object* p2c_generator_new_with_locals(P2C_GeneratorStepFn step, bool is_coroutine, const char * const *names, P2C_Object * const *values, size_t count);
uint32_t p2c_generator_state(P2C_Object *generator);
void p2c_generator_set_state(P2C_Object *generator, uint32_t state);
P2C_Object* p2c_generator_local_get(P2C_Object *generator, const char *name);
void p2c_generator_local_set(P2C_Object *generator, const char *name, P2C_Object *value);
P2C_Object* p2c_generator_yield(P2C_Object *generator, P2C_Object *value, uint32_t next_state);
P2C_Object* p2c_generator_finish(P2C_Object *generator, P2C_Object *value);
P2C_Object* p2c_generator_finish_exception(P2C_Object *generator, P2C_Object *exception);
P2C_Object* p2c_generator_result(P2C_Object *generator);
P2C_Object* p2c_generator_await(P2C_Object *generator, P2C_Object *awaitable, uint32_t next_state);
P2C_Object* p2c_generator_await_result(P2C_Object *generator);
bool p2c_generator_is_done(P2C_Object *generator);
void p2c_async_schedule(P2C_Object *coroutine);
P2C_Object* p2c_async_run(P2C_Object *coroutine);
size_t p2c_async_pending_count(void);

void p2c_setattr(P2C_Object *obj, const char *name, P2C_Object *val);
void p2c_delattr(P2C_Object *obj, const char *name); /* del obj.attr */
P2C_Object* p2c_subscript_get(P2C_Object *obj, P2C_Object *key);
bool p2c_match_sequence(P2C_Object *obj, size_t min_len, bool allow_rest);
P2C_Object* p2c_match_sequence_item(P2C_Object *obj, size_t index);
P2C_Object* p2c_match_sequence_rest(P2C_Object *obj, size_t start, size_t end_from_tail);
bool p2c_match_mapping_has(P2C_Object *obj, P2C_Object *key);
P2C_Object* p2c_match_mapping_get(P2C_Object *obj, P2C_Object *key);
P2C_Object* p2c_match_mapping_rest(P2C_Object *obj, P2C_Object *const *keys, size_t key_count);
bool p2c_match_class_positional(P2C_Object *obj, const char *class_name, size_t index, P2C_Object **out_value);
P2C_Object* p2c_iter_at(P2C_Object *obj, int64_t position); /* for文の反復専用（p2c_subscript_getとは意味が異なる。dict等を参照） */
void p2c_subscript_set(P2C_Object *obj, P2C_Object *key, P2C_Object *val);
void p2c_subscript_delete(P2C_Object *obj, P2C_Object *key); /* del obj[key] */

#ifdef __cplusplus
}
#endif

#endif
