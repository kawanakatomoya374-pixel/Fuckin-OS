/* tcclib.c - a shared library (.c-osll) built by the ON-DEVICE compiler: `tcc -shared`.
 * It carries what a foreign linker's ET_DYN is likeliest to get different from gcc's:
 * a function, initialised data, and a table of POINTERS (which needs load-time
 * relocations - RELATIVE or 64 - that the loader has to apply for the bias it chose). */
int tcclib_add(int a, int b) { return a + b; }
int tcclib_data = 0x7CC1;
static int hidden(int x) { return x * 3; }
int tcclib_triple(int x) { return hidden(x); }
typedef int (*binop)(int, int);
const binop tcclib_table[2] = { tcclib_add, 0 };       /* pointer in .data.rel.ro / .data: needs a relocation */
int *tcclib_ptr = &tcclib_data;                         /* pointer to data: also needs one */
