/* Fixture: symbol versioning. Two definitions of the SAME name at
 * different versions - the case that is silently wrong in a loader that
 * ignores DT_VERSYM, because it binds callers to whichever copy the
 * hash chain happens to reach first. */
int ver_fn_v1(void) { return 100; }
int ver_fn_v2(void) { return 200; }
__asm__(".symver ver_fn_v1,ver_fn@COSLIB_1.0");
__asm__(".symver ver_fn_v2,ver_fn@@COSLIB_2.0");
