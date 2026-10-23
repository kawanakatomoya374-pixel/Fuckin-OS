/**
 * sample_hello_c.c - Sample C Program for TinyCC Testing
 * 
 * Phase 1: TinyCC 統合 - C-OS 内から C コードをコンパイル可能にする
 * 
 * 使用方法:
 *   tcc /sample_hello_c.c /hello
 *   open hello
 */

#include <stdint.h>
#include <stdbool.h>

/* C-OS API (簡易版) */
extern void cos_print(const char* str);
extern void cos_print_line(const char* str);
extern uint64_t cos_get_tick(void);

/* 標準ライブラリ風マクロ */
#define printf(fmt, ...) cos_print(fmt)
#define puts(str) cos_print_line(str)

/**
 * @brief エントリーポイント
 */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    
    puts("==================================");
    puts("  Hello from C-OS!");
    puts("==================================");
    puts("");
    puts("This program was compiled inside C-OS");
    puts("using the TinyCC compiler.");
    puts("");
    puts("C-OS Version: 4.0.8 alpha");
    puts("Compiler: TinyCC 0.9.27");
    puts("");
    
    /* 簡単な計算デモ */
    int a = 42;
    int b = 58;
    int sum = a + b;
    int product = a * b;
    
    puts("Simple calculation demo:");
    printf("  %d + %d = %d\n", a, b, sum);
    printf("  %d * %d = %d\n", a, b, product);
    
    puts("");
    puts("Program completed successfully!");
    
    return 0;
}
