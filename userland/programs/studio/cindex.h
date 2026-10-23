/* cindex.h - the C completion engine of C-OS Studio.
 *
 * It indexes C declarations (following #include), knows the locals and parameters at the
 * caret, infers the type of `a.b->c[i].` chains and offers members, #include paths,
 * directives, struct tags, keywords and snippets, and gives function signature help.
 *
 * It is a declaration indexer, not a compiler front end: see docs/STUDIO_COMPLETION.md
 * for exactly what it understands and what it does not. It is independent of the Studio
 * GUI so it can be tested on the host (host/test_cindex.c). */
#ifndef CINDEX_H
#define CINDEX_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

enum { CK_LOCAL, CK_PARAM, CK_FIELD, CK_FUNC, CK_VAR, CK_TYPEDEF, CK_TAG, CK_ENUMC, CK_MACRO, CK_KEYWORD, CK_SNIPPET, CK_FILE, CK_DIRECTIVE };

typedef struct {
    char label[72];        /* what is shown */
    char insert[200];      /* what replaces the typed prefix (snippets: with newlines, '\t' = one indent level) */
    char detail[160];      /* type / signature / value */
    uint8_t kind;
    int  score;
    int  fn_np;            /* >= 0: a function or function-like macro with that many parameters (UI adds "()") */
    int  caret_back;       /* snippets: characters from the end of `insert` back to where the caret goes; -1 = end */
} CxItem;

typedef struct { CxItem *item; int n, cap; size_t from; bool member; const char *ctx; } CxResult;

typedef struct {
    char text[512];        /* "int foo(int a, char *b)" */
    int  pstart[16], plen[16], np;
    int  active;           /* index of the parameter being typed (clamped to np-1; variadic stays on the last) */
    char name[64];
    bool variadic;
} CxSig;

typedef struct {
    char *(*read)(const char *path, size_t *len);                        /* malloc'd contents, or NULL */
    int   (*list)(const char *dir, char names[][64], uint8_t *is_dir, int max);   /* entries of a directory */
    char  sdk_dir[128];                                                  /* where <angle> includes are found */
    char  proj_dir[256];                                                 /* project root (searched for "quoted") */
    char  extra[4][256]; int nextra;                                     /* more -I directories */
} CxEnv;

void cx_set_env(const CxEnv *env);
void cx_reset(void);               /* forget every cached header (a file was saved / a project opened) */
/* Completion at `caret` in `text`. `version` changes whenever the text does (it keys a cache of the parse).
 * Returns the number of items; fills out->from (start of the text to replace) and out->member. */
int  cx_complete(const char *text, size_t len, size_t caret, const char *doc_path, uint32_t version, bool manual, CxResult *out);
bool cx_signature(const char *text, size_t len, size_t caret, const char *doc_path, uint32_t version, CxSig *out);
void cx_result_free(CxResult *r);
/* Declarations of the current document, for the outline / tests */
int  cx_debug_type_of(const char *text, size_t len, size_t caret, const char *doc_path, uint32_t version, const char *expr, char *out, size_t cap);
#endif
