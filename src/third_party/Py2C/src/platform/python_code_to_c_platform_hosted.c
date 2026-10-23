#include "platform/python_code_to_c_platform.h"

#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

void p2c_platform_init(void) {
}

void p2c_platform_shutdown(void) {
}

void p2c_platform_write(const char *s) {
    const P2C_Platform *platform = p2c_platform_current();
    const char *text = s ? s : "";
    if (platform && platform->write) platform->write(1, text, strlen(text), platform->user);
}

void p2c_platform_write_n(const char *s, size_t len) {
    const P2C_Platform *platform = p2c_platform_current();
    if (platform && platform->write && s && len) platform->write(1, s, len, platform->user);
}

size_t p2c_platform_read_line(char *buf, size_t cap) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    if (!buf || cap == 0) return 0;
    if (!fgets(buf, (int)cap, stdin)) {
        buf[0] = '\0';
        return 0;
    }
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
        len--;
    }
    return len;
#else
    (void)buf;
    (void)cap;
    return 0;
#endif
}

void p2c_platform_abort(const char *reason) {
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
    if (reason && *reason) {
        fprintf(stderr, "%s\n", reason);
    }
    /* abort()はstdioバッファをフラッシュしない。stdoutがファイル/パイプへ
     * リダイレクトされている場合（フルバッファリングになる一般的なケース:
     * CI、テストハーネス、`prog > out.txt`、GUIのサブプロセス出力キャプチャ等）、
     * p2c_raise()が例外送出直前に書き込んだ「ExceptionType: message」の
     * 診断メッセージがバッファに残ったままプロセスが強制終了し、
     * ユーザーには何も表示されない、という分かりにくい状況になっていた。 */
    fflush(stdout);
    fflush(stderr);
    abort();
#else
    (void)reason;
    while (1) { }
#endif
}
