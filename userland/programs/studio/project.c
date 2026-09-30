/* project.c - file system helpers, cosproj.txt, the Explorer tree, the
 * Outline symbol scan and the New Project templates. */
#include "studio.h"

/* ---------------------------------------------------------------- */
/* file system                                                       */
/* ---------------------------------------------------------------- */
bool fs_exists(const char *path) { cos_stat_t st; return cos_stat(path, &st) == 0; }
bool fs_is_dir(const char *path) { cos_stat_t st; return cos_stat(path, &st) == 0 && st.is_dir; }

char *fs_read_all(const char *path, size_t *len) {
    cos_stat_t st;
    if (cos_stat(path, &st) != 0 || st.is_dir) return NULL;
    if (st.size > (64u << 20)) return NULL;
    char *b = (char *)malloc((size_t)st.size + 1);
    if (!b) return NULL;
    ssize_t n = st.size ? cos_read_file(path, b, (size_t)st.size) : 0;
    if (n < 0) { free(b); return NULL; }
    b[n] = 0;
    if (len) *len = (size_t)n;
    return b;
}
bool fs_write_all(const char *path, const void *data, size_t len) {
    /* cos_write_file() rejects len == 0 (the kernel treats an empty transfer as an error), so an
     * empty file - `touch`, New File, truncating a log - is made by opening with CREATE_ALWAYS,
     * which truncates, and closing. */
    if (len == 0) {
        int fd = cos_open(path, COS_O_WRONLY_CREATE);
        if (fd < 0) return false;
        cos_close(fd);
        return true;
    }
    return cos_write_file(path, data, len) >= 0;
}

void path_join(char *out, size_t cap, const char *a, const char *b) {
    size_t n = strlen(a);
    if (n && a[n - 1] == '/') snprintf(out, cap, "%s%s", a, b);
    else snprintf(out, cap, "%s/%s", a, b);
}
const char *path_base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
void path_dir(const char *p, char *out, size_t cap) {
    const char *s = strrchr(p, '/');
    if (!s) { snprintf(out, cap, "."); return; }
    size_t n = (size_t)(s - p);
    if (n == 0) n = 1;
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n); out[n] = 0;
}
bool has_ext(const char *name, const char *ext) {
    size_t ln = strlen(name), le = strlen(ext);
    return ln > le && strcasecmp(name + ln - le, ext) == 0;
}

/* ---------------------------------------------------------------- */
/* Studio's own folder                                                */
/* ---------------------------------------------------------------- */
/* "C-OS Studio" holds the program itself and deliverables/, where everything Studio
 * makes lands: build output, new files, new projects. (STUDIO_HOME overrides it for the
 * host test harness.) */
const char *studio_home(void) {
    const char *o = cos_getenv("STUDIO_HOME");
    return o && o[0] ? o : "/C-OS Studio";
}
const char *studio_deliverables(void) {
    static char p[300];
    path_join(p, sizeof p, studio_home(), "deliverables");
    return p;
}
void studio_ensure_dirs(void) {
    if (!fs_is_dir(studio_home())) cos_mkdir(studio_home());
    if (!fs_is_dir(studio_deliverables())) cos_mkdir(studio_deliverables());
}

/* ---------------------------------------------------------------- */
/* Explorer tree                                                     */
/* ---------------------------------------------------------------- */
static char s_open[64][256]; static int s_nopen;
static char s_srcdirs[8][64]; static int s_nsrc;
static char s_srctok[8][128]; static int s_nsrctok;

static bool dir_is_open(const char *p) { for (int i = 0; i < s_nopen; ++i) if (!strcmp(s_open[i], p)) return true; return false; }
static void dir_set_open(const char *p, bool on) {
    for (int i = 0; i < s_nopen; ++i) if (!strcmp(s_open[i], p)) {
        if (!on) { memmove(s_open[i], s_open[i + 1], (size_t)(s_nopen - i - 1) * 256); --s_nopen; }
        return;
    }
    if (on && s_nopen < 64) snprintf(s_open[s_nopen++], 256, "%s", p);
}

typedef struct { char name[64]; bool dir; } Ent;
static int ent_rank(const Ent *e) {
    if (!e->dir) return 1000;
    for (int i = 0; i < s_nsrc; ++i) if (!strcmp(s_srcdirs[i], e->name)) return i;   /* `sources=` dirs first */
    return 500;
}
static int ent_cmp(const void *a, const void *b) {
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    int rx = ent_rank(x), ry = ent_rank(y);
    if (rx != ry) return rx < ry ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

static void scan_dir(Project *p, const char *dir, int depth) {
    int fd = cos_opendir(dir);
    if (fd < 0) return;
    Ent *ents = (Ent *)malloc(sizeof(Ent) * 256);
    if (!ents) { cos_close(fd); return; }
    int n = 0;
    cos_dirent_t de;
    while (n < 256 && cos_readdir(fd, &de) == 1) {
        if (de.name[0] == '.' || !strcmp(de.name, "..")) continue;
        snprintf(ents[n].name, sizeof ents[n].name, "%s", de.name);
        ents[n].dir = de.is_dir != 0;
        ++n;
    }
    cos_close(fd);
    qsort(ents, (size_t)n, sizeof(Ent), ent_cmp);
    for (int i = 0; i < n && p->nnodes < MAX_NODES; ++i) {
        Node *nd = &p->nodes[p->nnodes++];
        memset(nd, 0, sizeof *nd);
        snprintf(nd->name, sizeof nd->name, "%s", ents[i].name);
        path_join(nd->path, sizeof nd->path, dir, ents[i].name);
        nd->depth = depth; nd->is_dir = ents[i].dir;
        if (nd->is_dir && dir_is_open(nd->path)) {
            nd->open = true;
            scan_dir(p, nd->path, depth + 1);
        }
    }
    free(ents);
}

void project_rescan(Project *p) {
    p->nnodes = 0;
    Node *r = &p->nodes[p->nnodes++];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", p->name);
    snprintf(r->path, sizeof r->path, "%s", p->root);
    r->is_dir = true; r->open = true; r->depth = 0;
    scan_dir(p, p->root, 1);
}

static void parse_proj(Project *p, const char *text) {
    char line[256];
    const char *s = text;
    s_nsrc = 0; s_nsrctok = 0;
    while (*s) {
        size_t k = 0;
        while (*s && *s != '\n' && k < sizeof line - 1) line[k++] = *s++;
        if (*s == '\n') ++s;
        while (k && (line[k - 1] == '\r' || line[k - 1] == ' ')) --k;
        line[k] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *key = line, *val = eq + 1;
        if (!strcmp(key, "name")) snprintf(p->name, sizeof p->name, "%s", val);
        else if (!strcmp(key, "target")) snprintf(p->target, sizeof p->target, "%s", val);
        else if (!strcmp(key, "cflags")) snprintf(p->cflags, sizeof p->cflags, "%s", val);
        else if (!strcmp(key, "defines")) snprintf(p->defines, sizeof p->defines, "%s", val);
        else if (!strcmp(key, "includes")) snprintf(p->includes, sizeof p->includes, "%s", val);
        else if (!strcmp(key, "libs")) snprintf(p->libs, sizeof p->libs, "%s", val);
        else if (!strcmp(key, "type")) snprintf(p->type, sizeof p->type, "%s", val);           /* exe (default) | lib | shared */
        else if (!strcmp(key, "warnings")) p->warn_all = !strcmp(val, "all");
        else if (!strcmp(key, "werror")) p->werror = atoi(val) != 0;
        else if (!strcmp(key, "sources")) {
            /* comma separated files or directories; directories float to the top of the tree */
            char tmp[256]; snprintf(tmp, sizeof tmp, "%s", val);
            for (char *tok = strtok(tmp, ","); tok && s_nsrc < 8; tok = strtok(NULL, ",")) {
                while (*tok == ' ') ++tok;
                if (s_nsrctok < 8) snprintf(s_srctok[s_nsrctok++], 128, "%s", tok);
                char *sl = strchr(tok, '/'); if (sl) *sl = 0;
                snprintf(s_srcdirs[s_nsrc++], 64, "%s", tok);
            }
        }
    }
}

bool project_open(Project *p, const char *root) {
    if (!fs_is_dir(root)) return false;
    memset(p, 0, sizeof *p);
    snprintf(p->root, sizeof p->root, "%s", root);
    size_t n = strlen(p->root);
    while (n > 1 && p->root[n - 1] == '/') p->root[--n] = 0;
    snprintf(p->name, sizeof p->name, "%s", path_base(p->root));
    snprintf(p->target, sizeof p->target, "%s", p->name);
    snprintf(p->cflags, sizeof p->cflags, "-O0");
    snprintf(p->type, sizeof p->type, "exe");
    char pf[300]; path_join(pf, sizeof pf, p->root, "cosproj.txt");
    size_t len = 0; char *txt = fs_read_all(pf, &len);
    s_nsrc = 0; s_nsrctok = 0;
    if (txt) { parse_proj(p, txt); free(txt); }
    s_nopen = 0;
    dir_set_open(p->root, true);
    for (int i = 0; i < s_nsrc; ++i) { char d[300]; path_join(d, sizeof d, p->root, s_srcdirs[i]); dir_set_open(d, true); }
    if (!s_nsrc) { char d[300]; path_join(d, sizeof d, p->root, "src"); if (fs_is_dir(d)) dir_set_open(d, true); }
    p->loaded = true;
    project_rescan(p);
    return true;
}

void project_toggle(Project *p, int idx) {
    if (idx < 1 || idx >= p->nnodes || !p->nodes[idx].is_dir) return;
    dir_set_open(p->nodes[idx].path, !p->nodes[idx].open);
    project_rescan(p);
}

/* Walk up from `file` looking for cosproj.txt. */
bool project_find_root(const char *file, char *root, size_t cap) {
    char d[256]; path_dir(file, d, sizeof d);
    for (int i = 0; i < 6; ++i) {
        char pf[300]; path_join(pf, sizeof pf, d, "cosproj.txt");
        if (fs_exists(pf)) { snprintf(root, cap, "%s", d); return true; }
        if (!strcmp(d, "/") || !strcmp(d, ".")) break;
        char up[256]; path_dir(d, up, sizeof up);
        if (!strcmp(up, d)) break;
        snprintf(d, sizeof d, "%s", up);
    }
    return false;
}

/* ---------------------------------------------------------------- */
/* templates                                                         */
/* ---------------------------------------------------------------- */
static const char *T_CONSOLE =
    "#include <stdio.h>\n"
    "#include \"cos.h\"\n"
    "\n"
    "int main(void) {\n"
    "    printf(\"Hello from C-OS!\\n\");\n"
    "    return 0;\n"
    "}\n";
static const char *T_GUI =
    "#include <stdio.h>\n"
    "#include \"cos.h\"\n"
    "#include \"cos_ui.h\"\n"
    "\n"
    "// Hello, C-OS Studio\n"
    "int main(void) {\n"
    "    cos_win_info_t wi;\n"
    "    int64_t h = cos_win2_create(\"Hello\", 320, 200, &wi);\n"
    "    if (h <= 0) return 1;\n"
    "    cui_canvas c;\n"
    "    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);\n"
    "    cui_fill(&c, 0, 0, c.w, c.h, 0x1E2129);\n"
    "    cui_text(&c, cui_font_bold(20), 16, 16, \"Hello, C-OS!\", 0xFFFFFF);\n"
    "    cos_win2_present(h);\n"
    "    cos_win_event_t ev;\n"
    "    while (cos_win2_wait(h, &ev, 100) >= 0) {\n"
    "        if (ev.type == COS_EV_CLOSE) break;\n"
    "    }\n"
    "    cos_win2_close(h);\n"
    "    return 0;\n"
    "}\n";
static const char *T_AUDIO =
    "#include <stdio.h>\n"
    "#include <math.h>\n"
    "#include <stdint.h>\n"
    "#include \"cos.h\"\n"
    "\n"
    "// Plays a 660 Hz tone for two seconds.\n"
    "int main(void) {\n"
    "    const uint32_t rate = 44100;\n"
    "    if (cos_audio_open(rate, 2) != 0) { printf(\"no audio device\\n\"); return 1; }\n"
    "    static int16_t buf[2048];\n"
    "    double phase = 0.0, step = 2.0 * M_PI * 660.0 / rate;\n"
    "    uint64_t total = rate * 2, done = 0;\n"
    "    while (done < total) {\n"
    "        cos_audio_status_t st;\n"
    "        cos_audio_status(&st);\n"
    "        if (st.free_samples < 2048) { cos_sleep_ms(10); continue; }\n"
    "        for (int i = 0; i < 1024; ++i) {\n"
    "            int16_t s = (int16_t)(sin(phase) * 9000.0);\n"
    "            phase += step; if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;\n"
    "            buf[i * 2] = s; buf[i * 2 + 1] = s;\n"
    "        }\n"
    "        done += (uint64_t)cos_audio_write(buf, 2048) / 2;\n"
    "    }\n"
    "    cos_audio_drain();\n"
    "    cos_audio_close();\n"
    "    return 0;\n"
    "}\n";
static const char *T_EMPTY = "int main(void) {\n    return 0;\n}\n";

bool project_create(const char *parent, const char *name, int templ, char *out_main, size_t cap) {
    if (!name || !name[0] || strchr(name, '/')) return false;
    char root[256], src[280], f[300];
    path_join(root, sizeof root, parent, name);
    if (fs_exists(root)) return false;
    if (cos_mkdir(root) != 0) return false;
    path_join(src, sizeof src, root, "src");
    if (cos_mkdir(src) != 0) return false;
    path_join(f, sizeof f, root, "assets");
    cos_mkdir(f);
    const char *code = templ == 0 ? T_CONSOLE : templ == 1 ? T_GUI : templ == 2 ? T_AUDIO : T_EMPTY;
    path_join(f, sizeof f, src, "main.c");
    if (!fs_write_all(f, code, strlen(code))) return false;
    if (out_main) snprintf(out_main, cap, "%s", f);
    char meta[512];
    int n = snprintf(meta, sizeof meta,
        "name=%s\ntarget=%s\ncflags=-O0\nsources=src\n"
        "# type=exe            # exe (default) | lib | shared -> produces a .c-osll\n"
        "# defines=FOO,BAR=1   # comma separated -D flags\n"
        "# includes=vendor     # comma separated -I dirs, relative to this file unless absolute\n"
        "# libs=vendor/lib.a   # comma separated .a/.o files linked in after the sources\n"
        "# warnings=all        # -Wall\n"
        "# werror=1            # -Werror\n",
        name, name);
    path_join(f, sizeof f, root, "cosproj.txt");
    fs_write_all(f, meta, (size_t)n);
    char rd[256];
    n = snprintf(rd, sizeof rd, "%s\n\nCreated with C-OS Studio. Press F5 to build and run.\n", name);
    path_join(f, sizeof f, root, "README.txt");
    fs_write_all(f, rd, (size_t)n);
    return true;
}

/* ---------------------------------------------------------------- */
/* Outline: functions, structs, enums, unions at file scope           */
/* ---------------------------------------------------------------- */
static bool is_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'); }

int outline_scan(Doc *d, Sym *out, int max) {
    int cnt = 0, depth = 0;
    bool ic = false;
    const char *t = buf_text(&d->b);
    size_t nl = buf_lines(&d->b);
    static const char *const NOT_FN[] = { "if", "while", "for", "switch", "return", "sizeof", "do", "else", "defined", NULL };
    for (size_t l = 0; l < nl && cnt < max; ++l) {
        size_t a = buf_line_start(&d->b, l), e = buf_line_end(&d->b, l);
        const char *s = t + a; int n = (int)(e - a);
        Tok toks[128];
        bool ic0 = ic;
        int nt = hl_line(s, n, &ic, toks, 128);
        (void)ic0;
        if (depth == 0 && n > 0 && s[0] != '#' && s[0] != ' ' && s[0] != '\t' && s[0] != '}' && s[0] != '/') {
            /* struct/union/enum NAME { */
            const char *kinds[3] = { "struct", "union", "enum" };
            for (int k = 0; k < 3; ++k) {
                const char *kw = strstr(s, kinds[k]);
                size_t kl = strlen(kinds[k]);
                if (kw && kw < s + n && (kw == s || !is_idc(kw[-1])) && !is_idc(kw[kl])) {
                    const char *q = kw + kl;
                    while (q < s + n && (*q == ' ' || *q == '\t')) ++q;
                    const char *ns = q;
                    while (q < s + n && is_idc(*q)) ++q;
                    if (q > ns) {
                        const char *r = q;
                        while (r < s + n && (*r == ' ' || *r == '\t')) ++r;
                        bool brace_here = r < s + n && *r == '{';
                        bool brace_next = false;
                        if (r >= s + n && l + 1 < nl) { size_t na = buf_line_start(&d->b, l + 1); brace_next = t[na] == '{'; }
                        if (brace_here || brace_next) {
                            snprintf(out[cnt].kind, sizeof out[cnt].kind, "%s", kinds[k]);
                            size_t ln = (size_t)(q - ns); if (ln > 63) ln = 63;
                            memcpy(out[cnt].name, ns, ln); out[cnt].name[ln] = 0;
                            out[cnt].line = (int)l + 1;
                            ++cnt;
                        }
                    }
                    break;
                }
            }
            /* function definition: an identifier directly before '(' and the line does not end in ';' */
            if (cnt < max && (cnt == 0 || out[cnt - 1].line != (int)l + 1)) {
                for (int i = 0; i < nt; ++i) {
                    if (toks[i].kind != TK_FN) continue;
                    char name[64]; int ln = toks[i].len < 63 ? toks[i].len : 63;
                    memcpy(name, s + toks[i].start, (size_t)ln); name[ln] = 0;
                    bool skip = false;
                    for (int k = 0; NOT_FN[k]; ++k) if (!strcmp(NOT_FN[k], name)) skip = true;
                    if (skip) break;
                    /* find where the parameter list closes, on this line or the next few */
                    int par = 0; size_t ll = l; bool closed = false, brace = false, semi = false;
                    for (size_t look = 0; look < 4 && ll < nl && !closed; ++look, ++ll) {
                        size_t la = buf_line_start(&d->b, ll), le = buf_line_end(&d->b, ll);
                        size_t i0 = look == 0 ? a + (size_t)toks[i].start + (size_t)toks[i].len : la;
                        for (size_t q = i0; q < le; ++q) {
                            char c = t[q];
                            if (c == '(') ++par;
                            else if (c == ')') { if (--par <= 0) { closed = true; for (size_t z = q + 1; z < le; ++z) { if (t[z] == '{') brace = true; if (t[z] == ';') semi = true; if (t[z] == '{' || t[z] == ';') break; } if (!brace && !semi && ll + 1 < nl) { size_t na = buf_line_start(&d->b, ll + 1); if (t[na] == '{') brace = true; } break; } }
                        }
                    }
                    if (closed && brace && !semi) {
                        snprintf(out[cnt].kind, sizeof out[cnt].kind, "fn");
                        snprintf(out[cnt].name, sizeof out[cnt].name, "%s", name);
                        out[cnt].line = (int)l + 1;
                        ++cnt;
                    }
                    break;
                }
            }
        }
        /* track brace depth from tokens' text, skipping strings/comments/preprocessor */
        int ti = 0;
        for (int i = 0; i < n; ++i) {
            while (ti < nt && toks[ti].start + toks[ti].len <= i) ++ti;
            if (ti < nt && i >= toks[ti].start && (toks[ti].kind == TK_STR || toks[ti].kind == TK_CMT || toks[ti].kind == TK_PP)) { i = toks[ti].start + toks[ti].len - 1; continue; }
            if (s[i] == '{') ++depth; else if (s[i] == '}' && depth > 0) --depth;
        }
    }
    return cnt;
}

/* ---------------------------------------------------------------- */
/* the files a build compiles                                          */
/* ---------------------------------------------------------------- */
static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int add_dir_c_files(const char *dir, char out[][256], int n, int max) {
    int fd = cos_opendir(dir);
    if (fd < 0) return n;
    int start = n;
    cos_dirent_t de;
    while (n < max && cos_readdir(fd, &de) == 1)
        if (!de.is_dir && has_ext(de.name, ".c")) path_join(out[n++], 256, dir, de.name);
    cos_close(fd);
    qsort(out + start, (size_t)(n - start), 256, cmp_str);
    return n;
}

/* `sources=` entries (files or directories, comma separated) - or every .c in
 * src/ (else in the project root) when the project does not say. */
int project_sources(Project *p, char out[][256], int max) {
    int n = 0;
    for (int i = 0; i < s_nsrctok && n < max; ++i) {
        char path[300]; path_join(path, sizeof path, p->root, s_srctok[i]);
        if (fs_is_dir(path)) n = add_dir_c_files(path, out, n, max);
        else if (fs_exists(path)) snprintf(out[n++], 256, "%s", path);
    }
    if (n == 0) {
        char d[300]; path_join(d, sizeof d, p->root, "src");
        n = add_dir_c_files(fs_is_dir(d) ? d : p->root, out, 0, max);
    }
    return n;
}
