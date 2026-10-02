#ifndef PYTHON_CODE_TO_C_HTTPD_H
#define PYTHON_CODE_TO_C_HTTPD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * python_code_to_cの「本物のGUI」用、最小限のローカル開発サーバー。
 * 外部ライブラリに依存せず、POSIXソケットのみで実装している
 * （GTK/Qt/SDL等はこのビルド環境に開発ヘッダが無く、そもそも
 * hosted/hobby OS どちらでも重い依存を増やしたくないという方針にも
 * 合わないため、ブラウザをフロントエンドにする設計にしている）。
 *
 * 同時に1接続のみを処理する単純なブロッキングループ。ローカルの
 * ブラウザ1つ相手の開発ツールとして使うには十分。
 */

typedef struct {
    char method[8];        /* "GET" / "POST" */
    char path[512];         /* 例: "/api/python_code_to_c"（クエリ文字列は含まない） */
    char *body;             /* POSTボディ（無ければNULL）。呼び出し側はfree不要
                              * （ハンドラ呼び出しが終わるとhttpd側が解放する） */
    size_t body_len;
} P2C_HttpRequest;

/* リクエストを受け取り、必要なら p2c_httpd_send_response() で応答を書く。 */
typedef void (*P2C_HttpHandler)(const P2C_HttpRequest *req, int client_fd, void *ctx);

/*
 * host:port で listen し、リクエストが来るたびに handler を呼ぶブロッキング
 * ループに入る（Ctrl-Cで終了するまで戻らない）。
 * host には通常 "127.0.0.1" を指定する（外部ネットワークに公開しない
 * デフォルト。生成したコードのコンパイル・実行までできるツールなので、
 * うっかり外部公開しないことが重要）。
 * bind/listenに失敗した場合は 0 以外を返してすぐ戻る。
 */
int p2c_httpd_serve(const char *host, int port, P2C_HttpHandler handler, void *ctx);

/* ハンドラ内から呼ぶ: 完全なHTTPレスポンスを書いてよい形式に整形して送る。
 * body_len に (size_t)-1 を渡すと strlen(body) を使う（NUL終端文字列用）。 */
void p2c_httpd_send_response(int client_fd, int status_code, const char *status_text,
                              const char *content_type, const char *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif
