/*
 * C-OS Userland TinyCC Wrapper
 * Standalone compiler application
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libtcc.h"

// エントリーポイント
int main(int argc, char **argv) {
    TCCState *s;
    int i;
    int ret = 0;

    printf("C-OS TinyCC Compiler\n");

    s = tcc_new();
    if (!s) {
        fprintf(stderr, "tcc: could not create tcc state\n");
        return 1;
    }

    // C-OS 固有の設定
    tcc_set_options(s, argc, argv);

    // 引数の処理
    for(i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (tcc_parse_args(s, &argc, &argv, i) == 0) {
                // 引数処理失敗
            }
        }
    }

    // コンパイル実行
    ret = tcc_run(s, argc, argv);

    tcc_delete(s);

    return ret;
}
