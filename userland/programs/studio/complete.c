/* complete.c - the completion popup and signature help.
 *
 * Four modes, chosen by what's immediately before the caret:
 *   1. #include "|  or  #include <|   -> filenames from the project dir / SDK dir
 *   2. #|                             -> preprocessor directive keywords
 *   3. expr.| or expr->|              -> members of expr's resolved struct/union tag
 *   4. otherwise, after an identifier -> locals/params, then index.c's declarations
 *      (functions, globals, typedefs, tags, macros, enum constants), then C keywords
 *
 * Ranking within a mode is: locals/params first (they're what the person is most likely
 * to mean), then symbols declared in the current file, then everything pulled in through
 * #include, then keywords - each group alphabetical, all filtered by prefix.
 *
 * Signature help (index_func_params) is independent of the popup: it re-derives itself
 * from scratch after every keystroke by scanning outward for an enclosing call, so it
 * never gets out of sync with edits.
 */
#include "studio.h"

static bool idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= '0' && c <= '9'); }
static bool id0(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int  lcase(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static bool ieq_prefix(const char *w, const char *p, size_t pl) {
    for (size_t i = 0; i < pl; ++i) { if (!w[i]) return false; if (lcase((unsigned char)w[i]) != lcase((unsigned char)p[i])) return false; }
    return true;
}

static const char *const C_KEYWORDS[] = {
    "auto","break","case","char","const","continue","default","do","double","else","enum","extern","float","for",
    "goto","if","inline","int","long","register","restrict","return","short","signed","sizeof","static","struct",
    "switch","typedef","union","unsigned","void","volatile","while","_Bool","bool","true","false","NULL", NULL
};

static void push_item(const char *name, size_t nlen, uint8_t kind, const char *detail) {
    if (G.comp_n >= COMP_MAX || nlen == 0 || nlen >= 64) return;
    for (int i = 0; i < G.comp_n; ++i) if ((size_t)strlen(G.comp_items[i]) == nlen && !memcmp(G.comp_items[i], name, nlen)) return;
    memcpy(G.comp_items[G.comp_n], name, nlen); G.comp_items[G.comp_n][nlen] = 0;
    G.comp_kind[G.comp_n] = kind;
    snprintf(G.comp_detail[G.comp_n], sizeof G.comp_detail[0], "%s", detail ? detail : "");
    ++G.comp_n;
}

/* ------------------------------------------------------------------ */
/* locals & parameters: scan the enclosing function body up to caret   */
/* ------------------------------------------------------------------ */
#define MAX_LOCALS 64
typedef struct { char name[56]; char type[100]; } Local;

/* finds the '{' that opens the function body containing `caret` (by brace-depth walking
 * backward from caret), and the position just after the '(' of its parameter list. Returns
 * false if caret doesn't look like it's inside a function body (e.g. file scope). */
static bool find_enclosing_function(Doc *d, size_t caret, size_t *body_open, size_t *sig_start) {
    int depth = 0;
    size_t i = caret;
    *body_open = (size_t)-1;
    while (i > 0) {
        --i;
        char c = buf_at(&d->b, i);
        if (c == '}') ++depth;
        else if (c == '{') { if (depth == 0) { *body_open = i; break; } --depth; }
        if (i == 0) return false;
    }
    if (i == 0 && buf_at(&d->b, 0) != '{') return false;
    if (i == 0) *body_open = 0;
    /* walk back over the signature "TYPE NAME(PARAMS)" to its start: a line that, once
     * blank-trimmed, ends the previous statement (';', '}', or buffer start) */
    size_t j = *body_open;
    while (j > 0 && (buf_at(&d->b, j - 1) == ' ' || buf_at(&d->b, j - 1) == '\t' || buf_at(&d->b, j - 1) == '\n' || buf_at(&d->b, j - 1) == '\r')) --j;
    int pd = 0; size_t k = j;
    while (k > 0) {
        --k;
        char c = buf_at(&d->b, k);
        if (c == ')') ++pd;
        else if (c == '(') { --pd; if (pd == 0) break; }
    }
    if (k == 0 && buf_at(&d->b, 0) != '(') return false;
    *sig_start = k + 1;              /* just after the opening '(' of the parameter list */
    return true;
}

static void parse_decls_in_range(const char *s, size_t n, Local *out, int *cnt, int max) {
    size_t i = 0;
    while (i < n && *cnt < max) {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
        if (i >= n) break;
        size_t start = i;
        int depth = 0; size_t j = i;
        while (j < n) { char c = s[j]; if (c=='('||c=='['||c=='{') ++depth; else if (c==')'||c==']'||c=='}') --depth; else if ((c==';'||c==',') && depth<=0) break; ++j; }
        size_t end = j;
        /* skip obvious non-declarations: control keywords, lines with '=' before any type-looking word is fine (still a decl) */
        char stmt[200]; size_t sl = end - start; if (sl >= sizeof stmt) sl = sizeof stmt - 1;
        memcpy(stmt, s + start, sl); stmt[sl] = 0;
        bool looks_ctrl = false;
        static const char *const CTRL[] = { "if","for","while","switch","return","case","default","else","do","break","continue","goto",0 };
        { size_t k=0; while (k<sl && id0((unsigned char)stmt[k])) ++k; char w[16]; size_t wl=k<15?k:15; memcpy(w,stmt,wl); w[wl]=0;
          for (int c=0; CTRL[c]; ++c) if (!strcmp(CTRL[c], w)) looks_ctrl = true; }
        if (!looks_ctrl && sl > 0) {
            /* strip an initializer "= ..." at depth 0 (rough: cut at first top-level '=' not part of ==,<=,>=,!=) */
            int d2 = 0; size_t eq = sl;
            for (size_t k = 0; k < sl; ++k) {
                char c = stmt[k];
                if (c=='('||c=='['||c=='{') ++d2; else if (c==')'||c==']'||c=='}') --d2;
                else if (c=='=' && d2<=0 && (k==0||(stmt[k-1]!='='&&stmt[k-1]!='<'&&stmt[k-1]!='>'&&stmt[k-1]!='!')) && (k+1>=sl||stmt[k+1]!='=')) { eq = k; break; }
            }
            /* one or more comma-separated declarators sharing a base type before the FIRST name */
            char decl0[200]; size_t dl = eq < sl ? eq : sl; if (dl >= sizeof decl0) dl = sizeof decl0 - 1;
            memcpy(decl0, stmt, dl); decl0[dl] = 0;
            /* find the last identifier in decl0 -> the declared name; everything before (minus stars) is the type */
            size_t e = dl; while (e > 0 && (decl0[e-1]==' '||decl0[e-1]=='\t')) --e;
            /* strip a trailing [...] */
            while (e > 0 && decl0[e-1] == ']') { int bd=0; size_t q=e; while(q>0){--q; if(decl0[q]==']')++bd; else if(decl0[q]=='['){--bd; if(!bd)break;}} e=q; while (e>0 && (decl0[e-1]==' '||decl0[e-1]=='\t')) --e; }
            size_t nmend = e; while (e > 0 && idc((unsigned char)decl0[e-1])) --e;
            if (nmend > e && id0((unsigned char)decl0[e])) {
                size_t nl = nmend - e; char nm[56]; if (nl >= sizeof nm) nl = sizeof nm - 1; memcpy(nm, decl0 + e, nl); nm[nl]=0;
                static const char *const KW[] = { "return","void",0 };
                bool kwskip=false; for(int c=0;KW[c];++c) if(!strcmp(KW[c],nm)) kwskip=true;
                if (!kwskip && *cnt < max) {
                    bool ptr = e>0 && decl0[e-1]=='*';
                    size_t tb = e; while (tb>0 && (decl0[tb-1]=='*'||decl0[tb-1]==' '||decl0[tb-1]=='\t')) --tb;
                    char ty[100]; size_t tl = tb; if (tl >= sizeof ty) tl = sizeof ty - 1; memcpy(ty, decl0, tl); ty[tl]=0;
                    /* collapse internal whitespace */
                    char *w=ty; bool sp=false; for(char *r=ty;*r;++r){ if(*r==' '||*r=='\t'){ if(w>ty) sp=true; continue;} if(sp){*w++=' ';sp=false;} *w++=*r; } *w=0;
                    if (ptr) strlcat(ty, " *", sizeof ty);
                    snprintf(out[*cnt].name, sizeof out[0].name, "%s", nm);
                    snprintf(out[*cnt].type, sizeof out[0].type, "%s", ty);
                    ++*cnt;
                }
            }
        }
        i = end < n ? end + 1 : end;
    }
}

static int collect_scope(Doc *d, size_t caret, Local *out, int max) {
    int cnt = 0;
    size_t body_open, sig_start;
    if (!find_enclosing_function(d, caret, &body_open, &sig_start)) return 0;
    /* parameters: from sig_start to the matching ')' */
    int pd = 1; size_t k = sig_start;
    size_t blen = buf_len(&d->b);
    while (k < blen && pd > 0) { char c = buf_at(&d->b, k); if (c=='(') ++pd; else if (c==')') { --pd; if (!pd) break; } ++k; }
    char *ps = index_clean_range(d, sig_start, k);
    if (ps) { parse_decls_in_range(ps, k - sig_start, out, &cnt, max); free(ps); }
    /* locals: from just after the body's '{' to caret */
    char *bs = index_clean_range(d, body_open + 1, caret);
    if (bs) { parse_decls_in_range(bs, caret - (body_open + 1), out + cnt, &cnt, max); free(bs); }
    return cnt;
}

static bool scope_find(Local *locals, int n, const char *name, char *type_out, size_t cap) {
    for (int i = n - 1; i >= 0; --i)     /* most recent declaration wins */
        if (!strcmp(locals[i].name, name)) { snprintf(type_out, cap, "%s", locals[i].type); return true; }
    return false;
}

/* ------------------------------------------------------------------ */
/* member-expression resolution: "a.b[3]->c" -> the struct tag whose   */
/* members follow the final '.'/'->' , or "" if it can't be resolved   */
/* ------------------------------------------------------------------ */
static const char *resolve_expr_tag(Doc *d, size_t end, Local *locals, int nlocals) {
    static char tagbuf[64];
    char segs[8][56]; int nseg = 0;
    size_t p = end;
    for (;;) {
        while (p > 0 && (buf_at(&d->b, p - 1) == ' ' || buf_at(&d->b, p - 1) == '\t')) --p;
        while (p > 0 && buf_at(&d->b, p - 1) == ']') {
            int depth = 0; size_t q = p;
            do { --q; char c = buf_at(&d->b, q); if (c == ']') ++depth; else if (c == '[') --depth; } while (q > 0 && depth > 0);
            p = q;
            while (p > 0 && (buf_at(&d->b, p - 1) == ' ' || buf_at(&d->b, p - 1) == '\t')) --p;
        }
        size_t idend = p;
        while (p > 0 && idc((unsigned char)buf_at(&d->b, p - 1))) --p;
        if (p == idend || !id0((unsigned char)buf_at(&d->b, p))) return "";
        size_t nl = idend - p; if (nl >= sizeof segs[0]) return "";
        if (nseg < 8) { char tmp[56]; buf_copy(&d->b, p, nl, tmp); tmp[nl] = 0; snprintf(segs[nseg], sizeof segs[0], "%s", tmp); ++nseg; }
        size_t q = p;
        while (q > 0 && (buf_at(&d->b, q - 1) == ' ' || buf_at(&d->b, q - 1) == '\t')) --q;
        if (q > 0 && buf_at(&d->b, q - 1) == '.') { p = q - 1; continue; }
        if (q > 1 && buf_at(&d->b, q - 1) == '>' && buf_at(&d->b, q - 2) == '-') { p = q - 2; continue; }
        break;
    }
    if (nseg == 0) return "";
    char basetype[104] = "";
    if (!scope_find(locals, nlocals, segs[nseg - 1], basetype, sizeof basetype))
        snprintf(basetype, sizeof basetype, "%s", index_var_type(segs[nseg - 1]));
    char tag[64]; snprintf(tag, sizeof tag, "%s", index_resolve_tag(basetype));
    for (int k = nseg - 2; k >= 0; --k) {
        if (!tag[0]) return "";
        const void *out[1];
        int c = index_members(tag, segs[k], strlen(segs[k]), out, 1);
        if (c < 1 || strcmp(decl_name(out[0]), segs[k]) != 0) return "";   /* index_members prefix-matches; require exact */
        snprintf(tag, sizeof tag, "%s", index_resolve_tag(decl_detail(out[0])));
    }
    snprintf(tagbuf, sizeof tagbuf, "%s", tag);
    return tagbuf;
}

/* ------------------------------------------------------------------ */
/* the four modes                                                      */
/* ------------------------------------------------------------------ */
static const char *kind_label(uint8_t k) {
    switch (k) {
        case DK_FUNC: return "fn"; case DK_VAR: return "var"; case DK_TYPEDEF: return "type";
        case DK_STRUCT: return "struct"; case DK_UNION: return "union"; case DK_ENUM: return "enum";
        case DK_MACRO: return "macro"; case DK_ENUMCONST: return "const"; case DK_MEMBER: return "member";
        default: return "";
    }
}

static void complete_members(Doc *d, size_t op_end, size_t word_start, size_t word_end) {
    Local locals[MAX_LOCALS]; int nl = collect_scope(d, d->caret, locals, MAX_LOCALS);
    const char *tag = resolve_expr_tag(d, op_end, locals, nl);
    if (!tag[0]) return;
    char pre[56]; size_t pl = word_end - word_start; if (pl >= sizeof pre) pl = sizeof pre - 1;
    buf_copy(&d->b, word_start, pl, pre); pre[pl] = 0;
    const void *out[COMP_MAX];
    int n = index_members(tag, pre, pl, out, COMP_MAX);
    for (int i = 0; i < n; ++i) push_item(decl_name(out[i]), strlen(decl_name(out[i])), DK_MEMBER, decl_detail(out[i]));
    if (G.comp_n) { G.comp_open = true; G.comp_sel = 0; G.comp_from = word_start; G.comp_is_member = true; }
}

static void complete_include(Doc *d, bool angled, size_t path_start, size_t caret) {
    char pre[160]; size_t pl = caret - path_start; if (pl >= sizeof pre) pl = sizeof pre - 1;
    buf_copy(&d->b, path_start, pl, pre); pre[pl] = 0;
    char dir[256];
    if (angled) snprintf(dir, sizeof dir, "/system/sdk/include");
    else { path_dir(d->path[0] ? d->path : "/", dir, sizeof dir); }
    /* only the trailing component after the last '/' is what we're completing */
    char subdir[256] = ""; const char *base = pre;
    const char *slash = strrchr(pre, '/');
    if (slash) { size_t dn = (size_t)(slash - pre); if (dn >= sizeof subdir) dn = sizeof subdir - 1; memcpy(subdir, pre, dn); subdir[dn] = 0; base = slash + 1; }
    char full[300]; if (subdir[0]) path_join(full, sizeof full, dir, subdir); else snprintf(full, sizeof full, "%s", dir);
    size_t bl = strlen(base);
    int fd = cos_opendir(full);
    if (fd >= 0) {
        cos_dirent_t de;
        while (G.comp_n < COMP_MAX && cos_readdir(fd, &de) == 1) {
            if (de.name[0] == '.') continue;
            if (!de.is_dir && !has_ext(de.name, ".h") && !has_ext(de.name, ".hpp")) continue;
            if (bl && !ieq_prefix(de.name, base, bl)) continue;
            char shown[64]; snprintf(shown, sizeof shown, "%s%s", de.name, de.is_dir ? "/" : "");
            push_item(shown, strlen(shown), 255 /* not a DK_*, drawn plainly */, de.is_dir ? "folder" : "header");
        }
        cos_close(fd);
    }
    if (G.comp_n) { G.comp_open = true; G.comp_sel = 0; G.comp_from = path_start + (size_t)(base - pre); G.comp_is_include = true; }
}

static void complete_directive(Doc *d, size_t word_start, size_t caret) {
    static const char *const DIRS[] = { "include","define","ifdef","ifndef","endif","else","elif","undef","pragma","error","if", NULL };
    char pre[32]; size_t pl = caret - word_start; if (pl >= sizeof pre) pl = sizeof pre - 1;
    buf_copy(&d->b, word_start, pl, pre); pre[pl] = 0;
    for (int i = 0; DIRS[i] && G.comp_n < COMP_MAX; ++i) if (!pl || ieq_prefix(DIRS[i], pre, pl)) push_item(DIRS[i], strlen(DIRS[i]), 255, "directive");
    if (G.comp_n) { G.comp_open = true; G.comp_sel = 0; G.comp_from = word_start; }
}

static void complete_identifier(Doc *d, bool force) {
    size_t p = d->caret, a = p;
    while (a > 0 && idc((unsigned char)buf_at(&d->b, a - 1))) --a;
    size_t pl = p - a;
    if (!force && (pl < 2 || pl > 60)) return;
    if (p < buf_len(&d->b) && idc((unsigned char)buf_at(&d->b, p))) return;    /* caret is inside a word */
    char pre[64]; if (pl >= sizeof pre) pl = sizeof pre - 1; buf_copy(&d->b, a, pl, pre); pre[pl] = 0;
    if (pre[0] >= '0' && pre[0] <= '9') return;
    G.comp_from = a;

    Local locals[MAX_LOCALS]; int nl = collect_scope(d, d->caret, locals, MAX_LOCALS);
    for (int i = nl - 1; i >= 0 && G.comp_n < COMP_MAX; --i)
        if ((!pl || ieq_prefix(locals[i].name, pre, pl)) && strlen(locals[i].name) > pl)
            push_item(locals[i].name, strlen(locals[i].name), DK_VAR, locals[i].type);

    const void *out[COMP_MAX];
    int n = index_find_prefix(pre, pl, out, COMP_MAX - G.comp_n);
    for (int i = 0; i < n && G.comp_n < COMP_MAX; ++i) {
        const char *nm = decl_name(out[i]);
        if (strlen(nm) > pl) push_item(nm, strlen(nm), decl_kind(out[i]), decl_detail(out[i]));
    }
    for (int i = 0; C_KEYWORDS[i] && G.comp_n < COMP_MAX; ++i) {
        size_t wl = strlen(C_KEYWORDS[i]);
        if (wl > pl && (!pl || ieq_prefix(C_KEYWORDS[i], pre, pl))) push_item(C_KEYWORDS[i], wl, 255, "keyword");
    }
    if (G.comp_n) { G.comp_open = true; G.comp_sel = 0; }
}

void comp_update(Doc *d) { comp_update_auto(d, false); }

void comp_update_auto(Doc *d, bool force) {
    G.comp_open = false; G.comp_n = 0; G.comp_is_member = false; G.comp_is_include = false;
    if (!d || doc_has_sel(d)) return;
    index_ensure_fresh(d, &G.proj);
    size_t caret = d->caret;
    size_t ls = buf_line_start(&d->b, buf_line_of(&d->b, caret));

    /* #include "..."  or  #include <...> , still open on this line before the caret */
    {
        char line[300]; size_t ll = caret - ls; if (ll >= sizeof line) ll = sizeof line - 1;
        buf_copy(&d->b, ls, ll, line); line[ll] = 0;
        char *inc = strstr(line, "#include");
        if (inc) {
            char *q = strchr(inc, '"'), *ab = strchr(inc, '<');
            char open = 0; long off = -1;
            if (q && (!ab || q < ab)) { open = '"'; off = (q - line) + 1; }
            else if (ab) { open = '<'; off = (ab - line) + 1; }
            if (open) {
                const char *rest = line + off;
                char close = open == '"' ? '"' : '>';
                if (!strchr(rest, close)) { complete_include(d, open == '<', ls + (size_t)off, caret); return; }
            }
        }
    }
    /* '#' then a partial word and nothing else before it on the line */
    {
        size_t p = ls;
        while (p < caret && (buf_at(&d->b, p) == ' ' || buf_at(&d->b, p) == '\t')) ++p;
        if (p < caret && buf_at(&d->b, p) == '#') {
            size_t q = p + 1; bool ok = true;
            for (size_t k = q; k < caret; ++k) if (!idc((unsigned char)buf_at(&d->b, k))) { ok = false; break; }
            if (ok) { complete_directive(d, q, caret); return; }
        }
    }
    /* member access: caret is right after '.'|'->' plus whatever of the member name is typed */
    {
        size_t wstart = caret;
        while (wstart > 0 && idc((unsigned char)buf_at(&d->b, wstart - 1))) --wstart;
        size_t q = wstart;
        bool dot = q > 0 && buf_at(&d->b, q - 1) == '.';
        bool arrow = !dot && q > 1 && buf_at(&d->b, q - 1) == '>' && buf_at(&d->b, q - 2) == '-';
        if (dot || arrow) { complete_members(d, dot ? q - 1 : q - 2, wstart, caret); return; }
    }
    complete_identifier(d, force);
}

void comp_accept(Doc *d) {
    if (!G.comp_open || G.comp_sel < 0 || G.comp_sel >= G.comp_n) return;
    const char *w = G.comp_items[G.comp_sel];
    uint8_t kind = G.comp_kind[G.comp_sel];
    size_t a = G.comp_from, p = d->caret;
    if (a > p) { G.comp_open = false; return; }
    char ins[80]; snprintf(ins, sizeof ins, "%s", w);
    bool place_inside_parens = false;
    if (kind == DK_FUNC && !G.comp_is_member) { strlcat(ins, "()", sizeof ins); place_inside_parens = true; }
    doc_group_begin(d);
    doc_replace(d, a, p - a, ins, strlen(ins), false);
    doc_group_end(d);
    d->caret = d->anchor = a + strlen(ins) - (place_inside_parens ? 1 : 0);
    G.comp_open = false;
    if (place_inside_parens) sig_update(d);   /* land inside the parens with signature help already showing */
}

/* ------------------------------------------------------------------ */
/* signature help                                                      */
/* ------------------------------------------------------------------ */
static void split_params(const char *s, char out[8][48], int *n) {
    *n = 0;
    size_t len = strlen(s);
    if (len < 2) return;
    size_t a = 1, end = len; while (end > 0 && s[end - 1] != ')') --end;   /* drop the trailing ')' and anything after */
    if (end > 0) --end; else end = len;
    int depth = 0; size_t start = a;
    for (size_t i = a; i <= end && *n < 8; ++i) {
        char c = i < end ? s[i] : ',';
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') --depth;
        else if (c == ',' && depth <= 0) {
            size_t b = i; while (b > start && (s[b-1]==' ')) --b;
            size_t st = start; while (st < b && s[st]==' ') ++st;
            size_t l = b - st; if (l >= 48) l = 47;
            if (l) { memcpy(out[*n], s + st, l); out[*n][l] = 0; ++*n; }
            start = i + 1;
        }
    }
    if (*n == 1 && (!strcmp(out[0], "void") || out[0][0] == 0)) *n = 0;
}

void sig_update(Doc *d) {
    G.sig_open = false;
    if (!d) return;
    index_ensure_fresh(d, &G.proj);   /* cheap when already current: a path+version compare, no rescan */
    size_t p = d->caret;
    int depth = 0; size_t open_paren = (size_t)-1;
    size_t limit = p > 400 ? p - 400 : 0;
    while (p > limit) {
        --p;
        char c = buf_at(&d->b, p);
        if (c == ')') ++depth;
        else if (c == '(') { if (depth == 0) { open_paren = p; break; } --depth; }
        else if (c == ';' || c == '{' || c == '}') break;    /* left the statement: no enclosing call */
    }
    if (open_paren == (size_t)-1) return;
    size_t nameend = open_paren; while (nameend > 0 && (buf_at(&d->b, nameend - 1) == ' ' || buf_at(&d->b, nameend - 1) == '\t')) --nameend;
    size_t namestart = nameend;
    while (namestart > 0 && idc((unsigned char)buf_at(&d->b, namestart - 1))) --namestart;
    if (namestart == nameend) return;
    char nm[56]; size_t nl = nameend - namestart; if (nl >= sizeof nm) nl = sizeof nm - 1;
    buf_copy(&d->b, namestart, nl, nm); nm[nl] = 0;
    Local locals[MAX_LOCALS]; int nlv = collect_scope(d, d->caret, locals, MAX_LOCALS);
    char localtype[104];
    if (scope_find(locals, nlv, nm, localtype, sizeof localtype)) return;   /* it's a variable, e.g. a function pointer call: skip */
    const char *params = index_func_params(nm);
    if (params[0] != '(') return;
    snprintf(G.sig_name, sizeof G.sig_name, "%s", nm);
    split_params(params, G.sig_params, &G.sig_nparams);
    /* which parameter holds the caret: count top-level commas between open_paren+1 and d->caret */
    int idx = 0; int d2 = 0;
    for (size_t k = open_paren + 1; k < d->caret; ++k) {
        char c = buf_at(&d->b, k);
        if (c=='('||c=='[') ++d2; else if (c==')'||c==']') --d2; else if (c==',' && d2<=0) ++idx;
    }
    G.sig_active = G.sig_nparams ? (idx < G.sig_nparams ? idx : G.sig_nparams - 1) : -1;
    G.sig_paren_pos = open_paren;
    G.sig_open = true;
}
