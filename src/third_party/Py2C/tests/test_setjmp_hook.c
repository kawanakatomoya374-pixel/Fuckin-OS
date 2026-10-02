/*
 * setjmp/longjmp のOS差し替えフック（P2C_SETJMP / P2C_LONGJMP）の回帰。
 *
 * 自作OSではカーネルの setjmp/longjmp（コンテキストスイッチ、syscall、
 * あるいは独自実装）へ置き換えたい。ランタイム本体と生成コードの例外機構は
 * P2C_SETJMP / P2C_LONGJMP だけを通るので、この2つを定義すればよい。
 *
 * test-setjmp-hook は tests/setjmp_hook_override.h を -include して、
 * ランタイム本体（src/runtime/python_code_to_c_runtime.c）も同じフック経由で
 * コンパイルする。オーバーライドは libc の setjmp/longjmp を呼びつつ回数を
 * 数えるので、「ランタイムが本当にフックを通ったか」を検証できる。
 */
#include "core/python_code_to_c.h"
#include "runtime/python_code_to_c_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* フック呼び出し回数（tests/setjmp_hook_override.h が extern 宣言する） */
int p2c_test_setjmp_calls = 0;
int p2c_test_longjmp_calls = 0;

#define CHECK(cond, code) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return (code); } } while (0)

/* 生成コードと同じ形: except フレームを張り、raise を捕まえる。 */
static int catch_raise(void) {
    P2C_ExceptFrame ef;
    ef.prev = p2c_exc_stack;
    ef.exc = NULL;
    p2c_exc_stack = &ef;
    if (P2C_SETJMP(ef.env) == 0) {
        p2c_raise(p2c_make_exception("ValueError", "hooked"));
        p2c_exc_stack = ef.prev;
        return 1; /* ここへは戻らない */
    }
    p2c_exc_stack = ef.prev;
    if (!p2c_exc_name_match(ef.exc, "ValueError")) return 2;
    return 0;
}

int main(void) {
    p2c_runtime_init(NULL, 0);

    /* 1. 生成コードと同じ手順がフック経由で動作する */
    int status = catch_raise();
    CHECK(status == 0, 10);
    CHECK(p2c_test_setjmp_calls >= 1, 11);  /* フックが使われた */
    CHECK(p2c_test_longjmp_calls == 1, 12); /* p2c_raise() がフックを通った */

    /* 2. 生成コードも生の setjmp( ではなくフックを出力する */
    char *code = NULL;
    P2C_Result result = python_to_c(
        "try:\n"
        "    raise ValueError('x')\n"
        "except ValueError:\n"
        "    ok = 1\n"
        "for i in range(3):\n"
        "    ok += i\n",
        NULL, &code);
    CHECK(result == P2C_OK, 13);
    CHECK(code != NULL, 14);
    CHECK(strstr(code, "P2C_SETJMP(") != NULL, 15);
    CHECK(strstr(code, " setjmp(") == NULL, 17);
    free(code);

    /* 3. 変換を繰り返しても（複数回のシーケンスでも）フックが使われる */
    int before = p2c_test_setjmp_calls;
    code = NULL;
    result = python_to_c("x = 1\ntry:\n    pass\nexcept Exception:\n    pass\n", NULL, &code);
    CHECK(result == P2C_OK, 18);
    free(code);
    CHECK(p2c_test_setjmp_calls >= before, 19);

    p2c_runtime_shutdown();
    printf("setjmp hook: ok (setjmp=%d longjmp=%d)\n",
           p2c_test_setjmp_calls, p2c_test_longjmp_calls);
    return 0;
}
