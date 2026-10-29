/* sprint lint: walks the AST and reports likely mistakes (SPEC 22.2). */
#include "tools_int.h"

/* ------------------------------------------------------------------ */
/* lint                                                                 */
/* ------------------------------------------------------------------ */
typedef struct HNode {
    const char *name;
    size_t len;
} HNode;

typedef struct Lint {
    const char *file;
    int json;
    int nwarn;
    /* read-name table (open addressing) */
    HNode *tab;
    size_t cap, cnt;
} Lint;

static void warn(Lint *L, const Node *n, const char *code, const char *fmt, ...) SP_PRINTF(4, 5);
static void walk(Lint *L, const Node *n, int in_class);

static uint32_t name_hash(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void tab_add(Lint *L, const char *s, size_t n) {
    if ((L->cnt + 1) * 2 > L->cap) {
        size_t nc = L->cap ? L->cap * 2 : 256;
        HNode *nt = calloc(nc, sizeof *nt);
        if (!nt) exit(14);
        for (size_t i = 0; i < L->cap; i++)
            if (L->tab[i].name) {
                size_t j = name_hash(L->tab[i].name, L->tab[i].len) & (nc - 1);
                while (nt[j].name) j = (j + 1) & (nc - 1);
                nt[j] = L->tab[i];
            }
        free(L->tab);
        L->tab = nt;
        L->cap = nc;
    }
    size_t j = name_hash(s, n) & (L->cap - 1);
    while (L->tab[j].name) {
        if (L->tab[j].len == n && memcmp(L->tab[j].name, s, n) == 0) return;
        j = (j + 1) & (L->cap - 1);
    }
    L->tab[j].name = s; /* points into the AST arena, alive for the whole lint run */
    L->tab[j].len = n;
    L->cnt++;
}

static int tab_has(const Lint *L, const char *s, size_t n) {
    if (!L->cap) return 0;
    size_t j = name_hash(s, n) & (L->cap - 1);
    while (L->tab[j].name) {
        if (L->tab[j].len == n && memcmp(L->tab[j].name, s, n) == 0) return 1;
        j = (j + 1) & (L->cap - 1);
    }
    return 0;
}

static void warn(Lint *L, const Node *n, const char *code, const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    L->nwarn++;
    if (L->json) {
        (void)printf("{\"severity\":\"warning\",\"code\":\"%s\",\"file\":\"%s\",\"line\":%d,\"col\":%d,\"message\":\"", code,
                     L->file, n->line, n->col);
        for (const char *s = msg; *s; s++) {
            if (*s == '"' || *s == '\\') (void)fputc('\\', stdout);
            (void)fputc(*s, stdout);
        }
        (void)fputs("\"}\n", stdout);
    } else {
        (void)printf("%s:%d:%d: warning[%s]: %s\n", L->file, n->line, n->col, code, msg);
    }
}

/* Pass 1: collect every name that is read. */
static void collect_reads(Lint *L, const Node *n) {
    if (!n) return;
    if (n->kind == N_NAME) { tab_add(L, n->str, n->slen); }
    if (n->kind == N_FUNC) {
        /* parameter name nodes are declarations; only their default expressions are reads */
        if (n->a) collect_reads(L, n->a);
        if (n->b) collect_reads(L, n->b);
        for (int i = 0; i < n->n; i++) collect_reads(L, n->list[i]->a);
        if (n->c) collect_reads(L, n->c->a);
        return;
    }
    if (n->kind == S_FROM) return;
    if (n->kind == S_ASSIGN && n->op == TK_ASSIGN && n->a && n->a->kind == N_NAME) {
        collect_reads(L, n->b);
        return;
    }
    if (n->kind == S_ASSIGN && n->a && n->a->kind == N_NAME) { /* compound: a write, not a read */
        collect_reads(L, n->b);
        return;
    }
    collect_reads(L, n->a);
    collect_reads(L, n->b);
    if (n->kind != N_FUNC) collect_reads(L, n->c);
    for (int i = 0; i < n->n; i++) collect_reads(L, n->list[i]);
}

static const char *const PRELUDE[] = {"print", "len", "type", "to_string", "to_int", "to_float", "range", "error",
                                      "error_trace", "has", "delete", "deep_equal", "classof", "class_name",
                                      "is_instance", NULL};

static int is_prelude(const char *s, size_t n) {
    for (int i = 0; PRELUDE[i]; i++)
        if (strlen(PRELUDE[i]) == n && memcmp(PRELUDE[i], s, n) == 0) return 1;
    return 0;
}

static int ignorable(const char *s, size_t n) { return n == 0 || s[0] == '_' || s[0] == '$'; }

static int terminates(int kind) {
    return kind == S_RETURN || kind == S_BREAK || kind == S_CONTINUE || kind == S_THROW || kind == S_PANIC;
}

static void check_decl(Lint *L, const Node *s, int in_class) {
    if (in_class) return;
    const char *what = NULL;
    if (s->kind == S_LET || s->kind == S_CONST) what = "variable";
    else if (s->kind == S_FUNC) what = "function";
    else if (s->kind == S_CLASS) what = "class";
    if (!what || !s->str) return;
    if (!ignorable(s->str, s->slen)) {
        if (is_prelude(s->str, s->slen)) warn(L, s, "L005", "'%s' shadows a built-in function", s->str);
        if (!s->exported && !tab_has(L, s->str, s->slen)) warn(L, s, "L002", "unused %s '%s'", what, s->str);
    }
}


static void walk_block(Lint *L, Node *const *list, int cnt, int in_class) {
    for (int i = 0; i < cnt; i++) {
        const Node *s = list[i];
        if (i > 0 && terminates(list[i - 1]->kind) && !(s->kind == S_PASS)) {
            warn(L, s, "L003", "unreachable code");
            /* one report per block */
            for (int j = i; j < cnt; j++) walk(L, list[j], in_class);
            return;
        }
        walk(L, s, in_class);
    }
}

static void walk(Lint *L, const Node *n, int in_class) {
    if (!n) return;
    switch (n->kind) {
    case S_BLOCK:
        walk_block(L, n->list, n->n, 0);
        return;
    case S_LET:
    case S_CONST:
    case S_FUNC:
        check_decl(L, n, in_class);
        break;
    case S_CLASS:
        check_decl(L, n, 0);
        /* members: fields and methods are not variables */
        for (int i = 0; i < n->n; i++) walk(L, n->list[i], 1);
        walk(L, n->a, 0);
        walk(L, n->b, 0);
        walk(L, n->c, 0);
        return;
    case S_IMPORT:
        if (n->decl && !ignorable(n->decl->name, n->decl->len) && !tab_has(L, n->decl->name, n->decl->len))
            warn(L, n, "L001", "unused import '%.*s'", (int)n->decl->len, n->decl->name);
        return;
    case S_FROM:
        for (int i = 0; i < n->n; i++) {
            const Node *it = n->list[i];
            if (it->decl && !ignorable(it->decl->name, it->decl->len) && !tab_has(L, it->decl->name, it->decl->len))
                warn(L, it, "L001", "unused import '%.*s'", (int)it->decl->len, it->decl->name);
        }
        return;
    case N_TABLE: {
        for (int i = 0; i + 1 < n->n; i += 2) {
            const Node *k = n->list[i];
            if (k->kind != N_STRING && k->kind != N_INT) continue;
            for (int j = 0; j < i; j += 2) {
                const Node *o = n->list[j];
                if (o->kind != k->kind) continue;
                int same = k->kind == N_INT ? o->ival == k->ival : (o->slen == k->slen && memcmp(o->str, k->str, k->slen) == 0);
                if (same) {
                    if (k->kind == N_INT) warn(L, k, "L004", "duplicate table key %lld", (long long)k->ival);
                    else warn(L, k, "L004", "duplicate table key '%s'", k->str);
                    break;
                }
            }
        }
        break;
    }
    case S_TRY:
        if (n->b && n->b->kind == S_BLOCK && n->b->n == 1 && n->b->list[0]->kind == S_PASS)
            warn(L, n->b, "L006", "empty catch block silently discards the error");
        break;
    case N_FUNC:
        /* walk defaults and the body; parameters are not variables */
        for (int i = 0; i < n->n; i++) walk(L, n->list[i]->a, 0);
        if (n->c) walk(L, n->c->a, 0);
        walk(L, n->a, 0);
        walk(L, n->b, 0);
        return;
    default:
        break;
    }
    walk(L, n->a, 0);
    walk(L, n->b, 0);
    walk(L, n->c, 0);
    if (n->kind == S_MULTI) {
        for (int i = 0; i < n->n; i++) walk(L, n->list[i], 0);
    } else {
        for (int i = 0; i < n->n; i++) walk(L, n->list[i], in_class);
    }
}

static int lint_file(const char *path, int json, int *total) {
    char *src;
    size_t len;
    if (load_source(path, &src, &len) != 0) {
        (void)fprintf(stderr, "sprint: cannot read '%s'\n", path);
        return 3;
    }
    if (check_text(path, src, len, json)) { free(src); return 12; }
    Front F;
    memset(&F, 0, sizeof F);
    F.file = path;
    (void)snprintf(F.diag.file, sizeof F.diag.file, "%s", path);
    Arena *A = arena_new();
    TokenList tl;
    memset(&tl, 0, sizeof tl);
    Node *root = NULL;
    int bad = 0;
    if (lex_guarded(&F, src, len, &tl)) bad = 1;
    else {
        Node *volatile r = NULL;
        if (setjmp(F.jb) == 0) r = parse_program(&F, A, &tl);
        else bad = 1;
        root = r;
    }
    front_done(&F);
    int rc = 0;
    if (bad || !root) {
        print_front_diag(&F.diag, json);
        rc = 12;
    } else {
        Lint L;
        memset(&L, 0, sizeof L);
        L.file = path;
        L.json = json;
        collect_reads(&L, root);
        walk_block(&L, root->list, root->n, 0);
        *total += L.nwarn;
        free(L.tab);
    }
    toklist_free(&tl);
    arena_free(A);
    free(src);
    return rc;
}

int tool_lint(int argc, char **argv) {
    int json = 0, i = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--json") == 0) json = 1;
        else if (strcmp(argv[i], "--") == 0) { i++; break; }
        else return 2;
    }
    if (i >= argc) return 2;
    int rc = 0, total = 0;
    for (; i < argc; i++) {
        int r = lint_file(argv[i], json, &total);
        if (r && !rc) rc = r;
    }
    if (!rc && total) rc = 1;
    return rc;
}
