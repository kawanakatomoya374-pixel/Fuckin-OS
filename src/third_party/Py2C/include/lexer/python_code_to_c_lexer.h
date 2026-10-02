#ifndef PYTHON_CODE_TO_C_LEXER_H
#define PYTHON_CODE_TO_C_LEXER_H

#include "common/python_code_to_c_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================
 * トークン種別
 * ======================================== */

typedef enum {
    /* リテラル */
    TOK_INT_LITERAL,        /* 123 */
    TOK_FLOAT_LITERAL,      /* 3.14 */
    TOK_STR_LITERAL,        /* "hello" */
    TOK_FSTRING_LITERAL,    /* f"hello {name}" -- テキストはエスケープ処理済みのraw内容(波括弧含む)を保持 */
    TOK_BOOL_LITERAL,       /* True, False */
    TOK_NONE_LITERAL,       /* None */

    /* 識別子 */
    TOK_IDENTIFIER,         /* variable_name */

    /* キーワード */
    TOK_KW_AND,             /* and */
    TOK_KW_AS,              /* as */
    TOK_KW_ASYNC,           /* async */
    TOK_KW_AWAIT,           /* await */
    TOK_KW_ASSERT,          /* assert */
    TOK_KW_BREAK,           /* break */
    TOK_KW_CLASS,           /* class */
    TOK_KW_CASE,            /* case */
    TOK_KW_MATCH,           /* match */
    TOK_KW_CONTINUE,        /* continue */
    TOK_KW_DEF,             /* def */
    TOK_KW_DEL,             /* del */
    TOK_KW_ELIF,            /* elif */
    TOK_KW_ELSE,            /* else */
    TOK_KW_EXCEPT,          /* except */
    TOK_KW_FINALLY,         /* finally */
    TOK_KW_FOR,             /* for */
    TOK_KW_FROM,            /* from */
    TOK_KW_GLOBAL,          /* global */
    TOK_KW_NONLOCAL,        /* nonlocal */
    TOK_KW_IF,              /* if */
    TOK_KW_IMPORT,          /* import */
    TOK_KW_IN,              /* in */
    TOK_KW_IS,              /* is */
    TOK_KW_LAMBDA,          /* lambda */
    TOK_KW_NOT,             /* not */
    TOK_KW_OR,              /* or */
    TOK_KW_PASS,            /* pass */
    TOK_KW_RAISE,           /* raise */
    TOK_KW_RETURN,          /* return */
    TOK_KW_TRY,             /* try */
    TOK_KW_WHILE,           /* while */
    TOK_KW_WITH,            /* with */
    TOK_KW_YIELD,           /* yield */

    /* 演算子 */
    TOK_PLUS,               /* + */
    TOK_MINUS,              /* - */
    TOK_STAR,               /* * */
    TOK_SLASH,              /* / */
    TOK_DBL_SLASH,          /* // */
    TOK_PERCENT,            /* % */
    TOK_DBL_STAR,           /* ** */
    TOK_AT,                 /* @ */
    TOK_LSHIFT,             /* << */
    TOK_RSHIFT,             /* >> */
    TOK_AMPERSAND,          /* & */
    TOK_PIPE,               /* | */
    TOK_CARET,              /* ^ */
    TOK_TILDE,              /* ~ */
    TOK_LT,                 /* < */
    TOK_GT,                 /* > */
    TOK_LE,                 /* <= */
    TOK_GE,                 /* >= */
    TOK_EQ,                 /* == */
    TOK_NE,                 /* != */

    /* 代入演算子 */
    TOK_ASSIGN,             /* = */
    TOK_WALRUS,             /* := */
    TOK_PLUS_ASSIGN,        /* += */
    TOK_MINUS_ASSIGN,       /* -= */
    TOK_STAR_ASSIGN,        /* *= */
    TOK_SLASH_ASSIGN,       /* /= */
    TOK_DBL_SLASH_ASSIGN,   /* //= */
    TOK_PERCENT_ASSIGN,     /* %= */
    TOK_DBL_STAR_ASSIGN,    /* **= */
    TOK_LSHIFT_ASSIGN,      /* <<= */
    TOK_RSHIFT_ASSIGN,      /* >>= */
    TOK_AMP_ASSIGN,         /* &= */
    TOK_PIPE_ASSIGN,        /* |= */
    TOK_CARET_ASSIGN,       /* ^= */

    /* デリミタ */
    TOK_LPAREN,             /* ( */
    TOK_RPAREN,             /* ) */
    TOK_LBRACKET,           /* [ */
    TOK_RBRACKET,           /* ] */
    TOK_LBRACE,             /* { */
    TOK_RBRACE,             /* } */
    TOK_COMMA,              /* , */
    TOK_COLON,              /* : */
    TOK_DOT,                /* . */
    TOK_SEMICOLON,          /* ; */
    TOK_ARROW,              /* -> */
    TOK_ELLIPSIS,           /* ... */

    /* インデント/改行 */
    TOK_INDENT,             /* インデント増加 */
    TOK_DEDENT,             /* インデント減少 */
    TOK_NEWLINE,            /* 改行 */

    /* 特殊 */
    TOK_EOF,                /* 入力終了 */
    TOK_COMMENT,            /* # コメント */
    TOK_UNKNOWN             /* 不明なトークン */
} P2C_TokenType;

/* ========================================
 * トークン構造体
 * ======================================== */

typedef struct {
    P2C_TokenType type;
    char *text;             /* トークン文字列（所有権：アロケータ） */
    size_t len;
    uint32_t line;          /* 行番号（1-based） */
    uint32_t col;           /* 列番号（1-based） */
    uint32_t indent;        /* インデントレベル（INDENT/DEDENT用） */
} P2C_Token;

/* ========================================
 * Lexer構造体
 * ======================================== */

typedef struct P2C_Lexer P2C_Lexer;

struct P2C_Lexer {
    P2C_Allocator *alloc;
    const char *source;     /* ソースコード（改行正規化時のみowned_sourceを参照） */
    size_t source_len;
    char *owned_source;     /* 改行正規化で内部複製した場合の所有ポインタ（NULL可） */
    size_t pos;             /* 現在位置 */
    uint32_t line;
    uint32_t col;

    /* インデント管理 */
    P2C_Vector *indent_stack; /* int* のスタック */
    bool at_line_start;
    bool emitted_newline;

    /* 先行トークン（ルックアhead用） */
    P2C_Token *current;
    P2C_Token *peek;
    bool has_peek;
    P2C_Token *peek2;
    /* 引退したトークン（解放せず保持し、lexer破棄時に一括解放する）。
     * parser は CURRENT(p) を控えて NEXT(p) の直後に line/col/text を読む形を
     * 多用するため、即時解放すると解放後参照になる（ASanが実例を検出した）。
     * 解析中のメモリは増えるが、AST と同様に解析終了時にまとめて解放される。 */
    P2C_Vector *retired_tokens;
    /* 直前のトークン。parser が CURRENT(p) を控えて NEXT(p) の直後に
     * line/col/text を読む形を多用するため、即時解放すると解放後参照に
     * なる（ASan: heap-use-after-free）。1つだけ保持し、さらに次へ進んだ
     * 時点で解放する（余分なメモリは1トークン分）。 */
    bool has_peek2;
    uint32_t grouping_depth; /* (), [], {} 内では改行・インデントを無視 */
};

/* ========================================
 * Lexer API
 * ======================================== */

/* Lexer作成/破棄 */
P2C_Lexer* p2c_lexer_new(P2C_Allocator *a, const char *source, size_t len);
void p2c_lexer_free(P2C_Lexer *lex);

/* トークン取得 */
P2C_Token* p2c_lexer_next(P2C_Lexer *lex);      /* 次のトークン（所有権移動） */
P2C_Token* p2c_lexer_peek(P2C_Lexer *lex);      /* 先読み（所有権なし） */
P2C_Token* p2c_lexer_peek2(P2C_Lexer *lex);      /* 2つ先の先読み（所有権なし） */
bool p2c_lexer_consume(P2C_Lexer *lex, P2C_TokenType type); /* 期待型を消費 */
P2C_Token* p2c_lexer_expect(P2C_Lexer *lex, P2C_TokenType type, P2C_Result *out_err); /* 期待型を取得 */

/* ユーティリティ */
const char* p2c_token_type_name(P2C_TokenType type);
void p2c_token_free(P2C_Token *tok, P2C_Allocator *a);
P2C_Token* p2c_token_clone(P2C_Token *tok, P2C_Allocator *a);

#ifdef __cplusplus
}
#endif

#endif /* PYTHON_CODE_TO_C_LEXER_H */
