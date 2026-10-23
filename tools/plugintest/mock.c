#include "mock.h"
int g_fail, g_pass; struct mfile g_files[16]; int g_nfiles; char g_cfg[32][2][64]; int g_ncfg; char g_out[16384]; char g_status[200];
void mock_reset(void) { g_nfiles = 0; g_ncfg = 0; g_out[0] = 0; g_status[0] = 0; }
void mock_put(const char *path, const char *text) {
    for (int i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].path, path)) { g_files[i].len = strlen(text); memcpy(g_files[i].data, text, g_files[i].len + 1); return; }
    strcpy(g_files[g_nfiles].path, path); g_files[g_nfiles].len = strlen(text); memcpy(g_files[g_nfiles].data, text, g_files[g_nfiles].len + 1); g_nfiles++;
}
const char *mock_get(const char *path) { for (int i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].path, path)) return g_files[i].data; return NULL; }
