/* HobbyOS向けELF（カーネルレス・スタンドアロン）のエントリポイント (Alpha0.6)
 *
 * `make hobbyos elf`（または `make hobbyos-elf`）で作られる「HobbyOSへそのまま
 * 組み込める自己完結ELF」の入口である。libc を使わず、ランタイム同梱の最小libc
 * （memcpy/strlen/snprintf等）と p2c_embed の組込みヒープだけで動く。
 * カーネルはこのELFをロードして p2c_hobbyos_entry() を呼ぶだけでよい。
 *
 * 実カーネルへ組み込む場合の手順:
 *   1. p2c_hobbyos_output へUART等の出力関数を設定する（NULLなら出力なし）
 *   2. 必要なら p2c_embed_start() の代わりに p2c_platform_set() で自前の
 *      プラットフォーム（write/clock/abort）を登録する
 *   3. PYTHON_CODE_TO_C_NO_LIBC_STUBS を定義してカーネルのlibcを使う構成に
 *      切り替える場合は、malloc/free/realloc/calloc と setjmp/longjmp を
 *      カーネル側で提供する（templates/hobby_os/README.md 参照）
 */

#include "runtime/python_code_to_c_runtime.h"
#include "platform/python_code_to_c_embed.h"

/* --embed-entry で生成されたモジュールエントリ（P2C_Object *NAME(void)）。 */
extern P2C_Object *p2c_hobbyos_program(void);

#ifndef P2C_HOBBYOS_HEAP_BYTES
#define P2C_HOBBYOS_HEAP_BYTES (128u * 1024u)
#endif
#ifndef P2C_HOBBYOS_CONSOLE_BYTES
#define P2C_HOBBYOS_CONSOLE_BYTES 512u
#endif

/* カーネルが差し替える出力フック。既定（NULL）では出力先が無いため、
 * 何も書かずにコンソールバッファだけへ記録する。 */
void (*p2c_hobbyos_output)(const char *data, size_t len) = NULL;

static unsigned char g_hobbyos_heap[P2C_HOBBYOS_HEAP_BYTES];
static char g_hobbyos_console[P2C_HOBBYOS_CONSOLE_BYTES];
static P2C_EmbedHeap g_hobbyos_heap_state;

static void p2c_hobbyos_write(void *user, const char *data, size_t len) {
    (void)user;
    if (p2c_hobbyos_output && data && len > 0) p2c_hobbyos_output(data, len);
}

static void p2c_hobbyos_panic(const char *reason, void *user) {
    (void)user;
    /* カーネルが復帰しない前提のフック。復帰した場合は p2c_embed 側が停止する。 */
    if (p2c_hobbyos_output && reason) {
        p2c_hobbyos_output("panic: ", 7);
        p2c_hobbyos_output(reason, strlen(reason));
        p2c_hobbyos_output("\n", 1);
    }
}

/* 直近の出力（コンソールバッファ）を取り出す。カーネルのデバッグ用。 */
const char *p2c_hobbyos_console_text(void) {
    return p2c_embed_console_text();
}

int p2c_hobbyos_entry(void) {
    P2C_EmbedConfig config;
    P2C_Object *result;

    p2c_embed_config_init(&config);
    p2c_embed_config_use_heap(&config, &g_hobbyos_heap_state, g_hobbyos_heap, sizeof(g_hobbyos_heap));
    p2c_embed_config_use_console(&config, g_hobbyos_console, sizeof(g_hobbyos_console));
    p2c_embed_config_use_uart(&config, p2c_hobbyos_write, NULL);
    config.panic = p2c_hobbyos_panic;
    config.raise_memory_error = true;
    /* スタック区間はカーネルが p2c_embed_config_use_stack() で宣言するまで
     * 未指定のままにする（GCは安全側で停止し、誤回収しない）。 */

    if (p2c_embed_start(&config) != 0) return 1;
    result = p2c_embed_run_program(p2c_hobbyos_program);
    p2c_embed_stop();
    return result ? 0 : 1;
}
