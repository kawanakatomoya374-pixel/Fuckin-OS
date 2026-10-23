/*
 * C-OS PNG content handler for the upstream NetSurf content factory.
 *
 * This keeps image loading inside NetSurf's normal fetch -> content ->
 * object -> redraw path.  Decoding uses C-OS's freestanding PNG decoder
 * and produces the frontend bitmap object consumed by the plotter.
 */
/* The kernel build defines this legacy alias, but NetSurf declares the
 * same token as an enum member in plot_style.h. */
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
#include "../apps/png_decoder.h"

#define COS_NS_PNG_MAX_DIMENSION 2048u

typedef struct cos_ns_png_content {
    struct content base;
    struct bitmap *bitmap;
} cos_ns_png_content;

static uint32_t cos_ns_png_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static nserror cos_ns_png_create(const content_handler *handler,
                                 lwc_string *mime_type,
                                 const struct http_parameter *params,
                                 struct llcache_handle *llcache,
                                 const char *fallback_charset,
                                 bool quirks,
                                 struct content **out)
{
    cos_ns_png_content *png = calloc(1, sizeof(*png));
    if (png == NULL) return NSERROR_NOMEM;

    nserror err = content__init(&png->base, handler, mime_type, params,
                                llcache, fallback_charset, quirks);
    if (err != NSERROR_OK) {
        free(png);
        return err;
    }
    *out = &png->base;
    return NSERROR_OK;
}

static bool cos_ns_png_process_data(struct content *c, const char *data,
                                    unsigned int size)
{
    (void)c;
    (void)data;
    (void)size;
    /* Source bytes are retained by the standard llcache.  Decode once the
     * complete PNG is available so a single decoded bitmap is authoritative. */
    return true;
}

static bool cos_ns_png_data_complete(struct content *c)
{
    cos_ns_png_content *png = (cos_ns_png_content *)c;
    size_t source_size = 0;
    const uint8_t *source = content__get_source_data(c, &source_size);
    static const uint8_t signature[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };

    if (source == NULL || source_size < 24 ||
        memcmp(source, signature, sizeof(signature)) != 0) {
        content_broadcast_error(c, NSERROR_PNG_ERROR, "Invalid PNG image");
        content_set_error(c);
        return false;
    }

    uint32_t source_width = cos_ns_png_be32(source + 16);
    uint32_t source_height = cos_ns_png_be32(source + 20);
    if (source_width == 0 || source_height == 0 ||
        source_width > COS_NS_PNG_MAX_DIMENSION ||
        source_height > COS_NS_PNG_MAX_DIMENSION) {
        content_broadcast_error(c, NSERROR_BAD_SIZE, "PNG dimensions unsupported");
        content_set_error(c);
        return false;
    }

    png->bitmap = guit->bitmap->create((int)source_width,
                                       (int)source_height, BITMAP_CLEAR);
    if (png->bitmap == NULL) {
        content_broadcast_error(c, NSERROR_NOMEM, "PNG bitmap allocation failed");
        content_set_error(c);
        return false;
    }

    uint64_t decoded_width = 0;
    uint64_t decoded_height = 0;
    uint8_t *pixels = guit->bitmap->get_buffer(png->bitmap);
    if (pixels == NULL ||
        !png_decode(source, (uint64_t)source_size, pixels,
                    source_width, source_height,
                    &decoded_width, &decoded_height) ||
        decoded_width != source_width || decoded_height != source_height) {
        guit->bitmap->destroy(png->bitmap);
        png->bitmap = NULL;
        content_broadcast_error(c, NSERROR_PNG_ERROR, "PNG decode failed");
        content_set_error(c);
        return false;
    }

    /* png_decode writes byte-wise BGRA. The C-OS plotter reads this frontend
     * bitmap representation directly and applies alpha during blitting. */
    guit->bitmap->set_opaque(png->bitmap, false);
    guit->bitmap->modified(png->bitmap);
    c->width = (int)decoded_width;
    c->height = (int)decoded_height;
    c->size += (size_t)decoded_width * (size_t)decoded_height * 4u;
    content_set_ready(c);
    content_set_done(c);
    content_set_status(c, "");
    return true;
}

static bool cos_ns_png_redraw(struct content *c,
                               struct content_redraw_data *data,
                               const struct rect *clip,
                               const struct redraw_context *ctx)
{
    cos_ns_png_content *png = (cos_ns_png_content *)c;
    if (png->bitmap == NULL || data == NULL || ctx == NULL ||
        ctx->plot == NULL || ctx->plot->bitmap == NULL) return false;

    bitmap_flags_t flags = BITMAPF_NONE;
    if (data->repeat_x) flags |= BITMAPF_REPEAT_X;
    if (data->repeat_y) flags |= BITMAPF_REPEAT_Y;
    return ctx->plot->bitmap(ctx, png->bitmap, data->x, data->y,
                             data->width, data->height,
                             data->background_colour, flags) == NSERROR_OK;
}

static void cos_ns_png_destroy(struct content *c)
{
    cos_ns_png_content *png = (cos_ns_png_content *)c;
    if (png->bitmap != NULL) {
        guit->bitmap->destroy(png->bitmap);
        png->bitmap = NULL;
    }
}

static void *cos_ns_png_get_internal(const struct content *c, void *context)
{
    (void)context;
    const cos_ns_png_content *png = (const cos_ns_png_content *)c;
    return png->bitmap;
}

static bool cos_ns_png_is_opaque(struct content *c)
{
    cos_ns_png_content *png = (cos_ns_png_content *)c;
    return png->bitmap != NULL && guit->bitmap->get_opaque(png->bitmap);
}

static content_type cos_ns_png_type(void)
{
    return CONTENT_IMAGE;
}

static const content_handler cos_ns_png_handler = {
    .create = cos_ns_png_create,
    .process_data = cos_ns_png_process_data,
    .data_complete = cos_ns_png_data_complete,
    .destroy = cos_ns_png_destroy,
    .redraw = cos_ns_png_redraw,
    .get_internal = cos_ns_png_get_internal,
    .is_opaque = cos_ns_png_is_opaque,
    .type = cos_ns_png_type
};

nserror cos_netsurf_png_init(void)
{
    static const char *types[] = { "image/png", "image/x-png" };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        nserror err = content_factory_register_handler(types[i],
                                                        &cos_ns_png_handler);
        if (err != NSERROR_OK) return err;
    }
    return NSERROR_OK;
}
