#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "tools/python_code_to_c_httpd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* 受信ヘッダ（リクエスト行＋ヘッダ全体）の上限。 */
#define P2C_HTTPD_MAX_HEADERS (64u * 1024u)
/* 応答ヘッダの固定長バッファ。これに収まらない応答は送らない（500 に置き換える）。 */
#define P2C_HTTPD_RESP_HDR_MAX 1024u
/* 送信の1回あたりの期限（秒）。相手が読まなくなった接続で子プロセスを止めないため。 */
#define P2C_HTTPD_SEND_TIMEOUT_S 30

#ifdef MSG_NOSIGNAL
#define P2C_SEND_FLAGS MSG_NOSIGNAL
#else
#define P2C_SEND_FLAGS 0
#endif

void p2c_httpd_limits_default(P2C_HttpLimits *limits) {
    if (!limits) return;
    limits->max_connections = 16;
    limits->request_timeout_ms = 15000;
    limits->max_body_bytes = (size_t)4u * 1024u * 1024u;
    limits->handler_timeout_s = 120;
}

/* 単調増加時計（ミリ秒）。受信の全体期限の判定に使う。 */
static long long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/* 大文字小文字を無視して n 文字を比較する（HTTP のヘッダ名の比較用）。 */
static int ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = (unsigned char)a[i];
        int cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return 0;
    }
    return 1;
}

/* len バイトを dst（容量 cap、必ずNUL終端）へ切り詰めつきでコピーする。 */
static void copy_bounded(char *dst, size_t cap, const char *src, size_t len) {
    if (!dst || cap == 0) return;
    if (len > cap - 1) len = cap - 1;
    if (len > 0 && src) memcpy(dst, src, len);
    dst[len] = '\0';
}

/* 全バイトを送る。送信期限切れ・切断は -1。 */
static int send_all(int fd, const char *p, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, P2C_SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* fd が読めるまで、期限 deadline（now_ms の基準）まで待つ。1=読める 0=期限切れ -1=エラー */
static int wait_readable(int fd, long long deadline) {
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) return 0;
        if (left > 2000000000LL) left = 2000000000LL;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int r = poll(&pfd, 1, (int)left);
        if (r > 0) return 1;
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* 期限付きの受信。戻り値: >0 受信バイト数, 0 相手が閉じた, -2 期限切れ, -1 エラー */
static ssize_t recv_deadline(int fd, char *buf, size_t cap, long long deadline) {
    for (;;) {
        int w = wait_readable(fd, deadline);
        if (w == 0) return -2;
        if (w < 0) return -1;
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        return n;
    }
}

/* buf[from..len) の中から "\r\n\r\n" の終端位置（直後のオフセット）を探す。無ければ 0。 */
static size_t find_header_end(const char *buf, size_t len, size_t from) {
    if (len < 4) return 0;
    size_t start = from >= 3 ? from - 3 : 0;
    for (size_t i = start; i + 4 <= len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return 0;
}

/* 値の前後の空白（SP/HT）と CR/LF を取り除いた範囲を返す。 */
static void trim_range(const char **p, const char **end) {
    while (*p < *end && (**p == ' ' || **p == '\t')) (*p)++;
    while (*end > *p && ((*end)[-1] == ' ' || (*end)[-1] == '\t' || (*end)[-1] == '\r')) (*end)--;
}

typedef struct {
    int has_cl;
    size_t content_length;
    int chunked;
} P2C_HeadInfo;

/* 解析エラーの HTTP ステータスとメッセージ */
typedef struct {
    int status;
    const char *text;
} P2C_HttpErr;

/*
 * ヘッダ部（末尾の空行を含まない）を解析して req と info へ格納する。
 * 失敗時は err に応答すべき状態を入れて 0 を返す。
 */
static int parse_head(const char *hdr, size_t hdr_len, P2C_HttpRequest *req, P2C_HeadInfo *info,
                      P2C_HttpErr *err) {
    const char *end = hdr + hdr_len;
    const char *line_end = end; /* リクエスト行の終わり（CRLF の位置） */
    for (const char *q = hdr; q + 1 < end; q++) {
        if (q[0] == '\r' && q[1] == '\n') {
            line_end = q;
            break;
        }
    }

    /* リクエスト行: METHOD SP target SP HTTP/1.x */
    const char *sp1 = memchr(hdr, ' ', (size_t)(line_end - hdr));
    if (!sp1 || sp1 == hdr || (size_t)(sp1 - hdr) >= sizeof(req->method)) {
        err->status = 400;
        err->text = "bad request line";
        return 0;
    }
    copy_bounded(req->method, sizeof(req->method), hdr, (size_t)(sp1 - hdr));

    const char *target = sp1 + 1;
    const char *sp2 = memchr(target, ' ', (size_t)(line_end - target));
    if (!sp2 || sp2 == target || *target != '/') {
        err->status = 400;
        err->text = "bad request line";
        return 0;
    }
    if ((size_t)(line_end - (sp2 + 1)) < 8 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        err->status = 400;
        err->text = "unsupported HTTP version";
        return 0;
    }
    const char *q = memchr(target, '?', (size_t)(sp2 - target));
    const char *path_end = q ? q : sp2;
    if ((size_t)(path_end - target) >= sizeof(req->path)) {
        err->status = 414;
        err->text = "request target too long";
        return 0;
    }
    copy_bounded(req->path, sizeof(req->path), target, (size_t)(path_end - target));

    if (line_end == end) return 1; /* ヘッダ行が無い */

    /* ヘッダ行: "Name: value" を1行ずつ読む。obs-fold（行頭の空白）は拒否する。 */
    const char *p = line_end + 2;
    while (p < end) {
        const char *le = end;
        for (const char *r = p; r + 1 < end; r++) {
            if (r[0] == '\r' && r[1] == '\n') {
                le = r;
                break;
            }
        }
        if (le == p) break;
        if (*p == ' ' || *p == '\t') {
            err->status = 400;
            err->text = "obsolete header folding is not supported";
            return 0;
        }
        const char *colon = memchr(p, ':', (size_t)(le - p));
        if (!colon || colon == p) {
            err->status = 400;
            err->text = "malformed header";
            return 0;
        }
        size_t name_len = (size_t)(colon - p);
        const char *vp = colon + 1;
        const char *ve = le;
        trim_range(&vp, &ve);
        size_t vlen = (size_t)(ve - vp);

        if (name_len == 4 && ieq_n(p, "host", 4)) {
            copy_bounded(req->host, sizeof(req->host), vp, vlen);
        } else if (name_len == 6 && ieq_n(p, "origin", 6)) {
            copy_bounded(req->origin, sizeof(req->origin), vp, vlen);
            req->has_origin = 1;
        } else if (name_len == 11 && ieq_n(p, "x-p2c-token", 11)) {
            copy_bounded(req->token, sizeof(req->token), vp, vlen);
        } else if (name_len == 14 && ieq_n(p, "content-length", 14)) {
            size_t v = 0;
            if (vlen == 0 || vlen > 18) {
                err->status = 400;
                err->text = "invalid Content-Length";
                return 0;
            }
            for (size_t i = 0; i < vlen; i++) {
                if (vp[i] < '0' || vp[i] > '9') {
                    err->status = 400;
                    err->text = "invalid Content-Length";
                    return 0;
                }
                v = v * 10u + (size_t)(vp[i] - '0');
            }
            /* 複数の Content-Length で値が食い違うと、本文の境界が曖昧になる。 */
            if (info->has_cl && info->content_length != v) {
                err->status = 400;
                err->text = "conflicting Content-Length headers";
                return 0;
            }
            info->has_cl = 1;
            info->content_length = v;
        } else if (name_len == 17 && ieq_n(p, "transfer-encoding", 17)) {
            info->chunked = 1;
        }
        p = le + 2;
    }
    return 1;
}

/* 拒否の応答を送り、残りの入力を少しだけ読み捨ててから閉じる。
 * 読み残しのまま close すると TCP が RST を送り、クライアントが応答を読めないことがあるため。 */
static void reject_and_drain(int fd, int status, const char *status_text, const char *msg) {
    p2c_httpd_send_response(fd, status, status_text, "text/plain; charset=utf-8", msg, (size_t)-1);
    shutdown(fd, SHUT_WR);
    char sink[4096];
    size_t drained = 0;
    long long until = now_ms() + 500;
    while (drained < (size_t)P2C_HTTPD_MAX_HEADERS) {
        if (wait_readable(fd, until) != 1) break;
        ssize_t n = recv(fd, sink, sizeof(sink), 0);
        if (n <= 0) break;
        drained += (size_t)n;
    }
}

/* 1接続を処理する（子プロセス内で呼ばれる）。fd は呼び出し側が閉じる。 */
static void handle_connection(int fd, const P2C_HttpLimits *lim, P2C_HttpHandler handler, void *ctx) {
    long long deadline = now_ms() + (long long)lim->request_timeout_ms;
    struct timeval tv;
    tv.tv_sec = P2C_HTTPD_SEND_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char *buf = (char*)malloc((size_t)P2C_HTTPD_MAX_HEADERS + 1u);
    if (!buf) {
        reject_and_drain(fd, 503, "Service Unavailable", "out of memory");
        return;
    }

    P2C_HttpErr err = { 0, NULL };
    size_t total = 0;
    size_t hdr_end = 0;
    for (;;) {
        if (total >= (size_t)P2C_HTTPD_MAX_HEADERS) {
            err.status = 431;
            err.text = "request headers too large";
            break;
        }
        ssize_t n = recv_deadline(fd, buf + total, (size_t)P2C_HTTPD_MAX_HEADERS - total, deadline);
        if (n == -2) {
            err.status = 408;
            err.text = "request timeout";
            break;
        }
        if (n <= 0) break; /* 相手が閉じた・エラー: 応答できる相手がいない */
        size_t scan_from = total;
        total += (size_t)n;
        hdr_end = find_header_end(buf, total, scan_from);
        if (hdr_end) break;
    }
    buf[total] = '\0';

    if (err.status == 0 && hdr_end == 0) {
        free(buf);
        return; /* 相手が閉じただけ */
    }
    if (err.status != 0) {
        reject_and_drain(fd, err.status, err.status == 408 ? "Request Timeout" : "Error", err.text);
        free(buf);
        return;
    }

    P2C_HttpRequest req;
    memset(&req, 0, sizeof(req));
    P2C_HeadInfo info;
    memset(&info, 0, sizeof(info));
    /* 末尾の空行（\r\n\r\n）は解析に含めない。 */
    size_t head_len = hdr_end - 4;
    if (!parse_head(buf, head_len, &req, &info, &err)) {
        reject_and_drain(fd, err.status, "Error", err.text);
        free(buf);
        return;
    }
    if (info.chunked) {
        reject_and_drain(fd, 501, "Not Implemented", "chunked transfer encoding is not supported");
        free(buf);
        return;
    }
    const int body_expected = strcmp(req.method, "POST") == 0 || strcmp(req.method, "PUT") == 0 ||
                              strcmp(req.method, "PATCH") == 0;
    if (body_expected && !info.has_cl) {
        reject_and_drain(fd, 411, "Length Required", "Content-Length is required");
        free(buf);
        return;
    }
    if (info.content_length > lim->max_body_bytes) {
        reject_and_drain(fd, 413, "Payload Too Large", "request body too large");
        free(buf);
        return;
    }

    const size_t need = info.content_length;
    if (need > 0) {
        char *body = (char*)malloc(need + 1u);
        if (!body) {
            reject_and_drain(fd, 503, "Service Unavailable", "out of memory");
            free(buf);
            return;
        }
        size_t have = total - hdr_end;
        if (have > need) have = need; /* パイプライン送信の余分は読まない */
        if (have > 0) memcpy(body, buf + hdr_end, have);
        size_t got = have;
        while (got < need) {
            ssize_t n = recv_deadline(fd, body + got, need - got, deadline);
            if (n == -2) {
                free(body);
                reject_and_drain(fd, 408, "Request Timeout", "request timeout");
                free(buf);
                return;
            }
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got < need) {
            /* 本文が途中で切れた。処理せず 400 を返す（切れた入力を実行しないため）。 */
            free(body);
            reject_and_drain(fd, 400, "Bad Request", "incomplete request body");
            free(buf);
            return;
        }
        body[need] = '\0';
        req.body = body;
        req.body_len = need;
        handler(&req, fd, ctx);
        free(body);
    } else {
        handler(&req, fd, ctx);
    }
    free(buf);
}

void p2c_httpd_send_response(int client_fd, int status_code, const char *status_text,
                              const char *content_type, const char *body, size_t body_len) {
    if (body_len == (size_t)-1) body_len = body ? strlen(body) : 0;
    char header[P2C_HTTPD_RESP_HDR_MAX];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "\r\n",
        status_code, status_text ? status_text : "OK",
        content_type ? content_type : "text/plain; charset=utf-8",
        body_len);
    /* 切り詰めて送ると、ヘッダの途中で応答が壊れる（また、長さ計算とバッファの不一致で
     * バッファ外を読む）。収まらない場合は固定の 500 応答に置き換える。 */
    if (hlen < 0 || (size_t)hlen >= sizeof(header)) {
        static const char fallback[] =
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(client_fd, fallback, sizeof(fallback) - 1);
        return;
    }
    if (send_all(client_fd, header, (size_t)hlen) != 0) return;
    if (body && body_len > 0) send_all(client_fd, body, body_len);
}

int p2c_httpd_serve_ex(const char *host, int port, const P2C_HttpLimits *limits,
                       P2C_HttpHandler handler, void *ctx) {
    P2C_HttpLimits lim;
    if (limits) lim = *limits;
    else p2c_httpd_limits_default(&lim);
    if (lim.max_connections < 1) lim.max_connections = 1;
    if (lim.handler_timeout_s < 1) lim.handler_timeout_s = 1;

    signal(SIGPIPE, SIG_IGN); /* 切断された接続への送信でプロセスが落ちないように */

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return 1;
    int yes = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (!host || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(srv_fd);
        return 2;
    }
    /* sockaddr_in を sockaddr として渡すと、型の食い違い（strict aliasing）の警告になる
     * ため、同じ大きさの sockaddr へ値としてコピーしてから渡す。 */
    struct sockaddr sa;
    memset(&sa, 0, sizeof(sa));
    memcpy(&sa, &addr, sizeof(addr) < sizeof(sa) ? sizeof(addr) : sizeof(sa));
    if (bind(srv_fd, &sa, sizeof(addr)) < 0) {
        close(srv_fd);
        return 3;
    }
    if (listen(srv_fd, 64) < 0) {
        close(srv_fd);
        return 4;
    }

    int live = 0; /* 生きている子プロセス数 */
    for (;;) {
        /* 終了済みの子を回収する */
        while (live > 0 && waitpid(-1, NULL, WNOHANG) > 0) live--;
        /* 上限に達したら、どれかが終わるまで待つ（超過分は backlog で待機させる） */
        while (live >= lim.max_connections) {
            pid_t done = waitpid(-1, NULL, 0);
            if (done > 0) live--;
            else if (errno != EINTR) live = 0;
        }

        int cfd = accept(srv_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN) continue;
            /* EMFILE 等: 少し待ってから再試行する（ループを空回りさせない） */
            struct timespec ts = { 0, 100000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        fflush(NULL); /* 子が親の未出力バッファを二重に出さないように */
        pid_t pid = fork();
        if (pid == 0) {
            close(srv_fd);
            /* ハンドラが止まっても子が居続けないよう絶対期限を置く */
            alarm((unsigned)lim.handler_timeout_s);
            handle_connection(cfd, &lim, handler, ctx);
            close(cfd);
            _exit(0);
        }
        if (pid > 0) {
            live++;
            close(cfd);
        } else {
            /* fork できない場合だけ、この接続を親で直列に処理する（資源不足時の縮退動作） */
            handle_connection(cfd, &lim, handler, ctx);
            close(cfd);
        }
    }
}

int p2c_httpd_serve(const char *host, int port, P2C_HttpHandler handler, void *ctx) {
    return p2c_httpd_serve_ex(host, port, NULL, handler, ctx);
}
