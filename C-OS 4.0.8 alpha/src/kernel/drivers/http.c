/**
 * http.c - Simple HTTP/HTTPS Client Implementation
 */

#include "http.h"
#include "tcp.h"
#include "dns.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "tls_backend.h"
#include "timer.h"
#include "brotli_decoder.h"
#include "http2_client.h"
#include "task.h"
/* gzip_inflate() reuses the DEFLATE core png_decoder.c already has for PNG's
 * IDAT stream - see the comment on http_decode_gzip_body() below. apps/ is
 * not on this file's normal include path, hence the relative path. */
#include "../../apps/png_decoder.h"

extern void net_init(void);
extern void net_poll(void);
extern bool net_is_connected(void);
extern ip_addr_t dhcp_get_ip(void);

/* The response owner is always supplied explicitly.  Keeping a mutable
 * `http_instance` pointer here associated Set-Cookie values with whichever
 * request happened to be created last, which becomes incorrect as soon as
 * browser-owned workers overlap. */
/* Every HTTP client owns its TCP socket, TLS session, receive staging and
 * decoding workspace. Limit active transports to two as the first safe stage:
 * this allows independent fetch workers to overlap while keeping memory and
 * descriptor pressure bounded until wider load testing is complete. */
#define HTTP_MAX_ACTIVE_TRANSPORTS 2u
static volatile uint32_t g_http_active_transports = 0;
static volatile uint32_t g_http_peak_active_transports = 0;
/* Shared metadata is protected independently of transfer execution. This lets
 * a future bounded worker pool overlap unrelated sockets without exposing the
 * global cookie jar or idle-connection pool to concurrent mutation. */
static volatile uint32_t g_http_cookie_lock = 0;
static volatile uint32_t g_http_keepalive_lock = 0;

static void http_spin_lock(volatile uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("pause");
    }
}

static void http_spin_unlock(volatile uint32_t *lock) {
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

/* BearSSL commonly exposes plaintext in TLS-record-sized spans. A 16KiB
 * request-local staging buffer avoids excessive TLS/TCP copies and remains
 * independent for each active HTTP client. */
#define HTTP_RECEIVE_CHUNK_SIZE (16u * 1024u)

/* The NetSurf port creates a short-lived http_client_t for each document and
 * subresource.  Retaining only the underlying self-delimited HTTP/1.1
 * transport lets every ordinary same-origin page (HTML, CSS, favicon, image)
 * avoid repeated DNS/TCP/TLS setup without sharing mutable response buffers.
 * The current safe fetcher executes transports serially, so this bounded pool
 * needs no worker locking. */
#define HTTP_KEEPALIVE_POOL_SIZE 4

typedef struct {
    int active;
    socket_t* socket;
    tls_session_t* tls_session;
    uint64_t port;
    http_scheme_t scheme;
    char host[HTTP_MAX_HOST];
    uint64_t last_used;
} http_keepalive_slot_t;

static http_keepalive_slot_t g_http_keepalive_pool[HTTP_KEEPALIVE_POOL_SIZE];

/* A bounded semaphore protects against unbounded browser subresource fan-out,
 * not against shared request state. Cookie and keep-alive metadata have their
 * own locks, and each active client owns all TCP/TLS/decode buffers. */
static void http_transport_gate_acquire(void) {
    for (;;) {
        uint32_t active = __atomic_load_n(&g_http_active_transports, __ATOMIC_ACQUIRE);
        if (active < HTTP_MAX_ACTIVE_TRANSPORTS &&
            __atomic_compare_exchange_n(&g_http_active_transports, &active,
                                        active + 1u, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            uint32_t new_active = active + 1u;
            uint32_t peak = __atomic_load_n(&g_http_peak_active_transports,
                                            __ATOMIC_RELAXED);
            while (new_active > peak &&
                   !__atomic_compare_exchange_n(&g_http_peak_active_transports,
                                                &peak, new_active, false,
                                                __ATOMIC_RELAXED,
                                                __ATOMIC_RELAXED)) {
                /* compare_exchange refreshes peak; retry only while needed */
            }
            return;
        }
        thread_yield();
    }
}

void http_get_transport_stats(uint32_t* active, uint32_t* peak_active) {
    if (active != NULL) {
        *active = __atomic_load_n(&g_http_active_transports, __ATOMIC_RELAXED);
    }
    if (peak_active != NULL) {
        *peak_active = __atomic_load_n(&g_http_peak_active_transports,
                                       __ATOMIC_RELAXED);
    }
}

static void http_transport_gate_release(void) {
    uint32_t previous = __atomic_fetch_sub(&g_http_active_transports, 1u,
                                            __ATOMIC_RELEASE);
    if (previous == 0u) {
        /* Defensive recovery: a future error path must not underflow the
         * semaphore and permanently admit unbounded transports. */
        __atomic_store_n(&g_http_active_transports, 0u, __ATOMIC_RELEASE);
        serial_puts("[HTTP] transport semaphore underflow recovered\n");
    }
}

static char* http_find_header_end(char* response);
static char* http_find_header_value(char* headers, const char* key);
void http_store_cookies_from_response(http_client_t* http);
static int http_request_internal(http_client_t* http, const char* method, const char* url, const char* data, int depth);

#define HTTP_COOKIE_MAX 32
#define HTTP_COOKIE_NAME_MAX 128
#define HTTP_COOKIE_VALUE_MAX 512
#define HTTP_COOKIE_DOMAIN_MAX HTTP_MAX_HOST
#define HTTP_COOKIE_PATH_MAX 256

typedef struct {
    int active;
    int secure;
    char host[HTTP_MAX_HOST];
    char path[HTTP_COOKIE_PATH_MAX];
    char name[HTTP_COOKIE_NAME_MAX];
    char value[HTTP_COOKIE_VALUE_MAX];
} http_cookie_t;

static http_cookie_t g_http_cookies[HTTP_COOKIE_MAX];

static int http_ci_equal(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == *b;
}

static int http_ci_starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return 0;
    while (*prefix) {
        char a = *text++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static int http_header_name_matches(const char* line, const char* key) {
    if (!line || !key) return 0;
    while (*line && *key) {
        char a = *line++;
        char b = *key++;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *key == '\0' && *line == ':';
}

static void http_copy_text(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < dst_sz && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static void http_trim_value(char* value) {
    if (!value) return;
    while (*value == ' ' || *value == '\t') ++value;
}


static int http_host_matches_cookie(const char* host, const char* cookie_host) {
    if (!host || !cookie_host || !cookie_host[0]) return 0;
    if (http_ci_equal(host, cookie_host)) return 1;

    size_t host_len = strlen(host);
    size_t cookie_len = strlen(cookie_host);
    if (host_len <= cookie_len) return 0;
    if (strcmp(host + host_len - cookie_len, cookie_host) != 0) return 0;
    return host[host_len - cookie_len - 1] == '.';
}

static int http_path_matches_cookie(const char* req_path, const char* cookie_path) {
    if (!req_path || !cookie_path || !cookie_path[0]) return 0;
    if (cookie_path[0] != '/') return 0;

    size_t req_len = strlen(req_path);
    size_t cookie_len = strlen(cookie_path);
    if (cookie_len == 1) return req_len > 0 && req_path[0] == '/';
    if (req_len < cookie_len) return 0;
    if (strncmp(req_path, cookie_path, cookie_len) != 0) return 0;
    if (req_len == cookie_len) return 1;
    if (cookie_path[cookie_len - 1] == '/') return 1;
    return req_path[cookie_len] == '/' || req_path[cookie_len] == '\0';
}

static const char* http_path_dir(const char* path, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return "/";
    if (!path || !path[0]) {
        out[0] = '/';
        out[1] = '\0';
        return out;
    }
    size_t len = strlen(path);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
    char* slash = strrchr(out, '/');
    if (!slash) {
        out[0] = '/';
        out[1] = '\0';
        return out;
    }
    if (slash == out) {
        slash[1] = '\0';
        return out;
    }
    slash[1] = '\0';
    return out;
}

static void http_store_cookie_from_parts(http_scheme_t scheme,
                                        const char *request_host,
                                        const char *request_path,
                                        const char* set_cookie) {
    if (!request_host || !request_path || !set_cookie || !set_cookie[0]) return;

    char line[1024];
    http_copy_text(line, sizeof(line), set_cookie);

    char* semi = strchr(line, ';');
    if (semi) *semi = '\0';

    char* eq = strchr(line, '=');
    if (!eq || eq == line) return;
    *eq = '\0';

    char name[HTTP_COOKIE_NAME_MAX];
    char value[HTTP_COOKIE_VALUE_MAX];
    http_copy_text(name, sizeof(name), line);
    http_copy_text(value, sizeof(value), eq + 1);
    if (!name[0]) return;

    char cookie_host[HTTP_MAX_HOST];
    char cookie_path[HTTP_COOKIE_PATH_MAX];
    http_copy_text(cookie_host, sizeof(cookie_host), request_host);
    http_path_dir(request_path, cookie_path, sizeof(cookie_path));

    int secure = (scheme == HTTP_SCHEME_HTTPS);
    int remove = 0;

    const char* attr = semi ? semi + 1 : NULL;
    while (attr && *attr) {
        while (*attr == ' ' || *attr == '\t' || *attr == ';') ++attr;
        if (!*attr) break;
        const char* next = strchr(attr, ';');
        size_t attr_len = next ? (size_t)(next - attr) : strlen(attr);

        if (attr_len >= 7 && http_ci_starts_with(attr, "Domain=")) {
            size_t dlen = attr_len - 7;
            if (dlen >= sizeof(cookie_host)) dlen = sizeof(cookie_host) - 1;
            memcpy(cookie_host, attr + 7, dlen);
            cookie_host[dlen] = '\0';
            while (cookie_host[0] == '.') {
                memmove(cookie_host, cookie_host + 1, strlen(cookie_host));
            }
        } else if (attr_len >= 5 && http_ci_starts_with(attr, "Path=")) {
            size_t plen = attr_len - 5;
            if (plen >= sizeof(cookie_path)) plen = sizeof(cookie_path) - 1;
            memcpy(cookie_path, attr + 5, plen);
            cookie_path[plen] = '\0';
            if (cookie_path[0] == '\0') http_copy_text(cookie_path, sizeof(cookie_path), "/");
        } else if (attr_len >= 6 && http_ci_starts_with(attr, "Secure")) {
            secure = 1;
        } else if (attr_len >= 8 && http_ci_starts_with(attr, "Max-Age=")) {
            const char* age = attr + 8;
            if (atoi(age) <= 0) remove = 1;
        }
        attr = next ? next + 1 : NULL;
    }

    http_spin_lock(&g_http_cookie_lock);
    for (int i = 0; i < HTTP_COOKIE_MAX; ++i) {
        if (g_http_cookies[i].active &&
            http_ci_equal(g_http_cookies[i].host, cookie_host) &&
            http_ci_equal(g_http_cookies[i].path, cookie_path) &&
            http_ci_equal(g_http_cookies[i].name, name)) {
            if (remove) {
                memset(&g_http_cookies[i], 0, sizeof(g_http_cookies[i]));
                http_spin_unlock(&g_http_cookie_lock);
                return;
            }
            http_copy_text(g_http_cookies[i].value, sizeof(g_http_cookies[i].value), value);
            g_http_cookies[i].secure = secure;
            http_spin_unlock(&g_http_cookie_lock);
            return;
        }
    }

    if (remove) {
        http_spin_unlock(&g_http_cookie_lock);
        return;
    }

    for (int i = 0; i < HTTP_COOKIE_MAX; ++i) {
        if (!g_http_cookies[i].active) {
            g_http_cookies[i].active = 1;
            g_http_cookies[i].secure = secure;
            http_copy_text(g_http_cookies[i].host, sizeof(g_http_cookies[i].host), cookie_host);
            http_copy_text(g_http_cookies[i].path, sizeof(g_http_cookies[i].path), cookie_path);
            http_copy_text(g_http_cookies[i].name, sizeof(g_http_cookies[i].name), name);
            http_copy_text(g_http_cookies[i].value, sizeof(g_http_cookies[i].value), value);
            http_spin_unlock(&g_http_cookie_lock);
            return;
        }
    }
    http_spin_unlock(&g_http_cookie_lock);
}

static void http_store_cookie_from_header(const http_client_t* http, const char* set_cookie)
{
    if (http == NULL) return;
    http_store_cookie_from_parts(http->scheme, http->host, http->path, set_cookie);
}

void http_store_cookie_header_for(const http_client_t* http, const char* set_cookie)
{
    if (http != NULL && set_cookie != NULL) {
        http_store_cookie_from_header(http, set_cookie);
    }
}

void http_store_cookie_header(const char* set_cookie)
{
    /* Legacy callers do not provide an origin, so accepting their cookie would
     * create a cross-request association.  The internal response parser and
     * NetSurf fetcher use http_store_cookie_header_for() instead. */
    (void)set_cookie;
}

void http_store_cookies_from_response(http_client_t* http) {
    if (!http || !http->response[0]) return;
    char* headers = http->response;
    char* end = http_find_header_end(http->response);
    if (end) *end = '\0';

    char* p = headers;
    while (*p) {
        char* line_end = strstr(p, "\r\n");
        if (!line_end) line_end = p + strlen(p);
        if (http_ci_starts_with(p, "Set-Cookie:")) {
            const char* value = p + 11;
            while (*value == ' ' || *value == '\t') ++value;
            http_store_cookie_from_header(http, value);
        }
        if (*line_end == '\0') break;
        p = line_end + 2;
    }

    if (end) *end = '\r';
}

static size_t http_append_cookie_header_for_parts(http_scheme_t scheme,
                                                   const char *host,
                                                   const char *path,
                                                   char* out, size_t out_sz) {
    if (!host || !path || !out || out_sz == 0) return 0;
    size_t written = 0;
    int first = 1;
    
    http_spin_lock(&g_http_cookie_lock);
    for (int i = 0; i < HTTP_COOKIE_MAX; ++i) {
        const http_cookie_t* c = &g_http_cookies[i];
        if (!c->active) continue;
        if (!http_host_matches_cookie(host, c->host)) continue;
        if (scheme == HTTP_SCHEME_HTTP && c->secure) continue;
        if (!http_path_matches_cookie(path, c->path)) continue;

        int n = snprintf(out + written, out_sz - written, "%s%s=%s",
                         first ? "" : "; ", c->name, c->value);
        if (n < 0 || (size_t)n >= out_sz - written) break;
        written += (size_t)n;
        first = 0;
    }
    http_spin_unlock(&g_http_cookie_lock);
    return written;
}

size_t http_append_cookie_header_for_transport(const http_client_t* http, char* out, size_t out_sz) {
    if (http == NULL) return 0;
    return http_append_cookie_header_for_parts(http->scheme, http->host, http->path,
                                               out, out_sz);
}

/* Build only the tiny URL scope required by the cookie jar. This deliberately
 * avoids instantiating http_client_t here: that type owns a 10 MiB response
 * buffer and must never be put on a GUI/QuickJS stack for DOM cookie access. */
static int http_cookie_scope_from_url(const char *url, http_scheme_t *scheme,
                                      char *host, char *path)
{
    uint64_t port = 0;
    if (url == NULL || scheme == NULL || host == NULL || path == NULL) return -1;
    if (http_ci_starts_with(url, "https://")) *scheme = HTTP_SCHEME_HTTPS;
    else if (http_ci_starts_with(url, "http://")) *scheme = HTTP_SCHEME_HTTP;
    else return -1;
    return http_parse_url(url, host, path, &port);
}

int http_set_document_cookie_for_url(const char *url, const char *set_cookie)
{
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    http_scheme_t scheme;
    if (http_cookie_scope_from_url(url, &scheme, host, path) != 0) return -1;
    http_store_cookie_from_parts(scheme, host, path, set_cookie);
    return 0;
}

size_t http_get_document_cookie_for_url(const char *url, char *out, size_t out_sz)
{
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    http_scheme_t scheme;
    if (out != NULL && out_sz != 0) out[0] = '\0';
    if (http_cookie_scope_from_url(url, &scheme, host, path) != 0) return 0;
    return http_append_cookie_header_for_parts(scheme, host, path, out, out_sz);
}

static int http_is_https(const http_client_t* http) {
    return http && http->scheme == HTTP_SCHEME_HTTPS;
}

static void http_drive_network(void) {
    for (int i = 0; i < 8; ++i) {
        net_poll();
    }
}

static int http_has_ipv4_lease(void) {
    ip_addr_t ip = dhcp_get_ip();
    return net_is_connected() &&
           (ip.addr[0] != 0 || ip.addr[1] != 0 ||
            ip.addr[2] != 0 || ip.addr[3] != 0);
}

static int http_ensure_network_ready(void) {
    /* NIC registration precedes DHCP. Do not start DNS/TLS in that interval,
     * otherwise the initial resolver can race the DHCP DNS-server update. */
    if (http_has_ipv4_lease()) return 0;

    /* Keep polling the already-running DHCP state machine. Calling net_init()
     * while an interface exists resets DHCP and was the reason repeated HTTPS
     * requests never reached BOUND. */
    for (int i = 0; i < 64; ++i) {
        http_drive_network();
        if (http_has_ipv4_lease()) return 0;
        for (volatile int delay = 0; delay < 20000; ++delay) { }
    }

    if (!net_is_connected()) {
        serial_puts("[HTTP] Network interface not ready, initializing stack\n");
        net_init();
        for (int i = 0; i < 64; ++i) {
            http_drive_network();
            if (http_has_ipv4_lease()) return 0;
            for (volatile int delay = 0; delay < 20000; ++delay) { }
        }
    }

    serial_puts("[HTTP] DHCP lease not ready\n");
    return -1;
}

static void http_reset_response(http_client_t* http) {
    if (!http) return;
    http->status_code = 0;
    http->response_len = 0;
    http->response[0] = '\0';
    http->keep_alive = 0;
}

static int http_transport_send(http_client_t* http, const char* data, size_t len) {
    if (!http || !data) return -1;
    if (http_is_https(http)) {
        if (!http->tls_session) return -1;
        return tls_send(http->tls_session, data, len);
    }
    if (!http->socket) return -1;
    return socket_send(http->socket, (void*)data, len, 0);
}

static int http_transport_recv(http_client_t* http, char* buffer, size_t max_len) {
    if (!http || !buffer || max_len == 0) return -1;
    if (http_is_https(http)) {
        if (!http->tls_session) return -1;
        return tls_recv(http->tls_session, buffer, max_len);
    }
    if (!http->socket) return -1;
    return socket_recv(http->socket, buffer, max_len, 0);
}

static void http_transport_close(http_client_t* http) {
    if (!http) return;
    if (http->tls_session) {
        tls_close(http->tls_session);
        http->tls_session = NULL;
    }
    if (http->socket) {
        socket_close(http->socket);
        http->socket = NULL;
    }
    http->connected = 0;
}

static void http_keepalive_slot_close(http_keepalive_slot_t* slot) {
    if (slot == NULL || !slot->active) return;
    if (slot->tls_session != NULL) {
        tls_close(slot->tls_session);
        slot->tls_session = NULL;
    }
    if (slot->socket != NULL) {
        socket_close(slot->socket);
        slot->socket = NULL;
    }
    slot->active = 0;
    slot->host[0] = '\0';
}

static int http_keepalive_take(http_client_t* http) {
    if (http == NULL) return 0;
    http_spin_lock(&g_http_keepalive_lock);
    for (int i = 0; i < HTTP_KEEPALIVE_POOL_SIZE; ++i) {
        http_keepalive_slot_t* slot = &g_http_keepalive_pool[i];
        if (!slot->active || slot->scheme != http->scheme ||
            slot->port != http->port || !http_ci_equal(slot->host, http->host)) {
            continue;
        }
        http->socket = slot->socket;
        http->tls_session = slot->tls_session;
        http->connected = 1;
        slot->socket = NULL;
        slot->tls_session = NULL;
        slot->active = 0;
        http_spin_unlock(&g_http_keepalive_lock);
        serial_puts("[HTTP] Reusing pooled Keep-Alive transport\n");
        return 1;
    }
    http_spin_unlock(&g_http_keepalive_lock);
    return 0;
}

static void http_keepalive_store(http_client_t* http) {
    if (http == NULL || !http->connected || !http->keep_alive) return;
    int target = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < HTTP_KEEPALIVE_POOL_SIZE; ++i) {
        http_keepalive_slot_t* slot = &g_http_keepalive_pool[i];
        if (!slot->active) { target = i; break; }
        if (slot->last_used < oldest) { oldest = slot->last_used; target = i; }
    }
    if (target < 0) return;
    tls_session_t* old_tls = NULL;
    socket_t* old_socket = NULL;
    http_spin_lock(&g_http_keepalive_lock);
    http_keepalive_slot_t* slot = &g_http_keepalive_pool[target];
    if (slot->active) {
        old_tls = slot->tls_session;
        old_socket = slot->socket;
    }
    slot->socket = http->socket;
    slot->tls_session = http->tls_session;
    slot->port = http->port;
    slot->scheme = http->scheme;
    http_copy_text(slot->host, sizeof(slot->host), http->host);
    slot->last_used = get_timer_ticks();
    slot->active = 1;
    http->socket = NULL;
    http->tls_session = NULL;
    http->connected = 0;
    http->keep_alive = 0;
    http_spin_unlock(&g_http_keepalive_lock);
    /* Closing an evicted TLS session may poll/yield; it must never happen
     * under the metadata lock. */
    if (old_tls != NULL) tls_close(old_tls);
    if (old_socket != NULL) socket_close(old_socket);
}

static char* http_find_header_end(char* response) {
    return response ? strstr(response, "\r\n\r\n") : NULL;
}

static char* http_find_header_value(char* headers, const char* key) {
    if (!headers || !key || !key[0]) return NULL;
    size_t key_len = strlen(key);
    char* p = headers;
    while (*p) {
        char* line_end = strstr(p, "\r\n");
        if (!line_end) line_end = p + strlen(p);
        if ((size_t)(line_end - p) > key_len + 1) {
            if (http_header_name_matches(p, key)) {
                char* value = p + key_len + 1;
                while (*value == ' ' || *value == '\t') value++;
                return value;
            }
        }
        if (*line_end == '\0') break;
        p = line_end + 2;
    }
    return NULL;
}


const char* http_get_header(http_client_t* http, const char* name) {
    if (!http || !name || !name[0]) return NULL;
    char* headers = http->response;
    char* end = http_find_header_end(http->response);
    if (!headers || !end) return NULL;
    *end = '\0';

    size_t name_len = strlen(name);
    char* p = headers;
    while (*p) {
        char* line_end = strstr(p, "\r\n");
        if (!line_end) line_end = p + strlen(p);
        if ((size_t)(line_end - p) > name_len + 1 && http_header_name_matches(p, name)) {
            char* value = p + name_len + 1;
            while (*value == ' ' || *value == '\t') value++;
            *end = '\r';
            return value;
        }
        if (*line_end == '\0') break;
        p = line_end + 2;
    }
    *end = '\r';
    return NULL;
}

static int http_has_chunked_encoding(char* headers) {
    char* te = http_find_header_value(headers, "Transfer-Encoding");
    if (!te) return 0;
    return strstr(te, "chunked") != NULL;
}

static int http_copy_chunked_body(char* body, size_t body_len, char* out, size_t out_max) {
    size_t in_pos = 0;
    size_t out_pos = 0;
    if (!body || !out || out_max == 0) return -1;
    while (in_pos < body_len) {
        unsigned long chunk_len = 0;
        int saw_digit = 0;
        int in_extension = 0;
        int header_complete = 0;
        while (in_pos < body_len) {
            char c = body[in_pos++];
            if (c == '\r') continue;
            if (c == '\n') { header_complete = 1; break; }
            if (c == ';' && saw_digit) { in_extension = 1; continue; }
            if (in_extension) continue; /* ignore RFC chunk extensions */
            if (c >= '0' && c <= '9') { saw_digit = 1; chunk_len = (chunk_len << 4) + (unsigned long)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { saw_digit = 1; chunk_len = (chunk_len << 4) + (unsigned long)(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { saw_digit = 1; chunk_len = (chunk_len << 4) + (unsigned long)(c - 'A' + 10); }
            else return out_pos > 0 ? (int)out_pos : -1;
        }
        if (!header_complete || !saw_digit) return out_pos > 0 ? (int)out_pos : -1;
        if (chunk_len == 0) break;
        /* The response buffer is deliberately bounded.  If its final
         * chunk is incomplete, preserve all preceding complete chunks so
         * the browser can render the valid leading HTML instead of turning
         * a large Internet page into a total transport failure. */
        if (in_pos + chunk_len > body_len) {
            /* A large first chunk (Google currently uses 0x8000 bytes) can
             * exceed HTTP_MAX_RESP.  Copy the available prefix of that
             * chunk; it is already valid HTML and is sufficient for a
             * meaningful NetSurf render. */
            size_t available = body_len - in_pos;
            size_t room = out_max - 1u - out_pos;
            if (available > room) available = room;
            if (available > 0) {
                memcpy(out + out_pos, body + in_pos, available);
                out_pos += available;
            }
            out[out_pos] = '\0';
            return out_pos > 0 ? (int)out_pos : -1;
        }
        if (out_pos + chunk_len >= out_max) chunk_len = out_max - 1 - out_pos;
        memcpy(out + out_pos, body + in_pos, (size_t)chunk_len);
        out_pos += (size_t)chunk_len;
        in_pos += (size_t)chunk_len;
        if (in_pos + 1 < body_len && body[in_pos] == '\r' && body[in_pos + 1] == '\n') in_pos += 2;
        else if (in_pos < body_len && body[in_pos] == '\n') in_pos += 1;
        else {
            while (in_pos < body_len && body[in_pos] != '\n') in_pos++;
            if (in_pos < body_len) in_pos++;
        }
    }
    out[out_pos] = '\0';
    return (int)out_pos;
}

/* HTTP_MAX_RESP is intentionally large enough for real web documents.
 * The decode workspace is allocated per http_client_t, never on a kernel
 * stack and never shared across simultaneous requests. */

int http_decode_response(http_client_t* http) {
    char* headers = http->response;
    char* header_end = http_find_header_end(http->response);
    if (!header_end) {
        http->status_code = 0;
        return -1;
    }

    /* Temporarily terminate the header block for status/header parsing, but
     * retain the wire-format delimiter afterwards.  Public body accessors
     * locate the delimiter again, and leaving this byte as NUL causes them
     * to fall back to the entire HTTP response (headers included). */
    *header_end = '\0';
    char* body = header_end + 4;

    if (strncmp(headers, "HTTP/", 5) == 0) {
        char* space = strchr(headers, ' ');
        if (space) http->status_code = atoi(space + 1);
    }

    if (http_has_chunked_encoding(headers)) {
        if (http->decode_workspace == NULL) {
            *header_end = '\r';
            return -1;
        }
        int decoded_len = http_copy_chunked_body(body, (size_t)(http->response_len - (uint64_t)(body - http->response)), http->decode_workspace, HTTP_MAX_RESP);
        if (decoded_len < 0) {
            serial_puts("[HTTP] Failed to decode chunked body\n");
            *header_end = '\r';
            return -1;
        }
        size_t header_len = (size_t)(body - http->response);
        if (header_len + (size_t)decoded_len + 1 > sizeof(http->response)) {
            *header_end = '\r';
            return -1;
        }
        memcpy(http->response + header_len, http->decode_workspace, (size_t)decoded_len + 1);
        http->response_len = (uint64_t)(header_len + (size_t)decoded_len);
        /* Falls through to Content-Encoding below rather than returning here:
         * chunking is only how these bytes were framed for the wire, not
         * what the content actually is - a chunked AND gzipped response is
         * completely ordinary and needs both undone, in that order. */
    }

    /* Content codings are undone only after the HTTP chunk framing has been
     * removed. Both decoders write to bounded static workspaces while the
     * transport gate is held, then restore the usual contiguous response that
     * NetSurf and QuickJS already consume. */
    char* content_encoding = http_find_header_value(headers, "Content-Encoding");
    size_t header_len = (size_t)(body - http->response);
    size_t body_len = (size_t)(http->response_len - (uint64_t)header_len);
    size_t body_cap = sizeof(http->response) - header_len - 1u;
    if (content_encoding && http_ci_starts_with(content_encoding, "br")) {
        size_t decoded_len = 0;
        int br_rc = body_cap == 0 ? -1 :
            cos_brotli_decode_http_body((const uint8_t*)body, body_len,
                (uint8_t*)http->decode_workspace, body_cap, &decoded_len);
        if (br_rc >= 0) {
            memcpy(http->response + header_len, http->decode_workspace, decoded_len);
            http->response[header_len + decoded_len] = '\0';
            http->response_len = (uint64_t)(header_len + decoded_len);
            if (br_rc > 0) {
                serial_puts("[HTTP] Brotli body clipped to response capacity; rendering prefix\n");
            }
        } else {
            serial_puts("[HTTP] Failed to decode Brotli body\n");
            *header_end = '\r';
            return -1;
        }
    } else if (content_encoding && http_ci_starts_with(content_encoding, "gzip")) {
        /* gzip_inflate deliberately succeeds after filling its output cap.
         * Reserve the response header and NUL terminator before invoking it,
         * so a decompressed page larger than HTTP_MAX_RESP yields a valid
         * leading document rather than an avoidable all-or-nothing failure. */
        uint64_t decoded_len = 0;
        if (body_cap != 0 &&
            gzip_inflate((const uint8_t*)body, (uint64_t)body_len,
                         (uint8_t*)http->decode_workspace,
                         (uint64_t)body_cap, &decoded_len)) {
            memcpy(http->response + header_len, http->decode_workspace, (size_t)decoded_len);
            http->response[header_len + (size_t)decoded_len] = '\0';
            http->response_len = (uint64_t)(header_len + (size_t)decoded_len);
            if ((size_t)decoded_len == body_cap) {
                serial_puts("[HTTP] gzip body clipped to response capacity; rendering prefix\n");
            }
        } else {
            serial_puts("[HTTP] Failed to decode gzip body\n");
            *header_end = '\r';
            return -1;
        }
    }

    *header_end = '\r';
    return 0;
}

static int http_build_request(http_client_t* http, const char* method, const char* data, char* request, size_t request_sz) {
    if (!http || !method || !request || request_sz == 0) return -1;
    request[0] = '\0';

    /* Advertise the actual engine rather than Chrome.  Major search sites
     * otherwise select an application-shell response that hides its HTML
     * form until a full browser DOM implementation has run.  NetSurf's own
     * identity selects the standards-based fallback markup that this native
     * NetSurf form/link path can render and submit. */
    const char* ua = "NetSurf/3.11 (C-OS; x86_64; QuickJS)";
    char cookie_hdr[1024];
    cookie_hdr[0] = '\0';
    http_append_cookie_header_for_transport(http, cookie_hdr, sizeof(cookie_hdr));

    if (data) {
        int len = snprintf(request, request_sz,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
            "Accept-Encoding: br, gzip\r\n"
            "Accept-Language: en-US,en;q=0.9\r\n"
            "Upgrade-Insecure-Requests: 1\r\n"
            "%s%s%s"
            "Content-Type: application/x-www-form-urlencoded\r\n"
            "Content-Length: %u\r\n"
            "Connection: keep-alive\r\n\r\n"
            "%s",
            method, http->path, http->host, ua,
            cookie_hdr[0] ? "Cookie: " : "",
            cookie_hdr[0] ? cookie_hdr : "",
            cookie_hdr[0] ? "\r\n" : "",
            (unsigned)strlen(data), data);
        return (len < 0 || (size_t)len >= request_sz) ? -1 : 0;
    }

    int len = snprintf(request, request_sz,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
        "Accept-Encoding: br, gzip\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Upgrade-Insecure-Requests: 1\r\n"
        "%s%s%s"
        "Connection: keep-alive\r\n\r\n",
        method, http->path, http->host, ua,
        cookie_hdr[0] ? "Cookie: " : "",
        cookie_hdr[0] ? cookie_hdr : "",
        cookie_hdr[0] ? "\r\n" : "");
    return (len < 0 || (size_t)len >= request_sz) ? -1 : 0;
}

http_client_t* http_create(void) {
    http_client_t* http = (http_client_t*)kmalloc(sizeof(http_client_t));
    if (!http) return NULL;

    memset(http, 0, sizeof(http_client_t));
    http->decode_workspace = (char*)kmalloc(HTTP_MAX_RESP);
    if (http->decode_workspace == NULL) {
        kfree(http);
        return NULL;
    }
    http->socket = NULL;
    http->tls_session = NULL;
    http->connected = 0;
    http->scheme = HTTP_SCHEME_HTTP;
    http->status_code = 0;

    return http;
}

void http_destroy(http_client_t* http) {
    if (!http) return;
    /* The client owns its socket/TLS/decode buffers. Keep-alive metadata has
     * its dedicated lock, so completed owner-thread cleanup must not consume a
     * scarce active-transfer permit while other workers are in flight. */
    http_keepalive_store(http);
    http_transport_close(http);
    if (http->decode_workspace != NULL) {
        kfree(http->decode_workspace);
        http->decode_workspace = NULL;
    }
    kfree(http);
}

int http_parse_url(const char* url, char* host, char* path, uint64_t* port) {
    const char* pos = url;
    uint64_t default_port = HTTP_PORT;

    if (!url || !host || !path) return -1;

    if (strncmp(pos, "http://", 7) == 0) {
        pos += 7;
        default_port = HTTP_PORT;
    } else if (strncmp(pos, "https://", 8) == 0) {
        pos += 8;
        default_port = HTTPS_PORT;
    }

    if (port) *port = default_port;

    const char* slash = strchr(pos, '/');
    const char* colon = strchr(pos, ':');

    int host_len;
    if (slash) {
        host_len = (int)(slash - pos);
        if (colon && colon < slash) {
            host_len = (int)(colon - pos);
            if (port) *port = (uint64_t)atoi(colon + 1);
        }
    } else {
        host_len = (int)strlen(pos);
        if (colon) {
            host_len = (int)(colon - pos);
            if (port) *port = (uint64_t)atoi(colon + 1);
        }
    }

    if (host_len <= 0) return -1;
    if (host_len >= HTTP_MAX_HOST) host_len = HTTP_MAX_HOST - 1;
    memcpy(host, pos, (size_t)host_len);
    host[host_len] = '\0';

    if (slash) {
        strncpy(path, slash, HTTP_MAX_PATH - 1);
        path[HTTP_MAX_PATH - 1] = '\0';
    } else {
        snprintf(path, HTTP_MAX_PATH, "/");
    }

    return 0;
}

/* Returns 1 only for a strict dotted-decimal IPv4 literal. */
static int http_parse_ipv4_literal(const char* text, ip_addr_t* out) {
    if (!text || !out) return 0;
    const char* p = text;
    for (int part = 0; part < 4; ++part) {
        int value = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            if (value > 255) return 0;
            ++p;
            ++digits;
        }
        if (digits == 0) return 0;
        out->addr[part] = (uint8_t)value;
        if (part < 3) {
            if (*p != '.') return 0;
            ++p;
        }
    }
    return *p == '\0';
}

int http_connect(http_client_t* http) {
    if (!http) return -1;

    if (http_ensure_network_ready() < 0) {
        serial_puts("[HTTP] Network stack unavailable\n");
        return -1;
    }

    /* A pooled HTTP/1.1 connection cannot be upgraded to h2 in-place.
     * Requests that may negotiate h2 always start a new TLS connection. */
    if (!http->allow_http2 && http_keepalive_take(http)) {
        return 0;
    }

    if (http_is_https(http)) {
        if (!tls_backend_available()) {
            serial_puts("[HTTP] HTTPS requested but TLS backend is unavailable\n");
            return -1;
        }
        http->tls_session = http->allow_http2 ?
            tls_connect_prefer_http2(http->host, http->port) :
            tls_connect(http->host, http->port);
        if (!http->tls_session) {
            serial_puts("[HTTP] TLS connect failed\n");
            return -1;
        }
        http->connected = 1;
        serial_puts("[HTTP] Connected via TLS\n");
        return 0;
    }

    int resolved = http_parse_ipv4_literal(http->host, &http->server_ip) ? 0 : -1;
    if (resolved == 0) {
        serial_puts("[HTTP] Using numeric IPv4 host\n");
    }
    for (int attempt = 0; resolved < 0 && attempt < 4; ++attempt) {
        resolved = dns_resolve(http->host, &http->server_ip);
        if (resolved == 0) break;
        http_drive_network();
    }
    if (resolved < 0) {
        serial_puts("[HTTP] Failed to resolve hostname\n");
        return -1;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (http->socket) {
            socket_close(http->socket);
            http->socket = NULL;
        }

        http->socket = socket_create(AF_INET, SOCK_STREAM, 0);
        if (!http->socket) {
            serial_puts("[HTTP] Failed to create socket\n");
            return -1;
        }

        sock_addr_t addr;
        addr.family = AF_INET;
        addr.port = http->port;
        addr.addr = http->server_ip;

        if (socket_connect(http->socket, &addr) == 0) {
            http->connected = 1;
            serial_puts("[HTTP] Connected to server\n");
            return 0;
        }

        serial_puts("[HTTP] Connect attempt failed, retrying...\n");
        http_transport_close(http);
        http_drive_network();
    }

    serial_puts("[HTTP] Failed to connect\n");
    return -1;
}

int http_send_request(http_client_t* http, const char* method, const char* data) {
    if (!http || !method) return -1;
    uint64_t t_connect_start = get_timer_ticks();
    if (!http->connected) {
        if (http_connect(http) < 0) return -1;
    }
    uint64_t t_connected = get_timer_ticks();

    /* Only an ALPN-confirmed h2 connection is routed into nghttp2. A peer
     * selecting http/1.1 (or no ALPN) remains on the established path. */
    if (http->allow_http2 && http_is_https(http)) {
        const char *alpn = tls_get_selected_protocol(http->tls_session);
        if (alpn && strcmp(alpn, "h2") == 0) {
            serial_puts("[HTTP/2] ALPN selected h2; starting single GET stream\n");
            if (strcmp(method, "GET") == 0 && http2_get_response(http) == 0) {
                return 1; /* Response is already complete and decoded. */
            }
            serial_puts("[HTTP/2] h2 request failed; caller may retry HTTP/1.1\n");
            return -1;
        }
    }

    char request[HTTP_MAX_URL];
    if (http_build_request(http, method, data, request, sizeof(request)) < 0) {
        serial_puts("[HTTP] Request too large\n");
        return -1;
    }

    serial_puts("[HTTP] Sending request:\n");
    serial_puts(request);
    serial_puts("\n");

    int sent = http_transport_send(http, request, strlen(request));
    uint64_t t_sent = get_timer_ticks();
    serial_puts("[HTTP/PERF] transport_connect_ms=");
    serial_putdec(t_connected - t_connect_start);
    serial_puts(" request_write_ms=");
    serial_putdec(t_sent - t_connected);
    serial_puts("\n");
    if (sent < 0 && strcmp(method, "GET") == 0) {
        /* A just-created TLS session can occasionally fail while driving its
         * first handshake record (for example during initial DHCP/DNS/NIC
         * settling).  No HTTP GET bytes have been accepted by this function on
         * the error path, so one fresh-transport retry is idempotent and avoids
         * turning a transient first-click failure into a blank browser page.
         * Do not automatically replay POST. */
        serial_puts("[HTTP] Initial GET send failed; reconnecting once\n");
        http_transport_close(http);
        if (http_connect(http) == 0) {
            sent = http_transport_send(http, request, strlen(request));
        }
    }
    if (sent < 0) {
        serial_puts("[HTTP] Failed to send request\n");
        return -1;
    }

    return 0;
}

/* Validate chunk boundaries without using C string functions: a chunked body
 * can be gzip-compressed and therefore contain embedded NUL bytes. The old
 * strstr("0\\r\\n\\r\\n") check both stopped at such NULs and rejected legal
 * trailer fields after the zero-size chunk. */
static int http_chunked_body_complete(const char* body, size_t body_len)
{
    if (!body) return 0;
    size_t pos = 0;
    while (pos < body_len) {
        unsigned long chunk_len = 0;
        int saw_digit = 0;
        int in_extension = 0;
        int header_complete = 0;
        while (pos < body_len) {
            unsigned char c = (unsigned char)body[pos++];
            if (c == '\r') continue;
            if (c == '\n') { header_complete = 1; break; }
            if (c == ';' && saw_digit) { in_extension = 1; continue; }
            if (in_extension) continue;
            if (c >= '0' && c <= '9') {
                saw_digit = 1;
                chunk_len = (chunk_len << 4) + (unsigned long)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                saw_digit = 1;
                chunk_len = (chunk_len << 4) + (unsigned long)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                saw_digit = 1;
                chunk_len = (chunk_len << 4) + (unsigned long)(c - 'A' + 10);
            } else {
                return 0;
            }
        }
        if (!header_complete || !saw_digit) return 0;

        if (chunk_len == 0) {
            /* Consume zero or more RFC trailer lines, ending with an empty
             * line. A bare final LF is accepted for permissive HTTP/1.0 peers. */
            while (pos < body_len) {
                size_t line_start = pos;
                while (pos < body_len && body[pos] != '\n') ++pos;
                if (pos == body_len) return 0;
                size_t line_end = pos++;
                if (line_end == line_start ||
                    (line_end == line_start + 1 && body[line_start] == '\r')) {
                    return 1;
                }
            }
            return 0;
        }

        if (chunk_len > body_len - pos) return 0;
        pos += (size_t)chunk_len;
        if (pos >= body_len) return 0;
        if (body[pos] == '\r') {
            ++pos;
            if (pos >= body_len || body[pos] != '\n') return 0;
            ++pos;
        } else if (body[pos] == '\n') {
            ++pos;
        } else {
            return 0;
        }
    }
    return 0;
}

static int http_response_complete(const http_client_t* http)
{
    if (!http || http->response_len == 0) return 0;
    char *response = (char *)http->response;
    char *header_end = http_find_header_end(response);
    if (!header_end) return 0;

    const char *body = header_end + 4;
    size_t body_len = (size_t)(http->response_len - (uint64_t)(body - response));
    char *headers = response;
    char saved = *header_end;
    *header_end = '\0';
    int chunked = http_has_chunked_encoding(headers);
    char *content_length = http_find_header_value(headers, "Content-Length");
    int status = 0;
    if (strncmp(headers, "HTTP/", 5) == 0) {
        char *space = strchr(headers, ' ');
        if (space) status = atoi(space + 1);
    }
    unsigned long expected = content_length ? strtoul(content_length, NULL, 10) : 0;
    *header_end = saved;

    /* HEAD/204/304 responses have no response body. */
    if (status == 204 || status == 304) return 1;
    if (chunked) {
        return http_chunked_body_complete(body, body_len);
    }
    if (content_length != NULL) return body_len >= (size_t)expected;
    return 0; /* close-delimited response: wait for transport EOF */
}

/* RFC-compliant close-delimited bodies are complete only once the peer closes
 * the connection.  The local TCP and BearSSL wrappers report that condition
 * as a negative read result after buffered plaintext is drained.  Keep this
 * decision separate from http_response_complete(): a response without an
 * explicit length must never be accepted merely because a five-second wait
 * expired. */
static void http_log_framing_failure(const http_client_t* http, int transport_eof)
{
    if (!http || http->response_len == 0) return;
    char *response = (char *)http->response;
    char *header_end = http_find_header_end(response);
    serial_puts("[HTTP] framing diagnostics bytes=");
    serial_putdec(http->response_len);
    serial_puts(" eof=");
    serial_putdec((uint64_t)transport_eof);
    serial_puts(" header_end=");
    serial_putdec(header_end != NULL ? 1u : 0u);
    serial_puts("\n");
    if (header_end == NULL) return;

    char saved = *header_end;
    *header_end = '\0';
    char *content_length = http_find_header_value(response, "Content-Length");
    char *transfer_encoding = http_find_header_value(response, "Transfer-Encoding");
    char *connection = http_find_header_value(response, "Connection");
    serial_puts("[HTTP] framing headers CL=");
    serial_puts(content_length ? content_length : "(none)");
    serial_puts(" TE=");
    serial_puts(transfer_encoding ? transfer_encoding : "(none)");
    serial_puts(" Connection=");
    serial_puts(connection ? connection : "(none)");
    serial_puts("\n");
    *header_end = saved;
}

static int http_response_accepts_close_delimited_eof(const http_client_t* http)
{
    if (!http || http->response_len == 0) return 0;
    char *response = (char *)http->response;
    char *header_end = http_find_header_end(response);
    if (!header_end) return 0;

    char saved = *header_end;
    *header_end = '\0';
    int chunked = http_has_chunked_encoding(response);
    char *content_length = http_find_header_value(response, "Content-Length");
    char *connection = http_find_header_value(response, "Connection");
    int http_10 = strncmp(response, "HTTP/1.0", 8) == 0;
    int connection_close = connection != NULL && http_ci_starts_with(connection, "close");
    *header_end = saved;

    return !chunked && content_length == NULL && (http_10 || connection_close);
}

int http_read_response(http_client_t* http) {
    if (!http) return -1;

    http_reset_response(http);

    char* buffer = http->receive_chunk;
    int total = 0;
    /* Internet peers can take materially longer than an in-process test
     * server to deliver their first response segment.  A loop-count timeout
     * expires in a few milliseconds on modern CPUs, so use the PIT-backed
     * kernel clock and allow a bounded ten-second network wait instead.  A
     * QEMU PCAP trace of neverssl.com showed its first valid response segment
     * at roughly 4.6 seconds; five seconds leaves no scheduler/NIC margin and
     * can incorrectly classify that slow but valid response as empty. */
    uint64_t deadline = get_timer_ticks() + 10000u;
    int transport_eof = 0;
    while (get_timer_ticks() < deadline) {
        int received = http_transport_recv(http, buffer, HTTP_RECEIVE_CHUNK_SIZE);
        if (received < 0) {
            transport_eof = 1;
            break;
        }
        if (received == 0) {
            http_drive_network();
            /* The transport is temporarily would-block. Yield this worker's
             * timeslice rather than spin so GUI/input and other ready network
             * work can make progress under the preemptive scheduler. */
            thread_yield();
            continue;
        }
        if (total + received >= HTTP_MAX_RESP - 1) break;
        memcpy(http->response + total, buffer, (size_t)received);
        total += received;
        http->response[total] = '\0';
        http->response_len = (uint64_t)total;
        if (http_response_complete(http)) {
            serial_puts("[HTTP] Response framing complete; stopping receive wait\n");
            break;
        }
    }

    http->response_len = (uint64_t)total;
    if (total <= 0) {
        serial_puts("[HTTP] Empty response\n");
        return -1;
    }

    /* Preserve framing state before chunk decoding replaces the wire-format
     * terminal chunk with its decoded body. */
    int framed_response = http_response_complete(http);
    if (!framed_response) {
        if (transport_eof && http_response_accepts_close_delimited_eof(http)) {
            /* The peer deliberately closed a close-delimited HTTP body.  It
             * is valid but not reusable as a keep-alive transport. */
            framed_response = 1;
            serial_puts("[HTTP] Close-delimited response completed at EOF\n");
        } else {
            serial_puts("[HTTP] Incomplete or unframed response\n");
            http_log_framing_failure(http, transport_eof);
            return -1;
        }
    }
    if (http_decode_response(http) < 0) {
        serial_puts("[HTTP] Response decode failed\n");
        return -1;
    }

    http_store_cookies_from_response(http);

    /* HTTP/1.1 is persistent by default, but only reuse a connection once a
     * self-delimited response has been received and the server has not opted
     * out with Connection: close.  Close-delimited bodies deliberately remain
     * non-reusable because their end is indicated by socket EOF. */
    const char* connection = http_get_header(http, "Connection");
    int server_closed = connection != NULL && http_ci_starts_with(connection, "close");
    int is_http_11 = strncmp(http->response, "HTTP/1.1", 8) == 0;
    http->keep_alive = (http->connected && framed_response &&
                        !server_closed && is_http_11) ? 1 : 0;
    if (http->keep_alive) {
        serial_puts("[HTTP] Keep-Alive reusable transport\n");
    }

    serial_puts("[HTTP] Response received, status: ");
    serial_putdec(http->status_code);
    serial_puts(", length: ");
    serial_putdec(http->response_len);
    serial_puts("\n");

    return 0;
}


static int http_path_is_absolute(const char* path) {
    return path && path[0] == '/';
}

static void http_build_absolute_url(const http_client_t* http, const char* location, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!http || !location || !location[0]) return;

    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
        http_copy_text(out, out_sz, location);
        return;
    }

    if (location[0] == '/' && location[1] == '/') {
        snprintf(out, out_sz, "%s:%s", http->scheme == HTTP_SCHEME_HTTPS ? "https" : "http", location);
        return;
    }

    if (location[0] == '/') {
        snprintf(out, out_sz, "%s://%s%s",
                 http->scheme == HTTP_SCHEME_HTTPS ? "https" : "http",
                 http->host, location);
        return;
    }

    char base[HTTP_MAX_PATH];
    http_path_dir(http->path, base, sizeof(base));
    if (base[0] != '/') http_copy_text(base, sizeof(base), "/");

    char combined[HTTP_MAX_PATH];
    http_copy_text(combined, sizeof(combined), base);
    if (combined[strlen(combined) - 1] != '/') {
        size_t l = strlen(combined);
        if (l + 1 < sizeof(combined)) {
            combined[l++] = '/';
            combined[l] = '\0';
        }
    }
    http_copy_text(out, out_sz, "");
    snprintf(out, out_sz, "%s://%s%s%s",
             http->scheme == HTTP_SCHEME_HTTPS ? "https" : "http",
             http->host, combined, location);
}

static int http_follow_redirect_if_needed(http_client_t* http, const char* method, const char* data, int depth) {
    if (!http || depth > 5) return -1;
    if (http->status_code != 301 && http->status_code != 302 && http->status_code != 303 &&
        http->status_code != 307 && http->status_code != 308) {
        return 0;
    }

    char* headers = http->response;
    char* body = http_find_header_end(http->response);
    if (body) *body = '\0';
    char* location = http_find_header_value(headers, "Location");
    if (!location || !location[0]) return 0;

    /* http_find_header_value returns a pointer into the header block rather
       than an allocated NUL-terminated value.  Bound Location at its CRLF;
       otherwise a redirect request appends every following response header to
       the request path, producing a malformed request and HTTP 400. */
    char location_value[HTTP_MAX_URL];
    size_t location_len = 0;
    while (location[location_len] != '\0' &&
           location[location_len] != '\r' &&
           location[location_len] != '\n' &&
           location_len + 1 < sizeof(location_value)) {
        location_value[location_len] = location[location_len];
        ++location_len;
    }
    location_value[location_len] = '\0';
    if (location_value[0] == '\0') return 0;

    int redirect_status = http->status_code;
    char next_url[HTTP_MAX_URL];
    http_build_absolute_url(http, location_value, next_url, sizeof(next_url));

    serial_puts("[HTTP] Redirect -> ");
    serial_puts(next_url[0] ? next_url : location_value);
    serial_puts("\n");

    const char* next = next_url[0] ? next_url : location_value;
    char next_host[HTTP_MAX_HOST];
    char next_path[HTTP_MAX_PATH];
    uint64_t next_port = 0;
    int next_scheme = (strncmp(next, "https://", 8) == 0) ?
                      HTTP_SCHEME_HTTPS : HTTP_SCHEME_HTTP;
    int same_origin = (http_parse_url(next, next_host, next_path, &next_port) == 0 &&
                       next_scheme == http->scheme && next_port == http->port &&
                       http_ci_equal(next_host, http->host));
    if (!http->keep_alive || !same_origin) {
        http_transport_close(http);
    } else {
        serial_puts("[HTTP] Redirect reusing same-origin Keep-Alive transport\n");
    }
    http_reset_response(http);

    const char* next_method = method;
    const char* next_data = data;
    if (redirect_status == 303) {
        next_method = "GET";
        next_data = NULL;
    } else if (redirect_status == 301 || redirect_status == 302) {
        if (method && strcmp(method, "POST") == 0) {
            next_method = "GET";
            next_data = NULL;
        }
    }
    return http_request_internal(http, next_method ? next_method : "GET", next, next_data, depth + 1);
}

static int http_request_internal(http_client_t* http, const char* method, const char* url, const char* data, int depth) {
    if (!http || !method || !url) return -1;
    if (depth > 5) return -1;

    char next_host[HTTP_MAX_HOST];
    char next_path[HTTP_MAX_PATH];
    uint64_t next_port = 0;
    if (http_parse_url(url, next_host, next_path, &next_port) < 0) {
        return -1;
    }
    http_scheme_t next_scheme = (strncmp(url, "https://", 8) == 0) ?
                                HTTP_SCHEME_HTTPS : HTTP_SCHEME_HTTP;
    if (http->connected &&
        (next_scheme != http->scheme || next_port != http->port ||
         !http_ci_equal(next_host, http->host))) {
        /* A keep-alive transport is origin-bound. Never send a request for a
         * different host, scheme or port through the previous connection. */
        http_transport_close(http);
    }
    if (http->connected && strcmp(method, "POST") == 0) {
        /* Servers may close an otherwise reusable HTTP/1.1 connection while
         * it sits in the small transport pool. A stale TLS session is safe to
         * retry for an idempotent GET, but replaying a POST after an uncertain
         * partial write can duplicate a state-changing form submission. Start
         * every POST on a fresh transport instead: this makes the first
         * attempt unambiguous and fixes real form actions such as Google's
         * cookie-consent save endpoint without silently replaying them. */
        serial_puts("[HTTP] POST uses fresh transport (closing Keep-Alive)\n");
        http_transport_close(http);
    }
    http_copy_text(http->host, sizeof(http->host), next_host);
    http_copy_text(http->path, sizeof(http->path), next_path);
    http->port = next_port;
    http->scheme = next_scheme;
    /* h2 is attempted only for a fresh, idempotent HTTPS GET. POST and plain
     * HTTP deliberately retain their proven HTTP/1.1 transport semantics. */
    http->allow_http2 = (next_scheme == HTTP_SCHEME_HTTPS &&
                         strcmp(method, "GET") == 0) ? 1 : 0;

    serial_puts("[HTTP] ");
    serial_puts(method);
    serial_puts(" ");
    serial_puts(url);
    serial_puts("\n");

    /* timer ticks are driven at 1 kHz. Emit per-stage timings so slow pages
     * can be attributed to connection/TLS versus body reception rather than
     * being misdiagnosed from end-to-end wall time alone. */
    uint64_t t0 = get_timer_ticks();
    int send_rc = http_send_request(http, method, data);
    if (send_rc < 0 && http->allow_http2 && strcmp(method, "GET") == 0) {
        /* An h2 failure must never leave a partially framed connection in the
         * HTTP/1.1 path. Discard it, then retry the idempotent GET with the
         * established http/1.1-only ALPN profile. */
        serial_puts("[HTTP/2] Fresh HTTP/1.1 fallback for failed GET\n");
        http_transport_close(http);
        http->allow_http2 = 0;
        send_rc = http_send_request(http, method, data);
    }
    if (send_rc < 0) return -1;
    uint64_t t1 = get_timer_ticks();
    int read_rc = (send_rc == 1) ? 0 : http_read_response(http);
    if (send_rc == 1) {
        /* http2_get_response has emitted GOAWAY. Do not reuse an h2 session
         * for a redirect or an HTTP/1.1 request; response bytes stay owned by
         * this client buffer after the transport is released. */
        http_transport_close(http);
    }
    if (read_rc < 0 && strcmp(method, "GET") == 0 && http->response_len == 0) {
        /* Some plain-HTTP origins silently drop a fresh request before
         * emitting even a status line.  A GET is idempotent at this transport
         * layer; retry exactly once on a brand-new connection, but never replay
         * POST and never retry partially received/malformed responses. */
        serial_puts("[HTTP] Empty GET response; reconnecting once\n");
        http_transport_close(http);
        send_rc = http_send_request(http, method, data);
        if (send_rc < 0) return -1;
        t1 = get_timer_ticks();
        read_rc = (send_rc == 1) ? 0 : http_read_response(http);
    }
    if (read_rc < 0) return -1;
    uint64_t t2 = get_timer_ticks();
    serial_puts("[HTTP/PERF] connect+send_ms=");
    serial_putdec(t1 - t0);
    serial_puts(" receive_ms=");
    serial_putdec(t2 - t1);
    serial_puts(" total_ms=");
    serial_putdec(t2 - t0);
    serial_puts(" bytes=");
    serial_putdec(http->response_len);
    serial_puts("\n");

    if (http->status_code == 301 || http->status_code == 302 || http->status_code == 303 ||
        http->status_code == 307 || http->status_code == 308) {
        return http_follow_redirect_if_needed(http, method, data, depth);
    }

    return 0;
}

int http_get(http_client_t* http, const char* url) {
    http_transport_gate_acquire();
    int rc = http_request_internal(http, "GET", url, NULL, 0);
    http_transport_gate_release();
    return rc;
}

int http_post(http_client_t* http, const char* url, const char* data) {
    http_transport_gate_acquire();
    int rc = http_request_internal(http, "POST", url, data, 0);
    http_transport_gate_release();
    return rc;
}

int http_status_code(http_client_t* http) {
    return http ? http->status_code : 0;
}

int http_get_effective_url(const http_client_t* http, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0) return -1;
    out[0] = '\0';
    if (http == NULL || http->host[0] == '\0' || http->path[0] == '\0') {
        return -1;
    }

    const char* scheme = (http->scheme == HTTP_SCHEME_HTTPS) ? "https" : "http";
    uint64_t default_port = (http->scheme == HTTP_SCHEME_HTTPS) ? HTTPS_PORT : HTTP_PORT;
    int written;
    if (http->port == default_port) {
        written = snprintf(out, out_sz, "%s://%s%s", scheme, http->host, http->path);
    } else {
        written = snprintf(out, out_sz, "%s://%s:%llu%s", scheme, http->host,
                           (unsigned long long)http->port, http->path);
    }
    if (written < 0 || (size_t)written >= out_sz) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

const char* http_response_body(http_client_t* http) {
    if (!http || !http->response_len) return NULL;
    char* body = http_find_header_end(http->response);
    if (body) return body + 4;
    return http->response;
}

uint64_t http_response_length(http_client_t* http) {
    if (!http) return 0;
    const char* body = http_response_body(http);
    if (body) {
        return http->response_len - (uint64_t)(body - http->response);
    }
    return http->response_len;
}

int http_fetch(const char* url, char* buffer, size_t max_len) {
    if (!buffer || max_len == 0) return -1;

    http_client_t* http = http_create();
    if (!http) return -1;

    int ret = http_get(http, url);
    if (ret == 0 && http->status_code == HTTP_OK) {
        const char* body = http_response_body(http);
        if (body) {
            uint64_t len = http_response_length(http);
            if (len > max_len - 1) len = max_len - 1;
            memcpy(buffer, body, (size_t)len);
            buffer[len] = '\0';
        }
    }

    int status = http->status_code;
    http_destroy(http);
    return (ret == 0 && status == HTTP_OK) ? 0 : -1;
}
