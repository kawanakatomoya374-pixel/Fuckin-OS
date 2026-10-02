/*
 * Freestanding exception machinery regression.
 *
 * PYTHON_CODE_TO_C_NO_STDLIB has no platform setjmp/longjmp, so a kernel
 * provides them for the runtime's jmp_buf layout
 * (typedef struct { void *buf[16]; } jmp_buf[1];). This test links the x86-64
 * reference implementation and exercises exactly the patterns the generated
 * code uses:
 *   - raise/longjmp caught by a handler frame in the same function
 *   - reading the exception object bound by "except E as name"
 *   - longjmp from a deeper call frame (iterator StopIteration)
 *   - nested frames, unhandled propagation, and frame pointer restore
 */
#include "platform/python_code_to_c_embed.h"
#include "host_stack_bounds.h"

#ifdef P2C_EMBED_TEST_TRACE
/* 段階の進行を1文字で出力する（デバッグ用）。 */
extern long write(int fd, const void *buf, unsigned long count);
static void trace_stage(char tag) { (void)write(2, &tag, 1); }
#else
static void trace_stage(char tag) { (void)tag; }
#endif

static unsigned char heap_storage[64u * 1024u];
static P2C_EmbedHeap heap;

/* generated-code equivalent: try/except with a bound exception name */
static int catch_bound_exception(void) {
    trace_stage('B');
    P2C_Object * volatile exc = &P2C_None;
    {
        P2C_ExceptFrame ef;
        ef.prev = p2c_exc_stack;
        ef.exc = NULL;
        p2c_exc_stack = &ef;
        int jmp = setjmp(ef.env);
        int handled = 0;
        if (jmp == 0) {
            p2c_raise(p2c_make_exception("ValueError", "boom"));
        } else if (p2c_exc_name_match(ef.exc, "ValueError")) {
            handled = 1;
            p2c_exc_stack = ef.prev;
            exc = ef.exc;
            P2C_Object *text = p2c_obj_str(exc);
            const char *msg = p2c_obj_as_str(text);
            if (!msg || strcmp(msg, "boom") != 0) return 21;
        }
        p2c_exc_stack = ef.prev;
        if (jmp != 0 && !handled) p2c_raise(ef.exc);
    }
    if (exc != &P2C_None && p2c_exc_name_match(exc, "ValueError")) return 0;
    return 22;
}

static P2C_Object *deep_raise(int depth) {
    if (depth > 0) {
        P2C_Object *inner = deep_raise(depth - 1);
        if (inner == &P2C_None) return &P2C_None;
    }
    if (depth == 0) p2c_raise(p2c_make_exception("StopIteration", ""));
    return &P2C_None;
}

static int catch_from_deeper_frame(void) {
    trace_stage('D');
    P2C_ExceptFrame ef;
    ef.prev = p2c_exc_stack;
    ef.exc = NULL;
    p2c_exc_stack = &ef;
    if (setjmp(ef.env) == 0) {
        (void)deep_raise(6);
        p2c_exc_stack = ef.prev;
        return 31;
    }
    p2c_exc_stack = ef.prev;
    if (!p2c_exc_name_match(ef.exc, "StopIteration")) return 32;
    return 0;
}

static int nested_frames(void) {
    trace_stage('N');
    P2C_ExceptFrame outer;
    outer.prev = p2c_exc_stack;
    outer.exc = NULL;
    p2c_exc_stack = &outer;
    if (setjmp(outer.env) != 0) {
        p2c_exc_stack = outer.prev;
        return p2c_exc_name_match(outer.exc, "RuntimeError") ? 0 : 41;
    }
    P2C_ExceptFrame inner;
    inner.prev = p2c_exc_stack;
    inner.exc = NULL;
    p2c_exc_stack = &inner;
    if (setjmp(inner.env) == 0) {
        p2c_raise(p2c_make_exception("KeyError", "missing"));
        p2c_exc_stack = inner.prev;
        p2c_exc_stack = outer.prev;
        return 42;
    }
    /* 内側では捕まえない: 外側へ再送出する */
    p2c_exc_stack = inner.prev;
    p2c_raise(p2c_make_exception("RuntimeError", "converted"));
    return 43;
}

int main(void) {
    if (p2c_embed_heap_init(&heap, heap_storage, sizeof(heap_storage)) != 0) return 10;
    p2c_embed_heap_set_default(&heap);
    P2C_EmbedConfig cfg;
    p2c_embed_config_init(&cfg);
    p2c_embed_config_use_heap(&cfg, &heap, heap_storage, sizeof(heap_storage));
    cfg.raise_memory_error = true;
    void *stack_lo = NULL;
    void *stack_hi = NULL;
    p2c_host_stack_bounds(&stack_lo, &stack_hi);
    p2c_embed_config_use_stack(&cfg, stack_lo, stack_hi);
    if (p2c_embed_start(&cfg) != 0) return 11;

    int status = catch_bound_exception();
    if (status != 0) { p2c_embed_stop(); return status; }
    status = catch_from_deeper_frame();
    if (status != 0) { p2c_embed_stop(); return status; }
    status = nested_frames();
    if (status != 0) { p2c_embed_stop(); return status; }

    p2c_embed_stop();
    return 0;
}
