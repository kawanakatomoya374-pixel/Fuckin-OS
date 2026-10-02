#ifndef PYTHON_CODE_TO_C_SEMANTIC_H
#define PYTHON_CODE_TO_C_SEMANTIC_H

#include "parser/python_code_to_c_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * 型システム
 * ======================================== */

typedef enum {
    TYPE_UNKNOWN,
    TYPE_NONE,
    TYPE_BOOL,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STR,
    TYPE_LIST,
    TYPE_DICT,
    TYPE_TUPLE,
    TYPE_FUNCTION,
    TYPE_CLASS,
    TYPE_OBJECT,
    TYPE_MODULE,
    TYPE_ANY
} P2C_TypeKind;

typedef struct P2C_Type {
    P2C_TypeKind kind;
    char *name;             /* クラス名等 */
    struct P2C_Type *elem_type; /* リスト要素型等 */
    P2C_Vector *arg_types;  /* 関数引数型（P2C_Type*） */
    struct P2C_Type *ret_type; /* 関数戻り値型 */
} P2C_Type;

/* 組み込み型 */
P2C_Type* p2c_type_builtin(P2C_TypeKind kind);
P2C_Type* p2c_type_new(P2C_Allocator *a, P2C_TypeKind kind, const char *name);
void p2c_type_free(P2C_Type *t, P2C_Allocator *a);

/* ========================================
 * シンボルテーブル
 * ======================================== */

/* 前方宣言 */
typedef struct P2C_SymbolScope P2C_SymbolScope;

typedef enum {
    SCOPE_MODULE,
    SCOPE_FUNCTION,
    SCOPE_CLASS,
    SCOPE_COMPREHENSION
} P2C_SymbolScopeType;

typedef enum {
    SYM_VARIABLE,
    SYM_FUNCTION,
    SYM_CLASS,
    SYM_PARAMETER,
    SYM_GLOBAL,
    SYM_BUILTIN
} P2C_SymbolKind;

typedef struct P2C_Symbol {
    char *name;
    P2C_SymbolKind kind;
    P2C_Type *type;
    struct P2C_Symbol *next;
    /* 関数/クラス用 */
    P2C_AstNode *def_node;
    bool is_initialized;
} P2C_Symbol;

struct P2C_SymbolScope {
    P2C_SymbolScope *parent;
    P2C_Map *symbols;       /* name -> P2C_Symbol* */
    P2C_SymbolScopeType scope_type;
};

typedef struct P2C_SymbolTable {
    P2C_Allocator *alloc;
    P2C_SymbolScope *current;
    P2C_SymbolScope *global;
} P2C_SymbolTable;

P2C_SymbolTable* p2c_symtab_new(P2C_Allocator *a);
void p2c_symtab_free(P2C_SymbolTable *tab);

/* スコープ操作 */
void p2c_symtab_push_scope(P2C_SymbolTable *tab, P2C_SymbolScopeType type);
void p2c_symtab_pop_scope(P2C_SymbolTable *tab);
P2C_SymbolScope* p2c_symtab_current_scope(P2C_SymbolTable *tab);

/* シンボル操作 */
P2C_Symbol* p2c_symtab_lookup(P2C_SymbolTable *tab, const char *name);
P2C_Symbol* p2c_symtab_lookup_local(P2C_SymbolTable *tab, const char *name);
P2C_Result p2c_symtab_define(P2C_SymbolTable *tab, const char *name, P2C_SymbolKind kind, P2C_Type *type);

/* ========================================
 * セマンティックアナライザー
 * ======================================== */

typedef struct P2C_Semantic {
    P2C_Allocator *alloc;
    P2C_SymbolTable *symtab;
    P2C_Result last_error;
    char *error_msg;
    uint32_t error_line;
    uint32_t error_col;
} P2C_Semantic;

P2C_Semantic* p2c_semantic_new(P2C_Allocator *a);
void p2c_semantic_free(P2C_Semantic *sem);
const char* p2c_semantic_error_msg(P2C_Semantic *sem);

/* メインエントリ */
P2C_Result p2c_semantic_analyze(P2C_Semantic *sem, P2C_AstModule *mod);

/* 個別分析関数 */
P2C_Result p2c_semantic_visit_stmt(P2C_Semantic *sem, P2C_AstStmt *stmt);
P2C_Result p2c_semantic_visit_expr(P2C_Semantic *sem, P2C_AstExpr *expr, P2C_Type **out_type);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_SEMANTIC_H */
