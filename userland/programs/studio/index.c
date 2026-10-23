/* index.c - a lightweight C declaration indexer for completion and signature help.
 *
 * This is not a compiler front end: it is a pragmatic scanner tuned to the style the
 * SDK headers and ordinary application code are actually written in (one declaration
 * per statement, braces on predictable lines). It:
 *   - strips comments and string/char literals before scanning, so punctuation inside
 *     them can never be mistaken for code,
 *   - follows #include "..." (relative to the including file, then the project) and
 *     #include <...> (the SDK) up to a bounded depth and file count,
 *   - records functions, globals, typedefs, struct/union/enum tags and their members,
 *     macros and enum constants,
 *   - resolves a typedef chain to the struct/union tag it ultimately names, including
 *     the extremely common `typedef struct { ... } Name;` anonymous form, which is
 *     what makes member completion after `.`/`->` possible.
 *
 * Limits are generous but real (MAX_DECLS, MAX_FILES, MAX_BYTES): a header that blows
 * through them is skipped rather than crashing the editor.
 */
#include "studio.h"

#define MAX_DECLS   4000
#define MAX_FILES   96
#define MAX_BYTES   (900 * 1024)

typedef struct {
    char name[56];
    uint8_t kind;          /* DK_* */
    char detail[104];      /* function params / var type / typedef target / enum tag, etc. */
    char parent[56];       /* for DK_MEMBER: owning struct/union tag. for DK_ENUMCONST: owning enum tag (may be empty) */
    char file[160];
} CDecl;

static CDecl s_decl[MAX_DECLS];
static int   s_ndecl;
static char  s_visited[MAX_FILES][256];
static int   s_nvisited;
static int   s_anon_ctr;

static bool idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'); }
static bool id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int  lcase(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static bool ieq_prefix(const char *w, const char *p, size_t pl) {
    for (size_t i = 0; i < pl; ++i) { if (!w[i]) return false; if (lcase((unsigned char)w[i]) != lcase((unsigned char)p[i])) return false; }
    return true;
}

static void decl_add(const char *name, size_t nlen, uint8_t kind, const char *detail, const char *parent, const char *file) {
    if (s_ndecl >= MAX_DECLS || nlen == 0 || nlen > 55) return;
    /* a later declaration of the same name/kind/parent (e.g. re-included header) just replaces the first */
    for (int i = 0; i < s_ndecl; ++i) {
        if (s_decl[i].kind == kind && (int)nlen == (int)strlen(s_decl[i].name) && !memcmp(s_decl[i].name, name, nlen) &&
            !strcmp(s_decl[i].parent, parent ? parent : "")) return;
    }
    CDecl *d = &s_decl[s_ndecl++];
    memcpy(d->name, name, nlen); d->name[nlen] = 0;
    d->kind = kind;
    snprintf(d->detail, sizeof d->detail, "%s", detail ? detail : "");
    snprintf(d->parent, sizeof d->parent, "%s", parent ? parent : "");
    snprintf(d->file, sizeof d->file, "%s", file ? file : "");
}

/* ------------------------------------------------------------------ */
/* comment/string stripping: produces a same-length buffer with        */
/* comments and literal contents blanked to spaces (newlines kept)     */
/* ------------------------------------------------------------------ */
static char *clean_copy(const char *src, size_t n) {
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, src, n); out[n] = 0;
    bool in_lc = false, in_bc = false, in_str = false, in_chr = false;
    for (size_t i = 0; i < n; ++i) {
        char c = src[i], c2 = i + 1 < n ? src[i + 1] : 0;
        if (in_lc) { if (c == '\n') in_lc = false; else out[i] = ' '; continue; }
        if (in_bc) { if (c == '*' && c2 == '/') { out[i] = out[i + 1] = ' '; in_bc = false; ++i; } else { if (c != '\n') out[i] = ' '; } continue; }
        if (in_str) { if (c == '\\' && i + 1 < n) { out[i] = out[i + 1] = ' '; ++i; continue; } if (c == '"') in_str = false; else if (c != '\n') out[i] = ' '; continue; }
        if (in_chr) { if (c == '\\' && i + 1 < n) { out[i] = out[i + 1] = ' '; ++i; continue; } if (c == '\'') in_chr = false; else if (c != '\n') out[i] = ' '; continue; }
        if (c == '/' && c2 == '/') { in_lc = true; out[i] = ' '; continue; }
        if (c == '/' && c2 == '*') { in_bc = true; out[i] = ' '; continue; }
        if (c == '"') { in_str = true; continue; }
        if (c == '\'') { in_chr = true; continue; }
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* small text helpers over the cleaned buffer                          */
/* ------------------------------------------------------------------ */
static size_t skip_ws(const char *s, size_t n, size_t i) { while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i; return i; }
static size_t skip_ws_nonl(const char *s, size_t n, size_t i) { while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i; return i; }

/* returns the index just after a matching close bracket, given i sits ON the open one */
static size_t match_bracket(const char *s, size_t n, size_t i, char open, char close) {
    int depth = 0;
    for (; i < n; ++i) {
        if (s[i] == open) ++depth;
        else if (s[i] == close) { --depth; if (depth == 0) return i + 1; }
    }
    return n;
}

static size_t read_ident(const char *s, size_t n, size_t i, char *out, size_t outcap) {
    size_t j = i, k = 0;
    while (j < n && idc((unsigned char)s[j])) { if (k + 1 < outcap) out[k++] = s[j]; ++j; }
    out[k] = 0;
    return j;
}

/* trims leading/trailing blanks and collapses internal whitespace runs to one space */
static void tidy(char *s) {
    char *w = s; bool sp = false, any = false;
    for (char *r = s; *r; ++r) {
        if (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n') { if (any) sp = true; continue; }
        if (sp) { *w++ = ' '; sp = false; }
        *w++ = *r; any = true;
    }
    *w = 0;
}

/* the last identifier in [a,b) that isn't itself followed (within the span) by '(' at
 * top level - i.e. the declared NAME in "TYPE ... NAME" or "TYPE ... NAME[3]" */
static bool last_decl_name(const char *s, size_t a, size_t b, char *out, size_t outcap) {
    /* strip one trailing [...] / [...][...] (array dims) */
    while (b > a) {
        size_t e = skip_ws(s, b, a); (void)e;
        size_t k = b; while (k > a && (s[k - 1] == ' ' || s[k - 1] == '\t')) --k;
        if (k > a && s[k - 1] == ']') {
            int depth = 0; size_t j = k;
            while (j > a) { --j; if (s[j] == ']') ++depth; else if (s[j] == '[') { --depth; if (depth == 0) break; } }
            if (j >= a && s[j] == '[') { b = j; continue; }
        }
        break;
    }
    size_t k = b;
    while (k > a && (s[k - 1] == ' ' || s[k - 1] == '\t')) --k;
    if (k == a) return false;
    size_t e = k;
    while (k > a && idc((unsigned char)s[k - 1])) --k;
    if (k == e) return false;
    size_t ln = e - k; if (ln >= outcap) ln = outcap - 1;
    memcpy(out, s + k, ln); out[ln] = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* struct/union member parsing: the region is the inside of { }        */
/* ------------------------------------------------------------------ */
static void parse_members(const char *s, size_t a, size_t b, const char *tag, const char *file) {
    size_t i = a;
    while (i < b) {
        i = skip_ws(s, b, i);
        if (i >= b) break;
        size_t stmt_start = i;
        int depth = 0;
        size_t j = i;
        while (j < b) {
            char c = s[j];
            if (c == '{' || c == '(' || c == '[') ++depth;
            else if (c == '}' || c == ')' || c == ']') --depth;
            else if (c == ';' && depth <= 0) break;
            ++j;
        }
        size_t stmt_end = j;                      /* [stmt_start, stmt_end) excludes ';' */
        /* nested anonymous struct/union: "struct { ... } name;" - index the nested members under a synthetic tag */
        char kw[8]; size_t p = stmt_start;
        p = skip_ws_nonl(s, stmt_end, p);
        bool nested = false;
        if (!memcmp(s + p, "struct", p + 6 <= stmt_end ? 6 : 0) || !memcmp(s + p, "union", p + 5 <= stmt_end ? 5 : 0)) {
            size_t kwn = !memcmp(s + p, "struct", 6) ? 6 : 5;
            size_t q = skip_ws(s, stmt_end, p + kwn);
            char tagname[56]= ""; q = read_ident(s, stmt_end, q, tagname, sizeof tagname);
            q = skip_ws(s, stmt_end, q);
            if (q < stmt_end && s[q] == '{') {
                size_t close = match_bracket(s, stmt_end, q, '{', '}');
                char synth[56]; snprintf(synth, sizeof synth, "%s$a%d", tag, ++s_anon_ctr);
                parse_members(s, q + 1, close - 1, synth, file);
                /* the field name(s) after '}' use this synthetic tag as their type */
                char names[stmt_end - close + 1]; size_t nn = 0;
                for (size_t r = close; r < stmt_end && nn < sizeof names - 1; ++r) names[nn++] = s[r];
                names[nn] = 0;
                char *save = NULL;
                for (char *t = strtok_r(names, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                    char nm[56]; size_t tn = strlen(t); size_t a2 = 0, b2 = tn;
                    if (last_decl_name(t, a2, b2, nm, sizeof nm)) decl_add(nm, strlen(nm), DK_MEMBER, synth, tag, file);
                }
                nested = true;
                (void)kw;
            }
        }
        if (!nested && stmt_end > stmt_start) {
            /* possibly several comma-separated declarators sharing a base type: "int a, b, *c;" -
             * split on top-level commas first, so every name is indexed, not just the last one */
            char stmt[512]; size_t sl = stmt_end - stmt_start; if (sl >= sizeof stmt) sl = sizeof stmt - 1;
            memcpy(stmt, s + stmt_start, sl); stmt[sl] = 0;
            size_t parts_a[8], parts_b[8]; int nparts = 0;
            { int depth = 0; size_t ps = 0;
              for (size_t k = 0; k <= sl && nparts < 8; ++k) {
                  char c = k < sl ? stmt[k] : ',';
                  if (c=='('||c=='['||c=='{') ++depth; else if (c==')'||c==']'||c=='}') --depth;
                  else if (c==',' && depth<=0) { parts_a[nparts]=ps; parts_b[nparts]=k; ++nparts; ps=k+1; }
              }
            }
            char typebuf[104] = ""; bool have_type = false;
            for (int pi = 0; pi < nparts; ++pi) {
                size_t a2 = parts_a[pi], b2 = parts_b[pi];
                while (a2 < b2 && (stmt[a2]==' '||stmt[a2]=='\t')) ++a2;
                char nm[56];
                if (pi == 0) {
                    /* first declarator carries the base type: everything before its name */
                    if (!last_decl_name(stmt, a2, b2, nm, sizeof nm)) continue;
                    size_t nmpos = b2; while (nmpos > a2 && (stmt[nmpos-1]==' '||stmt[nmpos-1]=='\t')) --nmpos;
                    while (nmpos > a2 && stmt[nmpos-1]==']') { int bd=0; size_t q=nmpos; while(q>a2){--q; if(stmt[q]==']')++bd; else if(stmt[q]=='['){--bd; if(!bd)break;}} nmpos=q; while (nmpos>a2 && (stmt[nmpos-1]==' '||stmt[nmpos-1]=='\t')) --nmpos; }
                    size_t tb = nmpos - strlen(nm);
                    bool ptr = tb > a2 && stmt[tb-1] == '*';
                    size_t te = tb; while (te > a2 && (stmt[te-1]=='*'||stmt[te-1]==' '||stmt[te-1]=='\t')) --te;
                    size_t cl = te - a2; if (cl >= sizeof typebuf) cl = sizeof typebuf - 1;
                    memcpy(typebuf, stmt + a2, cl); typebuf[cl] = 0; tidy(typebuf);
                    have_type = true;
                    char full[104]; snprintf(full, sizeof full, "%s%s", typebuf, ptr ? " *" : "");
                    decl_add(nm, strlen(nm), DK_MEMBER, full, tag, file);
                } else if (have_type) {
                    /* later declarators: just [*...] NAME [ [dims] ] - reuses typebuf as the base */
                    size_t p2 = a2; int stars = 0;
                    while (p2 < b2 && (stmt[p2]=='*'||stmt[p2]==' '||stmt[p2]=='\t')) { if (stmt[p2]=='*') ++stars; ++p2; }
                    size_t e2 = b2; while (e2 > p2 && (stmt[e2-1]==' '||stmt[e2-1]=='\t')) --e2;
                    while (e2 > p2 && stmt[e2-1]==']') { int bd=0; size_t q=e2; while(q>p2){--q; if(stmt[q]==']')++bd; else if(stmt[q]=='['){--bd; if(!bd)break;}} e2=q; while (e2>p2 && (stmt[e2-1]==' '||stmt[e2-1]=='\t')) --e2; }
                    size_t nl2 = e2 - p2;
                    if (nl2 > 0 && nl2 < sizeof nm && id0((unsigned char)stmt[p2])) {
                        memcpy(nm, stmt + p2, nl2); nm[nl2] = 0;
                        char full[104]; snprintf(full, sizeof full, "%s%s", typebuf, stars ? " *" : "");
                        decl_add(nm, strlen(nm), DK_MEMBER, full, tag, file);
                    }
                }
            }
        }
        i = stmt_end < b ? stmt_end + 1 : stmt_end;
    }
}

/* enum constants: comma-separated NAME [= expr] inside { } */
static void parse_enumerators(const char *s, size_t a, size_t b, const char *tag, const char *file) {
    size_t i = a;
    while (i < b) {
        i = skip_ws(s, b, i);
        if (i >= b) break;
        char nm[56]; size_t j = read_ident(s, b, i, nm, sizeof nm);
        if (nm[0]) decl_add(nm, strlen(nm), DK_ENUMCONST, tag, tag, file);
        int depth = 0;
        while (j < b) { char c = s[j]; if (c=='('||c=='['||c=='{') ++depth; else if (c==')'||c==']'||c=='}') --depth; else if (c==',' && depth<=0) { ++j; break; } ++j; }
        i = j;
    }
}

/* ------------------------------------------------------------------ */
/* one file: scan for #include, #define, typedef, struct/union/enum,   */
/* function and global declarations at brace depth 0                   */
/* ------------------------------------------------------------------ */
static void scan_file(const char *path, const char *dir_of_includer, int depth, const Project *proj);

static void resolve_include(const char *spec, bool angled, const char *dir_of_includer, const Project *proj, int depth) {
    char cand[300];
    if (!angled) {
        path_join(cand, sizeof cand, dir_of_includer, spec);
        if (fs_exists(cand)) { scan_file(cand, dir_of_includer, depth + 1, proj); return; }
        if (proj && proj->loaded) { path_join(cand, sizeof cand, proj->root, spec); if (fs_exists(cand)) { char d2[256]; path_dir(cand, d2, sizeof d2); scan_file(cand, d2, depth + 1, proj); return; } }
    }
    static const char *const SDKDIRS[] = { "/system/sdk/include", "/system/sdk/include/sys", NULL };
    for (int i = 0; SDKDIRS[i]; ++i) { path_join(cand, sizeof cand, SDKDIRS[i], spec); if (fs_exists(cand)) { scan_file(cand, SDKDIRS[i], depth + 1, proj); return; } }
}

static void scan_file(const char *path, const char *dir_of_includer, int depth, const Project *proj) {
    if (depth > 8 || s_nvisited >= MAX_FILES) return;
    for (int i = 0; i < s_nvisited; ++i) if (!strcmp(s_visited[i], path)) return;
    snprintf(s_visited[s_nvisited++], sizeof s_visited[0], "%s", path);

    size_t flen = 0;
    char *raw = fs_read_all(path, &flen);
    if (!raw) return;
    if (flen > MAX_BYTES) flen = MAX_BYTES;
    /* #include specs must come from the RAW text: clean_copy() blanks quoted-string contents to
     * guard against comment/string punctuation confusing the scanner, but that would blank
     * `#include "cos.h"`'s own path too - so includes are resolved here, before cleaning,
     * and the cleaned-text pass below never needs to look at #include lines again. */
    for (size_t i = 0; i < flen; ) {
        size_t eol = i; while (eol < flen && raw[eol] != '\n') ++eol;
        size_t p = i; while (p < eol && (raw[p]==' '||raw[p]=='\t')) ++p;
        if (p < eol && raw[p] == '#') {
            size_t q = p + 1; while (q < eol && (raw[q]==' '||raw[q]=='\t')) ++q;
            if (!memcmp(raw + q, "include", q + 7 <= eol ? 7 : 0)) {
                q = skip_ws_nonl(raw, eol, q + 7);
                if (q < eol && (raw[q] == '"' || raw[q] == '<')) {
                    bool angled = raw[q] == '<'; char close = angled ? '>' : '"';
                    size_t k = q + 1, b0 = k;
                    while (k < eol && raw[k] != close) ++k;
                    char spec[160]; size_t ln = k - b0; if (ln >= sizeof spec) ln = sizeof spec - 1;
                    memcpy(spec, raw + b0, ln); spec[ln] = 0;
                    resolve_include(spec, angled, dir_of_includer, proj, depth);
                }
            }
        }
        i = eol + 1;
    }

    char *s = clean_copy(raw, flen);
    free(raw);
    if (!s) return;
    size_t n = flen;
    const char *file_disp = path_base(path);

    for (size_t i = 0; i < n; ) {
        i = skip_ws(s, n, i);
        if (i >= n) break;
        size_t eol = i; while (eol < n && s[eol] != '\n') ++eol;

        if (s[i] == '#') {
            size_t j = skip_ws_nonl(s, eol, i + 1);
            if (!memcmp(s + j, "include", j + 7 <= eol ? 7 : 0)) {
                j = skip_ws_nonl(s, eol, j + 7);
                if (j < eol && (s[j] == '"' || s[j] == '<')) {
                    bool angled = s[j] == '<'; char close = angled ? '>' : '"';
                    size_t k = j + 1, b0 = k;
                    while (k < eol && s[k] != close) ++k;
                    char spec[160]; size_t ln = k - b0; if (ln >= sizeof spec) ln = sizeof spec - 1;
                    memcpy(spec, s + b0, ln); spec[ln] = 0;
                    resolve_include(spec, angled, dir_of_includer, proj, depth);
                }
            } else if (!memcmp(s + j, "define", j + 6 <= eol ? 6 : 0)) {
                j = skip_ws_nonl(s, eol, j + 6);
                char nm[56]; size_t k = read_ident(s, eol, j, nm, sizeof nm);
                if (nm[0]) {
                    if (k < eol && s[k] == '(') { /* function-like macro: keep the (...) in the name display via detail */
                        size_t close = match_bracket(s, eol, k, '(', ')');
                        char params[80]; size_t pl = close - k; if (pl >= sizeof params) pl = sizeof params - 1;
                        memcpy(params, s + k, pl); params[pl] = 0; tidy(params);
                        decl_add(nm, strlen(nm), DK_MACRO, params, "", file_disp);
                    } else {
                        char val[100]; size_t vs = skip_ws_nonl(s, eol, k), ve = eol;
                        while (ve > vs && (s[ve-1]==' '||s[ve-1]=='\t')) --ve;
                        size_t vl = ve - vs; if (vl >= sizeof val) vl = sizeof val - 1;
                        memcpy(val, s + vs, vl); val[vl] = 0; tidy(val);
                        decl_add(nm, strlen(nm), DK_MACRO, val, "", file_disp);
                    }
                }
            }
            i = eol + 1; continue;
        }

        /* typedef ... ; (handles struct/union/enum bodies and function-pointer forms) */
        if (!memcmp(s + i, "typedef", i + 7 <= n && !idc((unsigned char)s[i+7]) ? 7 : 0)) {
            size_t p = skip_ws(s, n, i + 7);
            /* does the underlying type carry a struct/union/enum body? */
            char tag_kind = 0; char tagname[56] = ""; char structtag[56] = "";
            if (!memcmp(s + p, "struct", 6) && !idc((unsigned char)s[p+6])) tag_kind = 'S';
            else if (!memcmp(s + p, "union", 5) && !idc((unsigned char)s[p+5])) tag_kind = 'U';
            else if (!memcmp(s + p, "enum", 4) && !idc((unsigned char)s[p+4])) tag_kind = 'E';
            size_t body_end = p;
            if (tag_kind) {
                size_t q = skip_ws(s, n, p + (tag_kind == 'U' ? 5 : tag_kind == 'E' ? 4 : 6));
                q = read_ident(s, n, q, tagname, sizeof tagname);
                q = skip_ws(s, n, q);
                if (q < n && s[q] == '{') {
                    size_t close = match_bracket(s, n, q, '{', '}');
                    snprintf(structtag, sizeof structtag, "%s", tagname[0] ? tagname : "");
                    if (!structtag[0]) snprintf(structtag, sizeof structtag, "$anon%d", ++s_anon_ctr);
                    if (tag_kind == 'E') parse_enumerators(s, q + 1, close - 1, structtag, file_disp);
                    else parse_members(s, q + 1, close - 1, structtag, file_disp);
                    /* register the tag itself even when synthetic (anonymous struct/union/enum), so
                     * index_resolve_tag can find it - only its NAME is invented, its members are real */
                    decl_add(structtag, strlen(structtag), tag_kind == 'S' ? DK_STRUCT : tag_kind == 'U' ? DK_UNION : DK_ENUM, "", "", file_disp);
                    if (tagname[0]) decl_add(tagname, strlen(tagname), tag_kind == 'S' ? DK_STRUCT : tag_kind == 'U' ? DK_UNION : DK_ENUM, "", "", file_disp);
                    body_end = close;
                } else if (tagname[0]) { snprintf(structtag, sizeof structtag, "%s", tagname); body_end = q; }
            }
            /* find the terminating ';' at depth 0 from body_end */
            size_t j = body_end; int depth = 0;
            while (j < n) { char c = s[j]; if (c=='('||c=='['||c=='{') ++depth; else if (c==')'||c==']'||c=='}') --depth; else if (c==';' && depth<=0) break; ++j; }
            /* function-pointer typedef: typedef RET (*NAME)(ARGS); */
            const char *starp = NULL;
            for (size_t k = body_end; k + 1 < j; ++k) if (s[k] == '(' && s[k+1] == '*') { starp = s + k; break; }
            if (starp) {
                size_t k = (size_t)(starp - s) + 2;
                char nm[56]; k = read_ident(s, j, k, nm, sizeof nm);
                if (nm[0]) decl_add(nm, strlen(nm), DK_TYPEDEF, "function pointer", "", file_disp);
            } else {
                char nm[56];
                if (last_decl_name(s, body_end, j, nm, sizeof nm) && nm[0]) {
                    decl_add(nm, strlen(nm), DK_TYPEDEF, structtag, "", file_disp);
                }
            }
            i = j + 1; continue;
        }

        /* struct/union/enum TAG { ... } [var[, var2]];  (not part of a typedef) */
        {
            char kwbuf[8]; size_t kwn = 0; char tk = 0;
            if (!memcmp(s + i, "struct", 6) && !idc((unsigned char)s[i+6])) { tk = 'S'; kwn = 6; }
            else if (!memcmp(s + i, "union", 5) && !idc((unsigned char)s[i+5])) { tk = 'U'; kwn = 5; }
            else if (!memcmp(s + i, "enum", 4) && !idc((unsigned char)s[i+4])) { tk = 'E'; kwn = 4; }
            (void)kwbuf;
            if (tk) {
                size_t q = skip_ws(s, n, i + kwn);
                char tagname[56] = ""; q = read_ident(s, n, q, tagname, sizeof tagname);
                q = skip_ws(s, n, q);
                if (q < n && s[q] == '{') {
                    size_t close = match_bracket(s, n, q, '{', '}');
                    char tag[56]; snprintf(tag, sizeof tag, "%s", tagname[0] ? tagname : "");
                    if (!tag[0]) snprintf(tag, sizeof tag, "$anon%d", ++s_anon_ctr);
                    if (tk == 'E') parse_enumerators(s, q + 1, close - 1, tag, file_disp);
                    else parse_members(s, q + 1, close - 1, tag, file_disp);
                    decl_add(tag, strlen(tag), tk == 'S' ? DK_STRUCT : tk == 'U' ? DK_UNION : DK_ENUM, "", "", file_disp);
                    if (tagname[0]) decl_add(tagname, strlen(tagname), tk == 'S' ? DK_STRUCT : tk == 'U' ? DK_UNION : DK_ENUM, "", "", file_disp);
                    /* trailing "} name, name2;" globals of this type */
                    size_t j = close; int depth = 0;
                    size_t stmt = close;
                    while (j < n) { char c = s[j]; if (c=='('||c=='['||c=='{') ++depth; else if (c==')'||c==']'||c=='}') --depth; else if (c==';' && depth<=0) break; ++j; }
                    if (j > stmt) {
                        char names[256]; size_t nl = j - stmt; if (nl >= sizeof names) nl = sizeof names - 1;
                        memcpy(names, s + stmt, nl); names[nl] = 0;
                        char *save = NULL;
                        for (char *t = strtok_r(names, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
                            char nm[56]; size_t tn = strlen(t);
                            if (last_decl_name(t, 0, tn, nm, sizeof nm) && nm[0]) decl_add(nm, strlen(nm), DK_VAR, tag, "", file_disp);
                        }
                    }
                    i = j + 1; continue;
                }
            }
        }

        /* a plain statement at top level ending in ';' : function prototype/definition or a global */
        {
            int depth = 0; size_t j = i; bool saw_paren_top = false; size_t paren_at = 0;
            while (j < n) {
                char c = s[j];
                if (c == '(') { if (depth == 0) { saw_paren_top = true; paren_at = j; } ++depth; }
                else if (c == ')') --depth;
                else if (c == '{') { if (depth <= 0) break; ++depth; }
                else if (c == '}') --depth;
                else if (c == ';' && depth <= 0) break;
                ++j;
            }
            if (j >= n) { i = n; continue; }
            if (s[j] == '{') {
                /* function definition (or a stray block) : "TYPE NAME(ARGS) {" */
                if (saw_paren_top) {
                    char nm[56];
                    if (last_decl_name(s, i, paren_at, nm, sizeof nm) && nm[0] && id0((unsigned char)nm[0])) {
                        static const char *const NOT_FN[] = { "if","for","while","switch","return","sizeof","do","else","defined",0 };
                        bool skip = false; for (int k = 0; NOT_FN[k]; ++k) if (!strcmp(NOT_FN[k], nm)) skip = true;
                        if (!skip) {
                            size_t close = match_bracket(s, n, paren_at, '(', ')');
                            char params[100]; size_t pl = close - paren_at; if (pl >= sizeof params) pl = sizeof params - 1;
                            memcpy(params, s + paren_at, pl); params[pl] = 0; tidy(params);
                            decl_add(nm, strlen(nm), DK_FUNC, params, "", file_disp);
                        }
                    }
                }
                size_t close = match_bracket(s, n, j, '{', '}');
                i = close; continue;
            }
            /* s[j] == ';' (or ran off) : declaration statement */
            if (saw_paren_top) {
                char nm[56];
                if (last_decl_name(s, i, paren_at, nm, sizeof nm) && nm[0] && id0((unsigned char)nm[0])) {
                    size_t close = match_bracket(s, n, paren_at, '(', ')');
                    /* only a prototype if nothing but ';' follows the ')' (not a call statement, not "(*fp)(...)") */
                    size_t q = skip_ws(s, j + 1, close);
                    if (q == j) {
                        char params[100]; size_t pl = close - paren_at; if (pl >= sizeof params) pl = sizeof params - 1;
                        memcpy(params, s + paren_at, pl); params[pl] = 0; tidy(params);
                        decl_add(nm, strlen(nm), DK_FUNC, params, "", file_disp);
                    }
                }
            } else {
                char nm[56];
                if (last_decl_name(s, i, j, nm, sizeof nm) && nm[0] && id0((unsigned char)nm[0])) {
                    static const char *const KWS[] = { "return","break","continue","goto","case","default",0 };
                    bool skip = false; for (int k = 0; KWS[k]; ++k) if (!strcmp(KWS[k], nm)) skip = true;
                    if (!skip) {
                        char typebuf[100]; size_t tb = j - strlen(nm) - i;
                        size_t cl = tb; if (cl >= sizeof typebuf) cl = sizeof typebuf - 1;
                        memcpy(typebuf, s + i, cl); typebuf[cl] = 0; tidy(typebuf);
                        decl_add(nm, strlen(nm), DK_VAR, typebuf, "", file_disp);
                    }
                }
            }
            i = j + 1; continue;
        }
    }
    free(s);
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */
static char s_indexed_path[256];
static uint32_t s_indexed_ver;

void index_rebuild(Doc *d, Project *proj) {
    s_ndecl = 0; s_nvisited = 0; s_anon_ctr = 0;
    if (!d) return;
    char dir[256]; path_dir(d->path[0] ? d->path : (proj && proj->loaded ? proj->root : "/"), dir, sizeof dir);
    /* the current buffer itself, unsaved edits included: index it straight from the live text
     * rather than from disk, so completion reflects what's on screen, not the last save */
    size_t n = buf_len(&d->b);
    if (n > MAX_BYTES) n = MAX_BYTES;
    const char *text = buf_text(&d->b);
    char *tmp = (char *)malloc(n + 1);
    if (tmp) {
        memcpy(tmp, text, n); tmp[n] = 0;
        /* scan_file expects a path it can fs_read_all(); write the live buffer to a scratch spot
         * only if unsaved changes exist, otherwise just scan the file on disk (cheaper, common case) */
    }
    if (tmp) free(tmp);
    if (d->path[0] && !d->dirty) {
        scan_file(d->path, dir, 0, proj);
    } else {
        /* unsaved: scan the in-memory text by treating it as file #0 manually */
        char *s = clean_copy(text, n);
        if (s) {
            /* reuse scan_file's logic by writing to a private temp path under /tmp is not available
             * cross-platform inside the editor process; instead duplicate the minimal driver here
             * by writing through fs so scan_file's single code path stays authoritative. */
            free(s);
        }
        if (fs_write_all("/tmp/.studio_index_scratch.c", text, n)) {
            scan_file("/tmp/.studio_index_scratch.c", dir, 0, proj);
        } else if (d->path[0]) {
            scan_file(d->path, dir, 0, proj);          /* fall back to the on-disk version */
        }
    }
    /* also pull in every other source the project knows about, at depth 1 (their own #includes still follow) */
    if (proj && proj->loaded) {
        char srcs[64][256]; int ns = project_sources(proj, srcs, 64);
        for (int i = 0; i < ns && s_nvisited < MAX_FILES; ++i) {
            if (d->path[0] && !strcmp(srcs[i], d->path)) continue;
            char d2[256]; path_dir(srcs[i], d2, sizeof d2);
            scan_file(srcs[i], d2, 1, proj);
        }
    }
    snprintf(s_indexed_path, sizeof s_indexed_path, "%s", d->path);
    s_indexed_ver = d->b.version;
}

void index_ensure_fresh(Doc *d, Project *proj) {
    if (!d) return;
    if (strcmp(s_indexed_path, d->path) != 0 || s_indexed_ver != d->b.version) index_rebuild(d, proj);
}

int index_find_prefix(const char *prefix, size_t plen, const void **out_decls, int max) {
    const CDecl **out = (const CDecl **)out_decls;
    int n = 0;
    for (int i = 0; i < s_ndecl && n < max; ++i) {
        if (s_decl[i].kind == DK_MEMBER || s_decl[i].kind == DK_ENUMCONST) continue;   /* not top-level names */
        if (plen && !ieq_prefix(s_decl[i].name, prefix, plen)) continue;
        out[n++] = &s_decl[i];
    }
    /* enum constants and macros ARE top-level names too */
    for (int i = 0; i < s_ndecl && n < max; ++i) {
        if (s_decl[i].kind != DK_ENUMCONST) continue;
        if (plen && !ieq_prefix(s_decl[i].name, prefix, plen)) continue;
        out[n++] = &s_decl[i];
    }
    return n;
}

int index_members(const char *tag, const char *prefix, size_t plen, const void **out_decls, int max) {
    const CDecl **out = (const CDecl **)out_decls;
    int n = 0;
    if (!tag || !tag[0]) return 0;
    for (int i = 0; i < s_ndecl && n < max; ++i) {
        if (s_decl[i].kind != DK_MEMBER) continue;
        if (strcmp(s_decl[i].parent, tag)) continue;
        if (plen && !ieq_prefix(s_decl[i].name, prefix, plen)) continue;
        out[n++] = &s_decl[i];
    }
    return n;
}

/* resolves a type text (e.g. "const Foo *", "struct Bar", "int") down to the struct/union
 * tag whose members should be offered, following typedefs; empty string if it never bottoms
 * out at a struct/union (e.g. "int", or a type we never saw a body for) */
static void strip_type_noise(char *t) {
    /* drop qualifiers and pointer stars; keep the base type word(s) */
    const char *drop[] = { "const ", "volatile ", "static ", "extern ", "unsigned ", "signed ", NULL };
    for (;;) {
        bool changed = false;
        for (int i = 0; drop[i]; ++i) { size_t l = strlen(drop[i]); if (!strncmp(t, drop[i], l)) { memmove(t, t + l, strlen(t + l) + 1); changed = true; } }
        if (!changed) break;
    }
    size_t l = strlen(t);
    while (l > 0 && (t[l-1] == '*' || t[l-1] == ' ' || t[l-1] == '\t')) t[--l] = 0;
}
const char *index_resolve_tag(const char *type_text) {
    static char buf[64];
    char t[104]; snprintf(t, sizeof t, "%s", type_text ? type_text : "");
    strip_type_noise(t);
    if (!strncmp(t, "struct ", 7)) { snprintf(buf, sizeof buf, "%s", t + 7); return buf; }
    if (!strncmp(t, "union ", 6))  { snprintf(buf, sizeof buf, "%s", t + 6); return buf; }
    if (!t[0]) return "";
    /* is t itself a struct/union tag we saw? */
    for (int i = 0; i < s_ndecl; ++i) if ((s_decl[i].kind == DK_STRUCT || s_decl[i].kind == DK_UNION) && !strcmp(s_decl[i].name, t)) { snprintf(buf, sizeof buf, "%s", t); return buf; }
    /* else follow typedef chain */
    for (int guard = 0; guard < 8; ++guard) {
        const CDecl *td = NULL;
        for (int i = 0; i < s_ndecl; ++i) if (s_decl[i].kind == DK_TYPEDEF && !strcmp(s_decl[i].name, t)) { td = &s_decl[i]; break; }
        if (!td) return "";
        char nt[104]; snprintf(nt, sizeof nt, "%s", td->detail);
        strip_type_noise(nt);
        if (!nt[0]) return "";
        for (int i = 0; i < s_ndecl; ++i) if ((s_decl[i].kind == DK_STRUCT || s_decl[i].kind == DK_UNION) && !strcmp(s_decl[i].name, nt)) { snprintf(buf, sizeof buf, "%s", nt); return buf; }
        snprintf(t, sizeof t, "%s", nt);
    }
    return "";
}

/* the declared type text of a global/local/param/member name (first match wins - locals
 * are checked before this is even called, so this only needs file-scope + index scope) */
const char *index_var_type(const char *name) {
    for (int i = 0; i < s_ndecl; ++i) if (s_decl[i].kind == DK_VAR && !strcmp(s_decl[i].name, name)) return s_decl[i].detail;
    return "";
}
const char *index_func_params(const char *name) {
    for (int i = 0; i < s_ndecl; ++i) if (s_decl[i].kind == DK_FUNC && !strcmp(s_decl[i].name, name)) return s_decl[i].detail;
    return "";
}
int index_count(void) { return s_ndecl; }
uint8_t index_kind_at(int i) { return s_decl[i].kind; }
const char *index_name_at(int i) { return s_decl[i].name; }
const char *index_detail_at(int i) { return s_decl[i].detail; }

/* Opaque-pointer accessors: index_find_prefix/index_members hand back `const void *`
 * pointers to internal CDecl records so complete.c never needs to know their layout. */
const char *decl_name(const void *p)   { return ((const CDecl *)p)->name; }
const char *decl_detail(const void *p) { return ((const CDecl *)p)->detail; }
uint8_t     decl_kind(const void *p)   { return ((const CDecl *)p)->kind; }

/* Exposes the comment/string stripper to complete.c, for scanning a bounded window of the
 * live buffer (a function body) the same safe way full files are scanned. Caller frees. */
char *index_clean_range(Doc *d, size_t a, size_t b) {
    if (b < a || !d) return NULL;
    size_t n = b - a;
    char *raw = (char *)malloc(n + 1);
    if (!raw) return NULL;
    buf_copy(&d->b, a, n, raw); raw[n] = 0;
    char *cleaned = clean_copy(raw, n);
    free(raw);
    return cleaned;
}
