/*
 * In-kernel transpilation regression: run the compiler core itself inside the
 * embedded (kernel-style) environment.
 *
 * This is the use case the freestanding core exists for: a hobby OS holds
 * Python source (from a shell, a file or a network buffer) and converts it to
 * C at runtime. That path needs the default allocator to work without a libc
 * (see src/common/python_code_to_c_common.c) and does not touch the hosted
 * platform layer.
 *
 * The host C library supplies the documented numeric/text helper contract
 * (snprintf, strtoll, strtod, math functions) just like a kernel would supply
 * its own; everything else goes through p2c_embed.
 */
#define P2C_EMBED_PROVIDE_LIBC_HEAP 1
#define P2C_EMBED_PROVIDE_PLATFORM_COMPAT 1
#include "platform/python_code_to_c_embed.h"
#include "core/python_code_to_c.h"

static unsigned char heap_storage[96u * 1024u];
static P2C_EmbedHeap heap;

static const char *SOURCE =
    "def total(values):\n"
    "    acc = 0\n"
    "    for value in values:\n"
    "        acc += value\n"
    "    return acc\n"
    "\n"
    "print(\"sum\", total([1, 2, 3, 4]))\n";

int main(void) {
    if (p2c_embed_heap_init(&heap, heap_storage, sizeof(heap_storage)) != 0) return 10;
    p2c_embed_heap_set_default(&heap);
    P2C_EmbedConfig cfg;
    p2c_embed_config_init(&cfg);
    p2c_embed_config_use_heap(&cfg, &heap, heap_storage, sizeof(heap_storage));
    cfg.raise_memory_error = true;
    if (p2c_embed_start(&cfg) != 0) return 11;

    char *generated = NULL;
    P2C_Result result = python_to_c(SOURCE, NULL, &generated);
    if (result != P2C_OK || !generated) { p2c_embed_stop(); return 20; }

    /* 生成Cが通常のmain()エントリを持つこと（カーネル内変換でも既定はmain） */
    if (strstr(generated, "int main(void)") == NULL) { free(generated); p2c_embed_stop(); return 21; }
    /* 変換結果が実際にPython意味論を反映していること */
    if (strstr(generated, "p2c_builtin_iter") == NULL) { free(generated); p2c_embed_stop(); return 22; }
    if (strstr(generated, "p2c_obj_add") == NULL) { free(generated); p2c_embed_stop(); return 23; }
    if (strstr(generated, "total") == NULL) { free(generated); p2c_embed_stop(); return 24; }

    /* 共有ヒープ: プラットフォーム（embed ヒープ）が変換器コアの確保にも
     * 使われること。以前はランタイムが libc/スタブの malloc を直接使っており、
     * カーネルが登録したアロケータは呼ばれなかった。 */
    if (!p2c_heap_uses_platform()) { free(generated); p2c_embed_stop(); return 29; }
    {
        void *probe = p2c_heap_alloc(64);
        if (!probe) { free(generated); p2c_embed_stop(); return 30; }
        unsigned char *base = (unsigned char*)heap.base;
        if ((unsigned char*)probe < base || (unsigned char*)probe >= base + heap.capacity) {
            p2c_heap_free(probe); free(generated); p2c_embed_stop(); return 31;
        }
        p2c_heap_free(probe);
    }

    /* 埋込みエントリ生成（--embed-entry相当）もAPIから使えること */
    P2C_TranspileOptions opts = P2C_DEFAULT_TRANSPILER_OPTIONS;
    opts.embed_entry = "kernel_python_program";
    char *embed_c = NULL;
    result = python_to_c(SOURCE, &opts, &embed_c);
    if (result != P2C_OK || !embed_c) { free(generated); p2c_embed_stop(); return 25; }
    if (strstr(embed_c, "P2C_Object* kernel_python_program(void)") == NULL) { free(embed_c); free(generated); p2c_embed_stop(); return 26; }
    if (strstr(embed_c, "int main(void)") != NULL) { free(embed_c); free(generated); p2c_embed_stop(); return 27; }

    /* 不正なエントリ名は診断されて拒否されること（C識別子のみ許可） */
    P2C_TranspileOptions bad = P2C_DEFAULT_TRANSPILER_OPTIONS;
    bad.embed_entry = "1bad; name";
    char *bad_c = NULL;
    result = python_to_c(SOURCE, &bad, &bad_c);
    if (result == P2C_OK || bad_c != NULL) { free(bad_c); free(embed_c); free(generated); p2c_embed_stop(); return 28; }

    free(embed_c);
    free(generated);
    p2c_embed_stop();
    return 0;
}
