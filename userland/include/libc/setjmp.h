#ifndef _COS_SETJMP_H
#define _COS_SETJMP_H
/* Same layout as cos.h's cos_jmp_buf; declared here directly so <setjmp.h>
 * does not drag the whole cos.h API into a portable program. */
typedef unsigned long jmp_buf[8];
int  cos_setjmp(unsigned long env[8]) __attribute__((returns_twice));
void cos_longjmp(unsigned long env[8], int val) __attribute__((noreturn));
/* A macro, not a wrapper function: setjmp must be called directly from
 * the frame that later longjmps back into it (returns_twice). */
#define setjmp(env)       cos_setjmp(env)
#define longjmp(env, val) cos_longjmp((env), (val))
#endif
