#include "tools/python_code_to_c_httpd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>

/* 1リクエストの上限（ヘッダ+ボディ合計）。ローカル開発ツールなので
 * 巨大にする必要はないが、それなりのサイズのPython/Cソースは
 * 受け付けられるようにしておく。 */
#define P2C_HTTPD_MAX_REQUEST (16 * 1024 * 1024)
#define P2C_HTTPD_MAX_HEADERS (64 * 1024)

/* strncasecmpがNO_STDLIB環境には無いのでこのファイル内だけの小さなヘルパー
 * を用意する（このhttpdはhostedのみで使う想定のため、ここは通常のlibcが
 * 前提。<strings.h>のstrncasecmpをそのまま使ってもよいが、環境依存の
 * ヘッダを増やしたくないため自前で書く）。 */
static int strncasecmp_local(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return ca - cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

/* 末尾を必ずNUL終端する有界コピー。
 * 以前は strncpy(dst, src, sizeof(dst) - 1) + 明示NUL を使っていたが、
 * 「境界がちょうどコピー元の文字列長」になる形のとき GCC が
 * -Wstringop-truncation を出し得る（ASan/UBSan併用の -O2 ビルドで顕在化する）。
 * 長さを明示した memcpy に置き換え、切り詰めと終端を1箇所で表す。 */
static void copy_bounded(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    size_t len = src ? strlen(src) : 0;
    if (len > dst_size - 1) len = dst_size - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ソケットから、ヘッダ終端("\r\n\r\n")が見つかるまで読み込む。
 * 見つかったらヘッダ部分の長さを返し、bodyの先頭が既に読めていれば
 * *out_body_start / *out_prefetched にその分も渡す。 */
static ssize_t read_headers(int fd, char *buf, size_t cap, size_t *header_end) {
    size_t total = 0;
    while (total < cap - 1) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break; /* 接続が閉じられた */
        total += (size_t)n;
        buf[total] = '\0';
        char *hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            *header_end = (size_t)(hdr_end - buf) + 4;
            return (ssize_t)total;
        }
        if (total >= P2C_HTTPD_MAX_HEADERS) return -1; /* ヘッダが大きすぎる */
    }
    return -1; /* ヘッダ終端が見つからないまま上限に達した */
}

static long parse_content_length(const char *headers) {
    const char *p = headers;
    while (*p) {
        if ((p == headers || *(p - 1) == '\n') &&
            (strncasecmp_local(p, "Content-Length:", 15) == 0)) {
            p += 15;
            while (*p == ' ' || *p == '\t') p++;
            return atol(p);
        }
        p++;
    }
    return -1;
}

void p2c_httpd_send_response(int client_fd, int status_code, const char *status_text,
                              const char *content_type, const char *body, size_t body_len) {
    if (body_len == (size_t)-1) body_len = body ? strlen(body) : 0;
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "\r\n",
        status_code, status_text ? status_text : "OK",
        content_type ? content_type : "text/plain; charset=utf-8",
        body_len);
    if (hlen > 0) {
        ssize_t off = 0;
        size_t hlen_sz = (size_t)hlen;
        while ((size_t)off < hlen_sz) {
            ssize_t n = send(client_fd, header + off, hlen_sz - (size_t)off, 0);
            if (n <= 0) return;
            off += n;
        }
    }
    size_t off = 0;
    while (off < body_len) {
        ssize_t n = send(client_fd, body + off, body_len - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

int p2c_httpd_serve(const char *host, int port, P2C_HttpHandler handler, void *ctx) {
    signal(SIGPIPE, SIG_IGN); /* クライアントが途中で切断してもプロセスを落とさない */

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return 1;

    int yes = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!host || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(srv_fd);
        return 2;
    }

    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(srv_fd);
        return 3;
    }
    if (listen(srv_fd, 16) < 0) {
        close(srv_fd);
        return 4;
    }

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        /* 読み込みが止まったクライアントにいつまでも待たされないよう
         * タイムアウトを設定する（ローカル開発ツールとしての防御的措置）。 */
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char *buf = malloc(P2C_HTTPD_MAX_HEADERS + 1);
        if (!buf) { close(client_fd); continue; }

        size_t header_end = 0;
        ssize_t total = read_headers(client_fd, buf, P2C_HTTPD_MAX_HEADERS + 1, &header_end);
        if (total < 0) {
            p2c_httpd_send_response(client_fd, 400, "Bad Request", "text/plain", "bad request", (size_t)-1);
            free(buf);
            close(client_fd);
            continue;
        }

        P2C_HttpRequest req;
        memset(&req, 0, sizeof(req));

        /* リクエスト行のパース: "METHOD /path HTTP/1.1" */
        char method_buf[8] = {0};
        char path_buf[512] = {0};
        sscanf(buf, "%7s %511s", method_buf, path_buf);
        copy_bounded(req.method, sizeof(req.method), method_buf);
        /* クエリ文字列を切り落とす */
        char *qmark = strchr(path_buf, '?');
        if (qmark) *qmark = '\0';
        copy_bounded(req.path, sizeof(req.path), path_buf);

        long content_length = parse_content_length(buf);
        size_t already = (size_t)total - header_end;

        if (content_length > 0 && content_length < P2C_HTTPD_MAX_REQUEST) {
            size_t need = (size_t)content_length;
            char *body = malloc(need + 1);
            if (body) {
                size_t have = already < need ? already : need;
                memcpy(body, buf + header_end, have);
                size_t got = have;
                while (got < need) {
                    ssize_t n = recv(client_fd, body + got, need - got, 0);
                    if (n <= 0) break;
                    got += (size_t)n;
                }
                body[got] = '\0';
                req.body = body;
                req.body_len = got;
            }
        }

        handler(&req, client_fd, ctx);

        free(req.body);
        free(buf);
        close(client_fd);
    }
}
