/**
 * virtio_gpu.h - VirtIO-GPU 2D/VirGL display driver.
 *
 * The driver supports a guaranteed VirtIO-GPU 2D scanout fallback and an
 * optional VirGL 3D path. When VIRTIO_GPU_F_VIRGL is negotiated, the driver
 * creates host-GPU render targets, caches guest-authored textures, submits
 * Gallium command streams and uses TGSI fragment shaders for rectangle fills
 * and composition. The 2D path remains available for hosts that do not
 * expose VirGL; it presents the CPU-authored backbuffer with
 * TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
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

typedef struct {
    uint32_t width, height;
    uint32_t stride_y, stride_u, stride_v;
    const uint8_t *y, *u, *v;
    uint64_t pts_ms;
    uint32_t matrix, range;
} cos_video_i420_t;

/* Uploads I420 planes as three R8_UNORM VirGL resources. This function does
 * not perform CPU YUV->RGBA conversion; it is the staging step for the
 * fragment-shader presentation path and returns false on any host/resource
 * failure so the caller can use the software fallback. */
bool virtio_gpu_virgl_i420_upload(const cos_video_i420_t *video);
bool virtio_gpu_virgl_i420_draw(const cos_video_i420_t *video, int x, int y, int w, int h);
bool virtio_gpu_virgl_i420_shader_selftest(void);

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

/* VirGL video command stream. These functions only build the standard
 * VIRGL_CCMD_* video packets; the host must expose virglrenderer video
 * support (and a VA-API backend) for them to execute. On older QEMU or a
 * host without video support the query returns false and callers must use
 * the software decoder path. */
bool virtio_gpu_virgl_video_available(void);
bool virtio_gpu_virgl_video_create_resource(uint32_t resource_id,
                                            void *backing, uint32_t bytes);
bool virtio_gpu_virgl_video_upload_resource(uint32_t resource_id,
                                            uint32_t bytes);
bool virtio_gpu_virgl_video_create_codec(uint32_t handle, uint32_t profile,
                                         uint32_t entrypoint, uint32_t chroma,
                                         uint32_t level, uint32_t width,
                                         uint32_t height, uint32_t max_ref);
bool virtio_gpu_virgl_video_destroy_codec(uint32_t handle);
bool virtio_gpu_virgl_video_create_buffer(uint32_t handle, uint32_t format,
                                          uint32_t width, uint32_t height,
                                          const uint32_t *resource_ids,
                                          uint32_t resource_count);
bool virtio_gpu_virgl_video_destroy_buffer(uint32_t handle);
bool virtio_gpu_virgl_video_begin_frame(uint32_t codec, uint32_t target);
bool virtio_gpu_virgl_video_decode_bitstream(uint32_t codec, uint32_t target,
                                             uint32_t descriptor_resource,
                                             uint32_t bitstream_resource,
                                             uint32_t bitstream_bytes);
bool virtio_gpu_virgl_video_end_frame(uint32_t codec, uint32_t target);
bool virtio_gpu_virgl_video_submit(void);

#endif /* COS_VIRTIO_GPU_H */
