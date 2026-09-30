/* tccapp.c - a program built by the ON-DEVICE compiler (TinyCC), not by gcc.
 * Linked ET_EXEC at 0x8000001000 against libcos.a. Deliberately touches
 * .data, .bss (16 KiB), a function pointer table and printf, because those are
 * the parts where a foreign linker's output most easily differs from what the
 * loader was tested against. */
#include "cos.h"
static int table_a(int x) { return x + 1; }
static int table_b(int x) { return x * 2; }
static int (*const table[])(int) = { table_a, table_b };
int initialised = 0x1234;
static char big_bss[16384];
int main(int argc, char **argv) {
    big_bss[100] = 7;
    cos_printf("tccapp argc=%d %d %d\n", argc, table[1](table[0](big_bss[100])), initialised);
    return 0;
}
