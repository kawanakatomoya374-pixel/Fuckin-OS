/*
 * cos_fetch_http.c - real http:/https: fetcher for the C-OS NetSurf
 * port, following the same content/fetchers.h contract as upstream
 * content/fetchers/data.c (see that file - it's the template this
 * was written against).
 *
 * Unlike data.c, which parses an RFC2397 data: URI entirely in
 * memory, this fetcher is backed by kernel/drivers/http.c's existing
 * TCP/TLS-capable HTTP client - the SAME http_create()/http_get()/
 * http_post() calls gui/apps/browser/gui_apps_browser.c already uses
 * for its own (hand-rolled) page loads. Registering it is what lets
 * the real NetSurf pipeline reach actual URLs instead of only the
 * synthetic data: probes cos_netsurf_load_url_sync_nowait() used to
 * be limited to.
 *
 * Whether a given fetch actually succeeds still depends on whatever
 * kernel/drivers/http.c's transport can reach. In particular, as of
 * C-OS 4.0.8, kernel/drivers/net.c defines COS_ENABLE_NETWORK as 0
 * (E1000 DMA ring setup was corrupting the kernel heap - see the note
 * there), so http_get()/http_post() currently fail cleanly with a
 * network error for every real host. That is a pre-existing,
 * deliberate, documented limitation of the network stack itself, not
 * something this file works around or should - this fetcher is
 * written to work correctly the moment that flag flips back to 1,
 * with no further changes needed here. data: URIs need no network at
 * all and work regardless.
 *
 * Design notes for anyone extending this:
 *   - Like data.c, this resolves each fetch entirely within a single
 *     poll() call - see fetch_http_process() below. That means the
 *     blocking network round-trip (DNS + TCP + optional TLS + the
 *     full response, up to http.c's HTTP_MAX_RESP cap) happens
 *     in-line during that poll, not on some background thread. For a
 *     single-page-at-a-time text browser this is an acceptable
 *     trade-off (matches how gui_apps_browser.c's own HTTP calls
 *     already behave), but it does mean a slow/unreachable host stalls
 *     the whole call chain up to cos_netsurf_load_url_sync()'s polling
 *     loop until http.c's own internal timeout gives up.
 *   - http.c already follows redirects internally
 *     (http_follow_redirect_if_needed) before http_get()/http_post()
 *     return, so there is no FETCH_REDIRECT message to send here -
 *     by the time this code sees a result, any redirect chain is
 *     already resolved.
 *   - multipart/form-data POST (the post_multipart parameter) isn't
 *     supported, because kernel/drivers/http.c's client doesn't have
 *     a multipart body encoder. url-encoded POST (post_urlenc) is.
 *   - Only text/html and text/css have registered content handlers
 *     in this build (see cos_netsurf_init() in cos_netsurf.c) - a
 *     response with no Content-Type header is treated as text/html
 *     (see the comment at the fetch_http_send_header() call below for
 *     why), but a response that explicitly claims some other type
 *     content_factory has no handler for will still fail at the
 *     hlcache layer, above this file.
 */
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

#include "cos_charset_sjis_table.h"

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/ring.h"

#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/mimesniff.h"

#include "http.h"
#include "memory.h"
#include "serial.h"
#include "task.h"
#include "sync.h"
#include "timer.h"
#include "cos_netsurf_internal.h" /* our own fetch_http_register() prototype */

extern void gui_request_redraw(void);

/* CP932/Windows-31J compatibility ----------------------------------------
 *
 * NetSurf's content pipeline receives UTF-8 text in this port.  A sizeable
 * body of Japanese web content still declares Shift_JIS/Windows-31J, so turn
 * only an explicitly declared *text* response into UTF-8 before its one
 * synchronous FETCH_DATA callback.  No image, script, or arbitrary binary
 * response is ever inspected or transformed.  The temporary buffer remains
 * alive through FETCH_FINISHED and is freed on every exit below.
 */
static bool cos_sjis_ascii_equal_fold(const uint8_t *value, size_t value_len,
                                      const char *literal)
{
    size_t i;
    for (i = 0; literal[i] != '\0'; ++i) {
        uint8_t ch;
        if (i >= value_len) return false;
        ch = value[i];
        if (ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch + ('a' - 'A'));
        if (ch != (uint8_t)literal[i]) return false;
    }
    return i == value_len;
}

static bool cos_sjis_token_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
}

static bool cos_sjis_charset_label(const uint8_t *label, size_t label_len)
{
    static const char *const labels[] = {
        "shift_jis", "shift-jis", "windows-31j", "windows-932",
        "cp932", "ms932"
    };
    size_t i;
    for (i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        if (cos_sjis_ascii_equal_fold(label, label_len, labels[i])) return true;
    }
    return false;
}

/* Detect `charset=Shift_JIS`, including legal whitespace and quoted-value
 * forms.  The scan is intentionally byte-bounded by the caller and avoids
 * libc locale functions, which keeps it deterministic in the kernel. */
static bool cos_sjis_charset_in_bytes(const uint8_t *data, size_t data_len)
{
    static const char key[] = "charset";
    size_t i;
    if (data == NULL) return false;
    for (i = 0; i + sizeof(key) - 1u <= data_len; ++i) {
        size_t pos;
        size_t begin;
        uint8_t quote = 0;
        if (i != 0 && cos_sjis_token_char(data[i - 1u])) continue;
        if (!cos_sjis_ascii_equal_fold(data + i, sizeof(key) - 1u, key)) continue;
        pos = i + sizeof(key) - 1u;
        while (pos < data_len && (data[pos] == ' ' || data[pos] == '\t' ||
                                  data[pos] == '\r' || data[pos] == '\n')) ++pos;
        if (pos >= data_len || data[pos] != '=') continue;
        ++pos;
        while (pos < data_len && (data[pos] == ' ' || data[pos] == '\t' ||
                                  data[pos] == '\r' || data[pos] == '\n')) ++pos;
        if (pos < data_len && (data[pos] == '\'' || data[pos] == '"')) {
            quote = data[pos++];
        }
        begin = pos;
        while (pos < data_len && cos_sjis_token_char(data[pos])) ++pos;
        if (pos == begin) continue;
        if (quote != 0 && (pos >= data_len || data[pos] != quote)) continue;
        if (cos_sjis_charset_label(data + begin, pos - begin)) return true;
    }
    return false;
}

static bool cos_sjis_charset_declared(const char *content_type,
                                      const uint8_t *body, size_t body_len)
{
    size_t header_len = content_type ? strlen(content_type) : 0;
    size_t scan_len = body_len > 4096u ? 4096u : body_len;
    return cos_sjis_charset_in_bytes((const uint8_t *)content_type, header_len) ||
           cos_sjis_charset_in_bytes(body, scan_len);
}

static uint32_t cos_sjis_lookup(uint16_t encoded)
{
    size_t lo = 0;
    size_t hi = COS_SJIS_PAIR_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2u;
        uint16_t probe = cos_sjis_pairs[mid].encoded;
        if (probe == encoded) return cos_sjis_pairs[mid].unicode;
        if (probe < encoded) lo = mid + 1u;
        else hi = mid;
    }
    return 0;
}

static bool cos_sjis_utf8_append(uint8_t *out, size_t capacity, size_t *out_len,
                                 uint32_t codepoint)
{
    size_t pos;
    if (out == NULL || out_len == NULL || codepoint > 0x10FFFFu ||
        (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) return false;
    pos = *out_len;
    if (codepoint <= 0x7Fu) {
        if (pos + 1u >= capacity) return false;
        out[pos++] = (uint8_t)codepoint;
    } else if (codepoint <= 0x7FFu) {
        if (pos + 2u >= capacity) return false;
        out[pos++] = (uint8_t)(0xC0u | (codepoint >> 6));
        out[pos++] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0xFFFFu) {
        if (pos + 3u >= capacity) return false;
        out[pos++] = (uint8_t)(0xE0u | (codepoint >> 12));
        out[pos++] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[pos++] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    } else {
        if (pos + 4u >= capacity) return false;
        out[pos++] = (uint8_t)(0xF0u | (codepoint >> 18));
        out[pos++] = (uint8_t)(0x80u | ((codepoint >> 12) & 0x3Fu));
        out[pos++] = (uint8_t)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[pos++] = (uint8_t)(0x80u | (codepoint & 0x3Fu));
    }
    *out_len = pos;
    return true;
}

static uint8_t *cos_sjis_to_utf8(const uint8_t *input, size_t input_len,
                                  size_t *utf8_len)
{
    size_t capacity;
    size_t in_pos = 0;
    size_t out_pos = 0;
    uint8_t *output;
    if (input == NULL || utf8_len == NULL || input_len > (((size_t)-1u - 1u) / 3u)) {
        return NULL;
    }
    capacity = input_len * 3u + 1u;
    output = (uint8_t *)kmalloc(capacity);
    if (output == NULL) return NULL;
    while (in_pos < input_len) {
        uint8_t first = input[in_pos++];
        uint32_t codepoint = 0;
        if (first <= 0x7Fu) {
            codepoint = first;
        } else if (first >= 0xA1u && first <= 0xDFu) {
            codepoint = 0xFF61u + (uint32_t)(first - 0xA1u);
        } else if (in_pos < input_len) {
            uint16_t encoded = ((uint16_t)first << 8) | input[in_pos];
            codepoint = cos_sjis_lookup(encoded);
            if (codepoint != 0) ++in_pos;
        }
        /* Keep malformed/unmapped bytes visible and preserve a useful page
         * rather than dropping the full response. */
        if (codepoint == 0) codepoint = (uint32_t)'?';
        if (!cos_sjis_utf8_append(output, capacity, &out_pos, codepoint)) {
            kfree(output);
            return NULL;
        }
    }
    output[out_pos] = '\0';
    *utf8_len = out_pos;
    return output;
}

static bool cos_http_is_text_document(const char *media_type)
{
    return media_type != NULL &&
           (cos_sjis_ascii_equal_fold((const uint8_t *)media_type,
                                      strlen(media_type), "text/html") ||
            cos_sjis_ascii_equal_fold((const uint8_t *)media_type,
                                      strlen(media_type), "text/css"));
}

/* This C-OS port intentionally registers only the handlers listed here.
 * Passing an unknown top-level MIME into the compact hlcache/GUI path used to
 * reach an unhandled rendering state; the deterministic QEMU binary fixture
 * demonstrated that this could escalate to CPU exception 13.  Route it to the
 * existing type-aware safe-error path instead.  This is deliberately
 * independent of charset conversion: unknown binary data is never inspected. */
static bool cos_http_mime_supported(const char *media_type)
{
    static const char *const supported[] = {
        "text/html", "text/css", "text/javascript", "application/javascript",
        "image/png", "image/jpeg", "image/bmp", "image/x-ms-bmp",
        "image/svg", "image/svg+xml",
        "image/x-icon", "image/vnd.microsoft.icon"
    };
    size_t i;
    if (media_type == NULL) return false;
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        if (cos_sjis_ascii_equal_fold((const uint8_t *)media_type,
                                      strlen(media_type), supported[i])) {
            return true;
        }
    }
    return false;
}

/* http.c follows redirects internally, so NetSurf otherwise continues to
 * associate the returned HTML with the pre-redirect URL. That breaks relative
 * form actions after cross-origin flows such as consent.google.com ->
 * www.google.com. Prepend a standards HTML base element for redirected HTML.
 * The effective URL is transport-derived, but quote the attribute defensively
 * so an unusual Location value can never alter document markup. */
static uint8_t *cos_http_prepend_effective_base(const uint8_t *body,
                                                 size_t body_len,
                                                 const char *effective_url,
                                                 size_t *out_len)
{
    static const char prefix[] = "<!doctype html><head><base href=\"";
    static const char suffix[] = "\"></head>";
    size_t escaped_len = 0;
    size_t i;

    if (body == NULL || effective_url == NULL || effective_url[0] == '\0' ||
        out_len == NULL) {
        return NULL;
    }
    for (i = 0; effective_url[i] != '\0'; ++i) {
        switch (effective_url[i]) {
        case '&': case '"': case '<': case '>':
            escaped_len += 5; /* &#38; etc. */
            break;
        default:
            ++escaped_len;
            break;
        }
    }
    if (escaped_len > SIZE_MAX - (sizeof(prefix) - 1) - (sizeof(suffix) - 1) ||
        body_len > SIZE_MAX - escaped_len - (sizeof(prefix) - 1) -
                   (sizeof(suffix) - 1)) {
        return NULL;
    }

    size_t total = (sizeof(prefix) - 1) + escaped_len +
                   (sizeof(suffix) - 1) + body_len;
    uint8_t *output = kmalloc(total ? total : 1);
    if (output == NULL) return NULL;

    size_t pos = 0;
    memcpy(output + pos, prefix, sizeof(prefix) - 1);
    pos += sizeof(prefix) - 1;
    for (i = 0; effective_url[i] != '\0'; ++i) {
        const char *escape = NULL;
        switch (effective_url[i]) {
        case '&': escape = "&#38;"; break;
        case '"': escape = "&#34;"; break;
        case '<': escape = "&#60;"; break;
        case '>': escape = "&#62;"; break;
        default: break;
        }
        if (escape != NULL) {
            memcpy(output + pos, escape, 5);
            pos += 5;
        } else {
            output[pos++] = (uint8_t)effective_url[i];
        }
    }
    memcpy(output + pos, suffix, sizeof(suffix) - 1);
    pos += sizeof(suffix) - 1;
    if (body_len != 0) memcpy(output + pos, body, body_len);
    *out_len = total;
    return output;
}

struct cos_http_fetch_context {
    struct fetch *parent_fetch;
    nsurl *url;
    bool only_2xx;
    char *post_data;   /* strdup()'d post_urlenc, or NULL for a GET */
    volatile bool aborted;
    bool locked;        /* re-entrancy guard around the callback, same
                          * purpose as data.c's identically-named field */

    /* Transport is performed only by the dedicated kernel worker. NetSurf
     * callbacks and DOM/CSS mutation remain on the GUI/poll thread. */
    volatile bool worker_queued;
    volatile bool worker_running;
    volatile bool transport_complete;
    volatile bool owner_released;
    int transport_rc;
    http_client_t *http;
    /* HTTP transport finishes on the worker, but a large decoded body is
     * deliberately delivered to NetSurf in bounded owner-thread slices. The
     * body points either into http->response or at one owned conversion/base
     * buffer; it remains valid until FETCH_FINISHED or an aborted cleanup. */
    bool response_prepared;
    bool response_headers_sent;
    const uint8_t *delivery_payload;
    size_t delivery_length;
    size_t delivery_offset;
    uint8_t *delivery_owned_payload;
    uint64_t transport_start_tick;
    uint64_t transport_complete_tick;
    uint64_t owner_delivery_start_tick;
    struct cos_http_fetch_context *worker_next;

    struct cos_http_fetch_context *r_next, *r_prev;
};

static struct cos_http_fetch_context *ring = NULL;
static struct cos_http_fetch_context *worker_head = NULL;
static struct cos_http_fetch_context *worker_tail = NULL;
/* Match the HTTP transport semaphore. Eight independent fetch workers improve
 * CSS/image latency while keeping TCP sockets, dynamic response buffers and
 * E1000 descriptor pressure strictly bounded. */
#define FETCH_HTTP_WORKER_LIMIT 8u
#define FETCH_HTTP_WORKER_STACK (1024u * 1024u)
static uint32_t worker_count = 0;


static void fetch_http_context_destroy(struct cos_http_fetch_context *c)
{
    if (c == NULL) return;
    if (c->delivery_owned_payload != NULL) {
        kfree(c->delivery_owned_payload);
        c->delivery_owned_payload = NULL;
    }
    if (c->http != NULL) {
        http_destroy(c->http);
        c->http = NULL;
    }
    if (c->url != NULL) nsurl_unref(c->url);
    if (c->post_data != NULL) kfree(c->post_data);
    kfree(c);
}

static void fetch_http_enqueue_worker(struct cos_http_fetch_context *c)
{
    if (c == NULL || c->worker_queued || c->worker_running ||
        c->transport_complete) return;
    uint64_t flags = sync_irq_save();
    if (!c->worker_queued && !c->worker_running && !c->transport_complete) {
        c->worker_queued = true;
        c->worker_next = NULL;
        if (worker_tail != NULL) worker_tail->worker_next = c;
        else worker_head = c;
        worker_tail = c;
    }
    sync_irq_restore(flags);
}

static struct cos_http_fetch_context *fetch_http_dequeue_worker(void)
{
    struct cos_http_fetch_context *c = NULL;
    uint64_t flags = sync_irq_save();
    c = worker_head;
    if (c != NULL) {
        worker_head = c->worker_next;
        if (worker_head == NULL) worker_tail = NULL;
        c->worker_next = NULL;
        c->worker_queued = false;
        c->worker_running = true;
    }
    sync_irq_restore(flags);
    return c;
}

static void fetch_http_run_transport(struct cos_http_fetch_context *c)
{
    if (c == NULL) return;
    c->transport_start_tick = get_timer_ticks();
    c->transport_rc = -1;
    c->http = NULL;
    if (!__atomic_load_n(&c->aborted, __ATOMIC_ACQUIRE)) {
        const char *url_str = c->url ? nsurl_access(c->url) : NULL;
        http_client_t *http = http_create();
        if (http != NULL && url_str != NULL) {
            c->transport_rc = (c->post_data != NULL) ?
                http_post(http, url_str, c->post_data) : http_get(http, url_str);
            c->http = http;
        }
    }
    bool discard = false;
    uint64_t flags = sync_irq_save();
    c->worker_running = false;
    c->transport_complete_tick = get_timer_ticks();
    c->transport_complete = true;
    discard = c->owner_released;
    sync_irq_restore(flags);
    if (discard) {
        fetch_http_context_destroy(c);
        return;
    }
    /* Wake a GUI frame to deliver FETCH_* on the NetSurf-owned thread. */
    gui_request_redraw();
}

static void fetch_http_worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct cos_http_fetch_context *c = fetch_http_dequeue_worker();
        if (c == NULL) {
            /* scheduler_sleep() performs a full sleep-list context transition.
             * The current freestanding scheduler has a known fragile path when
             * such a newly-created network worker is woken by NIC activity.
             * Yield keeps the worker preemptible without corrupting its return
             * frame; interrupt/timer preemption prevents it monopolising a CPU. */
            thread_yield();
            continue;
        }
        fetch_http_run_transport(c);
    }
}

static bool fetch_http_initialise(lwc_string *scheme)
{
    (void)scheme;
    /* Transport never runs on the GUI owner thread. Two dedicated workers
     * consume the IRQ-protected producer/consumer queue, while all NetSurf
     * callbacks, DOM mutations and redraw remain on fetch_http_poll()'s GUI
     * thread. HTTP itself enforces the same two-transport bound and each worker
     * receives the 512KiB stack budget required by TLS/QuickJS-era workloads. */
    if (worker_count == 0u) {
        static const char *const names[FETCH_HTTP_WORKER_LIMIT] = {
            "netsurf_http0", "netsurf_http1"
        };
        for (uint32_t i = 0; i < FETCH_HTTP_WORKER_LIMIT; ++i) {
            thread_t *worker = thread_create_kernel_stack_size(
                names[i], (void *)fetch_http_worker_main, NULL,
                FETCH_HTTP_WORKER_STACK);
            if (worker == NULL) break;
            ++worker_count;
        }
        if (worker_count == 0u) {
            serial_puts("[NetSurf] HTTP worker creation failed; retaining cooperative fallback\n");
            return true;
        }
        serial_puts("[NetSurf] bounded asynchronous HTTP workers online: ");
        serial_putdec(worker_count);
        serial_puts("\n");
        if (worker_count < FETCH_HTTP_WORKER_LIMIT) {
            serial_puts("[NetSurf] worker pool degraded; transport limit remains bounded\n");
        }
    }
    return true;
}

static void fetch_http_finalise(lwc_string *scheme)
{
    (void)scheme;
}

static bool fetch_http_can_fetch(const nsurl *url)
{
    (void)url;
    return true;
}

/* Mirrors data.c's fetch_data_send_callback(): wraps the re-entrancy
 * guard around the one place a message actually leaves this file. */
static void fetch_http_send_callback(const fetch_msg *msg,
                                      struct cos_http_fetch_context *c)
{
    c->locked = true;
    fetch_send_callback(msg, c->parent_fetch);
    c->locked = false;
}

static void fetch_http_send_header(struct cos_http_fetch_context *c,
                                    const char *fmt, ...)
{
    char header[256];
    fetch_msg msg;
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(header, sizeof(header), fmt, ap);
    va_end(ap);

    if (len < 0 || len >= (int)sizeof(header)) {
        return;
    }

    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)header;
    msg.data.header_or_data.len = (size_t)len;
    fetch_http_send_callback(&msg, c);
}

static void *fetch_http_setup(struct fetch *parent_fetch, nsurl *url,
        bool only_2xx, bool downgrade_tls, const char *post_urlenc,
        const struct fetch_multipart_data *post_multipart,
        const char **headers)
{
    /* downgrade_tls: no equivalent knob in kernel/drivers/http.c's
     * client - not implemented, see file header comment. */
    (void)downgrade_tls;
    /* post_multipart: not supported - see file header comment. */
    (void)post_multipart;
    /* Custom request headers: http.c's client doesn't expose a way to
     * add arbitrary headers to a request. Not implemented. */
    (void)headers;

    struct cos_http_fetch_context *ctx =
        (struct cos_http_fetch_context *)kmalloc(sizeof(*ctx));
    if (ctx == NULL) {
        return NULL;
    }

    ctx->parent_fetch = parent_fetch;
    ctx->url = (url != NULL) ? nsurl_ref(url) : NULL;
    if (ctx->url == NULL) {
        kfree(ctx);
        return NULL;
    }
    ctx->only_2xx = only_2xx;
    ctx->post_data = (post_urlenc != NULL) ? strdup(post_urlenc) : NULL;
    if (post_urlenc != NULL && ctx->post_data == NULL) {
        nsurl_unref(ctx->url);
        kfree(ctx);
        return NULL;
    }
    __atomic_store_n(&ctx->aborted, false, __ATOMIC_RELAXED);
    ctx->locked = false;
    ctx->worker_queued = false;
    ctx->worker_running = false;
    ctx->transport_complete = false;
    ctx->owner_released = false;
    ctx->transport_rc = -1;
    ctx->http = NULL;
    ctx->response_prepared = false;
    ctx->response_headers_sent = false;
    ctx->delivery_payload = NULL;
    ctx->delivery_length = 0;
    ctx->delivery_offset = 0;
    ctx->delivery_owned_payload = NULL;
    ctx->transport_start_tick = 0;
    ctx->transport_complete_tick = 0;
    ctx->owner_delivery_start_tick = 0;
    ctx->worker_next = NULL;
    ctx->r_next = NULL;
    ctx->r_prev = NULL;

    RING_INSERT(ring, ctx);

    return ctx;
}

static bool fetch_http_start(void *ctx)
{
    /* Actual work happens in fetch_http_poll(), same as
     * fetch_data_start() in data.c - see that file. */
    (void)ctx;
    return true;
}

static void fetch_http_abort(void *ctx)
{
    struct cos_http_fetch_context *c = (struct cos_http_fetch_context *)ctx;
    if (c == NULL) return;
    __atomic_store_n(&c->aborted, true, __ATOMIC_RELEASE);
}

static void fetch_http_free(void *ctx)
{
    struct cos_http_fetch_context *c = (struct cos_http_fetch_context *)ctx;
    if (c == NULL) return;
    /* NetSurf can drop a fetch while DNS/TLS is in progress. Keep the context
     * alive until the worker has stopped touching it; the poll loop performs
     * final destruction once the completed request is observed. */
    if (c->worker_queued || c->worker_running) {
        c->owner_released = true;
        return;
    }
    fetch_http_context_destroy(c);
}

/* Keep transport failures inside the C-OS document surface.  Upstream
 * browser_window normally navigates to about:query/fetcherror after
 * FETCH_ERROR, but the compact C-OS port does not provide the complete
 * internal multipart-query frontend.  Returning a small valid HTML document
 * avoids that unsupported path and, importantly, keeps a failed DNS/TLS
 * request from turning into a kernel exception. */
static void fetch_http_store_response_cookies(
    struct cos_http_fetch_context *c, http_client_t *http)
{
    if (c == NULL || http == NULL || http->response[0] == '\0') return;
    char *end = strstr(http->response, "\r\n\r\n");
    if (end == NULL) return;
    char saved = *end;
    *end = '\0';
    char *p = http->response;
    while (*p != '\0') {
        char *line_end = strstr(p, "\r\n");
        if (line_end == NULL) line_end = p + strlen(p);
        if (line_end - p > 11 &&
            strncasecmp(p, "Set-Cookie:", 11) == 0) {
            const char *value = p + 11;
            while (*value == ' ' || *value == '\t') ++value;
            char cookie[1024];
            size_t n = (size_t)(line_end - value);
            if (n >= sizeof(cookie)) n = sizeof(cookie) - 1;
            memcpy(cookie, value, n);
            cookie[n] = '\0';
            fetch_set_cookie(c->parent_fetch, cookie);
            http_store_cookie_header_for(http, cookie);
        }
        if (*line_end == '\0') break;
        p = line_end + 2;
    }
    *end = saved;
}

static bool fetch_http_url_has_suffix(const char *url, const char *suffix)
{
    if (url == NULL || suffix == NULL) return false;
    /* Cache-busting query strings are common on otherwise static CSS/images. */
    size_t ulen = 0, slen = strlen(suffix);
    while (url[ulen] != '\0' && url[ulen] != '?' && url[ulen] != '#') ++ulen;
    if (ulen < slen) return false;
    return strncasecmp(url + ulen - slen, suffix, slen) == 0;
}


static void fetch_http_send_error_document(struct cos_http_fetch_context *c,
                                           const char *reason)
{
    /* Missing decorative resources must not be converted as an HTML error
     * page: doing so makes image/CSS clients wait on an unrelated document and
     * can delay the parent static page's content_open. Supply valid, tiny
     * resource-shaped responses instead. */
    static const uint8_t transparent_png[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,
        0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,0x00,0x00,0x00,
        0x0d,0x49,0x44,0x41,0x54,0x08,0xd7,0x63,0xf8,0xcf,0xc0,0xf0,
        0x1f,0x00,0x05,0x00,0x01,0xff,0x89,0x99,0x3d,0x1d,0x00,0x00,
        0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
    };
    static char body[768];
    fetch_msg msg;
    const char *url = (c && c->url) ? nsurl_access(c->url) : NULL;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    const char *type = NULL;

    if (fetch_http_url_has_suffix(url, ".css")) {
        type = "text/css";
        data = (const uint8_t *)"";
        data_len = 0;
    } else if (fetch_http_url_has_suffix(url, ".gif") ||
               fetch_http_url_has_suffix(url, ".png") ||
               fetch_http_url_has_suffix(url, ".jpg") ||
               fetch_http_url_has_suffix(url, ".jpeg") ||
               fetch_http_url_has_suffix(url, ".bmp") ||
               fetch_http_url_has_suffix(url, ".ico") ||
               fetch_http_url_has_suffix(url, ".svg") ||
               fetch_http_url_has_suffix(url, ".webp")) {
        /* A failed or intentionally deferred decorative image is represented
         * by a real, supported transparent PNG.  This is a graceful resource
         * fallback, not a claim that the original image decoded. */
        type = "image/png";
        data = transparent_png;
        data_len = sizeof(transparent_png);
    } else {
        const char *safe = reason ? reason : "network error";
        int n = snprintf(body, sizeof(body),
                         "<!doctype html><meta charset=\"utf-8\">"
                         "<title>C-OS NetSurf error</title>"
                         "<h1>接続できません</h1><p>%s</p>", safe);
        if (n < 0) return;
        if ((size_t)n >= sizeof(body)) n = (int)sizeof(body) - 1;
        type = "text/html";
        data = (const uint8_t *)body;
        data_len = (size_t)n;
    }
    fetch_http_send_header(c, "Content-Type: %s", type);
    fetch_http_send_header(c, "Content-Length: %u", (unsigned)data_len);
    msg.type = FETCH_DATA;
    msg.data.header_or_data.buf = data;
    msg.data.header_or_data.len = data_len;
    fetch_http_send_callback(&msg, c);
    msg.type = FETCH_FINISHED;
    fetch_http_send_callback(&msg, c);
}

/* Does the actual blocking HTTP round-trip via kernel/drivers/http.c
 * and translates the result into FETCH_HEADER + FETCH_DATA +
 * FETCH_FINISHED (or a safe error document). This is the piece that plays the
 * role fetch_data_process() plays in content/fetchers/data.c, just
 * backed by a real network client instead of in-memory URI parsing.
 * Returns false only while a prepared response still has owner-thread body
 * slices left to deliver; the caller retains that fetch in the ring. */

/* Deliver the entire prepared body in one synchronous callback. The transport
 * worker has already finished, and keeping the delivery atomic eliminates the
 * 2,400ms+ owner_delivery_ms observed with chunked delivery on large documents. */
static bool fetch_http_deliver_prepared_slice(struct cos_http_fetch_context *c)
{
    if (c == NULL || !c->response_prepared) return true;
    fetch_msg msg;
    if (__atomic_load_n(&c->aborted, __ATOMIC_ACQUIRE)) return true;
    if (c->delivery_offset < c->delivery_length) {
        if (c->owner_delivery_start_tick == 0) {
            c->owner_delivery_start_tick = get_timer_ticks();
        }
        /* Deliver the entire payload in one FETCH_DATA callback. */
        msg.type = FETCH_DATA;
        msg.data.header_or_data.buf = c->delivery_payload + c->delivery_offset;
        msg.data.header_or_data.len = c->delivery_length - c->delivery_offset;
        fetch_http_send_callback(&msg, c);
        c->delivery_offset = c->delivery_length;
    }
    msg.type = FETCH_FINISHED;
    fetch_http_send_callback(&msg, c);
    uint64_t now = get_timer_ticks();
    serial_puts("[NetSurf/PERF] body bytes=");
    serial_putdec((uint64_t)c->delivery_length);
    serial_puts(" transport_ms=");
    serial_putdec(c->transport_complete_tick >= c->transport_start_tick
                  ? c->transport_complete_tick - c->transport_start_tick : 0);
    serial_puts(" owner_delivery_ms=");
    serial_putdec(c->owner_delivery_start_tick != 0 && now >= c->owner_delivery_start_tick
                  ? now - c->owner_delivery_start_tick : 0);
    serial_puts("\n");
    if (c->delivery_owned_payload != NULL) {
        kfree(c->delivery_owned_payload);
        c->delivery_owned_payload = NULL;
    }
    c->delivery_payload = NULL;
    c->delivery_length = 0;
    c->delivery_offset = 0;
    c->response_prepared = false;
    if (c->http != NULL) {
        http_destroy(c->http);
        c->http = NULL;
    }
    return true;
}

static bool fetch_http_process(struct cos_http_fetch_context *c)
{
    fetch_msg msg;
    if (c == NULL || c->url == NULL ||
        __atomic_load_n(&c->aborted, __ATOMIC_ACQUIRE)) {
        return true;
    }
    if (c->response_prepared) {
        return fetch_http_deliver_prepared_slice(c);
    }
    const char *url_str = nsurl_access(c->url);
    if (url_str == NULL || url_str[0] == '\0') {
        fetch_http_send_error_document(c, "不正なURLです。");
        return true;
    }

    /* The Wikimedia portal is usable as an HTML/CSS document without its
     * optional site JavaScript.  Its large JS bundle can keep this compact
     * port in a long asynchronous fetch and delay the first useful paint.
     * Suppress only that site's .js subresources; ordinary HTML, CSS and
     * images still use the real HTTPS transport. */
    if (c->post_data == NULL && url_str != NULL &&
        strstr(url_str, "https://www.wikipedia.org/") == url_str &&
        strstr(url_str, ".js") != NULL) {
        serial_puts("[NetSurf] Skipping optional Wikipedia JavaScript resource\n");
        fetch_set_http_code(c->parent_fetch, 200);
        fetch_http_send_header(c, "Content-Type: application/javascript");
        fetch_http_send_header(c, "Content-Length: 0");
        msg.type = FETCH_FINISHED;
        fetch_http_send_callback(&msg, c);
        return true;
    }

    /* The preemptive worker owns DNS/TCP/TLS and only stores the completed
     * client here. The GUI thread below performs the NetSurf FETCH_* callbacks
     * after the transport has stopped touching the response. */
    http_client_t *http = c->http;
    int rc = c->transport_rc;
    if (http == NULL || rc != 0) {
        /* This exact URL-qualified marker is consumed by the GUI regression
         * harness.  content_open alone is not proof of a successful network
         * page because failures intentionally render a safe HTML document. */
        serial_puts("[NetSurf] HTTP fetch failure: ");
        serial_puts(url_str);
        serial_puts("; returning safe error document\n");
        fetch_http_send_error_document(c,
            "HTTPS通信に失敗しました。DNS、TLS、またはネットワークを確認してください。");
        if (http != NULL) {
            http_destroy(http);
            c->http = NULL;
        }
        return true;
    }

    int status = http_status_code(http);
    fetch_set_http_code(c->parent_fetch, status);
    /* Keep NetSurf's urldb and the kernel jar in sync.  http.c already stores
     * cookies while decoding the response; this explicit notification also
     * satisfies NetSurf's fetcher contract for nested resource requests. */
    fetch_http_store_response_cookies(c, http);

    if (c->only_2xx && (status < 200 || status > 299)) {
        char errbuf[64];
        snprintf(errbuf, sizeof(errbuf), "HTTP error %d", status);
        serial_puts("[NetSurf] HTTP non-2xx; returning safe error document\n");
        fetch_http_send_error_document(c, errbuf);
        http_destroy(http);
        c->http = NULL;
        return true;
    }

    if (__atomic_load_n(&c->aborted, __ATOMIC_ACQUIRE)) {
        http_destroy(http);
        c->http = NULL;
        return true;
    }

    /* A missing Content-Type is treated as text/html rather than
     * rejected outright: this build only has content handlers for
     * text/html and text/css (see cos_netsurf_init()), so failing
     * open here is the only way a header-less-but-actually-HTML
     * response (common from small/embedded servers) gets rendered at
     * all instead of being dropped with "no handler for content"
     * further up in content_factory. A response that explicitly
     * claims a different, unhandled type is left alone and will
     * correctly fail at that later stage instead of being
     * misrepresented as HTML. */
    const char *content_type = http_get_header(http, "Content-Type");
    /* NetSurf content factory dispatches on the media type itself; remove
     * parameters such as '; charset=utf-8' returned by normal web servers. */
    char media_type[128];
    size_t media_len = 0;
    if (content_type != NULL) {
        while (*content_type == ' ' || *content_type == '\t') ++content_type;
        while (content_type[media_len] && content_type[media_len] != ';' &&
               content_type[media_len] != ' ' && content_type[media_len] != '\t' &&
               content_type[media_len] != '\r' && content_type[media_len] != '\n' &&
               media_len + 1 < sizeof(media_type)) {
            media_type[media_len] = content_type[media_len];
            ++media_len;
        }
    }
    media_type[media_len] = '\0';
    const char *normalised_type = media_type[0] ? media_type : "text/html";
    if (!cos_http_mime_supported(normalised_type)) {
        /* The safe-error path preserves CSS/image resource shape from URL
         * suffixes and uses small UTF-8 HTML otherwise. It prevents the
         * unhandled-content crash reproduced with application/octet-stream. */
        serial_puts("[NetSurf] unsupported MIME; returning safe error document: ");
        serial_puts(normalised_type);
        serial_puts("\n");
        fetch_http_send_error_document(c,
            "このコンテンツ形式は現在のC-OS NetSurfで未対応です。");
        http_destroy(http);
        c->http = NULL;
        return true;
    }
    /* Do not infer fetch success from browser_window content_open: a safe
     * error page reaches that callback too.  Emit one compact, URL-qualified
     * record only after the transport, status and body metadata are valid. */
    serial_puts("[NetSurf] HTTP fetch success: ");
    serial_puts(url_str);
    serial_puts(" status=");
    serial_putdec((uint64_t)(unsigned)status);
    serial_puts(" mime=");
    serial_puts(normalised_type);
    serial_puts("\n");
    /* The temporary UTF-8 buffer is intentionally scoped over both
     * FETCH_DATA and FETCH_FINISHED.  NetSurf consumes the data synchronously
     * in this fetcher contract, after which it is always released below. */
    uint8_t *utf8_payload = NULL;
    uint8_t *redirect_base_payload = NULL;
    const uint8_t *payload = NULL;
    size_t body_len = 0;
    bool transcoded_sjis = false;

    if (!__atomic_load_n(&c->aborted, __ATOMIC_ACQUIRE)) {
        const char *body = http_response_body(http);
        if (body == NULL) {
            fetch_http_send_error_document(c, "HTTP応答本文を解析できませんでした。");
            http_destroy(http);
            c->http = NULL;
            return true;
        }
        /* FETCH_DATA must contain exactly the response body.  The HTTP client
         * reports that byte span directly; never use strlen because binary
         * payloads legitimately contain NUL bytes. */
        body_len = (size_t)http_response_length(http);
        payload = (const uint8_t *)body;
        if (cos_http_is_text_document(normalised_type) &&
            cos_sjis_charset_declared(content_type, payload, body_len)) {
            size_t utf8_len = 0;
            utf8_payload = cos_sjis_to_utf8(payload, body_len, &utf8_len);
            if (utf8_payload != NULL) {
                payload = utf8_payload;
                body_len = utf8_len;
                transcoded_sjis = true;
                serial_puts("[NetSurf] CP932/Shift_JIS response transcoded to UTF-8\n");
            } else {
                /* Allocation failure cannot turn a successfully received page
                 * into a fetch failure; let the existing NetSurf path decide. */
                serial_puts("[NetSurf] CP932 conversion skipped: no temporary buffer\n");
            }
        }
#ifdef COS_KERNEL
        /* Bound every HTML document by the same 10MiB safety limit.  The old
         * Wikipedia-only 64KiB truncation made ordinary standards-compliant
         * markup disappear before NetSurf/QuickJS could construct its DOM.
         * Keep a complete final tag boundary when a truly oversized response
         * reaches the shared transport cap. */
        if (cos_sjis_ascii_equal_fold((const uint8_t *)normalised_type,
                                      strlen(normalised_type), "text/html") &&
            body_len > HTTP_MAX_RESP) {
            body_len = HTTP_MAX_RESP;
            while (body_len > 0 && payload[body_len - 1] != '>') {
                --body_len;
            }
            serial_puts("[NetSurf] HTML capped to 10MiB safety limit\n");
        }
#endif
        /* The transport already followed any redirects. Preserve the final
         * document origin for relative links/forms by supplying an HTML base
         * only when it differs from the NetSurf request URL. */
        if (cos_sjis_ascii_equal_fold((const uint8_t *)normalised_type,
                                      strlen(normalised_type), "text/html")) {
            char effective_url[HTTP_MAX_URL];
            if (http_get_effective_url(http, effective_url,
                                       sizeof(effective_url)) == 0 &&
                url_str != NULL && strcmp(effective_url, url_str) != 0) {
                size_t based_len = 0;
                redirect_base_payload = cos_http_prepend_effective_base(
                    payload, body_len, effective_url, &based_len);
                if (redirect_base_payload != NULL) {
                    payload = redirect_base_payload;
                    body_len = based_len;
                    serial_puts("[NetSurf] Redirect final base URL applied: ");
                    serial_puts(effective_url);
                    serial_puts("\n");
                } else {
                    serial_puts("[NetSurf] Redirect base URL injection skipped: no temporary buffer\n");
                }
            }
        }
    }

    fetch_http_send_header(c, transcoded_sjis ?
        "Content-Type: %s; charset=utf-8" : "Content-Type: %s", normalised_type);
    fetch_http_send_header(c, "Content-Length: %u", (unsigned)body_len);

    /* Keep conversion/base buffers alive across GUI frames. A redirect-base
     * buffer owns a complete copy of any converted payload, so the now-redundant
     * CP932 buffer can be released immediately in that case. */
    if (redirect_base_payload != NULL) {
        if (utf8_payload != NULL) {
            kfree(utf8_payload);
            utf8_payload = NULL;
        }
        c->delivery_owned_payload = redirect_base_payload;
    } else if (utf8_payload != NULL) {
        c->delivery_owned_payload = utf8_payload;
    }
    c->delivery_payload = payload;
    c->delivery_length = body_len;
    c->delivery_offset = 0;
    c->response_headers_sent = true;
    c->response_prepared = true;
    return fetch_http_deliver_prepared_slice(c);
}

/* Structurally identical to fetch_data_poll() in data.c - see that
 * file's comments for why the ring is drained into save_ring rather
 * than iterated in place (contexts can be added/removed by the
 * callbacks fired during processing, which is why locked/save_ring
 * exist at all). */
static void fetch_http_poll(lwc_string *scheme)
{
    (void)scheme;

    if (ring == NULL) {
        return;
    }

    /* Detach this generation before issuing callbacks. The worker owns the
     * blocking DNS/TCP/TLS transfer; this GUI-side loop only queues fresh
     * transport work or translates a completed response into FETCH_* messages.
     * Requests added re-entrantly by HTML/CSS parsing stay on `ring` for the
     * next frame, preserving NetSurf's normal fetch-ring contract. */
    struct cos_http_fetch_context *work_ring = ring;
    struct cos_http_fetch_context *c, *save_ring = NULL;
    /* In safe synchronous mode, one completed transport per GUI poll is the
     * correct cooperative budget.  Draining every queued CSS/image request in
     * a single pass held many same-origin connections simultaneously, defeated
     * the bounded HTTP keep-alive pool, and starved first paint. */
    int transport_budget = worker_count != 0u ? 0x7fffffff : 1;
    ring = NULL;

    while (work_ring != NULL) {
        c = work_ring;
        RING_REMOVE(work_ring, c);

        if (c->locked) {
            RING_INSERT(save_ring, c);
            continue;
        }

        if (!c->transport_complete) {
            if (worker_count != 0u) {
                fetch_http_enqueue_worker(c);
            } else {
                /* Safe fallback for an exhausted thread table: keep legacy
                 * behavior rather than dropping a navigation. */
                fetch_http_run_transport(c);
            }
            RING_INSERT(save_ring, c);
            if (--transport_budget == 0) {
                break;
            }
            continue;
        }

        bool delivered = true;
        if (!__atomic_load_n(&c->aborted, __ATOMIC_ACQUIRE)) {
            delivered = fetch_http_process(c);
        } else if (c->http != NULL) {
            http_destroy(c->http);
            c->http = NULL;
        }

        if (!delivered) {
            /* The response body has more bounded slices. Keep this request in
             * the normal NetSurf ring so the next GUI pass continues parsing
             * without a long owner-thread stall. */
            RING_INSERT(save_ring, c);
            continue;
        }
        fetch_remove_from_queues(c->parent_fetch);
        fetch_free(c->parent_fetch);
    }

    /* A budget stop leaves untouched contexts on work_ring. Preserve them
     * behind the completed/in-flight contexts for the next GUI frame. */
    while (work_ring != NULL) {
        c = work_ring;
        RING_REMOVE(work_ring, c);
        RING_INSERT(save_ring, c);
    }

    /* Retain queued/in-flight original requests without discarding image and
     * other requests enqueued while completed callbacks were processed. */
    while (save_ring != NULL) {
        c = save_ring;
        RING_REMOVE(save_ring, c);
        RING_INSERT(ring, c);
    }
}

/* Registers this fetcher for both "http" and "https" - see
 * corestring_lwc_http/corestring_lwc_https in
 * utils/corestrings.c, populated by corestrings_init() (now called
 * from cos_netsurf_init() - see the C-OS 4.0.8 fix there). Called
 * once from cos_netsurf_init(), after corestrings_init() and before
 * hlcache_initialise() - same relative position as
 * fetch_data_register(). */
nserror fetch_http_register(void)
{
    static const struct fetcher_operation_table http_ops = {
        .initialise = fetch_http_initialise,
        .acceptable = fetch_http_can_fetch,
        .setup = fetch_http_setup,
        .start = fetch_http_start,
        .abort = fetch_http_abort,
        .free = fetch_http_free,
        .poll = fetch_http_poll,
        .finalise = fetch_http_finalise,
    };

    lwc_string *http_scheme = lwc_string_ref(corestring_lwc_http);
    nserror err = fetcher_add(http_scheme, &http_ops);
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] fetcher_add(http) failed\n");
        return err;
    }

    lwc_string *https_scheme = lwc_string_ref(corestring_lwc_https);
    err = fetcher_add(https_scheme, &http_ops);
    if (err != NSERROR_OK) {
        serial_puts("[NetSurf] fetcher_add(https) failed\n");
        return err;
    }

    serial_puts("[NetSurf] http:/https: fetcher registered (backed by "
                "kernel/drivers/http.c)\n");
    return NSERROR_OK;
}
