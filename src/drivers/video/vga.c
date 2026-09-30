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
#include "virtio_gpu.h"
#include "math.h"
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
/* Text size is stored as a plain pixel advance width - font_px_width -
 * rather than the old integer 1x-4x "scale" + separate "extra small"
 * flag: a person asking to shrink text below the default should be
 * able to say exactly how far (down to 1px), not just pick one more
 * preset step, and one continuous unit does that without a second
 * flag to keep in sync. vga_set_font_scale()/vga_set_font_extra_small()
 * below still exist for existing callers, translated to/from this. */
#define FONT_PX_WIDTH_DEFAULT 8
#define FONT_PX_WIDTH_MIN     1
#define FONT_PX_WIDTH_MAX     32
static int font_px_width = FONT_PX_WIDTH_DEFAULT;
static int font_resolution = 1;
static uint64_t framebuffer_pitch_bytes = 0;
static uint8_t framebuffer_bpp = 0;
static uint64_t framebuffer_phys_addr = 0;
static uint64_t framebuffer_phys_size = 0;
static bool framebuffer_physical_reserved = false;
static vga_render_backend_t vga_active_backend = VGA_RENDER_BACKEND_CPU;

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

/* Same AVX2 row-copy helper gfx_blit() uses for its GFX_BLIT_COPY path
 * (see gfx_blit_avx2.c's file header for the safety requirements
 * around it - runtime-gated, function-local target("avx2") only).
 * SMP parallelism and AVX2 are additive here instead of the either/or
 * split this file used to have: previously, any machine with 2+
 * online CPUs took this tiled path with a plain per-row memcpy and
 * never got AVX2 at all, while a single-CPU machine got AVX2 (via the
 * gfx_blit() fallback below) but no parallelism.
 *
 * gfx_blit_avx2_available() is deliberately called from INSIDE the
 * worker below - on whichever CPU actually ends up running it - and
 * NOT once on the BSP with the answer handed down to every tile job.
 * CR4.OSXSAVE/XCR0 are per-CPU state; gfx_blit_avx2_available()
 * enables and verifies them for the CPU it runs on (see its own
 * comment on why a cached answer from a different CPU would be a
 * guaranteed #UD, not a graceful fallback), and this file dispatches
 * tile jobs to AP cores that have never otherwise touched AVX2. */
extern bool gfx_blit_avx2_available(void);
extern void gfx_blit_avx2_copy_row(uint32_t* dst, const uint32_t* src, int count);

static void vga_copy_tile_worker(void *opaque) {
    const vga_copy_tile_job_t *tile = (const vga_copy_tile_job_t *)opaque;
    if (tile == NULL || tile->dst == NULL || tile->src == NULL ||
        tile->width == 0 || tile->height == 0) return;
    bool use_avx2 = gfx_blit_avx2_available();
    for (uint32_t row = 0; row < tile->height; ++row) {
        uint32_t *dst = tile->dst + (size_t)(tile->y + row) * tile->dst_stride + tile->x;
        const uint32_t *src = tile->src + (size_t)(tile->y + row) * tile->src_stride + tile->x;
        if (use_avx2) {
            gfx_blit_avx2_copy_row(dst, src, (int)tile->width);
        } else {
            memcpy(dst, src, (size_t)tile->width * sizeof(uint32_t));
        }
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

/* Same allocation as vga_ensure_backbuffer() above, minus the
 * framebuffer_physical_reserved precondition. That flag exists purely
 * to sequence backbuffer allocation *after* the legacy framebuffer's
 * MMIO physical range has been reserved (see the comment on
 * vga_reserve_physical_regions()) - it is not a real functional
 * dependency of the backbuffer on the framebuffer. When there is no
 * legacy framebuffer at all (a plain `-device virtio-gpu-pci` output,
 * with no VBE-compatible BAR for GRUB/multiboot2 to hand us), that
 * sequencing concern does not apply, but vga_ensure_backbuffer() would
 * otherwise never allocate a backbuffer at all - nothing would ever
 * have anywhere to draw into. This is called lazily, from
 * vga_set_render_backend(GPU), only in that exact situation (backbuffer
 * still NULL), so it changes nothing about existing boots that do have
 * a legacy framebuffer. */
static void vga_ensure_backbuffer_no_framebuffer(void) {
    if (backbuffer || SCREEN_W == 0 || SCREEN_H == 0 || !memory_heap_ready()) {
        return;
    }
    size_t bytes = (size_t)(SCREEN_W * SCREEN_H * sizeof(uint32_t));
    backbuffer = (uint32_t*)kmalloc(bytes);
    if (backbuffer) {
        memset(backbuffer, 0, bytes);
    }
}

bool vga_gpu_backend_available(void) {
    return virtio_gpu_probe();
}

vga_render_backend_t vga_get_render_backend(void) {
    return vga_active_backend;
}

bool vga_set_render_backend(vga_render_backend_t backend) {
    if (backend == VGA_RENDER_BACKEND_GPU) {
        if (!backbuffer) {
            vga_ensure_backbuffer_no_framebuffer();
        }
        if (!backbuffer || SCREEN_W == 0 || SCREEN_H == 0) {
            serial_puts("[VGA] cannot switch to GPU backend: no backbuffer yet\n");
            return false;
        }
        size_t bytes = (size_t)(SCREEN_W * SCREEN_H * sizeof(uint32_t));
        if (!virtio_gpu_enable((uint32_t)SCREEN_W, (uint32_t)SCREEN_H, backbuffer, (uint64_t)bytes)) {
            serial_puts("[VGA] VirtIO-GPU backend unavailable, staying on CPU backend\n");
            vga_active_backend = VGA_RENDER_BACKEND_CPU;
            return false;
        }
        vga_active_backend = VGA_RENDER_BACKEND_GPU;
        /* Push the whole current frame right away rather than waiting
         * for whatever triggers the next redraw. */
        virtio_gpu_present_rect(0, 0, (int)SCREEN_W, (int)SCREEN_H);
        return true;
    }

    /* CPU */
    if (vga_active_backend == VGA_RENDER_BACKEND_GPU) {
        virtio_gpu_disable();
    }
    vga_active_backend = VGA_RENDER_BACKEND_CPU;
    if (framebuffer) {
        vga_flip();
    }
    return true;
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

static bool vga_gpu_routing(void);
/* Switches rectangle fills to execute on the host GPU through VirGL
 * (step 1 of moving GUI rendering onto the GPU). Requires the VirtIO-GPU
 * backend to be active and the host to offer VIRGL. Primitives that are
 * not converted yet (text, lines, images, ...) still draw into the CPU
 * backbuffer, which is not what is scanned out in this mode. */
bool vga_set_gpu_draw(bool enable) {
    if (!enable) {
        if (virtio_gpu_virgl_draw_active()) {
            virtio_gpu_virgl_end_draw();
            if (vga_active_backend == VGA_RENDER_BACKEND_GPU && backbuffer) {
                virtio_gpu_enable((uint32_t)SCREEN_W, (uint32_t)SCREEN_H, backbuffer,
                                  (uint64_t)SCREEN_W * (uint64_t)SCREEN_H * 4u);
            }
        }
        return true;
    }
    if (vga_active_backend != VGA_RENDER_BACKEND_GPU) return false;
    if (!virtio_gpu_virgl_available()) return false;
    return virtio_gpu_virgl_begin_draw();
}

bool vga_gpu_draw_active(void) { return virtio_gpu_virgl_draw_active(); }

/* Whether the host offered VIRGL at all (see virtio_gpu.c's feature
 * negotiation) - vga_set_gpu_draw(true) can only ever succeed when this
 * is also true, so Settings uses it to grey out the option rather than
 * offer a choice that would just fail. */
bool vga_gpu_virgl_available(void) { return virtio_gpu_virgl_available(); }

/* While suspended, drawing goes to the CPU backbuffer as usual even in
 * GPU draw mode - used to render something once on the CPU (e.g. the
 * desktop layer) before uploading it as a GPU texture. Nests. */
static int vga_gpu_suspend_depth = 0;
void vga_gpu_suspend(void) { ++vga_gpu_suspend_depth; }
void vga_gpu_resume(void)  { if (vga_gpu_suspend_depth > 0) --vga_gpu_suspend_depth; }
static bool vga_gpu_routing(void) {
    return vga_gpu_suspend_depth == 0 && virtio_gpu_virgl_draw_active();
}
bool vga_gpu_routing_active(void) { return vga_gpu_routing(); }

/* Places a CPU-authored image on screen through the GPU texture cache:
 * uploaded only when `version` (or the size) changes, then drawn with a
 * single GPU blit, scaled from (w,h) to (dst_w,dst_h) if they differ.
 * `key` identifies the image across frames (a stable address - the
 * source pixel buffer's own pointer is normally the natural choice,
 * since a genuinely new/reallocated buffer should upload as a new
 * texture anyway). Returns false if GPU draw mode is not active
 * (caller should draw on the CPU). */
bool vga_gpu_draw_image_scaled(const void* key, const uint32_t* pixels, int w, int h, int stride,
                               uint32_t version, bool alpha, int dx, int dy, int dst_w, int dst_h) {
    if (!vga_gpu_routing()) return false;
    uint32_t res = virtio_gpu_virgl_texture(key, pixels, w, h, stride, version, alpha);
    if (!res) return false;
    virtio_gpu_virgl_blit_texture(res, 0, 0, w, h, dx, dy, dst_w, dst_h);
    return true;
}

bool vga_gpu_draw_image(const void* key, const uint32_t* pixels, int w, int h, int stride,
                        uint32_t version, bool alpha, int dx, int dy) {
    return vga_gpu_draw_image_scaled(key, pixels, w, h, stride, version, alpha, dx, dy, w, h);
}

bool vga_gpu_cursor_set_image(const uint32_t* argb64, int hot_x, int hot_y, int x, int y) {
    return virtio_gpu_virgl_cursor_set_image(argb64, hot_x, hot_y, x, y);
}
bool vga_gpu_cursor_move(int x, int y) { return virtio_gpu_virgl_cursor_move(x, y); }
bool vga_gpu_cursor_ready(void) { return virtio_gpu_virgl_cursor_ready(); }

void vga_flip(void) {
    if (virtio_gpu_virgl_draw_active()) {
        virtio_gpu_virgl_present();
        return;
    }
    if (vga_active_backend == VGA_RENDER_BACKEND_GPU && virtio_gpu_is_active()) {
        virtio_gpu_present_rect(0, 0, (int)SCREEN_W, (int)SCREEN_H);
        return;
    }

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
    if (virtio_gpu_virgl_draw_active()) {
        /* GPU draw mode always presents the whole render target; the
         * batch has to be submitted regardless of how small the dirty
         * rect is, and RESOURCE_FLUSH of the full screen costs the same
         * as a partial one for a host-side GL texture. */
        (void)x; (void)y; (void)w; (void)h;
        virtio_gpu_virgl_present();
        return;
    }
    if (vga_active_backend == VGA_RENDER_BACKEND_GPU && virtio_gpu_is_active()) {
        virtio_gpu_present_rect(x, y, w, h);
        return;
    }

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
    /* Routes through vga_fill_rect() (a 1x1 fill) when GPU draw mode is
     * active, rather than writing the CPU backbuffer directly - that
     * backbuffer is not what GPU draw mode scans out, so every caller
     * built on top of this one primitive (vga_draw_line(), the Bresenham
     * circle/arc stepping in vga_draw_circle()/vga_draw_circle_quadrant(),
     * and therefore every outline/border in the UI) would otherwise
     * silently draw nothing visible in GPU mode. See vga_fill_circle()'s
     * own comment above for the fill-side half of this same gap (already
     * fixed there by routing through vga_fill_rect() directly, since a
     * filled circle is cheaper as whole scanline rects than as
     * one-pixel-at-a-time fills here). */
    if (vga_gpu_routing_active()) {
        vga_fill_rect(x, y, 1, 1, color);
        return;
    }
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

    /* GPU draw mode: the fill is executed by the host GPU (see
     * virtio_gpu_virgl_fill_rect()), not written by this CPU. */
    if (vga_gpu_routing()) {
        virtio_gpu_virgl_fill_rect(x, y, w, h, (uint32_t)color);
        return;
    }

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

/* Reads a rectangle back out of the backbuffer into a caller-supplied
 * buffer. The inverse of vga_copy_rect_strided(), used to memoise an
 * already-composited layer so it can be blitted back on later frames
 * instead of being re-rendered. */
void vga_read_rect(int x, int y, int w, int h, uint32_t *dst_buf, int dst_stride) {
    if (dst_buf == NULL || dst_stride <= 0 || w <= 0 || h <= 0) return;
    vga_ensure_backbuffer();
    if (backbuffer == NULL) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)SCREEN_W || y >= (int)SCREEN_H) return;
    if (x + w > (int)SCREEN_W) w = (int)SCREEN_W - x;
    if (y + h > (int)SCREEN_H) h = (int)SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    gfx_surface_t dst = gfx_surface_make(dst_buf, dst_stride, h, dst_stride);
    gfx_surface_t src = vga_backbuffer_surface();
    gfx_blit(&dst, 0, 0, &src, x, y, w, h, GFX_BLIT_COPY, 0);
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
    /* Horizontal/vertical lines (by far the common case for this UI's
     * borders - see vga_draw_rounded_rect()'s straight edges above) are
     * one vga_fill_rect() call instead of walking pixel by pixel - an
     * ordinary CPU speedup, but it also matters more than that in GPU
     * draw mode: N one-pixel fills is N GPU commands where one fill is
     * one, so a window's straight border edges do not become the most
     * expensive thing about drawing it. Diagonal lines still fall
     * through to the Bresenham walk below, which is itself GPU-safe
     * now (see vga_put_pixel()'s own comment). */
    if (y0 == y1) {
        int x_lo = x0 < x1 ? x0 : x1;
        vga_fill_rect(x_lo, y0, (x0 < x1 ? x1 - x0 : x0 - x1) + 1, 1, color);
        return;
    }
    if (x0 == x1) {
        int y_lo = y0 < y1 ? y0 : y1;
        vga_fill_rect(x0, y_lo, 1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1, color);
        return;
    }
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
    font_px_width = FONT_PX_WIDTH_DEFAULT * scale;
}
int vga_get_font_scale(void) {
    /* Best-effort mapping back to the old 1x-4x steps for callers that
     * still ask in those terms - exact at the 4 original settings,
     * rounded to the nearest of them for anything chosen via the
     * finer-grained vga_set_font_pixel_width() below. */
    int nearest = (font_px_width + FONT_PX_WIDTH_DEFAULT / 2) / FONT_PX_WIDTH_DEFAULT;
    if (nearest < 1) nearest = 1;
    if (nearest > 4) nearest = 4;
    return nearest;
}

/* Fine-grained text size: the exact pixel advance width (see
 * vga_get_font_width() below - height always follows at 2x this, the
 * bitmap font's native aspect), rather than a handful of named presets.
 * Lets a person dial text all the way down to 1px if they want to,
 * instead of stopping at one more fixed "extra small" step. */
void vga_set_font_pixel_width(int px) {
    if (px < FONT_PX_WIDTH_MIN) px = FONT_PX_WIDTH_MIN;
    if (px > FONT_PX_WIDTH_MAX) px = FONT_PX_WIDTH_MAX;
    font_px_width = px;
}
int vga_get_font_pixel_width(void) { return font_px_width; }

/* Kept for existing callers - now just the two ends of the same
 * continuous range vga_set_font_pixel_width() controls, rather than a
 * separate flag that used to need keeping in sync with font_scale. */
void vga_set_font_extra_small(bool enable) {
    vga_set_font_pixel_width(enable ? FONT_PX_WIDTH_DEFAULT / 2 : FONT_PX_WIDTH_DEFAULT);
}
bool vga_get_font_extra_small(void) { return font_px_width < FONT_PX_WIDTH_DEFAULT; }

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

int vga_get_font_width(void) { return font_px_width; }
int vga_get_font_height(void) { return font_px_width * 2; }

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
    return vga_get_font_width() * 2;
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

/* Nearest-neighbour resize of a 1bpp glyph mask into (dst_w, dst_h),
 * sampling the centre of each destination pixel - the same technique
 * cos_ns_font_build_mask() in src/netsurf/cos_ns_font.c uses to hit an
 * arbitrary CSS pixel size, minus that function's optional bold/italic
 * passes (this is the OS's own plain UI text, not a styled web page).
 *
 * This replaces two separate things an earlier version of this file
 * did: (1) simply replicating each source pixel into an NxN block for
 * whole-number scale-up, and (2) OR-ing every 2x2 source block
 * together for exactly-half scale-down. Both are gone because (1)
 * never corrected for VGA_GLYPH_W (12) being wider than the actual
 * advance (8) - it drew every glyph 50% wider than its cell at every
 * size, which is what made this OS's own UI text look visibly
 * heavier/tighter than the browser's (cos_ns_font.c already did this
 * correction) - and (2)'s OR-ing biased a shrunk glyph's weight
 * heavier still, the opposite of what a smaller size should look like.
 * Plain point-sampling fits the glyph's ink into whatever destination
 * size is asked for, uniformly, whether that is larger or smaller than
 * native. */
static void vga_resize_glyph(const uint8_t* src, int src_w, int src_h, int src_stride,
                              uint8_t* out, int out_w, int out_h, int out_stride) {
    memset(out, 0, (size_t)out_stride * (size_t)out_h);
    for (int dy = 0; dy < out_h; ++dy) {
        int sy = ((dy * 2 + 1) * src_h) / (out_h * 2);
        if (sy >= src_h) sy = src_h - 1;
        for (int dx = 0; dx < out_w; ++dx) {
            int sx = ((dx * 2 + 1) * src_w) / (out_w * 2);
            if (sx >= src_w) sx = src_w - 1;
            uint8_t byte = src[sy * src_stride + (sx >> 3)];
            if (byte & (uint8_t)(0x80u >> (sx & 7))) {
                out[dy * out_stride + (dx >> 3)] |= (uint8_t)(0x80u >> (dx & 7));
            }
        }
    }
}

/* Resizes whatever vga_glyph_mask() returns for `codepoint` to exactly
 * fill a (dst_w, dst_h) cell and draws it at (x, y) - the single path
 * vga_draw_char() and vga_draw_codepoint() below both funnel through,
 * so ASCII and wide (JP) glyphs are sized/positioned by exactly the
 * same rule. Skips the resize step entirely (drawing the real bitmap
 * unchanged) only when the requested size already matches the source
 * exactly, which is never true at this OS's own default width (8px:
 * ASCII's native bitmap is 12 wide, not 8 - see vga_resize_glyph()'s
 * comment) but can be true for some other combination. */
/* Draws a 1bpp mask (already at its final on-screen size - dst_w x
 * dst_h) by decomposing each row into runs of contiguous "on" bits and
 * filling each run with one vga_fill_rect() call - "on" runs in `fg`,
 * and, only when the background is not transparent (see
 * vga_bg_is_transparent()), the gaps between them in `bg`.
 *
 * This is what makes text work in GPU draw mode without needing the
 * shader/blend pipeline at all: a bitmap font glyph is a hard on/off
 * mask (no antialiasing to blend), so every pixel is either "paint it
 * fg, opaque" or "leave/paint it bg, opaque" - exactly what the
 * existing opaque BLIT-based vga_fill_rect() already does correctly.
 * Row-run decomposition (rather than one fill per pixel) keeps this
 * cheap: a typical glyph row is a handful of runs, not one GPU command
 * per bit. Used only when vga_gpu_routing_active() - the CPU path
 * still uses vga_blit_mask() directly (one pass over the whole mask,
 * cheaper than routing every row through vga_fill_rect()'s GPU check
 * for nothing when there is no GPU to route to). */
static void vga_blit_mask_gpu(int x, int y, const uint8_t* bits, int w, int h,
                              int stride_bytes, uint64_t fg, uint64_t bg) {
    bool opaque_bg = !vga_bg_is_transparent(bg);

    /* Opaque background: one fill for the whole cell, then only the
     * foreground on top - instead of alternating fg/bg runs on every
     * row, which also defeated the driver's same-colour coalescing. */
    if (opaque_bg) vga_fill_rect(x, y, w, h, bg);

    if (w > 64 || h > 128) {
        /* Wider than the row bitmasks below: plain per-row runs. */
        for (int row = 0; row < h; ++row) {
            const uint8_t* rb = bits + (size_t)row * (size_t)stride_bytes;
            int col = 0;
            while (col < w) {
                if (!(rb[col >> 3] & (uint8_t)(0x80u >> (col & 7)))) { ++col; continue; }
                int s0 = col;
                while (col < w && (rb[col >> 3] & (uint8_t)(0x80u >> (col & 7)))) ++col;
                vga_fill_rect(x + s0, y + row, col - s0, 1, fg);
            }
        }
        return;
    }

    /* Each row as a 64-bit mask (bit c = pixel c). */
    uint64_t rows[128];
    for (int row = 0; row < h; ++row) {
        const uint8_t* rb = bits + (size_t)row * (size_t)stride_bytes;
        uint64_t m = 0;
        for (int col = 0; col < w; ++col)
            if (rb[col >> 3] & (uint8_t)(0x80u >> (col & 7))) m |= (1ull << col);
        rows[row] = m;
    }

    /* Greedy maximal rectangles: take each horizontal run of not-yet-
     * covered "on" pixels and extend it downward for as long as the next
     * row has that whole span on; the pixels absorbed below are removed
     * from those rows so they are not filled twice. A glyph's vertical
     * strokes and solid blocks become one fill instead of one per row. */
    for (int row = 0; row < h; ++row) {
        uint64_t m = rows[row];
        while (m) {
            int s0 = __builtin_ctzll(m);
            uint64_t shifted = m >> s0;
            int len = (shifted == ~0ull) ? (64 - s0) : __builtin_ctzll(~shifted);
            uint64_t span = (len >= 64) ? ~0ull : (((1ull << len) - 1ull) << s0);
            int hh = 1;
            while (row + hh < h && (rows[row + hh] & span) == span) {
                rows[row + hh] &= ~span;
                ++hh;
            }
            vga_fill_rect(x + s0, y + row, len, hh, fg);
            m &= ~span;
        }
    }
}

static void vga_draw_glyph_cell(int x, int y, uint32_t codepoint, uint64_t fg, uint64_t bg,
                                 int dst_w, int dst_h) {
    int src_w = 0, src_h = 0, src_stride = 0;
    const uint8_t* bits = vga_glyph_mask(codepoint, &src_w, &src_h, &src_stride);
    if (!bits || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    if (dst_w == src_w && dst_h == src_h) {
        if (vga_gpu_routing_active()) {
            vga_blit_mask_gpu(x, y, bits, src_w, src_h, src_stride, fg, bg);
        } else {
            vga_blit_mask(x, y, bits, src_w, src_h, src_stride, fg, bg);
        }
        return;
    }

    /* FONT_PX_WIDTH_MAX*2 wide, *4 tall (wide/JP glyphs are drawn at
     * twice the narrow advance in both axes - see the callers) is the
     * largest cell this ever needs to hold. */
    uint8_t resized[((FONT_PX_WIDTH_MAX * 2 + 7) / 8) * (FONT_PX_WIDTH_MAX * 4)];
    int stride = (dst_w + 7) / 8;
    vga_resize_glyph(bits, src_w, src_h, src_stride, resized, dst_w, dst_h, stride);
    if (vga_gpu_routing_active()) {
        vga_blit_mask_gpu(x, y, resized, dst_w, dst_h, stride, fg, bg);
    } else {
        vga_blit_mask(x, y, resized, dst_w, dst_h, stride, fg, bg);
    }
}

void vga_draw_char(int x, int y, char c, uint64_t fg, uint64_t bg) {
    vga_ensure_backbuffer();
    if (!backbuffer) return;
    vga_draw_glyph_cell(x, y, (uint8_t)c, fg, bg, font_px_width, font_px_width * 2);
}

static int vga_draw_codepoint(int x, int y, uint32_t codepoint, uint64_t fg, uint64_t bg) {
    vga_ensure_backbuffer();
    if (!backbuffer) return font_px_width;

    if (codepoint > 0xFFu && vga_lookup_jp_glyph(codepoint) != NULL) {
        int w = font_px_width * 2;
        vga_draw_glyph_cell(x, y, codepoint, fg, bg, w, font_px_width * 2);
        return w;
    }

    uint32_t cp = (codepoint <= 0xFFu) ? codepoint : (uint32_t)'?';
    vga_draw_glyph_cell(x, y, cp, fg, bg, font_px_width, font_px_width * 2);
    return font_px_width;
}

/* ---- glyph mask access, for callers that need to transform a glyph ----
 *
 * vga_draw_char()/vga_draw_string() can only draw a glyph at an integer
 * multiple of its native size, upright and unweighted, because that is
 * all gfx_blit_bitmap1()'s `scale` parameter expresses.
 *
 * That is fine for the OS's own UI, which picks one font size. It is not
 * enough for a browser: CSS font-size is an arbitrary pixel height, and
 * font-weight/font-style ask for bold and italic faces this bitmap font
 * does not have. Rather than teach vga.c about CSS, these expose the raw
 * 1bpp glyph so a caller can build whatever transformed mask it needs
 * (see src/netsurf/cos_ns_font.c, which scales, emboldens and slants)
 * and hand it back to vga_blit_mask() to draw.
 *
 * The returned pointer is to internal static storage and is valid until
 * the next call - copy it if you need to keep it. */
const uint8_t *vga_glyph_mask(uint32_t codepoint, int *out_w, int *out_h,
                               int *out_stride)
{
    static uint8_t wide_bits[16 * 2];

    if (codepoint > 0xFFu) {
        const cos_jp_font16_glyph_t *glyph = vga_lookup_jp_glyph(codepoint);
        if (glyph != NULL) {
            for (int row = 0; row < 16; ++row) {
                wide_bits[row * 2]     = (uint8_t)(glyph->rows[row] >> 8);
                wide_bits[row * 2 + 1] = (uint8_t)(glyph->rows[row] & 0xFF);
            }
            if (out_w) *out_w = 16;
            if (out_h) *out_h = 16;
            if (out_stride) *out_stride = 2;
            return wide_bits;
        }
        /* No wide glyph: fall through to the '?' replacement rather than
         * returning NULL, so a caller never has to special-case it. */
        codepoint = (uint32_t)'?';
    }

    if (out_w) *out_w = VGA_GLYPH_W;
    if (out_h) *out_h = VGA_GLYPH_H;
    if (out_stride) *out_stride = VGA_GLYPH_STRIDE_BYTES;
    return vga_get_cached_ascii_glyph((uint8_t)codepoint);
}

/* Draws an arbitrary 1bpp mask at native size (no scaling - the caller
 * has already produced the mask at the size it wants). */
void vga_blit_mask(int x, int y, const uint8_t *bits, int w, int h,
                    int stride_bytes, uint64_t fg, uint64_t bg)
{
    if (!bits || w <= 0 || h <= 0) return;
    vga_ensure_backbuffer();
    if (!backbuffer) return;
    gfx_surface_t bb = vga_backbuffer_surface();
    gfx_blit_bitmap1(&bb, x, y, bits, w, h, stride_bytes,
                     (uint32_t)fg, (uint32_t)bg, !vga_bg_is_transparent(bg), 1);
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
    /* One vga_fill_rect() per scanline row (half-width from the circle
     * equation) rather than testing every pixel of the bounding box
     * individually - besides being the obvious algorithmic improvement
     * (O(r) rect fills instead of O(r^2) individual pixel writes), this
     * is what makes a filled circle work AT ALL in GPU draw mode: only
     * vga_fill_rect() is routed to the GPU (see vga_set_gpu_draw()) - a
     * per-pixel vga_put_pixel() loop here would keep drawing into the
     * CPU backbuffer, which is not what GPU draw mode scans out. That
     * exact gap was silently dropping every rounded-rect corner (built
     * from four of these, see vga_fill_rounded_rect() above) in GPU
     * mode, leaving only its two straight cross-shaped fills visible -
     * the "pinched hexagon" button look this fixes. */
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy));
        vga_fill_rect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
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

/* Blends one horizontal span toward `color` by `alpha` (0..1) instead
 * of overwriting it - the "frosted glass" translucency the App Folder
 * overlay uses (see gui_app_folder.c), and the primitive
 * vga_blend_rounded_rect() and vga_dim_screen() below build on.
 *
 * In GPU draw mode this is one draw through the real shader pipeline
 * (see virtio_gpu_virgl_draw_rect_shader()) - the actual blending
 * happens in the GPU's own blend unit against whatever is already in
 * the render target, with no CPU-side pixel readback at all. Outside
 * GPU draw mode it does exactly that readback-blend-writeback, by hand,
 * on the CPU backbuffer - the two paths are expected to look the same,
 * not merely both "work". */
void vga_blend_row(int x, int y, int w, uint64_t color, float alpha) {
    if (w <= 0 || alpha <= 0.0f) return;
    if (vga_gpu_routing_active()) {
        uint32_t a = (uint32_t)(alpha * 255.0f + 0.5f);
        if (a > 255) a = 255;
        uint32_t argb = (a << 24) | ((uint32_t)color & 0x00FFFFFFu);
        virtio_gpu_virgl_draw_rect_shader(x, y, w, 1, argb);
        return;
    }

    extern uint32_t* backbuffer;
    if (!backbuffer) return;
    if (y < 0 || y >= (int)SCREEN_H) return;
    int x0 = x;
    int x1 = x + w;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)SCREEN_W) x1 = (int)SCREEN_W;
    if (x0 >= x1) return;
    int a = (int)(alpha * 256.0f);
    if (a < 0) a = 0;
    if (a > 256) a = 256;
    uint32_t cr = (uint32_t)(color >> 16) & 0xFF, cg = (uint32_t)(color >> 8) & 0xFF, cb = (uint32_t)color & 0xFF;
    uint32_t* row = backbuffer + (size_t)y * (size_t)SCREEN_W;
    for (int px = x0; px < x1; ++px) {
        uint32_t p = row[px];
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        r = (r * (uint32_t)(256 - a) + cr * (uint32_t)a) >> 8;
        g = (g * (uint32_t)(256 - a) + cg * (uint32_t)a) >> 8;
        b = (b * (uint32_t)(256 - a) + cb * (uint32_t)a) >> 8;
        row[px] = (r << 16) | (g << 8) | b;
    }
}

/* A translucent rounded rectangle: same corner-circle shape as
 * vga_fill_rounded_rect() (see that function's own row-by-row inset
 * technique), but blending toward `color` at `alpha` via
 * vga_blend_row() above instead of overwriting - GPU-aware for the
 * same reason vga_blend_row() is, since it is built entirely out of
 * calls to it. */
/* A translucent axis-aligned block: one shader quad in GPU draw mode,
 * row-by-row CPU blending otherwise. */
static void vga_blend_block(int x, int y, int w, int h, uint64_t color, float alpha) {
    if (w <= 0 || h <= 0) return;
    if (vga_gpu_routing_active()) {
        uint32_t a = (uint32_t)(alpha * 255.0f + 0.5f);
        if (a > 255) a = 255;
        virtio_gpu_virgl_draw_rect_shader(x, y, w, h, (a << 24) | ((uint32_t)color & 0x00FFFFFFu));
        return;
    }
    for (int row = 0; row < h; ++row) vga_blend_row(x, y + row, w, color, alpha);
}

void vga_blend_rounded_rect(int x, int y, int w, int h, int r, uint64_t color, float alpha) {
    if (w <= 0 || h <= 0 || alpha <= 0.0f) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    /* Consecutive rows with the same corner inset are blended as one
     * block - all the middle rows share inset 0, so a panel is 2r+1
     * draws instead of h. */
    int run_start = 0, run_inset = -1;
    for (int row = 0; row <= h; ++row) {
        int inset = -1;
        if (row < h) {
            inset = 0;
            if (r > 0) {
                int oy = -1;
                if (row < r) oy = (r - 1) - row;
                else if (row >= h - r) oy = row - (h - r);
                if (oy >= 0) {
                    double dx2 = (double)(r * r - oy * oy);
                    double dx = dx2 > 0.0 ? sqrt(dx2) : 0.0;
                    inset = r - (int)dx;
                    if (inset < 0) inset = 0;
                    if (inset > w / 2) inset = w / 2;
                }
            }
        }
        if (inset != run_inset) {
            if (run_inset >= 0) {
                vga_blend_block(x + run_inset, y + run_start, w - 2 * run_inset,
                                row - run_start, color, alpha);
            }
            run_start = row;
            run_inset = inset;
        }
    }
}

/* Darkens the whole screen toward black by `amount` (0..1) - the modal
 * dim behind an open App Folder overlay. GPU-aware via vga_blend_row()
 * the same way vga_blend_rounded_rect() is; the CPU path is kept as one
 * pass over the whole backbuffer (not row by row through
 * vga_blend_row()) since that is meaningfully cheaper for a full-screen
 * effect and there is no rounding to decompose here. */
void vga_dim_screen(float amount) {
    if (amount <= 0.0f) return;
    if (amount > 1.0f) amount = 1.0f;
    if (vga_gpu_routing_active()) {
        uint32_t a = (uint32_t)(amount * 255.0f + 0.5f);
        if (a > 255) a = 255;
        virtio_gpu_virgl_draw_rect_shader(0, 0, (int)SCREEN_W, (int)SCREEN_H, a << 24 /* black */);
        return;
    }

    extern uint32_t* backbuffer;
    if (!backbuffer) return;
    int keep = (int)((1.0f - amount) * 256.0f);
    if (keep < 0) keep = 0;
    if (keep > 256) keep = 256;
    size_t n = (size_t)SCREEN_W * (size_t)SCREEN_H;
    for (size_t i = 0; i < n; ++i) {
        uint32_t px = backbuffer[i];
        uint32_t r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
        r = (r * (uint32_t)keep) >> 8;
        g = (g * (uint32_t)keep) >> 8;
        b = (b * (uint32_t)keep) >> 8;
        backbuffer[i] = (r << 16) | (g << 8) | b;
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



