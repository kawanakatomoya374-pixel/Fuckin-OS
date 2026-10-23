/* cos_plugin_terminal.h - what a plugin of the Terminal ("terminal") can use. */
#ifndef COS_PLUGIN_TERMINAL_H
#define COS_PLUGIN_TERMINAL_H
#include "cos_plugin_base.h"

#define TERMP_ABI 1u

typedef struct term_io {
    void       (*print)(const char *line);                                  /* one line of output (may contain several, separated by \\n) */
    const char *cwd;                                                        /* the current directory */
    void       (*resolve)(const char *in, char *out, size_t cap);           /* a path typed by the user -> an absolute path */
    void       (*set_cwd)(const char *abs);                                 /* cd */
} term_io_t;

typedef struct {
    cos_pbase_t base;
    uint32_t abi, size;
    /* A command: fn(argc, argv, io) returns 0 on success; the terminal reports a nonzero result. */
    int (*add_command)(const char *name, const char *help, int (*fn)(int argc, char **argv, const term_io_t *io));
    /* Called with every line before it is run; may rewrite it into `out` (return nonzero) - aliases, abbreviations. */
    int (*add_rewriter)(int (*fn)(const char *line, char *out, size_t cap));
} term_api_t;
#endif
