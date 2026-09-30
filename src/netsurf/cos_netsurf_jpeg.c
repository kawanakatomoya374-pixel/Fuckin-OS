/*
 * C-OS JPEG content handler for the upstream NetSurf content factory.
 *
 * The standalone C-OS viewer already contains a freestanding baseline-JPEG
 * decoder.  This handler deliberately calls its stateless memory API instead
 * of the viewer singleton, then owns an ordinary frontend bitmap for the full
 * NetSurf fetch -> content -> object -> redraw lifecycle.  Unsupported
 * progressive/arithmetic JPEGs report a content error; they are never replaced
 * with fabricated pixels.
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
#include "../apps/jpeg_viewer.h"

#define COS_NS_JPEG_MAX_WIDTH  1920u
#define COS_NS_JPEG_MAX_HEIGHT 1080u
#define COS_NS_JPEG_SCRATCH_BYTES \
    ((size_t)COS_NS_JPEG_MAX_WIDTH * (size_t)COS_NS_JPEG_MAX_HEIGHT * 4u)

/* The freestanding JPEG decoder densely packs rows at the decoded width.
 * Decode to bounded BSS first, then copy into an exactly-sized NetSurf bitmap
 * so the plotter's bitmap stride always equals the image width. */
static uint8_t cos_ns_jpeg_decode_scratch[COS_NS_JPEG_SCRATCH_BYTES];

typedef struct cos_ns_jpeg_content {
    struct content base;
    struct bitmap *bitmap;
} cos_ns_jpeg_content;

static nserror cos_ns_jpeg_create(const content_handler *handler,
                                  lwc_string *mime_type,
                                  const struct http_parameter *params,
                                  struct llcache_handle *llcache,
                                  const char *fallback_charset,
                                  bool quirks,
                                  struct content **out)
{
    cos_ns_jpeg_content *jpeg = calloc(1, sizeof(*jpeg));
    if (jpeg == NULL) return NSERROR_NOMEM;

    nserror err = content__init(&jpeg->base, handler, mime_type, params,
                                llcache, fallback_charset, quirks);
    if (err != NSERROR_OK) {
        free(jpeg);
        return err;
    }
    *out = &jpeg->base;
    return NSERROR_OK;
}

static bool cos_ns_jpeg_process_data(struct content *c, const char *data,
                                     unsigned int size)
{
    (void)c;
    (void)data;
    (void)size;
    /* Decode after FETCH_FINISHED from the complete llcache byte span. */
    return true;
}

static bool cos_ns_jpeg_data_complete(struct content *c)
{
    cos_ns_jpeg_content *jpeg = (cos_ns_jpeg_content *)c;
    size_t source_size = 0;
    const uint8_t *source = content__get_source_data(c, &source_size);
    if (source == NULL || source_size < 3 ||
        source[0] != 0xff || source[1] != 0xd8 || source[2] != 0xff) {
        content_broadcast_error(c, NSERROR_UNKNOWN, "Invalid JPEG image");
        content_set_error(c);
        return false;
    }

    uint64_t width = 0;
    uint64_t height = 0;
    uint8_t components = 0;
    int rc = jpeg_decode_memory_to_bgra(source, (uint64_t)source_size,
                                        cos_ns_jpeg_decode_scratch,
                                        COS_NS_JPEG_MAX_WIDTH,
                                        COS_NS_JPEG_MAX_HEIGHT,
                                        &width, &height, &components);
    if (rc != 0 || width == 0 || height == 0 ||
        width > COS_NS_JPEG_MAX_WIDTH || height > COS_NS_JPEG_MAX_HEIGHT) {
        content_broadcast_error(c, NSERROR_UNKNOWN,
                                "Unsupported, corrupt, or oversized JPEG image");
        content_set_error(c);
        return false;
    }

    jpeg->bitmap = guit->bitmap->create((int)width, (int)height, BITMAP_CLEAR);
    if (jpeg->bitmap == NULL) {
        content_broadcast_error(c, NSERROR_NOMEM, "JPEG bitmap allocation failed");
        content_set_error(c);
        return false;
    }
    uint8_t *pixels = guit->bitmap->get_buffer(jpeg->bitmap);
    if (pixels == NULL) {
        guit->bitmap->destroy(jpeg->bitmap);
        jpeg->bitmap = NULL;
        content_broadcast_error(c, NSERROR_NOMEM, "JPEG bitmap buffer unavailable");
        content_set_error(c);
        return false;
    }
    size_t row_bytes = (size_t)width * 4u;
    for (uint64_t y = 0; y < height; ++y) {
        memcpy(pixels + (size_t)y * row_bytes,
               cos_ns_jpeg_decode_scratch + (size_t)y * row_bytes,
               row_bytes);
    }

    /* JPEG has no alpha channel. */
    guit->bitmap->set_opaque(jpeg->bitmap, true);
    guit->bitmap->modified(jpeg->bitmap);
    c->width = (int)width;
    c->height = (int)height;
    c->size += (size_t)width * (size_t)height * 4u;
    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");
    (void)components;
    return true;
}

static bool cos_ns_jpeg_redraw(struct content *c,
                                struct content_redraw_data *data,
                                const struct rect *clip,
                                const struct redraw_context *ctx)
{
    cos_ns_jpeg_content *jpeg = (cos_ns_jpeg_content *)c;
    if (jpeg->bitmap == NULL || data == NULL || ctx == NULL ||
        ctx->plot == NULL || ctx->plot->bitmap == NULL) return false;

    bitmap_flags_t flags = BITMAPF_NONE;
    if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
    if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;
    return ctx->plot->bitmap(ctx, jpeg->bitmap, data->x, data->y,
                             data->width, data->height,
                             data->background_colour, flags) == NSERROR_OK;
}

static void cos_ns_jpeg_destroy(struct content *c)
{
    cos_ns_jpeg_content *jpeg = (cos_ns_jpeg_content *)c;
    if (jpeg->bitmap != NULL) {
        guit->bitmap->destroy(jpeg->bitmap);
        jpeg->bitmap = NULL;
    }
}

static void *cos_ns_jpeg_get_internal(const struct content *c, void *context)
{
    (void)context;
    return ((const cos_ns_jpeg_content *)c)->bitmap;
}

static bool cos_ns_jpeg_is_opaque(struct content *c)
{
    cos_ns_jpeg_content *jpeg = (cos_ns_jpeg_content *)c;
    return jpeg->bitmap != NULL && guit->bitmap->get_opaque(jpeg->bitmap);
}

static content_type cos_ns_jpeg_type(void)
{
    return CONTENT_IMAGE;
}

static const content_handler cos_ns_jpeg_handler = {
    .create = cos_ns_jpeg_create,
    .process_data = cos_ns_jpeg_process_data,
    .data_complete = cos_ns_jpeg_data_complete,
    .destroy = cos_ns_jpeg_destroy,
    .redraw = cos_ns_jpeg_redraw,
    .get_internal = cos_ns_jpeg_get_internal,
    .is_opaque = cos_ns_jpeg_is_opaque,
    .type = cos_ns_jpeg_type
};

nserror cos_netsurf_jpeg_init(void)
{
    static const char *types[] = {
        "image/jpeg", "image/jpg", "image/pjpeg", "image/jfif"
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        nserror err = content_factory_register_handler(types[i],
                                                        &cos_ns_jpeg_handler);
        if (err != NSERROR_OK) return err;
    }
    return NSERROR_OK;
}
