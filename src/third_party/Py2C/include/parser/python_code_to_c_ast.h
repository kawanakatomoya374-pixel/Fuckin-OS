/* --fallback で未対応構文の代わりに parser が作る名前ノードの接頭辞。
 * この接頭辞で始まる AST_NAME は codegen がランタイムの
 * p2c_fallback_expr() 呼び出し（実行時に NotImplementedError）へ変換する。 */
#define P2C_UNSUPPORTED_NAME_PREFIX "_p2c_unsupported_"

#ifndef PYTHON_CODE_TO_C_AST_H
#define PYTHON_CODE_TO_C_AST_H

#include "lexer/python_code_to_c_lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * ASTノード種別
 * ======================================== */

typedef enum {
    /* 式（Expressions） */
    AST_BINOP,              /* 二項演算 */
    AST_UNARYOP,            /* 単項演算 */
    AST_COMPARE,            /* 比較演算 */
    AST_BOOLOP,             /* 論理演算（and/or） */
    AST_CALL,               /* 関数呼び出し */
    AST_ATTRIBUTE,          /* 属性アクセス（obj.attr） */
    AST_SUBSCRIPT,          /* 添字アクセス（obj[idx]） */
    AST_NAME,               /* 変数名 */
    AST_CONST,              /* 定数リテラル */
    AST_IFEXP,              /* 条件式（x if c else y） */
    AST_LAMBDA,             /* lambda式 */
    AST_LIST,               /* リストリテラル */
    AST_TUPLE,              /* タプルリテラル */
        AST_DICT,              /* 辞書リテラル */
    AST_SET,               /* setリテラル */
    AST_COMPREHENSION,      /* 内包表記 */
    AST_STARRED,            /* *args */
    AST_NAMED_EXPR,         /* 代入式 (name := value) */
    AST_YIELD,              /* yield [value] */
    AST_AWAIT,              /* await value */
    AST_GENERATOR_EXPRESSION, /* (expression for target in iterable ...) */

    /* 文（Statements） */
    AST_ASSIGN,             /* 代入 */
    AST_AUGASSIGN,          /* 複合代入（+=等） */
    AST_ANNASSIGN,          /* 型注釈付き代入 */
    AST_RETURN,             /* return */
    AST_EXPR_STMT,          /* 式文 */
    AST_PASS,               /* pass */
    AST_BREAK,              /* break */
    AST_CONTINUE,           /* continue */
    AST_BLOCK,              /* セミコロン区切りの複数文 (simple_stmt内部用) */

    /* 制御構造 */
    AST_IF,                 /* if文 */
    AST_WHILE,              /* while文 */
    AST_FOR,                /* for文 */
    AST_ASYNC_FOR,          /* async for文 */
    AST_TRY,                /* try文 */
    AST_RAISE,              /* raise */
    AST_ASSERT,             /* assert */
    AST_MATCH,              /* match/case */

    /* 関数・クラス定義 */
    AST_FUNCTIONDEF,        /* 関数定義 */
    AST_CLASSDEF,           /* クラス定義 */
    AST_GLOBAL,             /* global */
    AST_NONLOCAL,           /* nonlocal */

    /* モジュール */
    AST_MODULE,             /* モジュール（ルートノード） */
    AST_IMPORT,             /* import */
    AST_IMPORTFROM,         /* from ... import */

    /* その他 */
    AST_WITH,               /* with文 */
    AST_DELETE,             /* del */
} P2C_AstType;

/* 前方宣言 */
typedef struct P2C_AstNode P2C_AstNode;
typedef struct P2C_AstExpr P2C_AstExpr;
typedef struct P2C_AstStmt P2C_AstStmt;

/* ========================================
 * 演算子型
 * ======================================== */

typedef enum {
    OP_ADD, OP_SUB, OP_MULT, OP_DIV, OP_FLOORDIV, OP_MOD, OP_POW,
    OP_LSHIFT, OP_RSHIFT, OP_BITAND, OP_BITOR, OP_BITXOR,
    OP_LT, OP_LE, OP_EQ, OP_NE, OP_GT, OP_GE,
    OP_IS, OP_ISNOT, OP_IN, OP_NOTIN,
    OP_NOT, OP_UADD, OP_USUB, OP_INVERT,
    OP_AND, OP_OR
} P2C_AstOperator;

/* ========================================
 * 式ノード
 * ======================================== */

/* 二項演算 */
typedef struct {
    P2C_AstExpr *left;
    P2C_AstOperator op;
    P2C_AstExpr *right;
    bool is_fstring_concat;
} P2C_AstBinOp;

/* 単項演算 */
typedef struct {
    P2C_AstOperator op;
    P2C_AstExpr *operand;
} P2C_AstUnaryOp;

/* 比較演算 */
typedef struct {
    P2C_AstExpr *left;
    P2C_Vector *ops;        /* P2C_AstOperator* */
    P2C_Vector *comparators; /* P2C_AstExpr* */
} P2C_AstCompare;

/* 論理演算 */
typedef struct {
    P2C_AstOperator op;     /* OP_AND or OP_OR */
    P2C_Vector *values;     /* P2C_AstExpr* */
} P2C_AstBoolOp;

/* 関数呼び出し */
typedef struct {
    P2C_AstExpr *func;
    P2C_Vector *args;       /* P2C_AstExpr* */
    P2C_Vector *keywords;   /* P2C_AstKeyword* */
} P2C_AstCall;

/* 属性アクセス */
typedef struct {
    P2C_AstExpr *value;
    char *attr;             /* 属性名 */
} P2C_AstAttribute;

/* 添字アクセス */
typedef struct {
    P2C_AstExpr *value;
    P2C_AstExpr *slice;
} P2C_AstSubscript;

/* 変数名 */
typedef struct {
    char *name;
} P2C_AstName;

/* 定数 */
typedef struct {
    P2C_TokenType token_type; /* INT_LITERAL, FLOAT_LITERAL, STR_LITERAL, etc. */
    char *value;            /* 文字列表現 */
} P2C_AstConst;

/* 条件式 */
typedef struct {
    P2C_AstExpr *test;
    P2C_AstExpr *body;
    P2C_AstExpr *orelse;
} P2C_AstIfExp;

/* lambda */
typedef struct {
    P2C_Vector *args;       /* P2C_AstArg* */
    P2C_AstExpr *body;
} P2C_AstLambda;

/* リスト・タプル */
typedef struct {
    P2C_Vector *elts;       /* P2C_AstExpr* */
} P2C_AstList;
typedef P2C_AstList P2C_AstTuple;

/* 辞書 */
typedef struct {
    P2C_Vector *keys;       /* P2C_AstExpr*（NULLあり） */
    P2C_Vector *values;     /* P2C_AstExpr* */
} P2C_AstDict;

/* キーワード引数（func(a=1)） */
typedef struct {
    char *arg;
    P2C_AstExpr *value;
} P2C_AstKeyword;

/* 内包表記 */
typedef struct {
    P2C_AstExpr *elt;
    P2C_Vector *generators; /* P2C_AstComprehension* */
    bool set_result;
    P2C_AstExpr *dict_key;
} P2C_AstComprehension;

typedef struct {
    P2C_AstExpr *target;
    P2C_AstExpr *iter;
    P2C_Vector *ifs;        /* P2C_AstExpr* */
} P2C_AstComprehensionGen;

/* Starred */
typedef struct {
    P2C_AstExpr *value;
} P2C_AstStarred;

/* 代入式 */
typedef struct {
    P2C_AstExpr *target;
    P2C_AstExpr *value;
} P2C_AstNamedExpr;

typedef struct {
    P2C_AstExpr *value;     /* NULL = yield None */
    bool from;              /* `yield from iterable` の反復委譲 */
} P2C_AstYield;

typedef struct {
    P2C_AstExpr *value;
} P2C_AstAwait;

/* ========================================
 * 文ノード
 * ======================================== */

/* 代入 */
typedef struct {
    P2C_Vector *targets;    /* P2C_AstExpr* */
    P2C_AstExpr *value;
} P2C_AstAssign;

/* 複合代入 */
typedef struct {
    P2C_AstExpr *target;
    P2C_AstOperator op;
    P2C_AstExpr *value;
} P2C_AstAugAssign;

/* 型注釈付き代入 */
typedef struct {
    P2C_AstExpr *target;
    P2C_AstExpr *annotation;
    P2C_AstExpr *value;     /* NULLあり */
    int simple;             /* 単純代入かどうか */
} P2C_AstAnnAssign;

/* return */
typedef struct {
    P2C_AstExpr *value;     /* NULL = return None */
} P2C_AstReturn;

/* 式文 */
typedef struct {
    P2C_AstExpr *value;
} P2C_AstExprStmt;

/* if文 */
typedef struct {
    P2C_AstExpr *test;
    P2C_Vector *body;       /* P2C_AstStmt* */
    P2C_Vector *orelse;     /* P2C_AstStmt* */
} P2C_AstIf;

/* while文 */
typedef struct {
    P2C_AstExpr *test;
    P2C_Vector *body;       /* P2C_AstStmt* */
    P2C_Vector *orelse;     /* P2C_AstStmt* */
} P2C_AstWhile;

/* for文 */
typedef struct {
    P2C_AstExpr *target;
    P2C_AstExpr *iter;
    P2C_Vector *body;       /* P2C_AstStmt* */
    P2C_Vector *orelse;     /* P2C_AstStmt* */
} P2C_AstFor;

/* try文 */
typedef struct {
    P2C_Vector *body;       /* P2C_AstStmt* */
    P2C_Vector *handlers;   /* P2C_AstExceptHandler* */
    P2C_Vector *orelse;     /* P2C_AstStmt* */
    P2C_Vector *finalbody;  /* P2C_AstStmt* */
} P2C_AstTry;

/* except節 */
typedef struct {
    P2C_AstExpr *type;      /* 例外型（NULLあり） */
    char *name;             /* 例外変数名（NULLあり） */
    P2C_Vector *body;       /* P2C_AstStmt* */
} P2C_AstExceptHandler;

/* raise */
typedef struct {
    P2C_AstExpr *exc;       /* 例外（NULLあり） */
    P2C_AstExpr *cause;     /* from（NULLあり） */
} P2C_AstRaise;

/* assert */
typedef struct {
    P2C_AstExpr *test;
    P2C_AstExpr *msg;       /* NULLあり */
} P2C_AstAssert;

/* match/case */
typedef enum {
    P2C_MATCH_VALUE,
    P2C_MATCH_CAPTURE,
    P2C_MATCH_WILDCARD,
    P2C_MATCH_SEQUENCE,
    P2C_MATCH_MAPPING,
    P2C_MATCH_STAR,
    P2C_MATCH_AS,
    P2C_MATCH_OR,
    P2C_MATCH_CLASS
} P2C_MatchKind;

typedef struct P2C_AstMatchPattern P2C_AstMatchPattern;
struct P2C_AstMatchPattern {
    P2C_MatchKind kind;
    P2C_AstExpr *value;       /* value pattern または mapping key、NULLあり */
    char *capture_name;        /* capture/star/as用、NULLあり */
    char *rest_name;           /* mapping **rest用、NULLあり */
    char *class_name;          /* class pattern型名、NULLあり */
    P2C_Vector *children;      /* P2C_AstMatchPattern*。sequence/as/or/class属性用 */
    P2C_Vector *keys;          /* P2C_AstExpr*。mapping用 */
    P2C_Vector *attr_names;    /* char*。class keyword属性名用 */
};

typedef struct {
    P2C_AstMatchPattern *pattern;
    P2C_AstExpr *guard;        /* NULLあり */
    P2C_Vector *body;          /* P2C_AstStmt* */
} P2C_AstMatchCase;

typedef struct {
    P2C_AstExpr *subject;
    P2C_Vector *cases;      /* P2C_AstMatchCase* */
} P2C_AstMatch;

/* 関数定義 */
typedef struct {
    char *name;
    P2C_Vector *args;       /* P2C_AstArg*：位置orキーワード引数、続いてキーワード専用引数（宣言順） */
    int kwonly_start;       /* args[] 内でキーワード専用引数が始まるインデックス（無ければ p2c_vec_len(args)） */
    int posonly_count;       /* args[]先頭からの位置専用引数数。Python 3.8+の`/`区切りを表す */
    char *vararg;            /* *args のパラメータ名（無ければNULL） */
    char *kwarg;              /* **kwargs のパラメータ名（無ければNULL） */
    P2C_Vector *body;       /* P2C_AstStmt* */
    P2C_Vector *decorator_list; /* P2C_AstExpr* */
    P2C_AstExpr *returns;   /* 戻り値型注釈（NULLあり） */
    bool is_async;          /* async def */
} P2C_AstFunctionDef;

/* 引数 */
typedef struct {
    char *name;
    P2C_AstExpr *annotation; /* 型注釈（NULLあり） */
    P2C_AstExpr *default_val; /* デフォルト値（NULLあり） */
} P2C_AstArg;

/* クラス定義 */
typedef struct {
    char *name;
    P2C_Vector *bases;      /* P2C_AstExpr* */
    P2C_Vector *keywords;   /* P2C_AstKeyword* */
    P2C_Vector *body;       /* P2C_AstStmt* */
    P2C_Vector *decorator_list; /* P2C_AstExpr* */
} P2C_AstClassDef;

/* global/nonlocal */
typedef struct {
    P2C_Vector *names;      /* char* */
} P2C_AstGlobal;
typedef P2C_AstGlobal P2C_AstNonlocal;

/* import */
typedef struct {
    P2C_Vector *names;      /* P2C_AstAlias* */
} P2C_AstImport;

typedef struct {
    char *name;
    char *asname;           /* NULLあり */
} P2C_AstAlias;

typedef struct {
    char *module;           /* NULLあり */
    P2C_Vector *names;      /* P2C_AstAlias* */
    int level;              /* 相対インポートレベル */
} P2C_AstImportFrom;

/* with */
typedef struct {
    P2C_Vector *items;      /* P2C_AstWithItem* */
    P2C_Vector *body;       /* P2C_AstStmt* */
    bool is_async;          /* async with */
} P2C_AstWith;

typedef struct {
    P2C_AstExpr *context_expr;
    P2C_AstExpr *optional_vars; /* NULLあり */
} P2C_AstWithItem;

/* delete */
typedef struct {
    P2C_Vector *targets;    /* P2C_AstExpr* */
} P2C_AstDelete;

/* セミコロン区切りの複数文を1ノードにまとめる（parse_simple_stmt内部用）*/
typedef struct {
    P2C_Vector *stmts;      /* P2C_AstStmt* */
} P2C_AstBlock;

/* モジュール（ルート） */
typedef struct {
    P2C_Vector *body;       /* P2C_AstStmt* */
} P2C_AstModule;

/* ========================================
 * ASTノード共用体
 * ======================================== */

struct P2C_AstNode {
    P2C_AstType type;
    uint32_t line;
    uint32_t col;
    union {
        /* 式 */
        P2C_AstBinOp binop;
        P2C_AstUnaryOp unaryop;
        P2C_AstCompare compare;
        P2C_AstBoolOp boolop;
        P2C_AstCall call;
        P2C_AstAttribute attribute;
        P2C_AstSubscript subscript;
        P2C_AstName name;
        P2C_AstConst constant;
        P2C_AstIfExp ifexp;
        P2C_AstLambda lambda;
        P2C_AstList list;
        P2C_AstTuple tuple;
        P2C_AstDict dict;
        P2C_AstComprehension comprehension;
        P2C_AstStarred starred;
        P2C_AstNamedExpr named_expr;
        P2C_AstYield yield_expr;
        P2C_AstAwait await_expr;

        /* 文 */
        P2C_AstAssign assign;
        P2C_AstAugAssign augassign;
        P2C_AstAnnAssign annassign;
        P2C_AstReturn return_stmt;
        P2C_AstExprStmt expr_stmt;
        P2C_AstIf if_stmt;
        P2C_AstWhile while_stmt;
        P2C_AstFor for_stmt;
        P2C_AstTry try_stmt;
        P2C_AstRaise raise;
        P2C_AstAssert assert_stmt;
        P2C_AstMatch match_stmt;
        P2C_AstFunctionDef functiondef;
        P2C_AstClassDef classdef;
        P2C_AstGlobal global;
        P2C_AstNonlocal nonlocal_stmt;
        P2C_AstImport import_stmt;
        P2C_AstImportFrom importfrom;
        P2C_AstWith with;
        P2C_AstDelete delete;
        P2C_AstBlock block;
        P2C_AstModule module;
    } u;
};

/* Expr/Stmt は AstNode と同じ */
struct P2C_AstExpr { P2C_AstNode base; };
struct P2C_AstStmt { P2C_AstNode base; };

/* ========================================
 * AST API
 * ======================================== */

/* ノード作成（アロケータ使用） */
P2C_AstNode* p2c_ast_new(P2C_Allocator *a, P2C_AstType type, uint32_t line, uint32_t col);
P2C_AstExpr* p2c_ast_expr_new(P2C_Allocator *a, P2C_AstType type, uint32_t line, uint32_t col);
P2C_AstStmt* p2c_ast_stmt_new(P2C_Allocator *a, P2C_AstType type, uint32_t line, uint32_t col);

/* ノード破棄 */
void p2c_ast_free(P2C_AstNode *node, P2C_Allocator *a);
void p2c_ast_expr_free(P2C_AstExpr *expr, P2C_Allocator *a);
void p2c_ast_stmt_free(P2C_AstStmt *stmt, P2C_Allocator *a);

/* ヘルパー：ノード作成 */
P2C_AstExpr* p2c_ast_name(P2C_Allocator *a, const char *name, uint32_t line, uint32_t col);
P2C_AstExpr* p2c_ast_const_int(P2C_Allocator *a, const char *value, uint32_t line, uint32_t col);
P2C_AstExpr* p2c_ast_const_float(P2C_Allocator *a, const char *value, uint32_t line, uint32_t col);
P2C_AstExpr* p2c_ast_const_str(P2C_Allocator *a, const char *value, uint32_t line, uint32_t col);
P2C_AstExpr* p2c_ast_const_bool(P2C_Allocator *a, bool value, uint32_t line, uint32_t col);
P2C_AstExpr* p2c_ast_const_none(P2C_Allocator *a, uint32_t line, uint32_t col);

/* ヘルパー：演算子名取得 */
const char* p2c_ast_op_name(P2C_AstOperator op);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_AST_H */
