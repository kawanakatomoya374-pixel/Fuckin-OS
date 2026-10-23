/* maketest.c - host harness for make.c (testing only, not part of the .c-os build).
 *   maketest DIR [make-args]      plan the build and print the commands
 *   maketest DIR help             print make_help() / what `make help` would do */
#include "cosmake.h"
#include <unistd.h>

bool fs_exists(const char *path) { cos_stat_t st; return cos_stat(path, &st) == 0; }
bool fs_is_dir(const char *path) { cos_stat_t st; return cos_stat(path, &st) == 0 && st.is_dir; }
char *fs_read_all(const char *path, size_t *len) {
    cos_stat_t st; if (cos_stat(path, &st) != 0 || st.is_dir) return NULL;
    char *b = (char *)malloc((size_t)st.size + 1); if (!b) return NULL;
    ssize_t n = st.size ? cos_read_file(path, b, (size_t)st.size) : 0; if (n < 0) { free(b); return NULL; }
    b[n] = 0; if (len) *len = (size_t)n; return b;
}
bool fs_write_all(const char *path, const void *data, size_t len) { return len == 0 ? cos_write_file(path, "", 0) >= 0 || 1 : cos_write_file(path, data, len) >= 0; }
void path_join(char *out, size_t cap, const char *a, const char *b) { size_t n = strlen(a); if (n && a[n - 1] == '/') snprintf(out, cap, "%s%s", a, b); else snprintf(out, cap, "%s/%s", a, b); }
const char *path_base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
bool has_ext(const char *name, const char *ext) { size_t ln = strlen(name), le = strlen(ext); return ln > le && strcasecmp(name + ln - le, ext) == 0; }

static void line(const char *l) { printf("%s\n", l); }
static void note(const char *m) { printf("# %s\n", m); }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *dir = argv[1], *target = NULL, *ov[8]; int nov = 0;
    for (int i = 2; i < argc; ++i) { if (!strcmp(argv[i], "-B")) make_set_always(true); else if (strchr(argv[i], '=')) ov[nov++] = argv[i]; else target = argv[i]; }
    MakePlan p; char err[200];
    if (!make_plan(dir, "Makefile", target, ov, nov, &p, err, sizeof err, note)) { printf("ERROR: %s\n", err); return 1; }
    if (p.builtin_help) { printf("[builtin help]\n"); make_help(dir, "Makefile", line); return 0; }
    printf("goal=%s steps=%d\n", p.goal, p.n);
    for (int i = 0; i < p.n; ++i) printf("%s%s\n", p.steps[i].silent ? "@" : "", p.steps[i].show);
    return 0;
}
