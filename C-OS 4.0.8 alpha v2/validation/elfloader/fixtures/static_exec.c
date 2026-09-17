/* Fixture: classic statically linked non-PIE ET_EXEC - the ONLY shape
 * the old loader accepted. Kept so the rewrite is proven not to have
 * regressed the case that already worked. */
__attribute__((used)) volatile int g_bss_must_be_zero;
__attribute__((used)) volatile int g_data = 0x11223344;
__attribute__((used)) const char g_rodata[] = "COS_RODATA_MARKER";

long value(void) { return 0xC0FFEE; }
void _start(void) { for (;;) { } }
