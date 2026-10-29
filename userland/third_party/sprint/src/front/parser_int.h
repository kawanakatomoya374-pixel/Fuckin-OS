/* Parser (layer 3, SPEC 4): the parser state, the token helpers and the functions shared by the parser parts. */
#ifndef PARSER_INT_H
#define PARSER_INT_H

/* Layer 3: parser (SPEC 4). Produces an arena-allocated AST. */
#include "sp.h"

typedef struct Parser {
    Front *F;
    Arena *A;
    const Token *t;
    int n, p;
    int depth;
    int pending_nl; /* a match expression just ended its indented block: it stands in for the statement's newline */
    int *ndestr; /* counter for hidden destructuring temporaries (shared with sub-parsers) */
} Parser;

#define MAX_NEST 256

static inline const Token *cur(Parser *P) { return &P->t[P->p]; }
static inline const Token *peek_at(Parser *P, int k) {
    int i = P->p + k;
    return i < P->n ? &P->t[i] : &P->t[P->n - 1];
}
static inline int at(Parser *P, int kind) { return cur(P)->kind == kind; }

SP_NORETURN void perr_at(Parser *P, const Token *t, const char *fmt, ...) SP_PRINTF(3, 4);

static inline const Token *advance(Parser *P) {
    const Token *t = cur(P);
    if (t->kind != TK_EOF) P->p++;
    return t;
}

static inline int accept(Parser *P, int kind) {
    if (at(P, kind)) { P->p++; return 1; }
    return 0;
}

static inline const Token *expect(Parser *P, int kind, const char *what) {
    if (!at(P, kind)) perr_at(P, cur(P), "expected %s, found %s", what, tok_name(cur(P)->kind));
    return advance(P);
}

static inline Node *mk(Parser *P, int kind, const Token *t) {
    Node *n = arena_alloc(P->A, sizeof *n);
    n->kind = kind;
    n->line = t->line;
    n->col = t->col;
    n->offset = t->offset;
    return n;
}

static inline Decl *mkdecl(Parser *P, const char *name, size_t len, int is_const) {
    Decl *d = arena_alloc(P->A, sizeof *d);
    d->name = arena_strndup(P->A, name, len);
    d->len = len;
    d->is_const = is_const;
    return d;
}

typedef struct NodeVec {
    Node **v;
    int n, cap;
} NodeVec;
static inline void nv_push(Parser *P, NodeVec *v, Node *n) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        Node **nv = arena_alloc(P->A, sizeof(Node *) * (size_t)nc);
        if (v->n) memcpy(nv, v->v, sizeof(Node *) * (size_t)v->n);
        v->v = nv;
        v->cap = nc;
    }
    v->v[v->n++] = n;
}

static inline void enter(Parser *P) {
    if (++P->depth > MAX_NEST) perr_at(P, cur(P), "nesting too deep (limit %d)", MAX_NEST);
}
static inline void leave(Parser *P) { P->depth--; }

/* functions shared between the parts of this stage */
Node *primary(Parser *P);
Node *postfix(Parser *P);
Node *bin(Parser *P, const Token *t, int op, Node *a, Node *b);
Node *or_expr(Parser *P);
Node *pipe_expr(Parser *P);
Node *conditional(Parser *P);
Node *expression(Parser *P);
int is_name_tok(int k);
const Token *expect_name(Parser *P, const char *what);
Node *parse_func_rest(Parser *P, const Token *ftok, const Token *name, int lambda);
Node *make_name(Parser *P, const Token *t);
Node *match_expr(Parser *P);
Node *dlet(Parser *P, const Token *t, int is_const, const char *name, size_t len, Node *init);
Node *loop_pattern(Parser *P, const Token *kw, Decl **decl);
Node *block_with_prefix(Parser *P, Node *pre, Node *body);

#endif
