#ifndef PYTHON_CODE_TO_C_PLATFORM_H
#define PYTHON_CODE_TO_C_PLATFORM_H

#include "common/python_code_to_c_common.h"

/* Platform contract for hosted systems and hobby OS kernels.  Implementations
 * may provide only these hooks; the compiler core does not depend on libc. */
typedef struct {
    void *(*alloc)(size_t size, void *user);
    void *(*realloc)(void *ptr, size_t old_size, size_t new_size, void *user);
    void (*free)(void *ptr, size_t size, void *user);
    void (*write)(int stream, const char *data, size_t len, void *user);
    uint64_t (*clock_ms)(void *user);
    void *user;
} P2C_Platform;

const P2C_Platform *p2c_platform_default(void);
void p2c_platform_set(const P2C_Platform *platform);
const P2C_Platform *p2c_platform_current(void);

/* アロケータだけを1回で注入する（write/clock_ms は既定実装を引き継ぐ）。
 *
 *   p2c_platform_set_allocator(kalloc, krealloc, kfree, kernel_ctx);
 *   p2c_platform_set_allocator(NULL, NULL, NULL, NULL);  // 解除
 *
 * これ1回で、共有ヒープ（p2c_heap_*）＝「ランタイムと変換器コアの唯一の
 * ヒープ」がこのアロケータになる。P2C_Platform 一式を用意する必要はない。
 * 3つとも非NULLのときだけ設定される（いずれかがNULLなら解除）。 */
void p2c_platform_set_allocator(void *(*alloc_fn)(size_t size, void *user),
                                void *(*realloc_fn)(void *ptr, size_t old_size, size_t new_size, void *user),
                                void (*free_fn)(void *ptr, size_t size, void *user),
                                void *user);

/* Runtime compatibility hooks. Hosted and kernel adapters may implement these
 * directly; the default hosted adapter is provided by the project. */
void p2c_platform_init(void);
void p2c_platform_shutdown(void);
void p2c_platform_write(const char *s);
void p2c_platform_write_n(const char *s, size_t len);
size_t p2c_platform_read_line(char *buf, size_t cap);
void p2c_platform_abort(const char *reason);

#endif
