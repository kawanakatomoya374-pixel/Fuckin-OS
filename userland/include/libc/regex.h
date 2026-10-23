#ifndef _COS_REGEX_H
#define _COS_REGEX_H
#include "_cos_libc_cfg.h"
_COS_BEGIN
typedef struct { void *prog; int re_nsub; } regex_t;
typedef int regoff_t;
typedef struct { regoff_t rm_so, rm_eo; } regmatch_t;
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NOSUB 4
#define REG_NEWLINE 8
#define REG_NOTBOL 1
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ESPACE 12
int regcomp(regex_t *re, const char *pattern, int flags);
int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t *m, int flags);
size_t regerror(int code, const regex_t *re, char *buf, size_t size);
void regfree(regex_t *re);
_COS_END
#endif
