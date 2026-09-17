/*
 * gui_renderer.c - Enhanced GUI Rendering System
 * Optimized drawing routines with double buffering and hardware acceleration
 */

#include "gui.h"
#include "vga.h"
#include "memory.h"
#include "string.h"
#include "module_interface.h"
#include "serial.h"
#include <stdarg.h>
#include <stdint.h>
#include "timer.h"
#include "smp.h"
#include "task.h"

/* External memory/time API from the kernel */
extern void* cos_mem_alloc(uint64_t size, uint8_t type);
extern void  cos_mem_free(void* ptr);
extern uint64_t cos_system_get_time(void);

uint64_t cos_system_get_time(void) {
    return get_timer_ticks();
}

// API stubs
#define COS_API_OK 0
#define COS_API_NO_MEMORY -1
#define COS_API_ERROR -2
#define MODULE_TYPE_STORAGE 1
#define MODULE_STATUS_OK 0
#define COS_MEM_TYPE_USER 0

static void gui_renderer_log(const char* level, const char* module, const char* message) {
    serial_puts("[");
    serial_puts(level);
    serial_puts("] ");
    serial_puts(module ? module : "GUI_RENDERER");
    if (message) {
        serial_puts(": ");
        serial_puts(message);
    }
    serial_puts("\n");
}

void API_LOG_ERROR(const char* module, const char* operation, int error) {
    (void)operation;
    (void)error;
    gui_renderer_log("ERROR", module, "operation failed");
}

void API_LOG_DEBUG(const char* module, const char* message) {
    gui_renderer_log("DEBUG", module, message);
}

void API_LOG_INFO(const char* module, const char* message) {
    gui_renderer_log("INFO", module, message);
}

// String function stubs
static int gui_renderer_atoi(const char* s) {
    int sign = 1;
    int result = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') { result = result * 10 + (*s - '0'); s++; }
    return sign * result;
}

static void gui_renderer_format_u64(uint64_t value, char* out) {
    char tmp[32];
    int i = 0;
    if (value == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (value > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    for (int j = 0; j < i; j++) out[j] = tmp[i - 1 - j];
    out[i] = '\0';
}

static int gui_renderer_snprintf(char* str, size_t size, const char* format, ...) {
    if (!str || size == 0) return 0;
    va_list args;
    va_start(args, format);
    char* out = str;
    const char* end = str + size - 1;
    while (format && *format && out < end) {
        if (*format == '%') {
            format++;
            if (!*format) break;
            char numbuf[32];
            switch (*format) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    if (!s) s = "(null)";
                    while (*s && out < end) *out++ = *s++;
                    break;
                }
                case 'd': {
                    int n = va_arg(args, int);
                    if (n < 0 && out < end) { *out++ = '-'; n = -n; }
                    gui_renderer_format_u64((uint64_t)n, numbuf);
                    for (const char* p = numbuf; *p && out < end; ++p) *out++ = *p;
                    break;
                }
                case 'u': {
                    uint64_t n = va_arg(args, uint64_t);
                    gui_renderer_format_u64(n, numbuf);
                    for (const char* p = numbuf; *p && out < end; ++p) *out++ = *p;
                    break;
                }
                case '%':
                    *out++ = '%';
                    break;
                default:
                    *out++ = '%';
                    if (out < end) *out++ = *format;
                    break;
            }
            format++;
        } else {
            *out++ = *format++;
        }
    }
    *out = '\0';
    va_end(args);
    return (int)(out - str);
}


/* Screen dimensions */
#define SCREEN_WIDTH    ((uint64_t)SCREEN_W)
#define SCREEN_HEIGHT   ((uint64_t)SCREEN_H)
#define SCREEN_SIZE      ((size_t)(SCREEN_WIDTH * SCREEN_HEIGHT))

/* Rendering modes */
#define RENDER_MODE_SOFTWARE    0
#define RENDER_MODE_HARDWARE    1
#define RENDER_MODE_DOUBLE_BUF  2

/* Drawing operations */
typedef struct {
    uint8_t  type;      /* 0=fill, 1=line, 2=rect, 3=text, 4=image */
    uint64_t x, y;
    uint64_t width, height;
    uint64_t color;
    void*    data;
    bool     dirty;
} draw_op_t;

/* Render buffer structure */
typedef struct {
    uint32_t* buffer; /* XRGB8888: one aligned 32-bit pixel per sample */
    uint64_t  width;
    uint64_t  height;
    uint64_t  size;
    bool      dirty;
    int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
} render_buffer_t;

/* Renderer state */
static struct {
    render_buffer_t front_buffer;
    render_buffer_t back_buffer;
    render_buffer_t* active_buffer;
    render_buffer_t* display_buffer;
    
    draw_op_t draw_queue[1024];
    int draw_queue_count;
    
    uint8_t render_mode;
    bool vsync_enabled;
    uint64_t frame_count;
    uint64_t last_frame_time;
    uint64_t fps;
    
    /* Clipping */
    uint64_t clip_x, clip_y, clip_w, clip_h;
    bool clipping_enabled;
} renderer = {0};

/* A tile job owns a disjoint set of rows. Workers never touch renderer's
 * clipping state or dirty accumulator; the GUI/BSP marks the combined region
 * after all submitted jobs complete. This avoids races while still allowing
 * large fills and future image/tile conversion work to use idle APs. */
typedef struct {
    uint32_t *buffer;
    uint64_t y;
    uint64_t height;
    uint64_t x;
    uint64_t width;
    uint32_t color;
} gui_fill_tile_job_t;

static void gui_fill_tile_worker(void *opaque) {
    gui_fill_tile_job_t *job = (gui_fill_tile_job_t *)opaque;
    if (!job || !job->buffer || job->width == 0 || job->height == 0) return;
    for (uint64_t row = 0; row < job->height; ++row) {
        uint32_t *dst = job->buffer + (job->y + row) * SCREEN_WIDTH + job->x;
        for (uint64_t col = 0; col < job->width; ++col) dst[col] = job->color;
    }
}

static bool gui_renderer_fill_rect_parallel(uint64_t x, uint64_t y,
                                             uint64_t width, uint64_t height,
                                             uint32_t color) {
    uint32_t online = smp_online_cpu_count();
    if (online < 2u || width * height < 16384u || !renderer.active_buffer ||
        !renderer.active_buffer->buffer) return false;

    uint32_t workers = online > 8u ? 8u : online;
    if (workers > height) workers = (uint32_t)height;
    if (workers < 2u) return false;

    gui_fill_tile_job_t tiles[8];
    smp_background_job_t jobs[8];
    uint32_t submitted = 0;
    uint64_t base = height / workers;
    uint64_t extra = height % workers;
    uint64_t cursor = y;

    /* Build one disjoint horizontal stripe for every online logical CPU.
     * Tile 0 belongs to the BSP; the remaining stripes are offered to idle
     * APs. This gives SMP2=2 concurrent stripes, SMP4=4, and SMP8=8 whenever
     * all APs are idle, rather than making the BSP wait while only APs draw. */
    for (uint32_t i = 0; i < workers; ++i) {
        uint64_t tile_height = base + (i < extra ? 1u : 0u);
        tiles[i].buffer = renderer.active_buffer->buffer;
        tiles[i].x = x;
        tiles[i].y = cursor;
        tiles[i].width = width;
        tiles[i].height = tile_height;
        tiles[i].color = color;
        cursor += tile_height;
        smp_background_job_init(&jobs[i], gui_fill_tile_worker, &tiles[i],
                                renderer.frame_count, 0,
                                SMP_WORK_PRIORITY_NORMAL);
    }

    /* Queue AP work first, then let the GUI/BSP render its own stripe while
     * the AP workers run. Queue pressure falls back safely to BSP execution
     * for that individual stripe; no task writes an overlapping row range. */
    for (uint32_t i = 1; i < workers; ++i) {
        if (smp_submit_background_job(&jobs[i])) {
            ++submitted;
        } else {
            gui_fill_tile_worker(&tiles[i]);
        }
    }
    gui_fill_tile_worker(&tiles[0]);

    for (uint32_t i = 1; i < workers; ++i) {
        if (__atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) !=
            SMP_BACKGROUND_JOB_QUEUED &&
            __atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE) !=
            SMP_BACKGROUND_JOB_RUNNING) continue;
        while (!smp_background_job_is_done(&jobs[i])) thread_yield();
    }
    (void)submitted;
    return true;
}

static void renderer_mark_dirty(render_buffer_t *buffer, int x, int y, int w, int h)
{
    if (buffer == NULL || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)SCREEN_W) x1 = (int)SCREEN_W;
    int y1 = y + h; if (y1 > (int)SCREEN_H) y1 = (int)SCREEN_H;
    if (x1 <= x0 || y1 <= y0) return;
    if (!buffer->dirty) {
        buffer->dirty_x0 = x0; buffer->dirty_y0 = y0;
        buffer->dirty_x1 = x1; buffer->dirty_y1 = y1;
        buffer->dirty = true;
        return;
    }
    if (x0 < buffer->dirty_x0) buffer->dirty_x0 = x0;
    if (y0 < buffer->dirty_y0) buffer->dirty_y0 = y0;
    if (x1 > buffer->dirty_x1) buffer->dirty_x1 = x1;
    if (y1 > buffer->dirty_y1) buffer->dirty_y1 = y1;
}

/* Initialize renderer */
int gui_renderer_init(void) {
    API_LOG_INFO("GUI_RENDERER", "Initializing enhanced GUI renderer");
    
    /* Allocate front buffer */
    renderer.front_buffer.buffer = (uint32_t*)cos_mem_alloc(SCREEN_SIZE * sizeof(uint32_t), COS_MEM_TYPE_USER);
    if (!renderer.front_buffer.buffer) {
        API_LOG_ERROR("GUI_RENDERER", "front_buffer alloc", COS_API_NO_MEMORY);
        return COS_API_NO_MEMORY;
    }
    
    renderer.front_buffer.width = SCREEN_WIDTH;
    renderer.front_buffer.height = SCREEN_HEIGHT;
    renderer.front_buffer.size = SCREEN_SIZE * sizeof(uint32_t);
    renderer.front_buffer.dirty = false;
    
    /* Allocate back buffer for double buffering */
    renderer.back_buffer.buffer = (uint32_t*)cos_mem_alloc(SCREEN_SIZE * sizeof(uint32_t), COS_MEM_TYPE_USER);
    if (!renderer.back_buffer.buffer) {
        API_LOG_ERROR("GUI_RENDERER", "back_buffer alloc", COS_API_NO_MEMORY);
        cos_mem_free(renderer.front_buffer.buffer);
        return COS_API_NO_MEMORY;
    }
    
    renderer.back_buffer.width = SCREEN_WIDTH;
    renderer.back_buffer.height = SCREEN_HEIGHT;
    renderer.back_buffer.size = SCREEN_SIZE * sizeof(uint32_t);
    renderer.back_buffer.dirty = false;
    
    /* Set active buffers */
    renderer.active_buffer = &renderer.back_buffer;
    renderer.display_buffer = &renderer.front_buffer;
    
    /* Initialize state */
    renderer.draw_queue_count = 0;
    renderer.render_mode = RENDER_MODE_DOUBLE_BUF;
    renderer.vsync_enabled = true;
    renderer.frame_count = 0;
    renderer.last_frame_time = cos_system_get_time();
    renderer.fps = 60;
    
    /* Initialize clipping */
    renderer.clip_x = 0;
    renderer.clip_y = 0;
    renderer.clip_w = SCREEN_WIDTH;
    renderer.clip_h = SCREEN_HEIGHT;
    renderer.clipping_enabled = false;
    /* Both software surfaces begin as the same XRGB frame. Thereafter present
     * copies only the changed rectangle back to the next draw surface. */
    memset(renderer.front_buffer.buffer, 0, renderer.front_buffer.size);
    memset(renderer.back_buffer.buffer, 0, renderer.back_buffer.size);
    renderer_mark_dirty(renderer.active_buffer, 0, 0, (int)SCREEN_W, (int)SCREEN_H);
    
    API_LOG_INFO("GUI_RENDERER", "Renderer initialized successfully");
    return COS_API_OK;
}

/* Cleanup renderer */
int gui_renderer_cleanup(void) {
    API_LOG_INFO("GUI_RENDERER", "Cleaning up GUI renderer");
    
    if (renderer.front_buffer.buffer) {
        cos_mem_free(renderer.front_buffer.buffer);
        renderer.front_buffer.buffer = NULL;
    }
    
    if (renderer.back_buffer.buffer) {
        cos_mem_free(renderer.back_buffer.buffer);
        renderer.back_buffer.buffer = NULL;
    }
    
    renderer.draw_queue_count = 0;
    
    return COS_API_OK;
}

/* Set clipping rectangle */
void gui_renderer_set_clip(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    renderer.clip_x = x;
    renderer.clip_y = y;
    renderer.clip_w = w;
    renderer.clip_h = h;
    renderer.clipping_enabled = true;
}

/* Reset clipping */
void gui_renderer_reset_clip(void) {
    renderer.clip_x = 0;
    renderer.clip_y = 0;
    renderer.clip_w = SCREEN_WIDTH;
    renderer.clip_h = SCREEN_HEIGHT;
    renderer.clipping_enabled = false;
}

/* Check if point is within clipping bounds */
static inline bool is_clipped(uint64_t x, uint64_t y) {
    if (!renderer.clipping_enabled) return false;
    
    return (x < renderer.clip_x || x >= renderer.clip_x + renderer.clip_w ||
            y < renderer.clip_y || y >= renderer.clip_y + renderer.clip_h);
}

/* Optimized pixel drawing with clipping */
static inline void draw_pixel_clipped(uint64_t x, uint64_t y, uint64_t color) {
    if (is_clipped(x, y)) return;
    
    if (x < SCREEN_WIDTH && y < SCREEN_HEIGHT) {
        renderer.active_buffer->buffer[y * SCREEN_WIDTH + x] = (uint32_t)color;
        renderer_mark_dirty(renderer.active_buffer, (int)x, (int)y, 1, 1);
    }
}

/* Optimized horizontal line drawing */
static void draw_hline_optimized(uint64_t x, uint64_t y, uint64_t width, uint64_t color) {
    if (y >= SCREEN_HEIGHT) return;
    
    uint64_t start_x = x;
    uint64_t end_x = x + width;
    
    /* Apply clipping */
    if (renderer.clipping_enabled) {
        if (y < renderer.clip_y || y >= renderer.clip_y + renderer.clip_h) return;
        if (start_x < renderer.clip_x) start_x = renderer.clip_x;
        if (end_x > renderer.clip_x + renderer.clip_w) end_x = renderer.clip_x + renderer.clip_w;
    }
    
    if (start_x >= end_x || start_x >= SCREEN_WIDTH) return;
    if (end_x > SCREEN_WIDTH) end_x = SCREEN_WIDTH;
    
    /* Draw line using optimized memory fill */
    uint32_t* pixel_ptr = &renderer.active_buffer->buffer[y * SCREEN_WIDTH + start_x];
    uint64_t count = end_x - start_x;
    
    for (uint64_t i = 0; i < count; i++) {
        pixel_ptr[i] = (uint32_t)color;
    }
    renderer_mark_dirty(renderer.active_buffer, (int)start_x, (int)y,
                        (int)count, 1);
}

/* Optimized vertical line drawing */
static void draw_vline_optimized(uint64_t x, uint64_t y, uint64_t height, uint64_t color) {
    if (x >= SCREEN_WIDTH) return;
    
    uint64_t start_y = y;
    uint64_t end_y = y + height;
    
    /* Apply clipping */
    if (renderer.clipping_enabled) {
        if (x < renderer.clip_x || x >= renderer.clip_x + renderer.clip_w) return;
        if (start_y < renderer.clip_y) start_y = renderer.clip_y;
        if (end_y > renderer.clip_y + renderer.clip_h) end_y = renderer.clip_y + renderer.clip_h;
    }
    
    if (start_y >= end_y || start_y >= SCREEN_HEIGHT) return;
    if (end_y > SCREEN_HEIGHT) end_y = SCREEN_HEIGHT;
    
    /* Draw line */
    for (uint64_t py = start_y; py < end_y; py++) {
        renderer.active_buffer->buffer[py * SCREEN_WIDTH + x] = (uint32_t)color;
    }
    renderer_mark_dirty(renderer.active_buffer, (int)x, (int)start_y, 1,
                        (int)(end_y - start_y));
}

/* Optimized rectangle drawing */
void gui_renderer_draw_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint64_t color, bool filled) {
    if (filled) {
        uint64_t x0 = x, y0 = y, x1 = x + width, y1 = y + height;
        if (renderer.clipping_enabled) {
            if (x0 < renderer.clip_x) x0 = renderer.clip_x;
            if (y0 < renderer.clip_y) y0 = renderer.clip_y;
            if (x1 > renderer.clip_x + renderer.clip_w) x1 = renderer.clip_x + renderer.clip_w;
            if (y1 > renderer.clip_y + renderer.clip_h) y1 = renderer.clip_y + renderer.clip_h;
        }
        if (x1 > SCREEN_WIDTH) x1 = SCREEN_WIDTH;
        if (y1 > SCREEN_HEIGHT) y1 = SCREEN_HEIGHT;
        if (x1 > x0 && y1 > y0 &&
            !gui_renderer_fill_rect_parallel(x0, y0, x1 - x0, y1 - y0, (uint32_t)color)) {
            for (uint64_t py = y0; py < y1; ++py)
                draw_hline_optimized(x0, py, x1 - x0, color);
        } else if (x1 > x0 && y1 > y0) {
            renderer_mark_dirty(renderer.active_buffer, (int)x0, (int)y0,
                                (int)(x1 - x0), (int)(y1 - y0));
        }
    } else {
        /* Outline rectangle */
        draw_hline_optimized(x, y, width, color);
        draw_hline_optimized(x, y + height - 1, width, color);
        draw_vline_optimized(x, y, height, color);
        draw_vline_optimized(x + width - 1, y, height, color);
    }
}

/* Optimized gradient rectangle */
void gui_renderer_draw_gradient_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, 
                                   uint64_t color_top, uint64_t color_bottom) {
    for (uint64_t py = y; py < y + height; py++) {
        /* Calculate gradient color */
        uint64_t factor = (py - y) * 256 / height;
        
        uint8_t r_top = (color_top >> 16) & 0xFF;
        uint8_t g_top = (color_top >> 8) & 0xFF;
        uint8_t b_top = color_top & 0xFF;
        
        uint8_t r_bot = (color_bottom >> 16) & 0xFF;
        uint8_t g_bot = (color_bottom >> 8) & 0xFF;
        uint8_t b_bot = color_bottom & 0xFF;
        
        uint8_t r = r_top + ((r_bot - r_top) * factor) / 256;
        uint8_t g = g_top + ((g_bot - g_top) * factor) / 256;
        uint8_t b = b_top + ((b_bot - b_top) * factor) / 256;
        
        uint64_t gradient_color = (r << 16) | (g << 8) | b;
        
        draw_hline_optimized(x, py, width, gradient_color);
    }
}

/* Optimized circle drawing */
void gui_renderer_draw_circle(uint64_t cx, uint64_t cy, uint64_t radius, uint64_t color, bool filled) {
    int64_t x = radius;
    int64_t y = 0;
    int64_t err = 0;
    
    while (x >= y) {
        if (filled) {
            /* Draw horizontal lines for filled circle */
            draw_hline_optimized(cx - x, cy + y, x * 2 + 1, color);
            draw_hline_optimized(cx - x, cy - y, x * 2 + 1, color);
            draw_hline_optimized(cx - y, cy + x, y * 2 + 1, color);
            draw_hline_optimized(cx - y, cy - x, y * 2 + 1, color);
        } else {
            /* Draw outline */
            draw_pixel_clipped(cx + x, cy + y, color);
            draw_pixel_clipped(cx - x, cy + y, color);
            draw_pixel_clipped(cx + x, cy - y, color);
            draw_pixel_clipped(cx - x, cy - y, color);
            draw_pixel_clipped(cx + y, cy + x, color);
            draw_pixel_clipped(cx - y, cy + x, color);
            draw_pixel_clipped(cx + y, cy - x, color);
            draw_pixel_clipped(cx - y, cy - x, color);
        }
        
        if (err <= 0) {
            y += 1;
            err += 2 * y + 1;
        }
        
        if (err > 0) {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
}

/* Clear screen with color */
void gui_renderer_clear_screen(uint64_t color) {
    uint32_t* buffer = renderer.active_buffer->buffer;
    uint64_t count = SCREEN_SIZE;
    
    /* Use optimized memory fill */
    for (uint64_t i = 0; i < count; i++) {
        buffer[i] = (uint32_t)color;
    }
    
    renderer_mark_dirty(renderer.active_buffer, 0, 0, (int)SCREEN_W, (int)SCREEN_H);
}

/* Swap buffers for double buffering */
void gui_renderer_swap_buffers(void) {
    if (renderer.render_mode != RENDER_MODE_DOUBLE_BUF) return;
    
    /* Wait for VSync if enabled */
    if (renderer.vsync_enabled) {
        vga_wait_vblank();
    }
    
    /* Swap buffers */
    render_buffer_t* temp = renderer.active_buffer;
    renderer.active_buffer = renderer.display_buffer;
    renderer.display_buffer = temp;
    
    /* Update frame counter and FPS */
    renderer.frame_count++;
    uint64_t current_time = cos_system_get_time();
    if (current_time - renderer.last_frame_time >= 1000) {
        renderer.fps = renderer.frame_count;
        renderer.frame_count = 0;
        renderer.last_frame_time = current_time;
        
        char fps_buf[32];
        unsigned long long fps_val = (unsigned long long)renderer.fps;
        int pos = 0;
        if (fps_val == 0) {
            fps_buf[pos++] = '0';
        } else {
            char tmp[32];
            int t = 0;
            while (fps_val > 0 && t < (int)sizeof(tmp)) {
                tmp[t++] = (char)('0' + (fps_val % 10ULL));
                fps_val /= 10ULL;
            }
            while (t > 0) fps_buf[pos++] = tmp[--t];
        }
        fps_buf[pos] = '\0';
        API_LOG_DEBUG("GUI_RENDERER", fps_buf);
    }
}

/* Present buffer to screen */
void gui_renderer_present(void) {
    if (!renderer.display_buffer || !renderer.display_buffer->dirty) return;
    if (!vga_has_framebuffer()) {
        renderer.display_buffer->dirty = false;
        return;
    }

    uint32_t* src_buffer = renderer.display_buffer->buffer;
    if (!src_buffer) {
        renderer.display_buffer->dirty = false;
        return;
    }

    int x = renderer.display_buffer->dirty_x0;
    int y = renderer.display_buffer->dirty_y0;
    int w = renderer.display_buffer->dirty_x1 - x;
    int h = renderer.display_buffer->dirty_y1 - y;
    if (w <= 0 || h <= 0) { renderer.display_buffer->dirty = false; return; }

    /* One stride-aware BitBlt to the canonical VGA backbuffer, then only the
     * same rectangle to VRAM. This replaces the former full-screen
     * vga_put_pixel loop and keeps CPU writes cache-local until presentation. */
    vga_copy_rect_strided(x, y, w, h,
                          src_buffer + (size_t)y * (size_t)SCREEN_W + (size_t)x,
                          (int)SCREEN_W);
    vga_flip_rect(x, y, w, h);

    /* Maintain identical front/back software surfaces so the next frame may
     * draw only its new dirty rectangle without forcing a full repaint. */
    if (renderer.active_buffer && renderer.active_buffer->buffer) {
        for (int row = 0; row < h; ++row) {
            memcpy(renderer.active_buffer->buffer + (size_t)(y + row) * (size_t)SCREEN_W + (size_t)x,
                   src_buffer + (size_t)(y + row) * (size_t)SCREEN_W + (size_t)x,
                   (size_t)w * sizeof(uint32_t));
        }
    }
    renderer.display_buffer->dirty = false;
}

/* Get renderer statistics */
void gui_renderer_get_stats(uint64_t* fps, uint64_t* frame_count) {
    if (fps) *fps = renderer.fps;
    if (frame_count) *frame_count = renderer.frame_count;
}

/* Enable/disable VSync */
void gui_renderer_set_vsync(bool enabled) {
    renderer.vsync_enabled = enabled;
}

/* Set render mode */
void gui_renderer_set_mode(uint8_t mode) {
    renderer.render_mode = mode;
}

static void gui_renderer_cleanup_wrapper(void) {
    (void)gui_renderer_cleanup();
}

static int gui_renderer_get_status(void) {
    return (renderer.front_buffer.buffer && renderer.back_buffer.buffer) ? COS_API_OK : COS_API_NO_MEMORY;
}

/* Module interface implementation */
static int gui_renderer_configure(const char* config) {

    API_LOG_INFO("GUI_RENDERER", "Configuration received");

    if (!config) {
        return COS_API_ERROR;
    }

    if (strncmp(config, "vsync:", 6) == 0) {
        bool enabled = (strcmp(config + 6, "true") == 0);
        gui_renderer_set_vsync(enabled);
        return COS_API_OK;
    }
    else if (strncmp(config, "mode:", 5) == 0) {
        uint8_t mode = (uint8_t)atoi(config + 5);
        gui_renderer_set_mode(mode);
        return COS_API_OK;
    }

    return COS_API_ERROR;
}

static const char* gui_renderer_get_info(void) {
    static char info[256];
    gui_renderer_snprintf(info, sizeof(info),
        "GUI Renderer: mode=%u, fps=%u, vsync=%s",
        renderer.render_mode, renderer.fps,
        renderer.vsync_enabled ? "enabled" : "disabled");
    return info;
}

/* Main GUI renderer module */
module_interface_t gui_renderer_module = {
    .name = "GUI Renderer",
    .version = "4.0.5",
    .type = MODULE_TYPE_GUI,
    .status = MODULE_STATUS_OK,
    .init = gui_renderer_init,
    .cleanup = gui_renderer_cleanup_wrapper,
    .get_status = gui_renderer_get_status,
    .configure = gui_renderer_configure,
    .get_info = gui_renderer_get_info,
};
