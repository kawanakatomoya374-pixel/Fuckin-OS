/* dl_bad.c - imports a function nothing defines: dlopen must fail and leave nothing behind (/lib/dlbad.c-osll). */
extern int dl_does_not_exist(int);
int dlbad_call(int x) { return dl_does_not_exist(x) + 1; }
