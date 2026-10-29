/* Compiler stage (layer 4+5): types and the functions shared by the compiler parts. */
#ifndef COMPILER_INT_H
#define COMPILER_INT_H

/* Layer 4+5: resolver + bytecode compiler (SPEC 5, 17).
 *
 * Closure capture analysis: the whole program is compiled twice. The first
 * ("dry") pass only records which declarations are captured by an inner
 * function (Decl.captured); the second pass emits the real code and uses that
 * knowledge to decide which locals live in heap cells. */
#include "sp.h"

typedef struct Local {
    const char *name;
    size_t len;
    Decl *decl;
    int slot;
    int depth;
    int cell_ready;
} Local;

typedef struct Up {
    int from_local;
    int index;
    Decl *decl;
} Up;

typedef struct ExitEnt {
    int is_defer; /* 0 = try handler, 1 = defer scope */
    int slot;
} ExitEnt;

typedef struct Loop {
    const char *label;
    size_t llen;
    int nexits;
    uint32_t *breaks;
    int nbreaks, capbreaks;
    uint32_t cont_pc;
} Loop;

typedef struct MVar {
    const char *name;
    size_t len;
    int is_const;
    const char *modname; /* non-NULL for `import m` bindings of builtin modules */
    int user_mod;        /* >= 0: `import m` binding of a user module */
    int alias_slot;      /* >= 0: `from m import x` alias of another module's slot */
} MVar;

typedef struct ClassCtx {
    Node *decl;
    struct ClassCtx *parent;
    int slot;        /* module variable slot of the class */
    int base_fields; /* number of inherited fields */
    int nfields;     /* own fields */
    int foreign;     /* describes a class exported by an earlier module (decl is a name-only copy) */
    int has_fin;     /* exported class: module slot of its field-initialiser function (whole chain) */
    int fin_slot;
} ClassCtx;

typedef struct FS {
    struct FS *parent;
    Proto *p;
    Local *loc;
    int nloc, caploc;
    int scope_depth;
    int next_slot, max_slot;
    Up *up;
    int nup, capup;
    Loop *loops;
    int nloops, caploops;
    int depth, max_depth;
    ExitEnt *exits;
    int nexits, capexits;
    int is_module;
    ClassCtx *cls; /* set while compiling a method body */
    int is_init;
    int super_slot;
    int cur_line, cur_col;
    int last_line, last_col;
} FS;

typedef struct Comp {
    Front *F;
    VM *vm;
    Program *prog;
    FS *fs;
    int dry;
    MVar *mv;
    int nmv, capmv;
    ClassCtx **classes;
    int ncls, capcls;
    Program *real;  /* program holding the already-compiled modules */
    int mv_start;   /* first module variable visible to the module being compiled */
    int dup_floor;  /* module variables from here on belong to the current module (redeclaring one is an error) */
    int name_start; /* first module variable whose name is recorded for this module */
    int ncls_keep;  /* REPL: classes of earlier chunks, kept across passes */
    int cur_mod;
    int entry_proto;
    ExportEnt *exports;
    int nexp, capexp;
    ClassCtx *pending_cls;
    int pending_init;
} Comp;

typedef enum ResKind { R_LOCAL, R_UPVAL, R_MODULE, R_LINK } ResKind;
typedef struct Res {
    ResKind kind;
    int idx;
    int is_const;
    int cell; /* R_LOCAL: stored in a cell */
    const char *modname;
    int user_mod;
} Res;

/* functions shared between the parts of this stage */
void *xrealloc(void *p, size_t n);
SP_NORETURN void cerr(Comp *c, const Node *n, const char *kind, int code, const char *fmt, ...) SP_PRINTF(5, 6);
void setpos(Comp *c, const Node *n);
Proto *new_proto(Comp *c, const char *name);
int proto_index(const Comp *c, const Proto *p);
void e8(Comp *c, unsigned b);
void e16(Comp *c, unsigned v);
void e32(Comp *c, uint32_t v);
void emit_op(Comp *c, int op, int delta);
void op0(Comp *c, int op, int delta);
void op_u16(Comp *c, int op, unsigned v, int delta);
void op_u8(Comp *c, int op, unsigned v, int delta);
uint32_t jump_fwd(Comp *c, int op, int delta);
void patch_here(Comp *c, uint32_t at);
void jump_back(Comp *c, int op, uint32_t target, int delta);
int add_const(Comp *c, SpValue v);
int str_const(Comp *c, const char *s, size_t len);
void emit_const_str(Comp *c, const char *s, size_t len);
int add_link(Comp *c, int kind, const char *module, const char *member);
void fs_init(FS *fs, FS *parent, Proto *p);
void fs_free(FS *fs);
void begin_scope(Comp *c);
void end_scope(Comp *c);
int declare_local(Comp *c, const Node *where, Decl *d);
int declare_hidden(Comp *c, const Node *where);
int find_mvar(const Comp *c, const char *name, size_t len);
int declare_mvar(Comp *c, const Node *where, const char *name, size_t len, int is_const, const char *modname);
int declare_hidden_mvar(Comp *c, const Node *where);
Res resolve_name(Comp *c, const Node *n);
void emit_load(Comp *c, Res r);
void emit_store(Comp *c, Res r);
int binop_code(int tk);
void emit_int(Comp *c, int64_t v);
void check_member(Comp *c, const Node *obj, const Node *m);
void expr(Comp *c, Node *n);
int at_module_top(const Comp *c);
void stmt_list(Comp *c, Node **list, int n);
void block_stmts(Comp *c, Node *b);
const ClassCtx *persist_chain(Program *pg, const ClassCtx *k);
void free_foreign_classes(Program *pg);
void compile_class(Comp *c, Node *n);
void compile_super_call(Comp *c, Node *n);
void compile_super_method(Comp *c, Node *n);
int member_user_slot(Comp *c, const Node *obj, const Node *m);
void export_add(Comp *c, const Node *where, const char *name, size_t len, int slot);
void compile_import(Comp *c, Node *n);
void compile_from(Comp *c, Node *n);
int compile_func(Comp *c, Node *fn, Proto *shell);

#endif
