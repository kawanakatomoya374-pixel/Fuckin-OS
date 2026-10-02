#ifndef PYTHON_CODE_TO_C_PARSER_H
#define PYTHON_CODE_TO_C_PARSER_H

#include "parser/python_code_to_c_ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * パーサー構造体
 * ======================================== */

typedef struct P2C_Parser P2C_Parser;

struct P2C_Parser {
    P2C_Allocator *alloc;
    P2C_Lexer *lexer;
    P2C_Result last_error;
    char *error_msg;
    uint32_t error_line;
    uint32_t error_col;
};

/* ========================================
 * パーサーAPI
 * ======================================== */

P2C_Parser* p2c_parser_new(P2C_Allocator *a, P2C_Lexer *lex);
void p2c_parser_free(P2C_Parser *par);
const char* p2c_parser_error_msg(P2C_Parser *par);

/* メインエントリ：モジュール全体をパース */
P2C_AstModule* p2c_parser_parse_module(P2C_Parser *par, P2C_Result *out_err);

/* --fallback: 未対応構文（bytes/複素数リテラル等）をエラーにせず、
 * 実行時に NotImplementedError を送出するスタブへ置き換える。
 * 既定は無効（明確な診断を返す）。 */
void p2c_parser_set_fallback(bool enabled);
bool p2c_parser_fallback_enabled(void);

/* 個別パース関数（再帰下降） */
P2C_AstStmt* p2c_parser_stmt(P2C_Parser *par, P2C_Result *out_err);
P2C_AstExpr* p2c_parser_expr(P2C_Parser *par, P2C_Result *out_err);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_PARSER_H */
