#ifndef COS_ALLOCA_H
#define COS_ALLOCA_H

/* alloca() has to actually be the compiler's own stack-allocation
 * builtin (it needs to allocate in *this* function's stack frame,
 * which only the compiler can arrange) - a normal extern function
 * declaration would compile, but fail to link (no such runtime
 * routine exists in this freestanding kernel) and would be semantically
 * wrong even if it somehow did link. Explicitly invoking
 * __builtin_alloca here means this works correctly regardless of
 * whether -fno-builtin is in effect for the including translation
 * unit (the project's real CFLAGS include it; the file-level
 * syntax-check harness used while developing this doesn't, which is
 * exactly the kind of mismatch this macro exists to avoid). */
#define alloca(size) __builtin_alloca(size)

#endif
