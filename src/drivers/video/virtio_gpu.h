/**
 * virtio_gpu.h - VirtIO-GPU 2D display driver.
 *
 * Scope, stated plainly: this drives the VirtIO-GPU device's *2D* mode
 * only (create a linear resource, attach guest RAM as its backing,
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH to present) - exactly the part
 * of the spec QEMU's `-device virtio-gpu-pci` needs for a plain
 * framebuffer, and exactly what C-OS's GUI (all of it goes through
 * vga_flip()/vga_flip_rect(), see vga.c) needs to hand its already-
 * rasterized backbuffer to the GPU for scanout. It deliberately does
 * NOT implement VIRTIO_GPU_F_VIRGL / any 3D command (CTX_CREATE,
 * RESOURCE_CREATE_3D, SUBMIT_3D, ...): every pixel is still rasterized
 * by the CPU exactly as before (fill_rect, draw_text, blits - see
 * gfx_blit.c), and the GPU's job here is limited to owning the actual
 * scanout/presentation step so C-OS can drive a `-device
 * virtio-gpu-pci` output that has no legacy VGA/VBE framebuffer BAR at
 * all (see the long comment in vga_set_render_backend() in vga.c for
 * why that matters). Hardware-accelerated *rasterization* (fill/blit
 * done BY the GPU rather than just displayed BY it) would require the
 * 3D/virgl path, which is real, separate future work.
 *
 * One resource (id 1), one scanout (id 0), one fixed size for the
 * whole boot session - this driver does not support resizing the
 * resource after virtio_gpu_enable() succeeds, since C-OS's own
 * SCREEN_W/SCREEN_H never change after boot (only the bitmap font's
 * integer scale does - see vga_set_font_scale()).
 */
#ifndef COS_VIRTIO_GPU_H
#define COS_VIRTIO_GPU_H

#include "types.h"

/* Existence check only - looks for the PCI device, touches no state
 * and does not require memory/paging to be ready yet. Lets callers
 * (e.g. the Settings GUI) show whether GPU mode is even offerable
 * without the side effects a real virtio_gpu_enable() attempt has. */
bool virtio_gpu_probe(void);

/* One-time bring-up: find the device, negotiate features, set up the
 * control queue, create a width x height linear 2D resource in
 * VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM (matching this kernel's own 32bpp
 * 0x00RRGGBB pixel layout byte-for-byte - see vga_color_to_u32() in
 * vga.c - so presenting never needs a pixel-format conversion pass),
 * attach backing_va (backing_size bytes, must be physically contiguous
 * - an ordinary kmalloc'd block satisfies this on this kernel's flat
 * direct-mapped physical memory model) as its backing store, and point
 * scanout 0 at it.
 *
 * Safe to call again after virtio_gpu_disable(): re-enables the
 * existing resource/backing rather than recreating them. Calling it
 * again with different width/height than a previous successful call
 * in the same boot is NOT supported and returns false (see the file
 * header comment - this never happens in practice since SCREEN_W/H
 * are fixed at boot).
 *
 * Returns false on any failure (device absent, feature/queue setup
 * failed, a GPU command errored) - in every such case no persistent
 * state is left active and the caller should keep using the CPU
 * present path. */
bool virtio_gpu_enable(uint32_t width, uint32_t height,
                        void* backing_va, uint64_t backing_size);

/* Point scanout 0 back at "nothing" (resource_id 0), without
 * destroying the resource or its attached backing - so a later
 * virtio_gpu_enable() call is cheap. Safe to call even if never
 * enabled. */
void virtio_gpu_disable(void);

/* True only while scanout 0 is actually pointed at our resource (i.e.
 * between a successful virtio_gpu_enable() and the next
 * virtio_gpu_disable(), or permanently false if the device/init never
 * succeeded at all). */
bool virtio_gpu_is_active(void);

/* Present the rect (x,y,w,h) of the backing buffer last passed to
 * virtio_gpu_enable(): TRANSFER_TO_HOST_2D copies that sub-rect from
 * guest RAM into the host-side resource, then RESOURCE_FLUSH tells the
 * device to actually scan it out. The rect is clamped to the
 * resource's width/height. Returns false (nothing displayed) if not
 * currently active or if either command failed/timed out - callers
 * should treat that as "this frame did not reach the screen", not as
 * a fatal error. */
bool virtio_gpu_present_rect(int x, int y, int w, int h);

/* VirGL (host 3D) - see virtio_gpu.c. */
bool virtio_gpu_virgl_available(void);
bool virtio_gpu_virgl_selftest(void);

/* GPU draw mode: while active, the scanout shows a host-GPU render
 * target and virtio_gpu_virgl_fill_rect() draws into it with Gallium
 * commands. virtio_gpu_virgl_present() submits the pending batch and
 * flushes the frame to the display. */
bool virtio_gpu_virgl_begin_draw(void);
void virtio_gpu_virgl_end_draw(void);
bool virtio_gpu_virgl_draw_active(void);
void virtio_gpu_virgl_fill_rect(int x, int y, int w, int h, uint32_t color);
bool virtio_gpu_virgl_present(void);

/* GPU texture cache + placement - see virtio_gpu.c. */
uint32_t virtio_gpu_virgl_texture(const void* key, const uint32_t* pixels,
                                  int w, int h, int stride, uint32_t version, bool alpha);
void virtio_gpu_virgl_blit_texture(uint32_t res, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh);

/* Hardware cursor plane (host-composited). */
bool virtio_gpu_virgl_cursor_set_image(const uint32_t* argb64, int hot_x, int hot_y, int x, int y);
bool virtio_gpu_virgl_cursor_move(int x, int y);
bool virtio_gpu_virgl_cursor_ready(void);

/* One-shot diagnostic - see virtio_gpu.c. */
bool virtio_gpu_virgl_alpha_blit_selftest(void);

/* Real Gallium draw pipeline (shaders + blend) - see virtio_gpu.c. */
bool virtio_gpu_virgl_draw_rect_shader(int x, int y, int w, int h, uint32_t color);
bool virtio_gpu_virgl_shader_blend_selftest(void);

#endif /* COS_VIRTIO_GPU_H */
