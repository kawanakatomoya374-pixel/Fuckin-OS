#include <stdio.h>
#include <stdlib.h>
long getline(char **lp, size_t *n, FILE *f) {
    size_t cap = *n, len = 0; char *b = *lp; int c = 0;
    if (!b || !cap) { cap = 128; b = (char*)malloc(cap); }
    while ((c = fgetc(f)) != EOF) { if (len + 2 > cap) { cap *= 2; b = (char*)realloc(b, cap); } b[len++] = (char)c; if (c == '\n') break; }
    if (!len) { *lp = b; *n = cap; return -1; }
    b[len] = 0; *lp = b; *n = cap; return (long)len;
}
