/* dl_p.c - a preload library (/lib/dlp.c-osll): defines dla_add, so a program that was started with COS_PRELOAD=/lib/dlp.c-osll gets this one instead of libdla's. */
int dla_add(int a, int b) { return a + b + 1000; }
