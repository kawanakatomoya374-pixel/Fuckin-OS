#include "http2_client.h"

#include "tls_backend.h"
#include "memory.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "timer.h"
#include "../../third_party/nghttp2/lib/includes/nghttp2/nghttp2.h"

/* This is deliberately a small, bounded transition adapter.  It serves one
 * GET stream per freshly-negotiated h2 TLS connection, then emits GOAWAY and
 * lets the existing HTTP owner close the transport.  It does not claim h2
 * multiplexing or shared-connection reuse yet. */
#define COS_H2_MAX_HEADERS  (16u * 1024u)
#define COS_H2_RX_CHUNK     (16u * 1024u)

typedef struct {
    http_client_t *http;
    int32_t stream_id;
    size_t header_bytes;
    int headers_finished;
    int body_started;
    int complete;
    int failed;
    uint32_t stream_error;
} cos_h2_request_t;

extern void *kmalloc(size_t size);
extern void *krealloc(void *ptr, size_t size);
extern void kfree(void *ptr);

static void *cos_h2_malloc(size_t size, void *user_data) {
    (void)user_data;
    return kmalloc(size);
}

static void cos_h2_free(void *ptr, void *user_data) {
    (void)user_data;
    if (ptr) kfree(ptr);
}

static void *cos_h2_calloc(size_t nmemb, size_t size, void *user_data) {
    (void)user_data;
    if (size != 0 && nmemb > ((size_t)-1) / size) return NULL;
    size_t bytes = nmemb * size;
    void *ptr = kmalloc(bytes);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}

static void *cos_h2_realloc(void *ptr, size_t size, void *user_data) {
    (void)user_data;
    return krealloc(ptr, size);
}

static int cos_h2_append(cos_h2_request_t *request, const char *data, size_t len) {
    if (!request || !request->http || !data) return -1;
    http_client_t *http = request->http;
    if (http->response_len + len + 1u > sizeof(http->response)) {
        serial_puts("[HTTP/2] Response exceeds bounded HTTP buffer\n");
        request->failed = 1;
        return -1;
    }
    memcpy(http->response + http->response_len, data, len);
    http->response_len += (uint64_t)len;
    http->response[http->response_len] = '\0';
    return 0;
}

static int cos_h2_append_header(cos_h2_request_t *request,
                                const uint8_t *name, size_t namelen,
                                const uint8_t *value, size_t valuelen) {
    if (!request || request->body_started) return 0; /* Ignore trailers. */
    if (request->header_bytes + namelen + valuelen + 4u > COS_H2_MAX_HEADERS) {
        serial_puts("[HTTP/2] Header block exceeds bounded limit\n");
        request->failed = 1;
        return -1;
    }
    if (cos_h2_append(request, (const char *)name, namelen) < 0 ||
        cos_h2_append(request, ": ", 2) < 0 ||
        cos_h2_append(request, (const char *)value, valuelen) < 0 ||
        cos_h2_append(request, "\r\n", 2) < 0) {
        return -1;
    }
    request->header_bytes += namelen + valuelen + 4u;
    return 0;
}

static ssize_t cos_h2_send_callback(nghttp2_session *session,
                                    const uint8_t *data, size_t length,
                                    int flags, void *user_data) {
    (void)session;
    (void)flags;
    cos_h2_request_t *request = (cos_h2_request_t *)user_data;
    if (!request || !request->http || !request->http->tls_session) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    size_t sent = 0;
    while (sent < length) {
        int rc = tls_send(request->http->tls_session, (const char *)data + sent,
                          length - sent);
        if (rc <= 0) {
            request->failed = 1;
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        sent += (size_t)rc;
    }
    return (ssize_t)sent;
}

static int cos_h2_on_header(nghttp2_session *session, const nghttp2_frame *frame,
                            const uint8_t *name, size_t namelen,
                            const uint8_t *value, size_t valuelen,
                            uint8_t flags, void *user_data) {
    (void)session;
    (void)flags;
    cos_h2_request_t *request = (cos_h2_request_t *)user_data;
    if (!request || !frame || frame->hd.stream_id != request->stream_id) return 0;
    /* Only the initial response header block is serialized. Trailers are
     * intentionally not folded into the synthetic HTTP response. */
    if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE || request->body_started) return 0;

    if (namelen == 7 && memcmp(name, ":status", 7) == 0) {
        char status_line[32];
        size_t copied = valuelen < 3 ? valuelen : 3;
        memcpy(status_line, value, copied);
        status_line[copied] = '\0';
        request->http->status_code = atoi(status_line);
        /* The common decoder reparses the synthetic status line. Preserve the
         * h2 :status value there rather than allowing the first regular header
         * name to be interpreted as a decimal response code. */
        if (cos_h2_append(request, (const char *)value, valuelen) < 0 ||
            cos_h2_append(request, "\r\n", 2) < 0) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        request->header_bytes += valuelen + 2u;
        return 0;
    }
    if (cos_h2_append_header(request, name, namelen, value, valuelen) < 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int cos_h2_on_data(nghttp2_session *session, uint8_t flags,
                          int32_t stream_id, const uint8_t *data,
                          size_t len, void *user_data) {
    (void)session;
    (void)flags;
    cos_h2_request_t *request = (cos_h2_request_t *)user_data;
    if (!request || stream_id != request->stream_id) return 0;
    if (!request->headers_finished) {
        if (cos_h2_append(request, "\r\n", 2) < 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
        request->headers_finished = 1;
    }
    request->body_started = 1;
    if (cos_h2_append(request, (const char *)data, len) < 0) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

static int cos_h2_on_stream_close(nghttp2_session *session, int32_t stream_id,
                                  uint32_t error_code, void *user_data) {
    (void)session;
    cos_h2_request_t *request = (cos_h2_request_t *)user_data;
    if (!request || stream_id != request->stream_id) return 0;
    if (!request->headers_finished) {
        if (cos_h2_append(request, "\r\n", 2) < 0) return NGHTTP2_ERR_CALLBACK_FAILURE;
        request->headers_finished = 1;
    }
    request->stream_error = error_code;
    request->complete = 1;
    if (error_code != NGHTTP2_NO_ERROR) request->failed = 1;
    return 0;
}

static int cos_h2_submit_get(nghttp2_session *session, http_client_t *http,
                             cos_h2_request_t *request) {
    static const uint8_t method[] = ":method";
    static const uint8_t method_value[] = "GET";
    static const uint8_t scheme[] = ":scheme";
    static const uint8_t scheme_value[] = "https";
    static const uint8_t authority[] = ":authority";
    static const uint8_t path[] = ":path";
    static const uint8_t user_agent[] = "user-agent";
    static const uint8_t user_agent_value[] = "C-OS/4.0.8";
    static const uint8_t accept[] = "accept";
    static const uint8_t accept_value[] = "text/html,application/xhtml+xml,*/*;q=0.8";
    static const uint8_t accept_encoding[] = "accept-encoding";
    static const uint8_t accept_encoding_value[] = "br, gzip";
    nghttp2_nv headers[8];
    char authority_value[HTTP_MAX_HOST + 8];
    char cookie_value[1024];
    cookie_value[0] = '\0';
    http_append_cookie_header_for_transport(http, cookie_value, sizeof(cookie_value));
    if (http->port == 443) {
        snprintf(authority_value, sizeof(authority_value), "%s", http->host);
    } else {
        snprintf(authority_value, sizeof(authority_value), "%s:%u", http->host,
                 (unsigned)http->port);
    }

    headers[0].name = (uint8_t *)method;
    headers[0].value = (uint8_t *)method_value;
    headers[0].namelen = sizeof(method) - 1u;
    headers[0].valuelen = sizeof(method_value) - 1u;
    headers[0].flags = NGHTTP2_NV_FLAG_NONE;
    headers[1].name = (uint8_t *)scheme;
    headers[1].value = (uint8_t *)scheme_value;
    headers[1].namelen = sizeof(scheme) - 1u;
    headers[1].valuelen = sizeof(scheme_value) - 1u;
    headers[1].flags = NGHTTP2_NV_FLAG_NONE;
    headers[2].name = (uint8_t *)authority;
    headers[2].value = (uint8_t *)authority_value;
    headers[2].namelen = sizeof(authority) - 1u;
    headers[2].valuelen = strlen(authority_value);
    headers[2].flags = NGHTTP2_NV_FLAG_NONE;
    headers[3].name = (uint8_t *)path;
    headers[3].value = (uint8_t *)(http->path[0] ? http->path : "/");
    headers[3].namelen = sizeof(path) - 1u;
    headers[3].valuelen = strlen((const char *)headers[3].value);
    headers[3].flags = NGHTTP2_NV_FLAG_NONE;
    headers[4].name = (uint8_t *)user_agent;
    headers[4].value = (uint8_t *)user_agent_value;
    headers[4].namelen = sizeof(user_agent) - 1u;
    headers[4].valuelen = sizeof(user_agent_value) - 1u;
    headers[4].flags = NGHTTP2_NV_FLAG_NONE;
    headers[5].name = (uint8_t *)accept;
    headers[5].value = (uint8_t *)accept_value;
    headers[5].namelen = sizeof(accept) - 1u;
    headers[5].valuelen = sizeof(accept_value) - 1u;
    headers[5].flags = NGHTTP2_NV_FLAG_NONE;
    headers[6].name = (uint8_t *)accept_encoding;
    headers[6].value = (uint8_t *)accept_encoding_value;
    headers[6].namelen = sizeof(accept_encoding) - 1u;
    headers[6].valuelen = sizeof(accept_encoding_value) - 1u;
    headers[6].flags = NGHTTP2_NV_FLAG_NONE;

    size_t header_count = 7;
    if (cookie_value[0]) {
        static const uint8_t cookie[] = "cookie";
        headers[header_count].name = (uint8_t *)cookie;
        headers[header_count].value = (uint8_t *)cookie_value;
        headers[header_count].namelen = sizeof(cookie) - 1u;
        headers[header_count].valuelen = strlen(cookie_value);
        headers[header_count].flags = NGHTTP2_NV_FLAG_NONE;
        ++header_count;
    }

    request->stream_id = nghttp2_submit_request(session, NULL, headers, header_count,
                                                 NULL, request);
    return request->stream_id > 0 ? 0 : -1;
}

int http2_get_response(http_client_t *http) {
    if (!http || !http->tls_session) return -1;
    const char *alpn = tls_get_selected_protocol(http->tls_session);
    if (!alpn || strcmp(alpn, "h2") != 0) return -1;

    nghttp2_session_callbacks *callbacks = NULL;
    nghttp2_session *session = NULL;
    nghttp2_mem mem = { NULL, cos_h2_malloc, cos_h2_free,
                        cos_h2_calloc, cos_h2_realloc };
    cos_h2_request_t request;
    memset(&request, 0, sizeof(request));
    request.http = http;
    http->response_len = 0;
    http->response[0] = '\0';
    http->status_code = 0;
    http->keep_alive = 0;
    http->used_http2 = 0;
    /* Preserve one conventional status line so the established response body,
     * Content-Encoding decoder, cookie parser and NetSurf handoff need no h2
     * special case. The :status callback appends the three-digit code. */
    if (cos_h2_append(&request, "HTTP/2 ", 7) < 0) goto fail;

    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        serial_puts("[HTTP/2] setup failed: callbacks\n");
        goto fail;
    }
    nghttp2_session_callbacks_set_send_callback(callbacks, cos_h2_send_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, cos_h2_on_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, cos_h2_on_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, cos_h2_on_stream_close);
    int ng_rc = nghttp2_session_client_new3(&session, callbacks, &request, NULL, &mem);
    if (ng_rc != 0) { serial_puts("[HTTP/2] setup failed: session\n"); goto fail; }
    ng_rc = nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, NULL, 0);
    if (ng_rc != 0) { serial_puts("[HTTP/2] setup failed: settings\n"); goto fail; }
    if (cos_h2_submit_get(session, http, &request) < 0) {
        serial_puts("[HTTP/2] setup failed: GET headers\n");
        goto fail;
    }
    ng_rc = nghttp2_session_send(session);
    if (ng_rc != 0 || request.failed) {
        serial_puts("[HTTP/2] outbound preface/settings/request failed\n");
        goto fail;
    }

    uint8_t rx[COS_H2_RX_CHUNK];
    uint64_t deadline = get_timer_ticks() + 10000u;
    while (!request.complete && !request.failed && get_timer_ticks() < deadline) {
        int received = tls_recv(http->tls_session, (char *)rx, sizeof(rx));
        if (received < 0) {
            serial_puts("[HTTP/2] TLS receive failed\n");
            request.failed = 1;
            break;
        }
        if (received == 0) {
            thread_yield();
            continue;
        }
        size_t offset = 0;
        while (offset < (size_t)received && !request.failed) {
            ssize_t consumed = nghttp2_session_mem_recv(session, rx + offset,
                                                         (size_t)received - offset);
            if (consumed <= 0) {
                serial_puts("[HTTP/2] nghttp2 receive rejected peer frame\n");
                request.failed = 1;
                break;
            }
            offset += (size_t)consumed;
        }
        if (!request.failed && nghttp2_session_send(session) != 0) {
            serial_puts("[HTTP/2] SETTINGS ACK/control-frame send failed\n");
            request.failed = 1;
        }
    }

    if (!request.complete || request.failed || request.stream_error != NGHTTP2_NO_ERROR ||
        http->status_code <= 0) {
        serial_puts("[HTTP/2] response did not complete cleanly\n");
        goto fail;
    }

    (void)nghttp2_submit_goaway(session, NGHTTP2_FLAG_NONE,
                                nghttp2_session_get_last_proc_stream_id(session),
                                NGHTTP2_NO_ERROR, NULL, 0);
    (void)nghttp2_session_send(session);
    nghttp2_session_del(session);
    nghttp2_session_callbacks_del(callbacks);

    if (http_decode_response(http) < 0) {
        serial_puts("[HTTP/2] Content decode failed\n");
        return -1;
    }
    http_store_cookies_from_response(http);
    http->used_http2 = 1;
    serial_puts("[HTTP/2] Single-stream response received, status: ");
    serial_putdec(http->status_code);
    serial_puts(", length: ");
    serial_putdec(http->response_len);
    serial_puts("\n");
    return 0;

fail:
    serial_puts("[HTTP/2] h2 transaction failed\n");
    if (session) nghttp2_session_del(session);
    if (callbacks) nghttp2_session_callbacks_del(callbacks);
    return -1;
}
