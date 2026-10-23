/* ja.c - the Japanese language pack of C-OS Message, built as a shared library (/system/lang/ja.c-osll) and loaded with cos_dlopen().
 * It carries data only: a table of English text (as written in the program) -> Japanese. Format specifiers and \n \t must stay the same.
 * A pack for another language is a copy of this file with its own table. */
#include <stdint.h>
struct cos_lang_entry { const char *src, *dst; };
struct cos_lang_pack { uint32_t magic, version, count, reserved; const char *id; const struct cos_lang_entry *tab; };
static const struct cos_lang_entry tab[] = {
#include "ja_table.inc"
};
const struct cos_lang_pack cos_lang_pack = { 0x474E4C43u /* "CLNG" */, 1, (uint32_t)(sizeof tab / sizeof tab[0]), 0, "ja", tab };
