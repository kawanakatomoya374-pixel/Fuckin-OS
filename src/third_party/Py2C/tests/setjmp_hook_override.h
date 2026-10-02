/*
 * setjmp/longjmp のOS差し替えフックのテスト用オーバーライド。
 *
 * test-setjmp-hook は、テスト本体とランタイム本体
 * (src/runtime/python_code_to_c_runtime.c) を同じフラグでコンパイルする際に
 *   -include tests/setjmp_hook_override.h
 * を付ける。これにより「カーネルがフックを差し替えた」状態になり、
 * ランタイム/生成コードが P2C_SETJMP / P2C_LONGJMP を実際に通っているか
 * どうかを呼び出し回数で検証できる。
 *
 * 実体はホストの libc setjmp/longjmp のまま。カーネルは同じ形で
 * 独自の実装（例: examples/embed/x86_64_setjmp.c や、コンテキスト
 * スイッチを行う kernel_setjmp）を呼べばよい。
 * 必ずマクロとして定義すること: 組み込み setjmp は「呼び出し元の関数が
 * 二度戻る」前提でフレームを保存するため、関数でラップすると未定義動作に
 * なる（既定実装がマクロなのはこの理由）。
 */
#ifndef P2C_TEST_SETJMP_HOOK_H
#define P2C_TEST_SETJMP_HOOK_H

extern int p2c_test_setjmp_calls;
extern int p2c_test_longjmp_calls;

#define P2C_SETJMP(env)       (p2c_test_setjmp_calls++, setjmp(env))
#define P2C_LONGJMP(env, val) (p2c_test_longjmp_calls++, longjmp((env), (val)))

#endif /* P2C_TEST_SETJMP_HOOK_H */
