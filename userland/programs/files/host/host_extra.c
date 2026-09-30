/* host_extra.c - the file-manager syscalls on top of POSIX, for running Files on the host
 * (with ../../studio/host/host_shim.c supplying the window and the rest). Testing only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "cos.h"

static uint8_t attr_of(const char *name, const struct stat *st) { (void)st; return name[0] == '.' ? COS_ATTR_HIDDEN : 0; }
int cos_stat_ex(const char *p, cos_stat_ex_t *o) {
    struct stat st; if (stat(p, &st)) return -1;
    memset(o, 0, sizeof *o); o->size = (uint64_t)st.st_size; o->mtime = (uint64_t)st.st_mtime; o->is_dir = S_ISDIR(st.st_mode) ? 1 : 0; o->attr = attr_of(strrchr(p, '/') ? strrchr(p, '/') + 1 : p, &st);
    return 0;
}
int cos_listdir_ex(const char *path, cos_dirent_ex_t *out, int max) {
    DIR *d = opendir(path); if (!d) return -1;
    int n = 0; struct dirent *e;
    while (n < max && (e = readdir(d))) {
        char full[1024]; snprintf(full, sizeof full, "%s/%s", strcmp(path, "/") ? path : "", e->d_name);
        struct stat st; if (stat(full, &st)) continue;
        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
        out[n].is_dir = S_ISDIR(st.st_mode) ? 1 : 0; out[n].size = out[n].is_dir ? 0 : (uint64_t)st.st_size; out[n].mtime = (uint64_t)st.st_mtime;
        out[n].attr = attr_of(e->d_name, &st); ++n;
    }
    closedir(d); return n;
}
int cos_fs_space(uint64_t *t, uint64_t *f) { struct statvfs v; if (statvfs("/", &v)) return -1; *t = (uint64_t)v.f_blocks * v.f_frsize; *f = (uint64_t)v.f_bavail * v.f_frsize; return 0; }
int cos_open_path(const char *p) { FILE *f = fopen("/tmp/files_open.log", "a"); if (f) { fprintf(f, "%s\n", p); fclose(f); } return 0; }
uint32_t cos_ui_prefs(void) { const char *e = getenv("FILES_UI"); return e ? (uint32_t)atoi(e) : 0; }
int64_t cos_clip_set(const void *d, size_t n) { FILE *f = fopen("/tmp/files_clip.bin", "wb"); if (!f) return -1; if (n) fwrite(d, 1, n, f); fclose(f); return (int64_t)n; }
int64_t cos_clip_get(void *o, size_t cap) {
    FILE *f = fopen("/tmp/files_clip.bin", "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (!o) { fclose(f); return n; }
    size_t r = fread(o, 1, cap, f); fclose(f); return (int64_t)r;
}
