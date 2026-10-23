/*
 * tcc_lib_stub.c - TinyCC library stubs for C-OS
 * 
 * Phase 1: TinyCC 統合 - 実際の libtcc.c 実装が利用できない場合のスタブ
 */

#include <stdint.h>
#include <string.h>
#include "serial.h"

/* TCCState の実体 (簡易版) */
typedef struct TCCState {
    int output_type;
    char include_paths[4][256];
    int include_path_count;
    char defines[10][64];
    int define_count;
    void *error_opaque;
    void (*error_func)(void*, const char*);
} TCCState;

TCCState* tcc_new(void) {
    serial_puts("[TCC] tcc_new() called (stub)\n");
    /* 簡易アロケーション (実際には kmalloc を使用) */
    extern void* kmalloc(size_t size);
    TCCState *s = (TCCState*)kmalloc(sizeof(TCCState));
    if (s) {
        memset(s, 0, sizeof(TCCState));
        s->include_path_count = 0;
        s->define_count = 0;
    }
    return s;
}

void tcc_delete(TCCState* s) {
    serial_puts("[TCC] tcc_delete() called (stub)\n");
    if (s) {
        extern void kfree(void* ptr);
        kfree(s);
    }
}

int tcc_set_output_type(TCCState* s, int output_type) {
    (void)s; (void)output_type;
    serial_puts("[TCC] tcc_set_output_type() called (stub)\n");
    return 0;
}

int tcc_add_include_path(TCCState* s, const char* pathname) {
    if (!s || !pathname) return -1;
    if (s->include_path_count >= 4) return -1;
    strncpy(s->include_paths[s->include_path_count++], pathname, 255);
    serial_puts("[TCC] tcc_add_include_path(): ");
    serial_puts(pathname);
    serial_puts("\n");
    return 0;
}

int tcc_add_sysinclude_path(TCCState* s, const char* pathname) {
    return tcc_add_include_path(s, pathname);
}

void tcc_define_symbol(TCCState* s, const char* sym, const char* value) {
    if (!s || !sym) return;
    if (s->define_count >= 10) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s=%s", sym, value ? value : "1");
    strncpy(s->defines[s->define_count++], buf, 63);
    serial_puts("[TCC] tcc_define_symbol(): ");
    serial_puts(buf);
    serial_puts("\n");
}

int tcc_compile_string(TCCState* s, const char* buf) {
    (void)s; (void)buf;
    serial_puts("[TCC] tcc_compile_string() called (stub - not implemented)\n");
    return -1; /* Not implemented in stub */
}

int tcc_add_file(TCCState* s, const char* filename) {
    (void)s; (void)filename;
    serial_puts("[TCC] tcc_add_file(): ");
    serial_puts(filename);
    serial_puts(" (stub - not implemented)\n");
    return -1; /* Not implemented in stub */
}

int tcc_output_file(TCCState* s, const char* filename) {
    (void)s; (void)filename;
    serial_puts("[TCC] tcc_output_file(): ");
    serial_puts(filename);
    serial_puts(" (stub - not implemented)\n");
    return -1; /* Not implemented in stub */
}

int tcc_relocate(TCCState* s) {
    (void)s;
    serial_puts("[TCC] tcc_relocate() called (stub)\n");
    return -1; /* Not implemented in stub */
}

void* tcc_get_symbol(TCCState* s, const char* name) {
    (void)s; (void)name;
    serial_puts("[TCC] tcc_get_symbol(): ");
    serial_puts(name);
    serial_puts(" (stub - returns NULL)\n");
    return NULL;
}

void tcc_set_error_func(TCCState* s, void* error_opaque, void* error_func) {
    if (!s) return;
    s->error_opaque = error_opaque;
    s->error_func = (void(*)(void*, const char*))error_func;
    serial_puts("[TCC] tcc_set_error_func() called (stub)\n");
}

/* snprintf スタブ */
int snprintf(char* str, size_t size, const char* format, ...) {
    (void)str; (void)size; (void)format;
    return 0;
}
