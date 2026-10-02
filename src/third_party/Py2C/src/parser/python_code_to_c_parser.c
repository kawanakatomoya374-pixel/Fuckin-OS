#include "parser/python_code_to_c_parser.h"
#include "common/python_code_to_c_common.h"
#include <stddef.h>
#ifndef UINT64_MAX
#define UINT64_MAX 18446744073709551615ULL
#endif
#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807LL
#endif
#ifndef PYTHON_CODE_TO_C_NO_STDLIB
#include <ctype.h>
#include <stdio.h>
#endif

/* 前方宣言 */
static P2C_AstExpr* parse_expr(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_atom(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_power(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_factor(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_term(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_arith(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_shift(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_bitand(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_bitor(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_bitxor(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_comparison(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_not_test(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_and_test(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_or_test(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_conditional(P2C_Parser *p, P2C_Result *err);
static P2C_AstExpr* parse_lambda(P2C_Parser *p, P2C_Result *err);
static P2C_AstStmt* parse_stmt(P2C_Parser *p, P2C_Result *err);
static P2C_AstStmt* parse_simple_stmt(P2C_Parser *p, P2C_Result *err);
static P2C_AstStmt* parse_compound_stmt(P2C_Parser *p, P2C_Result *err);
static P2C_AstStmt* parse_assignment_or_expr(P2C_Parser *p, P2C_Result *err);
static P2C_AstStmt* parse_match_stmt(P2C_Parser *p, P2C_Result *err);
static P2C_Vector* parse_suite(P2C_Parser *p, P2C_Result *err);

#define CURRENT(p) p2c_lexer_peek((p)->lexer)
#define CONSUME(p, t) p2c_lexer_consume((p)->lexer, (t))
#define NEXT(p) p2c_lexer_next((p)->lexer)
#define EXPECT(p, t, e) p2c_lexer_expect((p)->lexer, (t), (e))

static void set_error(P2C_Parser *p, const char *msg) {
    P2C_Token *tok = CURRENT(p);
    p->error_line = tok ? tok->line : 0;
    p->error_col = tok ? tok->col : 0;
    if (p->error_msg) p2c_free(p->alloc, p->error_msg);
    p->error_msg = p2c_alloc(p->alloc, strlen(msg) + 1);
    if (p->error_msg) strcpy(p->error_msg, msg);
}

/* set_error()はエラー箇所を常に「今のトークン位置」(CURRENT(p))から取るが、
 * f-string内の式のように、既にメインの字句解析を通り過ぎたテキストを
 * 後から読み直して検証する場合は CURRENT(p) が無関係な(次の文の)トークンを
 * 指してしまう。そうした箇所では、呼び出し元が知っている正しい行・列を
 * 明示的に渡せるこちらを使う。 */
static void set_error_at(P2C_Parser *p, const char *msg, uint32_t line, uint32_t col) {
    p->error_line = line;
    p->error_col = col;
    if (p->error_msg) p2c_free(p->alloc, p->error_msg);
    p->error_msg = p2c_alloc(p->alloc, strlen(msg) + 1);
    if (p->error_msg) strcpy(p->error_msg, msg);
}

/* 「構文的には認識できるが、python_code_to_cがまだ対応していない」Python機能を
 * 検出したときに使う、原因を名指しする専用のエラー。汎用の
 * "unexpected token in expression" よりも次に取るべき行動が明確になる。
 * feature: 未対応の機能名（例: "list/dict/set 内包表記"）
 * hint:    代替案や補足（NULL可） */
static void set_unsupported_error(P2C_Parser *p, const char *feature, const char *hint) {
    char buf[256];
    if (hint) snprintf(buf, sizeof(buf), "unsupported syntax: %s is not supported by python_code_to_c. %s", feature, hint);
    else snprintf(buf, sizeof(buf), "unsupported syntax: %s is not supported by python_code_to_c.", feature);
    set_error(p, buf);
}

/* --fallback（未対応構文をランタイムスタブへ置換）の有効/無効。
 * 変換器は単一スレッドなのでファイルスコープで足りる。 */
static bool g_parser_fallback = false;

void p2c_parser_set_fallback(bool enabled) { g_parser_fallback = enabled; }
bool p2c_parser_fallback_enabled(void) { return g_parser_fallback; }

/* 未対応構文の代わりに「呼ぶとNotImplementedErrorになる」名前ノードを作る。
 * codegen が接頭辞を見て p2c_fallback_expr() へ変換する。 */
static P2C_AstExpr* unsupported_stub_expr(P2C_Parser *p, P2C_Token *tok, const char *feature) {
    char name[160];
    size_t len;
    P2C_AstExpr *expr = p2c_ast_expr_new(p->alloc, AST_NAME,
                                         tok ? tok->line : 0u, tok ? tok->col : 0u);
    if (!expr) return NULL;
    snprintf(name, sizeof(name), P2C_UNSUPPORTED_NAME_PREFIX "%s",
             feature ? feature : "construct");
    len = strlen(name);
    expr->base.u.name.name = (char*)p2c_alloc(p->alloc, len + 1u);
    if (!expr->base.u.name.name) return NULL;
    memcpy(expr->base.u.name.name, name, len + 1u);
    return expr;
}

static bool is_at_end(P2C_Parser *p) {
    P2C_Token *tok = CURRENT(p);
    return !tok || tok->type == TOK_EOF;
}

/* ========================================
 * パーサー作成/破棄
 * ======================================== */

P2C_Parser* p2c_parser_new(P2C_Allocator *a, P2C_Lexer *lex) {
    P2C_Parser *p = p2c_alloc(a, sizeof(P2C_Parser));
    if (!p) return NULL;
    p->alloc = a;
    p->lexer = lex;
    p->last_error = P2C_OK;
    p->error_msg = NULL;
    p->error_line = 0;
    p->error_col = 0;
    return p;
}

void p2c_parser_free(P2C_Parser *p) {
    if (!p) return;
    if (p->error_msg) p2c_free(p->alloc, p->error_msg);
    p2c_free(p->alloc, p);
}

const char* p2c_parser_error_msg(P2C_Parser *p) {
    return p ? p->error_msg : NULL;
}

/* ========================================
 * 式の構文解析（優先度順）
 * ======================================== */

/* トークンから演算子へ */
static P2C_AstOperator token_to_binop(P2C_TokenType t) {
    switch (t) {
        case TOK_PLUS: return OP_ADD;
        case TOK_MINUS: return OP_SUB;
        case TOK_STAR: return OP_MULT;
        case TOK_SLASH: return OP_DIV;
        case TOK_DBL_SLASH: return OP_FLOORDIV;
        case TOK_PERCENT: return OP_MOD;
        case TOK_DBL_STAR: return OP_POW;
        case TOK_LSHIFT: return OP_LSHIFT;
        case TOK_RSHIFT: return OP_RSHIFT;
        case TOK_AMPERSAND: return OP_BITAND;
        case TOK_PIPE: return OP_BITOR;
        case TOK_CARET: return OP_BITXOR;
        default: return OP_ADD;
    }
}

static P2C_AstOperator token_to_cmpop(P2C_TokenType t) {
    switch (t) {
        case TOK_LT: return OP_LT;
        case TOK_LE: return OP_LE;
        case TOK_EQ: return OP_EQ;
        case TOK_NE: return OP_NE;
        case TOK_GT: return OP_GT;
        case TOK_GE: return OP_GE;
        default: return OP_EQ;
    }
}

/* f-string の {...} プレースホルダを実際の式としてパースするための
 * 使い捨てサブレクサ/サブパーサ。式の断片だけを渡して parse_expr する。 */
static P2C_AstExpr* parse_expr_from_substring(P2C_Parser *p, const char *text, size_t len, P2C_Result *err) {
    P2C_Lexer *sublex = p2c_lexer_new(p->alloc, text, len);
    if (!sublex) return NULL;
    P2C_Parser *subp = p2c_parser_new(p->alloc, sublex);
    if (!subp) { p2c_lexer_free(sublex); return NULL; }
    P2C_AstExpr *expr = parse_expr(subp, err);
    p2c_parser_free(subp);
    p2c_lexer_free(sublex);
    return expr;
}

static P2C_AstExpr* fstr_concat(P2C_Parser *p, P2C_AstExpr *a, P2C_AstExpr *b, uint32_t line, uint32_t col) {
    if (!a) return b;
    if (!b) return a;
    P2C_AstExpr *n = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
    if (!n) return a;
    n->base.u.binop.op = OP_ADD;
    n->base.u.binop.left = a;
    n->base.u.binop.right = b;
    n->base.u.binop.is_fstring_concat = true;
    return n;
}

/* {expr} を str(expr) 呼び出しに、{expr:.Nf} を p2c_format_fixed(expr, N) 呼び出しに包む。
 * それ以外の書式指定は p2c_fstr_fmt(expr, spec) 経由でランタイムに委譲する。 */
static P2C_AstExpr* fstr_wrap_value(P2C_Parser *p, P2C_AstExpr *inner, char conversion, const char *spec, size_t spec_len, uint32_t line, uint32_t col) {
    if (conversion == 'r' || conversion == 'a' || conversion == 's') {
        P2C_AstExpr *converted = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
        if (!converted) return inner;
        converted->base.u.call.args = p2c_vec_new(p->alloc, NULL);
        p2c_vec_push(converted->base.u.call.args, inner);
        converted->base.u.call.func = p2c_ast_name(p->alloc, conversion == 's' ? "str" : "repr", line, col);
        inner = converted;
    }
    /* 書式なし: str(expr) */
    if (!spec || spec_len == 0) {
        P2C_AstExpr *call = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
        if (!call) return inner;
        call->base.u.call.args = p2c_vec_new(p->alloc, NULL);
        p2c_vec_push(call->base.u.call.args, inner);
        call->base.u.call.func = p2c_ast_name(p->alloc, "str", line, col);
        return call;
    }

    /* :.Nf → p2c_format_fixed(expr, N) */
    if (spec_len >= 3 && spec[0] == '.' && spec[spec_len - 1] == 'f') {
        bool ok = true; int n = 0;
        for (size_t k = 1; k < spec_len - 1; k++) {
            if (!isdigit((unsigned char)spec[k])) { ok = false; break; }
            n = n * 10 + (spec[k] - '0');
        }
        if (ok) {
            P2C_AstExpr *call = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
            if (!call) return inner;
            call->base.u.call.args = p2c_vec_new(p->alloc, NULL);
            p2c_vec_push(call->base.u.call.args, inner);
            call->base.u.call.func = p2c_ast_name(p->alloc, "p2c_format_fixed", line, col);
            char digitbuf[16]; snprintf(digitbuf, sizeof(digitbuf), "%d", n);
            p2c_vec_push(call->base.u.call.args, p2c_ast_const_int(p->alloc, digitbuf, line, col));
            return call;
        }
    }

    /* それ以外の書式指定: p2c_fstr_fmt(expr, "spec") でランタイムに委譲する
     * これで :05d / :>10s / :+.3e などすべてのPython書式に対応できる。 */
    char *spec_copy = p2c_alloc(p->alloc, spec_len + 1);
    if (!spec_copy) {
        P2C_AstExpr *call = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
        if (!call) return inner;
        call->base.u.call.args = p2c_vec_new(p->alloc, NULL);
        p2c_vec_push(call->base.u.call.args, inner);
        call->base.u.call.func = p2c_ast_name(p->alloc, "str", line, col);
        return call;
    }
    memcpy(spec_copy, spec, spec_len); spec_copy[spec_len] = '\0';
    P2C_AstExpr *call = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
    if (!call) return inner;
    call->base.u.call.args = p2c_vec_new(p->alloc, NULL);
    p2c_vec_push(call->base.u.call.args, inner);
    call->base.u.call.func = p2c_ast_name(p->alloc, "p2c_fstr_fmt", line, col);
    p2c_vec_push(call->base.u.call.args, p2c_ast_const_str(p->alloc, spec_copy, line, col));
    return call;
}

static P2C_AstExpr* build_fstring_expr(P2C_Parser *p, const char *text, size_t len, uint32_t line, uint32_t col, P2C_Result *err) {
    P2C_AstExpr *result = NULL;
    P2C_String *lit = p2c_str_new(p->alloc);
    if (!lit) return NULL;
    size_t i = 0;
    while (i < len) {
        if (text[i] == '{' && i + 1 < len && text[i+1] == '{') {
            p2c_str_append_char(lit, '{');
            i += 2;
        } else if (text[i] == '}' && i + 1 < len && text[i+1] == '}') {
            p2c_str_append_char(lit, '}');
            i += 2;
        } else if (text[i] == '{') {
            if (p2c_str_len(lit) > 0) {
                P2C_AstExpr *lit_node = p2c_ast_const_str(p->alloc, p2c_str_cstr(lit), line, col);
                result = fstr_concat(p, result, lit_node, line, col);
                p2c_str_free(lit); lit = p2c_str_new(p->alloc); if (!lit) return NULL;
            }
            size_t j = i + 1;
            int depth = 1;
            char q = 0;
            while (j < len && depth > 0) {
                char c = text[j];
                if (q) { if (c == '\\') { j += 2; continue; } if (c == q) q = 0; j++; continue; }
                if (c == '"' || c == '\'') { q = c; j++; continue; }
                if (c == '{') depth++;
                if (c == '}') { depth--; if (depth == 0) break; }
                j++;
            }
            size_t content_start = i + 1, content_end = j; /* j は閉じ'}'の位置（無い場合はlen） */
            size_t expr_end = content_end;
            size_t spec_start = (size_t)-1;
            char conversion = 0;
            {
                int d2 = 0; char q2 = 0;
                for (size_t k = content_start; k < content_end; k++) {
                    char c = text[k];
                    if (q2) { if (c == '\\') { k++; continue; } if (c == q2) q2 = 0; continue; }
                    if (c == '"' || c == '\'') { q2 = c; continue; }
                    if (c == '(' || c == '[') d2++;
                    if (c == ')' || c == ']') d2--;
                    if (c == '!' && d2 == 0 && k + 1 < content_end) {
                        conversion = text[k + 1];
                        expr_end = k;
                        if (k + 2 < content_end && text[k + 2] == ':') spec_start = k + 3;
                        break;
                    }
                    if (c == ':' && d2 == 0) { expr_end = k; spec_start = k + 1; break; }
                }
            }
            if (expr_end <= content_start) { p2c_str_free(lit); set_error_at(p, "empty expression in f-string", line, col); if (err) *err = P2C_ERR_SYNTAX; return NULL; }
            P2C_AstExpr *sub = parse_expr_from_substring(p, text + content_start, expr_end - content_start, err);
            if (!sub) { p2c_str_free(lit); return NULL; }
            const char *spec = (spec_start != (size_t)-1) ? text + spec_start : NULL;
            size_t spec_len = (spec_start != (size_t)-1) ? content_end - spec_start : 0;
            P2C_AstExpr *wrapped = fstr_wrap_value(p, sub, conversion, spec, spec_len, line, col);
            result = fstr_concat(p, result, wrapped, line, col);
            i = (j < len) ? j + 1 : j;
        } else {
            p2c_str_append_char(lit, text[i]);
            i++;
        }
    }
    if (p2c_str_len(lit) > 0 || !result) {
        P2C_AstExpr *lit_node = p2c_ast_const_str(p->alloc, p2c_str_cstr(lit), line, col);
        result = fstr_concat(p, result, lit_node, line, col);
    }
    p2c_str_free(lit);
    return result;
}

static P2C_AstExpr* ast_none_const(P2C_Parser *p, uint32_t line, uint32_t col) {
    P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_CONST, line, col);
    if (!e) return NULL;
    e->base.u.constant.token_type = TOK_NONE_LITERAL;
    e->base.u.constant.value = NULL;
    return e;
}

static bool is_cmpop_token(P2C_TokenType t) {
    return t == TOK_LT || t == TOK_LE || t == TOK_EQ || t == TOK_NE ||
           t == TOK_GT || t == TOK_GE;
}

/* atom: identifier | literal | '(' [yield_expr|testlist_comp] ')' | '[' [testlist_comp] ']' | '{' [dictorsetmaker] '}' */
/* 内包表記の 'for target in iter (if cond)*' を1つ以上パースする。
 * 呼び出し時点でCURRENTはTOK_KW_FORを指している前提。 */
static P2C_Vector* parse_comprehension_generators(P2C_Parser *p, P2C_Result *err) {
    P2C_Vector *gens = p2c_vec_new(p->alloc, NULL);
    if (!gens) return NULL;
    while (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
        NEXT(p);
        P2C_Token *target_tok = EXPECT(p, TOK_IDENTIFIER, err);
        uint32_t target_line;
        uint32_t target_col;
        if (!target_tok) { set_error(p, "expected loop variable name in comprehension"); return NULL; }
        target_line = target_tok->line;
        target_col = target_tok->col;
        P2C_AstExpr *target = p2c_ast_name(p->alloc, target_tok->text, target_line, target_col);
        if (CURRENT(p) && CURRENT(p)->type == TOK_COMMA) {
            /* for k, v in ... のタプルアンパックにも対応 */
            P2C_Vector *elts = p2c_vec_new(p->alloc, NULL);
            p2c_vec_push(elts, target);
            while (CONSUME(p, TOK_COMMA)) {
                if (!CURRENT(p) || CURRENT(p)->type == TOK_KW_IN) break;
                P2C_Token *more = EXPECT(p, TOK_IDENTIFIER, err);
                if (!more) { set_error(p, "expected loop variable name in comprehension"); return NULL; }
                p2c_vec_push(elts, p2c_ast_name(p->alloc, more->text, more->line, more->col));
            }
            P2C_AstExpr *tup = p2c_ast_expr_new(p->alloc, AST_TUPLE, target_line, target_col);
            tup->base.u.tuple.elts = elts;
            target = tup;
        }
        if (!EXPECT(p, TOK_KW_IN, err)) { set_error(p, "expected 'in' in comprehension"); return NULL; }
        /* iterはif/for/]/}/)に達するまでの式。三項式のifと区別するため、
         * ここでは論理式レベル(if/elseを含まない)までをparse_or_testで読む。 */
        P2C_AstExpr *iter = parse_or_test(p, err);
        if (!iter) return NULL;
        P2C_AstComprehensionGen *gen = p2c_alloc(p->alloc, sizeof(P2C_AstComprehensionGen));
        if (!gen) return NULL;
        gen->target = target;
        gen->iter = iter;
        gen->ifs = p2c_vec_new(p->alloc, NULL);
        while (CURRENT(p) && CURRENT(p)->type == TOK_KW_IF) {
            NEXT(p);
            P2C_AstExpr *cond = parse_or_test(p, err);
            if (!cond) return NULL;
            p2c_vec_push(gen->ifs, cond);
        }
        p2c_vec_push(gens, gen);
    }
    return gens;
}

static bool is_string_literal_token(P2C_TokenType type) {
    return type == TOK_STR_LITERAL || type == TOK_FSTRING_LITERAL;
}

/* Pythonでは同一式内の隣接する文字列リテラルを暗黙に連結する。
 * 改行をまたぐ括弧内の連結（例: f"a"\n f"b"）もここで処理する。 */
static P2C_AstExpr* parse_adjacent_string_literals(P2C_Parser *p, P2C_AstExpr *first,
                                                     P2C_Result *err) {
    P2C_AstExpr *result = first;
    while (CURRENT(p) && is_string_literal_token(CURRENT(p)->type)) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        P2C_AstExpr *next = NULL;
        if (tok->type == TOK_FSTRING_LITERAL) {
            char *raw = tok->text;
            size_t raw_len = tok->len;
            NEXT(p);
            next = build_fstring_expr(p, raw, raw_len, line, col, err);
        } else {
            next = p2c_ast_const_str(p->alloc, tok->text, line, col);
            NEXT(p);
        }
        if (!next) return NULL;
        if (result && result->base.type == AST_CONST &&
            result->base.u.constant.token_type == TOK_STR_LITERAL &&
            next->base.type == AST_CONST &&
            next->base.u.constant.token_type == TOK_STR_LITERAL) {
            size_t left_len = strlen(result->base.u.constant.value);
            size_t right_len = strlen(next->base.u.constant.value);
            char *joined = p2c_alloc(p->alloc, left_len + right_len + 1);
            if (!joined) return NULL;
            memcpy(joined, result->base.u.constant.value, left_len);
            memcpy(joined + left_len, next->base.u.constant.value, right_len + 1);
            p2c_free(p->alloc, result->base.u.constant.value);
            result->base.u.constant.value = joined;
            p2c_ast_expr_free(next, p->alloc);
        } else {
            result = fstr_concat(p, result, next, line, col);
        }
    }
    return result;
}

/* 数値リテラルの表記を「C として妥当な形」へ正規化する。
 * codegen は p2c_obj_from_int(<text>) / p2c_obj_from_float(<text>) を出力する
 * ため、text はそのまま C の数値でなければならない。
 *   - '_' 区切りは除去する（1_000 -> 1000。C11 の数値に '_' は書けない）
 *   - 0x/0o/0b は 10 進へ（0x1f -> 31）
 *   - int64 に収まらない基数つきリテラルはエラー（CPython は多倍長だが
 *     この処理系は int64 のみ。黙って別の値になるより明示的に失敗させる）
 * 成功時は p2c_alloc 済みの文字列を返す。失敗時は *ok=false を設定する。 */
static char* normalize_number_text(P2C_Parser *p, const char *text, size_t len, bool *ok) {
    char tmp[160];
    size_t n = 0;
    *ok = true;
    for (size_t i = 0; i < len && n + 1u < sizeof(tmp); i++) {
        if (text[i] != '_') tmp[n++] = text[i];
    }
    tmp[n] = '\0';
    char *out = p2c_alloc(p->alloc, n + 32u > 32u ? n + 32u : 32u);
    if (!out) { *ok = false; return NULL; }
    int base = 10;
    const char *digits = tmp;
    if (n > 2u && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) { base = 16; digits = tmp + 2; }
    else if (n > 2u && tmp[0] == '0' && (tmp[1] == 'o' || tmp[1] == 'O')) { base = 8; digits = tmp + 2; }
    else if (n > 2u && tmp[0] == '0' && (tmp[1] == 'b' || tmp[1] == 'B')) { base = 2; digits = tmp + 2; }
    if (base == 10) {
        memcpy(out, tmp, n + 1u);
        return out;
    }
    uint64_t value = 0;
    for (const char *q = digits; *q; q++) {
        int d;
        if (*q >= '0' && *q <= '9') d = *q - '0';
        else if (*q >= 'a' && *q <= 'f') d = *q - 'a' + 10;
        else if (*q >= 'A' && *q <= 'F') d = *q - 'A' + 10;
        else { set_error(p, "invalid digit in integer literal"); *ok = false; return NULL; }
        if (d >= base) { set_error(p, "invalid digit in integer literal"); *ok = false; return NULL; }
        if (value > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) {
            set_error(p, "integer literal too large for this runtime (int64)");
            *ok = false;
            return NULL;
        }
        value = value * (uint64_t)base + (uint64_t)d;
    }
    if (value > (uint64_t)INT64_MAX) {
        set_error(p, "integer literal too large for this runtime (int64)");
        *ok = false;
        return NULL;
    }
    snprintf(out, n + 32u, "%llu", (unsigned long long)value);
    return out;
}

static P2C_AstExpr* parse_atom(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    if (!tok || !tok->text) { if (err) *err = P2C_ERR_SYNTAX; return NULL; }
    
    uint32_t line = tok->line, col = tok->col;
    
    switch (tok->type) {
        case TOK_IDENTIFIER: {
            char *name = p2c_alloc(p->alloc, tok->len + 1);
            if (name) memcpy(name, tok->text, tok->len + 1);
            NEXT(p);
            return p2c_ast_name(p->alloc, name, line, col);
        }

        case TOK_INT_LITERAL:
        case TOK_FLOAT_LITERAL: {
            /* '_' 除去と 0x/0o/0b の 10 進化（C として妥当な表記にする）。 */
            bool num_ok = true;
            char *norm = normalize_number_text(p, tok->text, tok->len, &num_ok);
            if (!num_ok) return NULL;
            P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_CONST, line, col);
            if (e) {
                e->base.u.constant.token_type = tok->type;
                e->base.u.constant.value = norm;
            }
            NEXT(p);
            return e;
        }
        case TOK_STR_LITERAL: {
            P2C_AstExpr *e = p2c_ast_const_str(p->alloc, tok->text, line, col);
            NEXT(p);
            return parse_adjacent_string_literals(p, e, err);
        }
        case TOK_BOOL_LITERAL:
        case TOK_NONE_LITERAL:
        case TOK_ELLIPSIS: {
            P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_CONST, line, col);
            if (e) {
                e->base.u.constant.token_type = tok->type;
                e->base.u.constant.value = p2c_alloc(p->alloc, tok->len + 1);
                if (e->base.u.constant.value) memcpy(e->base.u.constant.value, tok->text, tok->len + 1);
            }
            NEXT(p);
            return e;
        }
        case TOK_FSTRING_LITERAL: {
            char *raw = tok->text;
            size_t raw_len = tok->len;
            NEXT(p);
            P2C_AstExpr *e = build_fstring_expr(p, raw, raw_len, line, col, err);
            return parse_adjacent_string_literals(p, e, err);
        }
        case TOK_LPAREN: {
            NEXT(p);
            if (CONSUME(p, TOK_RPAREN)) {
                /* 空タプル */
                P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_TUPLE, line, col);
                if (e) e->base.u.tuple.elts = p2c_vec_new(p->alloc, NULL);
                return e;
            }
            P2C_AstExpr *inner = parse_expr(p, err);
            if (!inner) return NULL;
            if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                P2C_Vector *gens = parse_comprehension_generators(p, err);
                P2C_AstExpr *genexp;
                if (!gens) return NULL;
                genexp = p2c_ast_expr_new(p->alloc, AST_GENERATOR_EXPRESSION, line, col);
                if (!genexp) return NULL;
                genexp->base.u.comprehension.elt = inner;
                genexp->base.u.comprehension.generators = gens;
                if (!EXPECT(p, TOK_RPAREN, err)) { set_error(p, "expected ')' after generator expression"); return NULL; }
                return genexp;
            }
            if (CONSUME(p, TOK_COMMA)) {
                P2C_AstExpr *tuple = p2c_ast_expr_new(p->alloc, AST_TUPLE, line, col);
                if (!tuple) return NULL;
                tuple->base.u.tuple.elts = p2c_vec_new(p->alloc, NULL);
                p2c_vec_push(tuple->base.u.tuple.elts, inner);
                if (CURRENT(p) && CURRENT(p)->type != TOK_RPAREN) {
                    while (1) {
                        P2C_AstExpr *item = parse_expr(p, err);
                        if (!item) return NULL;
                        p2c_vec_push(tuple->base.u.tuple.elts, item);
                        if (!CONSUME(p, TOK_COMMA)) break;
                        if (CURRENT(p) && CURRENT(p)->type == TOK_RPAREN) break;
                    }
                }
                if (!EXPECT(p, TOK_RPAREN, err)) {
                    set_error(p, "expected ')'");
                    return NULL;
                }
                return tuple;
            }
            if (!EXPECT(p, TOK_RPAREN, err)) {
                set_error(p, "expected ')'");
                return NULL;
            }
            return inner;
        }
        case TOK_LBRACKET: {
            NEXT(p);
            P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_LIST, line, col);
            if (!e) return NULL;
            e->base.u.list.elts = p2c_vec_new(p->alloc, NULL);
            if (CURRENT(p) && CURRENT(p)->type != TOK_RBRACKET) {
                while (1) {
                    P2C_AstExpr *item = parse_expr(p, err);
                    if (!item) return NULL;
                    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                        /* リスト内包表記 [item for target in iter (if cond)*]。
                         * 以前はここで常に「未対応」としてエラーにしていたが、
                         * 実際にサポートするよう変更した。 */
                        P2C_Vector *gens = parse_comprehension_generators(p, err);
                        if (!gens) return NULL;
                        P2C_AstExpr *comp = p2c_ast_expr_new(p->alloc, AST_COMPREHENSION, line, col);
                        if (!comp) return NULL;
                        comp->base.u.comprehension.elt = item;
                        comp->base.u.comprehension.generators = gens;
                        if (!EXPECT(p, TOK_RBRACKET, err)) { set_error(p, "expected ']' "); return NULL; }
                        p2c_vec_free(e->base.u.list.elts);
                        p2c_free(p->alloc, e);
                        return comp;
                    }
                    p2c_vec_push(e->base.u.list.elts, item);
                    if (!CONSUME(p, TOK_COMMA)) break;
                    if (CURRENT(p) && CURRENT(p)->type == TOK_RBRACKET) break;
                }
            }
            if (!EXPECT(p, TOK_RBRACKET, err)) {
                set_error(p, "expected ']'");
                return NULL;
            }
            return e;
        }
        case TOK_LBRACE: {
            NEXT(p);
            P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_DICT, line, col);
            if (!e) return NULL;
            e->base.u.dict.keys = p2c_vec_new(p->alloc, NULL);
            e->base.u.dict.values = p2c_vec_new(p->alloc, NULL);
            if (CURRENT(p) && CURRENT(p)->type != TOK_RBRACE) {
                while (1) {
                    if (CURRENT(p) && CURRENT(p)->type == TOK_DBL_STAR) {
                        NEXT(p);
                        P2C_AstExpr *mapping = parse_expr(p, err);
                        if (!mapping) return NULL;
                        p2c_vec_push(e->base.u.dict.keys, NULL);
                        p2c_vec_push(e->base.u.dict.values, mapping);
                        if (!CONSUME(p, TOK_COMMA)) break;
                        if (CURRENT(p) && CURRENT(p)->type == TOK_RBRACE) break;
                        continue;
                    }
                    P2C_AstExpr *key = parse_expr(p, err);
                    if (!key) return NULL;
                    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                        P2C_Vector *gens = parse_comprehension_generators(p, err);
                        if (!gens) return NULL;
                        P2C_AstExpr *comp = p2c_ast_expr_new(p->alloc, AST_COMPREHENSION, line, col);
                        if (!comp) return NULL;
                        comp->base.u.comprehension.elt = key;
                        comp->base.u.comprehension.generators = gens;
                        comp->base.u.comprehension.set_result = true;
                        if (!EXPECT(p, TOK_RBRACE, err)) { set_error(p, "expected '}'"); return NULL; }
                        p2c_vec_free(e->base.u.dict.keys);
                        p2c_vec_free(e->base.u.dict.values);
                        p2c_free(p->alloc, e);
                        return comp;
                    }
                    if (CURRENT(p) && (CURRENT(p)->type == TOK_COMMA || CURRENT(p)->type == TOK_RBRACE)) {
                        e->base.type = AST_SET;
                        p2c_vec_push(e->base.u.dict.keys, key);
                        p2c_vec_free(e->base.u.dict.values);
                        e->base.u.list.elts = e->base.u.dict.keys;
                        while (CONSUME(p, TOK_COMMA)) {
                            if (CURRENT(p) && CURRENT(p)->type == TOK_RBRACE) break;
                            P2C_AstExpr *item = parse_expr(p, err);
                            if (!item) return NULL;
                            if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                                set_unsupported_error(p, "set comprehensions ({x for x in ...})",
                                    "Use an explicit for-loop or a list comprehension until set comprehension support is added.");
                                if (err) *err = P2C_ERR_SYNTAX;
                                return NULL;
                            }
                            p2c_vec_push(e->base.u.list.elts, item);
                        }
                        if (!EXPECT(p, TOK_RBRACE, err)) { set_error(p, "expected '}'"); return NULL; }
                        return e;
                    }
                    p2c_vec_push(e->base.u.dict.keys, key);
                    if (!EXPECT(p, TOK_COLON, err)) {
                        set_error(p, "expected ':' in dict literal");
                        return NULL;
                    }
                    P2C_AstExpr *val = parse_expr(p, err);
                    if (!val) return NULL;
                    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                        P2C_Vector *gens = parse_comprehension_generators(p, err);
                        if (!gens) return NULL;
                        P2C_AstExpr *comp = p2c_ast_expr_new(p->alloc, AST_COMPREHENSION, line, col);
                        if (!comp) return NULL;
                        comp->base.u.comprehension.elt = val;
                        comp->base.u.comprehension.dict_key = key;
                        comp->base.u.comprehension.generators = gens;
                        if (!EXPECT(p, TOK_RBRACE, err)) { set_error(p, "expected '}'"); return NULL; }
                        p2c_vec_free(e->base.u.dict.keys);
                        p2c_vec_free(e->base.u.dict.values);
                        p2c_free(p->alloc, e);
                        return comp;
                    }
                    p2c_vec_push(e->base.u.dict.values, val);
                    if (!CONSUME(p, TOK_COMMA)) break;
                    if (CURRENT(p) && CURRENT(p)->type == TOK_RBRACE) break;
                }
            }
            if (!EXPECT(p, TOK_RBRACE, err)) {
                set_error(p, "expected '}'");
                return NULL;
            }
            return e;
        }
        case TOK_KW_YIELD: {
            NEXT(p);
            P2C_AstExpr *yield_expr = p2c_ast_expr_new(p->alloc, AST_YIELD, line, col);
            if (!yield_expr) return NULL;
            yield_expr->base.u.yield_expr.value = NULL;
            yield_expr->base.u.yield_expr.from = false;
            if (CONSUME(p, TOK_KW_FROM)) yield_expr->base.u.yield_expr.from = true;
            if (CURRENT(p) && CURRENT(p)->type != TOK_NEWLINE && CURRENT(p)->type != TOK_COMMA &&
                CURRENT(p)->type != TOK_RPAREN && CURRENT(p)->type != TOK_RBRACKET) {
                yield_expr->base.u.yield_expr.value = parse_expr(p, err);
                if (!yield_expr->base.u.yield_expr.value) return NULL;
            }
            if (yield_expr->base.u.yield_expr.from && !yield_expr->base.u.yield_expr.value) {
                set_error(p, "expected iterable after 'yield from'");
                if (err) *err = P2C_ERR_SYNTAX;
                return NULL;
            }
            return yield_expr;
        }
        default:
            if (tok->type == TOK_UNKNOWN && tok->text && strcmp(tok->text, "complexliteral") == 0) {
                if (g_parser_fallback) {
                    /* 未対応トークンを消費してからスタブ式を返す
                     * （消費しないと呼び出し側が同じトークンを見続ける）。 */
                    NEXT(p);
                    return unsupported_stub_expr(p, tok, "complex literal");
                }
                set_unsupported_error(p, "complex number literals (e.g. 2j, 3.5J)",
                    "python_code_to_c has no complex number type. Consider using a (real, imag) tuple or two separate float variables instead.");
            } else if (tok->type == TOK_UNKNOWN && tok->text && strcmp(tok->text, "bytesliteral") == 0) {
                if (g_parser_fallback) {
                    /* 未対応トークンを消費してからスタブ式を返す
                     * （消費しないと呼び出し側が同じトークンを見続ける）。 */
                    NEXT(p);
                    return unsupported_stub_expr(p, tok, "bytes literal");
                }
                set_unsupported_error(p, "bytes literals (b\"...\")",
                    "python_code_to_c has no bytes type. Consider using a regular str or a list of ints instead.");
            } else if (tok->type == TOK_UNKNOWN && tok->text && strcmp(tok->text, "unicodenamedescape") == 0) {
                if (g_parser_fallback) {
                    /* 未対応トークンを消費してからスタブ式を返す
                     * （消費しないと呼び出し側が同じトークンを見続ける）。 */
                    NEXT(p);
                    return unsupported_stub_expr(p, tok, "unicode name escape");
                }
                set_unsupported_error(p, "\\N{...} unicode name escapes",
                    "the Unicode name table is not embedded. Use \\uXXXX / \\UXXXXXXXX codepoint escapes instead.");
            } else if (tok->type == TOK_UNKNOWN && tok->text && strcmp(tok->text, "invalidhexescape") == 0) {
                set_error(p, "\\x escape must be followed by exactly two hexadecimal digits");
            } else if (tok->type == TOK_UNKNOWN && tok->text && strcmp(tok->text, "invalidunicodeescape") == 0) {
                set_error(p, "\\u and \\U escapes require 4 or 8 hexadecimal digits (and a valid codepoint)");
            } else {
                set_error(p, "unexpected token in expression");
            }
            if (err) *err = P2C_ERR_SYNTAX;
            return NULL;
    }
}

/* trailer: '(' [arglist] ')' | '[' subscriptlist ']' | '.' NAME */
static P2C_AstExpr* parse_trailer(P2C_Parser *p, P2C_AstExpr *primary, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    
    if (CONSUME(p, TOK_LPAREN)) {
        /* 関数呼び出し */
        P2C_AstExpr *call = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
        if (!call) return NULL;
        call->base.u.call.func = primary;
        call->base.u.call.args = p2c_vec_new(p->alloc, NULL);
        call->base.u.call.keywords = p2c_vec_new(p->alloc, NULL);
        
        if (CURRENT(p) && CURRENT(p)->type != TOK_RPAREN) {
            while (1) {
                /* **kwargs アンパック */
                if (CURRENT(p) && CURRENT(p)->type == TOK_DBL_STAR) {
                    NEXT(p); /* '**' を消費 */
                    P2C_AstExpr *val = parse_expr(p, err);
                    if (!val) return NULL;
                    /* arg=NULL はアンパック展開を意味するキーワード引数として扱う */
                    P2C_AstKeyword *kw = p2c_alloc(p->alloc, sizeof(P2C_AstKeyword));
                    if (kw) { kw->arg = NULL; kw->value = val; p2c_vec_push(call->base.u.call.keywords, kw); }
                }
                /* *args アンパック */
                else if (CURRENT(p) && CURRENT(p)->type == TOK_STAR) {
                    NEXT(p); /* '*' を消費 */
                    P2C_AstExpr *val = parse_expr(p, err);
                    if (!val) return NULL;
                    /* AST_STARRED ノードで包んで通常の位置引数スロットに入れる */
                    P2C_AstExpr *starred = p2c_ast_expr_new(p->alloc, AST_STARRED,
                        val->base.line, val->base.col);
                    if (!starred) return NULL;
                    starred->base.u.starred.value = val;
                    p2c_vec_push(call->base.u.call.args, starred);
                }
                /* キーワード引数チェック: IDENT '=' (ただし '==' ではない) */
                else if (CURRENT(p) && CURRENT(p)->type == TOK_IDENTIFIER &&
                    p2c_lexer_peek2(p->lexer) && p2c_lexer_peek2(p->lexer)->type == TOK_ASSIGN) {
                    P2C_Token *name_tok = CURRENT(p);
                    char *kw_name = p2c_alloc(p->alloc, name_tok->len + 1);
                    if (kw_name) memcpy(kw_name, name_tok->text, name_tok->len + 1);
                    NEXT(p); /* 識別子 */
                    NEXT(p); /* '=' */
                    P2C_AstExpr *val = parse_expr(p, err);
                    if (!val) return NULL;
                    P2C_AstKeyword *kw = p2c_alloc(p->alloc, sizeof(P2C_AstKeyword));
                    if (kw) { kw->arg = kw_name; kw->value = val; p2c_vec_push(call->base.u.call.keywords, kw); }
                } else {
                    P2C_AstExpr *arg = parse_expr(p, err);
                    if (!arg) return NULL;
                    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                        P2C_Vector *gens = parse_comprehension_generators(p, err);
                        P2C_AstExpr *genexp;
                        if (!gens) return NULL;
                        genexp = p2c_ast_expr_new(p->alloc, AST_GENERATOR_EXPRESSION, arg->base.line, arg->base.col);
                        if (!genexp) return NULL;
                        genexp->base.u.comprehension.elt = arg;
                        genexp->base.u.comprehension.generators = gens;
                        p2c_vec_push(call->base.u.call.args, genexp);
                        if (!CURRENT(p) || CURRENT(p)->type != TOK_RPAREN) {
                            set_error(p, "generator expression must be the only function argument unless parenthesized");
                            if (err) *err = P2C_ERR_SYNTAX;
                            return NULL;
                        }
                        break;
                    }
                    p2c_vec_push(call->base.u.call.args, arg);
                }
                
                if (!CONSUME(p, TOK_COMMA)) break;
                if (CURRENT(p) && CURRENT(p)->type == TOK_RPAREN) break;
            }
        }
        if (!EXPECT(p, TOK_RPAREN, err)) {
            set_error(p, "expected ')' after arguments");
            return NULL;
        }
        return call;
    }
    else if (CONSUME(p, TOK_LBRACKET)) {
        /* 添字アクセス、またはスライス (obj[start:stop:step]) */
        P2C_AstExpr *start = NULL, *stop = NULL, *step = NULL;
        bool is_slice = false;
        if (CURRENT(p) && CURRENT(p)->type != TOK_COLON && CURRENT(p)->type != TOK_RBRACKET) {
            start = parse_expr(p, err);
            if (!start) return NULL;
        }
        if (CURRENT(p) && CURRENT(p)->type == TOK_COLON) {
            is_slice = true;
            NEXT(p);
            if (CURRENT(p) && CURRENT(p)->type != TOK_COLON && CURRENT(p)->type != TOK_RBRACKET) {
                stop = parse_expr(p, err);
                if (!stop) return NULL;
            }
            if (CURRENT(p) && CURRENT(p)->type == TOK_COLON) {
                NEXT(p);
                if (CURRENT(p) && CURRENT(p)->type != TOK_RBRACKET) {
                    step = parse_expr(p, err);
                    if (!step) return NULL;
                }
            }
        }
        if (!EXPECT(p, TOK_RBRACKET, err)) {
            set_error(p, "expected ']'");
            return NULL;
        }
        if (is_slice) {
            P2C_AstExpr *call = p2c_ast_expr_new(p->alloc, AST_CALL, line, col);
            if (!call) return NULL;
            call->base.u.call.func = p2c_ast_name(p->alloc, "p2c_obj_slice", line, col);
            call->base.u.call.args = p2c_vec_new(p->alloc, NULL);
            p2c_vec_push(call->base.u.call.args, primary);
            p2c_vec_push(call->base.u.call.args, start ? start : ast_none_const(p, line, col));
            p2c_vec_push(call->base.u.call.args, stop ? stop : ast_none_const(p, line, col));
            p2c_vec_push(call->base.u.call.args, step ? step : ast_none_const(p, line, col));
            return call;
        }
        P2C_AstExpr *sub = p2c_ast_expr_new(p->alloc, AST_SUBSCRIPT, line, col);
        if (!sub) return NULL;
        sub->base.u.subscript.value = primary;
        sub->base.u.subscript.slice = start;
        if (!sub->base.u.subscript.slice) { set_error(p, "expected expression inside []"); return NULL; }
        return sub;
    }
    else if (CONSUME(p, TOK_DOT)) {
        /* 属性アクセス */
        P2C_Token *name_tok = CURRENT(p);
        if (!name_tok || name_tok->type != TOK_IDENTIFIER) {
            set_error(p, "expected attribute name after '.'");
            if (err) *err = P2C_ERR_SYNTAX;
            return NULL;
        }
        P2C_AstExpr *attr = p2c_ast_expr_new(p->alloc, AST_ATTRIBUTE, line, col);
        if (!attr) return NULL;
        attr->base.u.attribute.value = primary;
        attr->base.u.attribute.attr = p2c_alloc(p->alloc, name_tok->len + 1);
        if (attr->base.u.attribute.attr) memcpy(attr->base.u.attribute.attr, name_tok->text, name_tok->len + 1);
        NEXT(p);
        return attr;
    }
    return primary;
}

/* power: atom trailer* ['**' factor] */
static P2C_AstExpr* parse_power(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_atom(p, err);
    if (!left) return NULL;
    
    /* trailerを繰り返し */
    while (CURRENT(p) && (CURRENT(p)->type == TOK_LPAREN || 
                          CURRENT(p)->type == TOK_LBRACKET || 
                          CURRENT(p)->type == TOK_DOT)) {
        left = parse_trailer(p, left, err);
        if (!left) return NULL;
    }
    
    /* '**' factor */
    P2C_Token *tok = CURRENT(p);
    if (tok && tok->type == TOK_DBL_STAR) {
        uint32_t line = tok->line, col = tok->col;
        NEXT(p);
        P2C_AstExpr *right = parse_factor(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = OP_POW;
        e->base.u.binop.right = right;
        return e;
    }
    return left;
}

/* factor: ('+'|'-'|'~') factor | power */
static P2C_AstExpr* parse_factor(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    if (!tok) { if (err) *err = P2C_ERR_SYNTAX; return NULL; }
    
    if (tok->type == TOK_KW_AWAIT) {
        uint32_t line = tok->line, col = tok->col;
        NEXT(p);
        P2C_AstExpr *operand = parse_factor(p, err);
        if (!operand) return NULL;
        P2C_AstExpr *await_expr = p2c_ast_expr_new(p->alloc, AST_AWAIT, line, col);
        if (!await_expr) return NULL;
        await_expr->base.u.await_expr.value = operand;
        return await_expr;
    }
    if (tok->type == TOK_PLUS || tok->type == TOK_MINUS || tok->type == TOK_TILDE) {
        uint32_t line = tok->line, col = tok->col;
        P2C_AstOperator op = (tok->type == TOK_PLUS) ? OP_UADD : 
                             (tok->type == TOK_MINUS) ? OP_USUB : OP_INVERT;
        NEXT(p);
        P2C_AstExpr *operand = parse_factor(p, err);
        if (!operand) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_UNARYOP, line, col);
        if (!e) return NULL;
        e->base.u.unaryop.op = op;
        e->base.u.unaryop.operand = operand;
        return e;
    }
    return parse_power(p, err);
}

/* term: factor (('*'|'/'|'%'|'//'|'@') factor)* */
static P2C_AstExpr* parse_term(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_factor(p, err);
    if (!left) return NULL;
    
    while (CURRENT(p) && (CURRENT(p)->type == TOK_STAR || CURRENT(p)->type == TOK_SLASH ||
                          CURRENT(p)->type == TOK_PERCENT || CURRENT(p)->type == TOK_DBL_SLASH)) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        P2C_AstOperator op = token_to_binop(tok->type);
        NEXT(p);
        P2C_AstExpr *right = parse_factor(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = op;
        e->base.u.binop.right = right;
        left = e;
    }
    return left;
}

/* arith_expr: term (('+'|'-') term)* */
static P2C_AstExpr* parse_arith(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_term(p, err);
    if (!left) return NULL;
    
    while (CURRENT(p) && (CURRENT(p)->type == TOK_PLUS || CURRENT(p)->type == TOK_MINUS)) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        P2C_AstOperator op = (tok->type == TOK_PLUS) ? OP_ADD : OP_SUB;
        NEXT(p);
        P2C_AstExpr *right = parse_term(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = op;
        e->base.u.binop.right = right;
        left = e;
    }
    return left;
}

/* shift_expr: arith_expr (('<<'|'>>') arith_expr)* */
static P2C_AstExpr* parse_shift(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_arith(p, err);
    if (!left) return NULL;
    
    while (CURRENT(p) && (CURRENT(p)->type == TOK_LSHIFT || CURRENT(p)->type == TOK_RSHIFT)) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        P2C_AstOperator op = token_to_binop(tok->type);
        NEXT(p);
        P2C_AstExpr *right = parse_arith(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = op;
        e->base.u.binop.right = right;
        left = e;
    }
    return left;
}

/* and_expr: shift_expr ('&' shift_expr)* */
static P2C_AstExpr* parse_bitand(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_shift(p, err);
    if (!left) return NULL;
    
    while (CURRENT(p) && CURRENT(p)->type == TOK_AMPERSAND) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        NEXT(p);
        P2C_AstExpr *right = parse_shift(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = OP_BITAND;
        e->base.u.binop.right = right;
        left = e;
    }
    return left;
}

/* xor_expr: and_expr ('^' and_expr)* */
static P2C_AstExpr* parse_bitxor(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_bitand(p, err);
    if (!left) return NULL;
    
    while (CURRENT(p) && CURRENT(p)->type == TOK_CARET) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        NEXT(p);
        P2C_AstExpr *right = parse_bitand(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = OP_BITXOR;
        e->base.u.binop.right = right;
        left = e;
    }
    return left;
}

/* expr: xor_expr ('|' xor_expr)* */
static P2C_AstExpr* parse_bitor(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_bitxor(p, err);
    if (!left) return NULL;
    
    while (CURRENT(p) && CURRENT(p)->type == TOK_PIPE) {
        P2C_Token *tok = CURRENT(p);
        uint32_t line = tok->line, col = tok->col;
        NEXT(p);
        P2C_AstExpr *right = parse_bitxor(p, err);
        if (!right) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BINOP, line, col);
        if (!e) return NULL;
        e->base.u.binop.left = left;
        e->base.u.binop.op = OP_BITOR;
        e->base.u.binop.right = right;
        left = e;
    }
    return left;
}

/* comparison: expr (comp_op expr)* */
static P2C_AstExpr* parse_comparison(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_bitor(p, err);
    if (!left) return NULL;
    
    P2C_Vector *ops = NULL;
    P2C_Vector *cmps = NULL;
    uint32_t line = left->base.line, col = left->base.col;
    
    while (CURRENT(p) && (is_cmpop_token(CURRENT(p)->type) || 
                          CURRENT(p)->type == TOK_KW_IN ||
                          CURRENT(p)->type == TOK_KW_NOT ||
                          CURRENT(p)->type == TOK_KW_IS)) {
        P2C_Token *tok = CURRENT(p);
        P2C_AstOperator op;
        
        if (tok->type == TOK_KW_NOT && p2c_lexer_peek2(p->lexer) && 
            p2c_lexer_peek2(p->lexer)->type == TOK_KW_IN) {
            /* 'not in' 比較演算子: CURRENT(p) == tok == `not`, peek2 == `in`
             * 注意: CURRENT(p) と p2c_lexer_peek(p->lexer) は同じ（peek が
             * 現在のトークン）なので、次のトークンは必ず peek2 で取る。 */
            op = OP_NOTIN;
            NEXT(p); NEXT(p);
        } else if (tok->type == TOK_KW_NOT) {
            break; /* not_testの領域 */
        } else if (tok->type == TOK_KW_IS) {
            NEXT(p);
            if (CURRENT(p) && CURRENT(p)->type == TOK_KW_NOT) {
                op = OP_ISNOT;
                NEXT(p);
            } else {
                op = OP_IS;
            }
        } else if (tok->type == TOK_KW_IN) {
            op = OP_IN;
            NEXT(p);
        } else {
            op = token_to_cmpop(tok->type);
            NEXT(p);
        }
        
        if (!ops) {
            ops = p2c_vec_new(p->alloc, NULL);
            cmps = p2c_vec_new(p->alloc, NULL);
        }
        P2C_AstOperator *op_ptr = p2c_alloc(p->alloc, sizeof(P2C_AstOperator));
        *op_ptr = op;
        p2c_vec_push(ops, op_ptr);
        
        P2C_AstExpr *right = parse_bitor(p, err);
        if (!right) return NULL;
        p2c_vec_push(cmps, right);
    }
    
    if (ops) {
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_COMPARE, line, col);
        if (!e) return NULL;
        e->base.u.compare.left = left;
        e->base.u.compare.ops = ops;
        e->base.u.compare.comparators = cmps;
        return e;
    }
    return left;
}

/* not_test: 'not' not_test | comparison */
static P2C_AstExpr* parse_not_test(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    if (tok && tok->type == TOK_KW_NOT) {
        /* 'not in' は比較演算子（parse_comparison が担当）なので、
         * 'not' の次が 'in' であれば単項の 'not' として消費しない。
         * そのまま parse_comparison に落ちて 'not in' として処理させる。
         * 注意: ここに来た時点で 'not' の左辺は既に parse_comparison の
         * 1つ手前のレベルで解決されているはずなので、'not in' のケースは
         * 実際には parse_comparison の while ループが担当している。
         * ここで peek するのは、本当に単項 'not' なのかを確認するためだけ。 */
        P2C_Token *next = p2c_lexer_peek2(p->lexer);
        if (next && next->type == TOK_KW_IN) {
            /* 'not in' -> parse_comparison に任せる */
            return parse_comparison(p, err);
        }
        uint32_t line = tok->line, col = tok->col;
        NEXT(p);
        P2C_AstExpr *operand = parse_not_test(p, err);
        if (!operand) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_UNARYOP, line, col);
        if (!e) return NULL;
        e->base.u.unaryop.op = OP_NOT;
        e->base.u.unaryop.operand = operand;
        return e;
    }
    return parse_comparison(p, err);
}

/* and_test: not_test ('and' not_test)* */
static P2C_AstExpr* parse_and_test(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_not_test(p, err);
    if (!left) return NULL;
    
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_AND) {
        P2C_Vector *values = p2c_vec_new(p->alloc, NULL);
        p2c_vec_push(values, left);
        uint32_t line = left->base.line, col = left->base.col;
        
        while (CURRENT(p) && CURRENT(p)->type == TOK_KW_AND) {
            NEXT(p);
            P2C_AstExpr *right = parse_not_test(p, err);
            if (!right) return NULL;
            p2c_vec_push(values, right);
        }
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BOOLOP, line, col);
        if (!e) return NULL;
        e->base.u.boolop.op = OP_AND;
        e->base.u.boolop.values = values;
        return e;
    }
    return left;
}

/* or_test: and_test ('or' and_test)* */
static P2C_AstExpr* parse_or_test(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *left = parse_and_test(p, err);
    if (!left) return NULL;
    
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_OR) {
        P2C_Vector *values = p2c_vec_new(p->alloc, NULL);
        p2c_vec_push(values, left);
        uint32_t line = left->base.line, col = left->base.col;
        
        while (CURRENT(p) && CURRENT(p)->type == TOK_KW_OR) {
            NEXT(p);
            P2C_AstExpr *right = parse_and_test(p, err);
            if (!right) return NULL;
            p2c_vec_push(values, right);
        }
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_BOOLOP, line, col);
        if (!e) return NULL;
        e->base.u.boolop.op = OP_OR;
        e->base.u.boolop.values = values;
        return e;
    }
    return left;
}

/* conditional: or_test ['if' or_test 'else' conditional] */
static P2C_AstExpr* parse_conditional(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *body = parse_or_test(p, err);
    if (!body) return NULL;
    
    if (CONSUME(p, TOK_KW_IF)) {
        uint32_t line = body->base.line, col = body->base.col;
        P2C_AstExpr *test = parse_or_test(p, err);
        if (!test) return NULL;
        if (!EXPECT(p, TOK_KW_ELSE, err)) {
            set_error(p, "expected 'else' in conditional expression");
            return NULL;
        }
        P2C_AstExpr *orelse = parse_conditional(p, err);
        if (!orelse) return NULL;
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_IFEXP, line, col);
        if (!e) return NULL;
        e->base.u.ifexp.test = test;
        e->base.u.ifexp.body = body;
        e->base.u.ifexp.orelse = orelse;
        return e;
    }
    return body;
}

/* lambda */
static P2C_AstExpr* parse_lambda(P2C_Parser *p, P2C_Result *err) {
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_LAMBDA) {
        uint32_t line = CURRENT(p)->line, col = CURRENT(p)->col;
        NEXT(p);
        P2C_AstExpr *e = p2c_ast_expr_new(p->alloc, AST_LAMBDA, line, col);
        if (!e) return NULL;
        e->base.u.lambda.args = p2c_vec_new(p->alloc, NULL);
        
        /* 引数リスト（簡易実装：カンマ区切りの名前のみ） */
        while (CURRENT(p) && CURRENT(p)->type == TOK_IDENTIFIER) {
            P2C_AstArg *arg = p2c_alloc(p->alloc, sizeof(P2C_AstArg));
            P2C_Token *tok = CURRENT(p);
            arg->name = p2c_alloc(p->alloc, tok->len + 1);
            if (arg->name) memcpy(arg->name, tok->text, tok->len + 1);
            arg->annotation = NULL;
            arg->default_val = NULL;
            p2c_vec_push(e->base.u.lambda.args, arg);
            NEXT(p);
            if (!CONSUME(p, TOK_COMMA)) break;
        }
        
        if (!EXPECT(p, TOK_COLON, err)) {
            set_error(p, "expected ':' after lambda arguments");
            return NULL;
        }
        e->base.u.lambda.body = parse_expr(p, err);
        return e;
    }
    return parse_conditional(p, err);
}

/* 式エントリポイント */
static P2C_AstExpr* parse_expr(P2C_Parser *p, P2C_Result *err) {
    P2C_AstExpr *target = parse_lambda(p, err);
    if (!target) return NULL;
    if (!CONSUME(p, TOK_WALRUS)) return target;
    if (target->base.type != AST_NAME) {
        set_error(p, "assignment expression target must be a name");
        if (err) *err = P2C_ERR_SYNTAX;
        return NULL;
    }
    P2C_AstExpr *value = parse_expr(p, err);
    if (!value) return NULL;
    P2C_AstExpr *named = p2c_ast_expr_new(p->alloc, AST_NAMED_EXPR, target->base.line, target->base.col);
    if (!named) return NULL;
    named->base.u.named_expr.target = target;
    named->base.u.named_expr.value = value;
    return named;
}

/* ========================================
 * 文の構文解析
 * ======================================== */

/* suite: NEWLINE INDENT stmt+ DEDENT | simple_stmt */
static P2C_Vector* parse_suite(P2C_Parser *p, P2C_Result *err) {
    P2C_Vector *stmts = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    if (!stmts) return NULL;
    
    P2C_Token *tok = CURRENT(p);
    if (tok && tok->type == TOK_NEWLINE) {
        NEXT(p); /* NEWLINE */
        if (!EXPECT(p, TOK_INDENT, err)) {
            set_error(p, "expected indented block");
            p2c_vec_free(stmts);
            return NULL;
        }
        while (CURRENT(p) && CURRENT(p)->type != TOK_DEDENT && CURRENT(p)->type != TOK_EOF) {
            P2C_AstStmt *s = parse_stmt(p, err);
            if (!s) { p2c_vec_free(stmts); return NULL; }
            p2c_vec_push(stmts, s);
        }
        if (CURRENT(p) && CURRENT(p)->type == TOK_DEDENT) {
            NEXT(p);
        }
    } else {
        /* 単一行 */
        P2C_AstStmt *s = parse_simple_stmt(p, err);
        if (!s) { p2c_vec_free(stmts); return NULL; }
        p2c_vec_push(stmts, s);
    }
    return stmts;
}

/* simple_stmt: small_stmt (';' small_stmt)* [';'] NEWLINE */
static P2C_AstStmt* parse_simple_stmt(P2C_Parser *p, P2C_Result *err) {
    /* simple_stmt: small_stmt (';' small_stmt)* [';'] NEWLINE
     * セミコロン区切りで複数のsmall_stmtを1行に書ける（Python文法準拠）。
     * 複数ある場合は AST_BLOCK（stmtsリスト）でラップして返す。 */
    P2C_AstStmt *first = parse_assignment_or_expr(p, err);
    if (!first) return NULL;

    /* セミコロンがなければ従来通り単体で返す */
    if (!CURRENT(p) || CURRENT(p)->type != TOK_SEMICOLON) return first;

    /* セミコロンがある場合: 複数文をリストに集める */
    P2C_Vector *stmts = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    if (!stmts) return first;
    p2c_vec_push(stmts, first);

    while (CONSUME(p, TOK_SEMICOLON)) {
        /* 末尾セミコロン(行末・EOF前)は許容 */
        if (!CURRENT(p) || CURRENT(p)->type == TOK_NEWLINE || CURRENT(p)->type == TOK_EOF
            || CURRENT(p)->type == TOK_DEDENT) break;
        P2C_AstStmt *s = parse_assignment_or_expr(p, err);
        if (!s) { p2c_vec_free(stmts); return NULL; }
        p2c_vec_push(stmts, s);
    }

    /* AST_BLOCKノードで複数文をひとまとめにして返す */
    P2C_Token *tok = CURRENT(p);
    P2C_AstStmt *block = p2c_ast_stmt_new(p->alloc, AST_BLOCK, tok ? tok->line : 0, tok ? tok->col : 0);
    if (!block) { p2c_vec_free(stmts); return first; }
    block->base.u.block.stmts = stmts;
    return block;
}

static bool token_text_is(const P2C_Token *tok, const char *text) {
    return tok && tok->text && text && strcmp(tok->text, text) == 0;
}

static bool skip_type_parameter_list(P2C_Parser *p, P2C_Result *err) {
    int depth = 0;
    if (!CONSUME(p, TOK_LBRACKET)) return true;
    depth = 1;
    while (CURRENT(p) && depth > 0) {
        P2C_TokenType type = CURRENT(p)->type;
        if (type == TOK_LBRACKET) depth++;
        else if (type == TOK_RBRACKET) depth--;
        else if (type == TOK_NEWLINE || type == TOK_EOF) {
            set_error(p, "unterminated type parameter list");
            if (err) *err = P2C_ERR_SYNTAX;
            return false;
        }
        NEXT(p);
    }
    if (depth != 0) {
        set_error(p, "unterminated type parameter list");
        if (err) *err = P2C_ERR_SYNTAX;
        return false;
    }
    return true;
}

static P2C_AstStmt* parse_type_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok ? tok->line : 0;
    uint32_t col = tok ? tok->col : 0;
    NEXT(p);
    if (!EXPECT(p, TOK_IDENTIFIER, err)) {
        set_error(p, "expected type alias name after 'type'");
        return NULL;
    }
    if (!skip_type_parameter_list(p, err)) return NULL;
    if (!EXPECT(p, TOK_ASSIGN, err)) {
        set_error(p, "expected '=' in type statement");
        return NULL;
    }
    int depth = 0;
    while (CURRENT(p) && CURRENT(p)->type != TOK_EOF) {
        P2C_TokenType type = CURRENT(p)->type;
        if (depth == 0 && (type == TOK_NEWLINE || type == TOK_SEMICOLON)) break;
        if (type == TOK_LPAREN || type == TOK_LBRACKET || type == TOK_LBRACE) depth++;
        else if ((type == TOK_RPAREN || type == TOK_RBRACKET || type == TOK_RBRACE) && depth > 0) depth--;
        NEXT(p);
    }
    return p2c_ast_stmt_new(p->alloc, AST_PASS, line, col);
}

/* assignment_or_expr: target_list '=' ... | expr_stmt */
static P2C_AstStmt* parse_assignment_or_expr(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    if (!tok) { if (err) *err = P2C_ERR_SYNTAX; return NULL; }
    uint32_t line = tok->line, col = tok->col;

    if (tok->type == TOK_IDENTIFIER && token_text_is(tok, "type") &&
        p2c_lexer_peek2(p->lexer) && p2c_lexer_peek2(p->lexer)->type == TOK_IDENTIFIER) {
        return parse_type_stmt(p, err);
    }
    
    /* return文 */
    if (tok->type == TOK_KW_RETURN) {
        NEXT(p);
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_RETURN, line, col);
        if (!s) return NULL;
        if (CURRENT(p) && CURRENT(p)->type != TOK_NEWLINE && CURRENT(p)->type != TOK_EOF) {
            P2C_AstExpr *v0 = parse_expr(p, err);
            if (!v0) return NULL;
            if (CURRENT(p) && CURRENT(p)->type == TOK_COMMA) {
                /* return a, b のようなタプル戻り値 */
                P2C_Vector *elts = p2c_vec_new(p->alloc, NULL);
                if (!elts) return NULL;
                p2c_vec_push(elts, v0);
                while (CONSUME(p, TOK_COMMA)) {
                    if (!CURRENT(p) || CURRENT(p)->type == TOK_NEWLINE || CURRENT(p)->type == TOK_EOF) break;
                    P2C_AstExpr *item = parse_expr(p, err);
                    if (!item) { p2c_vec_free(elts); return NULL; }
                    p2c_vec_push(elts, item);
                }
                P2C_AstExpr *tup = p2c_ast_expr_new(p->alloc, AST_TUPLE, line, col);
                if (!tup) { p2c_vec_free(elts); return NULL; }
                tup->base.u.tuple.elts = elts;
                s->base.u.return_stmt.value = tup;
            } else {
                s->base.u.return_stmt.value = v0;
            }
        }
        return s;
    }
    
    /* pass */
    if (tok->type == TOK_KW_PASS) {
        NEXT(p);
        return p2c_ast_stmt_new(p->alloc, AST_PASS, line, col);
    }
    
    /* break */
    if (tok->type == TOK_KW_BREAK) {
        NEXT(p);
        return p2c_ast_stmt_new(p->alloc, AST_BREAK, line, col);
    }
    
    /* continue */
    if (tok->type == TOK_KW_CONTINUE) {
        NEXT(p);
        return p2c_ast_stmt_new(p->alloc, AST_CONTINUE, line, col);
    }
    
    /* global / nonlocal */
    if (tok->type == TOK_KW_GLOBAL || tok->type == TOK_KW_NONLOCAL) {
        P2C_AstType node_type = (tok->type == TOK_KW_GLOBAL) ? AST_GLOBAL : AST_NONLOCAL;
        NEXT(p);
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, node_type, line, col);
        if (!s) return NULL;
        s->base.u.global.names = p2c_vec_new(p->alloc, NULL);
        while (CURRENT(p) && CURRENT(p)->type == TOK_IDENTIFIER) {
            char *name = p2c_alloc(p->alloc, CURRENT(p)->len + 1);
            if (name) memcpy(name, CURRENT(p)->text, CURRENT(p)->len + 1);
            p2c_vec_push(s->base.u.global.names, name);
            NEXT(p);
            if (!CONSUME(p, TOK_COMMA)) break;
        }
        return s;
    }
    
    /* raise */
    if (tok->type == TOK_KW_RAISE) {
        NEXT(p);
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_RAISE, line, col);
        if (!s) return NULL;
        if (CURRENT(p) && CURRENT(p)->type != TOK_NEWLINE && CURRENT(p)->type != TOK_EOF) {
            s->base.u.raise.exc = parse_expr(p, err);
            if (CONSUME(p, TOK_KW_FROM)) {
                s->base.u.raise.cause = parse_expr(p, err);
            }
        }
        return s;
    }
    
    /* assert */
    if (tok->type == TOK_KW_ASSERT) {
        NEXT(p);
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_ASSERT, line, col);
        if (!s) return NULL;
        s->base.u.assert_stmt.test = parse_expr(p, err);
        if (!s->base.u.assert_stmt.test) return NULL;
        if (CONSUME(p, TOK_COMMA)) {
            s->base.u.assert_stmt.msg = parse_expr(p, err);
        }
        return s;
    }
    
    /* del */
    if (tok->type == TOK_KW_DEL) {
        NEXT(p);
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_DELETE, line, col);
        if (!s) return NULL;
        s->base.u.delete.targets = p2c_vec_new(p->alloc, NULL);
        while (1) {
            P2C_AstExpr *t = parse_expr(p, err);
            if (!t) return NULL;
            p2c_vec_push(s->base.u.delete.targets, t);
            if (!CONSUME(p, TOK_COMMA)) break;
        }
        return s;
    }
    
    /* 式を先にパースして代入か式文か判定 */
    P2C_AstExpr *first = NULL;
    if (CURRENT(p) && CURRENT(p)->type == TOK_STAR) {
        P2C_Token *star_tok = CURRENT(p);
        /* NEXT(p) は peek 済みトークンを消費する際に直前のトークンを解放する
         * （p2c_lexer_next）。そのため NEXT の後に star_tok を参照すると
         * 解放後参照になる（ASanが heap-use-after-free として検出した）。
         * 必要な値は進める前に控えておく。 */
        uint32_t star_line = star_tok->line;
        uint32_t star_col = star_tok->col;
        NEXT(p);
        P2C_AstExpr *value = parse_expr(p, err);
        if (!value) return NULL;
        first = p2c_ast_expr_new(p->alloc, AST_STARRED, star_line, star_col);
        if (!first) return NULL;
        first->base.u.starred.value = value;
    } else {
        first = parse_expr(p, err);
        if (!first) return NULL;
    }

    if (CONSUME(p, TOK_COLON)) {
        P2C_AstExpr *annotation = parse_expr(p, err);
        if (!annotation) return NULL;
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_ANNASSIGN, line, col);
        if (!s) return NULL;
        s->base.u.annassign.target = first;
        s->base.u.annassign.annotation = annotation;
        s->base.u.annassign.value = NULL;
        s->base.u.annassign.simple = first->base.type == AST_NAME ? 1 : 0;
        if (CONSUME(p, TOK_ASSIGN)) {
            s->base.u.annassign.value = parse_expr(p, err);
            if (!s->base.u.annassign.value) return NULL;
        }
        return s;
    }

    /* タプルアンパック代入 / 裸のタプル式:  a, b = 1, 2  /  a, b = some_tuple()  /  a, b
     * カンマが続く場合、複数のターゲット（またはRHS）をまとめてタプルとして扱う。 */
    if (CURRENT(p) && CURRENT(p)->type == TOK_COMMA) {
        P2C_Vector *elts = p2c_vec_new(p->alloc, NULL);
        if (!elts) return NULL;
        p2c_vec_push(elts, first);
        while (CONSUME(p, TOK_COMMA)) {
            if (!CURRENT(p) || CURRENT(p)->type == TOK_ASSIGN || CURRENT(p)->type == TOK_NEWLINE ||
                CURRENT(p)->type == TOK_EOF || CURRENT(p)->type == TOK_SEMICOLON) break; /* 末尾カンマ */
            P2C_AstExpr *item = NULL;
            if (CURRENT(p)->type == TOK_STAR) {
                uint32_t star_line = CURRENT(p)->line;
                uint32_t star_col = CURRENT(p)->col;
                NEXT(p);
                P2C_AstExpr *value = parse_expr(p, err);
                if (!value) { p2c_vec_free(elts); return NULL; }
                item = p2c_ast_expr_new(p->alloc, AST_STARRED, star_line, star_col);
                if (!item) { p2c_vec_free(elts); return NULL; }
                item->base.u.starred.value = value;
            } else {
                item = parse_expr(p, err);
                if (!item) { p2c_vec_free(elts); return NULL; }
            }
            p2c_vec_push(elts, item);
        }
        P2C_AstExpr *target_tuple = p2c_ast_expr_new(p->alloc, AST_TUPLE, line, col);
        if (!target_tuple) { p2c_vec_free(elts); return NULL; }
        target_tuple->base.u.tuple.elts = elts;

        if (CURRENT(p) && CURRENT(p)->type == TOK_ASSIGN) {
            NEXT(p);
            P2C_AstExpr *v0 = parse_expr(p, err);
            if (!v0) return NULL;
            P2C_AstExpr *value;
            if (CURRENT(p) && CURRENT(p)->type == TOK_COMMA) {
                P2C_Vector *velts = p2c_vec_new(p->alloc, NULL);
                if (!velts) return NULL;
                p2c_vec_push(velts, v0);
                while (CONSUME(p, TOK_COMMA)) {
                    if (!CURRENT(p) || CURRENT(p)->type == TOK_NEWLINE || CURRENT(p)->type == TOK_EOF || CURRENT(p)->type == TOK_SEMICOLON) break;
                    P2C_AstExpr *vitem = parse_expr(p, err);
                    if (!vitem) { p2c_vec_free(velts); return NULL; }
                    p2c_vec_push(velts, vitem);
                }
                P2C_AstExpr *value_tuple = p2c_ast_expr_new(p->alloc, AST_TUPLE, line, col);
                if (!value_tuple) { p2c_vec_free(velts); return NULL; }
                value_tuple->base.u.tuple.elts = velts;
                value = value_tuple;
            } else {
                value = v0; /* 右辺が単一式（例: 関数呼び出しがタプルを返す）の場合はそのまま展開対象にする */
            }
            P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_ASSIGN, line, col);
            if (!s) return NULL;
            s->base.u.assign.targets = p2c_vec_new(p->alloc, NULL);
            p2c_vec_push(s->base.u.assign.targets, target_tuple);
            s->base.u.assign.value = value;
            return s;
        }
        /* 代入でなければ裸のタプル式文として扱う */
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_EXPR_STMT, line, col);
        if (!s) return NULL;
        s->base.u.expr_stmt.value = target_tuple;
        return s;
    }

    /* 代入 */
    if (CURRENT(p) && CURRENT(p)->type == TOK_ASSIGN) {
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_ASSIGN, line, col);
        if (!s) return NULL;
        s->base.u.assign.targets = p2c_vec_new(p->alloc, NULL);
        p2c_vec_push(s->base.u.assign.targets, first);
        
        while (CONSUME(p, TOK_ASSIGN)) {
            /* 連鎖代入 */
            P2C_AstExpr *next = parse_expr(p, err);
            if (!next) return NULL;
            /* 最後の値が実際の値、それ以外はターゲット */
            if (CURRENT(p) && CURRENT(p)->type == TOK_ASSIGN) {
                p2c_vec_push(s->base.u.assign.targets, next);
            } else {
                s->base.u.assign.value = next;
            }
        }
        /* 単一代入の場合 */
        if (!s->base.u.assign.value) {
            /* 最後を値として取り出す */
            size_t n = p2c_vec_len(s->base.u.assign.targets);
            if (n >= 2) {
                s->base.u.assign.value = p2c_vec_pop(s->base.u.assign.targets);
            }
        }
        return s;
    }
    
    /* 複合代入 */
    if (CURRENT(p) && (CURRENT(p)->type == TOK_PLUS_ASSIGN || CURRENT(p)->type == TOK_MINUS_ASSIGN ||
                       CURRENT(p)->type == TOK_STAR_ASSIGN || CURRENT(p)->type == TOK_SLASH_ASSIGN ||
                       CURRENT(p)->type == TOK_DBL_SLASH_ASSIGN || CURRENT(p)->type == TOK_PERCENT_ASSIGN ||
                       CURRENT(p)->type == TOK_DBL_STAR_ASSIGN || CURRENT(p)->type == TOK_LSHIFT_ASSIGN ||
                       CURRENT(p)->type == TOK_RSHIFT_ASSIGN || CURRENT(p)->type == TOK_AMP_ASSIGN ||
                       CURRENT(p)->type == TOK_PIPE_ASSIGN || CURRENT(p)->type == TOK_CARET_ASSIGN)) {
        P2C_TokenType tt = CURRENT(p)->type;
        P2C_AstOperator op = token_to_binop(tt); /* 近似 */
        if (tt == TOK_PLUS_ASSIGN) op = OP_ADD;
        else if (tt == TOK_MINUS_ASSIGN) op = OP_SUB;
        else if (tt == TOK_STAR_ASSIGN) op = OP_MULT;
        else if (tt == TOK_SLASH_ASSIGN) op = OP_DIV;
        else if (tt == TOK_DBL_SLASH_ASSIGN) op = OP_FLOORDIV;
        else if (tt == TOK_PERCENT_ASSIGN) op = OP_MOD;
        else if (tt == TOK_DBL_STAR_ASSIGN) op = OP_POW;
        else if (tt == TOK_LSHIFT_ASSIGN) op = OP_LSHIFT;
        else if (tt == TOK_RSHIFT_ASSIGN) op = OP_RSHIFT;
        else if (tt == TOK_AMP_ASSIGN) op = OP_BITAND;
        else if (tt == TOK_PIPE_ASSIGN) op = OP_BITOR;
        else if (tt == TOK_CARET_ASSIGN) op = OP_BITXOR;
        
        NEXT(p);
        P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_AUGASSIGN, line, col);
        if (!s) return NULL;
        s->base.u.augassign.target = first;
        s->base.u.augassign.op = op;
        s->base.u.augassign.value = parse_expr(p, err);
        return s;
    }
    
    /* 式文 */
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_EXPR_STMT, line, col);
    if (s) s->base.u.expr_stmt.value = first;
    return s;
}

/* if_stmt */
static P2C_AstStmt* parse_if_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p); /* 'if' */
    
    P2C_AstExpr *test = parse_expr(p, err);
    if (!test) return NULL;
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after if condition");
        return NULL;
    }
    
    P2C_Vector *body = parse_suite(p, err);
    if (!body) return NULL;
    
    P2C_AstStmt *root = p2c_ast_stmt_new(p->alloc, AST_IF, line, col);
    if (!root) return NULL;
    root->base.u.if_stmt.test = test;
    root->base.u.if_stmt.body = body;
    root->base.u.if_stmt.orelse = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);

    /* curは「orelseがまだ確定していない、直近のifノード」を指す。
     * rootは呼び出し元へ返す本来のifノードとして常に保持する。 */
    P2C_AstStmt *cur = root;

    /* elif */
    while (CURRENT(p) && CURRENT(p)->type == TOK_KW_ELIF) {
        uint32_t elif_line = CURRENT(p)->line, elif_col = CURRENT(p)->col;
        NEXT(p);
        P2C_AstExpr *elif_test = parse_expr(p, err);
        if (!elif_test) return NULL;
        if (!EXPECT(p, TOK_COLON, err)) {
            set_error(p, "expected ':' after elif condition");
            return NULL;
        }
        P2C_Vector *elif_body = parse_suite(p, err);
        if (!elif_body) return NULL;

        /* elifを入れ子のifとしてorelseに格納 */
        P2C_AstStmt *elif_s = p2c_ast_stmt_new(p->alloc, AST_IF, elif_line, elif_col);
        if (!elif_s) return NULL;
        elif_s->base.u.if_stmt.test = elif_test;
        elif_s->base.u.if_stmt.body = elif_body;
        elif_s->base.u.if_stmt.orelse = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
        p2c_vec_push(cur->base.u.if_stmt.orelse, elif_s);
        /* 次のelif/elseは新しいifのorelseに入れる（rootはそのまま保持） */
        cur = elif_s;
    }

    /* else */
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_ELSE) {
        NEXT(p);
        if (!EXPECT(p, TOK_COLON, err)) {
            set_error(p, "expected ':' after else");
            return NULL;
        }
        P2C_Vector *else_body = parse_suite(p, err);
        if (!else_body) return NULL;
        /* 現在（最後のelif、なければroot）のorelseに設定 */
        p2c_vec_free(cur->base.u.if_stmt.orelse);
        cur->base.u.if_stmt.orelse = else_body;
    }

    return root;
}

/* while_stmt */
static P2C_AstStmt* parse_while_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p);
    
    P2C_AstExpr *test = parse_expr(p, err);
    if (!test) return NULL;
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after while condition");
        return NULL;
    }
    
    P2C_Vector *body = parse_suite(p, err);
    if (!body) return NULL;
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_WHILE, line, col);
    if (!s) return NULL;
    s->base.u.while_stmt.test = test;
    s->base.u.while_stmt.body = body;
    s->base.u.while_stmt.orelse = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_ELSE) {
        NEXT(p);
        if (!EXPECT(p, TOK_COLON, err)) return NULL;
        p2c_vec_free(s->base.u.while_stmt.orelse);
        s->base.u.while_stmt.orelse = parse_suite(p, err);
        if (!s->base.u.while_stmt.orelse) return NULL;
    }
    return s;
}

/* for_stmt */
static P2C_AstStmt* parse_for_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p);
    
    /* forのtargetは変数名、またはカンマ区切りの複数変数名（タプルアンパック）をサポート
     * （比較演算の'in'と衝突回避のため、式全体ではなく識別子の並びのみ受け付ける） */
    bool target_starred = CONSUME(p, TOK_STAR);
    P2C_Token *target_tok = EXPECT(p, TOK_IDENTIFIER, err);
    if (!target_tok) {
        set_error(p, "expected loop variable name in for statement");
        return NULL;
    }
    P2C_AstExpr *target = p2c_ast_name(p->alloc, target_tok->text, target_tok->line, target_tok->col);
    if (target_starred) {
        P2C_AstExpr *starred = p2c_ast_expr_new(p->alloc, AST_STARRED, target_tok->line, target_tok->col);
        if (!starred) return NULL;
        starred->base.u.starred.value = target;
        target = starred;
    }
    if (CURRENT(p) && CURRENT(p)->type == TOK_COMMA) {
        P2C_Vector *elts = p2c_vec_new(p->alloc, NULL);
        if (!elts) return NULL;
        p2c_vec_push(elts, target);
        while (CONSUME(p, TOK_COMMA)) {
            if (!CURRENT(p) || CURRENT(p)->type == TOK_KW_IN) break;
            bool more_starred = CONSUME(p, TOK_STAR);
            P2C_Token *more_tok = EXPECT(p, TOK_IDENTIFIER, err);
            if (!more_tok) { set_error(p, "expected loop variable name in for statement"); return NULL; }
            P2C_AstExpr *more = p2c_ast_name(p->alloc, more_tok->text, more_tok->line, more_tok->col);
            if (more_starred) {
                P2C_AstExpr *starred = p2c_ast_expr_new(p->alloc, AST_STARRED, more_tok->line, more_tok->col);
                if (!starred) return NULL;
                starred->base.u.starred.value = more;
                more = starred;
            }
            p2c_vec_push(elts, more);
        }
        P2C_AstExpr *tup = p2c_ast_expr_new(p->alloc, AST_TUPLE, line, col);
        if (!tup) return NULL;
        tup->base.u.tuple.elts = elts;
        target = tup;
    }
    if (!EXPECT(p, TOK_KW_IN, err)) {
        set_error(p, "expected 'in' in for statement");
        return NULL;
    }
    P2C_AstExpr *iter = parse_expr(p, err);
    if (!iter) return NULL;
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after for iterator");
        return NULL;
    }
    
    P2C_Vector *body = parse_suite(p, err);
    if (!body) return NULL;
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_FOR, line, col);
    if (!s) return NULL;
    s->base.u.for_stmt.target = target;
    s->base.u.for_stmt.iter = iter;
    s->base.u.for_stmt.body = body;
    s->base.u.for_stmt.orelse = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_ELSE) {
        NEXT(p);
        if (!EXPECT(p, TOK_COLON, err)) return NULL;
        p2c_vec_free(s->base.u.for_stmt.orelse);
        s->base.u.for_stmt.orelse = parse_suite(p, err);
        if (!s->base.u.for_stmt.orelse) return NULL;
    }
    return s;
}

/* try_stmt */
static P2C_AstStmt* parse_try_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p);
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after try");
        return NULL;
    }
    
    P2C_Vector *body = parse_suite(p, err);
    if (!body) return NULL;
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_TRY, line, col);
    if (!s) return NULL;
    s->base.u.try_stmt.body = body;
    s->base.u.try_stmt.handlers = p2c_vec_new(p->alloc, NULL);
    s->base.u.try_stmt.orelse = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    s->base.u.try_stmt.finalbody = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    
    /* except* */
    while (CURRENT(p) && CURRENT(p)->type == TOK_KW_EXCEPT) {
        NEXT(p);
        P2C_AstExceptHandler *h = p2c_alloc(p->alloc, sizeof(P2C_AstExceptHandler));
        memset(h, 0, sizeof(P2C_AstExceptHandler));
        
        if (CURRENT(p) && CURRENT(p)->type != TOK_COLON) {
            h->type = parse_expr(p, err);
            if (CONSUME(p, TOK_KW_AS)) {
                P2C_Token *name_tok = EXPECT(p, TOK_IDENTIFIER, err);
                if (name_tok) {
                    h->name = p2c_alloc(p->alloc, name_tok->len + 1);
                    if (h->name) memcpy(h->name, name_tok->text, name_tok->len + 1);
                }
            }
        }
        if (!EXPECT(p, TOK_COLON, err)) {
            set_error(p, "expected ':' after except clause");
            return NULL;
        }
        h->body = parse_suite(p, err);
        if (!h->body) return NULL;
        p2c_vec_push(s->base.u.try_stmt.handlers, h);
    }
    
    /* else */
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_ELSE) {
        NEXT(p);
        if (!EXPECT(p, TOK_COLON, err)) return NULL;
        p2c_vec_free(s->base.u.try_stmt.orelse);
        s->base.u.try_stmt.orelse = parse_suite(p, err);
        if (!s->base.u.try_stmt.orelse) return NULL;
    }
    
    /* finally */
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FINALLY) {
        NEXT(p);
        if (!EXPECT(p, TOK_COLON, err)) return NULL;
        p2c_vec_free(s->base.u.try_stmt.finalbody);
        s->base.u.try_stmt.finalbody = parse_suite(p, err);
        if (!s->base.u.try_stmt.finalbody) return NULL;
    }
    
    return s;
}

/* match_stmt */
static P2C_AstMatchPattern* match_pattern_new(P2C_Parser *p, P2C_MatchKind kind) {
    P2C_AstMatchPattern *pattern = p2c_alloc(p->alloc, sizeof(P2C_AstMatchPattern));
    if (!pattern) return NULL;
    memset(pattern, 0, sizeof(P2C_AstMatchPattern));
    pattern->kind = kind;
    return pattern;
}

static char* match_pattern_name_copy(P2C_Parser *p, P2C_Token *token) {
    char *name = p2c_alloc(p->alloc, token->len + 1);
    if (name) memcpy(name, token->text, token->len + 1);
    return name;
}

static bool match_pattern_has_binding(P2C_AstMatchPattern *pattern) {
    if (!pattern) return false;
    if (pattern->kind == P2C_MATCH_CAPTURE || pattern->kind == P2C_MATCH_STAR || pattern->kind == P2C_MATCH_AS) return true;
    if (pattern->children) {
        for (size_t i = 0; i < p2c_vec_len(pattern->children); i++) {
            if (match_pattern_has_binding((P2C_AstMatchPattern*)p2c_vec_get(pattern->children, i))) return true;
        }
    }
    return false;
}

static P2C_AstMatchPattern* parse_match_pattern(P2C_Parser *p, P2C_Result *err);

static P2C_AstMatchPattern* parse_match_pattern_atom(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *token = CURRENT(p);
    if (!token) return NULL;
    if (token->type == TOK_IDENTIFIER) {
        char *identifier_name = match_pattern_name_copy(p, token);
        if (!identifier_name) return NULL;
        NEXT(p);
        if (CURRENT(p) && CURRENT(p)->type == TOK_DOT) {
            P2C_AstExpr *value = p2c_ast_expr_new(p->alloc, AST_NAME, token->line, token->col);
            if (!value) return NULL;
            value->base.u.name.name = identifier_name;
            identifier_name = NULL;
            while (CONSUME(p, TOK_DOT)) {
                P2C_Token *attr_token = CURRENT(p);
                P2C_AstExpr *attribute;
                char *attr_name;
                uint32_t attr_line;
                uint32_t attr_col;
                if (!attr_token || attr_token->type != TOK_IDENTIFIER) return NULL;
                attr_line = attr_token->line;
                attr_col = attr_token->col;
                attr_name = match_pattern_name_copy(p, attr_token);
                if (!attr_name) return NULL;
                NEXT(p);
                attribute = p2c_ast_expr_new(p->alloc, AST_ATTRIBUTE, attr_line, attr_col);
                if (!attribute) return NULL;
                attribute->base.u.attribute.value = value;
                attribute->base.u.attribute.attr = attr_name;
                value = attribute;
            }
            if (CONSUME(p, TOK_LPAREN)) {
                P2C_AstMatchPattern *pattern = match_pattern_new(p, P2C_MATCH_CLASS);
                if (!pattern) return NULL;
                pattern->value = value;
                pattern->children = p2c_vec_new(p->alloc, NULL);
                pattern->attr_names = p2c_vec_new(p->alloc, NULL);
                if (!pattern->children || !pattern->attr_names) return NULL;
                if (CONSUME(p, TOK_RPAREN)) return pattern;
                while (1) {
                    P2C_Token *attr = CURRENT(p);
                    P2C_Token *after_attr = p2c_lexer_peek2(p->lexer);
                    P2C_AstMatchPattern *child;
                    char *attr_name = NULL;
                    if (attr && attr->type == TOK_IDENTIFIER && after_attr && after_attr->type == TOK_ASSIGN) {
                        attr_name = match_pattern_name_copy(p, attr);
                        if (!attr_name) return NULL;
                        NEXT(p);
                        if (!EXPECT(p, TOK_ASSIGN, err)) return NULL;
                    }
                    child = parse_match_pattern(p, err);
                    if (!child) return NULL;
                    p2c_vec_push(pattern->attr_names, attr_name);
                    p2c_vec_push(pattern->children, child);
                    if (!CONSUME(p, TOK_COMMA)) break;
                    if (CONSUME(p, TOK_RPAREN)) return pattern;
                }
                if (!EXPECT(p, TOK_RPAREN, err)) return NULL;
                return pattern;
            }
            {
                P2C_AstMatchPattern *pattern = match_pattern_new(p, P2C_MATCH_VALUE);
                if (!pattern) return NULL;
                pattern->value = value;
                return pattern;
            }
        }
        if (CONSUME(p, TOK_LPAREN)) {
            P2C_AstMatchPattern *pattern = match_pattern_new(p, P2C_MATCH_CLASS);
            if (!pattern) return NULL;
            pattern->class_name = identifier_name;
            pattern->children = p2c_vec_new(p->alloc, NULL);
            pattern->attr_names = p2c_vec_new(p->alloc, NULL);
            if (!pattern->class_name || !pattern->children || !pattern->attr_names) return NULL;
            if (CONSUME(p, TOK_RPAREN)) return pattern;
            while (1) {
                P2C_Token *attr = CURRENT(p);
                P2C_Token *after_attr = p2c_lexer_peek2(p->lexer);
                P2C_AstMatchPattern *child;
                char *attr_name = NULL;
                if (attr && attr->type == TOK_IDENTIFIER && after_attr && after_attr->type == TOK_ASSIGN) {
                    attr_name = match_pattern_name_copy(p, attr);
                    if (!attr_name) return NULL;
                    NEXT(p);
                    if (!EXPECT(p, TOK_ASSIGN, err)) return NULL;
                }
                child = parse_match_pattern(p, err);
                if (!child) return NULL;
                p2c_vec_push(pattern->attr_names, attr_name);
                p2c_vec_push(pattern->children, child);
                if (!CONSUME(p, TOK_COMMA)) break;
                if (CONSUME(p, TOK_RPAREN)) return pattern;
            }
            if (!EXPECT(p, TOK_RPAREN, err)) return NULL;
            return pattern;
        }
        {
            P2C_AstMatchPattern *pattern = match_pattern_new(p, (identifier_name[0] == '_' && identifier_name[1] == '\0') ? P2C_MATCH_WILDCARD : P2C_MATCH_CAPTURE);
            if (!pattern) return NULL;
            if (pattern->kind == P2C_MATCH_CAPTURE) pattern->capture_name = identifier_name;
            else p2c_free(p->alloc, identifier_name);
            return pattern;
        }
    }
    if (token->type == TOK_LBRACKET || token->type == TOK_LPAREN) {
        P2C_TokenType close = token->type == TOK_LBRACKET ? TOK_RBRACKET : TOK_RPAREN;
        bool parenthesized = token->type == TOK_LPAREN;
        NEXT(p);
        P2C_AstMatchPattern *pattern = match_pattern_new(p, P2C_MATCH_SEQUENCE);
        if (!pattern) return NULL;
        pattern->children = p2c_vec_new(p->alloc, NULL);
        if (!pattern->children) return NULL;
        if (!CONSUME(p, close)) {
            bool saw_comma = false;
            bool close_consumed = false;
            while (1) {
                P2C_AstMatchPattern *child;
                if (CONSUME(p, TOK_STAR)) {
                    P2C_Token *name = EXPECT(p, TOK_IDENTIFIER, err);
                    if (!name) return NULL;
                    child = match_pattern_new(p, P2C_MATCH_STAR);
                    if (!child) return NULL;
                    if (!(name->len == 1 && name->text[0] == '_')) child->capture_name = match_pattern_name_copy(p, name);
                } else {
                    child = parse_match_pattern(p, err);
                    if (!child) return NULL;
                }
                p2c_vec_push(pattern->children, child);
                if (!CONSUME(p, TOK_COMMA)) break;
                saw_comma = true;
                if (CONSUME(p, close)) { close_consumed = true; break; }
            }
            if (!saw_comma && parenthesized && p2c_vec_len(pattern->children) == 1) {
                P2C_AstMatchPattern *grouped = (P2C_AstMatchPattern*)p2c_vec_get(pattern->children, 0);
                pattern->children->len = 0;
                p2c_vec_free(pattern->children);
                p2c_free(p->alloc, pattern);
                if (!EXPECT(p, close, err)) return NULL;
                return grouped;
            }
            if (!close_consumed && !EXPECT(p, close, err)) return NULL;
        }
        return pattern;
    }
    if (token->type == TOK_LBRACE) {
        NEXT(p);
        P2C_AstMatchPattern *pattern = match_pattern_new(p, P2C_MATCH_MAPPING);
        if (!pattern) return NULL;
        pattern->keys = p2c_vec_new(p->alloc, NULL);
        pattern->children = p2c_vec_new(p->alloc, NULL);
        if (!pattern->keys || !pattern->children) return NULL;
        if (!CONSUME(p, TOK_RBRACE)) {
            while (1) {
                if (CONSUME(p, TOK_DBL_STAR)) {
                    P2C_Token *name = EXPECT(p, TOK_IDENTIFIER, err);
                    if (!name) return NULL;
                    pattern->rest_name = match_pattern_name_copy(p, name);
                    if (!pattern->rest_name) return NULL;
                    CONSUME(p, TOK_COMMA);
                    if (!EXPECT(p, TOK_RBRACE, err)) return NULL;
                    return pattern;
                }
                P2C_AstExpr *key = parse_factor(p, err);
                if (!key || key->base.type != AST_CONST) {
                    set_error(p, "mapping patterns currently require literal keys");
                    if (err) *err = P2C_ERR_SYNTAX;
                    return NULL;
                }
                if (!EXPECT(p, TOK_COLON, err)) return NULL;
                P2C_AstMatchPattern *child = parse_match_pattern(p, err);
                if (!child) return NULL;
                p2c_vec_push(pattern->keys, key);
                p2c_vec_push(pattern->children, child);
                if (!CONSUME(p, TOK_COMMA)) break;
                if (CONSUME(p, TOK_RBRACE)) return pattern;
            }
            if (!EXPECT(p, TOK_RBRACE, err)) return NULL;
        }
        return pattern;
    }
    {
        P2C_AstExpr *value = parse_factor(p, err);
        if (!value || value->base.type != AST_CONST) {
            set_error(p, "match patterns require literals, captures, sequence patterns, or mapping patterns");
            if (err) *err = P2C_ERR_SYNTAX;
            return NULL;
        }
        P2C_AstMatchPattern *pattern = match_pattern_new(p, P2C_MATCH_VALUE);
        if (!pattern) return NULL;
        pattern->value = value;
        return pattern;
    }
}

static P2C_AstMatchPattern* parse_match_pattern(P2C_Parser *p, P2C_Result *err) {
    P2C_AstMatchPattern *first = parse_match_pattern_atom(p, err);
    if (!first) return NULL;
    P2C_AstMatchPattern *result = first;
    if (CONSUME(p, TOK_PIPE)) {
        P2C_AstMatchPattern *or_pattern = match_pattern_new(p, P2C_MATCH_OR);
        if (!or_pattern) return NULL;
        or_pattern->children = p2c_vec_new(p->alloc, NULL);
        if (!or_pattern->children) return NULL;
        p2c_vec_push(or_pattern->children, first);
        do {
            P2C_AstMatchPattern *alternative = parse_match_pattern_atom(p, err);
            if (!alternative) return NULL;
            p2c_vec_push(or_pattern->children, alternative);
        } while (CONSUME(p, TOK_PIPE));
        for (size_t i = 0; i < p2c_vec_len(or_pattern->children); i++) {
            if (match_pattern_has_binding((P2C_AstMatchPattern*)p2c_vec_get(or_pattern->children, i))) {
                set_error(p, "or-pattern captures are not supported in the portable C11 backend");
                if (err) *err = P2C_ERR_SYNTAX;
                return NULL;
            }
        }
        result = or_pattern;
    }
    if (CONSUME(p, TOK_KW_AS)) {
        P2C_Token *name = EXPECT(p, TOK_IDENTIFIER, err);
        if (!name || (name->len == 1 && name->text[0] == '_')) {
            set_error(p, "expected a capture name after 'as' in match pattern");
            if (err) *err = P2C_ERR_SYNTAX;
            return NULL;
        }
        P2C_AstMatchPattern *as_pattern = match_pattern_new(p, P2C_MATCH_AS);
        if (!as_pattern) return NULL;
        as_pattern->capture_name = match_pattern_name_copy(p, name);
        as_pattern->children = p2c_vec_new(p->alloc, NULL);
        if (!as_pattern->children) return NULL;
        p2c_vec_push(as_pattern->children, result);
        result = as_pattern;
    }
    return result;
}

static P2C_AstStmt* parse_match_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p);
    P2C_AstExpr *subject = parse_expr(p, err);
    if (!subject) return NULL;
    if (!EXPECT(p, TOK_COLON, err) || !EXPECT(p, TOK_NEWLINE, err) || !EXPECT(p, TOK_INDENT, err)) {
        set_error(p, "expected an indented case block after match subject");
        return NULL;
    }
    P2C_AstStmt *stmt = p2c_ast_stmt_new(p->alloc, AST_MATCH, line, col);
    if (!stmt) return NULL;
    stmt->base.u.match_stmt.subject = subject;
    stmt->base.u.match_stmt.cases = p2c_vec_new(p->alloc, NULL);
    if (!stmt->base.u.match_stmt.cases) return NULL;
    while (CURRENT(p) && CURRENT(p)->type == TOK_KW_CASE) {
        NEXT(p);
        P2C_AstMatchCase *match_case = p2c_alloc(p->alloc, sizeof(P2C_AstMatchCase));
        if (!match_case) return NULL;
        memset(match_case, 0, sizeof(P2C_AstMatchCase));
        match_case->pattern = parse_match_pattern(p, err);
        if (!match_case->pattern) return NULL;
        if (CONSUME(p, TOK_KW_IF)) {
            match_case->guard = parse_expr(p, err);
            if (!match_case->guard) return NULL;
        }
        if (!EXPECT(p, TOK_COLON, err)) { set_error(p, "expected ':' after case pattern"); return NULL; }
        match_case->body = parse_suite(p, err);
        if (!match_case->body) return NULL;
        p2c_vec_push(stmt->base.u.match_stmt.cases, match_case);
    }
    if (p2c_vec_len(stmt->base.u.match_stmt.cases) == 0) { set_error(p, "match statement requires at least one case"); return NULL; }
    if (!EXPECT(p, TOK_DEDENT, err)) { set_error(p, "expected dedent after match block"); return NULL; }
    return stmt;
}

/* funcdef */
static P2C_AstStmt* parse_funcdef(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p); /* 'def' */
    
    P2C_Token *name_tok = EXPECT(p, TOK_IDENTIFIER, err);
    if (!name_tok) {
        set_error(p, "expected function name");
        return NULL;
    }
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_FUNCTIONDEF, line, col);
    if (!s) return NULL;
    s->base.u.functiondef.name = p2c_alloc(p->alloc, name_tok->len + 1);
    if (s->base.u.functiondef.name) memcpy(s->base.u.functiondef.name, name_tok->text, name_tok->len + 1);
    if (!skip_type_parameter_list(p, err)) return NULL;
    s->base.u.functiondef.args = p2c_vec_new(p->alloc, NULL);
    s->base.u.functiondef.decorator_list = p2c_vec_new(p->alloc, NULL);
    s->base.u.functiondef.returns = NULL;
    s->base.u.functiondef.vararg = NULL;
    s->base.u.functiondef.kwarg = NULL;
    s->base.u.functiondef.posonly_count = 0;
    s->base.u.functiondef.is_async = false;
    
    /* 引数リスト */
    if (!EXPECT(p, TOK_LPAREN, err)) {
        set_error(p, "expected '(' after function name");
        return NULL;
    }
    {
        int kwonly_start = -1;
        int posonly_count = 0;
        bool seen_slash = false;
        bool seen_star = false;
        bool seen_kwarg = false;
        if (CURRENT(p) && CURRENT(p)->type != TOK_RPAREN) {
            while (1) {
                if (CURRENT(p) && CURRENT(p)->type == TOK_RPAREN) break; /* 末尾カンマの後に ) が来た場合 */
                if (seen_kwarg) {
                    set_error(p, "arguments cannot follow **kwargs");
                    if (err) *err = P2C_ERR_SYNTAX;
                    return NULL;
                }
                if (CURRENT(p) && CURRENT(p)->type == TOK_SLASH) {
                    if (seen_slash || seen_star || p2c_vec_len(s->base.u.functiondef.args) == 0) {
                        set_error(p, "invalid positional-only parameter separator '/'");
                        if (err) *err = P2C_ERR_SYNTAX;
                        return NULL;
                    }
                    NEXT(p);
                    seen_slash = true;
                    posonly_count = (int)p2c_vec_len(s->base.u.functiondef.args);
                    if (!CONSUME(p, TOK_COMMA) && (!CURRENT(p) || CURRENT(p)->type != TOK_RPAREN)) {
                        set_error(p, "expected ',' or ')' after positional-only separator '/'");
                        if (err) *err = P2C_ERR_SYNTAX;
                        return NULL;
                    }
                    continue;
                }
                if (CURRENT(p) && CURRENT(p)->type == TOK_DBL_STAR) {
                    NEXT(p);
                    P2C_Token *kw_tok = EXPECT(p, TOK_IDENTIFIER, err);
                    if (!kw_tok) {
                        set_error(p, "expected parameter name after '**'");
                        return NULL;
                    }
                    s->base.u.functiondef.kwarg = p2c_alloc(p->alloc, kw_tok->len + 1);
                    if (s->base.u.functiondef.kwarg) memcpy(s->base.u.functiondef.kwarg, kw_tok->text, kw_tok->len + 1);
                    if (CONSUME(p, TOK_COLON)) {
                        parse_expr(p, err);
                    }
                    seen_kwarg = true;
                    if (!CONSUME(p, TOK_COMMA)) break;
                    continue;
                }
                if (CURRENT(p) && CURRENT(p)->type == TOK_STAR) {
                    if (seen_star) {
                        set_error(p, "duplicate * in parameter list");
                        if (err) *err = P2C_ERR_SYNTAX;
                        return NULL;
                    }
                    NEXT(p);
                    if (CURRENT(p) && CURRENT(p)->type == TOK_IDENTIFIER) {
                        P2C_Token *va_tok = EXPECT(p, TOK_IDENTIFIER, err);
                        if (!va_tok) {
                            set_error(p, "expected parameter name after '*'");
                            return NULL;
                        }
                        s->base.u.functiondef.vararg = p2c_alloc(p->alloc, va_tok->len + 1);
                        if (s->base.u.functiondef.vararg) memcpy(s->base.u.functiondef.vararg, va_tok->text, va_tok->len + 1);
                        if (CONSUME(p, TOK_COLON)) {
                            parse_expr(p, err);
                        }
                    } else if (!(CURRENT(p) && CURRENT(p)->type == TOK_COMMA)) {
                        set_error(p, "named arguments must follow bare *");
                        if (err) *err = P2C_ERR_SYNTAX;
                        return NULL;
                    }
                    seen_star = true;
                    kwonly_start = (int)p2c_vec_len(s->base.u.functiondef.args);
                    if (!CONSUME(p, TOK_COMMA)) break;
                    continue;
                }
                P2C_Token *arg_tok = EXPECT(p, TOK_IDENTIFIER, err);
                if (!arg_tok) {
                    set_error(p, "expected parameter name");
                    return NULL;
                }
                P2C_AstArg *arg = p2c_alloc(p->alloc, sizeof(P2C_AstArg));
                arg->name = p2c_alloc(p->alloc, arg_tok->len + 1);
                if (arg->name) memcpy(arg->name, arg_tok->text, arg_tok->len + 1);
                arg->annotation = NULL;
                arg->default_val = NULL;
                
                if (CONSUME(p, TOK_COLON)) {
                    arg->annotation = (P2C_AstExpr*)parse_expr(p, err);
                }
                if (CONSUME(p, TOK_ASSIGN)) {
                    arg->default_val = (P2C_AstExpr*)parse_expr(p, err);
                }
                p2c_vec_push(s->base.u.functiondef.args, arg);
                if (!CONSUME(p, TOK_COMMA)) break;
            }
        }
        s->base.u.functiondef.kwonly_start = (kwonly_start >= 0) ? kwonly_start : (int)p2c_vec_len(s->base.u.functiondef.args);
        s->base.u.functiondef.posonly_count = posonly_count;
    }
    if (!EXPECT(p, TOK_RPAREN, err)) {
        set_error(p, "expected ')' after parameters");
        return NULL;
    }
    
    /* 戻り値型注釈 */
    if (CONSUME(p, TOK_ARROW)) {
        s->base.u.functiondef.returns = (P2C_AstExpr*)parse_expr(p, err);
    }
    
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after function definition");
        return NULL;
    }
    
    s->base.u.functiondef.body = parse_suite(p, err);
    if (!s->base.u.functiondef.body) return NULL;
    return s;
}

/* classdef */
static P2C_AstStmt* parse_classdef(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p); /* 'class' */
    
    P2C_Token *name_tok = EXPECT(p, TOK_IDENTIFIER, err);
    if (!name_tok) {
        set_error(p, "expected class name");
        return NULL;
    }
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_CLASSDEF, line, col);
    if (!s) return NULL;
    s->base.u.classdef.name = p2c_alloc(p->alloc, name_tok->len + 1);
    if (s->base.u.classdef.name) memcpy(s->base.u.classdef.name, name_tok->text, name_tok->len + 1);
    if (!skip_type_parameter_list(p, err)) return NULL;
    s->base.u.classdef.bases = p2c_vec_new(p->alloc, NULL);
    s->base.u.classdef.keywords = p2c_vec_new(p->alloc, NULL);
    s->base.u.classdef.decorator_list = p2c_vec_new(p->alloc, NULL);
    
    /* 継承リスト */
    if (CONSUME(p, TOK_LPAREN)) {
        if (CURRENT(p) && CURRENT(p)->type != TOK_RPAREN) {
            while (1) {
                P2C_AstExpr *base = parse_expr(p, err);
                if (!base) return NULL;
                p2c_vec_push(s->base.u.classdef.bases, base);
                if (!CONSUME(p, TOK_COMMA)) break;
            }
        }
        if (!EXPECT(p, TOK_RPAREN, err)) {
            set_error(p, "expected ')' after base classes");
            return NULL;
        }
    }
    
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after class definition");
        return NULL;
    }
    
    s->base.u.classdef.body = parse_suite(p, err);
    if (!s->base.u.classdef.body) return NULL;
    return s;
}

/* import */
static P2C_AstStmt* parse_import(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p);
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_IMPORT, line, col);
    if (!s) return NULL;
    s->base.u.import_stmt.names = p2c_vec_new(p->alloc, NULL);
    
    while (1) {
        P2C_Token *name_tok = EXPECT(p, TOK_IDENTIFIER, err);
        if (!name_tok) {
            set_error(p, "expected module name in import");
            return NULL;
        }
        P2C_AstAlias *al = p2c_alloc(p->alloc, sizeof(P2C_AstAlias));
        al->name = p2c_alloc(p->alloc, name_tok->len + 1);
        if (al->name) memcpy(al->name, name_tok->text, name_tok->len + 1);
        al->asname = NULL;
        
        if (CONSUME(p, TOK_KW_AS)) {
            P2C_Token *as_tok = EXPECT(p, TOK_IDENTIFIER, err);
            if (as_tok) {
                al->asname = p2c_alloc(p->alloc, as_tok->len + 1);
                if (al->asname) memcpy(al->asname, as_tok->text, as_tok->len + 1);
            }
        }
        p2c_vec_push(s->base.u.import_stmt.names, al);
        if (!CONSUME(p, TOK_COMMA)) break;
    }
    return s;
}

/* from ... import */
static P2C_AstStmt* parse_import_from(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p); /* 'from' */
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_IMPORTFROM, line, col);
    if (!s) return NULL;
    s->base.u.importfrom.level = 0;
    
    /* モジュール名 */
    P2C_String *mod_str = p2c_str_new(p->alloc);
    int dot_count = 0;
    while (CONSUME(p, TOK_DOT)) dot_count++;
    s->base.u.importfrom.level = dot_count;
    
    if (CURRENT(p) && CURRENT(p)->type == TOK_IDENTIFIER) {
        while (1) {
            P2C_Token *name_tok = CURRENT(p);
            p2c_str_append_n(mod_str, name_tok->text, name_tok->len);
            NEXT(p);
            if (CONSUME(p, TOK_DOT)) {
                p2c_str_append(mod_str, ".");
            } else {
                break;
            }
        }
        s->base.u.importfrom.module = p2c_alloc(p->alloc, p2c_str_len(mod_str) + 1);
        if (s->base.u.importfrom.module) memcpy(s->base.u.importfrom.module, p2c_str_cstr(mod_str), p2c_str_len(mod_str) + 1);
    }
    p2c_str_free(mod_str);
    
    if (!EXPECT(p, TOK_KW_IMPORT, err)) {
        set_error(p, "expected 'import' in from statement");
        return NULL;
    }
    
    s->base.u.importfrom.names = p2c_vec_new(p->alloc, NULL);
    if (CONSUME(p, TOK_STAR)) {
        P2C_AstAlias *al = p2c_alloc(p->alloc, sizeof(P2C_AstAlias));
        al->name = p2c_alloc(p->alloc, 2);
        if (al->name) strcpy(al->name, "*");
        al->asname = NULL;
        p2c_vec_push(s->base.u.importfrom.names, al);
    } else {
        while (1) {
            P2C_Token *name_tok = EXPECT(p, TOK_IDENTIFIER, err);
            if (!name_tok) {
                set_error(p, "expected name in import");
                return NULL;
            }
            P2C_AstAlias *al = p2c_alloc(p->alloc, sizeof(P2C_AstAlias));
            al->name = p2c_alloc(p->alloc, name_tok->len + 1);
            if (al->name) memcpy(al->name, name_tok->text, name_tok->len + 1);
            al->asname = NULL;
            if (CONSUME(p, TOK_KW_AS)) {
                P2C_Token *as_tok = EXPECT(p, TOK_IDENTIFIER, err);
                if (as_tok) {
                    al->asname = p2c_alloc(p->alloc, as_tok->len + 1);
                    if (al->asname) memcpy(al->asname, as_tok->text, as_tok->len + 1);
                }
            }
            p2c_vec_push(s->base.u.importfrom.names, al);
            if (!CONSUME(p, TOK_COMMA)) break;
        }
    }
    return s;
}

/* with_stmt */
static P2C_AstStmt* parse_with_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    uint32_t line = tok->line, col = tok->col;
    NEXT(p);
    
    P2C_AstStmt *s = p2c_ast_stmt_new(p->alloc, AST_WITH, line, col);
    if (!s) return NULL;
    s->base.u.with.items = p2c_vec_new(p->alloc, NULL);
    s->base.u.with.body = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    s->base.u.with.is_async = false;
    bool grouped_items = CONSUME(p, TOK_LPAREN);
    if (grouped_items) {
        while (CURRENT(p) && (CURRENT(p)->type == TOK_NEWLINE || CURRENT(p)->type == TOK_INDENT || CURRENT(p)->type == TOK_DEDENT)) NEXT(p);
    }
    
    while (1) {
        P2C_AstExpr *ctx = parse_expr(p, err);
        if (!ctx) return NULL;
        P2C_AstWithItem *item = p2c_alloc(p->alloc, sizeof(P2C_AstWithItem));
        item->context_expr = ctx;
        item->optional_vars = NULL;
        if (CONSUME(p, TOK_KW_AS)) {
            item->optional_vars = parse_expr(p, err);
        }
        p2c_vec_push(s->base.u.with.items, item);
        if (!CONSUME(p, TOK_COMMA)) break;
        if (grouped_items) {
            while (CURRENT(p) && (CURRENT(p)->type == TOK_NEWLINE || CURRENT(p)->type == TOK_INDENT || CURRENT(p)->type == TOK_DEDENT)) NEXT(p);
            if (CURRENT(p) && CURRENT(p)->type == TOK_RPAREN) break;
        }
    }
    if (grouped_items) {
        while (CURRENT(p) && (CURRENT(p)->type == TOK_NEWLINE || CURRENT(p)->type == TOK_INDENT || CURRENT(p)->type == TOK_DEDENT)) NEXT(p);
    }
    if (grouped_items && !EXPECT(p, TOK_RPAREN, err)) {
        set_error(p, "expected ')' after with items");
        return NULL;
    }
    
    if (!EXPECT(p, TOK_COLON, err)) {
        set_error(p, "expected ':' after with items");
        return NULL;
    }
    s->base.u.with.body = parse_suite(p, err);
    if (!s->base.u.with.body) return NULL;
    return s;
}

static P2C_AstStmt* parse_decorated_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Vector *decorators = p2c_vec_new(p->alloc, NULL);
    P2C_AstStmt *stmt = NULL;
    if (!decorators) { if (err) *err = P2C_ERR_NOMEM; return NULL; }
    while (CURRENT(p) && CURRENT(p)->type == TOK_AT) {
        NEXT(p);
        P2C_AstExpr *decorator = parse_expr(p, err);
        if (!decorator) return NULL;
        p2c_vec_push(decorators, decorator);
        if (!EXPECT(p, TOK_NEWLINE, err)) {
            set_error(p, "expected newline after decorator expression");
            return NULL;
        }
    }
    if (CURRENT(p) && CURRENT(p)->type == TOK_KW_ASYNC) {
        NEXT(p);
        if (!CURRENT(p) || CURRENT(p)->type != TOK_KW_DEF) {
            set_error(p, "decorators may only be applied to function or class definitions");
            if (err) *err = P2C_ERR_SYNTAX;
            return NULL;
        }
        stmt = parse_funcdef(p, err);
        if (stmt) stmt->base.u.functiondef.is_async = true;
    } else if (CURRENT(p) && CURRENT(p)->type == TOK_KW_DEF) {
        stmt = parse_funcdef(p, err);
    } else if (CURRENT(p) && CURRENT(p)->type == TOK_KW_CLASS) {
        stmt = parse_classdef(p, err);
    } else {
        set_error(p, "decorators may only be applied to function or class definitions");
        if (err) *err = P2C_ERR_SYNTAX;
        return NULL;
    }
    if (!stmt) return NULL;
    if (stmt->base.type == AST_FUNCTIONDEF) {
        p2c_vec_free(stmt->base.u.functiondef.decorator_list);
        stmt->base.u.functiondef.decorator_list = decorators;
    } else {
        p2c_vec_free(stmt->base.u.classdef.decorator_list);
        stmt->base.u.classdef.decorator_list = decorators;
    }
    return stmt;
}

/* compound_stmt: if | while | for | try | funcdef | classdef | with */
static P2C_AstStmt* parse_compound_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_Token *tok = CURRENT(p);
    if (!tok) { if (err) *err = P2C_ERR_SYNTAX; return NULL; }

    if (tok->type == TOK_AT) return parse_decorated_stmt(p, err);
    
    switch (tok->type) {
        case TOK_KW_IF: return parse_if_stmt(p, err);
        case TOK_KW_WHILE: return parse_while_stmt(p, err);
        case TOK_KW_FOR: return parse_for_stmt(p, err);
        case TOK_KW_TRY: return parse_try_stmt(p, err);
        case TOK_KW_MATCH: return parse_match_stmt(p, err);
        case TOK_KW_ASYNC: {
            NEXT(p);
            if (CURRENT(p) && CURRENT(p)->type == TOK_KW_FOR) {
                P2C_AstStmt *async_for = parse_for_stmt(p, err);
                if (async_for) async_for->base.type = AST_ASYNC_FOR;
                return async_for;
            }
            if (CURRENT(p) && CURRENT(p)->type == TOK_KW_WITH) {
                P2C_AstStmt *async_with = parse_with_stmt(p, err);
                if (async_with) async_with->base.u.with.is_async = true;
                return async_with;
            }
            if (!CURRENT(p) || CURRENT(p)->type != TOK_KW_DEF) {
                set_error(p, "expected 'def', 'for', or 'with' after 'async'");
                if (err) *err = P2C_ERR_SYNTAX;
                return NULL;
            }
            P2C_AstStmt *async_def = parse_funcdef(p, err);
            if (async_def) async_def->base.u.functiondef.is_async = true;
            return async_def;
        }
        case TOK_KW_DEF: return parse_funcdef(p, err);
        case TOK_KW_CLASS: return parse_classdef(p, err);
        case TOK_KW_WITH: return parse_with_stmt(p, err);
        default: break;
    }
    
    /* importは文として処理 */
    if (tok->type == TOK_KW_IMPORT) return parse_import(p, err);
    if (tok->type == TOK_KW_FROM) return parse_import_from(p, err);
    
    return NULL;
}

/* stmt: simple_stmt | compound_stmt */
static P2C_AstStmt* parse_stmt(P2C_Parser *p, P2C_Result *err) {
    P2C_AstStmt *s = parse_compound_stmt(p, err);
    if (s) {
        /* compound_stmtの後にNEWLINEがあれば消費 */
        P2C_Token *tok = CURRENT(p);
        if (tok && tok->type == TOK_NEWLINE) {
            NEXT(p);
        }
        return s;
    }
    /* compoundでない場合はsimple_stmt */
    if (err && *err != P2C_OK) return NULL;
    s = parse_simple_stmt(p, err);
    /* simple_stmtの後にNEWLINEがあれば消費 */
    if (s) {
        P2C_Token *tok = CURRENT(p);
        if (tok && tok->type == TOK_NEWLINE) {
            NEXT(p);
        }
    }
    return s;
}

/* ========================================
 * モジュールパース（エントリポイント）
 * ======================================== */

P2C_AstModule* p2c_parser_parse_module(P2C_Parser *p, P2C_Result *out_err) {
    if (!p) { if (out_err) *out_err = P2C_ERR_INTERNAL; return NULL; }
    
    P2C_AstStmt *mod_stmt = p2c_ast_stmt_new(p->alloc, AST_MODULE, 1, 1);
    if (!mod_stmt) { if (out_err) *out_err = P2C_ERR_NOMEM; return NULL; }
    
    mod_stmt->base.u.module.body = p2c_vec_new(p->alloc, (P2C_VectorFreeFn)p2c_ast_stmt_free);
    
    while (!is_at_end(p)) {
        /* 先頭のNEWLINE/INDENT/DEDENTをスキップ */
        if (CURRENT(p) && (CURRENT(p)->type == TOK_NEWLINE || 
                           CURRENT(p)->type == TOK_INDENT ||
                           CURRENT(p)->type == TOK_DEDENT)) {
            NEXT(p);
            continue;
        }
        if (is_at_end(p)) break;
        
        P2C_AstStmt *s = parse_stmt(p, out_err);
        if (!s) {
            if (out_err && *out_err == P2C_OK) *out_err = P2C_ERR_SYNTAX;
            return NULL;
        }
        p2c_vec_push(mod_stmt->base.u.module.body, s);
    }
    
    if (out_err) *out_err = P2C_OK;
    return (P2C_AstModule*)mod_stmt;
}

/* 公開APIのラッパー */
P2C_AstStmt* p2c_parser_stmt(P2C_Parser *par, P2C_Result *out_err) {
    return parse_stmt(par, out_err);
}

P2C_AstExpr* p2c_parser_expr(P2C_Parser *par, P2C_Result *out_err) {
    return parse_expr(par, out_err);
}
