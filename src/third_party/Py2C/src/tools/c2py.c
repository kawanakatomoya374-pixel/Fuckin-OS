/*
 * c2py.c - 簡易 C -> Python 変換器
 *
 * python_code_to_c(Python->C)とは非対称に、こちらはトークンベースの
 * パターン変換器です。C言語の型システムやポインタ意味論を
 * 完全に再現することは意図的にスコープ外にしており、
 * 「読める・書き換えやすいPythonのたたき台を作る」ことを目標にしています。
 * 対応範囲はinclude/c2py.hのコメントを参照してください。
 *
 * 依存関係: 標準Cライブラリのみ。他のpython_code_to_cソースには依存しないため、
 * 単体でも組み込みやすい構成にしています。
 */
#include "tools/c2py.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

/* ---------- 確保ラッパ ----------
 * c2py は単体の小さな変換ツールなので、メモリ不足は「黙ってNULLを返して
 * 後でクラッシュ」ではなく、その場で明示的に終了する。以前は malloc/realloc の
 * 戻り値を確認せずに書き込んでおり、静的解析（-fanalyzer）が
 * CWE-690（NULL参照）/CWE-476/CWE-401（realloc失敗時の旧領域リーク）を
 * 7件報告していた。 */
static void c2py_oom(void) {
    fputs("c2py: out of memory\n", stderr);
    exit(1);
}
static void *c2py_alloc(size_t size) {
    void *p = malloc(size ? size : 1u);
    if (!p) c2py_oom();
    return p;
}
static void *c2py_realloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size ? size : 1u);
    if (!p) c2py_oom();
    return p;
}

/* ---------- 出力バッファ ---------- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} OutBuf;

static void ob_init(OutBuf *b) { b->data = (char*)c2py_alloc(1); b->data[0] = '\0'; b->len = 0; b->cap = 1; }
static void ob_reserve(OutBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t new_cap = b->cap * 2;
    while (new_cap < b->len + extra + 1) new_cap *= 2;
    b->data = (char*)c2py_realloc(b->data, new_cap);
    b->cap = new_cap;
}
static void ob_append(OutBuf *b, const char *s) {
    size_t l = strlen(s);
    ob_reserve(b, l);
    memcpy(b->data + b->len, s, l + 1);
    b->len += l;
}
static void ob_append_ch(OutBuf *b, char c) { char s[2] = {c, 0}; ob_append(b, s); }
static void ob_indent(OutBuf *b, int level) { for (int i = 0; i < level; i++) ob_append(b, "    "); }

/* ---------- トークナイザ ---------- */
typedef enum { T_IDENT, T_NUMBER, T_STRING, T_CHAR, T_PUNCT, T_END } TokType;

typedef struct {
    TokType type;
    char *text; /* malloc済み */
} Token;

typedef struct {
    Token *toks;
    size_t n;
    size_t cap;
} TokVec;

static void tv_init(TokVec *v) { v->toks = NULL; v->n = 0; v->cap = 0; }
static void tv_push(TokVec *v, TokType t, const char *start, size_t len) {
    if (v->n >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->toks = (Token*)c2py_realloc(v->toks, v->cap * sizeof(Token));
    }
    char *s = (char*)c2py_alloc(len + 1);
    memcpy(s, start, len);
    s[len] = '\0';
    v->toks[v->n].type = t;
    v->toks[v->n].text = s;
    v->n++;
}

static bool is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static const char *MULTI_PUNCT[] = {
    "<<=", ">>=", "...", "->", "++", "--", "&&", "||", "==", "!=", "<=", ">=",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "::", NULL
};

static void tokenize(const char *src, TokVec *out) {
    size_t i = 0, n = strlen(src);
    while (i < n) {
        char c = src[i];
        if (isspace((unsigned char)c)) { i++; continue; }
        /* コメント */
        if (c == '/' && i + 1 < n && src[i+1] == '/') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && src[i+1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i+1] == '/')) i++;
            i += 2;
            continue;
        }
        /* プリプロセッサ行はまるごとスキップしてコメント扱い */
        if (c == '#') {
            size_t start = i;
            while (i < n && src[i] != '\n') {
                /* 行末バックスラッシュで継続する行にも対応 */
                if (src[i] == '\\' && i + 1 < n && src[i+1] == '\n') i += 2; else i++;
            }
            tv_push(out, T_PUNCT, "#PPLINE#", 8); /* マーカー */
            tv_push(out, T_STRING, src + start, i - start);
            continue;
        }
        if (c == '"') {
            size_t start = i;
            i++;
            while (i < n && src[i] != '"') { if (src[i] == '\\' && i + 1 < n) i++; i++; }
            i++;
            tv_push(out, T_STRING, src + start, i - start);
            continue;
        }
        if (c == '\'') {
            size_t start = i;
            i++;
            while (i < n && src[i] != '\'') { if (src[i] == '\\' && i + 1 < n) i++; i++; }
            i++;
            tv_push(out, T_CHAR, src + start, i - start);
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)src[i+1]))) {
            size_t start = i;
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '.' ||
                   ((src[i] == '+' || src[i] == '-') && i > start && (src[i-1] == 'e' || src[i-1] == 'E')))) i++;
            tv_push(out, T_NUMBER, src + start, i - start);
            continue;
        }
        if (is_ident_start(c)) {
            size_t start = i;
            while (i < n && is_ident_char(src[i])) i++;
            tv_push(out, T_IDENT, src + start, i - start);
            continue;
        }
        bool matched = false;
        for (int k = 0; MULTI_PUNCT[k]; k++) {
            size_t l = strlen(MULTI_PUNCT[k]);
            if (i + l <= n && strncmp(src + i, MULTI_PUNCT[k], l) == 0) {
                tv_push(out, T_PUNCT, src + i, l);
                i += l;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        tv_push(out, T_PUNCT, src + i, 1);
        i++;
    }
    tv_push(out, T_END, "", 0);
}

/* ---------- パーサ状態 ---------- */
typedef struct {
    TokVec *tv;
    size_t pos;
    OutBuf notes; /* 変換できなかった箇所のメモ */
} Parser;

static Token *cur(Parser *p) { return &p->tv->toks[p->pos]; }
static bool at_end(Parser *p) { return cur(p)->type == T_END; }
static bool is_punct(Parser *p, const char *s) { return cur(p)->type == T_PUNCT && strcmp(cur(p)->text, s) == 0; }
static bool is_ident(Parser *p, const char *s) { return cur(p)->type == T_IDENT && strcmp(cur(p)->text, s) == 0; }
static void adv(Parser *p) { if (!at_end(p)) p->pos++; }

static const char *TYPE_KEYWORDS[] = {
    "int", "float", "double", "char", "long", "short", "unsigned", "signed",
    "void", "size_t", "ssize_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "bool", "_Bool", "const",
    "static", "extern", "struct", "enum", "union", NULL
};
static bool is_type_keyword(const char *s) {
    for (int i = 0; TYPE_KEYWORDS[i]; i++) if (strcmp(TYPE_KEYWORDS[i], s) == 0) return true;
    return false;
}

/* 現在位置からtype+*連続をスキップできるならスキップして真を返す。
 * その後ろに識別子が続くことまでは確認しない（呼び出し側の責務）。 */
static bool skip_type_tokens(Parser *p) {
    size_t start = p->pos;
    bool any = false;
    while (cur(p)->type == T_IDENT && is_type_keyword(cur(p)->text)) { adv(p); any = true; }
    while (is_punct(p, "*")) { adv(p); any = true; }
    if (!any) { p->pos = start; return false; }
    return true;
}

/* 型宣言 + 識別子 + ('=' | ';' | '[' | '(') のパターンを先読みで判定 */
static bool looks_like_decl(Parser *p) {
    size_t save = p->pos;
    bool ok = false;
    if (skip_type_tokens(p) && cur(p)->type == T_IDENT) {
        adv(p);
        while (is_punct(p, "[")) { /* 配列次元をスキップ */
            adv(p);
            int depth = 1;
            while (depth > 0 && !at_end(p)) {
                if (is_punct(p, "[")) depth++;
                else if (is_punct(p, "]")) depth--;
                adv(p);
            }
        }
        if (is_punct(p, "=") || is_punct(p, ";")) ok = true;
    }
    p->pos = save;
    return ok;
}

static void emit_expr(Parser *p, OutBuf *out, const char *stop_a, const char *stop_b);

/* 単純なキャスト "(type)" を読み飛ばせるなら読み飛ばして真を返す */
static bool try_skip_cast(Parser *p) {
    if (!is_punct(p, "(")) return false;
    size_t save = p->pos;
    adv(p);
    if (!skip_type_tokens(p) || cur(p)->type == T_IDENT) { /* skip_type_tokensが型を全く読まなかった場合はfalse相当 */ }
    /* 型キーワードを1つも読んでいなければキャストではない */
    size_t after_types = p->pos;
    if (after_types == save + 1) { p->pos = save; return false; }
    if (is_punct(p, ")")) { adv(p); return true; }
    p->pos = save;
    return false;
}

static void emit_expr(Parser *p, OutBuf *out, const char *stop_a, const char *stop_b) {
    int paren_depth = 0, brack_depth = 0;
    bool prev_was_operand = false;
    while (!at_end(p)) {
        if (paren_depth == 0 && brack_depth == 0) {
            if ((stop_a && is_punct(p, stop_a)) || (stop_b && is_punct(p, stop_b))) break;
        }
        Token *t = cur(p);
        if (t->type == T_PUNCT && strcmp(t->text, "(") == 0 && try_skip_cast(p)) {
            continue; /* キャストは無視して続行 */
        }
        if (t->type == T_IDENT) {
            if (strcmp(t->text, "true") == 0) ob_append(out, "True");
            else if (strcmp(t->text, "false") == 0) ob_append(out, "False");
            else if (strcmp(t->text, "NULL") == 0 || strcmp(t->text, "nullptr") == 0) ob_append(out, "None");
            else ob_append(out, t->text);
            prev_was_operand = true;
            adv(p);
        } else if (t->type == T_NUMBER) {
            ob_append(out, t->text);
            prev_was_operand = true;
            adv(p);
        } else if (t->type == T_STRING) {
            ob_append(out, t->text);
            prev_was_operand = true;
            adv(p);
        } else if (t->type == T_CHAR) {
            /* 'a' -> "a" (Pythonには文字型がないため文字列として扱う) */
            ob_append_ch(out, '"');
            ob_append(out, t->text + 1); /* 先頭のシングルクォートを飛ばす。末尾は次で処理 */
            /* 末尾のシングルクォートをダブルクォートに置換 */
            out->data[out->len - 1] = '"';
            prev_was_operand = true;
            adv(p);
        } else {
            const char *s = t->text;
            if (strcmp(s, "&&") == 0) ob_append(out, " and ");
            else if (strcmp(s, "||") == 0) ob_append(out, " or ");
            else if (strcmp(s, "!") == 0 && strcmp(s, "!=") != 0) ob_append(out, "not ");
            else if (strcmp(s, "->") == 0) ob_append(out, ".");
            else if (strcmp(s, "++") == 0) ob_append(out, " += 1"); /* 式中のi++は近似変換 */
            else if (strcmp(s, "--") == 0) ob_append(out, " -= 1");
            else if (strcmp(s, "(") == 0) { ob_append_ch(out, '('); paren_depth++; }
            else if (strcmp(s, ")") == 0) { ob_append_ch(out, ')'); paren_depth--; }
            else if (strcmp(s, "[") == 0) { ob_append_ch(out, '['); brack_depth++; }
            else if (strcmp(s, "]") == 0) { ob_append_ch(out, ']'); brack_depth--; }
            else if (strcmp(s, "&") == 0 && !prev_was_operand) { /* 単項&(アドレス取得)は意味を持たないため読み捨て */ }
            else if (strcmp(s, "&") == 0) ob_append(out, " & ");
            else if (strcmp(s, "*") == 0 && !prev_was_operand) { /* 単項*(デリファレンス)は読み捨て */ }
            else if (strcmp(s, ",") == 0) ob_append(out, ", ");
            else if (strcmp(s, ".") == 0) ob_append(out, ".");
            else ob_append(out, s);
            prev_was_operand = (strcmp(s, ")") == 0 || strcmp(s, "]") == 0);
            adv(p);
        }
    }
}

/* printf("fmt", args...) を print(...) に変換 */
static void emit_printf(Parser *p, OutBuf *out, int indent) {
    adv(p); /* printf */
    adv(p); /* ( */
    if (cur(p)->type != T_STRING) {
        /* フォーマット文字列が定数でない場合は簡易フォールバック */
        ob_indent(out, indent);
        ob_append(out, "print(");
        emit_expr(p, out, ")", NULL);
        ob_append(out, ")");
        if (is_punct(p, ")")) adv(p);
        if (is_punct(p, ";")) adv(p);
        ob_append(out, "\n");
        return;
    }
    char *raw = cur(p)->text; /* ダブルクォート込み */
    size_t rl = strlen(raw);
    char *fmt = (char*)c2py_alloc(rl);
    memcpy(fmt, raw + 1, rl - 2);
    fmt[rl - 2] = '\0';
    adv(p);

    /* 引数式を集める */
    char *arg_exprs[64];
    int nargs = 0;
    while (is_punct(p, ",")) {
        adv(p);
        OutBuf a; ob_init(&a);
        emit_expr(p, &a, ",", ")");
        if (nargs < 64) arg_exprs[nargs++] = a.data; else free(a.data);
    }
    if (is_punct(p, ")")) adv(p);
    if (is_punct(p, ";")) adv(p);

    /* フォーマット文字列を解析して f-string を組み立てる */
    OutBuf py; ob_init(&py);
    int argi = 0;
    size_t fl = strlen(fmt);
    /* 末尾の \n を1つだけ落とす（print()が改行を付与するため） */
    if (fl >= 2 && fmt[fl-2] == '\\' && fmt[fl-1] == 'n') fl -= 2;
    bool used_fstring = false;
    for (size_t i = 0; i < fl; i++) {
        if (fmt[i] == '%' && i + 1 < fl) {
            size_t j = i + 1;
            while (j < fl && strchr("-+ 0123456789.lhLqjzt", fmt[j])) j++;
            if (j < fl) {
                char conv = fmt[j];
                if (conv == '%') { ob_append_ch(&py, '%'); i = j; continue; }
                if (strchr("dioux Xufeg cs", conv) || strchr("dioufegcs", conv)) {
                    ob_append_ch(&py, '{');
                    if (argi < nargs) ob_append(&py, arg_exprs[argi++]);
                    ob_append_ch(&py, '}');
                    used_fstring = true;
                    i = j;
                    continue;
                }
            }
        }
        if (fmt[i] == '\\' && i + 1 < fl && fmt[i+1] == 'n') { ob_append(&py, "\\n"); i++; continue; }
        if (fmt[i] == '"') { ob_append(&py, "\\\""); continue; }
        ob_append_ch(&py, fmt[i]);
    }
    /* 残った実引数はフォーマットに対応がないため末尾にそのまま並べる */
    ob_indent(out, indent);
    ob_append(out, "print(");
    ob_append_ch(out, used_fstring ? 'f' : ' ');
    if (used_fstring && out->data[out->len-1] == 'f') { /* ok */ } else if (!used_fstring) { out->len--; out->data[out->len] = '\0'; }
    ob_append_ch(out, '"');
    ob_append(out, py.data);
    ob_append_ch(out, '"');
    for (int k = argi; k < nargs; k++) { ob_append(out, ", "); ob_append(out, arg_exprs[k]); }
    ob_append(out, ")\n");
    free(fmt);
    free(py.data);
    for (int k = 0; k < nargs; k++) free(arg_exprs[k]);
}

static void parse_block(Parser *p, OutBuf *out, int indent);

static void emit_simple_condition_block(Parser *p, OutBuf *out, int indent, const char *keyword) {
    adv(p); /* if / while */
    ob_indent(out, indent);
    ob_append(out, keyword);
    ob_append_ch(out, ' ');
    if (is_punct(p, "(")) adv(p);
    emit_expr(p, out, ")", NULL);
    if (is_punct(p, ")")) adv(p);
    ob_append(out, ":\n");
    if (is_punct(p, "{")) {
        adv(p);
        parse_block(p, out, indent + 1);
    } else {
        parse_block(p, out, indent + 1); /* 単文のみのif/whileにも対応 */
    }
}

/* for(init; cond; incr) を可能ならrange()ループへ、無理ならwhileへ変換 */
static void emit_for(Parser *p, OutBuf *out, int indent) {
    adv(p); /* for */
    if (is_punct(p, "(")) adv(p);

    size_t init_start = p->pos;
    OutBuf init_buf; ob_init(&init_buf);
    /* 変数名を先読み： TYPE? name = start */
    skip_type_tokens(p);
    char varname[128] = {0};
    bool simple_init = false;
    OutBuf start_expr; ob_init(&start_expr);
    if (cur(p)->type == T_IDENT) {
        strncpy(varname, cur(p)->text, sizeof(varname)-1);
        size_t save = p->pos;
        adv(p);
        if (is_punct(p, "=")) {
            adv(p);
            emit_expr(p, &start_expr, ";", NULL);
            simple_init = true;
        } else {
            p->pos = save;
        }
    }
    if (is_punct(p, ";")) adv(p);
    else { p->pos = init_start; /* 想定外: initを式として読み直す */ }

    OutBuf cond_buf; ob_init(&cond_buf);
    size_t cond_start = p->pos;
    char cmp_op[3] = {0};
    OutBuf end_expr; ob_init(&end_expr);
    bool simple_cond = false;
    if (simple_init && cur(p)->type == T_IDENT && strcmp(cur(p)->text, varname) == 0) {
        size_t save = p->pos;
        adv(p);
        if (cur(p)->type == T_PUNCT && (strcmp(cur(p)->text, "<") == 0 || strcmp(cur(p)->text, "<=") == 0 ||
            strcmp(cur(p)->text, ">") == 0 || strcmp(cur(p)->text, ">=") == 0)) {
            strncpy(cmp_op, cur(p)->text, 2);
            adv(p);
            emit_expr(p, &end_expr, ";", NULL);
            simple_cond = true;
        } else {
            p->pos = save;
        }
    }
    if (!simple_cond) { p->pos = cond_start; emit_expr(p, &cond_buf, ";", NULL); }
    if (is_punct(p, ";")) adv(p);

    size_t incr_start = p->pos;
    OutBuf incr_buf; ob_init(&incr_buf);
    bool simple_incr = false;
    char step_sign = '+';
    OutBuf step_expr; ob_init(&step_expr);
    if (simple_cond && cur(p)->type == T_IDENT && strcmp(cur(p)->text, varname) == 0) {
        size_t save = p->pos;
        adv(p);
        if (is_punct(p, "++")) { adv(p); ob_append(&step_expr, "1"); simple_incr = true; step_sign = '+'; }
        else if (is_punct(p, "--")) { adv(p); ob_append(&step_expr, "1"); simple_incr = true; step_sign = '-'; }
        else if (is_punct(p, "+=")) { adv(p); emit_expr(p, &step_expr, ")", NULL); simple_incr = true; step_sign = '+'; }
        else if (is_punct(p, "-=")) { adv(p); emit_expr(p, &step_expr, ")", NULL); simple_incr = true; step_sign = '-'; }
        else p->pos = save;
    }
    if (!simple_incr) { p->pos = incr_start; emit_expr(p, &incr_buf, ")", NULL); }
    if (is_punct(p, ")")) adv(p);

    if (simple_init && simple_cond && simple_incr) {
        ob_indent(out, indent);
        ob_append(out, "for "); ob_append(out, varname); ob_append(out, " in range(");
        ob_append(out, start_expr.data); ob_append(out, ", ");
        /* < / <= / > / >= の境界を range() の排他的上限に合わせる */
        bool ascending = (step_sign == '+');
        if (ascending) {
            ob_append(out, end_expr.data);
            if (strcmp(cmp_op, "<=") == 0) ob_append(out, " + 1");
        } else {
            ob_append(out, end_expr.data);
            if (strcmp(cmp_op, ">=") == 0) ob_append(out, " - 1");
        }
        ob_append(out, ", ");
        if (ascending) ob_append(out, step_expr.data);
        else { ob_append_ch(out, '-'); ob_append(out, step_expr.data); }
        ob_append(out, "):\n");
        if (is_punct(p, "{")) { adv(p); parse_block(p, out, indent + 1); }
        else parse_block(p, out, indent + 1);
    } else {
        /* 一般形: init文を出し、while条件でループ、本体末尾でincrを実行 */
        if (init_buf.len) { ob_indent(out, indent); ob_append(out, init_buf.data); ob_append(out, "\n"); }
        else if (simple_init) { ob_indent(out, indent); ob_append(out, varname); ob_append(out, " = "); ob_append(out, start_expr.data); ob_append(out, "\n"); }
        ob_indent(out, indent); ob_append(out, "while ");
        if (simple_cond) { ob_append(out, varname); ob_append_ch(out, ' '); ob_append(out, cmp_op); ob_append_ch(out, ' '); ob_append(out, end_expr.data); }
        else ob_append(out, cond_buf.data);
        ob_append(out, ":\n");
        OutBuf body; ob_init(&body);
        if (is_punct(p, "{")) { adv(p); parse_block(p, &body, indent + 1); }
        else parse_block(p, &body, indent + 1);
        ob_append(out, body.data);
        ob_indent(out, indent + 1);
        if (simple_incr) { ob_append(out, varname); ob_append(out, step_sign == '+' ? " += " : " -= "); ob_append(out, step_expr.data); ob_append(out, "\n"); }
        else { ob_append(out, incr_buf.data); ob_append(out, "\n"); }
        free(body.data);
    }
    free(init_buf.data); free(cond_buf.data); free(incr_buf.data);
    free(start_expr.data); free(end_expr.data); free(step_expr.data);
}

static void parse_statement(Parser *p, OutBuf *out, int indent) {
    if (is_punct(p, ";")) { adv(p); return; }
    if (is_punct(p, "{")) { adv(p); parse_block(p, out, indent); return; }
    if (is_punct(p, "#PPLINE#")) { adv(p); /* コメント化した行本体 */
        ob_indent(out, indent); ob_append(out, "# "); ob_append(out, cur(p)->text); ob_append(out, "\n"); adv(p); return; }

    if (is_ident(p, "if")) {
        emit_simple_condition_block(p, out, indent, "if");
        while (is_ident(p, "else")) {
            adv(p);
            if (is_ident(p, "if")) {
                emit_simple_condition_block(p, out, indent, "elif");
            } else {
                ob_indent(out, indent); ob_append(out, "else:\n");
                if (is_punct(p, "{")) { adv(p); parse_block(p, out, indent + 1); }
                else parse_block(p, out, indent + 1);
            }
        }
        return;
    }
    if (is_ident(p, "while")) { emit_simple_condition_block(p, out, indent, "while"); return; }
    if (is_ident(p, "for")) { emit_for(p, out, indent); return; }
    if (is_ident(p, "do")) {
        /* do { ... } while(cond); -> while True: ... ; if not cond: break */
        adv(p);
        ob_indent(out, indent); ob_append(out, "while True:\n");
        if (is_punct(p, "{")) { adv(p); parse_block(p, out, indent + 1); }
        else parse_block(p, out, indent + 1);
        if (is_ident(p, "while")) adv(p);
        if (is_punct(p, "(")) adv(p);
        ob_indent(out, indent + 1); ob_append(out, "if not (");
        emit_expr(p, out, ")", NULL);
        ob_append(out, "):\n");
        ob_indent(out, indent + 2); ob_append(out, "break\n");
        if (is_punct(p, ")")) adv(p);
        if (is_punct(p, ";")) adv(p);
        return;
    }
    if (is_ident(p, "return")) {
        adv(p);
        ob_indent(out, indent);
        if (is_punct(p, ";")) { ob_append(out, "return\n"); adv(p); return; }
        ob_append(out, "return ");
        emit_expr(p, out, ";", NULL);
        ob_append(out, "\n");
        if (is_punct(p, ";")) adv(p);
        return;
    }
    if (is_ident(p, "break")) { adv(p); ob_indent(out, indent); ob_append(out, "break\n"); if (is_punct(p, ";")) adv(p); return; }
    if (is_ident(p, "continue")) { adv(p); ob_indent(out, indent); ob_append(out, "continue\n"); if (is_punct(p, ";")) adv(p); return; }
    if (is_ident(p, "printf")) { emit_printf(p, out, indent); return; }
    if (is_ident(p, "struct") || is_ident(p, "typedef") || is_ident(p, "enum") || is_ident(p, "union")) {
        /* 未対応: マッチする;または}まで読み飛ばしてコメントを残す */
        ob_indent(out, indent); ob_append(out, "# TODO(c2py): struct/typedef/enum/union is not translated automatically\n");
        int depth = 0;
        while (!at_end(p)) {
            if (is_punct(p, "{")) { depth++; adv(p); continue; }
            if (is_punct(p, "}")) { depth--; adv(p); if (depth <= 0) break; continue; }
            if (depth == 0 && is_punct(p, ";")) { adv(p); break; }
            adv(p);
        }
        return;
    }

    if (looks_like_decl(p)) {
        skip_type_tokens(p);
        char name[128] = {0};
        strncpy(name, cur(p)->text, sizeof(name)-1);
        adv(p);
        /* 配列次元は読み飛ばす（サイズ情報は破棄） */
        while (is_punct(p, "[")) {
            adv(p);
            int depth = 1;
            while (depth > 0 && !at_end(p)) {
                if (is_punct(p, "[")) depth++;
                else if (is_punct(p, "]")) depth--;
                adv(p);
            }
        }
        ob_indent(out, indent);
        ob_append(out, name);
        if (is_punct(p, "=")) {
            adv(p);
            ob_append(out, " = ");
            /* {1,2,3} 形式の初期化子はPythonのリストに変換 */
            if (is_punct(p, "{")) {
                adv(p);
                ob_append_ch(out, '[');
                emit_expr(p, out, "}", NULL);
                ob_append_ch(out, ']');
                if (is_punct(p, "}")) adv(p);
            } else {
                emit_expr(p, out, ";", NULL);
            }
        } else {
            ob_append(out, " = None  # c2py: uninitialized in original C");
        }
        ob_append(out, "\n");
        if (is_punct(p, ";")) adv(p);
        return;
    }

    /* それ以外は式文として扱う（代入・関数呼び出し・i++等） */
    ob_indent(out, indent);
    emit_expr(p, out, ";", NULL);
    ob_append(out, "\n");
    if (is_punct(p, ";")) adv(p);
}

static void parse_block(Parser *p, OutBuf *out, int indent) {
    size_t produced_before = out->len;
    while (!at_end(p) && !is_punct(p, "}")) {
        parse_statement(p, out, indent);
    }
    if (is_punct(p, "}")) adv(p);
    if (out->len == produced_before) { ob_indent(out, indent); ob_append(out, "pass\n"); }
}

/* 関数定義かどうかを先読みで判定し、名前と引数名リストを取得する。
 * 戻り値: 0=関数ではない, 1=プロトタイプ(本体なし), 2=定義(本体あり) */
static int try_parse_func_header(Parser *p, char *name_out, size_t name_cap, char arg_names[][64], int *arg_count, int max_args) {
    size_t save = p->pos;
    skip_type_tokens(p);
    if (cur(p)->type != T_IDENT) { p->pos = save; return 0; }
    strncpy(name_out, cur(p)->text, name_cap - 1);
    adv(p);
    if (!is_punct(p, "(")) { p->pos = save; return 0; }
    adv(p);
    *arg_count = 0;
    if (is_ident(p, "void") ) {
        size_t s2 = p->pos; adv(p);
        if (!is_punct(p, ")")) p->pos = s2; /* "void foo(void, int)" のような形は無いのでこれで十分 */
    }
    while (!is_punct(p, ")") && !at_end(p)) {
        skip_type_tokens(p);
        if (cur(p)->type == T_IDENT) {
            if (*arg_count < max_args) { strncpy(arg_names[*arg_count], cur(p)->text, 63); (*arg_count)++; }
            adv(p);
        } else if (!is_punct(p, ",")) {
            adv(p); /* 想定外トークンは読み飛ばす（可変長引数の...等） */
        }
        if (is_punct(p, ",")) adv(p);
        while (is_punct(p, "[")) { adv(p); int depth = 1; while (depth > 0 && !at_end(p)) { if (is_punct(p,"[")) depth++; else if (is_punct(p,"]")) depth--; adv(p); } }
    }
    if (is_punct(p, ")")) adv(p);
    if (is_punct(p, "{")) return 2;
    if (is_punct(p, ";")) return 1;
    p->pos = save;
    return 0;
}

int c_to_python(const char *c_code, char **out_python_code) {
    if (!c_code || !out_python_code) return 1;
    TokVec tv; tv_init(&tv);
    tokenize(c_code, &tv);
    Parser p; p.tv = &tv; p.pos = 0; ob_init(&p.notes);

    OutBuf out; ob_init(&out);
    ob_append(&out, "# Generated by c2py (simplified C -> Python converter)\n");
    ob_append(&out, "# NOTE: pointers, structs, and manual memory management are not\n");
    ob_append(&out, "# faithfully translated. Review TODO markers before relying on this.\n\n");

    OutBuf toplevel_calls; ob_init(&toplevel_calls);
    bool has_main = false;

    while (!at_end(&p)) {
        if (is_punct(&p, "#PPLINE#")) {
            adv(&p);
            ob_append(&out, "# ");
            ob_append(&out, cur(&p)->text ? cur(&p)->text : "");
            ob_append(&out, "\n");
            adv(&p);
            continue;
        }
        size_t save = p.pos;
        char fname[128] = {0};
        char arg_names[32][64];
        int argc = 0;
        int kind = try_parse_func_header(&p, fname, sizeof(fname), arg_names, &argc, 32);
        if (kind == 1) { continue; } /* プロトタイプは無視 */
        if (kind == 2) {
            bool is_main = strcmp(fname, "main") == 0;
            if (is_main) has_main = true;
            adv(&p); /* '{' */
            ob_append(&out, "def ");
            ob_append(&out, is_main ? "main" : fname);
            ob_append(&out, "(");
            for (int i = 0; i < argc; i++) { if (i) ob_append(&out, ", "); ob_append(&out, arg_names[i]); }
            ob_append(&out, "):\n");
            parse_block(&p, &out, 1);
            ob_append(&out, "\n");
            continue;
        }
        p.pos = save;
        /* 関数でなければトップレベルの宣言/文として処理 */
        if (is_punct(&p, "#PPLINE#")) { adv(&p); adv(&p); continue; }
        size_t before = out.len;
        parse_statement(&p, &out, 0);
        (void)before;
    }

    if (has_main) ob_append(&out, "\nif __name__ == \"__main__\":\n    main()\n");

    for (size_t i = 0; i < tv.n; i++) free(tv.toks[i].text);
    free(tv.toks);
    free(p.notes.data);
    free(toplevel_calls.data);

    *out_python_code = out.data;
    return 0;
}
