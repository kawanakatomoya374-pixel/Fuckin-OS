#ifndef PYTHON_CODE_TO_C_HTTPD_H
#define PYTHON_CODE_TO_C_HTTPD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * python_code_to_c の Web GUI 用の、最小限のローカル開発サーバー。
 * 外部ライブラリに依存せず、POSIX ソケットと fork(2) だけで実装している。
 *
 * 動作の要点:
 *   - 接続ごとに子プロセスで処理する。1つの遅い接続が他の利用者の応答を止めない。
 *     同時処理数は max_connections までで、上限に達したら空きが出るまで accept を待つ。
 *   - 1リクエスト（ヘッダ＋ボディ）の受信には全体の期限（request_timeout_ms）を設ける。
 *     受信のたびにタイムアウトを数える方式では、少しずつ送り続けるクライアントが
 *     接続を際限なく占有できてしまうため。
 *   - ボディが Content-Length まで届かなかった場合はハンドラを呼ばず 400 を返す
 *     （途中で切れた本文を処理しない）。
 *   - Transfer-Encoding は受け付けない（501）。Content-Length の重複・不正は 400。
 *   - 応答ヘッダが固定長バッファに収まらない場合は送らず 500 にする
 *     （切り詰めた長さで送ると、バッファの外を読んでしまうため）。
 *
 * これは開発用の仕組みであり、インターネットに直接公開する用途は想定していない。
 * 公開の可否と防御（トークン・送信元検査）は呼び出し側（gui_web）が決める。
 */

typedef struct {
    char method[8];         /* "GET" / "POST" など */
    char path[512];         /* 例: "/api/run"（クエリ文字列は含まない） */
    char host[256];         /* Host ヘッダの値（無ければ空文字列） */
    char origin[256];       /* Origin ヘッダの値（無ければ空文字列） */
    int has_origin;         /* Origin ヘッダが存在したか（ブラウザ経由かどうかの判定に使う） */
    char token[160];        /* X-P2C-Token ヘッダの値（無ければ空文字列） */
    char *body;             /* リクエストボディ（無ければ NULL）。解放は httpd 側が行う */
    size_t body_len;
} P2C_HttpRequest;

/* リクエストを受け取り、p2c_httpd_send_response() で応答を書く。 */
typedef void (*P2C_HttpHandler)(const P2C_HttpRequest *req, int client_fd, void *ctx);

typedef struct {
    int max_connections;      /* 同時に処理する接続数の上限（既定 16） */
    int request_timeout_ms;   /* 1リクエストの受信期限。ヘッダとボディの合計（既定 15000） */
    size_t max_body_bytes;    /* リクエストボディの上限。超えたら 413（既定 4 MiB） */
    int handler_timeout_s;    /* 子プロセスの絶対上限（秒）。ハンドラが止まっても解放する（既定 120） */
} P2C_HttpLimits;

/* 既定値を設定する。 */
void p2c_httpd_limits_default(P2C_HttpLimits *limits);

/*
 * host:port で listen し、接続ごとに handler を呼ぶ（Ctrl-C で終了するまで戻らない）。
 * limits が NULL なら既定値を使う。
 * 戻り値: 1〜4 は socket/bind/listen の失敗（すぐ戻る）。
 */
int p2c_httpd_serve_ex(const char *host, int port, const P2C_HttpLimits *limits,
                       P2C_HttpHandler handler, void *ctx);

/* p2c_httpd_serve_ex(host, port, NULL, handler, ctx) と同じ。 */
int p2c_httpd_serve(const char *host, int port, P2C_HttpHandler handler, void *ctx);

/* ハンドラ内から呼ぶ: 完全な HTTP 応答を送る。body_len に (size_t)-1 を渡すと strlen(body)。 */
void p2c_httpd_send_response(int client_fd, int status_code, const char *status_text,
                              const char *content_type, const char *body, size_t body_len);

#ifdef __cplusplus
}
#endif

#endif
