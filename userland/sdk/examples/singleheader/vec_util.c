/* vec_util.c - uses mini_vec.h without the implementation (it is compiled into vec_main.c). */
#include "mini_vec.h"
int fill(mini_vec *v, int n) { for (int i = 1; i <= n; i++) mini_vec_push(v, i * i); return (int)v->len; }
