/*
 * python_code_to_cの「本物のGUI」: ブラウザで開く軽量なローカルWebサーバー。
 * 依存ライブラリなし（POSIXソケットのみ）で動く python_code_to_c_httpd.c の上に、
 * python_code_to_c/c2py APIを呼び出すルーティングを実装している。
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "tools/python_code_to_c_gui_web.h"
#include "tools/python_code_to_c_httpd.h"
#include "tools/python_code_to_c_runtime_locate.h"
#include "core/python_code_to_c.h"
#include "tools/c2py.h"
#include "tools/python_code_to_c_gui_html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>

static char *g_argv0 = NULL;

/* JSON文字列として安全に出力できるようエスケープする（このアプリ内で
 * 必要な最小限: バックスラッシュ・ダブルクォート・制御文字・改行のみ）。
 * out は十分な大きさ（少なくとも strlen(in)*6 + 1）を呼び出し側で確保する。 */
static void json_escape_append(char *out, size_t out_cap, size_t *out_len, const char *in) {
    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;
        const char *rep = NULL;
        char buf6[8];
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default:
                if (c < 0x20) {
                    snprintf(buf6, sizeof(buf6), "\\u%04x", c);
                    rep = buf6;
                }
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (*out_len + rl >= out_cap) return; /* 安全側に倒して打ち切る */
            memcpy(out + *out_len, rep, rl);
            *out_len += rl;
        } else {
            if (*out_len + 1 >= out_cap) return;
            out[(*out_len)++] = (char)c;
        }
    }
}

/* {"ok":true,"<field>":"<escaped text>"} を組み立てて返す。呼び出し側でfree。 */
static char *make_json_ok(const char *field, const char *text) {
    size_t text_len = text ? strlen(text) : 0;
    size_t cap = text_len * 6 + 128;
    char *out = malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "{\"ok\":true,\"%s\":\"", field);
    size_t len = (size_t)n;
    json_escape_append(out, cap, &len, text ? text : "");
    if (len + 3 < cap) { out[len++] = '"'; out[len++] = '}'; out[len] = '\0'; }
    return out;
}

static char *make_json_err(const char *error) {
    size_t cap = (error ? strlen(error) : 0) * 6 + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t len = (size_t)snprintf(out, cap, "{\"ok\":false,\"error\":\"");
    json_escape_append(out, cap, &len, error ? error : "unknown error");
    if (len + 3 < cap) { out[len++] = '"'; out[len++] = '}'; out[len] = '\0'; }
    return out;
}

static void handle_python_code_to_c(int client_fd, const char *src) {
    char *c_code = NULL;
    P2C_Result r = python_to_c(src, NULL, &c_code);
    if (r == P2C_OK && c_code) {
        char *json = make_json_ok("code", c_code);
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
        free(c_code);
    } else {
        char *json = make_json_err(p2c_last_error_details());
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
    }
}

static void handle_c2py(int client_fd, const char *src) {
    char *py_code = NULL;
    int r = c_to_python(src, &py_code);
    if (r == 0 && py_code) {
        char *json = make_json_ok("code", py_code);
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
        free(py_code);
    } else {
        char *json = make_json_err(py_code ? py_code : "conversion failed");
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
        free(py_code);
    }
}

/* Python -> C -> コンパイル -> 実行、をまとめて行い標準出力を回収する。
 * TUI版(gui_main.cのcompile_and_run_c)と同じ探索・コンパイル手順を使う。 */
static void handle_run(int client_fd, const char *src) {
    char *c_code = NULL;
    P2C_Result r = python_to_c(src, NULL, &c_code);
    if (r != P2C_OK || !c_code) {
        char *json = make_json_err(p2c_last_error_details());
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
        free(c_code);
        return;
    }

    P2C_RuntimeLocation loc;
    if (!p2c_locate_runtime(g_argv0, &loc)) {
        char *json = make_json_err("ランタイムのソース (python_code_to_c_runtime.c 等) が見つかりませんでした。"
                                    "PYTHON_CODE_TO_C_SRC_DIR環境変数でsrcディレクトリを指定するか、"
                                    "プロジェクトルートから起動してください。");
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
        free(c_code);
        return;
    }

    char c_path[] = "/tmp/python_code_to_c_gui_run_XXXXXX.c";
    char bin_path[] = "/tmp/python_code_to_c_gui_run_XXXXXX.bin";
    /* mkstempで安全にユニークな一時ファイルを作る */
    {
        char tmpl[] = "/tmp/python_code_to_c_gui_run_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) close(fd);
        snprintf(c_path, sizeof(c_path), "%s.c", tmpl);
        snprintf(bin_path, sizeof(bin_path), "%s.bin", tmpl);
        unlink(tmpl);
    }

    FILE *cf = fopen(c_path, "w");
    if (cf) { fputs(c_code, cf); fclose(cf); }
    free(c_code);

    const char *cc = getenv("CC");
    if (!cc) cc = "cc";
    /* 各パスは P2C_RuntimeLocation 上は char[4096] で、GCCのformat-truncation
     * 検査は「理論上はどのフィールドも最大4095バイトまでありうる」という前提で
     * 固定長バッファへの切り詰めを警告してくる。実際にそこまで長いパスには
     * まずならないが、固定バッファのサイズを闇雲に増やしても静的検査は
     * 満足しないため、実際の長さから必要サイズを計算して動的確保する。 */
    size_t cmd_cap = strlen(cc) + strlen(loc.include_dir) + strlen(c_path) +
        strlen(loc.runtime_c) + strlen(loc.common_c) + strlen(loc.platform_core_c) +
        strlen(loc.platform_c) + strlen(loc.pygame_c) + strlen(bin_path) + 80;
    char *cmd = (char*)malloc(cmd_cap);
    char compile_log[4096] = {0};
    if (cmd) {
#if defined(__GNUC__) && !defined(__clang__)
/* バッファ長は直上のstrlen計算から求めているため実際に切り詰めは起きないが、

 * GCCの範囲解析は「char[4096]のフィールドは最大4095バイト」と仮定するため

 * -Wformat-truncation を誤検出する。この呼び出しに限り明示的に抑止する。 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
        snprintf(cmd, cmd_cap,
            "%s -I\"%s\" -std=c11 \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" \"%s\" -lm -o \"%s\" 2>&1",
            cc, loc.include_dir, c_path, loc.runtime_c, loc.common_c, loc.platform_core_c, loc.platform_c, loc.pygame_c, bin_path);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        FILE *p = popen(cmd, "r");
        if (p) {
            size_t got = fread(compile_log, 1, sizeof(compile_log) - 1, p);
            compile_log[got] = '\0';
            pclose(p);
        }
        free(cmd);
    }

    if (access(bin_path, X_OK) != 0) {
        char errbuf[4600];
        snprintf(errbuf, sizeof(errbuf), "コンパイルに失敗しました:\n%s", compile_log);
        char *json = make_json_err(errbuf);
        p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
        free(json);
        unlink(c_path);
        return;
    }

    char run_cmd[600];
    snprintf(run_cmd, sizeof(run_cmd), "timeout 10 \"%s\" 2>&1", bin_path);
    FILE *rp = popen(run_cmd, "r");
    char *run_out = malloc(65536);
    size_t run_len = 0;
    if (rp && run_out) {
        run_len = fread(run_out, 1, 65535, rp);
        run_out[run_len] = '\0';
        pclose(rp);
    } else if (run_out) {
        run_out[0] = '\0';
    }

    char *json = make_json_ok("output", run_out ? run_out : "");
    p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
    free(json);
    free(run_out);
    unlink(c_path);
    unlink(bin_path);
}

static void handle_supported(int client_fd) {
    char *json = make_json_ok("text", p2c_supported_range_string());
    p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)-1);
    free(json);
}

static void handle_version(int client_fd) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"version\":\"python_code_to_c %s\"}", p2c_version_string());
    p2c_httpd_send_response(client_fd, 200, "OK", "application/json; charset=utf-8", buf, (size_t)-1);
}

static void send_gui_html(int client_fd) {
    const size_t part1_len = sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART1) - 1;
    const size_t part2_len = sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART2) - 1;
    const size_t part3_len = sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART3) - 1;
    const size_t part4_len = sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART4) - 1;
    const size_t total_len = part1_len + part2_len + part3_len + part4_len;
    char *html = (char*)malloc(total_len + 1);
    if (!html) {
        p2c_httpd_send_response(client_fd, 500, "Internal Server Error", "text/plain; charset=utf-8", "out of memory", (size_t)-1);
        return;
    }
    memcpy(html, PYTHON_CODE_TO_C_GUI_HTML_PART1, part1_len);
    memcpy(html + part1_len, PYTHON_CODE_TO_C_GUI_HTML_PART2, part2_len);
    memcpy(html + part1_len + part2_len, PYTHON_CODE_TO_C_GUI_HTML_PART3, part3_len);
    memcpy(html + part1_len + part2_len + part3_len, PYTHON_CODE_TO_C_GUI_HTML_PART4, part4_len);
    html[total_len] = '\0';
    p2c_httpd_send_response(client_fd, 200, "OK", "text/html; charset=utf-8", html, total_len);
    free(html);
}

static void router(const P2C_HttpRequest *req, int client_fd, void *ctx) {
    (void)ctx;
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/") == 0) {
        send_gui_html(client_fd);
        return;
    }
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/supported") == 0) {
        handle_supported(client_fd);
        return;
    }
    if (strcmp(req->method, "GET") == 0 && strcmp(req->path, "/api/version") == 0) {
        handle_version(client_fd);
        return;
    }
    if (strcmp(req->method, "POST") == 0 && req->body) {
        if (strcmp(req->path, "/api/python_code_to_c") == 0) { handle_python_code_to_c(client_fd, req->body); return; }
        if (strcmp(req->path, "/api/c2py") == 0) { handle_c2py(client_fd, req->body); return; }
        if (strcmp(req->path, "/api/run") == 0) { handle_run(client_fd, req->body); return; }
    }
    p2c_httpd_send_response(client_fd, 404, "Not Found", "text/plain; charset=utf-8", "not found", (size_t)-1);
}

static void try_open_browser(const char *url) {
    char cmd[600];
#if defined(__APPLE__)
    snprintf(cmd, sizeof(cmd), "open \"%s\" >/dev/null 2>&1 &", url);
#else
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", url);
#endif
    if (system(cmd) != 0) { /* 開けなくてもURLを表示済みなので問題ない */ }
}

int p2c_gui_web_main(int argc, char **argv) {
    g_argv0 = argv[0];
    const char *host = "127.0.0.1";
    int port = 8765;
    bool no_browser = false;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--port=", 7) == 0) port = atoi(argv[i] + 7);
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strncmp(argv[i], "--host=", 7) == 0) host = argv[i] + 7;
        else if (strcmp(argv[i], "--no-browser") == 0) no_browser = true;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/", host, port);

    printf("python_code_to_c GUI サーバーを起動しました: %s\n", url);
    printf("ブラウザで上記のURLを開いてください（Ctrl-Cで終了）。\n");
    printf("テキストメニュー版は引数なしで `%s` を起動してください。\n", argv[0]);
    fflush(stdout);

    if (!no_browser) try_open_browser(url);

    int rc = p2c_httpd_serve(host, port, router, NULL);
    if (rc != 0) {
        fprintf(stderr, "Error: サーバーの起動に失敗しました（port %d が使用中の可能性があります。"
                         "--port=<番号> で変更できます）。\n", port);
        return 1;
    }
    return 0;
}
