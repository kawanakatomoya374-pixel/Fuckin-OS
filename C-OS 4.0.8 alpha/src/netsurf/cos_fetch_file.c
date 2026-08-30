/*
 * cos_fetch_file.c - C-OS storage-backed file: fetcher for NetSurf.
 *
 * Local HTML must use the same HTML/CSS/QuickJS/content lifecycle as remote
 * pages.  This fetcher maps a constrained file:/// URL to the C-OS filesystem
 * and emits normal NetSurf FETCH_HEADER/FETCH_DATA/FETCH_FINISHED messages.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "content/fetch.h"
#include "content/fetchers.h"

#include "fs.h"
#include "memory.h"
#include "serial.h"

/* Path-aware reader is implemented by the C-OS filesystem; fs.h currently
 * exposes only the legacy root wrapper, while the browser already uses this
 * path-aware API for file:// navigation. */
extern const char *fs_read_file_at(const char *path, const char *name);

/* Browser document limits are independent from file-manager metadata/editor
 * buffers.  Local HTML/CSS/JS must follow the same 10MiB NetSurf safety limit
 * as network documents, rather than inheriting FS_MAX_DATA (32KiB). */
#define COS_FILE_FETCH_MAX_BYTES (10u * 1024u * 1024u)

typedef struct cos_file_fetch_context {
    struct fetch *parent_fetch;
    nsurl *url;
    char *data;
    size_t data_len;
    const char *mime;
    bool aborted;
    bool locked;
    struct cos_file_fetch_context *next;
} cos_file_fetch_context_t;

static cos_file_fetch_context_t *g_file_pending;

static void cos_file_fetch_send(const fetch_msg *msg, cos_file_fetch_context_t *ctx)
{
    ctx->locked = true;
    fetch_send_callback(msg, ctx->parent_fetch);
    ctx->locked = false;
}

static void cos_file_fetch_header(cos_file_fetch_context_t *ctx, const char *header)
{
    fetch_msg msg;
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)header;
    msg.data.header_or_data.len = strlen(header);
    cos_file_fetch_send(&msg, ctx);
}

static const char *cos_file_fetch_mime(const char *path)
{
    const char *dot = NULL;
    for (const char *p = path; p != NULL && *p; ++p) {
        if (*p == '.') dot = p;
    }
    if (dot == NULL) return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js") == 0 || strcmp(dot, ".mjs") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".bmp") == 0) return "image/bmp";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0) return "image/vnd.microsoft.icon";
    return "text/plain; charset=utf-8";
}

static int cos_file_fetch_hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode only a root-relative file:/// path.  Backslash, a drive marker and
 * parent traversal are rejected so a malformed URL cannot escape C-OS storage. */
static bool cos_file_fetch_path(const nsurl *url, char out[FS_MAX_PATH])
{
    const char *raw = nsurl_access(url);
    const char *prefix = "file://";
    if (raw == NULL || strncmp(raw, prefix, 7) != 0) return false;
    const char *p = raw + 7;
    while (*p == '/') ++p;
    size_t used = 0;
    char segment[FS_MAX_NAME];
    size_t segment_len = 0;

    while (*p && *p != '?' && *p != '#') {
        char c = *p++;
        if (c == '%' && p[0] && p[1]) {
            int hi = cos_file_fetch_hex(p[0]);
            int lo = cos_file_fetch_hex(p[1]);
            if (hi < 0 || lo < 0) return false;
            c = (char)((hi << 4) | lo);
            p += 2;
        }
        if (c == '\\' || c == ':') return false;
        if (c == '/') {
            if (segment_len == 2 && segment[0] == '.' && segment[1] == '.') return false;
            if (used != 0 && used + 1 < FS_MAX_PATH) out[used++] = '/';
            segment_len = 0;
            continue;
        }
        if ((unsigned char)c < 0x20 || used + 1 >= FS_MAX_PATH ||
            segment_len + 1 >= sizeof(segment)) return false;
        out[used++] = c;
        segment[segment_len++] = c;
    }
    if (segment_len == 2 && segment[0] == '.' && segment[1] == '.') return false;
    out[used] = '\0';
    return used != 0;
}

static bool cos_file_fetch_can_fetch(const nsurl *url)
{
    char path[FS_MAX_PATH];
    return cos_file_fetch_path(url, path);
}

static void *cos_file_fetch_setup(struct fetch *parent_fetch, nsurl *url,
                                  bool only_2xx, bool downgrade_tls,
                                  const char *post_urlenc,
                                  const struct fetch_multipart_data *post_multipart,
                                  const char **headers)
{
    (void)only_2xx;
    (void)downgrade_tls;
    (void)post_urlenc;
    (void)post_multipart;
    (void)headers;
    cos_file_fetch_context_t *ctx = kmalloc(sizeof(*ctx));
    if (ctx == NULL) return NULL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->parent_fetch = parent_fetch;
    ctx->url = nsurl_ref(url);
    if (ctx->url == NULL) {
        kfree(ctx);
        return NULL;
    }
    ctx->next = g_file_pending;
    g_file_pending = ctx;
    serial_puts("[NetSurf/file] queued: ");
    serial_puts(nsurl_access(url));
    serial_puts("\n");
    return ctx;
}

static bool cos_file_fetch_start(void *ctx)
{
    return ctx != NULL;
}

static void cos_file_fetch_abort(void *ctx)
{
    if (ctx != NULL) ((cos_file_fetch_context_t *)ctx)->aborted = true;
}

static void cos_file_fetch_free(void *ctx)
{
    cos_file_fetch_context_t *c = ctx;
    if (c == NULL) return;
    if (c->url != NULL) nsurl_unref(c->url);
    if (c->data != NULL) kfree(c->data);
    kfree(c);
}

static bool cos_file_fetch_load(cos_file_fetch_context_t *ctx, char path[FS_MAX_PATH])
{
    if (!cos_file_fetch_path(ctx->url, path)) {
        serial_puts("[NetSurf/file] rejected URL path\n");
        return false;
    }
    serial_puts("[NetSurf/file] reading /" );
    serial_puts(path);
    serial_puts("\n");
    const char *source = fs_read_file_at("/", path);
    if (source == NULL) {
        serial_puts("[NetSurf/file] filesystem read failed\n");
        return false;
    }
    size_t len = strlen(source);
    if (len > COS_FILE_FETCH_MAX_BYTES) {
        serial_puts("[NetSurf/file] rejected: exceeds 10MiB document limit\n");
        return false;
    }
    serial_puts("[NetSurf/file] read bytes=");
    serial_putdec((uint64_t)len);
    serial_puts("\n");
    ctx->data = kmalloc(len + 1);
    if (ctx->data == NULL) return false;
    memcpy(ctx->data, source, len);
    ctx->data[len] = '\0';
    ctx->data_len = len;
    ctx->mime = cos_file_fetch_mime(path);
    return true;
}

static void cos_file_fetch_process(cos_file_fetch_context_t *ctx)
{
    fetch_msg msg;
    char path[FS_MAX_PATH];
    if (ctx->aborted) return;
    if (!cos_file_fetch_load(ctx, path)) {
        msg.type = FETCH_ERROR;
        msg.data.error = "C-OS local file not found or unsupported";
        cos_file_fetch_send(&msg, ctx);
        return;
    }

    fetch_set_http_code(ctx->parent_fetch, 200);
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "Content-Type: %s", ctx->mime);
    cos_file_fetch_header(ctx, content_type);
    if (ctx->aborted) return;
    char content_length[64];
    snprintf(content_length, sizeof(content_length), "Content-Length: %u",
             (unsigned int)ctx->data_len);
    cos_file_fetch_header(ctx, content_length);
    if (ctx->aborted) return;
    cos_file_fetch_header(ctx, "Cache-Control: no-store");
    if (ctx->aborted) return;

    msg.type = FETCH_DATA;
    msg.data.header_or_data.buf = (const uint8_t *)ctx->data;
    msg.data.header_or_data.len = ctx->data_len;
    cos_file_fetch_send(&msg, ctx);
    if (ctx->aborted) return;
    msg.type = FETCH_FINISHED;
    cos_file_fetch_send(&msg, ctx);
    serial_puts("[NetSurf/file] finished\n");
}

static void cos_file_fetch_poll(lwc_string *scheme)
{
    (void)scheme;
    cos_file_fetch_context_t *deferred = NULL;
    while (g_file_pending != NULL) {
        cos_file_fetch_context_t *ctx = g_file_pending;
        g_file_pending = ctx->next;
        ctx->next = NULL;
        if (ctx->locked) {
            ctx->next = deferred;
            deferred = ctx;
            continue;
        }
        serial_puts("[NetSurf/file] processing queued request\n");
        cos_file_fetch_process(ctx);
        fetch_remove_from_queues(ctx->parent_fetch);
        fetch_free(ctx->parent_fetch);
    }
    g_file_pending = deferred;
}

nserror cos_fetch_file_register(void)
{
    lwc_string *scheme = lwc_string_ref(corestring_lwc_file);
    if (scheme == NULL) return NSERROR_INIT_FAILED;
    static const struct fetcher_operation_table ops = {
        .initialise = NULL,
        .acceptable = cos_file_fetch_can_fetch,
        .setup = cos_file_fetch_setup,
        .start = cos_file_fetch_start,
        .abort = cos_file_fetch_abort,
        .free = cos_file_fetch_free,
        .poll = cos_file_fetch_poll,
        .fdset = NULL,
        .finalise = NULL,
    };
    return fetcher_add(scheme, &ops);
}
