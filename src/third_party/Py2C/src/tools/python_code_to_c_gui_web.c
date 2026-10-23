/*
 * python_code_to_c の Web GUI: ブラウザで開くローカルの Web サーバー。
 *
 * 【セキュリティの前提（必ず読むこと）】
 *   /api/run は利用者が入力した Python を C にして、コンパイルし、実行する。
 *   そのため、この機能を誰が呼べるかが安全性のすべてになる。次の多層防御を入れている。
 *
 *   1. 既定は 127.0.0.1 のみで待ち受ける。外部へ公開するには --allow-remote が必要。
 *   2. 起動ごとに 256 ビットの乱数トークンを作り、HTML にだけ埋め込む。API の POST は
 *      X-P2C-Token ヘッダが無いと 403。このヘッダは CORS のプリフライトを伴うため、
 *      別のサイトのページからはブラウザが送れない（CSRF 対策）。
 *   3. Host ヘッダを許可リストと照合する（DNS リバインディング対策）。
 *   4. Origin ヘッダが付く要求は、許可した origin と完全一致する場合だけ受け付ける。
 *   5. --allow-remote で公開する場合、/api/run は --enable-run を付けない限り無効。
 *   6. シェルを使わない（popen/system は使わず fork/execv）。パスを文字列へ連結しない。
 *   7. 一時ファイルは mkdtemp で作った 0700 のディレクトリ内に、O_EXCL|O_NOFOLLOW で作る。
 *   8. 実行するプログラムは専用ディレクトリで、CPU・メモリ・ファイルサイズ・fd の上限と
 *      壁時計の期限（超えたらプロセスグループごと殺す）の下で走らせる。
 *
 *   注意: 8 はプロセスの資源制限であり、ファイルシステム・ネットワークの隔離ではない。
 *   信頼できない利用者にこの機能を公開してはならない。公開が必要な場合は、コンテナ等の
 *   隔離された環境で、利用者の認証を前段に置いて運用すること。
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
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define P2C_GUI_TOKEN_BYTES 32
#define P2C_GUI_TOKEN_HEX (P2C_GUI_TOKEN_BYTES * 2 + 1)
#define P2C_GUI_MAX_CODE (256u * 1024u)
#define P2C_GUI_RUN_OUTPUT_MAX (64u * 1024u)
#define P2C_GUI_COMPILE_WALL_MS (60 * 1000)
#define P2C_GUI_RUN_WALL_MS (10 * 1000)
#define P2C_GUI_RUN_CPU_S 10
#define P2C_GUI_RUN_AS_BYTES ((rlim_t)512u * 1024u * 1024u)
#define P2C_GUI_RUN_FSIZE_BYTES ((rlim_t)1024u * 1024u)
#define P2C_GUI_JSON_TYPE "application/json; charset=utf-8"
#define P2C_GUI_TOKEN_PLACEHOLDER "__P2C_TOKEN__"

typedef struct {
    char token[P2C_GUI_TOKEN_HEX];
    char host[64];
    int port;
    int allow_remote;
    int enable_run;
    int any_host;                /* 0.0.0.0 で待ち受けるとき: Host は任意（LAN の IP で来る相手を通すため） */
    char allowed_host[3][192];   /* Host ヘッダとして許可する値（host:port） */
    int allowed_count;
} P2C_GuiWebState;

/* 子プロセスは fork で複製されるため、この状態は親子で同じ内容を見る（書き換えは起動時だけ）。 */
static P2C_GuiWebState g_state;
static char *g_argv0 = NULL;

/* ── JSON 出力 ───────────────────────────────────────────────────────────── */

/* JSON 文字列として安全に追記する。容量が足りない場合は false を返す（切り詰めない）。 */
static bool json_append(char *out, size_t cap, size_t *len, const char *in) {
    for (const unsigned char *p = (const unsigned char*)in; *p; p++) {
        char tmp[8];
        const char *rep = NULL;
        size_t rl = 1;
        unsigned char c = *p;
        switch (c) {
            case '"':  rep = "\\\""; rl = 2; break;
            case '\\': rep = "\\\\"; rl = 2; break;
            case '\n': rep = "\\n"; rl = 2; break;
            case '\r': rep = "\\r"; rl = 2; break;
            case '\t': rep = "\\t"; rl = 2; break;
            default:
                if (c < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                    rep = tmp;
                    rl = 6;
                }
        }
        if (*len + rl + 1 > cap) return false;
        if (rep) memcpy(out + *len, rep, rl);
        else out[*len] = (char)c;
        *len += rl;
    }
    out[*len] = '\0';
    return true;
}

/* {"ok":true,"<field>":"<text>"} を組み立てる。呼び出し側で free する。 */
static char *make_json_ok(const char *field, const char *text) {
    const char *t = text ? text : "";
    size_t cap = strlen(t) * 6 + strlen(field) + 64;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "{\"ok\":true,\"%s\":\"", field);
    size_t len = n > 0 ? (size_t)n : 0;
    if (!json_append(out, cap, &len, t) || len + 3 > cap) {
        free(out);
        return NULL;
    }
    out[len++] = '"';
    out[len++] = '}';
    out[len] = '\0';
    return out;
}

static char *make_json_err(const char *error) {
    const char *t = error ? error : "unknown error";
    size_t cap = strlen(t) * 6 + 64;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t len = (size_t)snprintf(out, cap, "{\"ok\":false,\"error\":\"");
    if (!json_append(out, cap, &len, t) || len + 3 > cap) {
        free(out);
        return NULL;
    }
    out[len++] = '"';
    out[len++] = '}';
    out[len] = '\0';
    return out;
}

/* 応答を書き、確保した JSON を解放する。JSON が作れなかった場合は固定の 500 を返す。 */
static void send_json(int fd, char *json) {
    if (json) {
        p2c_httpd_send_response(fd, 200, "OK", P2C_GUI_JSON_TYPE, json, (size_t)-1);
        free(json);
    } else {
        p2c_httpd_send_response(fd, 500, "Internal Server Error", "text/plain; charset=utf-8",
                                "out of memory", (size_t)-1);
    }
}

static void send_json_error(int fd, const char *msg) {
    send_json(fd, make_json_err(msg));
}

/* ── 乱数・トークン ──────────────────────────────────────────────────────── */

/* /dev/urandom から n バイト読む。失敗は 0。 */
static int read_random(unsigned char *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}

static int make_token(char *out, size_t cap) {
    unsigned char raw[P2C_GUI_TOKEN_BYTES];
    static const char hex[] = "0123456789abcdef";
    if (cap < P2C_GUI_TOKEN_HEX || !read_random(raw, sizeof(raw))) return 0;
    for (size_t i = 0; i < sizeof(raw); i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 0x0f];
    }
    out[2 * sizeof(raw)] = '\0';
    return 1;
}

/* 定時間比較（トークンの一致判定で、一致位置を時間で漏らさない） */
static int token_equal(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    unsigned diff = (unsigned)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
    return la > 0 && diff == 0;
}

/* ── 子プロセス実行（シェルを使わない） ───────────────────────────────────── */

/* 子プロセスの資源上限。compile と run で値を変える。 */
typedef struct {
    int cpu_s;
    rlim_t as_bytes;
    rlim_t fsize_bytes;
} P2C_ChildLimits;

static void set_limit(int resource, rlim_t value) {
    struct rlimit rl;
    rl.rlim_cur = value;
    rl.rlim_max = value;
    setrlimit(resource, &rl);
}

/* 子プロセス側: 新しいプロセスグループにして、上限を適用し、exec する。 */
static void child_exec(char *const argv[], const char *cwd, const P2C_ChildLimits *lim, int out_fd) {
    setpgid(0, 0);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        dup2(devnull, 0);
        if (devnull > 2) close(devnull);
    }
    dup2(out_fd, 1);
    dup2(out_fd, 2);
    /* 親から継承した fd（クライアントの接続、待ち受けソケット等）を実行プログラムへ渡さない */
    for (int fd = 3; fd < 1024; fd++) close(fd);
    if (cwd && chdir(cwd) != 0) _exit(126);
    if (lim) {
        set_limit(RLIMIT_CPU, (rlim_t)lim->cpu_s);
        set_limit(RLIMIT_AS, lim->as_bytes);
        set_limit(RLIMIT_FSIZE, lim->fsize_bytes);
        set_limit(RLIMIT_CORE, 0);
        set_limit(RLIMIT_NOFILE, 64);
    }
    /* 注意: RLIMIT_NPROC は同一ユーザーの全プロセスに効くため設定しない。 */
    execvp(argv[0], argv);
    _exit(127);
}

/*
 * argv を実行し、標準出力と標準エラーを合わせて out（容量 out_cap、NUL 終端）へ集める。
 * 壁時計の期限 wall_ms を超えたらプロセスグループごと殺す。
 * 戻り値: 子の終了コード（シグナルで死んだ場合は 128+シグナル）、起動失敗は -1。
 * timed_out は期限切れなら 1 になる。出力が容量を超えた分は捨てる（子を詰まらせない）。
 */
static int run_capture(char *const argv[], const char *cwd, const P2C_ChildLimits *lim, int wall_ms,
                       char *out, size_t out_cap, int *timed_out) {
    int pfd[2];
    *timed_out = 0;
    out[0] = '\0';
    if (pipe(pfd) != 0) return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pfd[0]);
        child_exec(argv, cwd, lim, pfd[1]);
    }
    close(pfd[1]);

    size_t len = 0;
    long long deadline = 0;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        deadline = (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L) + wall_ms;
    }
    int eof = 0;
    while (!eof) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now = (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
        long long left = deadline - now;
        if (left <= 0) {
            *timed_out = 1;
            kill(-pid, SIGKILL);
            break;
        }
        struct pollfd p;
        p.fd = pfd[0];
        p.events = POLLIN;
        p.revents = 0;
        int r = poll(&p, 1, (int)(left > 1000 ? 1000 : left));
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;
        char chunk[4096];
        ssize_t n = read(pfd[0], chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {
            eof = 1;
            break;
        }
        if (len < out_cap - 1) {
            size_t room = out_cap - 1 - len;
            size_t take = (size_t)n < room ? (size_t)n : room;
            memcpy(out + len, chunk, take);
            len += take;
        }
    }
    close(pfd[0]);
    out[len] = '\0';

    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    /* 期限切れ・孫プロセスの残存に備えて、グループ全体を終わらせる */
    kill(-pid, SIGKILL);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* 一時ディレクトリの中身を消して、ディレクトリ自体も消す（ネストしない前提） */
static void remove_work_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
            unlink(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

/* 専用の作業ディレクトリ（0700）を作る。失敗は 0。 */
static int make_work_dir(char *dir, size_t cap) {
    static const char tmpl[] = "/tmp/p2c-gui-XXXXXX";
    if (cap < sizeof(tmpl)) return 0;
    memcpy(dir, tmpl, sizeof(tmpl));
    return mkdtemp(dir) != NULL;
}

/* 新規作成だけを許す形でファイルへ書く（既存ファイル・シンボリックリンクは開かない） */
static int write_new_file(const char *path, const char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return 0;
        }
        off += (size_t)n;
    }
    return close(fd) == 0;
}

/* ── 変換・実行の API ──────────────────────────────────────────────────────── */

static void handle_python_code_to_c(int fd, const char *src) {
    char *c_code = NULL;
    P2C_Result r = python_to_c(src, NULL, &c_code);
    if (r == P2C_OK && c_code) {
        send_json(fd, make_json_ok("code", c_code));
        free(c_code);
    } else {
        free(c_code);
        send_json_error(fd, p2c_last_error_details());
    }
}

static void handle_c2py(int fd, const char *src) {
    char *py_code = NULL;
    int r = c_to_python(src, &py_code);
    if (r == 0 && py_code) {
        send_json(fd, make_json_ok("code", py_code));
    } else {
        send_json_error(fd, py_code ? py_code : "conversion failed");
    }
    free(py_code);
}

static void handle_run(int fd, const char *src) {
    char *c_code = NULL;
    P2C_Result r = python_to_c(src, NULL, &c_code);
    if (r != P2C_OK || !c_code) {
        free(c_code);
        send_json_error(fd, p2c_last_error_details());
        return;
    }

    static P2C_RuntimeLocation loc; /* 大きいので static（子プロセスごとに別領域になる） */
    if (!p2c_locate_runtime(g_argv0, &loc)) {
        free(c_code);
        send_json_error(fd, "ランタイムのソース (python_code_to_c_runtime.c 等) が見つかりませんでした。"
                            "PYTHON_CODE_TO_C_SRC_DIR環境変数でsrcディレクトリを指定するか、"
                            "プロジェクトルートから起動してください。");
        return;
    }

    char work[64];
    if (!make_work_dir(work, sizeof(work))) {
        free(c_code);
        send_json_error(fd, "作業ディレクトリを作成できませんでした。");
        return;
    }
    char c_path[128], bin_path[128];
    snprintf(c_path, sizeof(c_path), "%s/prog.c", work);
    snprintf(bin_path, sizeof(bin_path), "%s/prog", work);

    if (!write_new_file(c_path, c_code, strlen(c_code))) {
        free(c_code);
        remove_work_dir(work);
        send_json_error(fd, "一時ソースファイルを作成できませんでした。");
        return;
    }
    free(c_code);

    /* CC は実行ファイル名（またはパス）1つとして扱う。引数付きの指定はサポートしない。 */
    char cc_buf[256];
    const char *cc_env = getenv("CC");
    snprintf(cc_buf, sizeof(cc_buf), "%s", (cc_env && *cc_env) ? cc_env : "cc");
    char arg_std[] = "-std=c11";
    char arg_I[] = "-I";
    char arg_lm[] = "-lm";
    char arg_o[] = "-o";
    char *cc_argv[] = {
        cc_buf, arg_std, arg_I, loc.include_dir,
        c_path, loc.runtime_c, loc.common_c, loc.unicode_tables_c, loc.platform_core_c, loc.platform_c, loc.pygame_c,
        arg_lm, arg_o, bin_path, NULL
    };
    /* コンパイラのログはサイズ上限付きで集める（静的な容量は呼び出しごとに確保する） */
    char *compile_log = (char*)malloc(4096);
    if (!compile_log) {
        remove_work_dir(work);
        send_json_error(fd, "out of memory");
        return;
    }
    P2C_ChildLimits compile_lim = { 60, (rlim_t)2048u * 1024u * 1024u, (rlim_t)64u * 1024u * 1024u };
    int timed_out = 0;
    /* コンパイルは現在のディレクトリで行う（ランタイムの探索結果は相対パスの場合があるため） */
    int rc = run_capture(cc_argv, NULL, &compile_lim, P2C_GUI_COMPILE_WALL_MS, compile_log, 4096, &timed_out);
    if (timed_out) {
        remove_work_dir(work);
        free(compile_log);
        send_json_error(fd, "コンパイルがタイムアウトしました。");
        return;
    }
    if (rc != 0 || access(bin_path, X_OK) != 0) {
        size_t cap = 4096 + 64;
        char *msg = (char*)malloc(cap);
        if (msg) {
            snprintf(msg, cap, "コンパイルに失敗しました:\n%s", compile_log);
            send_json_error(fd, msg);
            free(msg);
        } else {
            send_json_error(fd, "コンパイルに失敗しました。");
        }
        free(compile_log);
        remove_work_dir(work);
        return;
    }
    free(compile_log);

    char *run_out = (char*)malloc(P2C_GUI_RUN_OUTPUT_MAX);
    if (!run_out) {
        remove_work_dir(work);
        send_json_error(fd, "out of memory");
        return;
    }
    char *run_argv[] = { bin_path, NULL };
    P2C_ChildLimits run_lim = { P2C_GUI_RUN_CPU_S, P2C_GUI_RUN_AS_BYTES, P2C_GUI_RUN_FSIZE_BYTES };
    timed_out = 0;
    run_capture(run_argv, work, &run_lim, P2C_GUI_RUN_WALL_MS, run_out, P2C_GUI_RUN_OUTPUT_MAX, &timed_out);
    if (timed_out) {
        /* 注記を別バッファで組み立て、残り容量を超えないように追記する */
        char note[128];
        snprintf(note, sizeof(note), "\n[実行がタイムアウトしたため停止しました（%d 秒）]\n",
                 P2C_GUI_RUN_WALL_MS / 1000);
        size_t len = strlen(run_out);
        size_t room = P2C_GUI_RUN_OUTPUT_MAX - 1 - len;
        size_t nlen = strlen(note);
        if (nlen > room) nlen = room;
        memcpy(run_out + len, note, nlen);
        run_out[len + nlen] = '\0';
    }
    send_json(fd, make_json_ok("output", run_out));
    free(run_out);
    remove_work_dir(work);
}

static void handle_supported(int fd) {
    send_json(fd, make_json_ok("text", p2c_supported_range_string()));
}

static void handle_version(int fd) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"version\":\"python_code_to_c %s\"}", p2c_version_string());
    p2c_httpd_send_response(fd, 200, "OK", P2C_GUI_JSON_TYPE, buf, (size_t)-1);
}

/* 連結した HTML へトークンを差し込む（トークンは 16 進数だけなので、JS 文字列として安全） */
static char *build_html(const char *token, size_t *out_len) {
    const char *parts[4] = {
        PYTHON_CODE_TO_C_GUI_HTML_PART1, PYTHON_CODE_TO_C_GUI_HTML_PART2,
        PYTHON_CODE_TO_C_GUI_HTML_PART3, PYTHON_CODE_TO_C_GUI_HTML_PART4
    };
    const size_t lens[4] = {
        sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART1) - 1, sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART2) - 1,
        sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART3) - 1, sizeof(PYTHON_CODE_TO_C_GUI_HTML_PART4) - 1
    };
    size_t total = 0;
    for (int i = 0; i < 4; i++) total += lens[i];
    char *raw = (char*)malloc(total + 1);
    if (!raw) return NULL;
    size_t off = 0;
    for (int i = 0; i < 4; i++) {
        memcpy(raw + off, parts[i], lens[i]);
        off += lens[i];
    }
    raw[total] = '\0';

    const size_t ph_len = strlen(P2C_GUI_TOKEN_PLACEHOLDER);
    const size_t tok_len = strlen(token);
    size_t count = 0;
    for (const char *p = raw; (p = strstr(p, P2C_GUI_TOKEN_PLACEHOLDER)) != NULL; p += ph_len) count++;
    size_t out_cap = total + count * tok_len + 1;
    char *html = (char*)malloc(out_cap);
    if (!html) {
        free(raw);
        return NULL;
    }
    size_t o = 0;
    const char *p = raw;
    for (;;) {
        const char *hit = strstr(p, P2C_GUI_TOKEN_PLACEHOLDER);
        size_t chunk = hit ? (size_t)(hit - p) : strlen(p);
        memcpy(html + o, p, chunk);
        o += chunk;
        if (!hit) break;
        memcpy(html + o, token, tok_len);
        o += tok_len;
        p = hit + ph_len;
    }
    html[o] = '\0';
    free(raw);
    if (out_len) *out_len = o;
    return html;
}

static void send_gui_html(int fd) {
    size_t len = 0;
    char *html = build_html(g_state.token, &len);
    if (!html) {
        p2c_httpd_send_response(fd, 500, "Internal Server Error", "text/plain; charset=utf-8",
                                "out of memory", (size_t)-1);
        return;
    }
    p2c_httpd_send_response(fd, 200, "OK", "text/html; charset=utf-8", html, len);
    free(html);
}

/* ── 入口: 送信元の検査とルーティング ───────────────────────────────────── */

/* 許可リストへ "host:port" を追加する（容量は呼び出し側で確認済みの想定だが、念のため検査する） */
static void add_allowed_host(const char *host, int port) {
    if (g_state.allowed_count >= 3) return;
    char *dst = g_state.allowed_host[g_state.allowed_count];
    size_t cap = sizeof(g_state.allowed_host[0]);
    int n = snprintf(dst, cap, "%s:%d", host, port);
    if (n < 0 || (size_t)n >= cap) return;
    g_state.allowed_count++;
}

static int host_allowed(const char *host) {
    if (g_state.any_host) return 1; /* トークンと Origin の検査は引き続き行う */
    for (int i = 0; i < g_state.allowed_count; i++) {
        if (strcmp(host, g_state.allowed_host[i]) == 0) return 1;
    }
    return 0;
}

static void router(const P2C_HttpRequest *req, int fd, void *ctx) {
    (void)ctx;

    /* 1. Host: DNS リバインディングで別名のホストから来た要求を拒否する */
    if (!host_allowed(req->host)) {
        p2c_httpd_send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
                                "forbidden: unexpected Host header", (size_t)-1);
        return;
    }
    /* 2. Origin: ブラウザが付けた送信元が、この接続の Host（同一 origin）または許可した
     *    origin と一致する場合だけ受け付ける。別サイトのページは Origin が一致しない。 */
    if (req->has_origin) {
        int ok = 0;
        char same[300];
        snprintf(same, sizeof(same), "http://%s", req->host);
        if (strcmp(req->origin, same) == 0) ok = 1;
        for (int i = 0; !ok && i < g_state.allowed_count; i++) {
            char expect[200];
            snprintf(expect, sizeof(expect), "http://%s", g_state.allowed_host[i]);
            if (strcmp(req->origin, expect) == 0) ok = 1;
        }
        if (!ok) {
            p2c_httpd_send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
                                    "forbidden: cross-origin request", (size_t)-1);
            return;
        }
    }

    const int is_get = strcmp(req->method, "GET") == 0;
    const int is_post = strcmp(req->method, "POST") == 0;

    if (is_get && strcmp(req->path, "/") == 0) {
        send_gui_html(fd);
        return;
    }
    if (is_get && strcmp(req->path, "/api/supported") == 0) {
        handle_supported(fd);
        return;
    }
    if (is_get && strcmp(req->path, "/api/version") == 0) {
        handle_version(fd);
        return;
    }

    const int known_post = is_post && (strcmp(req->path, "/api/python_code_to_c") == 0 ||
                                       strcmp(req->path, "/api/c2py") == 0 ||
                                       strcmp(req->path, "/api/run") == 0);
    if (!known_post) {
        p2c_httpd_send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found", (size_t)-1);
        return;
    }

    /* 3. トークン: 変換・実行の API は、HTML から得たトークンを持つ要求だけに応じる */
    if (!token_equal(req->token, g_state.token)) {
        p2c_httpd_send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
                                "forbidden: missing or invalid X-P2C-Token", (size_t)-1);
        return;
    }
    if (!req->body) {
        send_json_error(fd, "リクエストボディがありません。");
        return;
    }
    if (req->body_len > P2C_GUI_MAX_CODE) {
        p2c_httpd_send_response(fd, 413, "Payload Too Large", "text/plain; charset=utf-8",
                                "source is too large", (size_t)-1);
        return;
    }

    if (strcmp(req->path, "/api/python_code_to_c") == 0) {
        handle_python_code_to_c(fd, req->body);
    } else if (strcmp(req->path, "/api/c2py") == 0) {
        handle_c2py(fd, req->body);
    } else {
        /* 4. /api/run は既定で有効（ループバック）。公開時は --enable-run が必要 */
        if (!g_state.enable_run) {
            p2c_httpd_send_response(fd, 403, "Forbidden", "text/plain; charset=utf-8",
                                    "forbidden: running programs is disabled (start with --enable-run)",
                                    (size_t)-1);
            return;
        }
        handle_run(fd, req->body);
    }
}

/* URL 用のホスト名の検査: 英数字と . - : だけ（引数へ安全に渡すため） */
static int host_is_safe(const char *h) {
    if (!*h || strlen(h) > 63) return 0;
    for (const char *p = h; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '-')) {
            return 0;
        }
    }
    return 1;
}

static int host_is_loopback(const char *h) {
    return strcmp(h, "127.0.0.1") == 0 || strcmp(h, "localhost") == 0;
}

/* ブラウザを開く。シェルは使わず、子プロセスで直接起動する（失敗しても無視してよい）。 */
static void try_open_browser(const char *url) {
    char u[200];
    snprintf(u, sizeof(u), "%s", url);
#if defined(__APPLE__)
    char prog[] = "open";
#else
    char prog[] = "xdg-open";
#endif
    char *argv[] = { prog, u, NULL };
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            close(devnull);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
}

static void usage(const char *prog) {
    fprintf(stderr,
        "使い方: %s [--port=N] [--host=ADDR] [--no-browser]\n"
        "            [--allow-remote] [--enable-run]\n"
        "  既定は 127.0.0.1:8765 のみで待ち受ける（このマシンからだけ使える）。\n"
        "  --host で別のアドレスを指定するには --allow-remote が必要。\n"
        "  --allow-remote で公開した場合、プログラムの実行（/api/run）は\n"
        "  --enable-run を付けない限り無効になる。\n", prog);
}

int p2c_gui_web_main(int argc, char **argv) {
    g_argv0 = argv[0];
    memset(&g_state, 0, sizeof(g_state));
    snprintf(g_state.host, sizeof(g_state.host), "%s", "127.0.0.1");
    g_state.port = 8765;
    int no_browser = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--port=", 7) == 0) {
            long v = strtol(argv[i] + 7, NULL, 10);
            if (v < 1 || v > 65535) {
                fprintf(stderr, "Error: --port は 1〜65535 の数字で指定してください。\n");
                return 2;
            }
            g_state.port = (int)v;
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            snprintf(g_state.host, sizeof(g_state.host), "%s", argv[++i]);
        } else if (strncmp(argv[i], "--host=", 7) == 0) {
            snprintf(g_state.host, sizeof(g_state.host), "%s", argv[i] + 7);
        } else if (strcmp(argv[i], "--no-browser") == 0) {
            no_browser = 1;
        } else if (strcmp(argv[i], "--allow-remote") == 0) {
            g_state.allow_remote = 1;
        } else if (strcmp(argv[i], "--enable-run") == 0) {
            g_state.enable_run = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: 不明なオプション: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!host_is_safe(g_state.host)) {
        fprintf(stderr, "Error: --host は IPv4 アドレスか localhost だけを指定できます。\n");
        return 2;
    }
    const int loopback = host_is_loopback(g_state.host);
    if (!loopback && !g_state.allow_remote) {
        fprintf(stderr, "Error: %s はこのマシン以外からも接続できる宛先です。\n"
                        "  公開する場合のみ --allow-remote を付けてください（利用者の認証が無い環境では\n"
                        "  プログラム実行機能を公開しないことを強く推奨します）。\n", g_state.host);
        return 2;
    }
    if (!loopback) {
        fprintf(stderr, "警告: %s で待ち受けます。同じネットワークの相手はトークン無しでは操作できませんが、\n"
                        "  トークンは起動した HTML を見られる相手に渡ります。信頼できる相手だけに使ってください。\n",
                g_state.host);
    }

    if (!make_token(g_state.token, sizeof(g_state.token))) {
        fprintf(stderr, "Error: 乱数（/dev/urandom）が使えないため、安全なトークンを作れません。\n");
        return 1;
    }

    /* 許可する Host / Origin の一覧。ループバックなら localhost も許可する。 */
    g_state.any_host = strcmp(g_state.host, "0.0.0.0") == 0;
    if (!g_state.any_host) add_allowed_host(g_state.host, g_state.port);
    if (loopback && strcmp(g_state.host, "localhost") != 0) add_allowed_host("localhost", g_state.port);
    if (loopback && strcmp(g_state.host, "127.0.0.1") != 0) add_allowed_host("127.0.0.1", g_state.port);
    /* /api/run は、ループバック以外に公開する場合は明示的に有効化された時だけ */
    if (loopback) g_state.enable_run = 1;

    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d/", g_state.host, g_state.port);
    printf("python_code_to_c GUI サーバーを起動しました: %s\n", url);
    printf("ブラウザで上記の URL を開いてください（Ctrl-C で終了）。\n");
    printf("プログラム実行: %s\n", g_state.enable_run ? "有効" : "無効（--enable-run で有効化）");
    printf("テキストメニュー版は引数なしで `%s` を起動してください。\n", argv[0]);
    fflush(stdout);

    if (!no_browser) try_open_browser(url);

    int rc = p2c_httpd_serve(g_state.host, g_state.port, router, NULL);
    if (rc != 0) {
        fprintf(stderr, "Error: サーバーの起動に失敗しました（port %d が使用中の可能性があります。"
                         "--port=<番号> で変更できます）。\n", g_state.port);
        return 1;
    }
    return 0;
}
