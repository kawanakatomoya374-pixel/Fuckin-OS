#ifndef MEMORY_H
#define MEMORY_H

#include "types.h"

/* C-OS 4.0.8 alpha Memory Manager
 *
 * 履歴 / 落とし穴:
 *   旧版では HEAP_BASE=0x4000000 / HEAP_SIZE=3.5GB をこのヘッダに直書きし、
 *   一方で src/kernel/memory.c は static uint8_t kernel_heap[HEAP_SIZE]
 *   (=32MiB) を BSS に置いていました。paging.c や一部ドライバが
 *   0x4000000 を前提にページングを組み立てる一方、kmalloc/kfree の
 *   実体は 32MiB の static 配列 — 二系統の前提が噛み合わず、
 *   kmalloc_pages() と paging_map_range() が食い違うケースがありました。
 *
 *   解:
 *     - 「真の定義」は src/kernel/memory.c 側に一本化する。
 *     - このヘッダは互換のための薄いシムとなり、paging 系は
 *       ランタイム解決 API  (cos_heap_get_base / cos_heap_get_total_size)
 *       経由で実際のヒープを取得する。
 *     - HEAP_BASE / HEAP_SIZE 定数は残すが、コンパイル時に使う "ヒント"
 *       としての意味しか持たせない。paging.c はもう直接参照しない。
 */

#define COS_HEAP_BASE_LEGACY_HINT   0x4000000UL        /* 旧コードの参照用ヒント */
#define COS_HEAP_BYTES_DEFAULT      (32UL  * 1024 * 1024) /* memory.c の既定値と一致 */
#define COS_HEAP_BYTES_MAX          (512UL * 1024 * 1024) /* 単一 alloc の最大 */
#define COS_HEAP_PAGES_DEFAULT      (COS_HEAP_BYTES_DEFAULT / 4096ULL)

/* stats クエリ (block_header_t は旧形式 — 互換シムのみ)。            *
 * 新しい memory_block_t / memory_stats_t は memory.c 内で定義され、   *
 * 直接外界に晒さない。GUI 側は kmemory_used() / kmemory_total() を     *
 * 経由してアクセスする。                                             */
#define BLOCK_MAGIC_LEGACY      0xDEADBEEFUL   /* 旧 API 互換のためのみ */
#define GUARD_SIZE              16

typedef struct block_header {
    uint64_t magic;
    uint64_t size;
    uint64_t real_size;
    uint8_t  free_;
    uint8_t  zone;
    uint8_t  guard[2];
    struct block_header* next;
    struct block_header* prev;
} block_header_t;

typedef struct {
    uint64_t total_mb;
    uint64_t used_mb;
    uint64_t free_mb;
    uint64_t total_allocs;
    uint64_t active_allocs;
    uint64_t peak_used;
} mem_stats_t;

/* ------------------------------------------------------------------------ *
 *  Canonical runtime heap API — paging / drivers must use these rather    *
 *  than any compile-time constant. The base address is resolved at boot    *
 *  (it is where memory.c's static kernel_heap lands in BSS), and the      *
 *  total size is whatever memory.c was built with (32 MiB by default).    *
 * ------------------------------------------------------------------------ */
void*    cos_heap_get_base(void);
uint64_t cos_heap_get_total_size(void);
uint64_t cos_heap_get_used_size(void);
uint64_t cos_heap_get_free_size(void);
/* Runtime physical direct-map upper bound supplied by the Multiboot parser. */
uint64_t cos_runtime_direct_map_extent(void);

void     memory_init(uint64_t mem_upper);
void*    kmalloc(size_t size);
void*    kmalloc_aligned(uint64_t size, uint64_t align);
void*    kmalloc_pages(size_t pages);
void*    krealloc(void* ptr, uint64_t new_size);
void     kfree(void* ptr);
uint64_t kmemory_used(void);
uint64_t kmemory_free(void);
uint64_t kmemory_total(void);
void     kmemory_get_stats(mem_stats_t* stats);

/* Public memory_stat API (consumed across the kernel) */
uint64_t memory_get_total(void);
uint64_t memory_get_used(void);
uint64_t memory_get_free(void);
void     memory_dump(void);

/* Legacy compat */
void     memory_stats(uint64_t* total, uint64_t* used, uint64_t* free_mem, uint64_t* active);
uint64_t memory_verify_all(void);
void     memory_print_stats(void);

#endif /* MEMORY_H */
