/* Standard library internals shared by the lib*.c parts. */
#ifndef LIB_INT_H
#define LIB_INT_H

/* Standard library: prelude, builtin modules, string/array methods (SPEC 8, 15) */
#define _POSIX_C_SOURCE 200809L
#include <stdatomic.h>
#include "sp.h"
#include <ctype.h>
#include <errno.h>
#include <time.h>

/* functions shared between the parts of this stage */
int sp_same(SpValue a, SpValue b);
int need_int(VM *vm, SpValue v, const char *what, int64_t *out);
int need_str(VM *vm, SpValue v, const char *what);
int need_num(VM *vm, SpValue v, const char *what, double *out);
SpValue vstr(VM *vm, const char *s, size_t n);
int find_bytes(const char *h, size_t hl, const char *nd, size_t nl, size_t start);
void sink_stdout(void *ud, const char *s, size_t n);
int n_has(VM *vm, int argc, SpValue *argv, SpValue *ret);
int arr_join(VM *vm, SArray *a, SString *sep, SpValue *ret);
int sm_contains(VM *vm, int argc, SpValue *argv, SpValue *ret);
int n_in(VM *vm, int argc, SpValue *argv, SpValue *ret);
/* native function tables of the builtin modules (registered in lib.c) */
extern const NativeDesc str_fns[], table_fns[], io_fns[], sys_fns[], time_fns[], gc_fns[], co_fns[];
extern const NativeDesc string_methods[];
extern const ModConst math_consts[];

#endif
