/* Sprint 2.0 reference implementation - internal header (milestone 1) */
#ifndef SP_H
#define SP_H

#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define SP_NORETURN __attribute__((noreturn))
#define SP_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define SP_NORETURN
#define SP_PRINTF(a, b)
#endif


/* The language requires exact IEEE-754 comparison (SPEC 7.5, 7.6). This is the
 * single place where -Wfloat-equal is silenced; every other site must use it. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif
static inline int sp_feq(double a, double b) { return a == b; }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ------------------------------------------------------------------ */
/* error codes (SPEC 12.2)                                             */
/* ------------------------------------------------------------------ */
enum {
    E_TYPE = 1, E_NAME = 2, E_INDEX = 3, E_KEY = 4, E_MATCH = 5, E_ARITH = 6,
    E_IMPORT = 7, E_IO = 8, E_CONST = 9, E_ASSERT = 10, E_RUNTIME = 11,
    E_PARSE = 12, E_PERM = 14, E_LEX = 15, E_VALUE = 16, E_OVERRIDE = 17,
    E_LOAD = 18, E_MEMORY = 19, E_TIMEOUT = 20, E_PANIC = 21, E_CLOSE = 22, E_USER = 100
};
const char *err_kind_name(int code);
int err_kind_code(const char *name, size_t len); /* 0 if not a runtime kind */

/* ------------------------------------------------------------------ */
/* diagnostics / front-end context                                     */
/* ------------------------------------------------------------------ */
typedef struct Diag {
    const char *kind;
    int code;
    char msg[256];
    char file[256];
    int line, col;
    size_t offset;
} Diag;

/* Cleanup stack: resources registered here are released if a front-end
 * error unwinds (longjmp) past their owner, so a failed compile leaks nothing. */
typedef struct FrontCl {
    void (*fn)(void *);
    void *p;
} FrontCl;

typedef struct Front {
    Diag diag;
    int has_diag;
    jmp_buf jb;
    const char *file;
    FrontCl *cl;
    int ncl, capcl;
} Front;

void front_push(Front *F, void (*fn)(void *), void *p);
void front_pop(Front *F);
void front_done(Front *F); /* release the cleanup stack itself */
void front_free_ptr(void *p);

SP_NORETURN void front_error(Front *F, const char *kind, int code, int line, int col,
                             size_t off, const char *fmt, ...) SP_PRINTF(7, 8);

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */
#define KEYWORD_LIST(X)                                                              \
    X(AND, "and") X(AS, "as") X(ASSERT, "assert") X(BREAK, "break") X(CASE, "case")  \
    X(CATCH, "catch") X(CLASS, "class") X(CONST, "const") X(CONTINUE, "continue")    \
    X(DEFER, "defer") X(ELIF, "elif") X(ELSE, "else") X(EXPORT, "export")            \
    X(EXTENDS, "extends") X(FALSE, "false") X(FOR, "for") X(FROM, "from")            \
    X(FUNC, "func") X(IF, "if") X(IMPORT, "import") X(IN, "in") X(LET, "let")        \
    X(MATCH, "match") X(NIL, "nil") X(NOT, "not") X(OR, "or")                        \
    X(OVERRIDE, "override") X(PASS, "pass") X(PANIC, "panic") X(RETURN, "return")    \
    X(SUPER, "super") X(THROW, "throw") X(TRUE, "true") X(TRY, "try")                \
    X(WHILE, "while") X(YIELD, "yield")

typedef enum TokKind {
    TK_EOF, TK_NEWLINE, TK_INDENT, TK_DEDENT, TK_IDENT, TK_INT, TK_FLOAT, TK_STRING,
#define X(n, s) TK_KW_##n,
    KEYWORD_LIST(X)
#undef X
    TK_KW_END_,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_SLASHSLASH, TK_PERCENT, TK_STARSTAR,
    TK_DOTDOT, TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_EQEQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_ASSIGN, TK_PLUSEQ, TK_MINUSEQ, TK_STAREQ, TK_SLASHEQ, TK_SLASHSLASHEQ,
    TK_PERCENTEQ, TK_DOTDOTEQ,
    TK_LPAREN, TK_RPAREN, TK_LBRACK, TK_RBRACK, TK_LBRACE, TK_RBRACE,
    TK_COLON, TK_COMMA, TK_DOT, TK_ELLIPSIS,
    TK_QQ, TK_STARSTAREQ, TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_SHLEQ, TK_SHREQ, TK_FSTRING, TK_FATARROW, TK_PIPEGT,
    TK_QDOT, TK_QBRACK, TK_QQASSIGN
} TokKind;

typedef struct Token {
    int kind;
    int line, col;
    size_t offset;
    const char *s; /* lexeme start (points into source) */
    size_t len;
    int64_t ival;
    int big; /* INT literal equal to 2^63 (only valid under unary minus) */
    double fval;
    char *sval; /* decoded string value (TK_STRING), NUL-terminated */
    size_t slen;
} Token;

typedef struct TokenList {
    Token *v;
    int n, cap;
} TokenList;

int keyword_kind(const char *s, size_t len); /* TK_KW_* or -1 */
const char *tok_name(int kind);
int utf8_validate(const char *s, size_t n, size_t *bad);
/* Lex a whole (LF-normalized) source buffer. Longjmps through F on error. */
void lex_source(Front *F, const char *src, size_t len, TokenList *out);
void lex_source_at(Front *F, const char *src, size_t len, TokenList *out, int line, int col);
void toklist_free(TokenList *t);

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */
typedef struct Arena Arena;
Arena *arena_new(void);
void *arena_alloc(Arena *a, size_t n);
char *arena_strndup(Arena *a, const char *s, size_t n);
void arena_free(Arena *a);

typedef enum NodeKind {
    N_NIL, N_TRUE, N_FALSE, N_INT, N_FLOAT, N_STRING, N_NAME, N_ARRAY, N_TABLE,
    N_FUNC, N_UNARY, N_BINARY, N_AND, N_OR, N_COND, N_CALL, N_INDEX, N_MEMBER,
    N_SPREAD,
    S_EXPR, S_LET, S_CONST, S_ASSIGN, S_IF, S_WHILE, S_FOR, S_RETURN, S_BREAK,
    S_CONTINUE, S_PASS, S_BLOCK, S_FUNC, S_THROW, S_TRY, S_IMPORT, S_FROM,
    S_ASSERT, S_PANIC, S_MATCH, N_CASE, P_LIT, P_WILD, P_BIND, P_ARRAY, P_TABLE, P_ENTRY, S_DEFER, S_CLASS, N_SUPER_CALL, N_SUPER_METHOD, S_YIELD,
    N_COALESCE, S_MULTI, N_SEQ
} NodeKind;

typedef struct Decl {
    const char *name;
    size_t len;
    int captured;
    int is_const;
} Decl;

typedef struct Node Node;
struct Node {
    int kind, line, col;
    size_t offset;
    int op;
    Node *a, *b, *c;
    Node **list;
    int n;
    char *str;
    size_t slen;
    char *str2; /* alias / secondary name */
    int64_t ival;
    double fval;
    Decl *decl, *decl2;
    int flag; /* N_FUNC: 1 = expression body (lambda); N_CALL: 1 = trailing spread */
    int exported; /* S_LET/S_CONST/S_FUNC/S_CLASS preceded by `export` */
};

Node *parse_program(Front *F, Arena *A, const TokenList *toks);

/* ------------------------------------------------------------------ */
/* values & objects                                                    */
/* ------------------------------------------------------------------ */
typedef enum SpType {
    T_NIL = 0, T_BOOL = 1, T_INT = 2, T_FLOAT = 3, T_STRING = 4, T_ARRAY = 5,
    T_TABLE = 6, T_FUNCTION = 7, T_CLASS = 8, T_INSTANCE = 9, T_MODULE = 10,
    T_ITERATOR = 11, T_COROUTINE = 12, T_ERROR = 13, T_NATIVE = 14,
    T_CELL = 15,  /* internal */
    T_UNINIT = 16 /* internal: uninitialised module slot */
} SpType;

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
#define SP_NOINIT __attribute__((uninitialized))
#else
#define SP_NOINIT
#endif
typedef struct Obj Obj;
typedef struct SpValue {
    union {
        uint8_t type;
        uint64_t w; /* type in the low byte, rest zero: lets a value be built with two 8-byte stores */
    };
    union {
        int64_t i;
        double f;
        int b;
        Obj *o;
    } as;
} SpValue;

struct Obj {
    uint8_t type;
    uint8_t marked;
    uint32_t id;   /* identity number shown in "<function #N>" and mixed into hashes (wraps after 2^32 objects) */
    Obj *next;
};

typedef struct SString {
    Obj h;
    uint32_t len;
    uint32_t hash;
    char data[];
} SString;

typedef struct SArray {
    Obj h;
    int64_t len, cap;
    SpValue *items;
} SArray;

typedef struct TEntry {
    SpValue key, val; /* key.type == T_NIL means dead entry */
} TEntry;

typedef struct STable {
    Obj h;
    int64_t count; /* live entries */
    int64_t nent;  /* used entries incl. dead */
    int64_t cap;   /* entries capacity */
    TEntry *ents;
    int32_t *idx;
    int64_t idxcap; /* power of two or 0 */
    uint64_t version;
} STable;

typedef struct SCell {
    Obj h;
    SpValue v;
} SCell;

typedef struct VM VM;
struct SIterator;
/* Internal iterator class (SPEC 20.6). next() returns 0 = item written to out,
 * 1 = end, 2 = error pending. It never runs Sprint code. */
typedef struct IterClass {
    const char *kind;
    int arity; /* 1 or 2 */
    int (*next)(VM *vm, struct SIterator *it, SpValue out[2]);
} IterClass;
typedef struct SIterator {
    Obj h;
    const IterClass *cls;
    int done;
    SpValue held; /* GC-traced value owned by the iterator (nil if none) */
    int64_t a, b, c;
} SIterator;
struct SNativeObj;
typedef int (*NativeFn)(VM *vm, int argc, SpValue *argv, SpValue *ret);
typedef struct NativeDesc {
    const char *name;
    int min_args, max_args; /* max_args < 0: variadic */
    NativeFn fn;
} NativeDesc;

/* Host-resource objects (SPEC 20.7): opaque data block + method table. */
typedef struct NativeClass {
    const char *name;
    const NativeDesc *methods; /* NULL-terminated; argv[0] is the receiver */
    void (*release)(void *self); /* NULL ok; called once on GC/VM destruction; must not touch the VM */
    void (*trace)(VM *vm, struct SNativeObj *self); /* NULL ok; marks held values via gc_mark */
} NativeClass;
typedef struct SNativeObj {
    Obj h;
    const NativeClass *cls;
    size_t size;
    void *data;
} SNativeObj;
#define AS_NATIVE(v) ((SNativeObj *)(v).as.o)

typedef struct Proto Proto;
typedef struct SFunc {
    Obj h;
    Proto *proto; /* NULL for native */
    const NativeDesc *nat;
    int nup;
    SCell **up;
} SFunc;

typedef struct SModule {
    Obj h;
    SString *name;
    STable *exports;
} SModule;

typedef struct SClass {
    Obj h;
    SString *name;
    struct SClass *parent;
    STable *fields;  /* name -> slot index (int), inherited fields first */
    int nfields;
    STable *methods; /* name -> function, inherited methods included */
    SFunc *ctor;     /* compiler-generated constructor (SPEC 10.3) */
    /* runtime-only construction fast path (derived from ctor's bytecode on first call) */
    int fast;        /* 0 = not analysed, 1 = usable, 2 = not applicable */
    SFunc *init;     /* the init method run by the fast path (NULL = none) */
    SpValue *dflt;   /* initial slot values (constant field initialisers); NULL = all nil */
} SClass;

typedef struct SInstance {
    Obj h;
    SClass *cls;
    int nslots;
    SpValue slots[];
} SInstance;

typedef struct SError {
    Obj h;
    SString *kind;
    int code;
    SString *message;
    SpValue cause, data, suppressed;
    SString *file;
    int line, col;
    SString *trace;
} SError;

#define V_NIL ((SpValue){T_NIL, {0}, {0}})
static inline SpValue v_nil(void) { SpValue v; v.w = T_NIL; v.as.i = 0; return v; }
static inline SpValue v_bool(int b) { SpValue v; v.w = T_BOOL; v.as.i = b != 0; return v; }
static inline SpValue v_int(int64_t i) { SpValue v; v.w = T_INT; v.as.i = i; return v; }
static inline SpValue v_float(double f) { SpValue v; v.w = T_FLOAT; v.as.i = 0; v.as.f = f; return v; }
static inline SpValue v_obj(int type, void *o) { SpValue v; v.w = (uint8_t)type; v.as.o = (Obj *)o; return v; }
static inline SpValue v_uninit(void) { SpValue v; v.w = T_UNINIT; v.as.i = 0; return v; }

#define IS_NIL(v) ((v).type == T_NIL)
#define IS_INT(v) ((v).type == T_INT)
#define IS_FLOAT(v) ((v).type == T_FLOAT)
#define IS_NUM(v) ((v).type == T_INT || (v).type == T_FLOAT)
#define IS_STR(v) ((v).type == T_STRING)
#define IS_ARRAY(v) ((v).type == T_ARRAY)
#define IS_TABLE(v) ((v).type == T_TABLE)
#define AS_STR(v) ((SString *)(v).as.o)
#define AS_ARRAY(v) ((SArray *)(v).as.o)
#define AS_TABLE(v) ((STable *)(v).as.o)
#define AS_FUNC(v) ((SFunc *)(v).as.o)
#define AS_MODULE(v) ((SModule *)(v).as.o)
#define AS_CLASS(v) ((SClass *)(v).as.o)
#define AS_CO(v) ((SCoroutine *)(v).as.o)
#define AS_INSTANCE(v) ((SInstance *)(v).as.o)
#define AS_ERROR(v) ((SError *)(v).as.o)
#define AS_ITER(v) ((SIterator *)(v).as.o)
#define AS_CELL(v) ((SCell *)(v).as.o)
static inline int sp_truthy(SpValue v) { return !(v.type == T_NIL || (v.type == T_BOOL && !v.as.b)); }
#define TRUTHY(v) sp_truthy(v)

/* ------------------------------------------------------------------ */
/* program / bytecode                                                  */
/* ------------------------------------------------------------------ */
typedef struct LineEnt {
    uint32_t pc;
    int line, col;
} LineEnt;

typedef struct UpDesc {
    uint8_t from_local;
    uint16_t index;
} UpDesc;

struct Proto {
    char *name;
    int nparams, nrequired, has_rest;
    int nlocals, max_stack;
    int nupvals;
    UpDesc *upvals;
    SpValue *consts;
    int nconsts, capconsts;
    int ndefaults;
    uint32_t *default_pcs; /* ndefaults + 1 entries */
    uint8_t *code;
    uint32_t code_len, code_cap;
    LineEnt *lines;
    int nlines, caplines;
    int is_module;
    int mod; /* index into Program.mods (selects the file name for traces) */
    uint8_t *ocode;        /* pristine copy of code from before quickening (the JIT compiles from it); NULL = code is pristine */
    struct JitProto *jit;  /* compiled loop regions (JIT) */
    int jhot;              /* back-edge countdown until the JIT is consulted (0 = not initialised) */
    struct JitUnit *junit; /* native entry of the whole function (JIT), NULL = not compiled */
    int jcall;             /* call countdown until the JIT is asked to compile the function (0 = not initialised) */
};

enum { LINK_PRELUDE = 0, LINK_MOD_EXPORT = 1, LINK_MOD_VALUE = 2 };
typedef struct Link {
    int kind;
    char *module;
    char *member;
} Link;

typedef struct ClassInfo {
    char *name;
    int nmethods;
    char **method_names;
    int nfields; /* own fields */
    char **field_names;
    int ctor_proto;
} ClassInfo;

typedef struct ExportEnt {
    char *name;
    int slot; /* global module-variable slot */
} ExportEnt;

typedef struct ModInfo {
    char *name;
    char file[256]; /* path used in traces */
    char rel[256];  /* search-root-relative path stored in images */
    uint8_t sha[32]; /* SHA-256 of the normalised source (computed on demand when lazy_src is set) */
    const char *lazy_src; /* bundled module: its source is static, so hashing is deferred to image writing */
    size_t lazy_len;
    int entry_proto;
    ExportEnt *exports;
    int nexports;
} ModInfo;

typedef struct Program {
    ModInfo *mods;
    int nmods, capmods;
    ClassInfo *classes;
    int nclasses, capclasses;
    Proto **protos;
    int nprotos, capprotos;
    Link *links;
    int nlinks, caplinks;
    int nslots;
    char **slot_names;
    void **fcls; /* compiler: exported classes other modules may extend */
    int nfcls, capfcls;
    char file[256];
    const VM *hostvm; /* embedding: source of registered host modules (may be NULL) */
} Program;

enum Op {
    OP_NOP = 0x00, OP_CONST = 0x01, OP_NIL = 0x02, OP_TRUE = 0x03, OP_FALSE = 0x04,
    OP_SMALLINT = 0x05, OP_POP = 0x06, OP_DUP = 0x07, OP_DUP2 = 0x08,
    OP_LOAD_LOCAL = 0x10, OP_STORE_LOCAL = 0x11, OP_MAKE_CELL = 0x12,
    OP_LOAD_CELL = 0x13, OP_STORE_CELL = 0x14, OP_LOAD_UPVAL = 0x15,
    OP_STORE_UPVAL = 0x16, OP_LOAD_MODULE = 0x17, OP_STORE_MODULE = 0x18,
    OP_LOAD_LINK = 0x19,
    OP_ADD = 0x20, OP_SUB = 0x21, OP_MUL = 0x22, OP_DIV = 0x23, OP_FLOOR_DIV = 0x24,
    OP_MOD = 0x25, OP_POW = 0x26, OP_NEG = 0x27, OP_BAND = 0x28, OP_BOR = 0x29,
    OP_BXOR = 0x2A, OP_BNOT = 0x2B, OP_SHL = 0x2C, OP_SHR = 0x2D, OP_NOT = 0x2E,
    OP_CONCAT = 0x2F,
    OP_EQ = 0x30, OP_NE = 0x31, OP_LT = 0x32, OP_LE = 0x33, OP_GT = 0x34, OP_GE = 0x35,
    OP_JUMP = 0x38, OP_JUMP_IF_FALSE = 0x39, OP_JUMP_IF_TRUE = 0x3A,
    OP_JUMP_IF_FALSE_OR_POP = 0x3B, OP_JUMP_IF_TRUE_OR_POP = 0x3C,
    OP_GET_INDEX = 0x40, OP_SET_INDEX = 0x41, OP_GET_MEMBER = 0x42,
    OP_SET_MEMBER = 0x43, OP_NEW_ARRAY = 0x44, OP_NEW_TABLE = 0x45,
    OP_CLOSURE = 0x46, OP_TEST_ARRAY = 0x48, OP_TEST_HAS = 0x49,
    OP_NEW_INSTANCE = 0x4A, OP_CLASS = 0x4B, OP_SET_SLOT = 0x4C, OP_CTOR_CHECK = 0x4D,
    OP_CALL = 0x50, OP_CALL_SPREAD = 0x51, OP_INVOKE = 0x52, OP_INVOKE_SPREAD = 0x53,
    OP_RETURN = 0x56,
    OP_RETURN_NIL = 0x57,
    OP_FOR_PREP = 0x60, OP_FOR_NEXT = 0x61, OP_RANGE_PREP = 0x62, OP_RANGE_NEXT = 0x63,
    OP_THROW = 0x68, OP_TRY_BEGIN = 0x69, OP_TRY_END = 0x6A,
    OP_PANIC = 0x70, OP_ASSERT_FAIL = 0x71, OP_MATCH_FAIL = 0x72,
    OP_DEFER_SCOPE = 0x74, OP_RUN_DEFERS = 0x75, OP_DEFER = 0x76, OP_DEFER_INVOKE = 0x77,
    OP_RAISE_ERR = 0x78, OP_YIELD = 0x79,
    /* Run-time-only fused opcodes (0x80..0xA0). Never present in sources or
     * images; prog_quicken() overwrites only the FIRST byte of a sequence, so
     * lengths, offsets, line tables and the following original instructions
     * stay intact (a jump into the middle of a sequence still works). */
    OP_Q_FIRST = 0x80,
    OP_Q_LLI_ADD = 0x80, OP_Q_LLI_SUB, OP_Q_LLI_MOD, OP_Q_LLI_LT, OP_Q_LLI_LE, OP_Q_LLI_GT,
    OP_Q_LLI_GE, OP_Q_LLI_EQ, OP_Q_LLI_NE,
    OP_Q_LLI_LT_JF, OP_Q_LLI_LE_JF, OP_Q_LLI_GT_JF, OP_Q_LLI_GE_JF, OP_Q_LLI_EQ_JF, OP_Q_LLI_NE_JF,
    OP_Q_LLL_ADD = 0x90, OP_Q_LLL_SUB, OP_Q_LLL_MUL, OP_Q_LLL_LT, OP_Q_LLL_LE, OP_Q_LLL_GT,
    OP_Q_LLL_GE, OP_Q_LLL_EQ, OP_Q_LLL_NE,
    OP_Q_LLL_LT_JF, OP_Q_LLL_LE_JF, OP_Q_LLL_GT_JF, OP_Q_LLL_GE_JF, OP_Q_LLL_EQ_JF, OP_Q_LLL_NE_JF,
    OP_Q_LLI_MUL = 0x8F,
    OP_Q_RANGE_NEXT_ST = 0xA0,
    OP_Q_SI_ADD = 0xA1, OP_Q_SI_SUB, OP_Q_SI_MUL,
    OP_Q_LLI_ADD_ST = 0xA4, OP_Q_LLI_SUB_ST,
    OP_Q_ADD_ST = 0xA6, OP_Q_SUB_ST,
    OP_Q_ADD_STM = 0xA8, OP_Q_SUB_STM,
    OP_Q_JUMP_RN = 0xAA, OP_Q_JUMP_LLL_LT, OP_Q_JUMP_LLI_LT,
    OP_Q_RET_LOCAL = 0xAD, OP_Q_LOCAL_GETM, OP_Q_LL_IDX,
    OP_Q_ACC_I = 0xB0, OP_Q_ACC_L, OP_Q_ACC_IM, OP_Q_ACC_LM, OP_Q_LLL_DIV, OP_Q_SETIDX_LLL, OP_Q_SETIDX_LLB, OP_Q_MUL_ST,
    OP_Q_SI_LT_JF, OP_Q_SI_LE_JF, OP_Q_SI_GT_JF, OP_Q_SI_GE_JF, OP_Q_SI_EQ_JF, OP_Q_SI_NE_JF,
    OP_Q_LLI_FDIV, OP_Q_IL_ADD, OP_Q_IL_MUL, OP_Q_IDX_L, OP_Q_UP_INC, OP_Q_RET_UP,
    OP_Q_ADD_L, OP_Q_SUB_L, OP_Q_MUL_L, OP_Q_DIV_L, OP_Q_MODEQ_JF, OP_Q_MODNE_JF, OP_Q_JUMP_JF
};

/* ------------------------------------------------------------------ */
/* VM                                                                  */
/* ------------------------------------------------------------------ */
typedef struct Limits {
    uint64_t max_heap_bytes;
    uint32_t max_call_depth;
    uint32_t max_operand_stack;
    uint32_t max_handlers;
    uint32_t max_trace_frames;
    uint32_t stdout_buffer_bytes;
    uint64_t max_string_bytes;
    uint64_t max_array_length;
    uint64_t max_table_entries;
    uint64_t max_instructions;
    uint32_t max_defer_records;
    uint32_t max_defer_values;
} Limits;

enum { CAP_STDIO_READ = 1, CAP_STDIO_WRITE = 2, CAP_FS_READ = 4, CAP_FS_WRITE = 8,
       CAP_CLOCK = 16, CAP_RANDOM = 32, CAP_FFI = 64 };

enum { FK_NORMAL = 0, FK_DEFER_RUN = 1, FK_DEFER_UNWIND = 2, FK_BOUNDARY = 3, FK_CTOR = 4 /* init run by the construction fast path; uev holds the new instance */ };

typedef struct Frame {
    SFunc *fn;
    const uint8_t *pc;
    SpValue *base;
    int kind;     /* FK_* */
    int dbase;    /* defer record depth at entry */
    SpValue uev;  /* FK_DEFER_UNWIND: the error being propagated */
} Frame;

typedef struct Handler {
    int frame;
    const uint8_t *pc;
    SpValue *sp;
    int ddepth;   /* defer record depth at TRY_BEGIN */
} Handler;

typedef struct DeferRec {
    int nvals;            /* callee/receiver + arguments in dvals */
    SString *method;      /* non-NULL: builtin method call on dvals[0] */
} DeferRec;

/* One execution context: the main program or a coroutine (SPEC 13.3). */
typedef struct ExecState {
    SpValue *stack, *stack_end, *sp;
    Frame *frames;
    int nframes, max_frames;
    Handler *handlers;
    int nh, hcap;
    DeferRec *drecs;
    int ndr, drcap;
    SpValue *dvals;
    int ndv, dvcap;
} ExecState;

enum { CO_SUSPENDED = 0, CO_RUNNING = 1, CO_NORMAL = 2, CO_DEAD = 3 };

typedef struct SCoroutine {
    Obj h;
    SFunc *fn;
    int status;
    int started;
    int closing;     /* resumed only to unwind its frames (coroutine.close) */
    int resume_for;  /* resumed by a `for` loop: finishing jumps to for_exit */
    const uint8_t *for_exit;
    ExecState st;      /* saved while suspended */
    ExecState caller;  /* the resumer's state while this coroutine runs */
    struct SCoroutine *resumer;
    void *block;
    size_t block_size;
} SCoroutine;

enum { SW_NONE = 0, SW_RESUME = 1, SW_CLOSE = 2 };

typedef int RunStatusInt;
typedef enum RunStatus {
    RS_OK = 0, RS_ERROR = 1, RS_PERM = 3, RS_MEMORY = 14, RS_PANIC = 101,
    RS_TIMEOUT = 124, RS_INTERRUPT = 130, RS_EXIT = 256
} RunStatus;

/* Direct-mapped (class, member name) -> field slot / method cache. Classes are
 * immutable after creation; the cache is cleared at every GC so a recycled
 * address can never produce a stale hit. */
typedef struct ICEnt {
    const SClass *cls;
    const SString *name;
    int kind; /* 0 empty, 1 field, 2 method */
    int64_t slot;
    SpValue fn;
} ICEnt;
enum { IC_SIZE = 256 };

/* Direct-mapped (table or module exports, member name) -> entry index cache; cleared at every GC. A hit also
 * needs the entry's key to still be the very string object it was found under, so rebuilds and deletions miss. */
typedef struct MCEnt {
    const STable *t;
    const SString *name;
    const SString *ks;
    int64_t idx;
} MCEnt;
enum { MC_SIZE = 256 };

/* objects up to this many bytes come from the per-VM size-class pools (value.c, gc.c) */
#define SP_POOL_MAX 256
struct VM {
    ICEnt ic[IC_SIZE];
    int no_quicken; /* --no-quicken: run the plain bytecode (differential testing) */
    int jit_on, jit_thresh, jit_dbg; /* copy-and-patch JIT (src/runtime/jit.c): enabled, hot threshold, trace to stderr */
    int gc_stress;                   /* developer knob (SPRINT_GC_STRESS=1): collect as often as possible */
    void *jit_cur;                   /* JIT: region whose exit stub ran last */
    void *jit_rsp;                   /* JIT: native stack pointer to unwind to when compiled code exits */
    int jit_fmax;                    /* JIT: frame count compiled code must not call deeper than */
    void *(*jit_new)(VM *, SClass *); /* JIT: allocates an instance (may collect garbage) */
    Obj *objs;
    void *pool_free[17]; /* size-class free lists for small objects (16-byte classes) */
    char *pool_cur, *pool_end;
    void **pool_chunks;
    int npool_chunks, cappool_chunks;
    size_t bytes, gc_next;
    uint32_t next_id;
    int gc_runs;
    Limits lim;
    int caps;
    int deterministic;
    char **allow_read, **allow_write; /* canonical permitted directories (fs) */
    int n_allow_read, n_allow_write;

    SpValue *stack, *stack_end, *sp;
    Frame *frames;
    int nframes, max_frames;
    Handler *handlers;
    int nh;
    DeferRec *drecs;
    int ndr;
    SpValue *dvals;
    int ndv;
    int hcap, drcap, dvcap; /* capacities of the current execution context */
    SCoroutine *cur_co;     /* NULL while the main program runs */
    ExecState main_exec;    /* the main program's context (fixed arrays) */
    int native_depth;       /* nested run_loop calls made by natives (sp_call) */
    SpValue bret;           /* result of a returning boundary frame */
    int sw_kind;            /* pending context switch requested by a native */
    SCoroutine *sw_co;

    Program *prog;
    SpValue *slots;
    SpValue *links;
    int slots_have, links_have, mods_run, protos_quick; /* incremental (REPL) execution progress */

    /* direct-mapped cache of builtin method lookups, keyed by (method-name constant, receiver kind) */
    uint32_t h_append; /* hash of "append" (inline fast path for array.append) */
    struct MCacheEnt {
        const SString *name;
        const NativeClass *ncls;
        const NativeDesc *d;
        int type;
    } mcache[256];

    /* pending runtime error (set by helpers, materialised at raise) */
    int pend_code;
    char pend_msg[256];
    SpValue pend_obj;
    SpValue uncaught;
    int exit_code;

    char *outbuf;
    size_t outcap, outlen;

    jmp_buf fatal_jb;
    int fatal_armed;
    int fatal_status;
    RunStatusInt result;
    uint64_t ileft; /* remaining ticks of the current chunk */
    uint64_t ibank; /* budget not yet handed out (UINT64_MAX = unlimited) */
    char fatal_msg[256];

    SString *type_names[17];
    SpValue *roots; /* permanent roots (prelude, modules) */
    int nroots, caproots;

    Obj **gray;
    size_t ngray, capgray;

    int prog_argc;
    char **prog_argv;
    const char *file;
    uint64_t rng[4];
    int rng_seeded;
    double vclock;

    /* ---- embedding (SPEC 20); all NULL/0 for the command-line tool ---- */
    size_t (*hwrite)(void *ud, int stream, const char *data, size_t len);
    ptrdiff_t (*hread)(void *ud, char *buf, size_t cap);
    void *hio_ud;
    int (*hloader)(void *ud, const char *name, const char **src, size_t *len, const char **disp);
    void *hloader_ud;
    struct HostMod **hmods;     /* modules registered with sp_register_module (each separately allocated: stable addresses) */
    int nhmods;
    SpValue *nroot_stack;       /* values created by the running native call (GC roots) */
    int nnroot, capnroot, maxnroot;
    struct HostRef *hrefs;      /* sp_ref roots */
    unsigned hflags;            /* SP_NATIVE_* flags of the running host native */
    int in_host;                /* inside a host native function */
    int in_iter;                /* inside a host iterator's next() */
    volatile int interrupted;   /* set asynchronously by sp_interrupt */
    struct HostDiag *hdiag;     /* diagnostics of the last failed compile */
    int nhdiag;
    void *hcfg;                 /* embed.c private configuration */
    int hrun;                   /* an embedded program is running */
    int max_reentry;            /* max nested sp_call depth */
    double deadline_s;          /* wall-clock deadline (0 = none), see max_runtime_ms */
    MCEnt mc[MC_SIZE]; /* last, so the hot fields above keep their small offsets */
};


/* memory */
void *vm_mem(VM *vm, void *p, size_t old, size_t nw);
VM *vm_new(const Limits *lim, int caps);
void vm_free(VM *vm);
void limits_default(Limits *l);

/* objects */
SString *vm_new_string(VM *vm, const char *s, size_t len);
SString *vm_alloc_string(VM *vm, size_t len); /* uninitialised data, hash unset */
void string_finish(SString *s);
SArray *vm_new_array(VM *vm, int64_t cap);
int array_push(VM *vm, SArray *a, SpValue v);
int array_reserve(VM *vm, SArray *a, int64_t need);
SFunc *vm_new_closure_obj(VM *vm, Proto *p, int nup);
SError *vm_new_error_obj(VM *vm);
STable *vm_new_table(VM *vm, int64_t cap);
SCell *vm_new_cell(VM *vm, SpValue v);
SFunc *vm_new_native(VM *vm, const NativeDesc *d);
SModule *vm_new_module(VM *vm, const char *name);
SClass *vm_new_class(VM *vm, SString *name, SClass *parent);
SInstance *vm_new_instance(VM *vm, SClass *cls);
SIterator *vm_new_iterator(VM *vm, const IterClass *cls, SpValue held, int64_t a, int64_t b, int64_t c);
SCoroutine *vm_new_coroutine(VM *vm, SFunc *fn, int64_t stack_slots);
void vm_co_release(VM *vm, SCoroutine *co);
void exec_save(const VM *vm, ExecState *st);
void exec_load(VM *vm, const ExecState *st);
void vm_add_root(VM *vm, SpValue v);
SpValue vm_perm_string(VM *vm, const char *s, size_t len);

int table_get(VM *vm, STable *t, SpValue key, SpValue *out, int *found);
SString *vm_concat_fast(VM *vm, const SpValue *ops, unsigned n); /* strings and ints only, NULL otherwise */
int table_set(VM *vm, STable *t, SpValue key, SpValue val); /* nil val deletes */
int table_get_str(STable *t, const char *s, size_t len, SpValue *out);
SNativeObj *vm_new_native_obj(VM *vm, const NativeClass *cls, size_t size);
int lib_fs_check(VM *vm, SpValue pv, int write, const char *op, char *out);
int lib_io_fail(VM *vm, int err, const char *op, const char *path);
extern const NativeDesc lib_gfx_fns[];
const uint32_t *gfx_canvas_pixels(SpValue v, int *w, int *h); /* host access to a canvas (the OS kernel) */
extern const NativeDesc lib_ffi_fns[];
uint32_t sp_hash_bytes(const char *s, size_t n);
uint32_t str_hash_calc(const char *s, size_t n); /* never 0 */
uint32_t str_hash_slow(SString *s);
/* A long string's hash is computed on first use (0 = not yet), so building big strings costs no hashing. */
static inline uint32_t str_hash_peek(const SString *s) { return s->hash ? s->hash : str_hash_calc(s->data, s->len); }
static inline uint32_t str_hash(SString *s) { return s->hash ? s->hash : str_hash_slow(s); }
size_t sp_fmt_int(char *buf, int64_t v);
extern const NativeDesc math_fns[];
extern const int math_fns_count;
int math_fast1(const NativeDesc *d, double x, double *out);
int math_pure_native(const NativeDesc *d);
int table_get_sstr(const STable *t, const SString *key, SpValue *out);
int64_t table_find_sstr(const STable *t, const SString *key); /* entry index or -1 */
void table_set_str(VM *vm, STable *t, const char *s, size_t len, SpValue v);
int table_reserve(VM *vm, STable *t, int64_t n);
int64_t table_next(STable *t, int64_t cursor, SpValue *k, SpValue *v);

/* semantics */
int value_eq(SpValue a, SpValue b);
int num_cmp(SpValue a, SpValue b, int *unordered); /* -1/0/1 */
const char *type_name(SpValue v);
void fmt_float(double d, char *buf);
typedef struct Out {
    char *dst;
    size_t cap, len;
    void (*sink)(void *ud, const char *s, size_t n);
    void *ud;
} Out;
void out_put(Out *o, const char *s, size_t n);
void fmt_value(VM *vm, SpValue v, int depth, int repr, Out *o);
SString *vm_to_string(VM *vm, SpValue v);
void vm_out_write(VM *vm, const char *s, size_t n);
void vm_out_flush(VM *vm);

/* errors */
int vm_set_error(VM *vm, int code, const char *fmt, ...) SP_PRINTF(3, 4);
SP_NORETURN void vm_fatal(VM *vm, int status, const char *fmt, ...) SP_PRINTF(3, 4);
SError *vm_new_error(VM *vm, int code, const char *kind, size_t klen, const char *msg,
                     size_t mlen);
SError *vm_make_error(VM *vm, int code, SString *kind, SString *msg);
const uint8_t *vm_error_ipc(const VM *vm);

/* gc */
void vm_gc(VM *vm);
void gc_mark(VM *vm, SpValue v); /* for NativeClass.trace */
#define GC_POINT(vm) do { if ((vm)->bytes >= (vm)->gc_next) vm_gc(vm); } while (0)

/* running */
RunStatus vm_run_program(VM *vm, Program *prog, int argc, char **argv);
int vm_call(VM *vm, SpValue fn, int argc, const SpValue *argv, SpValue *ret); /* 0 ok, 1 error pending */
RunStatus vm_run_chunk(VM *vm, Program *prog); /* REPL: run the modules added since the last call */
void vm_print_uncaught(VM *vm, int use_json);

/* ------------------------------------------------------------------ */
/* compiler                                                            */
/* ------------------------------------------------------------------ */
Program *program_new(void);
/* Compile one module (dependencies must already be in `prog`); longjmps through F on error. */
typedef struct ReplScope ReplScope; /* REPL: names/classes carried across input chunks */
ReplScope *replscope_new(void);
void replscope_free(ReplScope *r);
int replscope_slot(const ReplScope *r, const char *name);
int replscope_count(const ReplScope *r);
const char *replscope_name(const ReplScope *r, int i); /* NULL for slots that belong to imported modules */
size_t replscope_name_len(const ReplScope *r, int i);
int replscope_is_import(const ReplScope *r, int i);
void replscope_echo(ReplScope *r, int want);
int replscope_echoed(const ReplScope *r);
int replscope_wants_echo(const ReplScope *r);
void replscope_set_echoed(ReplScope *r);
void compile_module_scoped(Front *F, VM *vm, Program *prog, struct Node *root, const char *modname, const char *file,
                           ReplScope *rs);
void compile_module(Front *F, VM *vm, Program *prog, Node *root, const char *modname, const char *file);
void program_free(Program *p);
/* program images (.spc) and verifier */
void sp_sha256(const void *data, size_t len, uint8_t out[32]);
void sp_source_hash(const Program *pg, uint8_t out[32]);
uint8_t *image_encode(const Program *pg, size_t *out_len);
Program *image_decode(VM *vm, const uint8_t *buf, size_t len, char *err, size_t errsz);
void prog_quicken(Program *pg);

/* copy-and-patch loop JIT (x86-64 Linux only; elsewhere these are no-ops) */
#if defined(__x86_64__) && defined(__linux__) && !defined(SP_NO_JIT) && !defined(SP_OS) && !defined(SP_OS_USER)
#define SP_JIT 1
#else
#define SP_JIT 0
#endif
/* A function compiled as a whole: compiled callers call `entry` directly (r12 = base, r13 = kt). The layout is shared
 * with src/runtime/jit/stencils.S through jit/layout.h. */
typedef struct JitUnit {
    void *entry;
    SpValue *kt;
    const uint8_t *code;
    int32_t nparams, nlocals;
    int64_t need;          /* bytes of stack the callee needs above its base */
    int (*tramp)(VM *, SpValue *, const SpValue *, struct Frame *); /* entry from the interpreter */
    void *owner;           /* the JIT region that holds the code */
    Proto *proto;
} JitUnit;
/* Enter compiled code for the loop at HDR; 1 = it ran and left the interpreter state in the VM (top frame pc, vm->sp,
 * vm->ileft), 0 = nothing happened. */
int jit_site(VM *vm, Proto *p, const uint8_t *hdr, SpValue *base, SpValue *sp);
/* Called from OP_CALL: after the call countdown ran out. Returns the unit to enter, or NULL. */
JitUnit *jit_call_hot(VM *vm, SFunc *fn, SpValue *base);
/* Run a unit whose frame is already pushed (nf = top frame, base = its base); state as for jit_site. */
void jit_run(VM *vm, JitUnit *u, SpValue *base);
void jit_free_proto(Proto *p);
void jit_roots(Proto *p, void (*mark)(VM *, SpValue), VM *vm); /* GC: objects compiled code refers to */
void jit_configure(VM *vm);
int verify_program(const Program *pg, char *errbuf, size_t errsz);
Program *load_program(Front *F, VM *vm, const char *path, char *src, size_t len);
void load_chunk(Front *F, VM *vm, Program *prog, ReplScope *rs, Arena **keep, char *src, size_t len, const char *name);
Program *load_program_host(Front *F, VM *vm, const char *disp_path, char *src, size_t len);
char *sp_read_file(const char *path, size_t *len);
size_t sp_normalize(char *b, size_t n);
void proto_disasm(const Proto *p, FILE *out);
int op_len(int op);

/* ------------------------------------------------------------------ */
/* stdlib                                                              */
/* ------------------------------------------------------------------ */
extern const NativeDesc lib_json_fns[], lib_random_fns[], lib_fs_fns[], lib_typed_fns[];
/* typed arrays (lib_typed.c): native objects holding unboxed numbers */
/* element kinds of a typed array; the JIT reads the element straight from the payload (layout: TypedHdr, then the elements) */
typedef enum TKind { TK_I8, TK_U8, TK_I16, TK_U16, TK_I32, TK_U32, TK_I64, TK_F32, TK_F64, TK_COUNT } TKind;
#define TYPED_DATA_OFF 16 /* offset of element 0 in SNativeObj.data; the length (int64) is at offset 0 */
extern const NativeClass typed_classes[TK_COUNT];
static inline const NativeClass *typed_class(int kind) { return &typed_classes[kind]; }
static inline int typed_is(SpValue v) {
    return v.type == T_NATIVE && (uintptr_t)AS_NATIVE(v)->cls - (uintptr_t)typed_classes < TK_COUNT * sizeof(NativeClass);
}
int64_t typed_len(SpValue v);
int typed_kind(SpValue v);
int typed_peek(SpValue o, SpValue k, SpValue *out);
int typed_poke(SpValue o, SpValue k, SpValue v);
int typed_get(VM *vm, SpValue o, SpValue k, SpValue *out);
int typed_set(VM *vm, SpValue o, SpValue k, SpValue v);
int lib_allow_dir(VM *vm, int write, const char *dir);
void lib_ext_free(VM *vm);

typedef struct ModConst {
    const char *name;
    int kind; /* 0 int, 1 float */
    int64_t i;
    double f;
} ModConst;
typedef struct ModDesc {
    const char *name;
    const NativeDesc *fns;
    const ModConst *consts;
} ModDesc;

/* A module registered by the host (SPEC 20.5); `md.fns` is NULL-name terminated. */
typedef struct HostMod {
    char *name;
    ModDesc md;
    ModConst none;    /* empty const list terminator */
    NativeDesc *fns;  /* n entries + a terminator */
    void *hd;         /* parallel array of the host's SpNativeDesc */
    int n;
} HostMod;
typedef struct HostRef {
    struct HostRef *next, *prev;
    SpValue v;
} HostRef;
int lib_prelude_index(const char *name, size_t len);
const NativeDesc *lib_prelude_desc(int idx);
const ModDesc *lib_find_module(const VM *vm, const char *name, size_t len);

/* Sprint-source modules compiled into the binary (generated from stdlib). */
typedef struct BundledMod { const char *name; const char *src; size_t len; } BundledMod;
extern const BundledMod bundled_mods[]; /* terminated by a {0,0,0} entry */
const BundledMod *bundled_find(const char *name, size_t len);
int lib_module_has(const ModDesc *m, const char *member, size_t len);
void lib_init(VM *vm);
SpValue lib_prelude_value(VM *vm, int idx);
SpValue lib_module_value(VM *vm, const char *name);
SpValue lib_module_export(VM *vm, const char *module, const char *member, int *found);
int lib_invoke_builtin_method(VM *vm, SpValue recv, const char *name, size_t len, uint32_t hash, int argc,
                              SpValue *argv, SpValue *ret, int *found);
int sp_invoke_method_exists(int type, const char *name, size_t len);
/* same as lib_invoke_builtin_method, but the method name is a constant string and the lookup is cached */
int lib_invoke_builtin_method_s(VM *vm, SpValue recv, const SString *name, int argc, SpValue *argv, SpValue *ret, int *found);

#endif
