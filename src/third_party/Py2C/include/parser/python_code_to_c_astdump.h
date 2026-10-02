#ifndef PYTHON_CODE_TO_C_ASTDUMP_H
#define PYTHON_CODE_TO_C_ASTDUMP_H

#include "parser/python_code_to_c_ast.h"
#include "common/python_code_to_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * python_code_to_c_astdump: パース済みASTを人間が読める木構造テキストとして出力する。
 * --dump-ast オプション、および開発時のデバッグ用途で使う。
 * コード生成(codegen)には一切関与しない、純粋な可視化ユーティリティ。
 */

/* モジュール全体をダンプし、outに追記する。 */
void p2c_ast_dump_module(P2C_AstModule *module, P2C_String *out);

/* 単一の文/式ノードをダンプする（内部再帰にも使用）。 */
void p2c_ast_dump_stmt(P2C_AstStmt *stmt, P2C_String *out, int indent);
void p2c_ast_dump_expr(P2C_AstExpr *expr, P2C_String *out, int indent);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_ASTDUMP_H */
