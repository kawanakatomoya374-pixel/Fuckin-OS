/**
 * ctype.c - character classification, "C" locale only (this kernel has
 * no locale support, and QuickJS's own lexer only ever needs ASCII
 * classification anyway - non-ASCII identifier/whitespace handling in
 * JS source text is done in quickjs.c's own Unicode tables, not
 * through these).
 */
#include "ctype.h"

int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isxdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7F; }
int isprint(int c)  { return c >= 0x20 && c < 0x7F; }
int isascii(int c)  { return (unsigned int)c < 0x80; }
int ispunct(int c)  { return isprint(c) && c != ' ' && !isalnum(c); }

int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
