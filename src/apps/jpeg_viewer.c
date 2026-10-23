/*
 * jpeg_viewer.c - C-OS Image Viewer glue
 *
 * The decoders themselves live in separate, host-testable files:
 *   - jpeg_codec.c  JPEG (baseline, extended, progressive; see jpeg_codec.h)
 *   - png_decoder.c PNG + zlib/gzip inflate (see png_decoder.h)
 *   - bmp_codec.c   BMP/DIB incl. RLE and embedded JPEG/PNG (see bmp_codec.h)
 *   - image_scale.c area-average downscaler shared by all three
 *   - WebP still goes through cos_webp_decode() (third-party port).
 * See docs/image_decoders.md.
 *
 * This file owns the singleton viewer state (1920x1080 BGRA display buffer,
 * 16 MiB file buffer), loading, display/draw_scaled and the image_viewer_* /
 * jpeg_viewer_* API.  Pictures larger than the display buffer are no longer
 * rejected (JPEG) or cropped (PNG/BMP): they are shrunk to fit - JPEG in the
 * DCT domain (1/2, 1/4, 1/8) plus an exact area average if still needed,
 * PNG/BMP by area average - and current_width/current_height describe the
 * shrunk image.  jpeg_viewer_get_source_size() returns the original size and
 * jpeg_viewer_get_exif_orientation() the EXIF orientation (already applied to
 * the pixels).
 *
 * Any failed load fully resets viewer state (image_loaded, dimensions,
 * component count, source kind) rather than leaving the previous
 * image's info lying around looking like it applies to the new file.
 * A PNG that cannot be decoded still falls back to a placeholder pattern;
 * jpeg_viewer_get_source_kind() tells callers whether what's in the buffer
 * is real pixels or that placeholder.
 */
#include "types.h"
#include "io.h"
#include "serial.h"
#include "memory.h"
#include "vga.h"
#include "gfx_blit.h"
#include "png_decoder.h"
#include "jpeg_codec.h"
#include "bmp_codec.h"
#include <string.h>
#include <stdint.h>

extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);

/* ============================================================================
   Configuration
   ========================================================================== */
#define JPEG_MAX_WIDTH 1920
#define JPEG_MAX_HEIGHT 1080
#define JPEG_BUFFER_SIZE (JPEG_MAX_WIDTH * JPEG_MAX_HEIGHT * 4)  /* 32-bit BGRA */
#define JPEG_FILE_BUFFER_SIZE (16u * 1024u * 1024u)  /* screenshots/photos; bounded kernel decode workspace */

/* JPEG_ERROR_* codes come from jpeg_codec.h */

typedef enum {
    JPEG_SOURCE_NONE = 0,
    JPEG_SOURCE_REAL = 1,
    JPEG_SOURCE_PATTERN = 2
} jpeg_source_kind_t;

typedef struct {
    uint8_t* file_buffer;
    uint64_t file_buffer_size;
    uint64_t file_size;
} file_reader_t;

typedef struct {
    uint8_t* display_buffer;      /* BGRA, JPEG_MAX_WIDTH x JPEG_MAX_HEIGHT */
    uint64_t current_width;
    uint64_t current_height;
    uint8_t  components;
    uint64_t src_width;           /* size stored in the file, before shrinking */
    uint64_t src_height;
    int      exif_orientation;    /* 1..8; already applied to display_buffer */
    bool     image_loaded;
    int      source_kind;         /* jpeg_source_kind_t */
    char     current_filename[256];
} jpeg_viewer_state_t;

static jpeg_viewer_state_t viewer_state = {0};

static uint32_t s_jpeg_load_generation = 0;
/* Bumped by every successful load site below - the GPU texture cache
 * key for this viewer's currently-displayed image (see
 * jpeg_viewer_display()/jpeg_viewer_draw_scaled()) is keyed on this,
 * not on the conversion scratch buffer's address: that buffer can be
 * reused unchanged (same size) across two different images loaded back
 * to back, which would otherwise look like "nothing changed" to the
 * GPU cache and leave the previous photo on screen. */
static void jpeg_viewer_mark_loaded(void) {
    viewer_state.image_loaded = true;
    ++s_jpeg_load_generation;
}
static file_reader_t reader = {0};

/* The JPEG display buffer must survive EFM previews, standalone windows and
 * NetSurf's heap pressure. Keep these bounded workspaces in kernel BSS rather
 * than the fragmented general-purpose heap. */
static uint8_t s_jpeg_display_storage[JPEG_BUFFER_SIZE];
static uint8_t s_jpeg_file_storage[JPEG_FILE_BUFFER_SIZE];

static void reset_viewer_state_for_failure(void) {
    /* On any load failure, fully clear everything that describes "what
     * image is loaded" so a caller can't end up looking at the *new*
     * filename paired with the *old* image's dimensions/pixels. */
    viewer_state.image_loaded = false;
    viewer_state.current_width = 0;
    viewer_state.current_height = 0;
    viewer_state.components = 0;
    viewer_state.src_width = 0;
    viewer_state.src_height = 0;
    viewer_state.exif_orientation = 1;
    viewer_state.source_kind = JPEG_SOURCE_NONE;
}

/* ============================================================================
   Format probes
   ========================================================================== */
static bool is_jpeg_file(const uint8_t* data, uint64_t size) {
    return size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

static bool is_png_file(const uint8_t* data, uint64_t size) {
    return size >= 8 &&
        data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A;
}

static bool is_bmp_file(const uint8_t* data, uint64_t size) {
    return size >= 18 && data[0] == 'B' && data[1] == 'M';
}

/* RIFF container, "WEBP" fourcc at offset 8 - the same signature every
 * WebP file (lossy VP8, lossless VP8L, and the animated/extended VP8X
 * container this port's decoder does not support - see webp.c's own
 * CONFIG_WEBP_ANIM_DECODER=0 in config.h) starts with. */
static bool is_webp_file(const uint8_t* data, uint64_t size) {
    return size >= 12 &&
        data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P';
}

static bool parse_png_header(const uint8_t* data, uint64_t size, uint64_t* width, uint64_t* height, uint8_t* components) {
    /* PNG signature is 8 bytes; the IHDR chunk's width/height are at
     * bytes 16-23 (big-endian u32 each) - bounds-checked to 24 bytes,
     * matching every byte this function actually touches. */
    if (!is_png_file(data, size) || size < 24) return false;
    *width = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) | ((uint32_t)data[18] << 8) | (uint32_t)data[19];
    *height = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) | ((uint32_t)data[22] << 8) | (uint32_t)data[23];
    *components = 4;
    return *width > 0 && *height > 0;
}

/* ============================================================================
   Background loading
   ========================================================================== */
#include "sync.h"
#include "task.h"

int jpeg_viewer_load(const char* filename); /* forward decl for the thread entry below */
static int jpeg_viewer_load_impl(const char* filename); /* forward decl - real definition is further down */

static mutex_t         jpeg_load_mutex;
static bool            jpeg_load_mutex_ready = false;
static volatile bool   jpeg_load_busy = false;
static volatile int    jpeg_load_last_result = JPEG_ERROR_OK;
static char            jpeg_pending_filename[256];

static void jpeg_load_thread_entry(void* arg) {
    (void)arg;
    mutex_lock(&jpeg_load_mutex);
    jpeg_load_last_result = jpeg_viewer_load_impl(jpeg_pending_filename);
    jpeg_load_busy = false;
    mutex_unlock(&jpeg_load_mutex);
}

int jpeg_viewer_load_async(const char* filename) {
    if (!filename) return -1;
    if (!jpeg_load_mutex_ready) {
        mutex_init(&jpeg_load_mutex);
        jpeg_load_mutex_ready = true;
    }
    if (jpeg_load_busy) return -1;

    strncpy(jpeg_pending_filename, filename, sizeof(jpeg_pending_filename) - 1);
    jpeg_pending_filename[sizeof(jpeg_pending_filename) - 1] = '\0';

    jpeg_load_busy = true;
    if (!thread_create_kernel("jpeg_load", (void*)jpeg_load_thread_entry, NULL)) {
        jpeg_load_busy = false;
        return -1;
    }
    return 0;
}

bool jpeg_viewer_is_loading(void) {
    return jpeg_load_busy;
}

int jpeg_viewer_get_last_result(void) {
    return jpeg_load_last_result;
}

/* ============================================================================
   Initialization
   ========================================================================== */
int jpeg_viewer_init(void) {
    serial_puts("[JPEG] Initializing image viewer\n");
    viewer_state.display_buffer = s_jpeg_display_storage;
    reader.file_buffer = s_jpeg_file_storage;
    reader.file_buffer_size = JPEG_FILE_BUFFER_SIZE;
    reader.file_size = 0;

    memset(viewer_state.current_filename, 0, sizeof(viewer_state.current_filename));
    reset_viewer_state_for_failure();

    serial_puts("[JPEG] Image viewer initialized successfully\n");
    return 0;
}

int jpeg_viewer_load(const char* filename) {
    if (!jpeg_load_mutex_ready) {
        mutex_init(&jpeg_load_mutex);
        jpeg_load_mutex_ready = true;
    }
    mutex_lock(&jpeg_load_mutex);
    int result = jpeg_viewer_load_impl(filename);
    mutex_unlock(&jpeg_load_mutex);
    return result;
}

/* Decode a JPEG supplied by a network/content client without touching the
 * singleton file-viewer buffers or its visible state.  The codec keeps all of
 * its state in one heap block per call, so this is safe to call concurrently
 * with jpeg_viewer_load().  Full size or an error: an image (after EXIF
 * rotation) larger than max_width x max_height returns
 * JPEG_ERROR_BUFFER_OVERFLOW - use jpeg_decode_memory_to_bgra_fit() to get a
 * shrunk image instead. */
int jpeg_decode_memory_to_bgra(const uint8_t* data, uint64_t size,
                               uint8_t* out_bgra,
                               uint64_t max_width, uint64_t max_height,
                               uint64_t* out_width, uint64_t* out_height,
                               uint8_t* out_components)
{
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    if (out_components) *out_components = 0;
    if (data == NULL || out_bgra == NULL || max_width == 0 || max_height == 0 ||
        !is_jpeg_file(data, size)) {
        return JPEG_ERROR_INVALID_FORMAT;
    }
    jpeg_info_t info;
    int rc = cos_jpeg_codec_decode(data, size, out_bgra, max_width, max_height, 0, &info);
    if (rc != JPEG_ERROR_OK) return rc;
    if (info.width == 0 || info.height == 0 || info.width > max_width || info.height > max_height) {
        return JPEG_ERROR_INVALID_FORMAT;
    }
    if (out_width) *out_width = info.width;
    if (out_height) *out_height = info.height;
    if (out_components) *out_components = info.ncomp;
    return JPEG_ERROR_OK;
}

/* Like jpeg_decode_memory_to_bgra() but an oversized picture is shrunk to fit
 * (DCT-domain 1/2, 1/4, 1/8 scaling, then an exact area average if even 1/8 is
 * too large) instead of failing.  out_width/out_height describe the shrunk
 * image; src_width/src_height (each may be NULL) the original size. */
int jpeg_decode_memory_to_bgra_fit(const uint8_t* data, uint64_t size,
                                   uint8_t* out_bgra,
                                   uint64_t max_width, uint64_t max_height,
                                   uint64_t* out_width, uint64_t* out_height,
                                   uint8_t* out_components,
                                   uint64_t* src_width, uint64_t* src_height)
{
    if (out_width) *out_width = 0;
    if (out_height) *out_height = 0;
    if (out_components) *out_components = 0;
    if (src_width) *src_width = 0;
    if (src_height) *src_height = 0;
    if (data == NULL || out_bgra == NULL || max_width == 0 || max_height == 0 ||
        !is_jpeg_file(data, size)) {
        return JPEG_ERROR_INVALID_FORMAT;
    }
    jpeg_info_t info;
    int rc = cos_jpeg_codec_decode(data, size, out_bgra, max_width, max_height, JPEG_DEC_FIT, &info);
    if (rc != JPEG_ERROR_OK) return rc;
    if (info.width == 0 || info.height == 0 || info.width > max_width || info.height > max_height) {
        return JPEG_ERROR_INVALID_FORMAT;
    }
    if (out_width) *out_width = info.width;
    if (out_height) *out_height = info.height;
    if (out_components) *out_components = info.ncomp;
    if (src_width) *src_width = info.src_width;
    if (src_height) *src_height = info.src_height;
    return JPEG_ERROR_OK;
}

/* ============================================================================
   File loading
   ========================================================================== */
static int jpeg_viewer_load_impl(const char* filename) {
    serial_puts("[JPEG] Loading image: ");
    serial_puts(filename ? filename : "(null)");
    serial_puts("\n");

    if (!filename) {
        reset_viewer_state_for_failure();
        return JPEG_ERROR_BUFFER_OVERFLOW;
    }

    /* The viewer is shared by EFM previews and the standalone window.  A
     * failed third-party allocation/free must not make every later load fail
     * before filesystem access; rebuild any missing persistent buffers here. */
    if (!viewer_state.display_buffer) {
        viewer_state.display_buffer = s_jpeg_display_storage;
    }
    if (!reader.file_buffer) {
        reader.file_buffer = s_jpeg_file_storage;
        reader.file_buffer_size = JPEG_FILE_BUFFER_SIZE;
        reader.file_size = 0;
    }

    strncpy(viewer_state.current_filename, filename, sizeof(viewer_state.current_filename) - 1);
    viewer_state.current_filename[sizeof(viewer_state.current_filename) - 1] = '\0';

    /* Any early-return failure path below must leave viewer_state fully
     * reset (not carrying over the previous image's dimensions/pixels)
     * even though current_filename above now names the new (failed) file. */
    int64_t bytes_read = cos_fs_read_file(filename, reader.file_buffer, reader.file_buffer_size);
    if (bytes_read <= 0) {
        serial_puts("[JPEG] Failed to read file from filesystem\n");
        reset_viewer_state_for_failure();
        return JPEG_ERROR_INVALID_FORMAT;
    }
    reader.file_size = (uint64_t)bytes_read;

    serial_puts("[JPEG] File read: ");
    serial_putdec(reader.file_size);
    serial_puts(" bytes\n");

    const uint8_t* buf = reader.file_buffer;
    uint64_t size = reader.file_size;

    if (is_jpeg_file(buf, size)) {
        jpeg_info_t info;
        int rc = cos_jpeg_codec_decode(buf, size, viewer_state.display_buffer,
                                   JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT, JPEG_DEC_FIT, &info);
        if (rc != JPEG_ERROR_OK) {
            serial_puts("[JPEG] JPEG decode failed (unsupported feature or corrupt data)\n");
            reset_viewer_state_for_failure();
            return rc;
        }
        uint64_t w = info.width, h = info.height;
        if (w == 0 || h == 0 || w > JPEG_MAX_WIDTH || h > JPEG_MAX_HEIGHT) {
            reset_viewer_state_for_failure();
            return JPEG_ERROR_INVALID_FORMAT;
        }
        viewer_state.current_width = w;
        viewer_state.current_height = h;
        viewer_state.components = info.ncomp;
        viewer_state.src_width = info.src_width;
        viewer_state.src_height = info.src_height;
        viewer_state.exif_orientation = info.orientation ? info.orientation : 1;
        viewer_state.source_kind = JPEG_SOURCE_REAL;
        jpeg_viewer_mark_loaded();

        serial_puts("[JPEG] Decoded JPEG: ");
        serial_putdec(w); serial_puts("x"); serial_putdec(h);
        if (info.scale_denom > 1 || w != info.src_width || h != info.src_height) {
            serial_puts(" (source "); serial_putdec(info.src_width); serial_puts("x");
            serial_putdec(info.src_height); serial_puts(")");
        }
        if (info.truncated) serial_puts(" [truncated data]");
        serial_puts("\n");
        return JPEG_ERROR_OK;
    }

    if (is_bmp_file(buf, size)) {
        uint64_t w = 0, h = 0, sw = 0, sh = 0;
        bool ok = cos_bmp_decode_fit(buf, size, viewer_state.display_buffer,
                                 JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT, &w, &h, &sw, &sh);
        if (!ok || w == 0 || h == 0) {
            reset_viewer_state_for_failure();
            return JPEG_ERROR_INVALID_FORMAT;
        }
        viewer_state.current_width = w;
        viewer_state.current_height = h;
        viewer_state.components = 4;
        viewer_state.src_width = sw;
        viewer_state.src_height = sh;
        viewer_state.exif_orientation = 1;
        viewer_state.source_kind = JPEG_SOURCE_REAL;
        jpeg_viewer_mark_loaded();

        serial_puts("[JPEG] Decoded BMP: ");
        serial_putdec(w); serial_puts("x"); serial_putdec(h); serial_puts("\n");
        return JPEG_ERROR_OK;
    }

    if (is_webp_file(buf, size)) {
        extern bool cos_webp_decode(const uint8_t* data, uint64_t size, uint8_t* out_bgra,
                                     uint64_t max_out_w, uint64_t max_out_h,
                                     uint64_t* out_w, uint64_t* out_h);
        uint64_t w = 0, h = 0;
        if (cos_webp_decode(buf, size, viewer_state.display_buffer,
                             JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT, &w, &h) && w > 0 && h > 0) {
            viewer_state.current_width = w;
            viewer_state.current_height = h;
            viewer_state.components = 4;
            viewer_state.src_width = w;
            viewer_state.src_height = h;
            viewer_state.exif_orientation = 1;
            viewer_state.source_kind = JPEG_SOURCE_REAL;
            jpeg_viewer_mark_loaded();

            serial_puts("[JPEG] Decoded WebP: ");
            serial_putdec(w); serial_puts("x"); serial_putdec(h); serial_puts("\n");
            return JPEG_ERROR_OK;
        }
        serial_puts("[JPEG] WebP decode failed\n");
        reset_viewer_state_for_failure();
        return JPEG_ERROR_INVALID_FORMAT;
    }

    if (is_png_file(buf, size)) {
        uint64_t w = 0, h = 0, sw = 0, sh = 0;
        if (cos_png_decode_fit(buf, size, viewer_state.display_buffer, JPEG_MAX_WIDTH, JPEG_MAX_HEIGHT,
                           &w, &h, &sw, &sh) && w > 0 && h > 0) {
            viewer_state.current_width = w;
            viewer_state.current_height = h;
            viewer_state.components = 4;
            viewer_state.src_width = sw;
            viewer_state.src_height = sh;
            viewer_state.exif_orientation = 1;
            viewer_state.source_kind = JPEG_SOURCE_REAL;
            jpeg_viewer_mark_loaded();

            serial_puts("[JPEG] Decoded PNG: ");
            serial_putdec(w); serial_puts("x"); serial_putdec(h); serial_puts("\n");
            return JPEG_ERROR_OK;
        }

        /* Real decode failed (corrupt data or an invalid header -
         * cos_png_decode_fit() already logged the specific reason). Fall back to a header-only
         * placeholder so the viewer still shows *something* rather
         * than nothing. */
        uint8_t comps = 0;
        if (!parse_png_header(buf, size, &w, &h, &comps) || w == 0 || h == 0) {
            serial_puts("[JPEG] PNG header is invalid or truncated\n");
            reset_viewer_state_for_failure();
            return JPEG_ERROR_INVALID_FORMAT;
        }

        if (w > JPEG_MAX_WIDTH) w = JPEG_MAX_WIDTH;
        if (h > JPEG_MAX_HEIGHT) h = JPEG_MAX_HEIGHT;

        uint64_t checksum = w ^ h;
        uint8_t base_r = (uint8_t)(128 + (checksum & 0xFF) % 128);
        uint8_t base_g = (uint8_t)(100 + ((checksum >> 8) & 0xFF) % 128);
        uint8_t base_b = (uint8_t)(150 + ((checksum >> 16) & 0xFF) % 100);
        for (uint64_t y = 0; y < h; ++y) {
            for (uint64_t x = 0; x < w; ++x) {
                uint8_t r = (uint8_t)((x * 255) / w);
                uint8_t g = (uint8_t)((y * 255) / h);
                uint8_t b = (uint8_t)(((x + y) * 200) / (w + h));
                r = (uint8_t)((r + base_r) / 2);
                g = (uint8_t)((g + base_g) / 2);
                b = (uint8_t)((b + base_b) / 2);
                /* Dense packing (stride = w) - matches the decoders and
                 * what the display functions expect. */
                uint64_t off = (y * w + x) * 4;
                viewer_state.display_buffer[off + 0] = b;
                viewer_state.display_buffer[off + 1] = g;
                viewer_state.display_buffer[off + 2] = r;
                viewer_state.display_buffer[off + 3] = 255;
            }
        }
        serial_puts("[DBG] fallback: pattern fill done\n");

        viewer_state.current_width = w;
        viewer_state.current_height = h;
        viewer_state.components = comps;
        viewer_state.src_width = w;
        viewer_state.src_height = h;
        viewer_state.exif_orientation = 1;
        viewer_state.source_kind = JPEG_SOURCE_PATTERN;
        jpeg_viewer_mark_loaded();
        return JPEG_ERROR_OK;
    }

    serial_puts("[JPEG] Unrecognized image format\n");
    reset_viewer_state_for_failure();
    return JPEG_ERROR_UNSUPPORTED;
}

int jpeg_viewer_load_file(const char* file_path) {
    return jpeg_viewer_load(file_path);
}

/* ============================================================================
   Display

   NOTE: display_buffer only holds *real* decoded pixels when
   jpeg_viewer_get_source_kind() reports JPEG_SOURCE_REAL (JPEG, PNG, BMP,
   WebP).  For JPEG_SOURCE_PATTERN (a PNG that failed to decode) it holds a
   placeholder gradient, not the actual image content - callers that care about the
   distinction (vs. just wanting *something* to paint) should check.
   ========================================================================== */

/* display_buffer is BGRA bytes (see the struct field comment above);
 * gfx_blit's surfaces are 0x00RRGGBB uint32_t. This is a reusable
 * scratch buffer for that conversion, sized for the same worst case
 * as display_buffer itself and lazily allocated the same way, so
 * jpeg_viewer_display()/jpeg_viewer_draw_scaled() can hand gfx_blit a
 * real source surface without a per-call malloc/free. */
static uint32_t* s_jpeg_convert_scratch = NULL;
static uint64_t s_jpeg_convert_scratch_bytes = 0;

/* Converts the top-left w x h region of display_buffer into
 * s_jpeg_convert_scratch and returns a surface over it, or a
 * zeroed/NULL surface on failure (gfx_blit's own NULL-pixels check
 * makes that a safe no-op for callers). */
static gfx_surface_t jpeg_viewer_convert_to_surface(uint64_t w, uint64_t h) {
    gfx_surface_t empty = gfx_surface_make(NULL, 0, 0, 0);
    if (!viewer_state.display_buffer || w == 0 || h == 0) return empty;

    uint64_t pixel_count = w * h;
    uint64_t max_pixels = (uint64_t)JPEG_MAX_WIDTH * (uint64_t)JPEG_MAX_HEIGHT;
    if (pixel_count == 0 || pixel_count > max_pixels) return empty;

    /* The old path always reserved an additional 1920x1080x4 conversion
     * surface (about 8 MiB), even for small files such as the bundled
     * 644x347 JPEG. With NetSurf's cache resident this exhausted the kernel
     * heap on every preview repaint. Allocate only what the current image
     * requires and retain it for reuse. */
    uint64_t required_bytes = pixel_count * sizeof(uint32_t);
    if (!s_jpeg_convert_scratch || required_bytes > s_jpeg_convert_scratch_bytes) {
        uint32_t* replacement = (uint32_t*)kmalloc((size_t)required_bytes);
        if (!replacement) return empty;
        if (s_jpeg_convert_scratch) kfree(s_jpeg_convert_scratch);
        s_jpeg_convert_scratch = replacement;
        s_jpeg_convert_scratch_bytes = required_bytes;
    }

    const uint8_t* src = viewer_state.display_buffer;
    for (uint64_t i = 0; i < pixel_count; ++i) {
        uint64_t src_idx = i * 4;
        if (src_idx + 4 > JPEG_BUFFER_SIZE) break;
        uint8_t b = src[src_idx + 0];
        uint8_t g = src[src_idx + 1];
        uint8_t r = src[src_idx + 2];
        s_jpeg_convert_scratch[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
    return gfx_surface_make(s_jpeg_convert_scratch, (int)w, (int)h, (int)w);
}

int jpeg_viewer_display(void) {
    if (!viewer_state.image_loaded) {
        serial_puts("[JPEG] No image loaded\n");
        return -1;
    }

    uint64_t w = viewer_state.current_width;
    uint64_t h = viewer_state.current_height;

    if (!backbuffer) return -1;
    gfx_surface_t src = jpeg_viewer_convert_to_surface(w, h);
    if (vga_gpu_draw_image(&viewer_state, (const uint32_t*)src.pixels, (int)w, (int)h, (int)w,
                           s_jpeg_load_generation, false, 0, 0)) {
        return 0;
    }
    gfx_surface_t dst = gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
    gfx_blit(&dst, 0, 0, &src, 0, 0, (int)w, (int)h, GFX_BLIT_COPY, 0);
    return 0;
}

int jpeg_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    if (!viewer_state.image_loaded || !viewer_state.display_buffer) return -1;
    if (width == 0 || height == 0) return -1;

    uint64_t src_w = viewer_state.current_width;
    uint64_t src_h = viewer_state.current_height;
    if (src_w == 0 || src_h == 0) return -1;

    if (!backbuffer) return -1;

    /* Fit the image inside (width, height) preserving its aspect ratio,
     * then center it there, instead of stretching it to exactly fill
     * the box. Both callers (the standalone Image Viewer window and
     * EFM's file-preview pane) already paint the full box with a
     * suitable background before calling this, so the resulting
     * letterbox/pillarbox bars show that background rather than
     * needing a separate fill here. Without this, any photo whose
     * aspect ratio doesn't happen to match the viewer's current
     * window/pane shape - i.e. nearly every real photo - appears
     * visibly squashed or stretched. */
    uint64_t fit_w = width;
    uint64_t fit_h = (src_h * width) / src_w;
    if (fit_h > height) {
        fit_h = height;
        fit_w = (src_w * height) / src_h;
    }
    if (fit_w == 0) fit_w = 1;
    if (fit_h == 0) fit_h = 1;
    if (fit_w > width) fit_w = width;
    if (fit_h > height) fit_h = height;
    uint64_t fit_x = x + (width - fit_w) / 2;
    uint64_t fit_y = y + (height - fit_h) / 2;

    gfx_surface_t src = jpeg_viewer_convert_to_surface(src_w, src_h);
    if (vga_gpu_draw_image_scaled(&viewer_state, (const uint32_t*)src.pixels, (int)src_w, (int)src_h, (int)src_w,
                                  s_jpeg_load_generation, false,
                                  (int)fit_x, (int)fit_y, (int)fit_w, (int)fit_h)) {
        return 0;
    }
    gfx_surface_t dst = gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
    gfx_blit_scaled(&dst, (int)fit_x, (int)fit_y, (int)fit_w, (int)fit_h,
                     &src, 0, 0, (int)src_w, (int)src_h, GFX_BLIT_COPY, 0);
    return 0;
}

/* Ring-3 Photo.c-os uses this same decoder path so the legacy Image Viewer and
 * Photo.c-os cannot drift apart in format support.  The internal decoder keeps
 * BGRA bytes; the user window API uses packed 0x00RRGGBB pixels. */
int jpeg_viewer_decode_file_to_rgb(const char *filename, uint32_t *out,
                                   uint64_t out_capacity, uint64_t *out_width,
                                   uint64_t *out_height) {
    if (!filename || !out) return -1;
    int rc = jpeg_viewer_load(filename);
    if (rc != 0 || !viewer_state.image_loaded) return -1;
    uint64_t w = viewer_state.current_width, h = viewer_state.current_height;
    uint64_t need = w * h * sizeof(uint32_t);
    if (w == 0 || h == 0 || need > out_capacity) return -2;
    const uint8_t *src = viewer_state.display_buffer;
    for (uint64_t i = 0; i < w * h; ++i) {
        uint8_t b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        out[i] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    if (out_width) *out_width = w;
    if (out_height) *out_height = h;
    return 0;
}

/* ============================================================================
   Status / accessors
   ========================================================================== */
bool jpeg_viewer_is_loaded(void) {
    return viewer_state.image_loaded;
}

void jpeg_viewer_get_dimensions(uint64_t* out_width, uint64_t* out_height) {
    if (out_width) *out_width = viewer_state.current_width;
    if (out_height) *out_height = viewer_state.current_height;
}

int jpeg_viewer_get_info(uint64_t* width, uint64_t* height, uint8_t* components) {
    if (!viewer_state.image_loaded) return -1;
    if (width) *width = viewer_state.current_width;
    if (height) *height = viewer_state.current_height;
    if (components) *components = viewer_state.components;
    return 0;
}

const char* jpeg_viewer_get_filename(void) {
    return viewer_state.current_filename;
}

int jpeg_viewer_get_source_kind(void) {
    return viewer_state.source_kind;
}

/* Size of the picture as stored in the file (before shrinking to the display
 * buffer).  Returns -1 if nothing is loaded. */
int jpeg_viewer_get_source_size(uint64_t* width, uint64_t* height) {
    if (!viewer_state.image_loaded) return -1;
    if (width) *width = viewer_state.src_width;
    if (height) *height = viewer_state.src_height;
    return 0;
}

/* EXIF orientation (1..8) found in the loaded JPEG; 1 for everything else.
 * The pixels in the display buffer are already rotated/flipped accordingly. */
int jpeg_viewer_get_exif_orientation(void) {
    return viewer_state.image_loaded ? viewer_state.exif_orientation : 1;
}

/* ============================================================================
   image_viewer_* compatibility shims

   Several call sites across the GUI (fm_open_image_viewer()'s double-click
   handler, the file manager's grid-view thumbnail generator, the settings
   wallpaper picker) were written against an "image_viewer_*" API that was
   never actually implemented anywhere - only ever declared as weak externs,
   so every one of those call sites has always silently taken its failure
   branch. These thin wrappers connect them to the real jpeg_viewer_*
   implementation above.
   ========================================================================== */
int image_viewer_load_file(const char* file_path) {
    return jpeg_viewer_load_file(file_path);
}

int image_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height) {
    return jpeg_viewer_draw_scaled(x, y, width, height);
}

bool image_viewer_is_loaded(void) {
    return jpeg_viewer_is_loaded();
}

const char* image_viewer_get_filename(void) {
    return jpeg_viewer_get_filename();
}

/* Packed 0xAARRGGBB pixels, matching what callers (efm_thumbnail_preview.c)
 * expect. display_buffer is stored as BGRA bytes (B,G,R,A in that memory
 * order); reinterpreting 4 such bytes as a little-endian uint32_t on this
 * x86-64 target naturally produces (A<<24)|(R<<16)|(G<<8)|B - exactly that
 * format - so no repacking is needed, just a cast. */
const uint32_t* image_viewer_get_buffer(void) {
    if (!viewer_state.image_loaded || !viewer_state.display_buffer) return NULL;
    return (const uint32_t*)viewer_state.display_buffer;
}

uint64_t image_viewer_get_buffer_width(void) {
    return viewer_state.current_width;
}

uint64_t image_viewer_get_buffer_height(void) {
    return viewer_state.current_height;
}
