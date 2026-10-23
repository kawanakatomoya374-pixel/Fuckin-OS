/* vec_main.c - the program of the two-file single-header example (see cosproj.txt: build it from C-OS Studio). vec_util.c holds the implementation. */
#include <stdio.h>
#define MINI_VEC_IMPLEMENTATION
#include "mini_vec.h"
int fill(mini_vec *v, int n);
int main(void) {
    mini_vec v = { 0 };
    fill(&v, 10);
    printf("vec len=%d sum=%d\n", (int)v.len, mini_vec_sum(&v));
    mini_vec_free(&v);
    return 0;
}
