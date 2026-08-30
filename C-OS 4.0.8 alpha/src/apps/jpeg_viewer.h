#ifndef JPEG_VIEWER_H
#define JPEG_VIEWER_H

#include <stdint.h>
#include <stdbool.h>

/* JPEG Viewer - Simple Image Viewer for C-OS 4.0.8 alpha
 * Provides unified interface for JPEG image viewing
 */

/* JPEG Viewer state structure */
typedef struct {
    char current_file[256];
    bool image_loaded;
    bool fullscreen;
    int window_id;
    int zoom_level;
    uint64_t image_width;
    uint64_t image_height;
    uint64_t display_width;
    uint64_t display_height;
} jpeg_viewer_t;

/* JPEG Viewer API functions */
int jpeg_viewer_init(void);
int jpeg_viewer_load_file(const char* file_path);
/* Non-blocking variant: starts the load on a background kernel thread
 * and returns immediately. Poll jpeg_viewer_is_loading() until it
 * returns false, then check jpeg_viewer_is_loaded()/get_info() for the
 * result. Returns 0 if the load was started, -1 if one was already in
 * progress. */
int jpeg_viewer_load_async(const char* file_path);
bool jpeg_viewer_is_loading(void);
int jpeg_viewer_draw_scaled(uint64_t x, uint64_t y, uint64_t width, uint64_t height);
int jpeg_viewer_cleanup(void);
int jpeg_viewer_show_window(void);
int jpeg_viewer_zoom_in(void);
int jpeg_viewer_zoom_out(void);
int jpeg_viewer_reset_zoom(void);
int jpeg_viewer_toggle_fullscreen(void);
int jpeg_viewer_get_status(jpeg_viewer_t* status);
int jpeg_viewer_show_info(void);
bool jpeg_viewer_is_loaded(void);
const char* jpeg_viewer_get_filename(void);
int jpeg_viewer_get_info(uint64_t* width, uint64_t* height, uint8_t* components);

/* Decode a complete baseline JPEG already resident in memory into caller-owned
 * BGRA storage.  This has no file-system or global-viewer-state side effects,
 * so NetSurf image contents can use the same real decoder safely.  Progressive
 * and arithmetic-coded JPEGs return an error rather than producing a fake
 * image. */
int jpeg_decode_memory_to_bgra(const uint8_t* data, uint64_t size,
                               uint8_t* out_bgra,
                               uint64_t max_width, uint64_t max_height,
                               uint64_t* out_width, uint64_t* out_height,
                               uint8_t* out_components);

/* What's actually sitting in the display buffer right now. Callers
 * (window painting code) should check this before treating the buffer
 * contents as a faithful rendering of the file - JPEG_SOURCE_PATTERN
 * means the format wasn't decodable and a placeholder gradient was
 * drawn instead so the viewer doesn't just show a blank window. */
typedef enum {
    JPEG_SOURCE_NONE = 0,    /* nothing loaded */
    JPEG_SOURCE_REAL = 1,    /* display_buffer holds real decoded pixels */
    JPEG_SOURCE_PATTERN = 2  /* display_buffer holds a placeholder pattern */
} jpeg_source_kind_t;

int jpeg_viewer_get_source_kind(void);
/* Result of the most recently completed jpeg_viewer_load()/load_async()
 * call (a JPEG_ERROR_* code). Useful after jpeg_viewer_is_loading()
 * goes back to false to see whether the async load actually succeeded. */
int jpeg_viewer_get_last_result(void);

/* Module interface declaration */
// Module interface declaration removed

#endif
