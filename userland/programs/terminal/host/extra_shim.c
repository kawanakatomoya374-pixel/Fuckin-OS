/* the few cos_* calls the Terminal uses that studio/host/host_shim.c does not provide (host testing only) */
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include "cos.h"
int cos_listdir_ex(const char *path, cos_dirent_ex_t *out, int max) {
    DIR *d = opendir(path); if (!d) return -1;
    int n = 0; struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[600]; snprintf(p, sizeof p, "%s/%s", path, e->d_name); struct stat st; if (stat(p, &st)) continue;
        memset(&out[n], 0, sizeof out[n]); snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        out[n].is_dir = S_ISDIR(st.st_mode) ? 1 : 0; out[n].size = (uint64_t)st.st_size; ++n;
    }
    closedir(d); return n;
}
