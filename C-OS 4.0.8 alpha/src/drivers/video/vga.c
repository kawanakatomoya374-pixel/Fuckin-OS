#include "vga.h"
#include "vga_font24.h"
#include "jp_font16.h"
#include "gfx_blit.h"
#include "serial.h"
#include "memory.h"
#include "mm/paging.h"
#include "memory_physical.h"
#include "io.h"
#include "smp.h"
#include "task.h"
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

/* The backbuffer, wrapped as a gfx_surface_t so every drawing
 * primitive below can go through the shared BitBlt core in
 * gfx_blit.c instead of hand-rolling its own pixel loop. Built fresh
 * on each call (cheap - it's just a small struct) rather than cached,
 * since backbuffer can be (re)allocated by vga_ensure_backbuffer(). */
static inline gfx_surface_t vga_backbuffer_surface(void) {
    return gfx_surface_make(backbuffer, (int)SCREEN_W, (int)SCREEN_H, (int)SCREEN_W);
}

extern const vga_font24_glyph_t font24x24[];
#define VGA_FONT24X24_COUNT 95

uint64_t SCREEN_W = 1024;
uint64_t SCREEN_H = 768;
uint32_t* framebuffer = NULL;
uint32_t* backbuffer = NULL;
static uint64_t current_vga_color = 0xFFFFFFFF;
static int font_scale = 1;
static int font_resolution = 1;
static uint64_t framebuffer_pitch_bytes = 0;
static uint8_t framebuffer_bpp = 0;
static uint64_t framebuffer_phys_addr = 0;
static uint64_t framebuffer_phys_size = 0;
static bool framebuffer_physical_reserved = false;

/* The GUI owner constructs the complete scene and remains the only writer of
 * window/DOM/clip/dirty metadata. Its final 32bpp BitBlt, however, consists
 * of independent destination pixels. Split that transfer into an exact grid
 * of non-overlapping tiles when AP workers are online: SMP2=1x2,
 * SMP4=2x2, SMP6=2x3, SMP8=2x4. The BSP copies tile zero and joins the AP
 * jobs before returning, so no next-frame drawing can race the presentation.
 * 24bpp conversion remains BSP-only because it is format conversion rather
 * than same-format BitBlt. */
#define VGA_PARALLEL_TILE_MAX 8u
#define VGA_PARALLEL_COPY_MIN_PIXELS 32768u
/* Kept intentionally bounded: it proves AP tile dispatch in validation logs
 * without turning normal presentation into serial-console traffic. */
static unsigned int vga_parallel_copy_trace_budget = 4u;

typedef struct {
    uint32_t *dst;
    const uint32_t *src;
    uint32_t dst_stride;
    uint32_t src_stride;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} vga_copy_tile_job_t;

static void vga_copy_tile_worker(void *opaque) {
    const vga_copy_tile_job_t *tile = (const vga_copy_tile_job_t *)opaque;
    if (tile == NULL || tile->dst == NULL || tile->src == NULL ||
        tile->width == 0 || tile->height == 0) return;
    for (uint32_t row = 0; row < tile->height; ++row) {
        uint32_t *dst = tile->dst + (size_t)(tile->y + row) * tile->dst_stride + tile->x;
        const uint32_t *src = tile->src + (size_t)(tile->y + row) * tile->src_stride + tile->x;
        memcpy(dst, src, (size_t)tile->width * sizeof(uint32_t));
    }
}

static bool vga_copy_rect_tiled(uint32_t *dst, uint32_t dst_stride,
                                const uint32_t *src, uint32_t src_stride,
                                uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
    if (dst == NULL || src == NULL || width == 0 || height == 0 ||
        (uint64_t)width * height < VGA_PARALLEL_COPY_MIN_PIXELS) return false;

    uint32_t workers = smp_online_cpu_count();
    if (workers < 2u) return false;
    if (workers > VGA_PARALLEL_TILE_MAX) workers = VGA_PARALLEL_TILE_MAX;

    /* Use the largest exact divisor no greater than sqrt(workers). This
     * preserves one tile per online CPU without uncovered pixels. */
    uint32_t cols = 1u;
    for (uint32_t candidate = 2u; candidate <= workers / candidate; ++candidate) {
        if (workers % candidate == 0u) cols = candidate;
    }
    uint32_t rows = workers / cols;
    if (cols > width || rows > height) return false;

    vga_copy_tile_job_t tiles[VGA_PARALLEL_TILE_MAX];
    smp_background_job_t jobs[VGA_PARALLEL_TILE_MAX];
    for (uint32_t i = 0; i < workers; ++i) {
        uint32_t col = i % cols;
        uint32_t row = i / cols;
        uint32_t x0 = x + (uint32_t)(((uint64_t)width * col) / cols);
        uint32_t x1 = x + (uint32_t)(((uint64_t)width * (col + 1u)) / cols);
        uint32_t y0 = y + (uint32_t)(((uint64_t)height * row) / rows);
        uint32_t y1 = y + (uint32_t)(((uint64_t)height * (row + 1u)) / rows);
        tiles[i].dst = dst;
        tiles[i].src = src;
        tiles[i].dst_stride = dst_stride;
        tiles[i].src_stride = src_stride;
        tiles[i].x = x0;
        tiles[i].y = y0;
        tiles[i].width = x1 - x0;
        tiles[i].height = y1 - y0;
        smp_background_job_init(&jobs[i], vga_copy_tile_worker, &tiles[i], 0,
                                0, SMP_WORK_PRIORITY_NORMAL);
    }

    /* APs receive disjoint tiles first. The BSP owns tile 0 and all frame
     * metadata, and safely takes any tile whose queue submission fails. */
    for (uint32_t i = 1; i < workers; ++i) {
        if (!smp_submit_background_job(&jobs[i])) {
            vga_copy_tile_worker(&tiles[i]);
        }
    }
    vga_copy_tile_worker(&tiles[0]);
    for (uint32_t i = 1; i < workers; ++i) {
        uint32_t state = __atomic_load_n(&jobs[i].state, __ATOMIC_ACQUIRE);
        if (state == SMP_BACKGROUND_JOB_QUEUED || state == SMP_BACKGROUND_JOB_RUNNING) {
            while (!smp_background_job_is_done(&jobs[i])) thread_yield();
        }
    }
    if (vga_parallel_copy_trace_budget != 0u) {
        serial_puts("[VGA/SMP] tiled BitBlt grid=");
        serial_putdec(rows);
        serial_puts("x");
        serial_putdec(cols);
        serial_puts(" tiles=");
        serial_putdec(workers);
        serial_puts(" AP CPUs=");
        for (uint32_t i = 1; i < workers; ++i) {
            serial_putdec(__atomic_load_n(&jobs[i].assigned_cpu, __ATOMIC_ACQUIRE));
            if (i + 1u < workers) serial_puts(",");
        }
        serial_puts("\n");
        --vga_parallel_copy_trace_budget;
    }
    return true;
}

bool vga_has_framebuffer(void) {
    return framebuffer != NULL && SCREEN_W > 0 && SCREEN_H > 0;
}


/* Multiboot2 framebuffer tag layout (type 8). */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
} __attribute__((packed)) multiboot2_framebuffer_tag_t;

static inline uint32_t vga_color_to_u32(uint64_t color) {
    return (uint32_t)(color & 0x00FFFFFFu);
}

static void vga_clear_framebuffer(uint32_t color32) {
    if (!framebuffer) {
        return;
    }

    size_t pitch = framebuffer_pitch_bytes ? (size_t)framebuffer_pitch_bytes
                                           : (size_t)(SCREEN_W * (framebuffer_bpp == 24 ? 3u : 4u));
    uint8_t* fb = (uint8_t*)framebuffer;

    if (framebuffer_bpp == 24) {
        uint8_t b = (uint8_t)(color32 & 0xFFu);
        uint8_t g = (uint8_t)((color32 >> 8) & 0xFFu);
        uint8_t r = (uint8_t)((color32 >> 16) & 0xFFu);
        for (uint64_t y = 0; y < SCREEN_H; ++y) {
            uint8_t* row = fb + (size_t)y * pitch;
            for (uint64_t x = 0; x < SCREEN_W; ++x) {
                size_t off = (size_t)x * 3u;
                row[off + 0] = b;
                row[off + 1] = g;
                row[off + 2] = r;
            }
        }
        return;
    }

    for (uint64_t y = 0; y < SCREEN_H; ++y) {
        uint32_t* row = (uint32_t*)(fb + (size_t)y * pitch);
        for (uint64_t x = 0; x < SCREEN_W; ++x) {
            row[x] = color32;
        }
    }
}

static void vga_write_framebuffer_pixel(int x, int y, uint32_t color32) {
    if (!framebuffer) {
        return;
    }
    if (x < 0 || y < 0 || (uint64_t)x >= SCREEN_W || (uint64_t)y >= SCREEN_H) {
        return;
    }

    uint8_t* fb = (uint8_t*)framebuffer;
    size_t pitch = framebuffer_pitch_bytes ? (size_t)framebuffer_pitch_bytes
                                           : (size_t)(SCREEN_W * (framebuffer_bpp == 24 ? 3u : 4u));
    if (framebuffer_bpp == 24) {
        size_t off = (size_t)y * pitch + (size_t)x * 3u;
        fb[off + 0] = (uint8_t)(color32 & 0xFFu);
        fb[off + 1] = (uint8_t)((color32 >> 8) & 0xFFu);
        fb[off + 2] = (uint8_t)((color32 >> 16) & 0xFFu);
    } else {
        uint32_t* row = (uint32_t*)(fb + (size_t)y * pitch);
        row[x] = color32;
    }
}

static void vga_ensure_backbuffer(void) {
    if (backbuffer || SCREEN_W == 0 || SCREEN_H == 0 || !memory_heap_ready() || !framebuffer_physical_reserved) {
        return;
    }

    size_t bytes = (size_t)(SCREEN_W * SCREEN_H * sizeof(uint32_t));
    backbuffer = (uint32_t*)kmalloc(bytes);
    if (backbuffer) {
        memset(backbuffer, 0, bytes);
    }
}

static void vga_parse_multiboot2_framebuffer(uint64_t multiboot_info_addr) {
    serial_puts("[VGA] mb2 info addr=0x");
    serial_puthex(multiboot_info_addr);
    serial_puts("\n");
    if (multiboot_info_addr == 0) {
        serial_puts("[VGA] mb2 info addr is 0, no multiboot2 info block\n");
        return;
    }

    multiboot2_info_t* info = (multiboot2_info_t*)(uintptr_t)multiboot_info_addr;
    uint32_t total_size = info->total_size;
    serial_puts("[VGA] mb2 total_size=");
    serial_putdec(total_size);
    serial_puts("\n");
    if (total_size < sizeof(multiboot2_info_t)) {
        serial_puts("[VGA] mb2 total_size too small, aborting parse\n");
        return;
    }

    uint8_t* tag_ptr = (uint8_t*)info + sizeof(multiboot2_info_t);
    uint8_t* end_ptr = (uint8_t*)info + total_size;

    while (tag_ptr + sizeof(multiboot2_tag_t) <= end_ptr) {
        multiboot2_tag_t* tag = (multiboot2_tag_t*)tag_ptr;
        serial_puts("[VGA] mb2 tag type=");
        serial_putdec(tag->type);
        serial_puts(" size=");
        serial_putdec(tag->size);
        serial_puts("\n");
        if (tag->type == 0) {
            break;
        }

        if (tag->type == 8 && tag->size >= sizeof(multiboot2_framebuffer_tag_t)) {
            multiboot2_framebuffer_tag_t* fb = (multiboot2_framebuffer_tag_t*)tag;
            serial_puts("[VGA] fb tag: addr=0x");
            serial_puthex(fb->framebuffer_addr);
            serial_puts(" pitch=");
            serial_putdec(fb->framebuffer_pitch);
            serial_puts(" w=");
            serial_putdec(fb->framebuffer_width);
            serial_puts(" h=");
            serial_putdec(fb->framebuffer_height);
            serial_puts(" bpp=");
            serial_putdec(fb->framebuffer_bpp);
            serial_puts(" fbtype=");
            serial_putdec(fb->framebuffer_type);
            serial_puts("\n");
            if (fb->framebuffer_addr != 0 && (fb->framebuffer_bpp == 32 || fb->framebuffer_bpp == 24)) {
                framebuffer_phys_addr = fb->framebuffer_addr;
                framebuffer_phys_size = (uint64_t)fb->framebuffer_pitch * (uint64_t)fb->framebuffer_height;
                if (framebuffer_phys_size == 0) {
                    framebuffer_phys_addr = 0;
                    framebuffer = NULL;
                    serial_puts("[VGA] fb tag REJECTED (zero-sized framebuffer)\n");
                } else {
                    framebuffer_phys_size = (framebuffer_phys_size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
                    framebuffer = (uint32_t*)(uintptr_t)PHYS_TO_VIRT(fb->framebuffer_addr);
                    framebuffer_pitch_bytes = fb->framebuffer_pitch;
                    framebuffer_bpp = fb->framebuffer_bpp;
                    SCREEN_W = fb->framebuffer_width;
                    SCREEN_H = fb->framebuffer_height;
                    /* Every vga_flip() writes the entire framebuffer
                     * sequentially - Write-Combining lets the CPU
                     * batch those into wide bursts instead of one bus
                     * transaction per store. Safe to attempt
                     * unconditionally: it's a no-op if the region
                     * isn't already mapped the way it expects (see
                     * paging_mark_region_wc()'s own doc comment). */
                    paging_mark_region_wc(framebuffer_phys_addr, framebuffer_phys_size);
                    serial_puts("[VGA] fb tag accepted\n");
                    return;
                }
            } else {
                serial_puts("[VGA] fb tag REJECTED (addr==0 or unsupported bpp)\n");
            }
        }

        uint32_t step = (tag->size + 7u) & ~7u;
        if (step < sizeof(multiboot2_tag_t)) {
            break;
        }
        tag_ptr += step;
    }
    serial_puts("[VGA] mb2 tag scan finished, no usable framebuffer tag found\n");
}

void vga_reserve_physical_regions(void) {
    if (framebuffer_phys_addr && framebuffer_phys_size) {
        phys_memory_reserve_range((phys_addr_t)framebuffer_phys_addr, framebuffer_phys_size);
        framebuffer_physical_reserved = true;

        /* The framebuffer's physical address is typically an MMIO BAR
         * (e.g. QEMU's bochs-vbe device at 0xFD000000) far outside the
         * normal RAM range covered by the kernel's general higher-half
         * mapping. phys_memory_reserve_range() above only keeps the
         * physical page allocator from handing these pages out for
         * something else - it does not create a page table entry, so
         * without an explicit mapping here, the very first write through
         * PHYS_TO_VIRT(framebuffer_phys_addr) page-faults (present=0,
         * write, kernel mode) the moment anything tries to draw. */
        extern bool paging_map_range(uint64_t vs, uint64_t ps_arg, uint64_t size, uint64_t flags);
        uint64_t fb_virt = (uint64_t)PHYS_TO_VIRT(framebuffer_phys_addr);
        /* The paging bootstrap promotes aligned ranges to 2MiB leaves.  Map
         * the final partially occupied large page too: rendering never writes
         * beyond framebuffer_phys_size, while avoiding a slow and fragile
         * 4KiB tail walk for a 1024x768 (3MiB) framebuffer. */
        const uint64_t large_page = 1ULL << 21;
        uint64_t fb_map_size = (framebuffer_phys_size + large_page - 1ULL) & ~(large_page - 1ULL);
        if (!paging_map_range(fb_virt, framebuffer_phys_addr, fb_map_size, 0x3 /* present|writable */)) {
            serial_puts("[VGA] WARNING: failed to map framebuffer physical range into page tables"
                        " - drawing will page-fault\n");
        }

        vga_ensure_backbuffer();
    }
}

void vga_init(uint64_t multiboot_magic, uint64_t multiboot_info_addr) {
    (void)multiboot_magic;
    framebuffer = NULL;
    framebuffer_pitch_bytes = 0;
    framebuffer_bpp = 0;
    framebuffer_phys_addr = 0;
    framebuffer_phys_size = 0;
    framebuffer_physical_reserved = false;

    /* Prefer the framebuffer advertised by Multiboot2/GRUB. */
    vga_parse_multiboot2_framebuffer(multiboot_info_addr);

    /* Debug: Print framebuffer info */
    serial_puts("[VGA] Screen resolution: ");
    serial_putdec(SCREEN_W);
    serial_puts("x");
    serial_putdec(SCREEN_H);
    serial_puts("\n");

    /* If VirtualBox provides 640x480, we must accept it */
    /* Forcing 1280x720 would cause memory overflow and noise */
    if (SCREEN_W == 640 && SCREEN_H == 480) {
        serial_puts("[VGA] WARNING: VirtualBox only provides 640x480\n");
        serial_puts("[VGA] Accepting 640x480 to avoid memory overflow\n");
        /* Keep 640x480 - do not force 1280x720 */
        /* Forcing would cause: 1280*720*4 = 3,686,400 bytes */
        /* But framebuffer only has: 640*480*4 = 1,228,800 bytes */
        /* This would overflow by 2,457,600 bytes causing noise */
    }

    if (framebuffer && framebuffer_pitch_bytes == 0) {
        framebuffer_pitch_bytes = SCREEN_W * (framebuffer_bpp == 24 ? 3u : 4u);
    }

    /* Backbuffer allocation is deferred until the physical framebuffer
     * range has been reserved. That keeps early heap/page allocations from
     * ever racing with the MMIO region that backs the display. */
}

void vga_flip(void) {
    if (!framebuffer || !backbuffer) {
        return;
    }

    if (framebuffer_pitch_bytes == 0) {
        framebuffer_pitch_bytes = SCREEN_W * (framebuffer_bpp == 24 ? 3u : sizeof(uint32_t));
    }

    if (framebuffer_bpp == 24) {
        /* 24bpp needs a per-pixel channel repack (32bpp source packed
         * down to 3 bytes), which is a format conversion rather than a
         * same-format block transfer, so it stays as its own loop
         * instead of going through gfx_blit(). */
        uint8_t* dst = (uint8_t*)framebuffer;
        uint8_t* src = (uint8_t*)backbuffer;
        size_t row_bytes = (size_t)(SCREEN_W * sizeof(uint32_t));
        for (uint64_t y = 0; y < SCREEN_H; y++) {
            uint8_t* dst_row = dst + (size_t)y * framebuffer_pitch_bytes;
            uint32_t* src_row = (uint32_t*)(src + (size_t)y * row_bytes);
            for (uint64_t x = 0; x < SCREEN_W; ++x) {
                uint32_t c = src_row[x];
                size_t off = (size_t)x * 3u;
                dst_row[off + 0] = (uint8_t)(c & 0xFFu);
                dst_row[off + 1] = (uint8_t)((c >> 8) & 0xFFu);
                dst_row[off + 2] = (uint8_t)((c >> 16) & 0xFFu);
            }
        }
        return;
    }

    /* 32bpp: the final "present" step of the whole GUI - a same-format
     * block transfer from the backbuffer to the real hardware
     * framebuffer. This is THE BitBlt every frame ends with, so it
     * goes through the same primitive as everything else instead of
     * its own bespoke memcpy loop (functionally identical - still one
     * memcpy per row under the hood - just unified). */
    uint32_t fb_stride = (uint32_t)(framebuffer_pitch_bytes / sizeof(uint32_t));
    if (!vga_copy_rect_tiled(framebuffer, fb_stride, backbuffer, (uint32_t)SCREEN_W,
                             0, 0, (uint32_t)SCREEN_W, (uint32_t)SCREEN_H)) {
        gfx_surface_t fb_surface = gfx_surface_make(framebuffer, (int)SCREEN_W, (int)SCREEN_H,
                                                     (int)fb_stride);
        gfx_surface_t bb_surface = vga_backbuffer_surface();
        gfx_blit(&fb_surface, 0, 0, &bb_surface, 0, 0, (int)SCREEN_W, (int)SCREEN_H, GFX_BLIT_COPY, 0);
    }
}

void vga_flip_rect(int x, int y, int w, int h) {
    if (!framebuffer || !backbuffer || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)SCREEN_W || y >= (int)SCREEN_H) return;
    if (x + w > (int)SCREEN_W) w = (int)SCREEN_W - x;
    if (y + h > (int)SCREEN_H) h = (int)SCREEN_H - y;
    if (w <= 0 || h <= 0) return;

    if (framebuffer_pitch_bytes == 0) {
        framebuffer_pitch_bytes = SCREEN_W * (framebuffer_bpp == 24 ? 3u : sizeof(uint32_t));
    }
    if (framebuffer_bpp == 24) {
        uint8_t *dst = (uint8_t *)framebuffer;
        for (int row = 0; row < h; ++row) {
            uint8_t *dst_row = dst + (size_t)(y + row) * framebuffer_pitch_bytes + (size_t)x * 3u;
            uint32_t *src_row = backbuffer + (size_t)(y + row) * SCREEN_W + (size_t)x;
            for (int col = 0; col < w; ++col) {
                uint32_t c = src_row[col];
                dst_row[col * 3 + 0] = (uint8_t)(c & 0xFFu);
                dst_row[col * 3 + 1] = (uint8_t)((c >> 8) & 0xFFu);
                dst_row[col * 3 + 2] = (uint8_t)((c >> 16) & 0xFFu);
            }
        }
        return;
    }

    uint32_t fb_stride = (uint32_t)(framebuffer_pitch_bytes / sizeof(uint32_t));
    if (!vga_copy_rect_tiled(framebuffer, fb_stride, backbuffer, (uint32_t)SCREEN_W,
                             (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h)) {
        gfx_surface_t fb_surface = gfx_surface_make(framebuffer, (int)SCREEN_W,
                                                     (int)SCREEN_H, (int)fb_stride);
        gfx_surface_t bb_surface = vga_backbuffer_surface();
        gfx_blit(&fb_surface, x, y, &bb_surface, x, y, w, h, GFX_BLIT_COPY, 0);
    }
}

void vga_wait_vblank(void) {}

void vga_clear(uint64_t color) {
    vga_ensure_backbuffer();
    if (backbuffer) {
        gfx_surface_t bb = vga_backbuffer_surface();
        gfx_blit_fill(&bb, 0, 0, (int)SCREEN_W, (int)SCREEN_H, (uint32_t)color);
        return;
    }
    vga_clear_framebuffer(vga_color_to_u32(color));
}

void vga_put_pixel(int x, int y, uint64_t color) {
    vga_ensure_backbuffer();
    if (backbuffer) {
        gfx_surface_t bb = vga_backbuffer_surface();
        gfx_surface_set_pixel(&bb, x, y, (uint32_t)color);
    } else {
        vga_write_framebuffer_pixel(x, y, vga_color_to_u32(color));
    }
}


void vga_set_pixel(int x, int y, uint64_t color) {
    vga_put_pixel(x, y, color);
}

uint64_t vga_get_pixel(int x, int y) {
    if (x >= 0 && (uint64_t)x < SCREEN_W && y >= 0 && (uint64_t)y < SCREEN_H) {
        if (backbuffer) return backbuffer[y * SCREEN_W + x];
        if (framebuffer) {
            if (framebuffer_bpp == 24) {
                uint8_t* fb = (uint8_t*)framebuffer;
                size_t pitch = framebuffer_pitch_bytes ? (size_t)framebuffer_pitch_bytes : (size_t)(SCREEN_W * 3u);
                uint8_t* px = fb + (size_t)y * pitch + (size_t)x * 3u;
                return ((uint64_t)px[2] << 16) | ((uint64_t)px[1] << 8) | (uint64_t)px[0];
            }
            uint32_t* row = (uint32_t*)((uint8_t*)framebuffer + (size_t)y * (framebuffer_pitch_bytes ? (size_t)framebuffer_pitch_bytes : (size_t)(SCREEN_W * sizeof(uint32_t))));
            return row[x];
        }
    }
    return 0;
}

void vga_fill_rect(int x, int y, int w, int h, uint64_t color) {
    if (w <= 0 || h <= 0) return;

    /* Window chrome (shadow, gradient body, rounded rect, titlebar)
     * calls this dozens of times per frame at window height/width, so
     * this goes through the shared block-fill primitive (one clip
     * pass, then whole-row writes) instead of a per-pixel loop. */
    vga_ensure_backbuffer();
    if (backbuffer) {
        gfx_surface_t bb = vga_backbuffer_surface();
        gfx_blit_fill(&bb, x, y, w, h, (uint32_t)color);
        return;
    }

    /* No backbuffer yet (very early boot) - fall back to the direct
     * framebuffer-pixel path, same as before. */
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            vga_put_pixel(x + j, y + i, color);
        }
    }
}

/* This was declared in vga.h (in fact in *both* copies of it - see
 * the vga.h header-consolidation note) but never implemented anywhere
 * in the tree, which meant nothing could ever actually call it
 * without a link error. It's now the concrete BitBlt entry point:
 * copy a w x h block from src_buf(sx,sy) onto the screen at (dx,dy).
 * src_buf is tightly packed at a natural width of (sx + w) pixels -
 * i.e. it holds at least (sy + h) rows of (sx + w) pixels each, so
 * sx/sy=0 (the common case) means src_buf is exactly a w x h image,
 * and non-zero sx/sy let a caller blit a sub-rect out of a bitmap it
 * is keeping wider than what's being copied this call. */
void vga_copy_rect(int dx, int dy, int sx, int sy, int w, int h, uint32_t* src_buf) {
    if (!src_buf || w <= 0 || h <= 0 || sx < 0 || sy < 0) return;
    vga_ensure_backbuffer();
    if (!backbuffer) return;

    gfx_surface_t dst = vga_backbuffer_surface();
    gfx_surface_t src = gfx_surface_make(src_buf, sx + w, sy + h, sx + w);
    gfx_blit(&dst, dx, dy, &src, sx, sy, w, h, GFX_BLIT_COPY, 0);
}

void vga_copy_rect_strided(int dx, int dy, int w, int h, const uint32_t *src_buf,
                           int src_stride) {
    if (src_buf == NULL || src_stride <= 0 || w <= 0 || h <= 0) return;
    vga_ensure_backbuffer();
    if (backbuffer == NULL) return;
    gfx_surface_t dst = vga_backbuffer_surface();
    gfx_surface_t src = gfx_surface_make((uint32_t *)src_buf, src_stride,
                                         h, src_stride);
    gfx_blit(&dst, dx, dy, &src, 0, 0, w, h, GFX_BLIT_COPY, 0);
}

void vga_draw_rect(int x, int y, int w, int h, uint64_t color) {
    for (int j = 0; j < w; j++) {
        vga_put_pixel(x + j, y, color);
        vga_put_pixel(x + j, y + h - 1, color);
    }
    for (int i = 0; i < h; i++) {
        vga_put_pixel(x, y + i, color);
        vga_put_pixel(x + w - 1, y + i, color);
    }
}

void vga_draw_line(int x0, int y0, int x1, int y1, uint64_t color) {
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1), sx = x0 < x1 ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        vga_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

uint64_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint64_t)r << 16) | ((uint64_t)g << 8) | b;
}

uint64_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint64_t)a << 24) | ((uint64_t)r << 16) | ((uint64_t)g << 8) | b;
}

uint64_t lighten(uint64_t color, uint8_t amount) {
    uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    r = (r + amount > 255) ? 255 : r + amount;
    g = (g + amount > 255) ? 255 : g + amount;
    b = (b + amount > 255) ? 255 : b + amount;
    return rgb(r, g, b);
}


void vga_set_font_scale(int scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    font_scale = scale;
}
int vga_get_font_scale(void) { return font_scale; }
void vga_set_font_resolution(int res) {
    if (res < 1) res = 1;
    font_resolution = res;
}
int vga_get_font_resolution(void) { return font_resolution; }

static const vga_font24_glyph_t* vga_lookup_font24_glyph(uint32_t codepoint) {
    for (int i = 0; i < VGA_FONT24X24_COUNT; ++i) {
        if (font24x24[i].codepoint == codepoint) {
            return &font24x24[i];
        }
    }
    return NULL;
}

static bool vga_font24_pixel_on(const vga_font24_glyph_t* glyph, int row, int col) {
    if (!glyph || row < 0 || row >= 16 || col < 0 || col >= 8) {
        return false;
    }
    uint8_t bits = glyph->rows[row];
    int bit_idx = 7 - col;  // bit 7 is leftmost, bit 0 is rightmost
    if (bit_idx < 0 || bit_idx >= 8) {
        return false;
    }
    return (bits >> bit_idx) & 1u;
}

int vga_get_font_width(void) { return 8 * font_scale; }
int vga_get_font_height(void) { return 16 * font_scale; }

static bool vga_bg_is_transparent(uint64_t bg) {
    return bg == 0xFFFFFFFFULL;
}

static bool vga_fallback_glyph_on(uint8_t ch, int row, int col) {
    if (ch == ' ') return false;
    if (row == 0 || row == 15 || col == 0 || col == 11) return true;
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')) {
        return ((row + col + ch) & 3) == 0 || (row == 7 && col > 2 && col < 9);
    }
    return ((row * 13 + col * 7 + ch) & 5) == 0;
}

/* Glyph cell is 12 columns x 16 rows -> 2 bytes/row (MSB-first) in the
 * packed 1bpp format gfx_blit_bitmap1() expects. font24x24_data.c only
 * stores the left 8 columns per row (vga_font24_pixel_on() already
 * returns false for col >= 8), so the second byte is always 0 for a
 * real glyph; the synthetic fallback box/checkerboard glyph (used for
 * codepoints with no font entry) can set bits anywhere in the full 12
 * columns, so it packs both bytes from vga_fallback_glyph_on(). */
#define VGA_GLYPH_W 12
#define VGA_GLYPH_H 16
#define VGA_GLYPH_STRIDE_BYTES 2 /* ceil(12/8) */

/* NetSurf redraws the same ASCII-heavy UI and page text often.  Cache the
 * packed 1bpp cells after their first use so subsequent draws avoid both the
 * linear font24 lookup and fallback-glyph synthesis.  The cache stores source
 * masks only; foreground/background colours and scale remain dynamic and are
 * still applied by gfx_blit_bitmap1(). */
static uint8_t vga_ascii_glyph_cache[256][VGA_GLYPH_H * VGA_GLYPH_STRIDE_BYTES];
static bool vga_ascii_glyph_cached[256];

static const uint8_t* vga_get_cached_ascii_glyph(uint8_t ch) {
    uint8_t* bits = vga_ascii_glyph_cache[ch];
    if (vga_ascii_glyph_cached[ch]) return bits;

    const vga_font24_glyph_t* glyph = vga_lookup_font24_glyph((uint32_t)ch);
    if (glyph != NULL) {
        for (int row = 0; row < VGA_GLYPH_H; ++row) {
            bits[row * VGA_GLYPH_STRIDE_BYTES + 0] = glyph->rows[row];
            bits[row * VGA_GLYPH_STRIDE_BYTES + 1] = 0;
        }
    } else {
        for (int row = 0; row < VGA_GLYPH_H; ++row) {
            uint8_t b0 = 0, b1 = 0;
            for (int col = 0; col < VGA_GLYPH_W; ++col) {
                if (!vga_fallback_glyph_on(ch, row, col)) continue;
                if (col < 8) b0 |= (uint8_t)(1u << (7 - col));
                else         b1 |= (uint8_t)(1u << (7 - (col - 8)));
            }
            bits[row * VGA_GLYPH_STRIDE_BYTES + 0] = b0;
            bits[row * VGA_GLYPH_STRIDE_BYTES + 1] = b1;
        }
    }
    vga_ascii_glyph_cached[ch] = true;
    return bits;
}

static const cos_jp_font16_glyph_t* vga_lookup_jp_glyph(uint32_t codepoint) {
    uint32_t lo = 0, hi = cos_jp_font16_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t current = cos_jp_font16[mid].codepoint;
        if (current == codepoint) return &cos_jp_font16[mid];
        if (current < codepoint) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

int vga_get_codepoint_width(uint32_t codepoint) {
    if (codepoint <= 0xFFu) return vga_get_font_width();
    if (vga_lookup_jp_glyph(codepoint) == NULL) return vga_get_font_width();
    int scale = font_scale < 1 ? 1 : font_scale;
    return 16 * scale;
}

/* Decode one well-formed UTF-8 scalar value. Invalid or truncated sequences
 * consume one byte and are rendered by the regular fallback-glyph path. */
static uint32_t vga_utf8_decode(const char** cursor, const char* end) {
    const unsigned char* p = (const unsigned char*)*cursor;
    if (!p || (const char*)p >= end) return 0;
    uint32_t c = *p++;
    if (c < 0x80) { *cursor = (const char*)p; return c; }

    int extra = 0;
    uint32_t value = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; value = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; value = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; value = c & 0x07; }
    else { *cursor = (const char*)p; return c; }

    for (int i = 0; i < extra; ++i) {
        if ((const char*)p >= end || (*p & 0xC0) != 0x80) {
            *cursor = (const char*)p;
            return c;
        }
        value = (value << 6) | (*p++ & 0x3F);
    }
    *cursor = (const char*)p;
    return value;
}

void vga_draw_char(int x, int y, char c, uint64_t fg, uint64_t bg) {
    uint8_t ch = (uint8_t)c;
    bool transparent_bg = vga_bg_is_transparent(bg);
    int scale = font_scale < 1 ? 1 : font_scale;

    vga_ensure_backbuffer();
    if (!backbuffer) return;

    const uint8_t* bits = vga_get_cached_ascii_glyph(ch);
    gfx_surface_t bb = vga_backbuffer_surface();
    gfx_blit_bitmap1(&bb, x, y, bits, VGA_GLYPH_W, VGA_GLYPH_H, VGA_GLYPH_STRIDE_BYTES,
                      (uint32_t)fg, (uint32_t)bg, !transparent_bg, scale);
}

static int vga_draw_codepoint(int x, int y, uint32_t codepoint, uint64_t fg, uint64_t bg) {
    if (codepoint <= 0xFF) {
        vga_draw_char(x, y, (char)codepoint, fg, bg);
        return vga_get_font_width();
    }

    const cos_jp_font16_glyph_t* glyph = vga_lookup_jp_glyph(codepoint);
    if (!glyph) {
        vga_draw_char(x, y, '?', fg, bg);
        return vga_get_font_width();
    }

    uint8_t bits[16 * 2];
    for (int row = 0; row < 16; ++row) {
        bits[row * 2] = (uint8_t)(glyph->rows[row] >> 8);
        bits[row * 2 + 1] = (uint8_t)(glyph->rows[row] & 0xFF);
    }
    vga_ensure_backbuffer();
    if (!backbuffer) return 16 * (font_scale < 1 ? 1 : font_scale);
    gfx_surface_t bb = vga_backbuffer_surface();
    int scale = font_scale < 1 ? 1 : font_scale;
    gfx_blit_bitmap1(&bb, x, y, bits, 16, 16, 2, (uint32_t)fg, (uint32_t)bg,
                      !vga_bg_is_transparent(bg), scale);
    return 16 * scale;
}

void vga_draw_string(int x, int y, const char* s, uint64_t fg, uint64_t bg) {
    if (!s) return;
    const char* cursor = s;
    const char* end = s + strlen(s);
    while (cursor < end) {
        uint32_t codepoint = vga_utf8_decode(&cursor, end);
        x += vga_draw_codepoint(x, y, codepoint, fg, bg);
    }
}

void vga_draw_string_len(int x, int y, const char* s, int len, uint64_t fg, uint64_t bg) {
    if (!s || len <= 0) return;
    const char* cursor = s;
    const char* end = s + len;
    while (cursor < end) {
        uint32_t codepoint = vga_utf8_decode(&cursor, end);
        x += vga_draw_codepoint(x, y, codepoint, fg, bg);
    }
}


uint32_t gui_utf8_prev_char_start(const char* s, uint32_t offset) {
    if (offset == 0) return 0;
    offset--;
    while (offset > 0 && (s[offset] & 0xC0) == 0x80) offset--;
    return offset;
}

/* Minimal BMP (Windows Bitmap) decoder + blit. This had no callers
 * anywhere in the tree and was left as an empty stub - `{}` - so
 * anything that tried to use it silently drew nothing. Supports the
 * common uncompressed case: BITMAPFILEHEADER + BITMAPINFOHEADER
 * (>=40 bytes), 24bpp or 32bpp, BI_RGB (no compression), either row
 * order. That covers what C-OS's own asset pipeline and any ordinary
 * exporter produce; anything else (compressed, paletted, OS/2-style
 * headers) is rejected safely with a serial log line rather than
 * guessed at. Note the signature (inherited as-is) has no size
 * parameter, so - same as before this fix - the caller is trusted to
 * pass a buffer that actually contains a complete BMP; this function
 * cannot bounds-check against a length it was never given. */
void vga_draw_bmp(int x, int y, const uint8_t* bmp_data) {
    if (!bmp_data) return;
    if (bmp_data[0] != 'B' || bmp_data[1] != 'M') {
        serial_puts("[VGA] vga_draw_bmp: not a BMP (bad signature)\n");
        return;
    }

    uint32_t pixel_offset = (uint32_t)bmp_data[10] | ((uint32_t)bmp_data[11] << 8) |
                             ((uint32_t)bmp_data[12] << 16) | ((uint32_t)bmp_data[13] << 24);
    uint32_t header_size = (uint32_t)bmp_data[14] | ((uint32_t)bmp_data[15] << 8) |
                            ((uint32_t)bmp_data[16] << 16) | ((uint32_t)bmp_data[17] << 24);
    int32_t width = (int32_t)((uint32_t)bmp_data[18] | ((uint32_t)bmp_data[19] << 8) |
                               ((uint32_t)bmp_data[20] << 16) | ((uint32_t)bmp_data[21] << 24));
    int32_t height_raw = (int32_t)((uint32_t)bmp_data[22] | ((uint32_t)bmp_data[23] << 8) |
                                    ((uint32_t)bmp_data[24] << 16) | ((uint32_t)bmp_data[25] << 24));
    uint16_t bpp = (uint16_t)((uint32_t)bmp_data[28] | ((uint32_t)bmp_data[29] << 8));
    uint32_t compression = (uint32_t)bmp_data[30] | ((uint32_t)bmp_data[31] << 8) |
                            ((uint32_t)bmp_data[32] << 16) | ((uint32_t)bmp_data[33] << 24);

    if (header_size < 40) {
        serial_puts("[VGA] vga_draw_bmp: unsupported header (need BITMAPINFOHEADER or newer)\n");
        return;
    }
    if (compression != 0) {
        serial_puts("[VGA] vga_draw_bmp: compressed BMPs are not supported\n");
        return;
    }
    if (bpp != 24 && bpp != 32) {
        serial_puts("[VGA] vga_draw_bmp: only 24bpp/32bpp BMPs are supported\n");
        return;
    }
    if (width <= 0 || width > 8192) {
        serial_puts("[VGA] vga_draw_bmp: width out of range\n");
        return;
    }

    bool top_down = (height_raw < 0);
    int32_t height = top_down ? -height_raw : height_raw;
    if (height <= 0 || height > 8192) {
        serial_puts("[VGA] vga_draw_bmp: height out of range\n");
        return;
    }

    vga_ensure_backbuffer();
    if (!backbuffer) return;

    int bytes_per_pixel = bpp / 8;
    uint32_t row_stride = (uint32_t)(((width * bytes_per_pixel) + 3) & ~3);

    uint32_t* row_buf = (uint32_t*)kmalloc((size_t)width * sizeof(uint32_t));
    if (!row_buf) {
        serial_puts("[VGA] vga_draw_bmp: out of memory for row buffer\n");
        return;
    }

    gfx_surface_t dst = vga_backbuffer_surface();
    gfx_surface_t src_row = gfx_surface_make(row_buf, width, 1, width);

    for (int32_t row = 0; row < height; ++row) {
        /* BMP rows are bottom-up by default (the first row stored in
         * the file is the bottom of the image); a negative height
         * field means top-down instead. */
        int32_t file_row = top_down ? row : (height - 1 - row);
        const uint8_t* src = bmp_data + pixel_offset + (uint64_t)file_row * row_stride;

        for (int32_t col = 0; col < width; ++col) {
            const uint8_t* px = src + (size_t)col * bytes_per_pixel;
            uint8_t b = px[0], g = px[1], r = px[2];
            row_buf[col] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }

        /* One block-transfer call per row, same as everything else in
         * this file now goes through - not a per-pixel vga_put_pixel
         * loop. */
        gfx_blit(&dst, x, y + row, &src_row, 0, 0, width, 1, GFX_BLIT_COPY, 0);
    }

    kfree(row_buf);
}

void vga_fill_circle(int cx, int cy, int r, uint64_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                vga_put_pixel(cx + x, cy + y, color);
            }
        }
    }
}

void vga_draw_circle(int cx, int cy, int r, uint64_t color) {
    int x = r, y = 0;
    int err = 0;
    while (x >= y) {
        vga_put_pixel(cx + x, cy + y, color);
        vga_put_pixel(cx + y, cy + x, color);
        vga_put_pixel(cx - y, cy + x, color);
        vga_put_pixel(cx - x, cy + y, color);
        vga_put_pixel(cx - x, cy - y, color);
        vga_put_pixel(cx - y, cy - x, color);
        vga_put_pixel(cx + y, cy - x, color);
        vga_put_pixel(cx + x, cy - y, color);
        if (err <= 0) { y += 1; err += 2 * y + 1; }
        if (err > 0) { x -= 1; err -= 2 * x + 1; }
    }
}

void vga_fill_rounded_rect(int x, int y, int w, int h, int r, uint64_t color) {
    vga_fill_rect(x + r, y, w - 2 * r, h, color);
    vga_fill_rect(x, y + r, w, h - 2 * r, color);
    vga_fill_circle(x + r, y + r, r, color);
    vga_fill_circle(x + w - r - 1, y + r, r, color);
    vga_fill_circle(x + r, y + h - r - 1, r, color);
    vga_fill_circle(x + w - r - 1, y + h - r - 1, r, color);
}

/* Plots just the two Bresenham-circle symmetry points that belong to
 * one 90-degree quadrant of the circle centered at (cx,cy) - i.e. one
 * corner of a rounded rect. quadrant: 0=top-left (dx<=0,dy<=0),
 * 1=top-right (dx>=0,dy<=0), 2=bottom-left (dx<=0,dy>=0),
 * 3=bottom-right (dx>=0,dy>=0). Used below so vga_draw_rounded_rect's
 * corners are actually drawn, using the same circle math as
 * vga_draw_circle so standalone circles and rounded-rect corners look
 * consistent. */
static void vga_draw_circle_quadrant(int cx, int cy, int r, int quadrant, uint64_t color) {
    int x = r, y = 0;
    int err = 0;
    while (x >= y) {
        switch (quadrant) {
            case 0:
                vga_put_pixel(cx - x, cy - y, color);
                vga_put_pixel(cx - y, cy - x, color);
                break;
            case 1:
                vga_put_pixel(cx + y, cy - x, color);
                vga_put_pixel(cx + x, cy - y, color);
                break;
            case 2:
                vga_put_pixel(cx - y, cy + x, color);
                vga_put_pixel(cx - x, cy + y, color);
                break;
            default:
                vga_put_pixel(cx + x, cy + y, color);
                vga_put_pixel(cx + y, cy + x, color);
                break;
        }
        if (err <= 0) { y += 1; err += 2 * y + 1; }
        if (err > 0) { x -= 1; err -= 2 * x + 1; }
    }
}

void vga_draw_rounded_rect(int x, int y, int w, int h, int r, uint64_t color) {
    if (w <= 0 || h <= 0) return;
    if (r <= 0) { vga_draw_rect(x, y, w, h, color); return; }
    int max_r = (w < h ? w : h) / 2;
    if (r > max_r) r = max_r;

    /* Straight edges, inset by r at each end so they stop exactly
     * where the corner arcs begin. */
    vga_draw_line(x + r, y, x + w - r - 1, y, color);
    vga_draw_line(x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
    vga_draw_line(x, y + r, x, y + h - r - 1, color);
    vga_draw_line(x + w - 1, y + r, x + w - 1, y + h - r - 1, color);

    /* The 4 corner arcs that were missing before - same corner
     * centers vga_fill_rounded_rect() uses for its fill circles, so
     * the stroke lines up with the fill beneath it. */
    vga_draw_circle_quadrant(x + r,         y + r,         r, 0, color);
    vga_draw_circle_quadrant(x + w - r - 1, y + r,         r, 1, color);
    vga_draw_circle_quadrant(x + r,         y + h - r - 1, r, 2, color);
    vga_draw_circle_quadrant(x + w - r - 1, y + h - r - 1, r, 3, color);
}

uint64_t blend(uint64_t fg, uint64_t bg, uint8_t alpha) {
    uint8_t r = (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha)) / 255;
    uint8_t g = (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha)) / 255;
    uint8_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha)) / 255;
    return rgb(r, g, b);
}

uint64_t darken(uint64_t color, uint8_t amount) {
    uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    r = (r < amount) ? 0 : r - amount;
    g = (g < amount) ? 0 : g - amount;
    b = (b < amount) ? 0 : b - amount;
    return rgb(r, g, b);
}

int gui_clip_x = 0, gui_clip_y = 0, gui_clip_w = 0, gui_clip_h = 0;
int gui_clip_enabled = 0;

int vga_isin(int angle) {
    /* Simple integer sine approximation */
    return 0; 
}

int vga_icos(int angle) {
    /* Simple integer cosine approximation */
    return 1024;
}

void vga_draw_line_thick(int x0, int y0, int x1, int y1, int thick, uint64_t color) {
    vga_draw_line(x0, y0, x1, y1, color);
}



