/* Helpers shared by sprint fmt and sprint lint. */
#ifndef TOOLS_INT_H
#define TOOLS_INT_H

/* sprint fmt / sprint lint: source tools built on the lexer and parser.
 *
 * fmt   rewrites whitespace only (indentation, operator and comma spacing,
 *       trailing blanks, blank-line runs, end-of-file newline). The result is
 *       re-lexed and must be token-for-token identical to the input, otherwise
 *       nothing is written (exit 70).
 * lint  walks the AST and reports likely mistakes (SPEC 22.2). */
#define _POSIX_C_SOURCE 200809L
#include "sp.h"
#include "tools.h"

/* functions shared between the parts of this stage */
void print_front_diag(const Diag *d, int json);
int lex_guarded(Front *F, const char *src, size_t len, TokenList *tl);
int load_source(const char *path, char **src, size_t *len);
int check_text(const char *path, const char *src, size_t len, int json);

#endif
