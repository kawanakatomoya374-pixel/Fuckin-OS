/* Fixture: position-independent executable. Every pointer in .data needs
 * an R_X86_64_RELATIVE (or a RELR entry), so this is the case that
 * proves relative relocation actually happened. */
__attribute__((used)) volatile int g_bss_must_be_zero;
__attribute__((used)) const char g_rodata[] = "COS_PIE_MARKER";
/* A pointer to another object: this is what produces the relative
 * relocation. Its value on disk is the LINK-time address; after loading
 * it must equal the RUNTIME address of g_rodata. */
__attribute__((used)) const char *g_ptr = g_rodata;
__attribute__((used)) void *g_selfptr;

long value(void) { return 0xBEEF; }
void _start(void) { g_selfptr = (void *)&g_ptr; for (;;) { } }
