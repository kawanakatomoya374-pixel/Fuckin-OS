#include <stddef.h>
#include "runtime/python_code_to_c_runtime.h"

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *ptr, size_t size);

static int fail_after = -1;
static int allocation_count = 0;

static int should_fail(void) {
    if (fail_after < 0) return 0;
    if (allocation_count++ == fail_after) return 1;
    return 0;
}

void *__wrap_malloc(size_t size) {
    return should_fail() ? NULL : __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size) {
    return should_fail() ? NULL : __real_calloc(count, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    return should_fail() ? NULL : __real_realloc(ptr, size);
}

static int expect_rollback_dict(void) {
    size_t before = p2c_gc_object_count();
    allocation_count = 0;
    fail_after = 1;
    if (p2c_dict_new() != NULL) return 20;
    fail_after = -1;
    if (p2c_gc_object_count() != before) return 21;
    p2c_gc_collect();
    return 0;
}

static int expect_rollback_tuple(void) {
    size_t before = p2c_gc_object_count();
    allocation_count = 0;
    fail_after = 1;
    if (p2c_tuple_new(2) != NULL) return 30;
    fail_after = -1;
    if (p2c_gc_object_count() != before) return 31;
    p2c_gc_collect();
    return 0;
}

static int expect_rollback_string(void) {
    size_t before = p2c_gc_object_count();
    allocation_count = 0;
    fail_after = 1;
    if (p2c_obj_from_str("allocation failure") != NULL) return 40;
    fail_after = -1;
    if (p2c_gc_object_count() != before) return 41;
    p2c_gc_collect();
    return 0;
}

static int expect_rollback_string_repeat(void) {
    P2C_Object *source = p2c_obj_from_str("x");
    if (!source) return 50;
    size_t before = p2c_gc_object_count();
    allocation_count = 0;
    fail_after = 1;
    if (p2c_obj_mul(source, p2c_obj_from_int(4)) != &P2C_None) return 51;
    fail_after = -1;
    if (p2c_gc_object_count() != before) return 52;
    p2c_gc_collect();
    return 0;
}

int main(void) {
    int stack_mark = 0;
    p2c_runtime_init(NULL, 0);
    p2c_gc_init(&stack_mark);
    p2c_gc_set_enabled(false);
    /* このテストは「確保が失敗したときのロールバック」を検証するため、
     * オブジェクトのフリーリストを無効化して毎回 malloc/free を通す
     * （プールが効くと注入した失敗が使われない）。 */
    p2c_obj_pool_set_enabled(false);

    int status = expect_rollback_dict();
    if (status != 0) return status;
    status = expect_rollback_tuple();
    if (status != 0) return status;
    status = expect_rollback_string();
    if (status != 0) return status;
    status = expect_rollback_string_repeat();
    if (status != 0) return status;

    p2c_gc_set_enabled(true);
    p2c_runtime_shutdown();
    return 0;
}
