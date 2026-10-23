/*
 * 並行実行の回帰（pthread）。次の2点を確かめる。
 *
 *  1. 変換器（字句解析・構文解析・意味解析・生成）は、複数スレッドから同時に呼んでも
 *     単一スレッドと同じ C を出す（字句解析・解析の状態をスレッドごとに分けてあること）。
 *  2. ランタイムの「現在のインスタンス」はスレッドごとに分かれる。各スレッドが自分の
 *     コンテキストを作り、GC しきい値を小さくして収集を頻発させながら計算しても、
 *     他スレッドの選択に影響されず、結果が正しい。
 *
 * ランタイムの既定インスタンスは全スレッドで共有されるため、このテストでは使わない
 * （スレッド契約: 1スレッド1コンテキスト）。
 */
#include "core/python_code_to_c.h"
#include "runtime/python_code_to_c_runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPILE_THREADS 2
#define RUNTIME_THREADS 2
#define COMPILE_ROUNDS 30
#define RUNTIME_ITEMS 20000

static const char *programs[] = {
    "print(1 + 2)\n",
    "def f(x):\n    return x * x\nprint(f(7))\n",
    "xs = [i * 3 for i in range(4)]\nprint(xs)\n",
    "class C:\n    def m(self, a):\n        return a + 1\nprint(C().m(41))\n",
};
#define NPROGRAMS (sizeof(programs) / sizeof(programs[0]))

static char *expected[NPROGRAMS];
static pthread_mutex_t fail_lock = PTHREAD_MUTEX_INITIALIZER;
static int failures = 0;

static void record_failure(const char *what) {
    pthread_mutex_lock(&fail_lock);
    failures++;
    fprintf(stderr, "thread failure: %s\n", what);
    pthread_mutex_unlock(&fail_lock);
}

static void *compile_worker(void *arg) {
    (void)arg;
    for (int round = 0; round < COMPILE_ROUNDS; round++) {
        for (size_t i = 0; i < NPROGRAMS; i++) {
            char *out = NULL;
            P2C_Result r = python_to_c(programs[i], NULL, &out);
            if (r != P2C_OK || !out) {
                record_failure("python_to_c failed under concurrency");
            } else if (strcmp(out, expected[i]) != 0) {
                record_failure("python_to_c output differs from the single-threaded output");
            }
            free(out);
        }
    }
    return NULL;
}

static void *runtime_worker(void *arg) {
    int64_t id = (int64_t)(intptr_t)arg;
    struct P2C_RuntimeContext *ctx = p2c_runtime_context_create();
    if (!ctx) {
        record_failure("context create failed");
        return NULL;
    }
    p2c_runtime_context_select(ctx);
    int stack_marker = 0;
    p2c_gc_init(&stack_marker);
    p2c_runtime_init(NULL, 0);
    p2c_gc_set_threshold(64u * 1024u); /* 収集を頻発させ、他スレッドの確保と重ねる */

    P2C_Object *list = p2c_list_new();
    P2C_Object *acc = p2c_obj_from_int(0);
    int64_t expect = 0;
    for (int64_t i = 0; i < RUNTIME_ITEMS; i++) {
        int64_t v = i + id * 1000;
        p2c_list_append(list, p2c_obj_from_int(v));
        acc = p2c_obj_add(acc, p2c_obj_from_int(v));
        expect += v;
    }
    p2c_gc_collect();
    if (p2c_obj_as_int(acc) != expect) record_failure("runtime sum differs under concurrency");

    p2c_runtime_shutdown();
    p2c_runtime_context_select(NULL);
    p2c_runtime_context_destroy(ctx);
    return NULL;
}

int main(void) {
    for (size_t i = 0; i < NPROGRAMS; i++) {
        P2C_Result r = python_to_c(programs[i], NULL, &expected[i]);
        if (r != P2C_OK || !expected[i]) {
            fprintf(stderr, "reference conversion failed for program %zu\n", i);
            return 1;
        }
    }

    pthread_t threads[COMPILE_THREADS + RUNTIME_THREADS];
    int n = 0;
    for (int i = 0; i < COMPILE_THREADS; i++) {
        if (pthread_create(&threads[n++], NULL, compile_worker, NULL) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < RUNTIME_THREADS; i++) {
        if (pthread_create(&threads[n++], NULL, runtime_worker, (void*)(intptr_t)(i + 1)) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    for (int i = 0; i < n; i++) pthread_join(threads[i], NULL);

    for (size_t i = 0; i < NPROGRAMS; i++) free(expected[i]);
    if (failures != 0) {
        printf("threads_failed: %d\n", failures);
        return 1;
    }
    printf("threads_ok: %d compile + %d runtime threads\n", COMPILE_THREADS, RUNTIME_THREADS);
    return 0;
}
