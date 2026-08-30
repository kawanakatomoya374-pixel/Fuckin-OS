/**
 * http.h - Simple HTTP Client
 */

#ifndef HTTP_H
#define HTTP_H

#include "types.h"
#include "net.h"
#include "tls_backend.h"

#define HTTP_PORT       80
#define HTTPS_PORT      443

#define HTTP_MAX_URL    2048
#define HTTP_MAX_HOST   256
#define HTTP_MAX_PATH   1024
/* Real HTML responses frequently carry several KiB of security and cache
 * headers before the first article text.  Keep enough wire data to preserve
 * meaningful page content for the NetSurf renderer. */
/* NetSurf's HTML/CSS/DOM conversion must be able to retain substantial,
 * standards-oriented documents rather than a 64KiB/512KiB rendering prefix.
 * This is a per-document hard safety limit, not an attachment-specific path.
 * http_client_t is heap allocated; 10MiB fits the kernel allocator's 64MiB
 * maximum block while covering common article, application-shell, and local
 * document payloads. */
#define HTTP_MAX_RESP   (10u * 1024u * 1024u)

#define HTTP_OK         200
#define HTTP_FOUND      302
#define HTTP_NOT_FOUND  404

typedef enum {
    HTTP_SCHEME_HTTP = 0,
    HTTP_SCHEME_HTTPS = 1,
} http_scheme_t;

typedef struct {
    socket_t* socket;
    tls_session_t* tls_session;
    ip_addr_t server_ip;
    uint64_t port;
    http_scheme_t scheme;
    
    char host[HTTP_MAX_HOST];
    char path[HTTP_MAX_PATH];
    
    int status_code;
    char response[HTTP_MAX_RESP];
    uint64_t response_len;
    /* Request-local I/O and content-decoding buffers. They prevent two
     * transport workers from sharing TLS plaintext staging or gzip/Brotli
     * output when HTTP parallelism is enabled. */
    char receive_chunk[16 * 1024];
    char *decode_workspace;
    
    int connected;
    /* Set per request by the HTTP dispatcher. This remains off for plaintext
     * and non-idempotent methods, so existing HTTP/1.1 behavior is unchanged. */
    int allow_http2;
    /* Set only after a complete response has traversed the nghttp2 transport;
     * validation and metrics must not confuse HTTP/1.1 fallback with h2. */
    int used_http2;
    /* Set only after a framed HTTP response confirms that the server did not
     * request Connection: close.  Redirects on the same origin may reuse this
     * transport and avoid a second DNS/TCP/TLS handshake. */
    int keep_alive;
} http_client_t;

http_client_t* http_create(void);
void http_destroy(http_client_t* http);

/* URL parsing */
int http_parse_url(const char* url, char* host, char* path, uint64_t* port);

/* HTTP methods */
int http_get(http_client_t* http, const char* url);
int http_post(http_client_t* http, const char* url, const char* data);
/* Snapshot bounded-transport semaphore state for runtime validation. */
void http_get_transport_stats(uint32_t* active, uint32_t* peak_active);


/* Response handling */
int http_status_code(http_client_t* http);
const char* http_response_body(http_client_t* http);
uint64_t http_response_length(http_client_t* http);
const char* http_get_header(http_client_t* http, const char* name);

/* Return the final URL after the client's internally followed redirect chain.
 * The caller supplies storage; no transport or allocation occurs here. */
int http_get_effective_url(const http_client_t* http, char* out, size_t out_sz);

/* Accept a Set-Cookie value received by an upper-layer fetcher.  The
 * process-wide jar is shared by short-lived HTTP client instances, while
 * the explicit client argument preserves the origin context when worker
 * transports overlap. */
void http_store_cookie_header_for(const http_client_t* http, const char* set_cookie);
/* Compatibility entry point. It has no origin context and intentionally
 * does not persist a cookie; callers must use the explicit form above. */
void http_store_cookie_header(const char* set_cookie);

/* Browser DOM cookie bridge. These helpers construct only the URL scope needed
 * by the bounded process-wide jar; unlike http_create(), they do not allocate a
 * 10 MiB response buffer or perform I/O. The supplied URL must be HTTP(S).
 * `set_cookie` accepts the normal document.cookie assignment form, including
 * Domain, Path, Secure and Max-Age attributes. */
int http_set_document_cookie_for_url(const char *url, const char *set_cookie);
size_t http_get_document_cookie_for_url(const char *url, char *out, size_t out_sz);

/* Internal transport hooks shared with the ALPN-gated HTTP/2 adapter. They
 * preserve the single bounded response/decompression/cookie implementation. */
int http_decode_response(http_client_t* http);
void http_store_cookies_from_response(http_client_t* http);
size_t http_append_cookie_header_for_transport(const http_client_t* http,
                                               char* out, size_t out_sz);

/* Simple API for fetching web pages */
int http_fetch(const char* url, char* buffer, size_t max_len);

#endif /* HTTP_H */
