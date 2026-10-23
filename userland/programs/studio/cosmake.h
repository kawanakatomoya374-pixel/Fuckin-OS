/* cosmake.h - the Makefile engine shared by C-OS Studio's terminal and the C-OS Terminal.
 *
 * make.c parses a Makefile and turns "make [target]" into an ordered list of
 * commands (MakeStep); the caller runs them (there is no shell on C-OS).
 * make_help() reads the same Makefile and lists its documented targets.
 *
 * The caller provides the small file-system helpers declared at the bottom
 * (Studio: project.c, Terminal: tmake.c).
 */
#ifndef COSMAKE_H
#define COSMAKE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include "cos.h"

#define MAKE_MAXARGV 48
typedef struct {
    char  show[512];                 /* the command as written (after variable expansion) */
    char *argv[MAKE_MAXARGV]; int argc;
    char  output[256];               /* the file after `-o`, if any */
    bool  silent, ignore_err, last_of_target;
    char  sigtarget[128]; uint64_t sig;
} MakeStep;
typedef struct { MakeStep *steps; int n, cap; char goal[128]; bool up_to_date;
                 bool builtin_help; } MakePlan;   /* builtin_help: the goal is `help` and the Makefile has no runnable one -> call make_help() */

bool make_plan(const char *dir, const char *makefile, const char *target, const char *const *overrides, int nov,
               MakePlan *plan, char *err, size_t errcap, void (*msg)(const char *));
void make_plan_free(MakePlan *p);
void make_commit_step(const MakeStep *s);
void make_forget(const char *target);
void make_use_dir(const char *dir);  /* make_commit_step / make_forget act on <dir>/.cosmake (default: the dir of the last make_plan) */
void make_set_always(bool on);       /* make -B: rebuild everything, ignoring the recorded signatures */

/* What the Makefile says about its own `help` target:
 *   0  none                      -> make_help() describes the Makefile
 *   1  recipe only uses echo/printf/true -> safe to run as an ordinary target
 *   2  recipe needs grep/awk/sed/... which C-OS has no shell for -> use make_help() instead */
int  make_help_kind(const char *dir, const char *makefile);
/* Prints "usage" + a listing of the documented targets and variables of the Makefile, one line at a
 * time through `out`. Targets are documented with `target: deps ## text`, with a `# text` comment
 * line right above them, or sectioned with `##@ Title`. Returns false when the file cannot be read. */
bool make_help(const char *dir, const char *makefile, void (*out)(const char *line));

/* provided by the host program */
bool  fs_exists(const char *path);
bool  fs_is_dir(const char *path);
char *fs_read_all(const char *path, size_t *len);
bool  fs_write_all(const char *path, const void *data, size_t len);
void  path_join(char *out, size_t cap, const char *a, const char *b);
const char *path_base(const char *p);
bool  has_ext(const char *name, const char *ext);

#endif
