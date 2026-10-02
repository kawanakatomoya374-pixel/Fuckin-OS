#include "lexer/python_code_to_c_lexer.h"
#include "common/python_code_to_c_common.h"

/* キーワードテーブル */
typedef struct {
    const char *word;
    P2C_TokenType type;
} Keyword;

static const Keyword keywords[] = {
    {"and", TOK_KW_AND}, {"as", TOK_KW_AS}, {"async", TOK_KW_ASYNC}, {"await", TOK_KW_AWAIT}, {"assert", TOK_KW_ASSERT},
    {"break", TOK_KW_BREAK}, {"case", TOK_KW_CASE}, {"class", TOK_KW_CLASS}, {"continue", TOK_KW_CONTINUE},
    {"match", TOK_KW_MATCH},
    {"def", TOK_KW_DEF}, {"del", TOK_KW_DEL}, {"elif", TOK_KW_ELIF},
    {"else", TOK_KW_ELSE}, {"except", TOK_KW_EXCEPT}, {"finally", TOK_KW_FINALLY},
    {"for", TOK_KW_FOR}, {"from", TOK_KW_FROM}, {"global", TOK_KW_GLOBAL}, {"nonlocal", TOK_KW_NONLOCAL},
    {"if", TOK_KW_IF}, {"import", TOK_KW_IMPORT}, {"in", TOK_KW_IN},
    {"is", TOK_KW_IS}, {"lambda", TOK_KW_LAMBDA}, {"not", TOK_KW_NOT},
    {"or", TOK_KW_OR}, {"pass", TOK_KW_PASS}, {"raise", TOK_KW_RAISE},
    {"return", TOK_KW_RETURN}, {"try", TOK_KW_TRY}, {"while", TOK_KW_WHILE},
    {"with", TOK_KW_WITH}, {"yield", TOK_KW_YIELD},
    {NULL, TOK_UNKNOWN}
};

/* 先頭がキーワードかチェック */
static P2C_TokenType check_keyword(const char *s, size_t len) {
    for (int i = 0; keywords[i].word; i++) {
        size_t kwlen = strlen(keywords[i].word);
        if (kwlen == len && strncmp(s, keywords[i].word, len) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENTIFIER;
}

/* 空白文字チェック */
static int is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

/* ========================================
 * Lexer作成/破棄
 * ======================================== */

static void free_token_fn(void *p, P2C_Allocator *a) {
    (void)p; (void)a;
}

/* 改行正規化が必要か（'\r' を含むか）を調べる。
 * libcの memchr に依存せず、freestanding でも同じ挙動にする。 */
static bool source_has_carriage_return(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r') return true;
    }
    return false;
}

P2C_Lexer* p2c_lexer_new(P2C_Allocator *a, const char *source, size_t len) {
    if (!source) return NULL;
    if (len == 0) len = strlen(source);
    P2C_Lexer *lex = p2c_alloc(a, sizeof(P2C_Lexer));
    if (!lex) return NULL;

    lex->alloc = a;
    /* 引退トークンのリスト（要素の解放は p2c_lexer_free が alloc 付きで行う）。 */
    lex->retired_tokens = p2c_vec_new(a, NULL);
    if (!lex->retired_tokens) { p2c_free(a, lex); return NULL; }
    /* 改行コードの正規化（universal newlines）。
     * Pythonのソースは CRLF / CR / LF のいずれでも同じ意味を持つと規定されて
     * いる。以前は '\r' を改行として扱っていなかったため、Windowsで保存した
     * CRLF のソースでは行末の '\r' が未知トークンになり、インデント計算と
     * class本体のメソッド検出が壊れて「メソッドがクラスに登録されない」
     * （実行時に AttributeError になる）という誤変換を起こしていた。
     * '\r' を含むソースだけを内部バッファへ複製し、'\r\n' と孤立した '\r' を
     *  '\n' へ揃える（三連クォート文字列の内部も同時に正規化される）。
     * '\r' を含まないソースは複製せず、呼び出し側のバッファをそのまま参照する。 */
    lex->owned_source = NULL;
    lex->source = source;
    lex->source_len = len;
    if (source_has_carriage_return(source, len)) {
        char *normalized = p2c_alloc(a, len + 1);
        if (!normalized) { p2c_free(a, lex); return NULL; }
        size_t out = 0;
        for (size_t i = 0; i < len; i++) {
            if (source[i] == '\r') {
                if (i + 1 < len && source[i + 1] == '\n') i++;
                normalized[out++] = '\n';
            } else {
                normalized[out++] = source[i];
            }
        }
        normalized[out] = '\0';
        lex->source = normalized;
        lex->source_len = out;
        lex->owned_source = normalized;
    }
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->at_line_start = true;
    lex->emitted_newline = false;
    lex->current = NULL;
    lex->peek = NULL;
    lex->has_peek = false;
    lex->peek2 = NULL;
    lex->has_peek2 = false;
    lex->grouping_depth = 0;
    
    /* インデントスタック初期化（0をプッシュ） */
    lex->indent_stack = p2c_vec_new(a, free_token_fn);
    if (!lex->indent_stack) { p2c_free(a, lex); return NULL; }
    
    int *zero = p2c_alloc(a, sizeof(int));
    *zero = 0;
    p2c_vec_push(lex->indent_stack, zero);
    
    return lex;
}

/* トークンを解放せず引退リストへ移す（lexer破棄時に一括解放）。
 * 引退リストが無い場合（確保失敗）は従来どおり即時解放する。 */
static void lexer_retire(P2C_Lexer *lex, P2C_Token *tok) {
    if (!tok) return;
    if (lex->retired_tokens) {
        p2c_vec_push(lex->retired_tokens, tok);
        return;
    }
    p2c_token_free(tok, lex->alloc);
}

void p2c_lexer_free(P2C_Lexer *lex) {
    if (!lex) return;
    /* インデントスタックの数値を解放 */
    for (size_t i = 0; i < p2c_vec_len(lex->indent_stack); i++) {
        p2c_free(lex->alloc, p2c_vec_get(lex->indent_stack, i));
    }
    p2c_vec_free(lex->indent_stack);
    if (lex->current) p2c_token_free(lex->current, lex->alloc);
    if (lex->peek) p2c_token_free(lex->peek, lex->alloc);
    if (lex->peek2) p2c_token_free(lex->peek2, lex->alloc);
    if (lex->retired_tokens) {
        /* トークンの解放には alloc が要るため、要素解放関数ではなく明示ループで行う。 */
        for (size_t i = 0; i < p2c_vec_len(lex->retired_tokens); i++) {
            p2c_token_free((P2C_Token*)p2c_vec_get(lex->retired_tokens, i), lex->alloc);
        }
        p2c_vec_free(lex->retired_tokens);
        lex->retired_tokens = NULL;
    }
    /* 改行正規化のために内部で複製したソースだけを解放する。 */
    if (lex->owned_source) p2c_free(lex->alloc, lex->owned_source);
    p2c_free(lex->alloc, lex);
}

/* ========================================
 * 内部ヘルパー
 * ======================================== */

static char peek_char(P2C_Lexer *lex, size_t offset) {
    size_t pos = lex->pos + offset;
    if (pos >= lex->source_len) return '\0';
    return lex->source[pos];
}

static char advance(P2C_Lexer *lex) {
    if (lex->pos >= lex->source_len) return '\0';
    char c = lex->source[lex->pos];
    lex->pos++;
    if (c == '\n') {
        lex->line++;
        lex->col = 1;
        lex->at_line_start = true;
    } else {
        lex->col++;
    }
    return c;
}

static P2C_Token* make_token(P2C_Lexer *lex, P2C_TokenType type, const char *text, size_t len) {
    P2C_Token *tok = p2c_alloc(lex->alloc, sizeof(P2C_Token));
    if (!tok) return NULL;
    tok->type = type;
    tok->len = len;
    tok->line = lex->line;
    tok->col = lex->col - (uint32_t)len;
    tok->indent = 0;
    if (len > 0) {
        tok->text = p2c_alloc(lex->alloc, len + 1);
        if (tok->text) {
            memcpy(tok->text, text, len);
            tok->text[len] = '\0';
        }
    } else {
        tok->text = NULL;
    }
    return tok;
}

/* make_token()は「text/lenが実際に消費した元ソースの部分文字列そのもの」
 * という前提で列番号を逆算する(col = 現在列 - len)。しかし
 * TOK_UNKNOWN の「合成」エラーマーカー（例: 複素数リテラルやbytes
 * リテラルを検出した際に、パーサ側の分岐用に短い固定文字列を持たせる
 * ケース）ではtextが実際のソース文字列と無関係な長さになるため、その
 * ロジックのまま使うと列番号が桁あふれ・不正な値になる。
 * この専用ヘルパーは呼び出し側が明示的に始点(line/col)を渡すことで
 * それを回避する。 */
static P2C_Token* make_marker_token_at(P2C_Lexer *lex, P2C_TokenType type, const char *marker,
                                        uint32_t start_line, uint32_t start_col) {
    size_t len = strlen(marker);
    P2C_Token *tok = p2c_alloc(lex->alloc, sizeof(P2C_Token));
    if (!tok) return NULL;
    tok->type = type;
    tok->len = len;
    tok->line = start_line;
    tok->col = start_col;
    tok->indent = 0;
    tok->text = p2c_alloc(lex->alloc, len + 1);
    if (tok->text) memcpy(tok->text, marker, len + 1);
    (void)lex;
    return tok;
}

/* 行末までスキップ（コメントや空白） */
static void skip_line(P2C_Lexer *lex) {
    while (lex->pos < lex->source_len && peek_char(lex, 0) != '\n') {
        advance(lex);
    }
}

/* 空白をスキップ（改行は除く） */
static void skip_whitespace(P2C_Lexer *lex) {
    while (lex->pos < lex->source_len && is_whitespace(peek_char(lex, 0))) {
        advance(lex);
    }
}

/* 行頭のインデントを計算 */
static int count_indent(P2C_Lexer *lex) {
    int indent = 0;
    while (lex->pos < lex->source_len) {
        char c = peek_char(lex, 0);
        if (c == ' ') { indent++; advance(lex); }
        else if (c == '\t') { indent += 8; advance(lex); }
        else break;
    }
    return indent;
}

/* ========================================
 * 各種トークン読み取り
 * ======================================== */

/* 文字列リテラル（シングル/ダブルクォート） */
/* UnicodeコードポイントをUTF-8バイト列として追記する（P2Cランタイムの
 * 文字列表現はUTF-8）。範囲外の値はU+FFFDとして扱う。 */
static void append_utf8_codepoint(P2C_String *buf, uint32_t cp) {
    if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu; /* 不正値・代理対 */
    if (cp < 0x80u) {
        p2c_str_append_char(buf, (char)cp);
    } else if (cp < 0x800u) {
        p2c_str_append_char(buf, (char)(0xC0u | (cp >> 6)));
        p2c_str_append_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        p2c_str_append_char(buf, (char)(0xE0u | (cp >> 12)));
        p2c_str_append_char(buf, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        p2c_str_append_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    } else {
        p2c_str_append_char(buf, (char)(0xF0u | (cp >> 18)));
        p2c_str_append_char(buf, (char)(0x80u | ((cp >> 12) & 0x3Fu)));
        p2c_str_append_char(buf, (char)(0x80u | ((cp >> 6) & 0x3Fu)));
        p2c_str_append_char(buf, (char)(0x80u | (cp & 0x3Fu)));
    }
}

/* 16進数字1文字を値へ。非16進なら -1。 */
static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* 文字列エスケープ1つを解釈して buf へ追記する。
 * 呼び出し時点で lex は開始バックスラッシュの直後を指していること。
 * 戻り値 true = 解釈してエスケープ全体を消費した。
 *          false = 未対応（*marker に診断名を設定し、エスケープ全体は消費済み）。
 * 対応: \n \t \r \v \f \b \a \\ \" \' \0-\777 \xHH \uXXXX \UXXXXXXXX と行継続。
 * 未知のエスケープはバックスラッシュごと保持する（CPythonと同じ挙動）。 */
static bool decode_string_escape(P2C_Lexer *lex, P2C_String *buf, const char **marker) {
    char next = peek_char(lex, 0);
    *marker = NULL;
    switch (next) {
        case 'n': p2c_str_append_char(buf, '\n'); advance(lex); return true;
        case 't': p2c_str_append_char(buf, '\t'); advance(lex); return true;
        case 'r': p2c_str_append_char(buf, '\r'); advance(lex); return true;
        case 'v': p2c_str_append_char(buf, '\v'); advance(lex); return true;
        case 'f': p2c_str_append_char(buf, '\f'); advance(lex); return true;
        case 'b': p2c_str_append_char(buf, '\b'); advance(lex); return true;
        case 'a': p2c_str_append_char(buf, '\a'); advance(lex); return true;
        case '\\': p2c_str_append_char(buf, '\\'); advance(lex); return true;
        case '"': p2c_str_append_char(buf, '"'); advance(lex); return true;
        case '\'': p2c_str_append_char(buf, '\''); advance(lex); return true;
        case '\n':
            /* 行継続: バックスラッシュ+改行は何も生成しない。 */
            lex->line++; lex->col = 0;
            advance(lex);
            return true;
        case 'x': {
            advance(lex);
            int hi = hex_value(peek_char(lex, 0));
            int lo = (hi >= 0) ? hex_value(peek_char(lex, 1)) : -1;
            if (hi < 0 || lo < 0) { *marker = "invalidhexescape"; return false; }
            advance(lex); advance(lex);
            p2c_str_append_char(buf, (char)((hi << 4) | lo));
            return true;
        }
        case 'u': case 'U': {
            int digits = (next == 'u') ? 4 : 8;
            advance(lex);
            uint32_t cp = 0;
            for (int i = 0; i < digits; i++) {
                int hv = hex_value(peek_char(lex, 0));
                if (hv < 0) { *marker = "invalidunicodeescape"; return false; }
                cp = (cp << 4) | (uint32_t)hv;
                advance(lex);
            }
            if (digits == 8 && cp > 0x10FFFFu) { *marker = "invalidunicodeescape"; return false; }
            append_utf8_codepoint(buf, cp);
            return true;
        }
        case 'N': {
            /* \N{NAME} はUnicode名前表を必要とするため未対応。黙って壊れた
             * 文字列を作らず、明示的な診断トークンにする。 */
            advance(lex);
            if (peek_char(lex, 0) == '{') {
                while (lex->pos < lex->source_len && peek_char(lex, 0) != '}' && peek_char(lex, 0) != '\n') advance(lex);
                if (peek_char(lex, 0) == '}') advance(lex);
            }
            *marker = "unicodenamedescape";
            return false;
        }
        default:
            break;
    }
    if (next >= '0' && next <= '7') {
        /* 8進エスケープ: 最大3桁。 */
        int value = 0;
        int count = 0;
        while (count < 3 && peek_char(lex, 0) >= '0' && peek_char(lex, 0) <= '7') {
            value = (value << 3) | (peek_char(lex, 0) - '0');
            advance(lex);
            count++;
        }
        p2c_str_append_char(buf, (char)(value & 0xFF));
        return true;
    }
    if (next == '\0') {
        /* 文字列がバックスラッシュで終端: バックスラッシュだけを残す。 */
        p2c_str_append_char(buf, '\\');
        return true;
    }
    /* 未知のエスケープはバックスラッシュごと保持する（CPythonと同じ）。 */
    p2c_str_append_char(buf, '\\');
    p2c_str_append_char(buf, next);
    advance(lex);
    return true;
}

static P2C_Token* read_string_ex(P2C_Lexer *lex, char quote, bool raw) {
    size_t start = lex->pos;
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->col;
    advance(lex); /* 開始クォート */
    bool triple = (peek_char(lex, 0) == quote && peek_char(lex, 1) == quote);
    if (triple) { advance(lex); advance(lex); }
    
    P2C_String *buf = p2c_str_new(lex->alloc);
    if (!buf) return NULL;
    
    while (lex->pos < lex->source_len) {
        char c = peek_char(lex, 0);
        if (c == quote) {
            if (triple && !(peek_char(lex, 1) == quote && peek_char(lex, 2) == quote)) {
                p2c_str_append_char(buf, c); advance(lex); continue;
            }
            if (triple) { advance(lex); advance(lex); }
            advance(lex);
            P2C_Token *tok = p2c_alloc(lex->alloc, sizeof(P2C_Token));
            if (tok) {
                tok->type = TOK_STR_LITERAL;
                tok->text = p2c_alloc(lex->alloc, p2c_str_len(buf) + 1);
                if (tok->text) memcpy(tok->text, p2c_str_cstr(buf), p2c_str_len(buf) + 1);
                tok->len = p2c_str_len(buf);
                tok->line = start_line;
                tok->col = start_col;
            }
            p2c_str_free(buf);
            return tok;
        } else if (c == '\\' && !raw) {
            advance(lex);
            const char *marker = NULL;
            if (!decode_string_escape(lex, buf, &marker)) {
                p2c_str_free(buf);
                return make_marker_token_at(lex, TOK_UNKNOWN, marker, start_line, start_col);
            }
        } else if (c == '\\' && raw) {
            /* raw文字列: バックスラッシュはそのまま1文字として扱う
             * （ただし閉じクォート直前の \ による誤終端は防ぐため次の1文字も
             * そのまま取り込む。Pythonのraw文字列の実際の仕様に準ずる）。 */
            p2c_str_append_char(buf, c);
            advance(lex);
            if (peek_char(lex, 0)) { p2c_str_append_char(buf, peek_char(lex, 0)); advance(lex); }
        } else if (c == '\n') {
            p2c_str_append_char(buf, c);
            lex->line++; lex->col = 0;
            advance(lex);
        } else {
            p2c_str_append_char(buf, c);
            advance(lex);
        }
    }
    /* 文字列が閉じられていない */
    p2c_str_free(buf);
    return make_token(lex, TOK_UNKNOWN, lex->source + start, lex->pos - start);
}

/* 通常の(非raw)文字列。既存の呼び出し元との後方互換のため残す。 */
static P2C_Token* read_string(P2C_Lexer *lex, char quote) {
    return read_string_ex(lex, quote, false);
}

/* f-string リテラル。read_string とほぼ同じだが、{ と } はプレースホルダの
 * デリミタとしてそのまま保持する（パーサ側で式に分解する）。
 * ただし {{ / }} はPythonの仕様通りエスケープされたリテラルの { / } として1文字に畳む。 */
static P2C_Token* read_fstring(P2C_Lexer *lex, char quote) {
    size_t start = lex->pos;
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->col;
    advance(lex); /* 開始クォート */
    bool f_triple = (peek_char(lex, 0) == quote && peek_char(lex, 1) == quote);
    if (f_triple) { advance(lex); advance(lex); }

    P2C_String *buf = p2c_str_new(lex->alloc);
    if (!buf) return NULL;

    while (lex->pos < lex->source_len) {
        char c = peek_char(lex, 0);
        if (c == quote) {
            if (f_triple && !(peek_char(lex, 1) == quote && peek_char(lex, 2) == quote)) {
                p2c_str_append_char(buf, c); advance(lex); continue;
            }
            if (f_triple) { advance(lex); advance(lex); }
            advance(lex);
            P2C_Token *tok = p2c_alloc(lex->alloc, sizeof(P2C_Token));
            if (tok) {
                tok->type = TOK_FSTRING_LITERAL;
                tok->text = p2c_alloc(lex->alloc, p2c_str_len(buf) + 1);
                if (tok->text) memcpy(tok->text, p2c_str_cstr(buf), p2c_str_len(buf) + 1);
                tok->len = p2c_str_len(buf);
                tok->line = start_line;
                tok->col = start_col;
            }
            p2c_str_free(buf);
            return tok;
        } else if (c == '{' && peek_char(lex, 1) == '{') {
            /* {{ はエスケープされた '{' 一文字を意味するが、ここでは1文字に
             * 潰さずそのまま2文字通す。1文字に潰してしまうと、後段の
             * パーサ側のプレースホルダ検出("{"を見つけたら式の開始とみなす)
             * と区別がつかなくなるため、実際の畳み込みはパーサ側で行う。 */
            p2c_str_append_char(buf, '{'); p2c_str_append_char(buf, '{'); advance(lex); advance(lex);
        } else if (c == '}' && peek_char(lex, 1) == '}') {
            p2c_str_append_char(buf, '}'); p2c_str_append_char(buf, '}'); advance(lex); advance(lex);
        } else if (c == '{') {
            /* プレースホルダ本体はエスケープ処理せずそのまま取り込み、
             * パーサ側で独立した式としてトークナイズし直す。
             * 文字列リテラルのネスト（例: {d["x"]}）にも対応するため、
             * 内側のクォートで囲まれた区間は波括弧の対象外にする。 */
            p2c_str_append_char(buf, '{'); advance(lex);
            char inner_quote = 0;
            int depth = 1;
            while (lex->pos < lex->source_len && depth > 0) {
                char ic = peek_char(lex, 0);
                if (inner_quote) {
                    p2c_str_append_char(buf, ic); advance(lex);
                    if (ic == '\\') { if (lex->pos < lex->source_len) { p2c_str_append_char(buf, peek_char(lex, 0)); advance(lex); } }
                    else if (ic == inner_quote) inner_quote = 0;
                    continue;
                }
                if (ic == '"' || ic == '\'') { inner_quote = ic; p2c_str_append_char(buf, ic); advance(lex); continue; }
                if (ic == '{') depth++;
                if (ic == '}') { depth--; if (depth == 0) { p2c_str_append_char(buf, ic); advance(lex); break; } }
                p2c_str_append_char(buf, ic); advance(lex);
            }
        } else if (c == '\\') {
            advance(lex);
            const char *marker = NULL;
            if (!decode_string_escape(lex, buf, &marker)) {
                p2c_str_free(buf);
                return make_marker_token_at(lex, TOK_UNKNOWN, marker, start_line, start_col);
            }
        } else if (c == '\n') {
            p2c_str_append_char(buf, c);
            lex->line++; lex->col = 0;
            advance(lex);
        } else {
            p2c_str_append_char(buf, c);
            advance(lex);
        }
    }
    p2c_str_free(buf);
    return make_token(lex, TOK_UNKNOWN, lex->source + start, lex->pos - start);
}

/* 数値リテラル */
static P2C_Token* read_number(P2C_Lexer *lex) {
    size_t start = lex->pos;
    uint32_t start_col = lex->col;

    /* 基数つき整数（0x/0o/0b）。Python と同じく '_' 区切りを許す。
     * 表記の正規化（10 進化と '_' の除去）はパーサ側で行う。 */
    if (peek_char(lex, 0) == '0') {
        char p1 = peek_char(lex, 1);
        char low = (p1 >= 'A' && p1 <= 'Z') ? (char)(p1 - 'A' + 'a') : p1;
        if (low == 'x' || low == 'o' || low == 'b') {
            advance(lex);          /* '0' */
            advance(lex);          /* 'x' など */
            bool any = false;
            while (true) {
                char c = peek_char(lex, 0);
                bool ok = false;
                if (low == 'x') ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                else if (low == 'o') ok = (c >= '0' && c <= '7');
                else ok = (c == '0' || c == '1');
                if (ok) { any = true; advance(lex); continue; }
                /* '_' は数字の間にだけ置ける（末尾の '_' は識別子側に残す）。 */
                if (c == '_' && any && is_digit(peek_char(lex, 1))) { advance(lex); continue; }
                break;
            }
            if (!any) return make_marker_token_at(lex, TOK_UNKNOWN, "invalidliteral", lex->line, start_col);
            size_t rlen = lex->pos - start;
            P2C_Token *rtok = make_token(lex, TOK_INT_LITERAL, lex->source + start, rlen);
            if (rtok) rtok->col = start_col;
            return rtok;
        }
    }

    while (is_digit(peek_char(lex, 0)) || (peek_char(lex, 0) == '_' && is_digit(peek_char(lex, 1)))) advance(lex);

    bool is_float = false;
    if (peek_char(lex, 0) == '.' && is_digit(peek_char(lex, 1))) {
        is_float = true;
        advance(lex); /* '.' */
        while (is_digit(peek_char(lex, 0)) || (peek_char(lex, 0) == '_' && is_digit(peek_char(lex, 1)))) advance(lex);
    }

    /* 指数部 */
    if (peek_char(lex, 0) == 'e' || peek_char(lex, 0) == 'E') {
        is_float = true;
        advance(lex);
        if (peek_char(lex, 0) == '+' || peek_char(lex, 0) == '-') advance(lex);
        while (is_digit(peek_char(lex, 0)) || (peek_char(lex, 0) == '_' && is_digit(peek_char(lex, 1)))) advance(lex);
    }
    
    /* 複素数リテラル (2j, 3.5J) は python_code_to_c 非対応。
     * j/J を消費して TOK_UNKNOWN を返しパーサにエラーを出させる
     * （列番号はリテラル全体の開始位置を指すようにする）。 */
    if (peek_char(lex, 0) == 'j' || peek_char(lex, 0) == 'J') {
        advance(lex);
        return make_marker_token_at(lex, TOK_UNKNOWN, "complexliteral", lex->line, start_col);
    }
    size_t len = lex->pos - start;
    P2C_Token *tok = make_token(lex, is_float ? TOK_FLOAT_LITERAL : TOK_INT_LITERAL,
                                 lex->source + start, len);
    if (tok) tok->col = start_col;
    return tok;
}

/* 識別子またはキーワード */
static P2C_Token* read_identifier(P2C_Lexer *lex) {
    size_t start = lex->pos;
    uint32_t start_col = lex->col;
    
    while (is_alnum(peek_char(lex, 0))) advance(lex);
    
    size_t len = lex->pos - start;
    P2C_TokenType ttype = check_keyword(lex->source + start, len);
    
    /* True, False, None は特別 */
    if (len == 4 && strncmp(lex->source + start, "True", 4) == 0) ttype = TOK_BOOL_LITERAL;
    else if (len == 5 && strncmp(lex->source + start, "False", 5) == 0) ttype = TOK_BOOL_LITERAL;
    else if (len == 4 && strncmp(lex->source + start, "None", 4) == 0) ttype = TOK_NONE_LITERAL;
    
    P2C_Token *tok = make_token(lex, ttype, lex->source + start, len);
    if (tok) tok->col = start_col;
    return tok;
}

/* ========================================
 * メイントークン取得
 * ======================================== */

/* DEDENT保留用 */
static int pending_dedents = 0;

static P2C_Token* lexer_next_impl(P2C_Lexer *lex) {
    /* 保留中のDEDENTを処理 */
    if (pending_dedents > 0) {
        pending_dedents--;
        return make_token(lex, TOK_DEDENT, "", 0);
    }
    
    /* EOF前に残りのDEDENTを発行 */
    if (lex->pos >= lex->source_len) {
        if (p2c_vec_len(lex->indent_stack) > 1) {
            p2c_vec_pop(lex->indent_stack);
            return make_token(lex, TOK_DEDENT, "", 0);
        }
        return make_token(lex, TOK_EOF, "", 0);
    }
    
    /* 括弧内の物理改行は論理改行ではない。行頭の空白も
     * インデントとして扱わず、次のトークンまで読み飛ばす。 */
    if (lex->at_line_start && lex->grouping_depth > 0) {
        lex->at_line_start = false;
        while (peek_char(lex, 0) == ' ' || peek_char(lex, 0) == '\t' || peek_char(lex, 0) == '\r') advance(lex);
        if (peek_char(lex, 0) == '\n') {
            advance(lex);
            lex->at_line_start = true;
            return lexer_next_impl(lex);
        }
    }

    /* インデント処理（行頭の場合） */
    if (lex->at_line_start) {
        lex->at_line_start = false;
        
        int indent = count_indent(lex);
        
        /* コメント行または空行は無視して次の行へ */
        if (peek_char(lex, 0) == '#' || peek_char(lex, 0) == '\n' || peek_char(lex, 0) == '\0') {
            /* 行末までスキップ */
            while (lex->pos < lex->source_len && peek_char(lex, 0) != '\n') {
                advance(lex);
            }
            if (peek_char(lex, 0) == '\n') {
                advance(lex);
                lex->at_line_start = true;
            }
            return lexer_next_impl(lex);
        }
        
        /* インデントレベル比較 */
        int *top = (int*)p2c_vec_last(lex->indent_stack);
        if (indent > *top) {
            /* インデント増加 */
            int *new_indent = p2c_alloc(lex->alloc, sizeof(int));
            *new_indent = indent;
            p2c_vec_push(lex->indent_stack, new_indent);
            return make_token(lex, TOK_INDENT, "", 0);
        } else if (indent < *top) {
            /* インデント減少 - 複数のDEDENTを計算 */
            int dedent_count = 0;
            while (p2c_vec_len(lex->indent_stack) > 1) {
                p2c_vec_pop(lex->indent_stack);
                dedent_count++;
                top = (int*)p2c_vec_last(lex->indent_stack);
                if (*top == indent) break;
                if (*top < indent) {
                    /* 不適切なDEDENT */
                    return make_token(lex, TOK_UNKNOWN, "", 0);
                }
            }
            /* 最初のDEDENTを発行、残りは保留 */
            if (dedent_count > 1) {
                pending_dedents = dedent_count - 1;
            }
            return make_token(lex, TOK_DEDENT, "", 0);
        }
    }
    
    skip_whitespace(lex);
    
    if (lex->pos >= lex->source_len) {
        if (p2c_vec_len(lex->indent_stack) > 1) {
            p2c_vec_pop(lex->indent_stack);
            return make_token(lex, TOK_DEDENT, "", 0);
        }
        return make_token(lex, TOK_EOF, "", 0);
    }
    
    char c = peek_char(lex, 0);
    
    /* 改行。括弧内では論理行が継続するためトークンを発行しない。 */
    if (c == '\n') {
        advance(lex);
        lex->at_line_start = true;
        if (lex->grouping_depth > 0) return lexer_next_impl(lex);
        lex->emitted_newline = true;
        return make_token(lex, TOK_NEWLINE, "\n", 1);
    }
    
    /* コメント */
    if (c == '#') {
        skip_line(lex);
        if (peek_char(lex, 0) == '\n') {
            advance(lex);
            lex->emitted_newline = true;
            lex->at_line_start = true;
            return make_token(lex, TOK_NEWLINE, "\n", 1);
        }
        return make_token(lex, TOK_EOF, "", 0);
    }
    
    /* 文字列 */
    if (c == '"' || c == '\'') {
        return read_string(lex, c);
    }

    /* f-string: f"..." / F"...' */
    if ((c == 'f' || c == 'F') && (peek_char(lex, 1) == '"' || peek_char(lex, 1) == '\'')) {
        advance(lex); /* f/F プレフィックスを読み飛ばす */
        char quote = peek_char(lex, 0);
        return read_fstring(lex, quote);
    }

    /* raw文字列: r"..." / R"..."（エスケープ処理をしない） */
    if ((c == 'r' || c == 'R') && (peek_char(lex, 1) == '"' || peek_char(lex, 1) == '\'')) {
        advance(lex); /* r/R プレフィックスを読み飛ばす */
        char quote = peek_char(lex, 0);
        return read_string_ex(lex, quote, true);
    }

    /* u"..." / U"...": Python3では単なる文字列と同義（Python2互換のマーカー） */
    if ((c == 'u' || c == 'U') && (peek_char(lex, 1) == '"' || peek_char(lex, 1) == '\'')) {
        advance(lex);
        char quote = peek_char(lex, 0);
        return read_string_ex(lex, quote, false);
    }

    /* bytes文字列: b"..." / B"..." は python_code_to_c 未対応（bytes型自体がない）。
     * 未対応の識別子 'b' として素通りさせ意味不明な "undefined name 'b'"
     * エラーになるのを避けるため、ここで明示的に検出してエラーにする。
     * リテラル全体（閉じクォートまで）を読み飛ばしてから、開始位置を
     * 指すエラートークンを返す。 */
    if ((c == 'b' || c == 'B') && (peek_char(lex, 1) == '"' || peek_char(lex, 1) == '\'')) {
        uint32_t berr_line = lex->line, berr_col = lex->col;
        advance(lex); /* b/B */
        char bquote = peek_char(lex, 0);
        /* 中身は捨ててよいので read_string_ex の結果は破棄し、位置だけ使う */
        P2C_Token *discarded = read_string_ex(lex, bquote, true);
        (void)discarded;
        return make_marker_token_at(lex, TOK_UNKNOWN, "bytesliteral", berr_line, berr_col);
    }
    
    /* 数値 */
    if (is_digit(c)) {
        return read_number(lex);
    }
    
    /* 識別子/キーワード */
    if (is_alpha(c)) {
        return read_identifier(lex);
    }
    
    /* 演算子とデリミタ */
    size_t start = lex->pos;
    advance(lex);
    
    switch (c) {
        case '+':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_PLUS_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_PLUS, "+", 1);
        case '-':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_MINUS_ASSIGN, lex->source+start, 2); }
            if (peek_char(lex, 0) == '>') { advance(lex); return make_token(lex, TOK_ARROW, "->", 2); }
            return make_token(lex, TOK_MINUS, "-", 1);
        case '*':
            if (peek_char(lex, 0) == '*') { 
                advance(lex); 
                if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_DBL_STAR_ASSIGN, lex->source+start, 3); }
                return make_token(lex, TOK_DBL_STAR, "**", 2); 
            }
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_STAR_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_STAR, "*", 1);
        case '/':
            if (peek_char(lex, 0) == '/') {
                advance(lex);
                if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_DBL_SLASH_ASSIGN, lex->source+start, 3); }
                return make_token(lex, TOK_DBL_SLASH, "//", 2);
            }
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_SLASH_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_SLASH, "/", 1);
        case '%':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_PERCENT_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_PERCENT, "%", 1);
        case '@':
            return make_token(lex, TOK_AT, "@", 1);
        case '&':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_AMP_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_AMPERSAND, "&", 1);
        case '|':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_PIPE_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_PIPE, "|", 1);
        case '^':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_CARET_ASSIGN, lex->source+start, 2); }
            return make_token(lex, TOK_CARET, "^", 1);
        case '~':
            return make_token(lex, TOK_TILDE, "~", 1);
        case '<':
            if (peek_char(lex, 0) == '<') { 
                advance(lex);
                if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_LSHIFT_ASSIGN, lex->source+start, 3); }
                return make_token(lex, TOK_LSHIFT, "<<", 2); 
            }
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_LE, "<=", 2); }
            if (peek_char(lex, 0) == '>') { advance(lex); return make_token(lex, TOK_NE, "<>", 2); }
            return make_token(lex, TOK_LT, "<", 1);
        case '>':
            if (peek_char(lex, 0) == '>') { 
                advance(lex);
                if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_RSHIFT_ASSIGN, lex->source+start, 3); }
                return make_token(lex, TOK_RSHIFT, ">>", 2); 
            }
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_GE, ">=", 2); }
            return make_token(lex, TOK_GT, ">", 1);
        case '=':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_EQ, "==", 2); }
            return make_token(lex, TOK_ASSIGN, "=", 1);
        case '!':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_NE, "!=", 2); }
            return make_token(lex, TOK_UNKNOWN, "!", 1);
        case '(':
            lex->grouping_depth++;
            return make_token(lex, TOK_LPAREN, "(", 1);
        case ')':
            if (lex->grouping_depth > 0) lex->grouping_depth--;
            return make_token(lex, TOK_RPAREN, ")", 1);
        case '[':
            lex->grouping_depth++;
            return make_token(lex, TOK_LBRACKET, "[", 1);
        case ']':
            if (lex->grouping_depth > 0) lex->grouping_depth--;
            return make_token(lex, TOK_RBRACKET, "]", 1);
        case '{':
            lex->grouping_depth++;
            return make_token(lex, TOK_LBRACE, "{", 1);
        case '}':
            if (lex->grouping_depth > 0) lex->grouping_depth--;
            return make_token(lex, TOK_RBRACE, "}", 1);
        case ',':
            return make_token(lex, TOK_COMMA, ",", 1);
        case ':':
            if (peek_char(lex, 0) == '=') { advance(lex); return make_token(lex, TOK_WALRUS, ":=", 2); }
            return make_token(lex, TOK_COLON, ":", 1);
        case '.':
            if (peek_char(lex, 0) == '.' && peek_char(lex, 1) == '.') {
                advance(lex); advance(lex);
                return make_token(lex, TOK_ELLIPSIS, "...", 3);
            }
            return make_token(lex, TOK_DOT, ".", 1);
        case ';':
            return make_token(lex, TOK_SEMICOLON, ";", 1);
        default:
            return make_token(lex, TOK_UNKNOWN, lex->source + start, 1);
    }
}

/* ========================================
 * 公開API
 * ======================================== */

P2C_Token* p2c_lexer_next(P2C_Lexer *lex) {
    if (!lex) return NULL;
    
    /* 先行トークンがあればそれを返す */
    if (lex->has_peek) {
        lexer_retire(lex, lex->current);
        lex->has_peek = false;
        P2C_Token *tok = lex->peek;
        lex->peek = NULL;
        lex->current = tok;
        /* 2つ先の先読みがキャッシュされていれば、1つ先へ繰り上げる */
        if (lex->has_peek2) {
            lex->peek = lex->peek2;
            lex->has_peek = true;
            lex->peek2 = NULL;
            lex->has_peek2 = false;
        }
        return tok;
    }
    
    lexer_retire(lex, lex->current);
    lex->current = lexer_next_impl(lex);
    return lex->current;
}

P2C_Token* p2c_lexer_peek(P2C_Lexer *lex) {
    if (!lex) return NULL;
    if (lex->has_peek) return lex->peek;
    
    lex->peek = lexer_next_impl(lex);
    lex->has_peek = true;
    return lex->peek;
}

P2C_Token* p2c_lexer_peek2(P2C_Lexer *lex) {
    if (!lex) return NULL;
    if (lex->has_peek2) return lex->peek2;
    /* 1つ先(peek)がまだ確定していなければ先に確定させる */
    if (!lex->has_peek) {
        lex->peek = lexer_next_impl(lex);
        lex->has_peek = true;
    }
    lex->peek2 = lexer_next_impl(lex);
    lex->has_peek2 = true;
    return lex->peek2;
}

bool p2c_lexer_consume(P2C_Lexer *lex, P2C_TokenType type) {
    P2C_Token *tok = p2c_lexer_peek(lex);
    if (tok && tok->type == type) {
        p2c_lexer_next(lex);
        return true;
    }
    return false;
}

P2C_Token* p2c_lexer_expect(P2C_Lexer *lex, P2C_TokenType type, P2C_Result *out_err) {
    P2C_Token *tok = p2c_lexer_next(lex);
    if (!tok || tok->type != type) {
        if (out_err) *out_err = P2C_ERR_SYNTAX;
        return NULL;
    }
    if (out_err) *out_err = P2C_OK;
    return tok;
}

void p2c_token_free(P2C_Token *tok, P2C_Allocator *a) {
    if (!tok) return;
    if (tok->text) p2c_free(a, tok->text);
    p2c_free(a, tok);
}

P2C_Token* p2c_token_clone(P2C_Token *tok, P2C_Allocator *a) {
    if (!tok) return NULL;
    P2C_Token *c = p2c_alloc(a, sizeof(P2C_Token));
    if (!c) return NULL;
    *c = *tok;
    if (tok->text && tok->len > 0) {
        c->text = p2c_alloc(a, tok->len + 1);
        if (c->text) memcpy(c->text, tok->text, tok->len + 1);
    } else {
        c->text = NULL;
    }
    return c;
}

const char* p2c_token_type_name(P2C_TokenType type) {
    switch (type) {
        case TOK_INT_LITERAL: return "INT_LITERAL";
        case TOK_FLOAT_LITERAL: return "FLOAT_LITERAL";
        case TOK_STR_LITERAL: return "STR_LITERAL";
        case TOK_BOOL_LITERAL: return "BOOL_LITERAL";
        case TOK_NONE_LITERAL: return "NONE_LITERAL";
        case TOK_IDENTIFIER: return "IDENTIFIER";
        case TOK_KW_AND: return "and";
        case TOK_KW_AS: return "as";
        case TOK_KW_ASYNC: return "async";
        case TOK_KW_AWAIT: return "await";
        case TOK_KW_ASSERT: return "assert";
        case TOK_KW_BREAK: return "break";
        case TOK_KW_CLASS: return "class";
        case TOK_KW_CASE: return "case";
        case TOK_KW_MATCH: return "match";
        case TOK_KW_CONTINUE: return "continue";
        case TOK_KW_DEF: return "def";
        case TOK_KW_DEL: return "del";
        case TOK_KW_ELIF: return "elif";
        case TOK_KW_ELSE: return "else";
        case TOK_KW_EXCEPT: return "except";
        case TOK_KW_FINALLY: return "finally";
        case TOK_KW_FOR: return "for";
        case TOK_KW_FROM: return "from";
        case TOK_KW_GLOBAL: return "global";
        case TOK_KW_NONLOCAL: return "nonlocal";
        case TOK_KW_IF: return "if";
        case TOK_KW_IMPORT: return "import";
        case TOK_KW_IN: return "in";
        case TOK_KW_IS: return "is";
        case TOK_KW_LAMBDA: return "lambda";
        case TOK_KW_NOT: return "not";
        case TOK_KW_OR: return "or";
        case TOK_KW_PASS: return "pass";
        case TOK_KW_RAISE: return "raise";
        case TOK_KW_RETURN: return "return";
        case TOK_KW_TRY: return "try";
        case TOK_KW_WHILE: return "while";
        case TOK_KW_WITH: return "with";
        case TOK_KW_YIELD: return "yield";
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_DBL_SLASH: return "//";
        case TOK_PERCENT: return "%";
        case TOK_DBL_STAR: return "**";
        case TOK_AT: return "@";
        case TOK_LSHIFT: return "<<";
        case TOK_RSHIFT: return ">>";
        case TOK_AMPERSAND: return "&";
        case TOK_PIPE: return "|";
        case TOK_CARET: return "^";
        case TOK_TILDE: return "~";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LE: return "<=";
        case TOK_GE: return ">=";
        case TOK_EQ: return "==";
        case TOK_NE: return "!=";
        case TOK_ASSIGN: return "=";
        case TOK_WALRUS: return ":=";
        case TOK_PLUS_ASSIGN: return "+=";
        case TOK_MINUS_ASSIGN: return "-=";
        case TOK_STAR_ASSIGN: return "*=";
        case TOK_SLASH_ASSIGN: return "/=";
        case TOK_DBL_SLASH_ASSIGN: return "//=";
        case TOK_PERCENT_ASSIGN: return "%=";
        case TOK_DBL_STAR_ASSIGN: return "**=";
        case TOK_LSHIFT_ASSIGN: return "<<=";
        case TOK_RSHIFT_ASSIGN: return ">>=";
        case TOK_AMP_ASSIGN: return "&=";
        case TOK_PIPE_ASSIGN: return "|=";
        case TOK_CARET_ASSIGN: return "^=";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_LBRACKET: return "[";
        case TOK_RBRACKET: return "]";
        case TOK_LBRACE: return "{";
        case TOK_RBRACE: return "}";
        case TOK_COMMA: return ",";
        case TOK_COLON: return ":";
        case TOK_DOT: return ".";
        case TOK_SEMICOLON: return ";";
        case TOK_ARROW: return "->";
        case TOK_ELLIPSIS: return "...";
        case TOK_INDENT: return "INDENT";
        case TOK_DEDENT: return "DEDENT";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_EOF: return "EOF";
        case TOK_COMMENT: return "COMMENT";
        case TOK_UNKNOWN: return "UNKNOWN";
        default: return "?";
    }
}
