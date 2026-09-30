/**
 * virtio_gpu.c - VirtIO-GPU 2D display driver. See virtio_gpu.h for scope.
 *
 * Wire structs below mirror the virtio spec / Linux's
 * include/uapi/linux/virtio_gpu.h exactly (field names, order and
 * sizes) - this is a stable, publicly documented device protocol, not
 * anything specific to this kernel.
 */
#include "virtio_gpu.h"
#include "../../kernel/drivers/virtio_pci.h"
#include "mm/paging.h"
#include "serial.h"
#include "memory.h"
#include <string.h>

#define VIRTIO_GPU_PCI_DEVICE_ID 0x1050u /* 0x1040 + virtio device id 16 (GPU) */

#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D        0x0101u
#define VIRTIO_GPU_CMD_SET_SCANOUT               0x0103u
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH            0x0104u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D       0x0105u
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING   0x0106u

#define VIRTIO_GPU_RESP_OK_NODATA 0x1100u

#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2u

#define VIRTIO_GPU_RESOURCE_ID 1u
#define VIRTIO_GPU_SCANOUT_ID  0u

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
} __attribute__((packed)) virtio_gpu_ctrl_hdr_t;

typedef struct {
    uint32_t x, y, width, height;
} __attribute__((packed)) virtio_gpu_rect_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) virtio_gpu_resource_create_2d_t;

typedef struct {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_mem_entry_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    virtio_gpu_mem_entry_t entries[1];
} __attribute__((packed)) virtio_gpu_resource_attach_backing_1_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed)) virtio_gpu_set_scanout_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_transfer_to_host_2d_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_flush_t;


/* ---- VirGL (3D) protocol -----------------------------------------------
 * Command ids/structs from the virtio spec (virtio_gpu.h) and the
 * Gallium command-stream encoding from virglrenderer's
 * src/virgl_protocol.h. Only what is used below is defined. */
#define VIRTIO_GPU_F_VIRGL_BIT               0x1u

#define VIRTIO_GPU_CMD_GET_CAPSET_INFO        0x0108u
#define VIRTIO_GPU_CMD_CTX_CREATE             0x0200u
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE    0x0202u
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D     0x0204u
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D    0x0205u
#define VIRTIO_GPU_CMD_SUBMIT_3D              0x0207u
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO        0x1102u

#define VIRGL_MAX_REQ 16384u

#define VIRGL_CCMD_CREATE_OBJECT         1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_CLEAR                 7
#define VIRGL_CCMD_BLIT                  16
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 9
#define VIRGL_OBJECT_SURFACE             8
#define VIRGL_CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))

#define PIPE_TEXTURE_2D          2
#define PIPE_CLEAR_COLOR0        (1u << 2)
#define PIPE_MASK_RGBA           0xfu
#define PIPE_TEX_FILTER_NEAREST  0
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1
#define VIRGL_FORMAT_B8G8R8X8_UNORM 2
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW  (1u << 3)
#define VIRGL_BIND_SCANOUT       (1u << 18)

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_index;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_get_capset_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t capset_id;
    uint32_t capset_max_version;
    uint32_t capset_max_size;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resp_capset_info_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed)) virtio_gpu_ctx_create_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_ctx_resource_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_create_3d_t;

typedef struct {
    uint32_t x, y, z, w, h, d;
} __attribute__((packed)) virtio_gpu_box_t;

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_box_t box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed)) virtio_gpu_transfer_host_3d_t;
typedef virtio_gpu_transfer_host_3d_t virtio_gpu_transfer_from_host_3d_t;

static uint32_t g_readback_pixels[16];

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t size;
    uint32_t padding;
    uint32_t cmds[(VIRGL_MAX_REQ - 32) / 4];
} __attribute__((packed)) virtio_gpu_submit_3d_t;

/* A healthy device answers within a handful of iterations once the
 * physical-address bug this driver's development uncovered is fixed
 * (verified: every real round trip during boot testing completed
 * essentially immediately). This is sized to comfortably absorb a
 * slow or loaded host without ever hanging the kernel forever if a
 * device genuinely stops responding - it is a plain spin count, not a
 * timer, so it says nothing about real elapsed time on its own. */
#define VIRTIO_GPU_CMD_SPIN_LIMIT 20000000u

static virtio_pci_dev_t g_vdev;
static virtio_queue_t   g_controlq;
static virtio_queue_t   g_cursorq;
static bool             g_cursorq_ok = false;
static bool g_virgl      = false;
bool g_diag_cursor_watch = false; /* host negotiated VIRTIO_GPU_F_VIRGL */
static bool g_hw_ready   = false; /* device found, negotiated, resource+backing set up */
static bool g_active     = false; /* scanout currently pointed at our resource */
static uint32_t g_width  = 0;
static uint32_t g_height = 0;

static void gpu_hdr_init(virtio_gpu_ctrl_hdr_t* h, uint32_t type) {
    memset(h, 0, sizeof(*h));
    h->type = type;
}

/* Send a request already carrying its own ctrl_hdr and expect a plain
 * VIRTIO_GPU_RESP_OK_NODATA back. Returns false on transport failure,
 * timeout, or any non-OK response type (logged either way). */
static bool gpu_send_expect_ok(const void* req, uint32_t req_len) {
    virtio_gpu_ctrl_hdr_t resp;
    memset(&resp, 0, sizeof(resp));
    if (!virtio_pci_queue_send_and_wait(&g_vdev, &g_controlq, req, req_len,
                                         &resp, sizeof(resp), VIRTIO_GPU_CMD_SPIN_LIMIT)) {
        return false;
    }
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("[virtio-gpu] command failed, response type=0x");
        serial_puthex(resp.type);
        serial_puts("\n");
        return false;
    }
    return true;
}

bool virtio_gpu_probe(void) {
    return pci_get_device(VIRTIO_PCI_VENDOR_ID, VIRTIO_GPU_PCI_DEVICE_ID) != NULL;
}

static bool virtio_gpu_bring_up_hardware(uint32_t width, uint32_t height,
                                          void* backing_va, uint64_t backing_size) {
    if (!g_vdev.valid) {
        if (!virtio_pci_open(VIRTIO_GPU_PCI_DEVICE_ID, &g_vdev)) {
            serial_puts("[virtio-gpu] no VirtIO-GPU PCI device found\n");
            return false;
        }
    } else {
        /* Re-initialising after virtio_gpu_disable() reset the device:
         * the BAR windows are still mapped from the first open, and
         * re-running virtio_pci_open() would try to map them again
         * (the double-map failure fixed in virtio_pci_map_bar()), so
         * only redo the status handshake here. */
        virtio_pci_reset(&g_vdev);
        virtio_pci_add_status(&g_vdev, VIRTIO_STATUS_ACKNOWLEDGE);
        virtio_pci_add_status(&g_vdev, VIRTIO_STATUS_DRIVER);
    }
    uint32_t got_lo = 0;
    if (!virtio_pci_negotiate_features(&g_vdev, VIRTIO_GPU_F_VIRGL_BIT, &got_lo)) {
        serial_puts("[virtio-gpu] feature negotiation failed\n");
        return false;
    }
    g_virgl = (got_lo & VIRTIO_GPU_F_VIRGL_BIT) != 0;
    serial_puts(g_virgl ? "[virtio-gpu] VIRGL (3D) negotiated - host GPU rendering available\n"
                        : "[virtio-gpu] VIRGL not offered by host - 2D only\n");
    /* Largest request we ever send is resource_attach_backing (48
     * bytes with one mem_entry); round up generously. Every response
     * we care about is a plain 24-byte ctrl_hdr. */
    /* 16K request buffer: large enough for a SUBMIT_3D carrying a
     * reasonable Gallium command batch inline (see virgl_submit()),
     * not just the small fixed-size 2D commands. */
    if (!virtio_pci_queue_setup(&g_vdev, 0 /* controlq */, VIRGL_MAX_REQ, 256, &g_controlq)) {
        serial_puts("[virtio-gpu] control queue setup failed\n");
        return false;
    }
    /* cursorq (queue 1): hardware cursor plane. Optional - set up here
     * because queues must be configured before DRIVER_OK. */
    /* Response buffer is 32 bytes, not 16: a cursor command's response
     * is a plain struct virtio_gpu_ctrl_hdr, which is 24 bytes (type,
     * flags, fence_id, ctx_id, ring_idx, padding) - 16 was truncating
     * it, which is likely harmless on its own (this driver never reads
     * the response body), but is exactly the kind of guest/device
     * buffer-size mismatch worth not leaving in place while chasing a
     * real responsiveness bug in this same code path (see
     * cursor_send()'s diagnostic timing above it). */
    g_cursorq_ok = virtio_pci_queue_setup(&g_vdev, 1 /* cursorq */, 64, 32, &g_cursorq);
    virtio_pci_add_status(&g_vdev, VIRTIO_STATUS_DRIVER_OK);

    virtio_gpu_resource_create_2d_t create;
    gpu_hdr_init(&create.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
    create.resource_id = VIRTIO_GPU_RESOURCE_ID;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = width;
    create.height = height;
    if (!gpu_send_expect_ok(&create, sizeof(create))) {
        serial_puts("[virtio-gpu] RESOURCE_CREATE_2D failed\n");
        return false;
    }

    virtio_gpu_resource_attach_backing_1_t attach;
    gpu_hdr_init(&attach.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    attach.resource_id = VIRTIO_GPU_RESOURCE_ID;
    attach.nr_entries = 1;
    /* backing_va is an ordinary kernel pointer (the GUI's own
     * `backbuffer`, see vga_set_render_backend() in vga.c) living in
     * this kernel's low identity-mapped heap, not the separate
     * high-half PHYS_TO_VIRT/VIRT_TO_PHYS alias - paging_virt_to_phys()
     * is the correct resolver here; see the matching comment in
     * virtio_pci_queue_setup() for why VIRT_TO_PHYS() would silently
     * compute a bogus address instead. */
    attach.entries[0].addr = paging_virt_to_phys((uint64_t)(uintptr_t)backing_va);
    attach.entries[0].length = (uint32_t)backing_size;
    attach.entries[0].padding = 0;
    if (!gpu_send_expect_ok(&attach, sizeof(attach))) {
        serial_puts("[virtio-gpu] RESOURCE_ATTACH_BACKING failed\n");
        return false;
    }

    g_width = width;
    g_height = height;
    g_hw_ready = true;
    serial_puts("[virtio-gpu] resource created and backing attached (");
    serial_putdec(width);
    serial_puts("x");
    serial_putdec(height);
    serial_puts(")\n");
    return true;
}

static bool virtio_gpu_point_scanout(uint32_t resource_id) {
    virtio_gpu_set_scanout_t cmd;
    gpu_hdr_init(&cmd.hdr, VIRTIO_GPU_CMD_SET_SCANOUT);
    cmd.r.x = 0;
    cmd.r.y = 0;
    cmd.r.width = g_width;
    cmd.r.height = g_height;
    cmd.scanout_id = VIRTIO_GPU_SCANOUT_ID;
    cmd.resource_id = resource_id;
    return gpu_send_expect_ok(&cmd, sizeof(cmd));
}

bool virtio_gpu_enable(uint32_t width, uint32_t height, void* backing_va, uint64_t backing_size) {
    if (width == 0 || height == 0 || !backing_va || backing_size == 0) return false;

    if (!g_hw_ready) {
        if (!virtio_gpu_bring_up_hardware(width, height, backing_va, backing_size)) {
            return false;
        }
    } else if (width != g_width || height != g_height) {
        /* See the header comment: resizing an already-created resource
         * is not supported. This does not happen in practice since
         * SCREEN_W/H are fixed for the whole boot session. */
        serial_puts("[virtio-gpu] enable() called with a different size than the "
                     "already-created resource - not supported\n");
        return false;
    }

    if (!virtio_gpu_point_scanout(VIRTIO_GPU_RESOURCE_ID)) {
        serial_puts("[virtio-gpu] SET_SCANOUT failed\n");
        g_active = false;
        return false;
    }
    g_active = true;
    return true;
}

static void virtio_gpu_forget_device_state(void);
void virtio_gpu_disable(void) {
    if (!g_hw_ready) return;
    /* A full VirtIO device reset, not SET_SCANOUT(0). On virtio-vga
     * (QEMU's -vga virtio, the one device that is both a legacy VGA
     * adapter GRUB can use and a VirtIO-GPU), once the guest has driven
     * the VirtIO interface QEMU keeps displaying the VirtIO scanout; a
     * scanout pointed at resource 0 is then shown as "Display output is
     * not active" rather than falling back to the legacy framebuffer
     * the CPU backend draws into. A device reset is what hands the
     * display back to VGA. The cost is that every host-side resource
     * (2D target, VirGL context, textures, cursor) is gone, so the next
     * virtio_gpu_enable() re-runs the whole bring-up - which is why
     * virtio_gpu_forget_device_state() clears all of that bookkeeping
     * here rather than leaving stale ids that no longer exist host-side. */
    virtio_pci_reset(&g_vdev);
    virtio_gpu_forget_device_state();
    serial_puts("[virtio-gpu] device reset - display handed back to legacy VGA\n");
}

bool virtio_gpu_is_active(void) {
    return g_hw_ready && g_active;
}

bool virtio_gpu_present_rect(int x, int y, int w, int h) {
    if (!virtio_gpu_is_active()) return false;
    if (w <= 0 || h <= 0) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)g_width || y >= (int)g_height) return false;
    if (x + w > (int)g_width) w = (int)g_width - x;
    if (y + h > (int)g_height) h = (int)g_height - y;
    if (w <= 0 || h <= 0) return false;

    uint64_t stride = (uint64_t)g_width * 4u;
    uint64_t offset = (uint64_t)y * stride + (uint64_t)x * 4u;

    virtio_gpu_transfer_to_host_2d_t xfer;
    gpu_hdr_init(&xfer.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
    xfer.r.x = (uint32_t)x;
    xfer.r.y = (uint32_t)y;
    xfer.r.width = (uint32_t)w;
    xfer.r.height = (uint32_t)h;
    xfer.offset = offset;
    xfer.resource_id = VIRTIO_GPU_RESOURCE_ID;
    xfer.padding = 0;
    if (!gpu_send_expect_ok(&xfer, sizeof(xfer))) return false;

    virtio_gpu_resource_flush_t flush;
    gpu_hdr_init(&flush.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    flush.r = xfer.r;
    flush.resource_id = VIRTIO_GPU_RESOURCE_ID;
    flush.padding = 0;
    if (!gpu_send_expect_ok(&flush, sizeof(flush))) return false;

    return true;
}

/* =====================================================================
 * VirGL: host-GPU rendering via Gallium command streams (SUBMIT_3D).
 * ===================================================================== */

#define VIRGL_CTX_ID          1u
#define VIRGL_RES_SCREEN      2u   /* screen-sized render target + scanout */
#define VIRGL_RES_TILE        3u   /* small CPU-authored texture uploaded to the GPU */
#define VIRGL_RES_COLOR       4u   /* 1x1 texel every GPU fill is stretched from */
#define VIRGL_SURF_SCREEN     10u
#define VIRGL_TILE_W          64u
#define VIRGL_TILE_H          64u

static virtio_gpu_submit_3d_t g_submit;
static uint32_t g_ncmd = 0;
static uint32_t* g_tile_pixels = NULL;

bool virtio_gpu_virgl_available(void) { return g_hw_ready && g_virgl; }

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void virgl_begin(void) { g_ncmd = 0; }
static void virgl_emit(uint32_t v) {
    if (g_ncmd < sizeof(g_submit.cmds) / 4) g_submit.cmds[g_ncmd++] = v;
}
static bool virgl_submit(void) {
    gpu_hdr_init(&g_submit.hdr, VIRTIO_GPU_CMD_SUBMIT_3D);
    g_submit.hdr.ctx_id = VIRGL_CTX_ID;
    g_submit.size = g_ncmd * 4u;
    g_submit.padding = 0;
    return gpu_send_expect_ok(&g_submit, 32u + g_ncmd * 4u);
}

/* Gallium pipe->clear() on the currently bound framebuffer. */
static void virgl_cmd_clear(float r, float g, float b) {
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8));
    virgl_emit(PIPE_CLEAR_COLOR0);
    virgl_emit(f2u(r)); virgl_emit(f2u(g)); virgl_emit(f2u(b)); virgl_emit(f2u(1.0f));
    virgl_emit(0); virgl_emit(0);   /* depth (double) */
    virgl_emit(0);                  /* stencil */
}

/* Gallium pipe->blit(): GPU copies (and scales, if sizes differ) a
 * rectangle of one resource into another. This is the core primitive
 * for GPU-side 2D compositing. */
static void virgl_cmd_blit_ex(uint32_t dst_res, int dx, int dy, int dw, int dh,
                              uint32_t src_res, uint32_t src_fmt, int sx, int sy, int sw, int sh,
                              bool alpha_blend);
static void virgl_cmd_blit(uint32_t dst_res, int dx, int dy, int dw, int dh,
                           uint32_t src_res, int sx, int sy, int sw, int sh) {
    virgl_cmd_blit_ex(dst_res, dx, dy, dw, dh, src_res, VIRGL_FORMAT_B8G8R8X8_UNORM,
                      sx, sy, sw, sh, false);
}
static void virgl_cmd_blit_ex(uint32_t dst_res, int dx, int dy, int dw, int dh,
                              uint32_t src_res, uint32_t src_fmt, int sx, int sy, int sw, int sh,
                              bool alpha_blend) {
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, 21));
    virgl_emit(PIPE_MASK_RGBA | (PIPE_TEX_FILTER_NEAREST << 8) | (alpha_blend ? (1u << 12) : 0u));
    virgl_emit(0); virgl_emit(0);                       /* scissor (disabled) */
    virgl_emit(dst_res); virgl_emit(0); virgl_emit(VIRGL_FORMAT_B8G8R8X8_UNORM);
    virgl_emit((uint32_t)dx); virgl_emit((uint32_t)dy); virgl_emit(0);
    virgl_emit((uint32_t)dw); virgl_emit((uint32_t)dh); virgl_emit(1);
    virgl_emit(src_res); virgl_emit(0); virgl_emit(src_fmt);
    virgl_emit((uint32_t)sx); virgl_emit((uint32_t)sy); virgl_emit(0);
    virgl_emit((uint32_t)sw); virgl_emit((uint32_t)sh); virgl_emit(1);
}

static bool virgl_create_3d_fmt(uint32_t id, uint32_t w, uint32_t h, uint32_t bind, uint32_t fmt);
static bool virgl_create_3d(uint32_t id, uint32_t w, uint32_t h, uint32_t bind) {
    return virgl_create_3d_fmt(id, w, h, bind, VIRGL_FORMAT_B8G8R8X8_UNORM);
}
static bool virgl_create_3d_fmt(uint32_t id, uint32_t w, uint32_t h, uint32_t bind, uint32_t fmt) {
    virtio_gpu_resource_create_3d_t c;
    memset(&c, 0, sizeof(c));
    gpu_hdr_init(&c.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
    c.resource_id = id;
    c.target = PIPE_TEXTURE_2D;
    c.format = fmt;
    c.bind = bind;
    c.width = w; c.height = h; c.depth = 1; c.array_size = 1;
    if (!gpu_send_expect_ok(&c, sizeof(c))) return false;

    virtio_gpu_ctx_resource_t a;
    memset(&a, 0, sizeof(a));
    gpu_hdr_init(&a.hdr, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE);
    a.hdr.ctx_id = VIRGL_CTX_ID;
    a.resource_id = id;
    return gpu_send_expect_ok(&a, sizeof(a));
}

static bool virgl_attach_backing(uint32_t id, void* va, uint64_t size) {
    virtio_gpu_resource_attach_backing_1_t at;
    gpu_hdr_init(&at.hdr, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
    at.resource_id = id;
    at.nr_entries = 1;
    at.entries[0].addr = paging_virt_to_phys((uint64_t)(uintptr_t)va);
    at.entries[0].length = (uint32_t)size;
    at.entries[0].padding = 0;
    return gpu_send_expect_ok(&at, sizeof(at));
}

static bool virgl_upload(uint32_t id, uint32_t w, uint32_t h) {
    virtio_gpu_transfer_host_3d_t t;
    memset(&t, 0, sizeof(t));
    gpu_hdr_init(&t.hdr, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D);
    t.hdr.ctx_id = VIRGL_CTX_ID;
    t.box.w = w; t.box.h = h; t.box.d = 1;
    t.resource_id = id;
    t.stride = w * 4u;
    return gpu_send_expect_ok(&t, sizeof(t));
}


/* Creates (once) the VirGL context, the screen-sized render target that
 * is scanned out while GPU drawing is active, and the 1x1 "colour"
 * texture every GPU fill is sampled from. Idempotent. */
static bool g_virgl_ready = false;
static bool virgl_setup_screen(void) {
    if (g_virgl_ready) return true;
    if (!virtio_gpu_virgl_available()) {
        serial_puts("[virgl] not available (host did not offer VIRGL)\n");
        return false;
    }
    virtio_gpu_get_capset_info_t ci;
    memset(&ci, 0, sizeof(ci));
    gpu_hdr_init(&ci.hdr, VIRTIO_GPU_CMD_GET_CAPSET_INFO);
    ci.capset_index = 0;
    virtio_gpu_resp_capset_info_t cr;
    memset(&cr, 0, sizeof(cr));
    if (virtio_pci_queue_send_and_wait(&g_vdev, &g_controlq, &ci, sizeof(ci),
                                       &cr, sizeof(cr), VIRTIO_GPU_CMD_SPIN_LIMIT) &&
        cr.hdr.type == VIRTIO_GPU_RESP_OK_CAPSET_INFO) {
        serial_puts("[virgl] capset id=");
        serial_putdec(cr.capset_id);
        serial_puts(" max_version=");
        serial_putdec(cr.capset_max_version);
        serial_puts(" max_size=");
        serial_putdec(cr.capset_max_size);
        serial_puts("\n");
    } else {
        serial_puts("[virgl] GET_CAPSET_INFO failed\n");
        return false;
    }

    virtio_gpu_ctx_create_t cc;
    memset(&cc, 0, sizeof(cc));
    gpu_hdr_init(&cc.hdr, VIRTIO_GPU_CMD_CTX_CREATE);
    cc.hdr.ctx_id = VIRGL_CTX_ID;
    const char* nm = "c-os";
    cc.nlen = 4;
    memcpy(cc.debug_name, nm, 4);
    if (!gpu_send_expect_ok(&cc, sizeof(cc))) { serial_puts("[virgl] CTX_CREATE failed\n"); return false; }

    if (!virgl_create_3d(VIRGL_RES_SCREEN, g_width, g_height,
                         VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_SCANOUT)) {
        serial_puts("[virgl] screen RESOURCE_CREATE_3D failed\n"); return false;
    }

    if (!virgl_create_3d(VIRGL_RES_COLOR, 1, 1,
                         VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET)) {
        serial_puts("[virgl] colour RESOURCE_CREATE_3D failed\n"); return false;
    }
    g_virgl_ready = true;
    return true;
}

/* Proves end to end that the HOST GPU (not this kernel's CPU) produces
 * the pixels on screen: every pixel of the final frame comes from a
 * Gallium clear or blit executed by virglrenderer on the host's GL
 * driver. The only CPU-authored pixels are one 64x64 tile, uploaded
 * once as a texture and then replicated/scaled by the GPU. */
bool virtio_gpu_virgl_selftest(void) {
    if (!virgl_setup_screen()) return false;
    if (!virgl_create_3d(VIRGL_RES_TILE, VIRGL_TILE_W, VIRGL_TILE_H,
                         VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET)) {
        serial_puts("[virgl] tile RESOURCE_CREATE_3D failed\n"); return false;
    }

    /* CPU authors one small tile (a diagonal gradient with a border) -
     * the only pixels in this test the CPU writes. */
    g_tile_pixels = (uint32_t*)kmalloc_aligned(VIRGL_TILE_W * VIRGL_TILE_H * 4u, 4096);
    if (!g_tile_pixels) return false;
    for (uint32_t y = 0; y < VIRGL_TILE_H; ++y) {
        for (uint32_t x = 0; x < VIRGL_TILE_W; ++x) {
            uint32_t r = (x * 255u) / VIRGL_TILE_W, g = (y * 255u) / VIRGL_TILE_H, b = 200u;
            if (x < 3 || y < 3 || x >= VIRGL_TILE_W - 3 || y >= VIRGL_TILE_H - 3) r = g = b = 255u;
            g_tile_pixels[y * VIRGL_TILE_W + x] = (r << 16) | (g << 8) | b;
        }
    }
    if (!virgl_attach_backing(VIRGL_RES_TILE, g_tile_pixels, VIRGL_TILE_W * VIRGL_TILE_H * 4u) ||
        !virgl_upload(VIRGL_RES_TILE, VIRGL_TILE_W, VIRGL_TILE_H)) {
        serial_puts("[virgl] tile upload failed\n"); return false;
    }

    virgl_begin();
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    virgl_emit(VIRGL_SURF_SCREEN);
    virgl_emit(VIRGL_RES_SCREEN);
    virgl_emit(VIRGL_FORMAT_B8G8R8X8_UNORM);
    virgl_emit(0);            /* level */
    virgl_emit(0);            /* first/last layer */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    virgl_emit(1);            /* nr_cbufs */
    virgl_emit(0);            /* no depth/stencil surface */
    virgl_emit(VIRGL_SURF_SCREEN);
    virgl_cmd_clear(0.10f, 0.14f, 0.24f);
    /* A row of GPU-scaled copies of the tile: 64 -> 96..256 px. */
    int x = 40;
    for (int i = 0; i < 5; ++i) {
        int sz = 96 + i * 40;
        virgl_cmd_blit(VIRGL_RES_SCREEN, x, 120, sz, sz, VIRGL_RES_TILE, 0, 0, VIRGL_TILE_W, VIRGL_TILE_H);
        x += sz + 16;
    }
    /* A grid of 1:1 copies underneath. */
    for (int gy = 0; gy < 3; ++gy)
        for (int gx = 0; gx < 12; ++gx)
            virgl_cmd_blit(VIRGL_RES_SCREEN, 40 + gx * 80, 460 + gy * 80, 64, 64,
                           VIRGL_RES_TILE, 0, 0, VIRGL_TILE_W, VIRGL_TILE_H);
    if (!virgl_submit()) { serial_puts("[virgl] SUBMIT_3D failed\n"); return false; }
    serial_puts("[virgl] SUBMIT_3D ok: ");
    serial_putdec(g_ncmd);
    serial_puts(" dwords (clear + 41 GPU blits)\n");

    if (!virtio_gpu_point_scanout(VIRGL_RES_SCREEN)) { serial_puts("[virgl] SET_SCANOUT failed\n"); return false; }
    virtio_gpu_resource_flush_t fl;
    gpu_hdr_init(&fl.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    fl.r.x = 0; fl.r.y = 0; fl.r.width = g_width; fl.r.height = g_height;
    fl.resource_id = VIRGL_RES_SCREEN;
    fl.padding = 0;
    if (!gpu_send_expect_ok(&fl, sizeof(fl))) { serial_puts("[virgl] RESOURCE_FLUSH failed\n"); return false; }

    g_active = false; /* the 2D present path must not repoint the scanout during the test */
    serial_puts("[virgl] selftest frame presented from GPU-rendered resource\n");
    return true;
}

/* =====================================================================
 * GPU draw mode: the GUI's rectangle fills execute on the host GPU.
 *
 * Each fill is two Gallium commands appended to the pending SUBMIT_3D
 * batch: RESOURCE_INLINE_WRITE of the fill colour into the 1x1 colour
 * texture, then a BLIT of that texel stretched over the destination
 * rectangle of the screen render target. Because both live in the same
 * ordered command stream, fills land in exactly the order the GUI
 * issued them with no CPU/GPU synchronisation. The inline write is
 * skipped when the colour matches the previous fill's (the texel
 * already holds it), which is common for runs of chrome in one colour.
 * The batch is submitted when it fills up and at present time.
 * ===================================================================== */

static bool     g_virgl_draw = false;
static bool     g_color_valid = false;
static uint32_t g_last_color = 0;
static uint32_t g_frame_fills = 0, g_frame_writes = 0, g_frame_submits = 0;
static uint32_t g_frames = 0;
static uint32_t g_frame_tex_uploads, g_frame_tex_blits;

#define VIRGL_FILL_DWORDS (13u + 22u)

/* Submits whatever is already in g_submit, without touching the pending
 * (not yet emitted) fill below. */
static bool virgl_submit_raw(void) {
    if (g_ncmd == 0) return true;
    bool ok = virgl_submit();
    ++g_frame_submits;
    virgl_begin();
    return ok;
}

/* ---- fill coalescing ------------------------------------------------
 * The GUI issues fills in drawing order, and a large share of them are
 * the same colour and directly adjacent to the previous one: the rows of
 * a filled circle's middle, glyph strokes (see vga_blit_mask_gpu()), the
 * two halves of a rounded rect, window borders drawn edge by edge. The
 * most recent fill is therefore held back as "pending"; a new fill of
 * the same colour that extends it exactly (same x/w and directly below,
 * or same y/h and directly to the right) just grows it. Anything else -
 * a different colour, a non-adjacent rect, a texture blit, a shader
 * draw, a present - emits the pending fill first, so drawing order is
 * never changed. */
static bool g_pend_valid = false;
static int g_pend_x, g_pend_y, g_pend_w, g_pend_h;
static uint32_t g_pend_color;
static uint32_t g_frame_emitted = 0;
/* Fragment-shader cache for translucent draws - see virgl_emit_fs_for_color(). */
#define VIRGL_FS_CACHE      8
#define VIRGL_FS_HANDLE_BASE 40u
static uint32_t g_fs_cache_color[VIRGL_FS_CACHE];
static bool     g_fs_cache_valid[VIRGL_FS_CACHE];
static int      g_fs_cache_next = 0;
static int      g_fs_bound = -1;
static uint32_t g_frame_shader_draws = 0, g_frame_fs_compiles = 0;

static void virgl_emit_fill_now(int x, int y, int w, int h, uint32_t color) {
    if (g_ncmd + VIRGL_FILL_DWORDS > sizeof(g_submit.cmds) / 4) {
        (void)virgl_submit_raw();
    }
    if (!g_color_valid || color != g_last_color) {
        virgl_emit(VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 12));
        virgl_emit(VIRGL_RES_COLOR);
        virgl_emit(0);            /* level */
        virgl_emit(0);            /* usage */
        virgl_emit(4);            /* stride */
        virgl_emit(0);            /* layer_stride */
        virgl_emit(0); virgl_emit(0); virgl_emit(0);   /* x y z */
        virgl_emit(1); virgl_emit(1); virgl_emit(1);   /* w h d */
        virgl_emit(color);        /* one B8G8R8X8 texel */
        g_last_color = color;
        g_color_valid = true;
        ++g_frame_writes;
    }
    virgl_cmd_blit(VIRGL_RES_SCREEN, x, y, w, h, VIRGL_RES_COLOR, 0, 0, 1, 1);
    ++g_frame_emitted;
}

static void virgl_commit_pending_fill(void) {
    if (!g_pend_valid) return;
    g_pend_valid = false;
    virgl_emit_fill_now(g_pend_x, g_pend_y, g_pend_w, g_pend_h, g_pend_color);
}

static bool virgl_flush_batch(void) {
    virgl_commit_pending_fill();
    return virgl_submit_raw();
}

bool virtio_gpu_virgl_draw_active(void) { return g_virgl_draw; }

bool virtio_gpu_virgl_begin_draw(void) {
    if (!virgl_setup_screen()) return false;
    if (!virtio_gpu_point_scanout(VIRGL_RES_SCREEN)) {
        serial_puts("[virgl] SET_SCANOUT to GPU render target failed\n");
        return false;
    }
    g_active = false;          /* 2D present path is not used while GPU draws */
    g_virgl_draw = true;
    g_color_valid = false;
    virgl_begin();
    serial_puts("[virgl] GPU draw mode ON: rectangle fills now execute on the host GPU\n");
    return true;
}

void virtio_gpu_virgl_end_draw(void) {
    if (!g_virgl_draw) return;
    (void)virgl_flush_batch();
    g_virgl_draw = false;
    serial_puts("[virgl] GPU draw mode OFF\n");
}

void virtio_gpu_virgl_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_virgl_draw) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)g_width) w = (int)g_width - x;
    if (y + h > (int)g_height) h = (int)g_height - y;
    if (w <= 0 || h <= 0) return;
    color &= 0x00FFFFFFu;
    ++g_frame_fills;

    if (g_pend_valid && color == g_pend_color) {
        if (x == g_pend_x && w == g_pend_w && y == g_pend_y + g_pend_h) {  /* directly below */
            g_pend_h += h;
            return;
        }
        if (y == g_pend_y && h == g_pend_h && x == g_pend_x + g_pend_w) {  /* directly right */
            g_pend_w += w;
            return;
        }
        if (x >= g_pend_x && y >= g_pend_y &&                             /* fully covered */
            x + w <= g_pend_x + g_pend_w && y + h <= g_pend_y + g_pend_h) {
            return;
        }
    }
    virgl_commit_pending_fill();
    g_pend_valid = true;
    g_pend_x = x; g_pend_y = y; g_pend_w = w; g_pend_h = h;
    g_pend_color = color;
}

bool virtio_gpu_virgl_present(void) {
    extern uint64_t get_timer_ticks(void);
    extern bool g_diag_cursor_watch;
    uint64_t t0 = g_diag_cursor_watch ? get_timer_ticks() : 0;
    if (!g_virgl_draw) return false;
    bool ok = virgl_flush_batch();
    virtio_gpu_resource_flush_t fl;
    gpu_hdr_init(&fl.hdr, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
    fl.r.x = 0; fl.r.y = 0; fl.r.width = g_width; fl.r.height = g_height;
    fl.resource_id = VIRGL_RES_SCREEN;
    fl.padding = 0;
    ok = gpu_send_expect_ok(&fl, sizeof(fl)) && ok;

    ++g_frames;
    if (g_frames <= 3 || (g_frames % 300u) == 0) {
        serial_puts("[virgl] frame ");
        serial_putdec(g_frames);
        serial_puts(": GPU fills=");
        serial_putdec(g_frame_fills);
        serial_puts(" emitted=");
        serial_putdec(g_frame_emitted);
        serial_puts(" colour uploads=");
        serial_putdec(g_frame_writes);
        serial_puts(" shader draws=");
        serial_putdec(g_frame_shader_draws);
        serial_puts(" fs compiles=");
        serial_putdec(g_frame_fs_compiles);
        serial_puts(" tex blits=");
        serial_putdec(g_frame_tex_blits);
        serial_puts(" tex uploads=");
        serial_putdec(g_frame_tex_uploads);
        serial_puts(" submits=");
        serial_putdec(g_frame_submits);
        serial_puts(ok ? " ok\n" : " ERROR\n");
    }
    if (g_diag_cursor_watch && g_frames <= 10) {
        uint64_t t1 = get_timer_ticks();
        serial_puts("[DIAG] virgl_present wall_ms=");
        serial_putdec((uint32_t)(t1 - t0));
        serial_puts("\n");
    }
    g_frame_fills = g_frame_writes = g_frame_submits = 0;
    g_frame_emitted = 0;
    g_frame_shader_draws = g_frame_fs_compiles = 0;
    g_frame_tex_uploads = g_frame_tex_blits = 0;
    return ok;
}

/* =====================================================================
 * GPU texture cache: CPU-authored images (the desktop layer, icons, ...)
 * uploaded once and then placed by GPU blits every frame.
 *
 * Each entry is keyed by an opaque pointer the caller chooses (usually
 * the source buffer) plus a caller-supplied `version`: the image is
 * re-uploaded only when the version or size changes, so a static image
 * costs one TRANSFER_TO_HOST_3D total and a single BLIT per frame.
 * ===================================================================== */

#define VIRTIO_GPU_CMD_RESOURCE_UNREF 0x0102u
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D 0x0206u
#define VIRGL_TEX_MAX      48
#define VIRGL_TEX_ID_BASE  100u

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed)) virtio_gpu_resource_unref_t;

typedef struct {
    const void* key;
    uint32_t    res;
    uint32_t    w, h;
    uint32_t    version;
    bool        alpha;
    uint32_t*   staging;
} virgl_tex_t;

static virgl_tex_t g_tex[VIRGL_TEX_MAX];
static uint32_t g_tex_next_id = VIRGL_TEX_ID_BASE;

static void virgl_tex_release(virgl_tex_t* t) {
    if (t->res) {
        virtio_gpu_resource_unref_t u;
        memset(&u, 0, sizeof(u));
        gpu_hdr_init(&u.hdr, VIRTIO_GPU_CMD_RESOURCE_UNREF);
        u.resource_id = t->res;
        (void)gpu_send_expect_ok(&u, sizeof(u));
    }
    if (t->staging) kfree(t->staging);
    memset(t, 0, sizeof(*t));
}

/* Returns the GPU resource id holding `pixels` (w x h, `stride` pixels
 * per row, 0x00RRGGBB - or 0xAARRGGBB when `alpha`), uploading it first
 * if this key is new or its version/size changed. 0 on failure. */
uint32_t virtio_gpu_virgl_texture(const void* key, const uint32_t* pixels,
                                  int w, int h, int stride, uint32_t version, bool alpha) {
    if (!g_virgl_draw || !key || !pixels || w <= 0 || h <= 0) return 0;
    virgl_tex_t* t = NULL;
    virgl_tex_t* free_slot = NULL;
    for (int i = 0; i < VIRGL_TEX_MAX; ++i) {
        if (g_tex[i].key == key) { t = &g_tex[i]; break; }
        if (!g_tex[i].key && !free_slot) free_slot = &g_tex[i];
    }
    if (t && t->w == (uint32_t)w && t->h == (uint32_t)h && t->alpha == alpha &&
        t->version == version) {
        return t->res;
    }
    /* Anything already queued that samples an older copy of this
     * texture must execute before the upload overwrites it. */
    (void)virgl_flush_batch();

    if (t && (t->w != (uint32_t)w || t->h != (uint32_t)h || t->alpha != alpha)) {
        virgl_tex_release(t);
        free_slot = t;
        t = NULL;
    }
    if (!t) {
        if (!free_slot) {                 /* cache full: evict slot 0 */
            virgl_tex_release(&g_tex[0]);
            free_slot = &g_tex[0];
        }
        t = free_slot;
        t->staging = (uint32_t*)kmalloc_aligned((size_t)w * (size_t)h * 4u, 4096);
        if (!t->staging) { memset(t, 0, sizeof(*t)); return 0; }
        t->res = g_tex_next_id++;
        t->w = (uint32_t)w; t->h = (uint32_t)h; t->alpha = alpha; t->key = key;
        if (!virgl_create_3d_fmt(t->res, t->w, t->h, VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET,
                                 alpha ? VIRGL_FORMAT_B8G8R8A8_UNORM : VIRGL_FORMAT_B8G8R8X8_UNORM) ||
            !virgl_attach_backing(t->res, t->staging, (uint64_t)w * (uint64_t)h * 4u)) {
            serial_puts("[virgl] texture create failed\n");
            virgl_tex_release(t);
            return 0;
        }
    }
    for (int y = 0; y < h; ++y) {
        memcpy(t->staging + (size_t)y * (size_t)w, pixels + (size_t)y * (size_t)stride, (size_t)w * 4u);
    }
    if (!virgl_upload(t->res, t->w, t->h)) {
        serial_puts("[virgl] texture upload failed\n");
        return 0;
    }
    t->version = version;
    ++g_frame_tex_uploads;
    return t->res;
}

void virtio_gpu_virgl_blit_texture(uint32_t res, int sx, int sy, int sw, int sh,
                                   int dx, int dy, int dw, int dh) {
    if (!g_virgl_draw || !res) return;
    virgl_commit_pending_fill();
    bool alpha = false;
    for (int i = 0; i < VIRGL_TEX_MAX; ++i) if (g_tex[i].res == res) { alpha = g_tex[i].alpha; break; }
    if (g_ncmd + 24u > sizeof(g_submit.cmds) / 4) (void)virgl_flush_batch();
    virgl_cmd_blit_ex(VIRGL_RES_SCREEN, dx, dy, dw, dh, res,
                      alpha ? VIRGL_FORMAT_B8G8R8A8_UNORM : VIRGL_FORMAT_B8G8R8X8_UNORM,
                      sx, sy, sw, sh, alpha);
    ++g_frame_tex_blits;
}

/* =====================================================================
 * Hardware cursor plane (virtio-gpu cursorq). The host composites the
 * cursor over the scanout itself, so moving the mouse costs one tiny
 * MOVE_CURSOR message - no redraw, no upload, no present.
 * ===================================================================== */

#define VIRTIO_GPU_CMD_UPDATE_CURSOR 0x0300u
#define VIRTIO_GPU_CMD_MOVE_CURSOR   0x0301u
#define VIRGL_RES_CURSOR             5u
#define VIRGL_RES_PROBE              6u
#define VIRGL_SURF_PROBE             11u
#define VIRGL_CURSOR_DIM             64u

typedef struct {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t scanout_id, x, y, padding0;
    uint32_t resource_id, hot_x, hot_y, padding1;
} __attribute__((packed)) virtio_gpu_update_cursor_t;

static bool g_cursor_ready = false;
static uint32_t* g_cursor_pixels = NULL;
static uint32_t g_cursor_hot_x = 0, g_cursor_hot_y = 0;

static bool cursor_send(uint32_t type, int x, int y, uint32_t res) {
    if (!g_cursorq_ok) return false;
    virtio_gpu_update_cursor_t c;
    memset(&c, 0, sizeof(c));
    gpu_hdr_init(&c.hdr, type);
    c.scanout_id = VIRTIO_GPU_SCANOUT_ID;
    c.x = (uint32_t)(x < 0 ? 0 : x);
    c.y = (uint32_t)(y < 0 ? 0 : y);
    c.resource_id = res;
    c.hot_x = g_cursor_hot_x;
    c.hot_y = g_cursor_hot_y;
    uint8_t dummy[32];
    extern bool g_diag_cursor_watch;
    extern uint64_t get_timer_ticks(void);
    uint64_t t0 = 0;
    static int diag_n = 0;
    bool watch = g_diag_cursor_watch && diag_n < 10;
    if (watch) t0 = get_timer_ticks();
    bool ok = virtio_pci_queue_send_and_wait(&g_vdev, &g_cursorq, &c, sizeof(c),
                                             dummy, sizeof(dummy), VIRTIO_GPU_CMD_SPIN_LIMIT);
    if (watch) {
        uint64_t t1 = get_timer_ticks();
        ++diag_n;
        serial_puts("[DIAG] cursor_send type=");
        serial_putdec(type);
        serial_puts(" ok=");
        serial_putdec(ok ? 1 : 0);
        serial_puts(" ms=");
        serial_putdec((uint32_t)(t1 - t0));
        serial_puts("\n");
    }
    return ok;
}

/* `argb` is a 64x64 0xAARRGGBB image. */
bool virtio_gpu_virgl_cursor_set_image(const uint32_t* argb, int hot_x, int hot_y, int x, int y) {
    if (!g_virgl_draw || !g_cursorq_ok || !argb) return false;
    if (!g_cursor_ready) {
        g_cursor_pixels = (uint32_t*)kmalloc_aligned(VIRGL_CURSOR_DIM * VIRGL_CURSOR_DIM * 4u, 4096);
        if (!g_cursor_pixels) return false;
        if (!virgl_create_3d_fmt(VIRGL_RES_CURSOR, VIRGL_CURSOR_DIM, VIRGL_CURSOR_DIM,
                                 VIRGL_BIND_SAMPLER_VIEW | VIRGL_BIND_RENDER_TARGET,
                                 VIRGL_FORMAT_B8G8R8A8_UNORM) ||
            !virgl_attach_backing(VIRGL_RES_CURSOR, g_cursor_pixels, VIRGL_CURSOR_DIM * VIRGL_CURSOR_DIM * 4u)) {
            serial_puts("[virgl] cursor resource create failed\n");
            return false;
        }
        g_cursor_ready = true;
    }
    memcpy(g_cursor_pixels, argb, VIRGL_CURSOR_DIM * VIRGL_CURSOR_DIM * 4u);
    if (!virgl_upload(VIRGL_RES_CURSOR, VIRGL_CURSOR_DIM, VIRGL_CURSOR_DIM)) return false;
    g_cursor_hot_x = (uint32_t)hot_x;
    g_cursor_hot_y = (uint32_t)hot_y;
    bool ok = cursor_send(VIRTIO_GPU_CMD_UPDATE_CURSOR, x, y, VIRGL_RES_CURSOR);
    serial_puts(ok ? "[virgl] hardware cursor image set (host-composited cursor plane)\n"
                   : "[virgl] UPDATE_CURSOR failed\n");
    return ok;
}

bool virtio_gpu_virgl_cursor_move(int x, int y) {
    if (!g_virgl_draw || !g_cursor_ready) return false;
    return cursor_send(VIRTIO_GPU_CMD_MOVE_CURSOR, x, y, 0);
}

bool virtio_gpu_virgl_cursor_ready(void) { return g_virgl_draw && g_cursor_ready; }

/* =====================================================================
 * Alpha-blit probe: confirms (or refutes) that this host's virglrenderer
 * actually blends VIRGL_CMD_BLIT_S0_ALPHA_BLEND rather than silently
 * ignoring it and doing a plain copy - the one load-bearing assumption
 * "glyphs via alpha blit" depends on. Draws a solid blue rect, then
 * blits a small 50%-alpha red texture over its centre, and reports the
 * resulting colour read back from the resource. Blending predicts
 * ~(127,0,127); an ignored alpha flag predicts pure (255,0,0) - the two
 * are unambiguous enough to tell apart from a single pixel.
 * ===================================================================== */

static uint32_t virgl_readback_pixel(uint32_t res, int x, int y) {
    uint32_t px = 0;
    virtio_gpu_transfer_from_host_3d_t xf;
    memset(&xf, 0, sizeof(xf));
    gpu_hdr_init(&xf.hdr, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D);
    xf.hdr.ctx_id = VIRGL_CTX_ID;
    xf.box.x = (uint32_t)x; xf.box.y = (uint32_t)y; xf.box.w = 1; xf.box.h = 1; xf.box.d = 1;
    xf.resource_id = res;
    xf.stride = 4;
    if (!gpu_send_expect_ok(&xf, sizeof(xf))) {
        serial_puts("[virgl] readback TRANSFER_FROM_HOST_3D failed\n");
        return 0xFFFFFFFFu;
    }
    memcpy(&px, g_readback_pixels, 4);
    return px;
}

bool virtio_gpu_virgl_alpha_blit_selftest(void) {
    if (!virgl_setup_screen()) return false;

    if (!virgl_create_3d(VIRGL_RES_PROBE, 4, 4, VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW)) {
        serial_puts("[virgl] probe target create failed\n"); return false;
    }
    if (!virgl_attach_backing(VIRGL_RES_PROBE, g_readback_pixels, 16u * 4u)) {
        serial_puts("[virgl] probe readback backing attach failed\n"); return false;
    }

    static const uint32_t blue_px[4] = { 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu };
    uint32_t blue_res = virtio_gpu_virgl_texture((const void*)0x1001, blue_px, 4, 1, 4, 1, false);
    static const uint32_t red_half_px = 0x80FF0000u; /* B8G8R8A8: A=0x80 R=0xFF G=0 B=0 */
    uint32_t red_res  = virtio_gpu_virgl_texture((const void*)0x1002, &red_half_px, 1, 1, 1, 1, true);
    if (!blue_res || !red_res) { serial_puts("[virgl] probe texture upload failed\n"); return false; }

    virgl_begin();
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    virgl_emit(VIRGL_SURF_PROBE);
    virgl_emit(VIRGL_RES_PROBE);
    virgl_emit(VIRGL_FORMAT_B8G8R8X8_UNORM);
    virgl_emit(0); virgl_emit(0);
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    virgl_emit(1); virgl_emit(0); virgl_emit(VIRGL_SURF_PROBE);
    virgl_cmd_blit(VIRGL_RES_PROBE, 0, 0, 4, 4, blue_res, 0, 0, 4, 1);
    virgl_cmd_blit_ex(VIRGL_RES_PROBE, 0, 0, 4, 4, red_res, VIRGL_FORMAT_B8G8R8A8_UNORM,
                      0, 0, 1, 1, /*alpha_blend=*/true);
    if (!virgl_submit()) { serial_puts("[virgl] probe SUBMIT_3D failed\n"); return false; }

    uint32_t got = virgl_readback_pixel(VIRGL_RES_PROBE, 1, 1);
    uint32_t b = got & 0xFF, g = (got >> 8) & 0xFF, r = (got >> 16) & 0xFF;
    serial_puts("[virgl] alpha-blit probe pixel = R=");
    serial_putdec(r); serial_puts(" G="); serial_putdec(g); serial_puts(" B="); serial_putdec(b);
    serial_puts(" (blend predicts ~127/0/127, ignored-alpha predicts 255/0/0)\n");

    bool blended = (r > 60 && r < 200) && (b > 60 && b < 200) && g < 40;
    bool overwrote = (r > 220) && (g < 40) && (b < 40);
    serial_puts(blended ? "[virgl] RESULT: host DOES alpha-blend BLIT (plan A viable)\n"
              : overwrote ? "[virgl] RESULT: host IGNORES blit alpha, plain overwrite (plan A not viable as-is)\n"
                          : "[virgl] RESULT: unexpected value - inconclusive\n");
    return blended;
}

/* =====================================================================
 * Plan B: the real Gallium draw pipeline (shaders + blend state), used
 * to fill a rectangle with a flat, alpha-blended colour - the thing the
 * alpha-blit probe just proved BLIT itself cannot do on this host.
 *
 * The full by-hand encoding of this pipeline (two shaders as TGSI text,
 * four pipeline-state objects, a vertex buffer, DRAW_VBO) is the "hard
 * part" of driving virgl without Mesa, and is easy to get wrong in ways
 * that fail silently or get the command stream rejected outright. Every
 * dword below is ported from github.com/go-virtio/gpu (v0.6.1,
 * BSD-3-Clause) gpu3d_draw.go's DrawTriangle, which documents having
 * been VALIDATED against a real virglrenderer (llvmpipe) and fixing at
 * least one real encoding bug that only showed up against a live host
 * (VIRGL_CCMD_BIND_SHADER is 31, not the 32 a naive sequential reading
 * of virgl_protocol.h's enum suggests - the live renderer rejected 32
 * as SET_TESS_STATE with "Illegal command buffer"). Ported rather than
 * copied verbatim (Go -> this file's dword-builder style), but every
 * object-type id, command id, and field order below traces to that
 * source. The one piece their reference does not exercise (it draws
 * opaque triangles) is per-render-target ALPHA BLENDING - the whole
 * reason this pipeline is being built - added to the blend object
 * below and confirmed the same way the BLIT probe already was: draw,
 * then read the resulting pixel back and check it against the blended
 * value a correct implementation must produce.
 * ===================================================================== */

#define VIRGL_OBJECT_BLEND            1u
#define VIRGL_OBJECT_RASTERIZER       2u
#define VIRGL_OBJECT_DSA              3u
#define VIRGL_OBJECT_SHADER           4u
#define VIRGL_OBJECT_VERTEX_ELEMENTS  5u

#define VIRGL_CCMD_BIND_OBJECT         2u
#define VIRGL_CCMD_SET_VIEWPORT_STATE  4u
#define VIRGL_CCMD_SET_VERTEX_BUFFERS  6u
#define VIRGL_CCMD_DRAW_VBO            8u
/* BIND_SHADER=31, not 32 - see this section's header comment. */
#define VIRGL_CCMD_BIND_SHADER        31u

#define PIPE_SHADER_VERTEX    0u
#define PIPE_SHADER_FRAGMENT  1u
#define PIPE_PRIM_TRIANGLES   4u
#define PIPE_BUFFER_TARGET    0u   /* pipe_texture_target PIPE_BUFFER, for the vertex buffer resource */
#define VIRGL_BIND_VERTEX_BUFFER (1u << 4)
#define VIRGL_FORMAT_R32G32B32_FLOAT 30u

/* PIPE_BLENDFACTOR_*: unlike every other constant in this file, these
 * are NOT confirmed against a citable validated source (go-virtio/gpu's
 * DrawTriangle never enables blending) - best-effort from the
 * conventional Gallium ordering (ONE=1, SRC_ALPHA=4, ... inverted
 * factors offset +0x10). Marked loudly because, unlike a wrong command
 * id (which the renderer rejects outright and this driver would notice
 * immediately as a failed SUBMIT_3D), a wrong blend factor silently
 * blends with the WRONG math instead of failing - the readback probe
 * right after this pipeline's first use exists specifically to catch
 * that. If the probe's pixel does not match the expected blend, this
 * is the first place to look. */
#define PIPE_BLENDFACTOR_ONE          0x1u
#define PIPE_BLENDFACTOR_SRC_ALPHA    0x3u
#define PIPE_BLENDFACTOR_INV_SRC_ALPHA 0x13u
#define VIRGL_BLEND_S2_RT_BLEND_ENABLE (1u << 0)
#define VIRGL_BLEND_S2_RT_RGB_FUNC(x)   (((x) & 0x7u) << 1)
#define VIRGL_BLEND_S2_RT_RGB_SRC(x)    (((x) & 0x1fu) << 4)
#define VIRGL_BLEND_S2_RT_RGB_DST(x)    (((x) & 0x1fu) << 9)
#define VIRGL_BLEND_S2_RT_ALPHA_FUNC(x) (((x) & 0x7u) << 14)
#define VIRGL_BLEND_S2_RT_ALPHA_SRC(x)  (((x) & 0x1fu) << 17)
#define VIRGL_BLEND_S2_RT_ALPHA_DST(x)  (((x) & 0x1fu) << 22)
#define VIRGL_BLEND_S2_RT_COLORMASK(x)  (((x) & 0xfu) << 27)
#define PIPE_BLEND_ADD 0u

/* Minimal helpers for building TGSI text without libc float formatting:
 * virgl_ftoa3 renders a 0..1 float to 3 decimal places (ample precision
 * for a fill colour), and virgl_strcpy_at appends a NUL-terminated
 * string, returning how many bytes it wrote (so callers can chain
 * `n += virgl_strcpy_at(buf + n, "...")`). */
static void virgl_ftoa3(float v, char* out) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    int milli = (int)(v * 1000.0f + 0.5f);
    int whole = milli / 1000;
    int frac = milli % 1000;
    out[0] = (char)('0' + whole);
    out[1] = '.';
    out[2] = (char)('0' + (frac / 100));
    out[3] = (char)('0' + ((frac / 10) % 10));
    out[4] = (char)('0' + (frac % 10));
    out[5] = '\0';
}
static int virgl_strcpy_at(char* dst, const char* src) {
    int n = 0;
    while (src[n]) { dst[n] = src[n]; ++n; }
    return n;
}

#define VIRGL_QUAD_VS_HANDLE     20u
#define VIRGL_QUAD_FS_HANDLE     21u
#define VIRGL_QUAD_VE_HANDLE     22u
#define VIRGL_QUAD_RAST_HANDLE   23u
#define VIRGL_QUAD_BLEND_HANDLE  24u
#define VIRGL_QUAD_DSA_HANDLE    25u
#define VIRGL_QUAD_SURF_HANDLE   26u
#define VIRGL_RES_QUAD_VBUF      7u

/* TGSI text: passthrough vertex shader (clip-space position straight
 * through), flat-colour fragment shader with the colour baked into an
 * IMM - both verbatim-equivalent to go-virtio/gpu's vsText/fsTextFor,
 * see this section's header comment. */
static const char VIRGL_QUAD_VS_TEXT[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL OUT[0], POSITION\n"
    "MOV OUT[0], IN[0]\n"
    "END\n";

static void virgl_emit_shader(uint32_t handle, uint32_t shader_type, const char* text) {
    size_t text_len = strlen(text) + 1; /* +1: NUL terminator, counted in offlen */
    uint32_t text_dwords = (uint32_t)((text_len + 3) / 4);
    uint32_t length = 5u + text_dwords;
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, length));
    virgl_emit(handle);
    virgl_emit(shader_type);
    virgl_emit((uint32_t)text_len);
    virgl_emit(4096u); /* shader token budget - generous fixed value, see go-virtio/gpu's shaderTokenBudget */
    virgl_emit(0u);    /* streamout num_outputs = 0 */
    for (uint32_t i = 0; i < text_dwords; ++i) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4; ++b) {
            size_t idx = (size_t)i * 4 + b;
            uint8_t byte = (idx < text_len) ? (uint8_t)text[idx] : 0;
            w |= ((uint32_t)byte) << (b * 8);
        }
        virgl_emit(w);
    }
}

static void virgl_bind_shader(uint32_t handle, uint32_t shader_type) {
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_BIND_SHADER, 0, 2));
    virgl_emit(handle);
    virgl_emit(shader_type);
}

static void virgl_bind_object(uint32_t obj, uint32_t handle) {
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_BIND_OBJECT, obj, 1));
    virgl_emit(handle);
}

static bool g_quad_pipeline_ready = false;
static uint32_t g_quad_vbuf_pixels[4 * 3]; /* 4 verts * (x,y,z) - refilled per draw */

/* Builds the fixed part of the quad pipeline once: both shaders (colour
 * is set per-draw below, not baked in - see the FS text's IMM), vertex
 * elements, rasterizer, blend (with alpha blending enabled, the point
 * of this whole pipeline), and DSA. Everything here is BIND_OBJECT'd
 * once and stays bound; only SET_VIEWPORT_STATE/SET_VERTEX_BUFFERS and
 * the vertex buffer's contents change per draw. */
static bool virgl_quad_pipeline_setup(void) {
    if (g_quad_pipeline_ready) return true;
    if (!virgl_setup_screen()) return false;

    /* RESOURCE_CREATE_3D's `target` field (PIPE_BUFFER=0) is not exposed
     * by virgl_create_3d_fmt() (it always uses PIPE_TEXTURE_2D) - a
     * buffer resource needs PIPE_BUFFER, so this is built directly. */
    {
        virtio_gpu_resource_create_3d_t c;
        memset(&c, 0, sizeof(c));
        gpu_hdr_init(&c.hdr, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
        c.resource_id = VIRGL_RES_QUAD_VBUF;
        c.target = PIPE_BUFFER_TARGET;
        c.format = 0;
        c.bind = VIRGL_BIND_VERTEX_BUFFER;
        c.width = 4u * 12u; c.height = 1; c.depth = 1; c.array_size = 1;
        if (!gpu_send_expect_ok(&c, sizeof(c))) { serial_puts("[virgl] quad vbuf create failed\n"); return false; }
        virtio_gpu_ctx_resource_t a;
        memset(&a, 0, sizeof(a));
        gpu_hdr_init(&a.hdr, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE);
        a.hdr.ctx_id = VIRGL_CTX_ID;
        a.resource_id = VIRGL_RES_QUAD_VBUF;
        if (!gpu_send_expect_ok(&a, sizeof(a))) { serial_puts("[virgl] quad vbuf attach failed\n"); return false; }
    }
    if (!virgl_attach_backing(VIRGL_RES_QUAD_VBUF, g_quad_vbuf_pixels, sizeof(g_quad_vbuf_pixels))) {
        serial_puts("[virgl] quad vbuf backing failed\n"); return false;
    }

    (void)virgl_flush_batch();   /* setup below runs in its own batch */
    virgl_emit_shader(VIRGL_QUAD_VS_HANDLE, PIPE_SHADER_VERTEX, VIRGL_QUAD_VS_TEXT);
    virgl_bind_shader(VIRGL_QUAD_VS_HANDLE, PIPE_SHADER_VERTEX);

    /* Vertex elements: one element, position only (x,y,z), matching
     * VIRGL_QUAD_VS_TEXT's single IN[0]. */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, 5));
    virgl_emit(VIRGL_QUAD_VE_HANDLE);
    virgl_emit(0); virgl_emit(0); virgl_emit(0);
    virgl_emit(VIRGL_FORMAT_R32G32B32_FLOAT);
    virgl_bind_object(VIRGL_OBJECT_VERTEX_ELEMENTS, VIRGL_QUAD_VE_HANDLE);

    /* Rasterizer: DEPTH_CLIP | HALF_PIXEL_CENTER, no culling. */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_RASTERIZER, 9));
    virgl_emit(VIRGL_QUAD_RAST_HANDLE);
    virgl_emit((1u << 1) | (1u << 29));
    virgl_emit(f2u(1.0f)); virgl_emit(0); virgl_emit(0);
    virgl_emit(f2u(1.0f)); virgl_emit(f2u(0.0f)); virgl_emit(f2u(0.0f)); virgl_emit(f2u(0.0f));
    virgl_bind_object(VIRGL_OBJECT_RASTERIZER, VIRGL_QUAD_RAST_HANDLE);

    /* Blend: RT0 with blending ENABLED, SRC_ALPHA / INV_SRC_ALPHA, ADD,
     * full colour mask - the one addition go-virtio/gpu's reference
     * pipeline does not make (see this section's header comment on the
     * blend-factor values' confidence level). */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_BLEND, 11));
    virgl_emit(VIRGL_QUAD_BLEND_HANDLE);
    virgl_emit(0x1u); /* S0: independent_blend_enable=1 - try in case RT0 alone is not enough */
    virgl_emit(0);
    virgl_emit(VIRGL_BLEND_S2_RT_BLEND_ENABLE |
              VIRGL_BLEND_S2_RT_RGB_FUNC(PIPE_BLEND_ADD) |
              VIRGL_BLEND_S2_RT_RGB_SRC(PIPE_BLENDFACTOR_SRC_ALPHA) |
              VIRGL_BLEND_S2_RT_RGB_DST(PIPE_BLENDFACTOR_INV_SRC_ALPHA) |
              VIRGL_BLEND_S2_RT_ALPHA_FUNC(PIPE_BLEND_ADD) |
              VIRGL_BLEND_S2_RT_ALPHA_SRC(PIPE_BLENDFACTOR_ONE) |
              VIRGL_BLEND_S2_RT_ALPHA_DST(PIPE_BLENDFACTOR_INV_SRC_ALPHA) |
              VIRGL_BLEND_S2_RT_COLORMASK(0xFu));
    for (int i = 0; i < 7; ++i) virgl_emit(0);
    virgl_bind_object(VIRGL_OBJECT_BLEND, VIRGL_QUAD_BLEND_HANDLE);

    /* Render-target surface over the screen resource, created once. */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5));
    virgl_emit(VIRGL_QUAD_SURF_HANDLE);
    virgl_emit(VIRGL_RES_SCREEN);
    virgl_emit(VIRGL_FORMAT_B8G8R8X8_UNORM);
    virgl_emit(0); virgl_emit(0);

    /* DSA: depth/stencil/alpha-test all disabled. */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_DSA, 5));
    virgl_emit(VIRGL_QUAD_DSA_HANDLE);
    virgl_emit(0); virgl_emit(0); virgl_emit(0); virgl_emit(0);
    virgl_bind_object(VIRGL_OBJECT_DSA, VIRGL_QUAD_DSA_HANDLE);

    bool setup_ok = virgl_submit();
    virgl_begin();
    if (!setup_ok) { serial_puts("[virgl] quad pipeline setup SUBMIT_3D failed\n"); return false; }
    g_fs_bound = -1;
    for (int i = 0; i < VIRGL_FS_CACHE; ++i) g_fs_cache_valid[i] = false;
    g_quad_pipeline_ready = true;
    return true;
}

/* Draws one axis-aligned rectangle [x,y,x+w,y+h) filled with `color`
 * (0xAARRGGBB - alpha honoured, via the blend state set up above) onto
 * VIRGL_RES_SCREEN, using the real Gallium draw pipeline rather than
 * BLIT. Two triangles (as a triangle strip: TL, TR, BL, BR), clip-space
 * XY computed on the CPU from the pixel rect and the same viewport
 * transform go-virtio/gpu's DrawTriangle uses (scale/translate =
 * dimension/2), so the vertex shader's straight passthrough lands
 * exactly on the intended pixels. */
/* Fragment shaders are per colour (the colour is an IMM in the TGSI), so
 * compiling one is the expensive part of a translucent draw. They are
 * kept in a small cache keyed by colour, each under its own object
 * handle, and re-bound instead of re-created when a colour repeats -
 * which is the normal case: every row of one translucent panel, and the
 * same panel again next frame. */

static void virgl_emit_fs_for_color(uint32_t color) {
    int slot = -1;
    for (int i = 0; i < VIRGL_FS_CACHE; ++i) {
        if (g_fs_cache_valid[i] && g_fs_cache_color[i] == color) { slot = i; break; }
    }
    if (slot < 0) {
        slot = g_fs_cache_next;
        g_fs_cache_next = (g_fs_cache_next + 1) % VIRGL_FS_CACHE;
        float r = (float)((color >> 16) & 0xFF) / 255.0f;
        float g = (float)((color >> 8) & 0xFF) / 255.0f;
        float b = (float)(color & 0xFF) / 255.0f;
        float a = (float)((color >> 24) & 0xFF) / 255.0f;
        char fs_text[192];
        char rb[16], gb[16], bb[16], ab[16];
        virgl_ftoa3(r, rb); virgl_ftoa3(g, gb); virgl_ftoa3(b, bb); virgl_ftoa3(a, ab);
        int n = 0;
        n += virgl_strcpy_at(fs_text + n, "FRAG\nDCL OUT[0], COLOR\nIMM[0] FLT32 { ");
        n += virgl_strcpy_at(fs_text + n, rb); n += virgl_strcpy_at(fs_text + n, ", ");
        n += virgl_strcpy_at(fs_text + n, gb); n += virgl_strcpy_at(fs_text + n, ", ");
        n += virgl_strcpy_at(fs_text + n, bb); n += virgl_strcpy_at(fs_text + n, ", ");
        n += virgl_strcpy_at(fs_text + n, ab); n += virgl_strcpy_at(fs_text + n, " }\n");
        n += virgl_strcpy_at(fs_text + n, "MOV OUT[0], IMM[0]\nEND\n");
        fs_text[n] = '\0'; /* virgl_strcpy_at() does not NUL-terminate - see the
                            * "Illegal command buffer" history in this file */
        virgl_emit_shader(VIRGL_FS_HANDLE_BASE + (uint32_t)slot, PIPE_SHADER_FRAGMENT, fs_text);
        g_fs_cache_color[slot] = color;
        g_fs_cache_valid[slot] = true;
        if (g_fs_bound == slot) g_fs_bound = -1;   /* object replaced: re-bind below */
        ++g_frame_fs_compiles;
    }
    if (g_fs_bound != slot) {
        virgl_bind_shader(VIRGL_FS_HANDLE_BASE + (uint32_t)slot, PIPE_SHADER_FRAGMENT);
        g_fs_bound = slot;
    }
}

/* Draws one axis-aligned rectangle filled with `color` (0xAARRGGBB, alpha
 * honoured by the blend state from virgl_quad_pipeline_setup()) onto
 * VIRGL_RES_SCREEN through the real draw pipeline. Everything is appended
 * to the ordered command stream - vertex data included, via
 * RESOURCE_INLINE_WRITE into the vertex buffer - so a translucent draw no
 * longer costs a control-queue round trip and a submit of its own; it is
 * submitted with everything else at present time (or when the batch
 * fills). Framebuffer/viewport/vertex-buffer state is re-emitted per draw
 * (a few dwords) rather than assumed to survive the BLITs that plain
 * fills interleave with it. */
bool virtio_gpu_virgl_draw_rect_shader(int x, int y, int w, int h, uint32_t color) {
    if (!virgl_quad_pipeline_setup()) return false;
    if (w <= 0 || h <= 0) return false;

    /* Anything drawn before this must stay before it. */
    virgl_commit_pending_fill();
    if (g_ncmd + 160u > sizeof(g_submit.cmds) / 4) (void)virgl_submit_raw();

    float sw = (float)g_width, sh = (float)g_height;
    float x0 = (2.0f * (float)x / sw) - 1.0f;
    float y0 = (2.0f * (float)y / sh) - 1.0f;
    float x1 = (2.0f * (float)(x + w) / sw) - 1.0f;
    float y1 = (2.0f * (float)(y + h) / sh) - 1.0f;
    float verts[4 * 3] = {        /* TL, TR, BL, BR - one triangle strip */
        x0, y0, 0.0f,
        x1, y0, 0.0f,
        x0, y1, 0.0f,
        x1, y1, 0.0f,
    };

    virgl_emit_fs_for_color(color);

    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3));
    virgl_emit(1); virgl_emit(0); virgl_emit(VIRGL_QUAD_SURF_HANDLE);

    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, 7));
    virgl_emit(0);
    virgl_emit(f2u(sw / 2.0f)); virgl_emit(f2u(sh / 2.0f)); virgl_emit(f2u(0.5f));
    virgl_emit(f2u(sw / 2.0f)); virgl_emit(f2u(sh / 2.0f)); virgl_emit(f2u(0.5f));

    /* Vertex data, in-stream: box.x is a byte offset for a buffer. */
    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0, 11 + 12));
    virgl_emit(VIRGL_RES_QUAD_VBUF);
    virgl_emit(0); virgl_emit(0); virgl_emit(0); virgl_emit(0);   /* level usage stride layer_stride */
    virgl_emit(0); virgl_emit(0); virgl_emit(0);                  /* x y z */
    virgl_emit(sizeof(verts)); virgl_emit(1); virgl_emit(1);      /* w h d */
    for (int i = 0; i < 12; ++i) virgl_emit(f2u(verts[i]));

    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, 3));
    virgl_emit(12u); virgl_emit(0u); virgl_emit(VIRGL_RES_QUAD_VBUF);

    virgl_emit(VIRGL_CMD0(VIRGL_CCMD_DRAW_VBO, 0, 12));
    virgl_emit(0); virgl_emit(4); virgl_emit(5 /* PIPE_PRIM_TRIANGLE_STRIP */);
    virgl_emit(0); virgl_emit(1); virgl_emit(0); virgl_emit(0);
    virgl_emit(0); virgl_emit(0); virgl_emit(0); virgl_emit(0xFFFFFFFFu); virgl_emit(0);

    ++g_frame_shader_draws;
    return true;
}

/* Same probe shape as virtio_gpu_virgl_alpha_blit_selftest() (blue
 * background, 50%-alpha red on top, read back one pixel), but through
 * the real draw pipeline instead of BLIT - the empirical check for the
 * one inferred piece in virgl_quad_pipeline_setup(), the blend-factor
 * enum values (see that function's comment). */
bool virtio_gpu_virgl_shader_blend_selftest(void) {
    if (!virgl_quad_pipeline_setup()) return false;

    if (!virtio_gpu_virgl_draw_rect_shader(0, 0, (int)g_width, (int)g_height, 0xFF0000FFu)) {
        serial_puts("[virgl] shader-blend probe: background draw failed\n");
        return false;
    }
    if (!virtio_gpu_virgl_draw_rect_shader(10, 10, 20, 20, 0x80FF0000u)) {
        serial_puts("[virgl] shader-blend probe: blended draw failed\n");
        return false;
    }

    if (!virgl_create_3d(VIRGL_RES_PROBE, 1, 1, VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW)) {
        serial_puts("[virgl] shader-blend probe: readback target create failed\n");
        return false;
    }
    if (!virgl_attach_backing(VIRGL_RES_PROBE, g_readback_pixels, 4u)) {
        serial_puts("[virgl] shader-blend probe: readback backing failed\n");
        return false;
    }
    virgl_commit_pending_fill();
    virgl_cmd_blit(VIRGL_RES_PROBE, 0, 0, 1, 1, VIRGL_RES_SCREEN, 15, 15, 1, 1);
    if (!virgl_submit_raw()) { serial_puts("[virgl] shader-blend probe: copy-out BLIT failed\n"); return false; }

    uint32_t got = virgl_readback_pixel(VIRGL_RES_PROBE, 0, 0);
    uint32_t b = got & 0xFF, g = (got >> 8) & 0xFF, r = (got >> 16) & 0xFF;
    serial_puts("[virgl] shader-blend probe pixel = R=");
    serial_putdec(r); serial_puts(" G="); serial_putdec(g); serial_puts(" B="); serial_putdec(b);
    serial_puts(" (blend predicts ~127/0/127)\n");

    bool blended = (r > 60 && r < 200) && (b > 60 && b < 200) && g < 40;
    serial_puts(blended ? "[virgl] RESULT: shader pipeline DOES alpha-blend correctly (plan B viable)\n"
                        : "[virgl] RESULT: unexpected value - blend factor constants likely wrong\n");
    return blended;
}


/* Every piece of host-side-resource bookkeeping in this file, cleared
 * after a device reset (see virtio_gpu_disable()) so nothing refers to
 * a resource/context the host no longer has. Texture staging buffers
 * are freed; their resources died with the reset, so no UNREF is sent. */
static void virtio_gpu_forget_device_state(void) {
    g_hw_ready = false;
    g_active = false;
    g_virgl_ready = false;
    g_virgl_draw = false;
    g_color_valid = false;
    g_pend_valid = false;
    g_quad_pipeline_ready = false;
    g_fs_bound = -1;
    for (int i = 0; i < VIRGL_FS_CACHE; ++i) g_fs_cache_valid[i] = false;
    g_cursor_ready = false;
    g_cursorq_ok = false;
    g_ncmd = 0;
    for (int i = 0; i < VIRGL_TEX_MAX; ++i) {
        if (g_tex[i].staging) kfree(g_tex[i].staging);
    }
    memset(g_tex, 0, sizeof(g_tex));
    g_tex_next_id = VIRGL_TEX_ID_BASE;
}
