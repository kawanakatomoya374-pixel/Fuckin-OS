/*
 * C-OS WebP content handler for the upstream NetSurf content factory.
 *
 * Backed by the ported FFmpeg VP8/WebP decoder (see
 * src/third_party/ffmpeg_webp/CPORT_README.md) through its one exported
 * entry point, cos_webp_decode() - the same function the standalone
 * Image Viewer uses (src/apps/jpeg_viewer.c). This handler owns an
 * ordinary frontend bitmap for the full NetSurf fetch -> content ->
 * object -> redraw lifecycle, following cos_netsurf_jpeg.c's structure
 * exactly. Animated WebP is out of scope (the ported decoder's
 * CONFIG_WEBP_ANIM_DECODER is 0 - see config.h in that directory) -
 * such files report a content error rather than showing only their
 * first frame silently mislabeled as the whole image.
 */
#undef PLOT_FONT_FAMILY_SANS_SERIF

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "content/content.h"
#include "netsurf/content.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "netsurf/bitmap.h"
#include "netsurf/plotters.h"

#define COS_NS_WEBP_MAX_WIDTH  1920u
#define COS_NS_WEBP_MAX_HEIGHT 1080u
#define COS_NS_WEBP_SCRATCH_BYTES \
    ((size_t)COS_NS_WEBP_MAX_WIDTH * (size_t)COS_NS_WEBP_MAX_HEIGHT * 4u)

extern bool cos_webp_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                             uint64_t max_out_w, uint64_t max_out_h,
                             uint64_t* out_w, uint64_t* out_h);

/* Same reasoning as cos_ns_jpeg_decode_scratch in cos_netsurf_jpeg.c:
 * decode to bounded BSS first, then copy into an exactly-sized NetSurf
 * bitmap so the plotter's bitmap stride always equals the image width. */
static uint8_t cos_ns_webp_decode_scratch[COS_NS_WEBP_SCRATCH_BYTES];

typedef struct cos_ns_webp_content {
    struct content base;
    struct bitmap *bitmap;
} cos_ns_webp_content;

static nserror cos_ns_webp_create(const content_handler *handler,
                                  lwc_string *mime_type,
                                  const struct http_parameter *params,
                                  struct llcache_handle *llcache,
                                  const char *fallback_charset,
                                  bool quirks,
                                  struct content **out)
{
    cos_ns_webp_content *webp = calloc(1, sizeof(*webp));
    if (webp == NULL) return NSERROR_NOMEM;

    nserror err = content__init(&webp->base, handler, mime_type, params,
                                llcache, fallback_charset, quirks);
    if (err != NSERROR_OK) {
        free(webp);
        return err;
    }
    *out = &webp->base;
    return NSERROR_OK;
}

static bool cos_ns_webp_process_data(struct content *c, const char *data,
                                     unsigned int size)
{
    (void)c;
    (void)data;
    (void)size;
    /* Decode after FETCH_FINISHED from the complete llcache byte span. */
    return true;
}

static bool cos_ns_webp_data_complete(struct content *c)
{
    cos_ns_webp_content *webp = (cos_ns_webp_content *)c;
    size_t source_size = 0;
    const uint8_t *source = content__get_source_data(c, &source_size);
    /* RIFF container, "WEBP" fourcc at offset 8 - see is_webp_file() in
     * src/apps/jpeg_viewer.c for the same check on the standalone
     * viewer's path. */
    if (source == NULL || source_size < 12 ||
        source[0] != 'R' || source[1] != 'I' || source[2] != 'F' || source[3] != 'F' ||
        source[8] != 'W' || source[9] != 'E' || source[10] != 'B' || source[11] != 'P') {
        content_broadcast_error(c, NSERROR_UNKNOWN, "Invalid WebP image");
        content_set_error(c);
        return false;
    }

    uint64_t width = 0;
    uint64_t height = 0;
    bool ok = cos_webp_decode(source, (uint64_t)source_size,
                               cos_ns_webp_decode_scratch,
                               COS_NS_WEBP_MAX_WIDTH, COS_NS_WEBP_MAX_HEIGHT,
                               &width, &height);
    if (!ok || width == 0 || height == 0 ||
        width > COS_NS_WEBP_MAX_WIDTH || height > COS_NS_WEBP_MAX_HEIGHT) {
        content_broadcast_error(c, NSERROR_UNKNOWN,
                                "Unsupported, corrupt, or oversized WebP image");
        content_set_error(c);
        return false;
    }

    webp->bitmap = guit->bitmap->create((int)width, (int)height, BITMAP_CLEAR);
    if (webp->bitmap == NULL) {
        content_broadcast_error(c, NSERROR_NOMEM, "WebP bitmap allocation failed");
        content_set_error(c);
        return false;
    }
    uint8_t *pixels = guit->bitmap->get_buffer(webp->bitmap);
    if (pixels == NULL) {
        guit->bitmap->destroy(webp->bitmap);
        webp->bitmap = NULL;
        content_broadcast_error(c, NSERROR_NOMEM, "WebP bitmap buffer unavailable");
        content_set_error(c);
        return false;
    }
    size_t row_bytes = (size_t)width * 4u;
    bool has_alpha = false;
    for (uint64_t y = 0; y < height; ++y) {
        const uint8_t *src_row = cos_ns_webp_decode_scratch + (size_t)y * row_bytes;
        memcpy(pixels + (size_t)y * row_bytes, src_row, row_bytes);
        if (!has_alpha) {
            for (size_t x = 0; x < row_bytes; x += 4) {
                if (src_row[x + 3] != 255) { has_alpha = true; break; }
            }
        }
    }

    guit->bitmap->set_opaque(webp->bitmap, !has_alpha);
    guit->bitmap->modified(webp->bitmap);
    c->width = (int)width;
    c->height = (int)height;
    c->size += (size_t)width * (size_t)height * 4u;
    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");
    return true;
}

static bool cos_ns_webp_redraw(struct content *c,
                                struct content_redraw_data *data,
                                const struct rect *clip,
                                const struct redraw_context *ctx)
{
    cos_ns_webp_content *webp = (cos_ns_webp_content *)c;
    if (webp->bitmap == NULL || data == NULL || ctx == NULL ||
        ctx->plot == NULL || ctx->plot->bitmap == NULL) return false;

    bitmap_flags_t flags = BITMAPF_NONE;
    if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
    if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;
    return ctx->plot->bitmap(ctx, webp->bitmap, data->x, data->y,
                             data->width, data->height,
                             data->background_colour, flags) == NSERROR_OK;
}

static void cos_ns_webp_destroy(struct content *c)
{
    cos_ns_webp_content *webp = (cos_ns_webp_content *)c;
    if (webp->bitmap != NULL) {
        guit->bitmap->destroy(webp->bitmap);
        webp->bitmap = NULL;
    }
}

static void *cos_ns_webp_get_internal(const struct content *c, void *context)
{
    (void)context;
    return ((const cos_ns_webp_content *)c)->bitmap;
}

static bool cos_ns_webp_is_opaque(struct content *c)
{
    cos_ns_webp_content *webp = (cos_ns_webp_content *)c;
    return webp->bitmap != NULL && guit->bitmap->get_opaque(webp->bitmap);
}

static content_type cos_ns_webp_type(void)
{
    return CONTENT_IMAGE;
}

static const content_handler cos_ns_webp_handler = {
    .create = cos_ns_webp_create,
    .process_data = cos_ns_webp_process_data,
    .data_complete = cos_ns_webp_data_complete,
    .destroy = cos_ns_webp_destroy,
    .redraw = cos_ns_webp_redraw,
    .get_internal = cos_ns_webp_get_internal,
    .is_opaque = cos_ns_webp_is_opaque,
    .type = cos_ns_webp_type
};

nserror cos_netsurf_webp_init(void)
{
    static const char *types[] = { "image/webp" };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        nserror err = content_factory_register_handler(types[i],
                                                        &cos_ns_webp_handler);
        if (err != NSERROR_OK) return err;
    }
    return NSERROR_OK;
}
