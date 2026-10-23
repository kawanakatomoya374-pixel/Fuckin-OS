/* mini_vec.h - a tiny single-header library written the stb way: include it anywhere for the declarations, and in exactly one .c file
 * write  #define MINI_VEC_IMPLEMENTATION  before the include to get the code. */
#ifndef MINI_VEC_H
#define MINI_VEC_H
#include <stddef.h>
typedef struct { int *data; size_t len, cap; } mini_vec;
void mini_vec_push(mini_vec *v, int x);
int  mini_vec_sum(const mini_vec *v);
void mini_vec_free(mini_vec *v);
#endif

#ifdef MINI_VEC_IMPLEMENTATION
#include <stdlib.h>
void mini_vec_push(mini_vec *v, int x) {
    if (v->len == v->cap) { v->cap = v->cap ? v->cap * 2 : 4; v->data = realloc(v->data, v->cap * sizeof *v->data); }
    v->data[v->len++] = x;
}
int mini_vec_sum(const mini_vec *v) { int s = 0; for (size_t i = 0; i < v->len; i++) s += v->data[i]; return s; }
void mini_vec_free(mini_vec *v) { free(v->data); v->data = NULL; v->len = v->cap = 0; }
#endif
