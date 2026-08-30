/*
 * micropython_bridge.c - C-OS <-> MicroPython bridge
 *
 * This file provides the symbols needed by the GUI, Python IDE and the
 * MicroPython core without depending on the older prototype integration
 * files that were left in the tree.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "micropython_interface.h"
#include "../../include/serial.h"
#include "../../include/cos_api.h"

#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/stream.h"

extern void *mp_hal_get_heap_start(void);
extern void *mp_hal_get_heap_end(void);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);
extern uint64_t get_timer_ticks(void);


// MicroPython exposes sys.stdin/stdout/stderr as global stream objects. The
// bridge defines them here so Python output is captured by the IDE instead of
// disappearing into a stub.
typedef struct _mp_dummy_t {
    mp_obj_base_t base;
    uint8_t role;
} mp_dummy_t;

static char *s_capture_output = NULL;
static size_t s_capture_output_size = 0;
static size_t s_capture_output_len = 0;

static void bridge_capture_reset(char *dst, size_t dst_sz);
static void bridge_capture_put(const char *buf, size_t len);
static void bridge_capture_print(void *self, const char *buf, size_t len);
static void bridge_set_current_script(const char *name);
static int bridge_run_source(const char *src, const char *label, char *output, size_t output_sz, bool execute);

static const mp_print_t s_bridge_capture_printer;

static mp_uint_t cos_stdio_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode);
static mp_uint_t cos_stdio_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode);
static mp_uint_t cos_stdio_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode);

static const mp_stream_p_t cos_stdio_stream_p = {
    .read = cos_stdio_read,
    .write = cos_stdio_write,
    .ioctl = cos_stdio_ioctl,
    .is_text = true,
};

static void cos_stdio_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_cos_stdio,
    MP_QSTR__lt_stdin_gt_,
    MP_TYPE_FLAG_NONE,
    print, cos_stdio_print,
    protocol, &cos_stdio_stream_p
    );

struct _mp_dummy_t mp_sys_stdin_obj = { { &mp_type_cos_stdio }, 0 };
struct _mp_dummy_t mp_sys_stdout_obj = { { &mp_type_cos_stdio }, 1 };
struct _mp_dummy_t mp_sys_stderr_obj = { { &mp_type_cos_stdio }, 2 };

static bool s_initialized = false;
static bool s_repl_active = false;
static bool s_exec_in_progress = false;
static uint64_t s_exec_count = 0;
static uint64_t s_failed_count = 0;
static uint64_t s_last_exec_time = 0;
static char s_current_script[256] = "<uninitialized>";

#define MP_DEBUG_MAX_BREAKPOINTS 32
#define MP_DEBUG_MAX_LINE_LENGTH 1024
#define MP_DEBUG_MAX_SOURCE_SIZE (64U * 1024U)

typedef struct {
    bool loaded;
    bool paused;
    char *source;
    size_t source_len;
    char label[256];
    size_t cursor;
    uint32_t current_line;
    uint32_t breakpoints[MP_DEBUG_MAX_BREAKPOINTS];
    size_t breakpoint_count;
} micropython_debug_session_t;

static micropython_debug_session_t s_debug_session;

static void debug_session_reset(void);
static int debug_session_load_source(const char *source, const char *label);
static int debug_session_load_file(const char *filename);
static bool debug_session_has_breakpoint(uint32_t line);
static int debug_session_add_breakpoint(uint32_t line);
static int debug_session_remove_breakpoint(uint32_t line);
static void debug_session_clear_breakpoints(void);
static size_t debug_copy_line(const char *src, size_t src_len, size_t pos, char *line, size_t line_sz, size_t *next_pos);
static size_t debug_trimmed_length(const char *line);
static size_t debug_line_indent(const char *line);
static bool debug_line_ends_with_colon(const char *line);
static bool debug_line_ends_with_backslash(const char *line);
static int debug_line_open_depth(const char *line, int current_depth);
static size_t debug_collect_chunk(char *chunk, size_t chunk_sz, uint32_t *start_line, uint32_t *end_line, bool *has_more);
static int debug_execute_chunk_once(char *output, size_t output_sz);
static int debug_execute_until_breakpoint(bool stop_after_one, char *output, size_t output_sz);

static void bridge_copy_output(char *dst, size_t dst_sz, const char *src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < dst_sz && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void bridge_make_error(char *dst, size_t dst_sz, const char *msg) {
    bridge_copy_output(dst, dst_sz, msg ? msg : "MicroPython error");
}


static void debug_session_reset(void) {
    if (s_debug_session.source) {
        kfree(s_debug_session.source);
    }
    memset(&s_debug_session, 0, sizeof(s_debug_session));
}

static size_t debug_trimmed_length(const char *line) {
    if (!line) {
        return 0;
    }
    size_t len = strlen(line);
    while (len > 0) {
        char c = line[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        --len;
    }
    return len;
}

static size_t debug_line_indent(const char *line) {
    if (!line) {
        return 0;
    }
    size_t indent = 0;
    while (line[indent] == ' ' || line[indent] == '\t') {
        ++indent;
    }
    return indent;
}

static bool debug_line_is_blank_or_comment(const char *line) {
    if (!line) {
        return true;
    }
    size_t len = debug_trimmed_length(line);
    if (len == 0) {
        return true;
    }
    size_t i = 0;
    while (line[i] == ' ' || line[i] == '\t') {
        ++i;
    }
    return line[i] == '#';
}

static bool debug_line_ends_with_colon(const char *line) {
    if (!line) {
        return false;
    }
    size_t len = debug_trimmed_length(line);
    return (len > 0 && line[len - 1] == ':');
}

static bool debug_line_ends_with_backslash(const char *line) {
    if (!line) {
        return false;
    }
    size_t len = debug_trimmed_length(line);
    return (len > 0 && line[len - 1] == '\\');
}

static int debug_line_open_depth(const char *line, int current_depth) {
    if (!line) {
        return current_depth;
    }
    bool in_single = false;
    bool in_double = false;
    bool escape = false;
    int depth = current_depth;
    for (size_t i = 0; line[i] != '\0'; ++i) {
        char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\' && (in_single || in_double)) {
            escape = true;
            continue;
        }
        if (!in_double && c == '\'') {
            in_single = !in_single;
            continue;
        }
        if (!in_single && c == '"') {
            in_double = !in_double;
            continue;
        }
        if (in_single || in_double) {
            continue;
        }
        if (c == '#') {
            break;
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            if (depth > 0) {
                --depth;
            }
        }
    }
    return depth;
}

static size_t debug_copy_line(const char *src, size_t src_len, size_t pos, char *line, size_t line_sz, size_t *next_pos) {
    if (!src || !line || line_sz == 0 || pos >= src_len) {
        if (next_pos) {
            *next_pos = pos;
        }
        if (line && line_sz > 0) {
            line[0] = '\0';
        }
        return 0;
    }

    size_t i = 0;
    while (pos < src_len && src[pos] != '\n' && src[pos] != '\r') {
        if (i + 1 < line_sz) {
            line[i++] = src[pos];
        }
        ++pos;
    }
    line[i] = '\0';

    while (pos < src_len && (src[pos] == '\r' || src[pos] == '\n')) {
        if (src[pos] == '\r' && pos + 1 < src_len && src[pos + 1] == '\n') {
            pos += 2;
            break;
        }
        ++pos;
        if (src[pos - 1] == '\n') {
            break;
        }
    }

    if (next_pos) {
        *next_pos = pos;
    }
    return i;
}

static bool debug_session_has_breakpoint(uint32_t line) {
    for (size_t i = 0; i < s_debug_session.breakpoint_count; ++i) {
        if (s_debug_session.breakpoints[i] == line) {
            return true;
        }
    }
    return false;
}

static int debug_session_add_breakpoint(uint32_t line) {
    if (line == 0) {
        return -1;
    }
    if (debug_session_has_breakpoint(line)) {
        return 0;
    }
    if (s_debug_session.breakpoint_count >= MP_DEBUG_MAX_BREAKPOINTS) {
        return -1;
    }
    s_debug_session.breakpoints[s_debug_session.breakpoint_count++] = line;
    return 0;
}

static int debug_session_remove_breakpoint(uint32_t line) {
    for (size_t i = 0; i < s_debug_session.breakpoint_count; ++i) {
        if (s_debug_session.breakpoints[i] == line) {
            for (size_t j = i + 1; j < s_debug_session.breakpoint_count; ++j) {
                s_debug_session.breakpoints[j - 1] = s_debug_session.breakpoints[j];
            }
            --s_debug_session.breakpoint_count;
            return 0;
        }
    }
    return -1;
}

static void debug_session_clear_breakpoints(void) {
    s_debug_session.breakpoint_count = 0;
    memset(s_debug_session.breakpoints, 0, sizeof(s_debug_session.breakpoints));
}

static int debug_session_load_source(const char *source, const char *label) {
    debug_session_reset();
    if (!source) {
        return -1;
    }

    size_t len = strlen(source);
    if (len > MP_DEBUG_MAX_SOURCE_SIZE) {
        len = MP_DEBUG_MAX_SOURCE_SIZE;
    }

    s_debug_session.source = (char *)kmalloc(len + 1);
    if (!s_debug_session.source) {
        return -1;
    }
    memcpy(s_debug_session.source, source, len);
    s_debug_session.source[len] = '\0';
    s_debug_session.source_len = len;
    s_debug_session.cursor = 0;
    s_debug_session.current_line = 1;
    s_debug_session.loaded = true;
    s_debug_session.paused = false;
    s_debug_session.breakpoint_count = 0;
    bridge_copy_output(s_debug_session.label, sizeof(s_debug_session.label), label ? label : "<string>");
    return 0;
}

static int debug_session_load_file(const char *filename) {
    if (!filename || filename[0] == '\0') {
        return -1;
    }
    char *buf = (char *)kmalloc(MP_DEBUG_MAX_SOURCE_SIZE + 1);
    if (!buf) {
        return -1;
    }
    int rc = cos_fs_read_file(filename, buf, MP_DEBUG_MAX_SOURCE_SIZE);
    if (rc < 0) {
        kfree(buf);
        return -1;
    }
    if ((size_t)rc > MP_DEBUG_MAX_SOURCE_SIZE) {
        rc = (int)MP_DEBUG_MAX_SOURCE_SIZE;
    }
    buf[rc] = '\0';
    int load_rc = debug_session_load_source(buf, filename);
    kfree(buf);
    return load_rc;
}

static size_t debug_collect_chunk(char *chunk, size_t chunk_sz, uint32_t *start_line, uint32_t *end_line, bool *has_more) {
    if (!s_debug_session.loaded || !s_debug_session.source || !chunk || chunk_sz == 0) {
        return 0;
    }

    chunk[0] = '\0';
    size_t used = 0;
    size_t pos = s_debug_session.cursor;
    uint32_t line_no = s_debug_session.current_line;
    uint32_t chunk_start_line = 0;
    uint32_t chunk_end_line = 0;
    bool started = false;
    bool block_mode = false;
    bool continuation = false;
    int open_depth = 0;
    size_t base_indent = 0;
    char line[MP_DEBUG_MAX_LINE_LENGTH];

    while (pos < s_debug_session.source_len) {
        size_t next_pos = pos;
        debug_copy_line(s_debug_session.source, s_debug_session.source_len, pos, line, sizeof(line), &next_pos);
        bool blank_or_comment = debug_line_is_blank_or_comment(line);
        if (!started) {
            if (blank_or_comment) {
                pos = next_pos;
                ++line_no;
                continue;
            }
            started = true;
            chunk_start_line = line_no;
            base_indent = debug_line_indent(line);
            block_mode = debug_line_ends_with_colon(line);
        }

        size_t line_len = strlen(line);
        if (used + line_len + 2 >= chunk_sz) {
            break;
        }
        memcpy(chunk + used, line, line_len);
        used += line_len;
        chunk[used++] = '\n';
        chunk[used] = '\0';
        chunk_end_line = line_no;

        continuation = debug_line_ends_with_backslash(line);
        open_depth = debug_line_open_depth(line, open_depth);

        pos = next_pos;
        ++line_no;

        if (pos >= s_debug_session.source_len) {
            break;
        }

        size_t look_pos = pos;
        uint32_t look_line_no = line_no;
        char look_line[MP_DEBUG_MAX_LINE_LENGTH];
        size_t look_next = look_pos;
        bool found_next = false;
        while (look_pos < s_debug_session.source_len) {
            debug_copy_line(s_debug_session.source, s_debug_session.source_len, look_pos, look_line, sizeof(look_line), &look_next);
            if (!debug_line_is_blank_or_comment(look_line)) {
                found_next = true;
                break;
            }
            if (used + strlen(look_line) + 2 < chunk_sz) {
                size_t blank_len = strlen(look_line);
                memcpy(chunk + used, look_line, blank_len);
                used += blank_len;
                chunk[used++] = '\n';
                chunk[used] = '\0';
            }
            look_pos = look_next;
            ++look_line_no;
            pos = look_pos;
            line_no = look_line_no;
        }

        if (!found_next) {
            pos = look_pos;
            line_no = look_line_no;
            break;
        }

        size_t next_indent = debug_line_indent(look_line);
        if (open_depth > 0 || continuation) {
            continue;
        }
        if (block_mode) {
            if (next_indent > base_indent) {
                continue;
            }
            break;
        }
        if (next_indent > base_indent) {
            continue;
        }
        break;
    }

    s_debug_session.cursor = pos;
    s_debug_session.current_line = line_no;
    if (start_line) {
        *start_line = chunk_start_line;
    }
    if (end_line) {
        *end_line = chunk_end_line;
    }
    if (has_more) {
        *has_more = (pos < s_debug_session.source_len);
    }
    return used;
}

static int debug_execute_chunk_once(char *output, size_t output_sz) {
    char chunk[MP_DEBUG_MAX_SOURCE_SIZE + 1];
    uint32_t start_line = 0;
    uint32_t end_line = 0;
    bool has_more = false;
    size_t len = debug_collect_chunk(chunk, sizeof(chunk), &start_line, &end_line, &has_more);
    if (len == 0) {
        bridge_copy_output(output, output_sz, has_more ? "Debug paused" : "Debug finished");
        s_debug_session.paused = false;
        return 0;
    }

    ++s_exec_count;
    int rc = bridge_run_source(chunk, s_debug_session.label, output, output_sz, true);
    if (rc == MICROPYTHON_OK && has_more) {
        s_debug_session.paused = true;
        s_debug_session.current_line = end_line + 1;
        if (output && output[0] == '\0') {
            char msg[128];
            snprintf(msg, sizeof(msg), "Paused at line %u", (unsigned)end_line);
            bridge_copy_output(output, output_sz, msg);
        }
        return 1;
    }

    if (rc == MICROPYTHON_OK) {
        s_debug_session.paused = false;
    }
    return rc;
}

static int debug_execute_until_breakpoint(bool stop_after_one, char *output, size_t output_sz) {
    if (!s_debug_session.loaded) {
        bridge_make_error(output, output_sz, "Debug source not loaded");
        return MICROPYTHON_ERROR;
    }

    if (stop_after_one) {
        return debug_execute_chunk_once(output, output_sz);
    }

    while (s_debug_session.cursor < s_debug_session.source_len) {
        uint32_t start_line = 0;
        uint32_t end_line = 0;
        bool has_more = false;
        char chunk[MP_DEBUG_MAX_SOURCE_SIZE + 1];
        size_t len = debug_collect_chunk(chunk, sizeof(chunk), &start_line, &end_line, &has_more);
        if (len == 0) {
            s_debug_session.paused = false;
            bridge_copy_output(output, output_sz, "Debug finished");
            return 0;
        }

        if (debug_session_has_breakpoint(start_line)) {
            s_debug_session.paused = true;
            char msg[128];
            snprintf(msg, sizeof(msg), "Paused at breakpoint line %u", (unsigned)start_line);
            bridge_copy_output(output, output_sz, msg);
            return 1;
        }

        ++s_exec_count;
        int rc = bridge_run_source(chunk, s_debug_session.label, output, output_sz, true);
        if (rc != MICROPYTHON_OK) {
            s_debug_session.paused = false;
            return rc;
        }
        if (debug_session_has_breakpoint(end_line + 1) && s_debug_session.cursor < s_debug_session.source_len) {
            s_debug_session.paused = true;
            char msg[128];
            snprintf(msg, sizeof(msg), "Paused at breakpoint line %u", (unsigned)(end_line + 1));
            bridge_copy_output(output, output_sz, msg);
            return 1;
        }
        if (!has_more) {
            s_debug_session.paused = false;
            bridge_copy_output(output, output_sz, "Debug finished");
            return 0;
        }
    }

    s_debug_session.paused = false;
    bridge_copy_output(output, output_sz, "Debug finished");
    return 0;
}

static void bridge_set_current_script(const char *name) {
    if (name == NULL || name[0] == '\0') {
        bridge_copy_output(s_current_script, sizeof(s_current_script), "<string>");
        return;
    }
    bridge_copy_output(s_current_script, sizeof(s_current_script), name);
}

static int bridge_run_source(const char *src, const char *label, char *output, size_t output_sz, bool execute) {
    if (!src) {
        bridge_make_error(output, output_sz, "No source");
        return MICROPYTHON_ERROR;
    }

    bridge_set_current_script(label);
    bridge_capture_reset(output, output_sz);
    s_exec_in_progress = true;
    uint64_t start = get_timer_ticks();

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, src, strlen(src), 0);
        mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t fun = mp_compile(&tree, lex->source_name, false);
        if (execute) {
            mp_call_function_0(fun);
        }
        nlr_pop();

        s_last_exec_time = get_timer_ticks() - start;
        s_exec_in_progress = false;
        if (output && output[0] == '\0') {
            bridge_copy_output(output, output_sz, execute ? "OK" : "Syntax OK");
        }
        return MICROPYTHON_OK;
    }

    s_last_exec_time = get_timer_ticks() - start;
    s_exec_in_progress = false;
    if (execute) {
        s_failed_count++;
    }
    if (output) {
        if (output[0] == '\0') {
            mp_obj_print_exception(&s_bridge_capture_printer, (mp_obj_t)nlr.ret_val);
            if (output[0] == '\0') {
                bridge_make_error(output, output_sz, "MicroPython exception");
            }
        } else {
            bridge_capture_put("\n", 1);
            mp_obj_print_exception(&s_bridge_capture_printer, (mp_obj_t)nlr.ret_val);
        }
    }
    return MICROPYTHON_ERROR;
}

static void bridge_capture_reset(char *dst, size_t dst_sz) {
    s_capture_output = dst;
    s_capture_output_size = dst_sz;
    s_capture_output_len = 0;
    if (dst && dst_sz > 0) {
        dst[0] = '\0';
    }
}

static void bridge_capture_put(const char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        serial_putc(buf[i]);
    }

    if (s_capture_output == NULL || s_capture_output_size == 0) {
        return;
    }

    while (len > 0 && s_capture_output_len + 1 < s_capture_output_size) {
        size_t room = s_capture_output_size - 1 - s_capture_output_len;
        size_t n = len < room ? len : room;
        memcpy(s_capture_output + s_capture_output_len, buf, n);
        s_capture_output_len += n;
        len -= n;
        buf += n;
    }
    s_capture_output[s_capture_output_len] = '\0';
}

static void bridge_capture_print(void *self, const char *buf, size_t len) {
    (void)self;
    bridge_capture_put(buf, len);
}

static const mp_print_t s_bridge_capture_printer = {NULL, bridge_capture_print};

static void cos_stdio_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    const mp_dummy_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = "stdio";
    if (self->role == 0) {
        name = "stdin";
    } else if (self->role == 1) {
        name = "stdout";
    } else if (self->role == 2) {
        name = "stderr";
    }
    mp_printf(print, "<C-OS %s>", name);
}

static mp_uint_t cos_stdio_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    if (size == 0) {
        *errcode = 0;
        return 0;
    }
    int ch = serial_getc();
    if (ch < 0) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    ((char *)buf)[0] = (char)ch;
    *errcode = 0;
    return 1;
}

static mp_uint_t cos_stdio_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    (void)self_in;
    (void)errcode;
    bridge_capture_put((const char *)buf, (size_t)size);
    return size;
}

static mp_uint_t cos_stdio_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_dummy_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_STREAM_FLUSH:
        case MP_STREAM_CLOSE:
            return 0;
        case MP_STREAM_POLL:
            if ((arg & MP_STREAM_POLL_WR) != 0) {
                return MP_STREAM_POLL_WR;
            }
            if ((arg & MP_STREAM_POLL_RD) != 0 && self->role == 0) {
                return MP_STREAM_POLL_RD;
            }
            return 0;
        case MP_STREAM_GET_BUFFER_SIZE:
            return 128;
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

void __assert_fail(const char *expr, const char *file, unsigned int line, const char *func) {
    (void)expr; (void)file; (void)line; (void)func;
    serial_puts("ASSERT FAILED\n");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

int micropython_init(void) {
    if (s_initialized) {
        return MICROPYTHON_OK;
    }
    gc_init(mp_hal_get_heap_start(), mp_hal_get_heap_end());
    mp_init();
    s_initialized = true;
    s_repl_active = false;
    s_exec_in_progress = false;
    s_exec_count = 0;
    s_failed_count = 0;
    s_last_exec_time = 0;
    bridge_set_current_script("<boot>");
    debug_session_reset();
    return MICROPYTHON_OK;
}

int micropython_deinit(void) {
    if (!s_initialized) {
        return MICROPYTHON_OK;
    }
    mp_deinit();
    s_initialized = false;
    s_repl_active = false;
    s_exec_in_progress = false;
    debug_session_reset();
    return MICROPYTHON_OK;
}

int micropython_execute(const char *code, char *output, size_t output_size) {
    if (!s_initialized) {
        int rc = micropython_init();
        if (rc != MICROPYTHON_OK) {
            bridge_make_error(output, output_size, "MicroPython init failed");
            return rc;
        }
    }
    ++s_exec_count;
    return bridge_run_source(code, "<string>", output, output_size, true);
}

int micropython_debug_load_source(const char *source, const char *label) {
    return debug_session_load_source(source, label);
}

int micropython_debug_load_file(const char *filename) {
    return debug_session_load_file(filename);
}

int micropython_debug_add_breakpoint(uint32_t line) {
    return debug_session_add_breakpoint(line);
}

int micropython_debug_remove_breakpoint(uint32_t line) {
    return debug_session_remove_breakpoint(line);
}

int micropython_debug_clear_breakpoints(void) {
    debug_session_clear_breakpoints();
    return 0;
}

int micropython_debug_step(char *output, size_t output_size) {
    return debug_execute_until_breakpoint(true, output, output_size);
}

int micropython_debug_continue(char *output, size_t output_size) {
    return debug_execute_until_breakpoint(false, output, output_size);
}

bool micropython_debug_is_loaded(void) {
    return s_debug_session.loaded;
}

bool micropython_debug_is_paused(void) {
    return s_debug_session.paused;
}

uint32_t micropython_debug_current_line(void) {
    return s_debug_session.current_line;
}

const char *micropython_debug_current_label(void) {
    return s_debug_session.label;
}

int micropython_start_repl(void) {
    if (!s_initialized && micropython_init() != MICROPYTHON_OK) {
        return MICROPYTHON_ERROR;
    }
    s_repl_active = true;
    bridge_set_current_script("<repl>");
    return MICROPYTHON_OK;
}

int micropython_stop_repl(void) {
    s_repl_active = false;
    bridge_set_current_script("<idle>");
    return MICROPYTHON_OK;
}

bool micropython_is_initialized(void) {
    return s_initialized;
}

bool micropython_is_repl_active(void) {
    return s_repl_active;
}

bool micropython_is_execution_active(void) {
    return s_exec_in_progress;
}

uint64_t micropython_get_execution_count(void) {
    return s_exec_count;
}

uint64_t micropython_get_total_executions(void) {
    return s_exec_count;
}

uint64_t micropython_get_failed_executions(void) {
    return s_failed_count;
}

uint64_t micropython_get_last_execution_time(void) {
    return s_last_exec_time;
}

float micropython_get_success_rate(void) {
    if (s_exec_count == 0) {
        return 100.0f;
    }
    uint64_t ok = (s_exec_count > s_failed_count) ? (s_exec_count - s_failed_count) : 0;
    return (float)((ok * 100.0) / (double)s_exec_count);
}

const char *micropython_get_current_script(void) {
    return s_current_script;
}

void micropython_print_statistics(void) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "[MICROPYTHON] exec=%llu failed=%llu last=%llu ms current=%s\n",
                     (unsigned long long)s_exec_count,
                     (unsigned long long)s_failed_count,
                     (unsigned long long)s_last_exec_time,
                     s_current_script);
    if (n > 0) {
        serial_puts(buf);
    }
}

micropython_result_t micropython_register_cos_module(void) {
    return MICROPYTHON_OK;
}

micropython_result_t micropython_import_cos_module(const char *module_name) {
    (void)module_name;
    return MICROPYTHON_OK;
}

int micropython_execute_string(const char *code, char *output, size_t output_size) {
    return micropython_execute(code, output, output_size);
}

int micropython_execute_file(const char *filename, char *output, size_t output_size) {
    if (!filename || filename[0] == '\0') {
        bridge_make_error(output, output_size, "No filename");
        return MICROPYTHON_ERROR;
    }

    bridge_set_current_script(filename);

    char *buf = (char *)kmalloc(65537);
    if (!buf) {
        bridge_make_error(output, output_size, "Out of memory");
        return MICROPYTHON_MEMORY_ERROR;
    }

    int rc = cos_fs_read_file(filename, buf, 65536);
    if (rc < 0) {
        kfree(buf);
        bridge_make_error(output, output_size, "File not found");
        return MICROPYTHON_ERROR;
    }

    if ((size_t)rc < 65536) {
        buf[rc] = '\0';
    } else {
        buf[65536] = '\0';
    }

    ++s_exec_count;
    rc = bridge_run_source(buf, filename, output, output_size, true);
    kfree(buf);
    return rc;
}

int micropython_execute_script(const char *script_path, const char *input, char *output, size_t output_size) {
    if (input && input[0]) {
        return micropython_execute_string(input, output, output_size);
    }
    return micropython_execute_file(script_path, output, output_size);
}

int micropython_check_syntax(const char *code, char *error_msg, size_t msg_size) {
    if (!code || !error_msg || msg_size == 0) {
        return MICROPYTHON_ERROR;
    }
    return bridge_run_source(code, "<syntax>", error_msg, msg_size, false);
}

int micropython_integration_init(void) {
    return micropython_init();
}

void micropython_integration_cleanup(void) {
    micropython_deinit();
}

int micropython_execute_ai_command(const char *command, const char *input, char *output, size_t output_size) {
    if (!command) {
        bridge_make_error(output, output_size, "No command");
        return MICROPYTHON_ERROR;
    }

    if (strcmp(command, "run") == 0 || strcmp(command, "exec") == 0) {
        return micropython_execute_string(input ? input : "", output, output_size);
    }
    if (strcmp(command, "file") == 0) {
        return micropython_execute_file(input, output, output_size);
    }
    if (strcmp(command, "syntax") == 0) {
        return micropython_check_syntax(input ? input : "", output, output_size);
    }
    if (strcmp(command, "repl") == 0) {
        return micropython_start_repl();
    }
    if (strcmp(command, "stop") == 0) {
        return micropython_stop_repl();
    }

    bridge_make_error(output, output_size, "Unknown command");
    return MICROPYTHON_ERROR;
}

int micropython_init_cos_modules(void) {
    serial_puts("[MICROPYTHON] COS modules are available through the bridge\n");
    return 0;
}
