/* mock.h - a fake host for the plugin unit tests (run on the build machine by run.sh): an in-memory document, files and terminal output. */
#ifndef MOCK_H
#define MOCK_H
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cos_plugin.h"
#undef COS_PLUGIN
#define COS_PLUGIN(a, n, v, i, f) static const cos_plugin_info_t plugin_info_unused __attribute__((unused)) = { 0, 0, a, n, v, i, f }
extern int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static inline int m_snprintf(char *b, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
/* in-memory files */
struct mfile { char path[64]; char data[8192]; size_t len; };
extern struct mfile g_files[16]; extern int g_nfiles;
void mock_put(const char *path, const char *text);
const char *mock_get(const char *path);
extern char g_cfg[32][2][64]; extern int g_ncfg;
extern char g_out[16384]; extern char g_status[200];
void mock_reset(void);
#endif
