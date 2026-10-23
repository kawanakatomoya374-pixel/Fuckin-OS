/*
 * ベアメタル構成（PYTHON_CODE_TO_C_NO_STDLIB = 自作libcスタブ + libcなし）での
 * 例外機構と保守的スタックスキャンの回帰。
 *
 * 以前の実装では
 *   - longjmp スタブが無限ループだったため、raise した瞬間プロセスが停止した
 *     （try/except を含む生成コードがハング）
 *   - Linux 以外ではスタック境界が未登録のため自動GCが「安全側停止」になり、
 *     スタック上のローカル変数しか指していないオブジェクトが保護されない
 *     （逆に収集が一度も走らない）
 * という問題があった。このテストは自作OSと同じ構成（NO_STDLIB・プラットフォーム
 * 提供アロケータ・コンパイラ組み込み or カーネル提供 setjmp）で
 *   1. raise/except が実際に捕まえられること
 *   2. ローカル変数からしか到達できないオブジェクトが自動GCで保護されること
 * を確認する。
 */
#include "baremetal/python_code_to_c_baremetal.h"

/* raise → except で捕まえ、例外オブジェクトとメッセージを検証する。 */
static int exceptions_are_catchable(void) {
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    int status = 0;
    if (setjmp(frame.env) == 0) {
        p2c_raise(p2c_make_exception("ZeroDivisionError", "division by zero"));
        status = 1; /* ここへは到達しない */
    } else if (!p2c_exc_name_match(frame.exc, "ZeroDivisionError")) {
        status = 2;
    } else {
        P2C_Object *text = p2c_obj_str(frame.exc);
        const char *msg = p2c_obj_as_str(text);
        if (!msg || strcmp(msg, "division by zero") != 0) status = 3;
    }
    p2c_exc_stack = frame.prev;
    return status;
}

/* 例外を捕まえた側で、その例外を使って通常処理が続けられること。 */
static int exception_payload_is_usable(void) {
    P2C_ExceptFrame frame;
    frame.prev = p2c_exc_stack;
    frame.exc = NULL;
    p2c_exc_stack = &frame;
    P2C_Object *volatile exc = &P2C_None;
    if (setjmp(frame.env) == 0) {
        p2c_raise(p2c_make_exception("ValueError", "boom"));
    } else {
        exc = frame.exc;
    }
    p2c_exc_stack = frame.prev;
    if (exc == &P2C_None) return 10;
    P2C_Object *joined = p2c_obj_add(p2c_obj_from_str("caught:"), p2c_obj_str(exc));
    const char *text = p2c_obj_as_str(joined);
    if (!text || strcmp(text, "caught:boom") != 0) return 11;
    return 0;
}

/* スタック上のローカル変数からのみ到達できるオブジェクトを保持したまま
 * 自動GCを多発させる（しきい値1 = ほぼ毎回の確保で収集）。 */
static int stack_roots_are_scanned(void) {
    p2c_gc_set_threshold(1);
    size_t before = p2c_gc_collections_run();
    P2C_Object * volatile kept = p2c_list_new();
    if (!kept) return 20;
    for (int i = 0; i < 32; i++) {
        P2C_Object *tmp = p2c_list_new();
        if (!tmp) return 21;
        p2c_list_append(tmp, p2c_obj_from_int(i));
        p2c_list_append(kept, tmp);
    }
    if (p2c_gc_collections_run() == before) return 22; /* 収集が動いていない */
    if (p2c_list_len(kept) != 32) return 23;           /* 生存ローカルが回収された */
    if (p2c_gc_last_stack_words() == 0) return 24;     /* スタックスキャン未実行 */
    return 0;
}

static int g_program_status = 100; /* p2c_baremetal_run は戻り値を捨てるため静的領域で運ぶ */

static P2C_Object *baremetal_program_checks(void) {
    int status = exceptions_are_catchable();
    if (status != 0) { g_program_status = status; return &P2C_None; }
    status = exception_payload_is_usable();
    if (status != 0) { g_program_status = status; return &P2C_None; }
    status = stack_roots_are_scanned();
    if (status != 0) { g_program_status = status; return &P2C_None; }
    g_program_status = 0;
    p2c_print(p2c_obj_from_str("baremetal-ok"));
    return &P2C_None;
}

int main(void) {
    static unsigned char heap[128u * 1024u];
    static char console[128];
    P2C_BaremetalBoard board;
    p2c_baremetal_board_init(&board, heap, sizeof(heap), console, sizeof(console));
    if (p2c_baremetal_run(&board, baremetal_program_checks) != 0) return 1;
    if (g_program_status != 0) return g_program_status;
    if (strcmp(console, "baremetal-ok\n") != 0) return 2;
    if (board.heap_used == 0) return 3; /* プラットフォームのアロケータが使われた */
    return 0;
}
