#ifndef MOCK_TERM_H
#define MOCK_TERM_H
#include "mock.h"
#include "cos_plugin_terminal.h"
static void t_print(const char *l) { strcat(g_out, l); strcat(g_out, "\n"); }
static void t_resolve(const char *in, char *out, size_t cap) { snprintf(out, cap, "%s", in); }
static void t_setcwd(const char *a) { (void)a; }
static long t_rd(const char *p, uint64_t off, void *buf, size_t cap) { const char *d = mock_get(p); if (!d) return -1; size_t l = strlen(d); if (off >= l) return 0; size_t n = l - off; if (n > cap) n = cap; memcpy(buf, d + off, n); return (long)n; }
static int t_cs(const char *p, const char *k, const char *v) { (void)p; strcpy(g_cfg[g_ncfg][0], k); strcpy(g_cfg[g_ncfg][1], v); g_ncfg++; return 0; }
static int t_cg(const char *p, const char *k, char *out, size_t cap) { (void)p; for (int i = g_ncfg - 1; i >= 0; i--) if (!strcmp(g_cfg[i][0], k)) { snprintf(out, cap, "%s", g_cfg[i][1]); return 0; } return -1; }
static term_api_t t_api; static const term_io_t t_io = { t_print, "/", t_resolve, t_setcwd };
static struct { const char *n; int (*fn)(int, char **, const term_io_t *); } t_cmds[32]; static int t_ncmd; static int (*t_rw[4])(const char *, char *, size_t); static int t_nrw;
static int t_add_command(const char *n, const char *help, int (*fn)(int, char **, const term_io_t *)) { (void)help; t_cmds[t_ncmd].n = n; t_cmds[t_ncmd++].fn = fn; return 0; }
static int t_add_rewriter(int (*fn)(const char *, char *, size_t)) { t_rw[t_nrw++] = fn; return 0; }
static int t_call(const char *line) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", line); char *v[16]; int n = 0;
    for (char *p = strtok(buf, " "); p && n < 16; p = strtok(NULL, " ")) v[n++] = p;
    for (int i = 0; i < t_ncmd; i++) if (!strcmp(t_cmds[i].n, v[0])) return t_cmds[i].fn(n, v, &t_io);
    return -999;
}
static void t_setup(void) {
    memset(&t_api, 0, sizeof t_api); t_api.base.abi = COS_PBASE_ABI; t_api.base.snprintf = m_snprintf; t_api.base.read_file = t_rd; t_api.base.cfg_set = t_cs; t_api.base.cfg_get = t_cg;
    t_api.abi = TERMP_ABI; t_api.add_command = t_add_command; t_api.add_rewriter = t_add_rewriter; t_ncmd = 0; t_nrw = 0; g_out[0] = 0; g_ncfg = 0;
}
#endif
