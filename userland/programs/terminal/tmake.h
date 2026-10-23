/* tmake.h - `make` and the compiler for the C-OS Terminal.
 *
 * The Makefile is read by studio/make.c (the same engine C-OS Studio's terminal uses); this file runs
 * the commands it plans. There is no shell, so a recipe line is split into words and dispatched:
 * the compiler (tcc, cc, gcc... -> /bin/tcc.c-os, TinyCC), a few built-ins (echo mkdir rm cp mv touch
 * cat printf ...), another make (`$(MAKE) -C sub`) or a .c-os program. Child output is captured through a
 * log file (COS_STDOUT) and printed as it arrives, so the Terminal never blocks.
 */
#ifndef TMAKE_H
#define TMAKE_H
#include <stdbool.h>

typedef void (*tm_out_fn)(const char *line);        /* one line of output, no trailing newline */
void tm_init(tm_out_fn out);
int  tm_tokenize(char *line, char **argv, int max);   /* words, with "double" and 'single' quotes; argv[n] = NULL */

bool tm_busy(void);                                  /* a build or program is running */
bool tm_tick(void);                                  /* call often; true when something new was printed or the job ended */
void tm_stop(void);                                  /* Ctrl+C */

/* make [-f FILE] [-C DIR] [-n] [-B] [-s] [TARGET ...] [VAR=value ...] | make help */
void tm_make(int argc, char **argv, const char *cwd);
bool tm_is_compiler(const char *word);               /* tcc cc gcc clang cos-cc tcc.c-os */
/* tcc ARGS... or a program ("./a.c-os", "name", "name.c-os") run with its output captured. false: not found */
bool tm_exec(int argc, char **argv, const char *cwd);
/* "Makefile found" hint for a directory; prints nothing when there is none */
void tm_announce(const char *dir);
/* the Makefile name in `dir`, or NULL */
const char *tm_find_makefile(const char *dir);
#endif
