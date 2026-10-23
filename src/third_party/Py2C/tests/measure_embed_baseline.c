/* 組込みランタイムの初期化（組込みモジュール登録）に必要なヒープ量を実測する。 */
#include "platform/python_code_to_c_embed.h"
#include <stdio.h>

static unsigned char heap_storage[128u * 1024u];
static P2C_EmbedHeap heap;
static volatile int panic_hit;

static void measuring_panic(const char *reason, void *user) {
    (void)reason;
    (void)user;
    panic_hit = 1;
}

int main(void) {
    P2C_EmbedConfig cfg;
    p2c_embed_config_init(&cfg);
    p2c_embed_config_use_heap(&cfg, &heap, heap_storage, sizeof(heap_storage));
    cfg.panic = measuring_panic;
    cfg.raise_memory_error = false;
    cfg.enable_gc = false;
    if (p2c_embed_start(&cfg) != 0) {
        printf("embed_start_failed\n");
        return 1;
    }
    printf("heap_capacity=%zu used=%zu peak=%zu free=%zu largest_free=%zu panic=%d\n",
           heap.capacity, heap.used, heap.peak,
           p2c_embed_heap_free_bytes(&heap), p2c_embed_heap_largest_free(&heap), panic_hit);
    p2c_embed_stop();
    return 0;
}
