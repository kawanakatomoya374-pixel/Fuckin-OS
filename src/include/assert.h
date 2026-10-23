#ifndef COS_ASSERT_H
#define COS_ASSERT_H

/* No NDEBUG-driven strip-out here on purpose: QuickJS's own build
 * normally compiles with assertions enabled outside of release/perf
 * builds, and a failed invariant inside the JS engine is exactly the
 * kind of thing we'd rather halt loudly on (via cos_assert_fail,
 * which goes through the same panic path as abort() - see stdlib.h)
 * than silently ignore. */
void cos_assert_fail(const char* expr, const char* file, int line) __attribute__((noreturn));
void cos_assert_warn(const char* expr, const char* file, int line);

#undef assert
#ifdef COS_ASSERT_NONFATAL
/* Opt-in per component (the NetSurf core - see NS_NONFATAL_ASSERT in the
 * Makefile): upstream NetSurf release builds compile its asserts out with
 * NDEBUG and simply carry on, and a layout/box-tree inconsistency on one
 * web page must never halt the whole OS. Failures are still logged (once
 * per call site) so they stay visible and fixable. */
#define assert(expr) ((expr) ? (void)0 : cos_assert_warn(#expr, __FILE__, __LINE__))
#else
#define assert(expr) ((expr) ? (void)0 : cos_assert_fail(#expr, __FILE__, __LINE__))
#endif

#endif
