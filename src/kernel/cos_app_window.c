/**
 * cos_app_window.c - window surface for ring3 ".c-os" programs.
 *
 * THREADING MODEL (the reason this is a command queue and not direct
 * drawing)
 * ---------------------------------------------------------------------
 * A ring3 program runs on its own preemptively-scheduled thread, while
 * all GUI painting happens on the owner thread inside gui_update(). If a
 * syscall painted directly, a user program could be preempted halfway
 * through a draw, or paint concurrently with the compositor - corrupting
 * the backbuffer or racing the window list.
 *
 * So syscalls do not draw. They append a bounded list of rectangles to a
 * per-window queue under IRQ-disable, and the GUI thread replays that
 * queue when it paints the window. All actual drawing stays on the one
 * thread that is allowed to do it, and a user program cannot stall or
 * corrupt the compositor no matter what it submits.
 *
 * The queue is fixed-size and submissions past the cap are dropped
 * (counted, not silently ignored): a hostile program cannot make the
 * kernel allocate without bound, and cannot make the GUI thread do
 * unbounded work in one frame.
 */
#include "cos_app_window.h"
#include "memory.h"
#include "mm/paging.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "scheduler.h"
#include "sync.h"
#include "gui.h"
#include "vga.h"
#include "timer.h"

extern void gui_bring_to_front(int idx);

#define COS_APP_MAX_WINDOWS 16
#define COS_APP_MAX_RECTS   256
#define COS_APP_MAX_TEXT_CMDS 128
#define COS_APP_MAX_TEXT_LEN  63
#define COS_APP_MAX_INPUT_QUEUE 512   /* key events and the unified event queue: a busy app (the terminal while it runs a command) must not lose typed or pasted text */

typedef struct {
    int32_t  x, y, w, h;
    uint32_t color;
} cos_app_rect_t;

/* Text command: kept as a SEPARATE array from rects rather than one
 * interleaved command stream. Simplification, stated plainly: every
 * frame draws all queued rects first, then all queued text, so a program
 * that wants "background rect, then text on top of it" (the ordinary
 * case, and all this session's test app needs) works correctly, but a
 * program wanting text UNDER a later rect within the same frame cannot
 * express that. Revisit with a single ordered command stream if a real
 * use case needs it. */
typedef struct {
    int32_t  x, y;
    uint32_t fg, bg;
    uint8_t  len;
    char     text[COS_APP_MAX_TEXT_LEN + 1];
} cos_app_text_cmd_t;

typedef struct {
    bool           used;
    uint8_t        shell_layer;  /* 0 = an ordinary window; 1 = ring-3 taskbar, 2 = ring-3 start menu (no window_t, drawn by the compositor itself) */
    int32_t        shell_x, shell_y;
    bool           shell_visible;
    cos_shell_icon_t icons_staged[COS_SHELL_MAX_ICONS], icons_live[COS_SHELL_MAX_ICONS];
    int            n_icons_staged, n_icons_live;
    uint32_t       tag_staged, tag_live;     /* an opaque number the owner attaches to a frame (SYS_SHELL_ICONS) */
    uint32_t       win_uid;      /* window_t.uid - NOT a window_t*: see gui.h */
    /* ---- v2: shared pixel buffer (SYS_WIN2_*) ---- */
    uint32_t      *pixels;       /* kernel address (identity-mapped heap) - the app's
                                   * OWN buffer, mapped RW into its address space. The
                                   * compositor must never read this one directly: the
                                   * app can be mid-write to it at any instant (it is
                                   * preempted like any other thread), including
                                   * mid-glyph or mid-rect, with no lock between it and
                                   * the GUI thread's own, independently scheduled
                                   * render tick. See present_pixels below. */
    int32_t        pix_w, pix_h;
    uint64_t       user_va;      /* where the same pages are mapped for the owner */
    uint64_t       pix_bytes;    /* page-rounded allocation size */
    /* The compositor's copy: cos_app_window_present() snapshots `pixels`
     * into this kernel-only buffer in one memcpy, and cos_app_window_draw()
     * composites from THIS buffer, never from `pixels`. That memcpy is the
     * only "lock" needed - it turns "the app's buffer, torn or not,
     * whenever the GUI thread happens to look" into "the last frame the
     * app ever finished and explicitly presented", which is what
     * SYS_WIN2_PRESENT is supposed to mean. Without it, a window with
     * enough draw calls per frame to not fit in one scheduler timeslice
     * (File Manager's icon grid, Studio, any busy ring-3 UI) would
     * occasionally get composited mid-frame - a half-drawn window flashed
     * onto the screen for one tick - which reads as flicker, worst right
     * when a window first opens and does its heaviest initial draw. */
    uint32_t      *present_pixels;
    uint64_t       present_capacity;
    uint32_t       present_version;
    /* Optional decoded video plane. The compositor uploads/scales this
     * buffer through VirGL instead of asking ring-3 to rasterise it into
     * the window surface. */
    uint32_t      *video_pixels;
    uint64_t       video_capacity;
    /* Zero-copy video: two page-aligned RGBA buffers mapped into the owner's address space
     * (SYS_WIN2_VIDEO_MAP). The app converts a decoded picture straight into the one that is not
     * on screen - from any of its cores - and SYS_WIN2_VIDEO_COMMIT flips video_pixels to it, so
     * the kernel never copies or converts the frame. While video_pixels points at one of these,
     * video_ext is set and the other paths must not free it. */
    uint32_t      *vbuf[2];
    uint64_t       vbuf_bytes;      /* page-rounded size of each */
    uint64_t       vbuf_va;         /* user address of vbuf[0]; vbuf[1] follows */
    bool           video_ext;
    int32_t        video_w, video_h, video_stride;
    int32_t        video_x, video_y, video_dst_w, video_dst_h;
    uint32_t       video_version;
    bool           video_active;
    int32_t        vann_x, vann_y, vann_w, vann_h;   /* video rectangle last announced as dirty (client coordinates) */
    /* Growth-only staging for the SYS_WIN2_VIDEO_YUV420 path: the raw
     * planes are copied in here (same CR3-safe single memcpy per plane
     * the RGBA path above already did for one buffer) before conversion,
     * which then runs from kernel memory only - see
     * cos_app_window_video_frame_yuv420()'s comment for why that split
     * matters. Reused frame to frame like video_pixels above instead of
     * malloc/free per frame. */
    uint8_t       *yuv_y, *yuv_u, *yuv_v;
    uint64_t       yuv_y_capacity, yuv_u_capacity, yuv_v_capacity;
    bool           closed_by_user;
    /* "not responding": a v2 app that has neither polled its window nor presented for a while (and is not parked in SYS_WIN2_WAIT) gets a notice drawn over it with Wait / End program */
    uint64_t       last_alive_ms, hung_ignore_until;
    bool           hung, kill_req;
    int8_t         hung_hover;
    /* the window's client area changed (maximize, restore, tiling): the app gets COS_EV_RESIZE and may cos_win2_resize() its surface (app_sized); until it does, the surface is stretched over the client area */
    bool           app_sized;
    int32_t        last_cw, last_ch;
    char           title_saved[64];
    cos_win_event_t events[COS_APP_MAX_INPUT_QUEUE];
    uint32_t       ev_head, ev_tail, ev_dropped;
    thread_t      *waiter;          /* the thread sleeping in SYS_WIN2_WAIT on this surface, woken by ev_push_locked */
    int32_t        last_mx, last_my;
    uint8_t        last_buttons;
    char           path[256];    /* last COS_EV_OPEN path */
    char           dq[8][256];   /* paths of COS_EV_DROP events not collected yet (cos_win2_get_path takes the oldest) */
    int            dq_n;
    uint32_t       owner_pid;
    cos_app_rect_t rects[COS_APP_MAX_RECTS];
    uint32_t       rect_count;
    uint32_t       dropped;
    cos_app_text_cmd_t text_cmds[COS_APP_MAX_TEXT_CMDS];
    uint32_t       text_count;
    uint32_t       text_dropped;
    /* Ring buffer filled by handle_keyboard_for_window()'s WIN_COS_APP
     * case (gui_input.c, on the GUI owner thread - the same thread that
     * already owns all drawing) and drained by SYS_WIN_POLL_KEY on a
     * ring3 thread. Bounded and drop-counted for the same reason the
     * draw queues are: a hostile or simply slow-to-poll program must not
     * be able to make the kernel buffer unboundedly. */
    cos_app_key_event_t input_queue[COS_APP_MAX_INPUT_QUEUE];
    uint32_t       input_head, input_tail;
    uint32_t       input_dropped;
    /* Same bounded-ring-buffer treatment as input_queue above, for
     * mouse clicks instead of key presses - a separate queue rather
     * than a tagged union of the two event types, mirroring how
     * rects/text_cmds are already kept as separate arrays in this same
     * struct rather than one interleaved stream (see that comment). */
    cos_app_mouse_event_t mouse_queue[COS_APP_MAX_INPUT_QUEUE];
    uint32_t       mouse_head, mouse_tail;
    uint32_t       mouse_dropped;
    /* Pointer lock (SYS_WIN2_GRAB): while this window has the keyboard focus the compositor hides the cursor, keeps the
     * pointer in the middle of the window and hands the raw motion to the app through SYS_WIN2_INPUT instead. */
    bool           grab;
    int64_t        grab_dx, grab_dy;
} cos_app_surface_t;

static cos_app_surface_t g_surfaces[COS_APP_MAX_WINDOWS];

/* Handles are 1-based so 0 is never a valid handle and an uninitialised
 * variable in a user program cannot accidentally address a window. */
static void ev_push_locked(cos_app_surface_t *s, const cos_win_event_t *ev);
static void cos_app_pending_gui_close(uint32_t uid);

static window_t *surf_win(const cos_app_surface_t *s)
{
    int idx = gui_window_index_by_uid(s->win_uid);
    return idx >= 0 ? &windows[idx] : NULL;
}

/* A new video frame changes only the video rectangle: announce it (plus where the video was, if it moved or was
 * resized) as dirty instead of asking for a whole-scene redraw. */
static void video_announce(cos_app_surface_t *s)
{
    window_t *w = surf_win(s);
    int dw = s->video_dst_w > 0 ? s->video_dst_w : s->video_w, dh = s->video_dst_h > 0 ? s->video_dst_h : s->video_h;
    if (!w) { gui_request_redraw(); return; }
    int x0 = s->video_x, y0 = s->video_y, x1 = x0 + dw, y1 = y0 + dh;
    if (s->vann_w > 0) {
        if (s->vann_x < x0) x0 = s->vann_x;
        if (s->vann_y < y0) y0 = s->vann_y;
        if (s->vann_x + s->vann_w > x1) x1 = s->vann_x + s->vann_w;
        if (s->vann_y + s->vann_h > y1) y1 = s->vann_y + s->vann_h;
    }
    s->vann_x = s->video_x; s->vann_y = s->video_y; s->vann_w = dw; s->vann_h = dh;
    gui_dirty_rect(w->x + x0, w->y + TITLEBAR_H + y0, x1 - x0, y1 - y0);
}

static cos_app_surface_t *surface_from_handle(int64_t handle)
{
    if (handle < 1 || handle > COS_APP_MAX_WINDOWS) return NULL;
    cos_app_surface_t *s = &g_surfaces[handle - 1];
    if (!s->used || s->win_uid == 0) return NULL;
    return s;
}

int64_t cos_app_window_create(const char *title, int32_t width, int32_t height,
                              uint32_t owner_pid)
{
    /* Clamp rather than reject: a user program asking for a 10000x10000
     * window is not malicious, just wrong, and a clamped window is more
     * useful than a failure it probably will not check. */
    if (width  < 120) width  = 120;
    if (height < 90)  height = 90;
    if (width  > 1000) width  = 1000;
    if (height > 700)  height = 700;

    /* one process may not take the whole pool: at most 4 windows per pid */
    if (owner_pid) {
        int mine = 0;
        for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i)
            if (g_surfaces[i].used && g_surfaces[i].owner_pid == owner_pid) mine++;
        if (mine >= 4) return -1;
    }

    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        if (g_surfaces[i].used) continue;

        window_t *win = gui_open_window(WIN_COS_APP,
                                        title ? title : "C-OS App",
                                        120 + i * 24, 90 + i * 24,
                                        width, height);
        if (!win) return -1;

        g_surfaces[i].used       = true;
        g_surfaces[i].grab       = false;
        g_surfaces[i].grab_dx = g_surfaces[i].grab_dy = 0;
        g_surfaces[i].win_uid    = win->uid;
        g_surfaces[i].owner_pid  = owner_pid;
        g_surfaces[i].rect_count = 0;
        g_surfaces[i].dropped    = 0;

        /* Raise it. gui_open_window() appends to the window list, but the
         * .c-os process is launched during boot - before the desktop opens
         * its own windows - so without this the program's window sits at
         * index 0, underneath everything opened afterwards, and its
         * contents are painted and then immediately covered. That is
         * exactly what a pixel-level screenshot analysis showed: the draw
         * path ran with the right geometry and rect count every frame, yet
         * nothing was visible. */
        for (int k = 0; k < window_count; ++k) {
            if (&windows[k] == win) { gui_bring_to_front(k); break; }
        }

        serial_puts("[COSAPP] window created for pid=");
        serial_putdec((uint64_t)owner_pid);
        serial_puts(" handle=");
        serial_putdec((uint64_t)(i + 1));
        serial_puts("\n");
        return i + 1;
    }
    return -1;
}

bool cos_app_window_fill_rect(int64_t handle, int32_t x, int32_t y,
                              int32_t w, int32_t h, uint32_t color,
                              uint32_t caller_pid)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = surface_from_handle(handle);
    /* Ownership check: a handle is only usable by the process that
     * created it, so one ring3 program cannot draw into another's
     * window just by guessing a small integer. */
    if (!s || s->owner_pid != caller_pid) {
        sync_irq_restore(flags);
        return false;
    }
    if (w <= 0 || h <= 0) { sync_irq_restore(flags); return true; }

    if (s->rect_count >= COS_APP_MAX_RECTS) {
        ++s->dropped;
        sync_irq_restore(flags);
        return false;
    }
    cos_app_rect_t *r = &s->rects[s->rect_count++];
    r->x = x; r->y = y; r->w = w; r->h = h; r->color = color;
    sync_irq_restore(flags);
    return true;
}

bool cos_app_window_draw_text(int64_t handle, int32_t x, int32_t y,
                              uint32_t fg, uint32_t bg,
                              const char *text, uint32_t len,
                              uint32_t caller_pid)
{
    if (!text || len == 0) return true;
    if (len > COS_APP_MAX_TEXT_LEN) len = COS_APP_MAX_TEXT_LEN;

    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) {
        sync_irq_restore(flags);
        return false;
    }
    if (s->text_count >= COS_APP_MAX_TEXT_CMDS) {
        ++s->text_dropped;
        sync_irq_restore(flags);
        return false;
    }
    cos_app_text_cmd_t *cmd = &s->text_cmds[s->text_count++];
    cmd->x = x; cmd->y = y; cmd->fg = fg; cmd->bg = bg;
    cmd->len = (uint8_t)len;
    memcpy(cmd->text, text, len);
    cmd->text[len] = '\0';
    sync_irq_restore(flags);
    return true;
}

/* Called from gui_input.c's WIN_COS_APP branch of
 * handle_keyboard_for_window() - the GUI owner thread, the same thread
 * that already owns all drawing, so no additional locking beyond the
 * existing IRQ-disable convention is needed for the write side. Finds
 * the surface by window pointer (syscalls look it up by handle instead;
 * both paths reach the same table). */
void cos_app_window_push_key(void *win_ptr, char ascii, uint8_t special, uint8_t modifiers)
{
    window_t *win = (window_t *)win_ptr;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || !win || s->win_uid != win->uid) continue;
        uint32_t next = (s->input_tail + 1) % COS_APP_MAX_INPUT_QUEUE;
        if (next == s->input_head) {
            /* Full: drop the OLDEST rather than the newest by not
             * advancing - actually simpler and still correct to just
             * drop this new one, since a program not draining its input
             * fast enough losing its most recent keystroke (rather than
             * silently reordering) is the less surprising failure mode. */
            ++s->input_dropped;
        } else {
            s->input_queue[s->input_tail].ascii = ascii;
            s->input_queue[s->input_tail].special = special;
            s->input_queue[s->input_tail].modifiers = modifiers;
            s->input_tail = next;
        }
        if (s->pixels) {
            cos_win_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = COS_EV_KEY; ev.ascii = ascii; ev.special = special; ev.mods = modifiers;
            ev_push_locked(s, &ev);
        }
        break;
    }
    sync_irq_restore(flags);
}

bool cos_app_window_poll_key(int64_t handle, uint32_t caller_pid, cos_app_key_event_t *out)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) {
        sync_irq_restore(flags);
        return false;
    }
    if (s->input_head == s->input_tail) {
        sync_irq_restore(flags);
        return false;   /* queue empty - not an error, just nothing yet */
    }
    if (out) *out = s->input_queue[s->input_head];
    s->input_head = (s->input_head + 1) % COS_APP_MAX_INPUT_QUEUE;
    sync_irq_restore(flags);
    return true;
}

/* Called from gui_input.c's WIN_COS_APP branch of the click dispatch -
 * the GUI owner thread, same as push_key above, so the same
 * IRQ-disable-only locking is sufficient. */
void cos_app_window_push_mouse(void *win_ptr, int32_t x, int32_t y, uint8_t button)
{
    window_t *win = (window_t *)win_ptr;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || !win || s->win_uid != win->uid) continue;
        uint32_t next = (s->mouse_tail + 1) % COS_APP_MAX_INPUT_QUEUE;
        if (next == s->mouse_head) {
            ++s->mouse_dropped; /* full - drop the newest, same reasoning as push_key */
        } else {
            s->mouse_queue[s->mouse_tail].x = x;
            s->mouse_queue[s->mouse_tail].y = y;
            s->mouse_queue[s->mouse_tail].button = button;
            s->mouse_tail = next;
        }
        break;
    }
    sync_irq_restore(flags);
}

bool cos_app_window_poll_mouse(int64_t handle, uint32_t caller_pid, cos_app_mouse_event_t *out)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) {
        sync_irq_restore(flags);
        return false;
    }
    if (s->mouse_head == s->mouse_tail) {
        sync_irq_restore(flags);
        return false;   /* queue empty - not an error, just nothing clicked yet */
    }
    if (out) *out = s->mouse_queue[s->mouse_head];
    s->mouse_head = (s->mouse_head + 1) % COS_APP_MAX_INPUT_QUEUE;
    sync_irq_restore(flags);
    return true;
}

void cos_app_window_clear(int64_t handle, uint32_t caller_pid)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = surface_from_handle(handle);
    if (s && s->owner_pid == caller_pid) {
        s->rect_count = 0;
        s->text_count = 0;
    }
    sync_irq_restore(flags);
}

/* Called from the GUI render loop, on the owner thread. */

/* ---- "not responding" ----------------------------------------------------------------------------------------------------------------------------- */
#define HUNG_AFTER_MS 8000u
typedef struct { int px, py, pw, ph, bx[2], by, bw, bh; bool strip; } hung_layout_t;
static void hung_layout(int cx, int cy, int cw, int ch, hung_layout_t *L)
{
    L->strip = cw < 340 || ch < 170;
    if (L->strip) {                                   /* a small window: a bar along its top edge with the two buttons in it */
        L->px = cx; L->py = cy; L->pw = cw; L->ph = 28; L->bw = 70; L->bh = 22; L->by = cy + 3;
        L->bx[0] = cx + cw - 2 * L->bw - 10; L->bx[1] = cx + cw - L->bw - 4;
        return;
    }
    L->pw = cw - 24 < 480 ? cw - 24 : 480; L->ph = 150;
    L->px = cx + (cw - L->pw) / 2; L->py = cy + (ch - L->ph) / 2;
    L->bw = (L->pw - 60) / 2 < 190 ? (L->pw - 60) / 2 : 190; L->bh = 32; L->by = L->py + L->ph - L->bh - 14;
    L->bx[0] = L->px + 20; L->bx[1] = L->px + L->pw - L->bw - 20;
}
static void draw_hung_overlay(cos_app_surface_t *s, int cx, int cy, int cw, int ch)
{
    hung_layout_t L; hung_layout(cx, cy, cw, ch, &L);
    const char *lab[2] = { gui_text("Wait", "待機"), gui_text("End", "終了") };
    if (L.strip) {
        vga_fill_rect(cx, cy, cw, 28, 0x00402020);
        vga_draw_string(cx + 6, cy + 6, gui_text("Not responding", "応答なし"), 0x00FFFFFF, 0x00402020);
    } else {
        vga_fill_rect(L.px - 1, L.py - 1, L.pw + 2, L.ph + 2, 0x00B0B8C8);
        vga_fill_rect(L.px, L.py, L.pw, L.ph, 0x00F4F6FA);
        vga_fill_rect(L.px, L.py, L.pw, 4, 0x00E5484D);
        vga_draw_string(L.px + 20, L.py + 18, gui_text("This program is not responding", "このプログラムは応答していません"), 0x00182030, 0x00F4F6FA);
        vga_draw_string(L.px + 20, L.py + 46, gui_text("It may be busy, or it may have stopped.", "処理が長引いているか、停止している可能性があります。"), 0x00505A70, 0x00F4F6FA);
        vga_draw_string(L.px + 20, L.py + 66, gui_text("Wait a little longer, or end the program.", "もう少し待つか、プログラムを終了してください。"), 0x00505A70, 0x00F4F6FA);
        lab[0] = gui_text("Wait", "待機する"); lab[1] = gui_text("End program", "プログラムを終了");
    }
    for (int b = 0; b < 2; ++b) {
        uint64_t bg = b == 1 ? (s->hung_hover == 1 ? 0x00F06A6E : 0x00E5484D) : (s->hung_hover == 0 ? 0x00D0D6E4 : 0x00E1E6F0);
        uint64_t fg = b == 1 ? 0x00FFFFFF : 0x00182030;
        vga_fill_rect(L.bx[b], L.by, L.bw, L.bh, bg);
        vga_draw_string(L.bx[b] + (L.strip ? 8 : 14), L.by + (L.strip ? 3 : 8), lab[b], fg, bg);
    }
}
/* an app that does not size its own surface has it stretched over the window's client area when the two differ (a maximized window shows no black margin) */
static bool surf_scaled(const cos_app_surface_t *s, const window_t *win)
{
    int cw = win->w, ch = win->h - TITLEBAR_H;
    return !win->fullscreen && !s->app_sized && s->pix_w > 0 && s->pix_h > 0 && cw > 0 && ch > 0 && (cw != s->pix_w || ch != s->pix_h);
}
/* the mouse over a hung window: returns true when the event was taken (nothing reaches the frozen app) */
static bool hung_mouse(cos_app_surface_t *s, window_t *win, int32_t mx, int32_t my, uint8_t buttons)
{
    int cx = win->x, cy = win->y + TITLEBAR_H, cw = win->w, ch = win->h - TITLEBAR_H;
    hung_layout_t L; hung_layout(cx, cy, cw, ch, &L);
    int ax = mx + cx, ay = my + cy;                      /* mx, my are client coordinates */
    int8_t hv = -1;
    for (int b = 0; b < 2; ++b) if (ax >= L.bx[b] && ax < L.bx[b] + L.bw && ay >= L.by && ay < L.by + L.bh) hv = (int8_t)b;
    if (hv != s->hung_hover) { s->hung_hover = hv; gui_request_redraw(); }
    bool was = (s->last_buttons & 1) != 0, now = (buttons & 1) != 0;
    if (now && !was && hv == 0) { s->hung = false; s->hung_ignore_until = get_timer_ticks() + 30000; s->last_alive_ms = get_timer_ticks(); gui_request_redraw(); }
    else if (now && !was && hv == 1) { s->kill_req = true; gui_request_redraw(); }
    s->last_buttons = buttons;
    return true;
}

void cos_app_window_draw(int window_index)
{
    if (window_index < 0 || window_index >= MAX_WINDOWS) return;
    window_t *w = &windows[window_index];

    cos_app_surface_t *s = NULL;
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        if (g_surfaces[i].used && g_surfaces[i].win_uid == w->uid) { s = &g_surfaces[i]; break; }
    }
    if (!s) return;

    int cx = w->x;
    int cy = w->y + TITLEBAR_H;
    int cw = w->w;
    int ch = w->h - TITLEBAR_H;
    if (cw <= 0 || ch <= 0) return;

    if (!w->fullscreen && s->pixels && (cw != s->last_cw || ch != s->last_ch)) {            /* the client area changed: tell the app */
        if (s->last_cw) { cos_win_event_t rev; memset(&rev, 0, sizeof rev); rev.type = COS_EV_RESIZE; rev.x = cw; rev.y = ch; uint64_t fl = sync_irq_save(); ev_push_locked(s, &rev); sync_irq_restore(fl); }
        s->last_cw = cw; s->last_ch = ch;
    }
    if (s->present_pixels && surf_scaled(s, w)) {
        if (!vga_gpu_draw_image_scaled(s, s->present_pixels, s->pix_w, s->pix_h, s->pix_w, s->present_version, false, cx, cy, cw, ch))
            vga_blit_scaled_nearest(cx, cy, cw, ch, s->present_pixels, s->pix_w, s->pix_h, s->pix_w);
    } else if (s->present_pixels) {
        /* v2: the app's presented pixels - see present_pixels' own comment
         * on why this is a separate buffer from s->pixels (the app's own,
         * live, possibly-mid-write one) rather than that buffer directly.
         * In GPU draw mode the buffer is a GPU texture re-uploaded only
         * when present_version changes. */
        int bw = s->pix_w < cw ? s->pix_w : cw;
        int bh = s->pix_h < ch ? s->pix_h : ch;
        if (!vga_gpu_draw_image(s, s->present_pixels, s->pix_w, s->pix_h, s->pix_w,
                                s->present_version, false, cx, cy)) {
            vga_copy_rect_strided(cx, cy, bw, bh, s->present_pixels, s->pix_w);
        }
    }

    /* Video is a separate plane: the decoded frame is uploaded once per
     * version and scaled by the GPU blit shader. This keeps the expensive
     * resize/YUV presentation work out of the GUI compositor. */
    if (s->video_active && s->video_pixels && s->video_w > 0 && s->video_h > 0) {
        int dx = cx + s->video_x, dy = cy + s->video_y;
        int dw = s->video_dst_w > 0 ? s->video_dst_w : s->video_w;
        int dh = s->video_dst_h > 0 ? s->video_dst_h : s->video_h;
        if (!vga_gpu_draw_image_scaled(s, s->video_pixels, s->video_w, s->video_h,
                                        s->video_stride, s->video_version, false,
                                        dx, dy, dw, dh)) {
            /* CPU/2D mode (no VirGL): parallel LUT-based nearest-neighbour
             * scale instead of a per-pixel division + vga_put_pixel() loop
             * - see vga_blit_scaled_nearest()'s comment in vga.c. */
            vga_blit_scaled_nearest(dx, dy, dw, dh,
                                    s->video_pixels, s->video_w, s->video_h, s->video_stride);
        }
    }
    if (s->hung) draw_hung_overlay(s, cx, cy, cw, ch);
    if (s->pixels) return;

    /* Snapshot the queue under IRQ-disable so a syscall landing mid-paint
     * cannot change rect_count while it is being iterated. The rects
     * themselves are small and bounded, so copying is cheap and avoids
     * holding interrupts off for the whole (much slower) paint. */
    uint64_t flags = sync_irq_save();
    uint32_t count = s->rect_count;
    if (count > COS_APP_MAX_RECTS) count = COS_APP_MAX_RECTS;
    static cos_app_rect_t snapshot[COS_APP_MAX_RECTS];
    memcpy(snapshot, s->rects, count * sizeof(cos_app_rect_t));
    uint32_t text_count = s->text_count;
    if (text_count > COS_APP_MAX_TEXT_CMDS) text_count = COS_APP_MAX_TEXT_CMDS;
    static cos_app_text_cmd_t text_snapshot[COS_APP_MAX_TEXT_CMDS];
    memcpy(text_snapshot, s->text_cmds, text_count * sizeof(cos_app_text_cmd_t));
    sync_irq_restore(flags);

    /* Default background so a program that draws nothing still gets a
     * well-defined surface rather than whatever was underneath. */
    vga_fill_rect(cx, cy, cw, ch, 0x00202020);

    for (uint32_t i = 0; i < count; ++i) {
        const cos_app_rect_t *r = &snapshot[i];
        /* Clip to the client area in kernel space. The coordinates came
         * from ring3 and are arbitrary - including negative and huge -
         * so they are clamped here rather than trusted, otherwise a user
         * program could paint over the whole desktop, other windows, or
         * off the end of the framebuffer. */
        int rx = cx + r->x;
        int ry = cy + r->y;
        int rw = r->w;
        int rh = r->h;
        if (rx < cx) { rw -= (cx - rx); rx = cx; }
        if (ry < cy) { rh -= (cy - ry); ry = cy; }
        if (rx + rw > cx + cw) rw = (cx + cw) - rx;
        if (ry + rh > cy + ch) rh = (cy + ch) - ry;
        if (rw <= 0 || rh <= 0) continue;
        vga_fill_rect(rx, ry, rw, rh, r->color);
    }

    /* Text goes through the desktop's own clip-rect mechanism
     * (gui_set_clip/gui_clear_clip), already relied on elsewhere in the
     * GUI to bound drawing to a window's client area, rather than
     * manual per-glyph clamping: a variable-width UTF-8 string does not
     * have a single (w,h) to clamp the way a rect does, and this is the
     * SAME primitive other window kinds already trust for exactly this
     * purpose. Origin is still checked - a wildly out-of-range x/y is
     * simply skipped - so a huge coordinate cannot make the underlying
     * glyph renderer walk far off-screen before the clip rect even
     * applies. */
    gui_set_clip(cx, cy, cw, ch);
    for (uint32_t i = 0; i < text_count; ++i) {
        const cos_app_text_cmd_t *t = &text_snapshot[i];
        if (t->x < -10000 || t->x > 10000 || t->y < -10000 || t->y > 10000) continue;
        vga_draw_string_len(cx + t->x, cy + t->y, t->text, t->len, t->fg, t->bg);
    }
    gui_reset_clip();
}

bool cos_app_window_video_frame(int64_t handle, uint32_t caller_pid,
                                  const uint32_t *src, int32_t w, int32_t h,
                                  int32_t stride, int32_t x, int32_t y,
                                  int32_t dst_w, int32_t dst_h)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !src || w <= 0 || h <= 0 ||
        stride < w || w > 3840 || h > 2160) return false;
    uint64_t bytes = (uint64_t)stride * (uint64_t)h * 4u;
    if (!bytes || bytes > 64u * 1024u * 1024u) return false;
    if (s->video_ext) { s->video_pixels = NULL; s->video_capacity = 0; s->video_ext = false; }
    if (!s->video_pixels || s->video_capacity < bytes) {
        uint32_t *p = (uint32_t*)kmalloc((size_t)bytes);
        if (!p) return false;
        if (s->video_pixels) kfree(s->video_pixels);
        s->video_pixels = p;
        s->video_capacity = bytes;
    }
    memcpy(s->video_pixels, src, (size_t)bytes);
    s->video_w = w; s->video_h = h; s->video_stride = stride;
    s->video_x = x; s->video_y = y;
    s->video_dst_w = dst_w; s->video_dst_h = dst_h;
    s->video_active = true;
    ++s->video_version;
    if (!s->video_version) s->video_version = 1;
    video_announce(s);
    return true;
}

/* Planar-YUV counterpart to cos_app_window_video_frame() above, for
 * SYS_WIN2_VIDEO_YUV420. The caller (a decoder such as Media's H.264/
 * VP9/AV1 paths) hands over raw decoded planes instead of doing its own
 * single-threaded per-pixel YUV->RGBA conversion first.
 *
 * Two-step by design, NOT an optimisation to simplify away:
 *   1. memcpy() each plane from the (already syscall.c-validated) user
 *      pointers into kernel-owned staging buffers, on THIS CPU, exactly
 *      like cos_app_window_video_frame()'s single memcpy above.
 *   2. Hand the KERNEL-OWNED staging buffers to
 *      vga_convert_yuv420_to_rgba_parallel(), which may run step 2's
 *      work on other CPUs via the AP work-stealing queue.
 * Step 2 must never touch the caller's user-space pointers directly:
 * AP workers picked up off the work-stealing queue are not guaranteed to
 * be running under the calling process's CR3, so a user VA is only
 * meaningful on the CPU that validated and is still executing the
 * syscall (this one). Kernel heap addresses (the staging buffers,
 * video_pixels) are mapped identically from every CR3, which is what
 * makes handing THOSE to other CPUs safe. */
bool cos_app_window_video_frame_yuv420(int64_t handle, uint32_t caller_pid,
                                        const uint8_t *y, const uint8_t *u, const uint8_t *v,
                                        int32_t w, int32_t h,
                                        int32_t stride_y, int32_t stride_c,
                                        int32_t x, int32_t y_pos,
                                        int32_t dst_w, int32_t dst_h,
                                        int32_t bit_depth)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    int32_t sample_bytes = bit_depth > 8 ? 2 : 1;
    if (!s || s->owner_pid != caller_pid || !y || !u || !v ||
        (bit_depth < 8 || bit_depth > 12) ||
        w <= 0 || h <= 0 || w > 3840 || h > 2160 ||
        stride_y < w * sample_bytes || stride_c < ((w + 1) / 2) * sample_bytes) return false;

    int32_t chroma_h = (h + 1) / 2;
    uint64_t y_bytes = (uint64_t)stride_y * (uint64_t)h;
    uint64_t c_bytes = (uint64_t)stride_c * (uint64_t)chroma_h;
    uint64_t rgba_bytes = (uint64_t)w * (uint64_t)h * 4u;
    if (!y_bytes || !c_bytes || !rgba_bytes || rgba_bytes > 64u * 1024u * 1024u) return false;
    if (s->video_ext) { s->video_pixels = NULL; s->video_capacity = 0; s->video_ext = false; }

    if (!s->yuv_y || s->yuv_y_capacity < y_bytes) {
        uint8_t *p = (uint8_t*)kmalloc((size_t)y_bytes);
        if (!p) return false;
        if (s->yuv_y) kfree(s->yuv_y);
        s->yuv_y = p; s->yuv_y_capacity = y_bytes;
    }
    if (!s->yuv_u || s->yuv_u_capacity < c_bytes) {
        uint8_t *p = (uint8_t*)kmalloc((size_t)c_bytes);
        if (!p) return false;
        if (s->yuv_u) kfree(s->yuv_u);
        s->yuv_u = p; s->yuv_u_capacity = c_bytes;
    }
    if (!s->yuv_v || s->yuv_v_capacity < c_bytes) {
        uint8_t *p = (uint8_t*)kmalloc((size_t)c_bytes);
        if (!p) return false;
        if (s->yuv_v) kfree(s->yuv_v);
        s->yuv_v = p; s->yuv_v_capacity = c_bytes;
    }
    if (!s->video_pixels || s->video_capacity < rgba_bytes) {
        uint32_t *p = (uint32_t*)kmalloc((size_t)rgba_bytes);
        if (!p) return false;
        if (s->video_pixels) kfree(s->video_pixels);
        s->video_pixels = p; s->video_capacity = rgba_bytes;
    }

    memcpy(s->yuv_y, y, (size_t)y_bytes);
    memcpy(s->yuv_u, u, (size_t)c_bytes);
    memcpy(s->yuv_v, v, (size_t)c_bytes);

    vga_convert_yuv420_to_rgba_parallel(s->video_pixels, s->yuv_y, s->yuv_u, s->yuv_v,
                                        w, h, stride_y, stride_c, bit_depth);

    s->video_w = w; s->video_h = h; s->video_stride = w; /* RGBA output is tightly packed */
    s->video_x = x; s->video_y = y_pos;
    s->video_dst_w = dst_w; s->video_dst_h = dst_h;
    s->video_active = true;
    ++s->video_version;
    if (!s->video_version) s->video_version = 1;
    video_announce(s);
    return true;
}

void cos_app_window_release_for_pid(uint32_t pid)
{
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        if (g_surfaces[i].used && g_surfaces[i].owner_pid == pid) {
            g_surfaces[i].used = false;
            cos_app_pending_gui_close(g_surfaces[i].win_uid);
            g_surfaces[i].win_uid = 0;
            /* Owner is exiting: its page tables are about to be torn down
             * (leaf frames are never freed by that - see
             * free_page_table_recursive()), so just return the buffer to
             * the kernel heap. The GUI window is closed by the GUI thread
             * when it next sees an orphaned WIN_COS_APP (draw below). */
            if (g_surfaces[i].pixels) { kfree(g_surfaces[i].pixels); g_surfaces[i].pixels = NULL; }
            if (g_surfaces[i].present_pixels) { kfree(g_surfaces[i].present_pixels); g_surfaces[i].present_pixels = NULL; g_surfaces[i].present_capacity = 0; }
            if (g_surfaces[i].video_pixels && !g_surfaces[i].video_ext) kfree(g_surfaces[i].video_pixels);
            g_surfaces[i].video_pixels = NULL; g_surfaces[i].video_capacity = 0; g_surfaces[i].video_ext = false;
            for (int b = 0; b < 2; ++b) if (g_surfaces[i].vbuf[b]) { kfree(g_surfaces[i].vbuf[b]); g_surfaces[i].vbuf[b] = NULL; }
            g_surfaces[i].vbuf_bytes = 0;
            if (g_surfaces[i].yuv_y) { kfree(g_surfaces[i].yuv_y); g_surfaces[i].yuv_y = NULL; g_surfaces[i].yuv_y_capacity = 0; }
            if (g_surfaces[i].yuv_u) { kfree(g_surfaces[i].yuv_u); g_surfaces[i].yuv_u = NULL; g_surfaces[i].yuv_u_capacity = 0; }
            if (g_surfaces[i].yuv_v) { kfree(g_surfaces[i].yuv_v); g_surfaces[i].yuv_v = NULL; g_surfaces[i].yuv_v_capacity = 0; }
            g_surfaces[i].rect_count = 0;
            g_surfaces[i].text_count = 0;
            g_surfaces[i].input_head = 0;
            g_surfaces[i].input_tail = 0;
            g_surfaces[i].mouse_head = 0;
            g_surfaces[i].mouse_tail = 0;
            if (g_surfaces[i].shell_layer) { g_surfaces[i].shell_layer = 0; g_surfaces[i].shell_visible = false; gui_request_redraw(); }   /* the kernel taskbar / start menu take over again */
        }
    }
    sync_irq_restore(flags);
}


/* Window removal must happen on the GUI thread (gui_close_window()
 * compacts windows[] under the renderer's feet), so syscalls and process
 * exit only queue the uid here; cos_app_window_gui_tick() does the rest. */
#define COS_APP_PENDING_CLOSE 32
static uint32_t g_pending_close[COS_APP_PENDING_CLOSE];
static void cos_app_pending_gui_close(uint32_t uid)
{
    if (!uid) return;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_PENDING_CLOSE; ++i) {
        if (g_pending_close[i] == 0) { g_pending_close[i] = uid; break; }
    }
    sync_irq_restore(flags);
    gui_request_redraw();
}

static void hung_tick(void)
{
    uint64_t now = get_timer_ticks();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || !s->pixels || s->shell_layer || s->win_uid == 0) continue;
        process_t *p = process_get_by_pid(s->owner_pid);
        if (!p || p->state == TASK_ZOMBIE || p->state == TASK_UNUSED) continue;
        bool h = !s->waiter && now > s->hung_ignore_until && now - s->last_alive_ms > HUNG_AFTER_MS;
        if (h != s->hung) {
            s->hung = h; s->hung_hover = -1;
            serial_puts(h ? "[HUNG] not responding: pid " : "[HUNG] responding again: pid "); serial_putdec(s->owner_pid); serial_puts(" "); serial_puts(p->name); serial_puts("\n");
            window_t *w = surf_win(s);
            if (w) {
                if (h) { strncpy(s->title_saved, w->title, sizeof s->title_saved - 1); size_t l = strlen(w->title); const char *suf = gui_text(" (Not responding)", " (応答なし)"); if (l + strlen(suf) < sizeof w->title) strcat(w->title, suf); }
                else if (s->title_saved[0]) { strncpy(w->title, s->title_saved, sizeof w->title - 1); s->title_saved[0] = 0; }
            }
            gui_request_redraw();
        }
        if (!s->hung && s->title_saved[0]) {                                                   /* "Wait" was chosen: the title loses its suffix */
            window_t *w2 = surf_win(s);
            if (w2) { strncpy(w2->title, s->title_saved, sizeof w2->title - 1); w2->title[sizeof w2->title - 1] = 0; }
            s->title_saved[0] = 0; gui_request_redraw();
        }
        if (s->kill_req) {
            s->kill_req = false;
            serial_puts("[HUNG] ending pid "); serial_putdec(s->owner_pid); serial_puts(" "); serial_puts(p->name); serial_puts(" at the user's request\n");
            process_exit(p, -9);
        }
    }
}

/* GUI thread, once per frame. Closes windows whose app closed them, and
 * any WIN_COS_APP window whose surface no longer exists (owner exited). */
void cos_app_window_gui_tick(void)
{
    hung_tick();
    uint32_t todo[COS_APP_PENDING_CLOSE];
    int n = 0;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_PENDING_CLOSE; ++i) {
        if (g_pending_close[i]) { todo[n++] = g_pending_close[i]; g_pending_close[i] = 0; }
    }
    sync_irq_restore(flags);
    for (int k = 0; k < n; ++k) {
        int idx = gui_window_index_by_uid(todo[k]);
        if (idx >= 0) gui_close_window(idx);
    }
    for (int idx = window_count - 1; idx >= 0; --idx) {
        if (windows[idx].kind != WIN_COS_APP) continue;
        bool owned = false;
        for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
            if (g_surfaces[i].used && g_surfaces[i].win_uid == windows[idx].uid) { owned = true; break; }
        }
        if (!owned) gui_close_window(idx);
    }
}

/* ======================================================================
 * v2: shared pixel buffer + unified event queue
 * ==================================================================== */
#define COS_SURF_VA_BASE   0x0000040000000000ULL   /* PML4 slot 8: per-process user range */
#define COS_SURF_VA_SLOT   0x0000000001000000ULL   /* 16 MiB per window slot */

static void ev_push_locked(cos_app_surface_t *s, const cos_win_event_t *ev)
{
    uint32_t next = (s->ev_tail + 1) % COS_APP_MAX_INPUT_QUEUE;
    if (next == s->ev_head) {
        /* Full: coalesce motion rather than lose button/key edges. */
        if (ev->type == COS_EV_MOUSE_MOVE) { ++s->ev_dropped; return; }
        s->ev_head = (s->ev_head + 1) % COS_APP_MAX_INPUT_QUEUE;   /* drop oldest */
        ++s->ev_dropped;
    }
    s->events[s->ev_tail] = *ev;
    s->ev_tail = next;
    if (s->waiter) thread_wake(s->waiter);     /* event-driven: an idle app sleeps until input actually arrives */
}

/* Allocates the shared pixel buffer and the compositor's snapshot for an already-created surface and maps the buffer into the
 * calling (owner) process. Shared by ordinary windows and shell surfaces. */
static int64_t surface_setup(int64_t handle, int32_t w, int32_t h, uint32_t owner_pid, cos_win_info_t *out, bool announce)
{
    cos_app_surface_t *s = &g_surfaces[handle - 1];

    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4u + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    /* Page-aligned and page-rounded, so no other kernel object shares a
     * page that becomes user-visible. */
    uint32_t *pix = (uint32_t *)kmalloc_aligned(bytes, PAGE_SIZE);
    if (!pix) { cos_app_window_close(handle, owner_pid); return -1; }
    memset(pix, 0xF4, bytes);                       /* light neutral until the app draws */
    s->last_alive_ms = get_timer_ticks(); s->hung = false; s->kill_req = false; s->hung_hover = -1; s->hung_ignore_until = 0;
    /* The compositor's own copy (see present_pixels' struct comment) -
     * same neutral fill, so the window shows the same "nothing drawn yet"
     * placeholder it always has up to the app's first SYS_WIN2_PRESENT,
     * rather than showing nothing at all for those first few frames. */
    uint32_t *present_pix = (uint32_t *)kmalloc(bytes);
    if (!present_pix) { kfree(pix); cos_app_window_close(handle, owner_pid); return -1; }
    memset(present_pix, 0xF4, bytes);
    uint64_t va = COS_SURF_VA_BASE + (uint64_t)(handle - 1) * COS_SURF_VA_SLOT;
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        /* Runs in the owner's syscall, so "current" is its address space. */
        paging_unmap_page(va + off);                /* stale mapping from an earlier window in this slot */
        if (!paging_map_page(va + off, (uint64_t)(uintptr_t)pix + off, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            for (uint64_t u = 0; u < off; u += PAGE_SIZE) paging_unmap_page(va + u);
            kfree(pix);
            kfree(present_pix);
            cos_app_window_close(handle, owner_pid);
            return -1;
        }
    }
    uint64_t flags = sync_irq_save();
    s->pixels = pix;
    s->present_pixels = present_pix;
    s->present_capacity = bytes;
    s->pix_w = w;
    s->pix_h = h;
    s->user_va = va;
    s->pix_bytes = bytes;
    s->present_version = 1;
    s->closed_by_user = false;
    s->ev_head = s->ev_tail = s->ev_dropped = 0;
    s->last_buttons = 0;
    sync_irq_restore(flags);

    if (announce) {
        { extern void cos_music_on_window_created(uint32_t pid); cos_music_on_window_created(owner_pid); }
        { extern void cos_studio_on_window_created(uint32_t pid); cos_studio_on_window_created(owner_pid); }
        { extern void cos_files_on_window_created(uint32_t pid); cos_files_on_window_created(owner_pid); }
    }
    out->handle = handle;
    out->pixels = va;
    out->width = w;
    out->height = h;
    out->stride = w;
    out->reserved = 0;
    gui_request_redraw();
    return handle;
}


int64_t cos_app_window_create_v2(const char *title, int32_t w, int32_t h, uint32_t owner_pid, cos_win_info_t *out)
{
    if (!out) return -1;
    if (w < 64) w = 64;
    if (h < 48) h = 48;
    if (w > 1000) w = 1000;
    if (h > 680) h = 680;
    int64_t handle = cos_app_window_create(title, w, h + TITLEBAR_H, owner_pid);
    if (handle <= 0) return -1;
    return surface_setup(handle, w, h, owner_pid, out, true);
}

/* ---- shell surfaces ---------------------------------------------------------------------------------------------------
 * Shell layers are surfaces with no window_t: layer 1 is the ring-3 taskbar, layer 2 the ring-3 start menu (hidden until
 * the taskbar asks for it). They are not in windows[], have no chrome, cannot be dragged or focused, and the compositor
 * draws them at a fixed place. Their mouse and key events come from the compositor, not from window hit-testing. The
 * owner may also hand over a list of app icons (cos_shell_icon_t) that the compositor draws on top, since the kernel owns
 * the icon art; the list is staged by SYS_SHELL_ICONS and goes live with the next present, so icons never lag the pixels. */
#define COS_SHELL_UID_BASE 0xF0000000u

static cos_app_surface_t *shell_find(int layer)
{
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i)
        if (g_surfaces[i].used && g_surfaces[i].shell_layer == layer) return &g_surfaces[i];
    return NULL;
}

int64_t cos_shell_surface_create(int32_t layer, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t owner_pid, cos_win_info_t *out)
{
    if ((layer < 1 || layer > 6) || !out || w < 64 || h < 16 || w > 4096 || h > 1024) return -1;
    if ((uint64_t)w * (uint64_t)h * 4u > COS_SURF_VA_SLOT) return -1;
    uint64_t flags = sync_irq_save();
    int slot = -1;
    cos_app_surface_t *old = shell_find(layer);
    if (old) {
        process_t *p = process_get_by_pid(old->owner_pid);
        bool alive = p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
        if (alive) { sync_irq_restore(flags); return -1; }          /* one per layer; the owner is still running */
    }
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) if (!g_surfaces[i].used) { slot = i; break; }
    if (slot < 0) { sync_irq_restore(flags); return -1; }
    cos_app_surface_t *s = &g_surfaces[slot];
    memset(s, 0, sizeof *s);
    s->used = true; s->shell_layer = (uint8_t)layer; s->shell_x = x; s->shell_y = y;
    s->shell_visible = (layer != 2);
    s->win_uid = COS_SHELL_UID_BASE + (uint32_t)layer;
    s->owner_pid = owner_pid;
    sync_irq_restore(flags);
    int64_t handle = slot + 1;
    return surface_setup(handle, w, h, owner_pid, out, false);
}

static cos_app_surface_t *shell_ready(int layer)
{
    cos_app_surface_t *s = shell_find(layer);
    if (!s || !s->present_pixels || s->present_version < 2) return NULL;
    return s;
}

/* Compositor: draws a layer. False when it has nothing to show (no such layer, hidden, or no first frame yet), so the
 * caller falls back (layer 1: the kernel taskbar). */
static bool shell_draw(int layer)
{
    cos_app_surface_t *s = shell_ready(layer);
    if (!s || !s->shell_visible) return false;
    if (layer == 4) { extern int gui_notif_count(void); if (gui_notif_count() <= 0) return false; }
    if (layer == 3 || layer == 4 || layer == 5 || layer == 6) {
        /* Colour-keyed: only the runs of non-key pixels are copied, so the wallpaper (already drawn) shows through. */
        for (int y = 0; y < s->pix_h; ++y) {
            const uint32_t *row = s->present_pixels + (size_t)y * (size_t)s->pix_w;
            int x = 0;
            while (x < s->pix_w) {
                while (x < s->pix_w && (row[x] & 0xFFFFFFu) == COS_DESKTOP_KEY) ++x;
                int x0 = x;
                while (x < s->pix_w && (row[x] & 0xFFFFFFu) != COS_DESKTOP_KEY) ++x;
                /* a pixel whose alpha byte is 1..254 is translucent (the desktop's rubber band): blended with what is already drawn */
                for (int a = x0; a < x; ) {
                    bool tr = ((row[a] >> 24) - 1u) < 254u;
                    int e = a + 1;
                    while (e < x && ((((row[e] >> 24) - 1u) < 254u) == tr)) ++e;
                    if (!tr) vga_copy_rect_strided(s->shell_x + a, s->shell_y + y, e - a, 1, row + a, e - a);
                    else {
                        static uint32_t blend[1024];
                        for (int a0 = a; a0 < e; a0 += 1024) {
                            int cnt = e - a0 < 1024 ? e - a0 : 1024;
                            vga_read_rect(s->shell_x + a0, s->shell_y + y, cnt, 1, blend, 1024);
                            for (int k = 0; k < cnt; ++k) {
                                uint32_t src = row[a0 + k], dst = blend[k], al = src >> 24, ia = 255 - al;
                                uint32_t rr = (((src >> 16) & 255) * al + ((dst >> 16) & 255) * ia) / 255;
                                uint32_t gg = (((src >> 8) & 255) * al + ((dst >> 8) & 255) * ia) / 255;
                                uint32_t bb = ((src & 255) * al + (dst & 255) * ia) / 255;
                                blend[k] = (rr << 16) | (gg << 8) | bb;
                            }
                            vga_copy_rect_strided(s->shell_x + a0, s->shell_y + y, cnt, 1, blend, 1024);
                        }
                    }
                    a = e;
                }
            }
        }
    } else if (!vga_gpu_draw_image(s, s->present_pixels, s->pix_w, s->pix_h, s->pix_w, s->present_version, false, s->shell_x, s->shell_y))
        vga_copy_rect_strided(s->shell_x, s->shell_y, s->pix_w, s->pix_h, s->present_pixels, s->pix_w);
    extern void gui_draw_app_icon(int kind, int x, int y, int size, bool hov);
    extern void gui_draw_desktop_icon_index(int index, int x, int y, int size);
    for (int i = 0; i < s->n_icons_live; ++i) {
        cos_shell_icon_t *ic = &s->icons_live[i];
        extern void gui_draw_ctx_icon_art(int icon_id, int x, int y, bool hov);
        if (ic->kind >= 1000) gui_draw_ctx_icon_art(ic->kind - 1000, s->shell_x + ic->x, s->shell_y + ic->y, false);
        else if (ic->kind < 0) gui_draw_desktop_icon_index(-ic->kind - 1, s->shell_x + ic->x, s->shell_y + ic->y, ic->size);
        else gui_draw_app_icon(ic->kind, s->shell_x + ic->x, s->shell_y + ic->y, ic->size, false);
    }
    return true;
}
bool cos_shell_panel_draw(void) { return shell_draw(1); }
bool cos_shell_menu_draw(void)  { return shell_draw(2); }
bool cos_shell_desktop_draw(void) { return shell_draw(3); }
bool cos_shell_notify_draw(void) { return shell_draw(4); }
bool cos_shell_ctx_draw(void) { return shell_draw(5); }
bool cos_shell_balloon_draw(void) { return shell_draw(6); }          /* layer 6: the tray balloon above the taskbar (update notice), colour-keyed, no input */
uint32_t cos_shell_layer_tag(int layer) { cos_app_surface_t *s = shell_ready(layer); return s ? s->tag_live : 0xFFFFFFFFu; }
uint32_t cos_shell_desktop_version(void) { cos_app_surface_t *s = shell_ready(3); return s ? s->present_version : 0; }

bool cos_shell_layer_geometry(int layer, int *x, int *y, int *w, int *h)
{
    cos_app_surface_t *s = shell_ready(layer);
    if (!s) return false;
    if (x) *x = s->shell_x;
    if (y) *y = s->shell_y;
    if (w) *w = s->pix_w;
    if (h) *h = s->pix_h;
    return true;
}
bool cos_shell_panel_geometry(int *x, int *y, int *w, int *h) { return cos_shell_layer_geometry(1, x, y, w, h); }

bool cos_shell_layer_visible(int layer)
{
    cos_app_surface_t *s = shell_find(layer);
    return s && s->shell_visible;
}

/* Shows/hides a layer and tells its owner. False when the layer has no live, presented surface. */
bool cos_shell_layer_set_visible(int layer, bool visible)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = shell_find(layer);
    if (!s || !s->pixels || (visible && !s->present_pixels)) { sync_irq_restore(flags); return false; }
    if (s->shell_visible != visible) {
        s->shell_visible = visible;
        cos_win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = visible ? COS_EV_SHELL_SHOW : COS_EV_SHELL_HIDE;
        s->last_buttons = 0;
        ev_push_locked(s, &ev);
    }
    sync_irq_restore(flags);
    gui_request_redraw();
    return true;
}

void cos_shell_layer_key(int layer, char ascii, uint8_t special, uint8_t mods)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = shell_find(layer);
    if (s && s->pixels) {
        cos_win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.type = COS_EV_KEY; ev.ascii = ascii; ev.special = special; ev.mods = mods;
        ev_push_locked(s, &ev);
    }
    sync_irq_restore(flags);
}

/* Same diffing as cos_app_window_mouse_state(), for a layer: coordinates are relative to its top-left. */
void cos_shell_layer_mouse(int layer, int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel, bool inside)
{
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = shell_find(layer);
    if (s && s->pixels) {
        cos_win_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.x = cx; ev.y = cy; ev.buttons = buttons;
        if ((inside || s->last_buttons) && (cx != s->last_mx || cy != s->last_my)) { ev.type = COS_EV_MOUSE_MOVE; ev_push_locked(s, &ev); }
        for (uint8_t b = 1; b <= 4; b <<= 1) {
            bool now = (buttons & b) != 0, was = (s->last_buttons & b) != 0;
            if (now && !was && inside) { ev.type = COS_EV_MOUSE_DOWN; ev.button = b; ev_push_locked(s, &ev); }
            else if (!now && was)      { ev.type = COS_EV_MOUSE_UP;   ev.button = b; ev_push_locked(s, &ev); }
        }
        if (wheel && inside) { ev.type = COS_EV_WHEEL; ev.button = 0; ev.wheel = wheel; ev_push_locked(s, &ev); }
        s->last_mx = cx; s->last_my = cy;
        s->last_buttons = inside || s->last_buttons ? buttons : 0;
    }
    sync_irq_restore(flags);
}
void cos_shell_panel_mouse(int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel, bool inside) { cos_shell_layer_mouse(1, cx, cy, buttons, wheel, inside); }

/* SYS_SHELL_ICONS: stage the icon list; it goes live with the next present of that surface. */
int cos_shell_set_icons(int layer, uint32_t owner_pid, const cos_shell_icon_t *icons, int n, uint32_t tag)
{
    if (n < 0 || n > COS_SHELL_MAX_ICONS) return -1;
    uint64_t flags = sync_irq_save();
    cos_app_surface_t *s = shell_find(layer);
    if (!s || s->owner_pid != owner_pid) { sync_irq_restore(flags); return -1; }
    for (int i = 0; i < n; ++i) s->icons_staged[i] = icons[i];
    s->n_icons_staged = n;
    s->tag_staged = tag;
    sync_irq_restore(flags);
    return 0;
}

static void shell_commit_icons(cos_app_surface_t *s)
{
    if (!s->shell_layer) return;
    uint64_t flags = sync_irq_save();
    memcpy(s->icons_live, s->icons_staged, (size_t)s->n_icons_staged * sizeof(cos_shell_icon_t));
    s->n_icons_live = s->n_icons_staged;
    s->tag_live = s->tag_staged;
    sync_irq_restore(flags);
}

/* Which process owns the window with this uid (0 for windows the kernel itself draws). */
uint32_t cos_app_window_owner_pid(uint32_t win_uid)
{
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i)
        if (g_surfaces[i].used && g_surfaces[i].win_uid == win_uid) return g_surfaces[i].owner_pid;
    return 0;
}

#define COS_VID_VA_BASE   0x0000048000000000ULL   /* PML4 slot 9 */
#define COS_VID_VA_SLOT   0x0000000008000000ULL   /* 128 MiB per window: two 4K RGBA buffers fit */

/* Maps (allocating on first use or when the picture grows) the two video buffers for a w x h picture into the
 * caller's address space and returns the user address of the first; the second follows `bytes` later. */
bool cos_app_window_video_map(int64_t handle, uint32_t caller_pid, int32_t w, int32_t h, uint64_t *va_out, uint64_t *bytes_out)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !va_out || !bytes_out || w <= 0 || h <= 0 || w > 3840 || h > 2160) return false;
    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4u + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (s->vbuf[0] && s->vbuf_bytes >= bytes) { *va_out = s->vbuf_va; *bytes_out = s->vbuf_bytes; return true; }
    if (bytes * 2u > COS_VID_VA_SLOT) return false;
    if (s->vbuf[0]) {                         /* grow: drop the old pair first */
        s->video_active = false;
        for (int b = 0; b < 2; ++b)
            for (uint64_t off = 0; off < s->vbuf_bytes; off += PAGE_SIZE) paging_unmap_page(s->vbuf_va + (uint64_t)b * s->vbuf_bytes + off);
        if (s->video_ext) { s->video_pixels = NULL; s->video_capacity = 0; s->video_ext = false; }
        for (int b = 0; b < 2; ++b) { kfree(s->vbuf[b]); s->vbuf[b] = NULL; }
        s->vbuf_bytes = 0;
    }
    uint32_t *buf[2];
    for (int b = 0; b < 2; ++b) {
        buf[b] = (uint32_t *)kmalloc_aligned(bytes, PAGE_SIZE);
        if (!buf[b]) { if (b) kfree(buf[0]); return false; }
        memset(buf[b], 0, bytes);
    }
    uint64_t va = COS_VID_VA_BASE + (uint64_t)(handle - 1) * COS_VID_VA_SLOT;
    for (int b = 0; b < 2; ++b) {
        for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
            uint64_t v = va + (uint64_t)b * bytes + off;
            paging_unmap_page(v);
            if (!paging_map_page(v, (uint64_t)(uintptr_t)buf[b] + off, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
                for (int bb = 0; bb <= b; ++bb)
                    for (uint64_t u = 0; u < bytes; u += PAGE_SIZE) paging_unmap_page(va + (uint64_t)bb * bytes + u);
                kfree(buf[0]); kfree(buf[1]);
                return false;
            }
        }
    }
    s->vbuf[0] = buf[0]; s->vbuf[1] = buf[1]; s->vbuf_bytes = bytes; s->vbuf_va = va;
    *va_out = va; *bytes_out = bytes;
    return true;
}

/* Shows buffer `idx` (just filled by the app) as the video plane. */
bool cos_app_window_video_commit(int64_t handle, uint32_t caller_pid, int32_t idx, int32_t w, int32_t h,
                                 int32_t x, int32_t y, int32_t dst_w, int32_t dst_h)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || idx < 0 || idx > 1 || !s->vbuf[idx] || w <= 0 || h <= 0 ||
        (uint64_t)w * (uint64_t)h * 4u > s->vbuf_bytes) return false;
    s->video_pixels = s->vbuf[idx];
    s->video_capacity = s->vbuf_bytes;
    s->video_ext = true;
    s->video_w = w; s->video_h = h; s->video_stride = w;
    s->video_x = x; s->video_y = y;
    s->video_dst_w = dst_w; s->video_dst_h = dst_h;
    s->video_active = true;
    ++s->video_version;
    if (!s->video_version) s->video_version = 1;
    video_announce(s);
    return true;
}

/* Whole-window present, but only what actually changed is copied and shown. The app's buffer is compared with the
 * compositor's snapshot band by band; each band that differs is copied (just its changed column span) and announced as
 * a dirty rectangle, so every ring-3 app - including ones that redraw their whole window - gets partial frames, and a
 * redraw that produced identical pixels costs no frame at all. Returns true if anything changed. */
#define PRESENT_BAND 16
static bool present_diff(cos_app_surface_t *s)
{
    const int W = s->pix_w, H = s->pix_h;
    uint32_t *app = s->pixels, *snap = s->present_pixels;
    window_t *win = surf_win(s);
    bool any = false;
    int bx0[1024 / PRESENT_BAND + 8], by0[1024 / PRESENT_BAND + 8], bx1[1024 / PRESENT_BAND + 8], by1[1024 / PRESENT_BAND + 8];
    int nb = 0;
    if ((H + PRESENT_BAND - 1) / PRESENT_BAND > (int)(sizeof bx0 / sizeof bx0[0])) {   /* taller than the band table: copy it all */
        memcpy(snap, app, (size_t)W * (size_t)H * sizeof(uint32_t));
        gui_request_redraw();
        return true;
    }
    for (int y0 = 0; y0 < H; y0 += PRESENT_BAND) {
        int y1 = y0 + PRESENT_BAND < H ? y0 + PRESENT_BAND : H;
        int minx = W, maxx = -1, miny = -1, maxy = -1;
        for (int y = y0; y < y1; ++y) {
            const uint64_t *a = (const uint64_t *)(app + (size_t)y * W), *b = (const uint64_t *)(snap + (size_t)y * W);
            int pairs = W / 2, lo = 0;
            while (lo < pairs && a[lo] == b[lo]) ++lo;
            int left, right;
            if (lo == pairs) {                                   /* even part identical: check the odd trailing pixel */
                if (W & 1 && app[(size_t)y * W + W - 1] != snap[(size_t)y * W + W - 1]) { left = right = W - 1; }
                else continue;
            } else {
                int hi = pairs - 1;
                while (hi > lo && a[hi] == b[hi]) --hi;
                left = lo * 2 + (((uint32_t)a[lo] == (uint32_t)b[lo]) ? 1 : 0);
                right = hi * 2 + 1;
                if ((uint32_t)(a[hi] >> 32) == (uint32_t)(b[hi] >> 32)) right = hi * 2;
                if (W & 1 && app[(size_t)y * W + W - 1] != snap[(size_t)y * W + W - 1]) right = W - 1;
            }
            memcpy(snap + (size_t)y * W + left, app + (size_t)y * W + left, (size_t)(right - left + 1) * sizeof(uint32_t));
            if (left < minx) minx = left;
            if (right > maxx) maxx = right;
            if (miny < 0) miny = y;
            maxy = y;
        }
        if (maxx >= 0) { bx0[nb] = minx; bx1[nb] = maxx + 1; by0[nb] = miny; by1[nb] = maxy + 1; ++nb; any = true; }
    }
    if (!any) return false;
    if (!win) { gui_request_redraw(); return true; }
    /* merge vertically adjacent bands into rectangles (union of x spans), then announce them */
    int i = 0;
    while (i < nb) {
        int x0 = bx0[i], x1 = bx1[i], y0 = by0[i], y1 = by1[i], j = i + 1;
        while (j < nb && by0[j] <= y1 + PRESENT_BAND) {
            if (bx0[j] < x0) x0 = bx0[j];
            if (bx1[j] > x1) x1 = bx1[j];
            y1 = by1[j]; ++j;
        }
        gui_dirty_rect(win->x + x0, win->y + TITLEBAR_H + y0, x1 - x0, y1 - y0);
        i = j;
    }
    return true;
}

bool cos_app_window_present(int64_t handle, uint32_t caller_pid)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !s->pixels) return false;
    s->last_alive_ms = get_timer_ticks();
    shell_commit_icons(s);
    /* Snapshot the app's buffer into the compositor's own copy before
     * advancing present_version - see present_pixels' struct comment.
     * This runs on the calling (app's) thread/CR3, so reading s->pixels
     * here is safe the same way every other same-process syscall copy in
     * this file is; it is ONLY unsafe for a different thread (the GUI
     * compositor) to read without this snapshot in between. */
    if (s->present_pixels && s->present_capacity >= s->pix_bytes) {
        if (present_diff(s)) ++s->present_version;     /* nothing changed -> nothing to show: no frame at all */
        return true;
    }
    ++s->present_version;
    gui_request_redraw();
    return true;
}

/* Presents only the given client-area rectangles: copies those rows from the app's buffer into the compositor's
 * snapshot (cost proportional to the damage, not to the window) and announces the matching screen rectangles as
 * dirty, so the frame that shows them is itself partial. Rectangles are clamped to the surface; an empty list or a
 * surface without a snapshot falls back to a whole-window present. */
bool cos_app_window_present_rects(int64_t handle, uint32_t caller_pid, const cos_win_rect_t *r, uint32_t n)
{
    { cos_app_surface_t *sa = surface_from_handle(handle); if (sa && sa->owner_pid == caller_pid) sa->last_alive_ms = get_timer_ticks(); }
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !s->pixels) return false;
    if (s->shell_layer) return cos_app_window_present(handle, caller_pid);
    if (!s->present_pixels || s->present_capacity < s->pix_bytes || n == 0 || !r) return cos_app_window_present(handle, caller_pid);
    window_t *w = surf_win(s);
    for (uint32_t i = 0; i < n; ++i) {
        int x0 = r[i].x, y0 = r[i].y, x1 = r[i].x + r[i].w, y1 = r[i].y + r[i].h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > s->pix_w) x1 = s->pix_w;
        if (y1 > s->pix_h) y1 = s->pix_h;
        if (x1 <= x0 || y1 <= y0) continue;
        for (int y = y0; y < y1; ++y)
            memcpy(s->present_pixels + (size_t)y * (size_t)s->pix_w + (size_t)x0,
                   s->pixels + (size_t)y * (size_t)s->pix_w + (size_t)x0, (size_t)(x1 - x0) * sizeof(uint32_t));
        if (w) gui_dirty_rect(w->x + x0, w->y + TITLEBAR_H + y0, x1 - x0, y1 - y0);
    }
    ++s->present_version;
    if (!w) gui_request_redraw();
    return true;
}

/* Waits up to `ms` for an event. The emptiness check and the sleep happen in one IRQ-off section, so an event
 * pushed in between cannot be missed; ev_push_locked() wakes the sleeper. Returns 1 with *out filled, 0 on
 * timeout, -1 on a bad handle. */
int cos_app_window_wait_event(int64_t handle, uint32_t caller_pid, cos_win_event_t *out, uint64_t ms)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return -1;
    int got = 0;
    s->last_alive_ms = get_timer_ticks();
    uint64_t flags = sync_irq_save();
    if (s->ev_head == s->ev_tail && ms > 0) {
        s->waiter = thread_get_current();
        scheduler_sleep(ms);
        s->waiter = NULL;
        s->last_alive_ms = get_timer_ticks();
    }
    if (s->ev_head != s->ev_tail) {
        *out = s->events[s->ev_head];
        s->ev_head = (s->ev_head + 1) % COS_APP_MAX_INPUT_QUEUE;
        got = 1;
    }
    sync_irq_restore(flags);
    return got;
}

int cos_app_window_poll_event(int64_t handle, uint32_t caller_pid, cos_win_event_t *out)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return -1;
    s->last_alive_ms = get_timer_ticks();
    int got = 0;
    uint64_t flags = sync_irq_save();
    if (s->ev_head != s->ev_tail) {
        *out = s->events[s->ev_head];
        s->ev_head = (s->ev_head + 1) % COS_APP_MAX_INPUT_QUEUE;
        got = 1;
    }
    sync_irq_restore(flags);
    return got;
}

bool cos_app_window_set_title(int64_t handle, uint32_t caller_pid, const char *title)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !title) return false;
    window_t *w = surf_win(s);
    if (!w) return false;
    size_t n = 0;
    while (title[n] && n < sizeof(w->title) - 1) { w->title[n] = title[n]; ++n; }
    w->title[n] = '\0';
    gui_request_redraw();
    return true;
}

/* ---- pointer lock + raw input for games --------------------------------------------------------------------------- */
#include "keyboard.h"
bool cos_app_window_set_grab(int64_t handle, uint32_t caller_pid, bool on)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return false;
    uint64_t flags = sync_irq_save();
    if (s->grab != on) { s->grab = on; s->grab_dx = s->grab_dy = 0; }
    sync_irq_restore(flags);
    gui_request_redraw();
    return true;
}

/* GUI thread: does this window hold the pointer lock? (the caller checks that it is also the focused window) */
bool cos_app_window_is_grabbing(const void *win_ptr)
{
    const window_t *win = (const window_t *)win_ptr;
    if (!win) return false;
    bool r = false;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i)
        if (g_surfaces[i].used && g_surfaces[i].grab && g_surfaces[i].win_uid == win->uid) { r = true; break; }
    sync_irq_restore(flags);
    return r;
}

/* GUI thread, once per frame while the lock is active: the raw motion of the frame, and the buttons (delivered as ordinary
 * DOWN/UP events at the window centre, whatever the host pointer is doing). */
void cos_app_window_grab_feed(void *win_ptr, int64_t dx, int64_t dy, int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel)
{
    window_t *win = (window_t *)win_ptr;
    if (!win) return;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || !s->pixels || s->win_uid != win->uid) continue;
        s->grab_dx += dx; s->grab_dy += dy;
        cos_win_event_t ev;
        memset(&ev, 0, sizeof ev);
        ev.x = cx; ev.y = cy; ev.buttons = buttons;
        for (uint8_t b = 1; b <= 4; b <<= 1) {
            bool now = (buttons & b) != 0, was = (s->last_buttons & b) != 0;
            if (now && !was)      { ev.type = COS_EV_MOUSE_DOWN; ev.button = b; ev_push_locked(s, &ev); }
            else if (!now && was) { ev.type = COS_EV_MOUSE_UP;   ev.button = b; ev_push_locked(s, &ev); }
        }
        if (wheel) { ev.type = COS_EV_WHEEL; ev.button = 0; ev.wheel = wheel; ev_push_locked(s, &ev); }
        s->last_mx = cx; s->last_my = cy; s->last_buttons = buttons;
        break;
    }
    sync_irq_restore(flags);
}

/* The state a game polls once per frame: keys down, motion since the last call, whether the window is the focused one. */
int cos_app_window_input_state(int64_t handle, uint32_t caller_pid, cos_win_input_t *out)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !out) return -1;
    memset(out, 0, sizeof *out);
    window_t *w = surf_win(s);
    extern bool gui_window_is_keyboard_focus(const void *win_ptr);
    bool focused = w && gui_window_is_keyboard_focus(w);
    out->focused = focused ? 1 : 0;
    uint64_t flags = sync_irq_save();
    out->grabbed = s->grab ? 1 : 0;
    out->fullscreen = (w && w->fullscreen) ? 1 : 0;
    out->dx = (int32_t)s->grab_dx; out->dy = (int32_t)s->grab_dy;
    s->grab_dx = s->grab_dy = 0;
    sync_irq_restore(flags);
    if (focused) keyboard_key_state(out->keys);              /* an unfocused window sees no keys: nothing sticks across a window switch */
    return 0;
}

bool cos_app_window_close(int64_t handle, uint32_t caller_pid)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return false;
    /* Explicit close from the owner: unmap its view of the buffer (we are
     * in its address space), then free it and drop the GUI window. */
    if (s->pixels) {
        for (uint64_t off = 0; off < s->pix_bytes; off += PAGE_SIZE) paging_unmap_page(s->user_va + off);
    }
    if (s->vbuf[0]) {
        s->video_active = false;
        for (int b = 0; b < 2; ++b)
            for (uint64_t off = 0; off < s->vbuf_bytes; off += PAGE_SIZE) paging_unmap_page(s->vbuf_va + (uint64_t)b * s->vbuf_bytes + off);
        for (int b = 0; b < 2; ++b) { kfree(s->vbuf[b]); s->vbuf[b] = NULL; }
        s->vbuf_bytes = 0;
        if (s->video_ext) { s->video_pixels = NULL; s->video_capacity = 0; s->video_ext = false; }
    }
    uint32_t uid = s->win_uid;
    uint32_t *pix = s->pixels;
    uint32_t *present_pix = s->present_pixels;
    uint64_t flags = sync_irq_save();
    s->used = false;
    s->win_uid = 0;
    s->pixels = NULL;
    s->present_pixels = NULL;
    s->present_capacity = 0;
    sync_irq_restore(flags);
    if (pix) kfree(pix);
    if (present_pix) kfree(present_pix);
    cos_app_pending_gui_close(uid);
    return true;
}

/* Called by the GUI thread every frame for the WIN_COS_APP window under
 * (or capturing) the mouse: turns raw mouse state into MOVE/DOWN/UP/WHEEL
 * events by diffing against the last state, so apps get drags (sliders,
 * seek bars, selection) and releases - not just clicks. */
void cos_app_window_mouse_state(void *win_ptr, int32_t cx, int32_t cy, uint8_t buttons, int32_t wheel, bool inside)
{
    window_t *win = (window_t *)win_ptr;
    if (!win) return;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || !s->pixels || s->win_uid != win->uid) continue;
        if (s->hung) { hung_mouse(s, win, cx, cy, buttons); break; }
        if (surf_scaled(s, win)) { cx = (int32_t)((int64_t)cx * s->pix_w / (win->w > 0 ? win->w : 1)); cy = (int32_t)((int64_t)cy * s->pix_h / (win->h - TITLEBAR_H > 0 ? win->h - TITLEBAR_H : 1)); }
        cos_win_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.x = cx; ev.y = cy; ev.buttons = buttons;
        if ((inside || s->last_buttons) && (cx != s->last_mx || cy != s->last_my)) {
            ev.type = COS_EV_MOUSE_MOVE; ev_push_locked(s, &ev);
        }
        for (uint8_t b = 1; b <= 4; b <<= 1) {
            bool now = (buttons & b) != 0, was = (s->last_buttons & b) != 0;
            if (now && !was && inside) { ev.type = COS_EV_MOUSE_DOWN; ev.button = b; ev_push_locked(s, &ev); }
            else if (!now && was)      { ev.type = COS_EV_MOUSE_UP;   ev.button = b; ev_push_locked(s, &ev); }
        }
        if (wheel && inside) { ev.type = COS_EV_WHEEL; ev.button = 0; ev.wheel = wheel; ev_push_locked(s, &ev); }
        s->last_mx = cx; s->last_my = cy;
        s->last_buttons = inside || s->last_buttons ? buttons : 0;
        break;
    }
    sync_irq_restore(flags);
}

bool cos_app_window_user_close(void *win_ptr)
{
    window_t *win = (window_t *)win_ptr;
    if (!win) return false;
    bool handled = false;
    uint64_t flags = sync_irq_save();
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || s->win_uid != win->uid) continue;
        if (s->pixels && s->hung) { s->hung_hover = 1; gui_request_redraw(); handled = true; break; }        /* a frozen app cannot answer a close request: the notice offers "End program" */
        if (s->pixels) {
            /* v2 app: ask it to close (it may want to save). A second
             * click while the request is pending closes the window anyway,
             * so a hung app can never make its window unclosable. */
            if (!s->closed_by_user) {
                cos_win_event_t ev; memset(&ev, 0, sizeof(ev)); ev.type = COS_EV_CLOSE;
                ev_push_locked(s, &ev);
                s->closed_by_user = true;
                handled = true;
            }
        }
        break;
    }
    sync_irq_restore(flags);
    return handled;
}

/* Hands a file path to the app that owns `pid`'s first v2 window, as a
 * COS_EV_OPEN (the OS "open with") or COS_EV_DROP (drag and drop) event,
 * and raises that window. GUI thread only. */
bool cos_app_window_deliver_path(uint32_t pid, const char *path, uint32_t ev_type, int32_t x, int32_t y)
{
    if (!path) return false;
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (!s->used || !s->pixels || s->owner_pid != pid) continue;
        uint64_t flags = sync_irq_save();
        size_t n = 0;
        if (ev_type == COS_EV_DROP) {
            if (s->dq_n >= 8) { sync_irq_restore(flags); return false; }
            char *d = s->dq[s->dq_n++];
            while (path[n] && n < sizeof(s->dq[0]) - 1) { d[n] = path[n]; ++n; }
            d[n] = '\0';
        } else {
            while (path[n] && n < sizeof(s->path) - 1) { s->path[n] = path[n]; ++n; }
            s->path[n] = '\0';
        }
        cos_win_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ev_type; ev.x = x; ev.y = y;
        ev_push_locked(s, &ev);
        sync_irq_restore(flags);
        int idx = gui_window_index_by_uid(s->win_uid);
        if (idx >= 0) gui_bring_to_front(idx);
        gui_request_redraw();
        return true;
    }
    return false;
}

int cos_app_window_get_path(int64_t handle, uint32_t caller_pid, char *out, uint64_t cap)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !out || !cap) return -1;
    uint64_t flags = sync_irq_save();
    size_t n = 0;
    const char *src = s->path;
    if (s->dq_n > 0) {                                           /* a dropped file: hand out the oldest */
        static char first[256];
        memcpy(first, s->dq[0], sizeof first);
        for (int k = 1; k < s->dq_n; ++k) memcpy(s->dq[k - 1], s->dq[k], sizeof s->dq[0]);
        --s->dq_n;
        src = first;
    }
    while (src[n] && n + 1 < cap) { out[n] = src[n]; ++n; }
    out[n] = '\0';
    sync_irq_restore(flags);
    return (int)n;
}

/* The owner of the ring-3 window with this uid (0: none). */
uint32_t cos_app_window_owner_of_uid(uint32_t win_uid)
{
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (s->used && s->pixels && s->win_uid == win_uid) return s->owner_pid;
    }
    return 0;
}

/* A file dropped onto a ring-3 window (by uid): COS_EV_DROP at client x,y. */
bool cos_app_window_deliver_drop_uid(uint32_t win_uid, const char *path, int32_t x, int32_t y)
{
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        cos_app_surface_t *s = &g_surfaces[i];
        if (s->used && s->pixels && s->win_uid == win_uid)
            return cos_app_window_deliver_path(s->owner_pid, path, COS_EV_DROP, x, y);
    }
    return false;
}

/* ---- real full screen ----------------------------------------------------------------------------------------------------
 * The window takes the whole screen (x=y=0, no title bar: its client area is the window), the compositor draws nothing else
 * (gui_draw), and the surface is shown scaled to fit with its aspect ratio kept, on black. Mouse coordinates are mapped back. */
static void fs_fit(const cos_app_surface_t *s, int *x, int *y, int *w, int *h)
{
    int sw = (int)SCREEN_W, sh = (int)SCREEN_H, pw = s->pix_w, ph = s->pix_h;
    if (pw <= 0 || ph <= 0) { *x = 0; *y = 0; *w = sw; *h = sh; return; }
    if ((int64_t)pw * sh > (int64_t)ph * sw) { *w = sw; *h = (int)((int64_t)ph * sw / pw); }
    else                                      { *h = sh; *w = (int)((int64_t)pw * sh / ph); }
    *x = (sw - *w) / 2; *y = (sh - *h) / 2;
}

int cos_app_window_set_fullscreen(int64_t handle, uint32_t caller_pid, int mode)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return -1;
    window_t *w = surf_win(s);
    if (!w) return -1;
    bool on = mode == 2 ? !w->fullscreen : mode != 0;
    if (on && !w->fullscreen) {
        w->fs_x = w->x; w->fs_y = w->y; w->fs_w = w->w; w->fs_h = w->h;
        w->x = 0; w->y = 0; w->w = (int)SCREEN_W; w->h = (int)SCREEN_H;
        w->fullscreen = true;
        w->minimized = false;
        for (int k = 0; k < window_count; ++k) if (&windows[k] == w) { gui_bring_to_front(k); break; }
    } else if (!on && w->fullscreen) {
        w->x = w->fs_x; w->y = w->fs_y; w->w = w->fs_w; w->h = w->fs_h;
        w->fullscreen = false;
    }
    gui_request_redraw();
    return w->fullscreen ? 1 : 0;
}

/* Failsafe for the keyboard (Ctrl+Alt+F12) in case the program that owns a fullscreen window stops answering. */
void gui_fullscreen_force_exit(void)
{
    for (int i = 0; i < window_count; ++i) {
        window_t *w = &windows[i];
        if (!w->fullscreen) continue;
        w->x = w->fs_x; w->y = w->fs_y; w->w = w->fs_w; w->h = w->fs_h;
        w->fullscreen = false;
    }
    gui_request_redraw();
}

/* Screen position -> surface position of a fullscreen window; false if the pointer is on the black bars. */
bool cos_app_window_fs_map(const void *win_ptr, int mx, int my, int *cx, int *cy)
{
    const window_t *w = (const window_t *)win_ptr;
    cos_app_surface_t *s = NULL;
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i)
        if (g_surfaces[i].used && g_surfaces[i].win_uid == w->uid) { s = &g_surfaces[i]; break; }
    if (!s || s->pix_w <= 0) { *cx = mx; *cy = my; return false; }
    int fx, fy, fw, fh;
    fs_fit(s, &fx, &fy, &fw, &fh);
    if (fw <= 0 || fh <= 0) { *cx = mx; *cy = my; return false; }
    int64_t x = (int64_t)(mx - fx) * s->pix_w / fw, y = (int64_t)(my - fy) * s->pix_h / fh;
    bool inside = mx >= fx && my >= fy && mx < fx + fw && my < fy + fh;
    if (x < 0) x = 0;
    if (x >= s->pix_w) x = s->pix_w - 1;
    if (y < 0) y = 0;
    if (y >= s->pix_h) y = s->pix_h - 1;
    *cx = (int)x; *cy = (int)y;
    return inside;
}

void cos_app_window_draw_fullscreen(int window_index)
{
    if (window_index < 0 || window_index >= MAX_WINDOWS) return;
    window_t *w = &windows[window_index];
    cos_app_surface_t *s = NULL;
    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i)
        if (g_surfaces[i].used && g_surfaces[i].win_uid == w->uid) { s = &g_surfaces[i]; break; }
    int sw = (int)SCREEN_W, sh = (int)SCREEN_H;
    if (!s || !s->present_pixels) { vga_fill_rect(0, 0, sw, sh, 0x00000000); return; }
    int fx, fy, fw, fh;
    fs_fit(s, &fx, &fy, &fw, &fh);
    if (fy > 0) { vga_fill_rect(0, 0, sw, fy, 0); vga_fill_rect(0, fy + fh, sw, sh - (fy + fh), 0); }
    if (fx > 0) { vga_fill_rect(0, fy, fx, fh, 0); vga_fill_rect(fx + fw, fy, sw - (fx + fw), fh, 0); }
    if (fw == s->pix_w && fh == s->pix_h) {
        if (!vga_gpu_draw_image(s, s->present_pixels, s->pix_w, s->pix_h, s->pix_w, s->present_version, false, fx, fy))
            vga_copy_rect_strided(fx, fy, fw, fh, s->present_pixels, s->pix_w);
    } else if (!vga_gpu_draw_image_scaled(s, s->present_pixels, s->pix_w, s->pix_h, s->pix_w, s->present_version, false, fx, fy, fw, fh)) {
        vga_blit_scaled_nearest(fx, fy, fw, fh, s->present_pixels, s->pix_w, s->pix_h, s->pix_w);
    }
}

/* Resizes a v2 window's pixel surface (a game going full screen wants the screen's own resolution). The buffers are replaced and mapped at the
 * same user address; the compositor may be reading them on another CPU, so the size and the buffer pointer change in an order that is safe
 * for either (never new dimensions over a smaller buffer), and the old snapshot is only freed at the next resize. */
static uint32_t *s_retired[COS_APP_MAX_WINDOWS];

int cos_app_window_resize(int64_t handle, uint32_t caller_pid, int32_t w, int32_t h, cos_win_info_t *out)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !s->pixels || s->shell_layer || !out) return -1;
    if (w < 64) w = 64;
    if (h < 48) h = 48;
    if (w > 4096 || h > 4096) return -1;
    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4u + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (bytes > COS_SURF_VA_SLOT) return -1;
    if (w == s->pix_w && h == s->pix_h) goto fill_out;
    uint32_t *pix = (uint32_t *)kmalloc_aligned(bytes, PAGE_SIZE);
    if (!pix) return -1;
    uint32_t *present_pix = (uint32_t *)kmalloc(bytes);
    if (!present_pix) { kfree(pix); return -1; }
    memset(pix, 0, bytes); memset(present_pix, 0, bytes);
    uint64_t va = s->user_va;
    for (uint64_t off = 0; off < s->pix_bytes; off += PAGE_SIZE) paging_unmap_page(va + off);
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        if (!paging_map_page(va + off, (uint64_t)(uintptr_t)pix + off, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            for (uint64_t u = 0; u < off; u += PAGE_SIZE) paging_unmap_page(va + u);
            kfree(pix); kfree(present_pix);
            return -1;                                   /* (the old mapping is gone: the caller must treat this as fatal) */
        }
    }
    uint32_t *old_pix = s->pixels, *old_present = s->present_pixels;
    s->app_sized = true;
    int idx = (int)(s - g_surfaces);
    uint64_t flags = sync_irq_save();
    bool grow = bytes >= s->present_capacity;
    if (grow) { s->present_pixels = present_pix; s->present_capacity = bytes; __sync_synchronize(); s->pix_w = w; s->pix_h = h; }
    else      { s->pix_w = w; s->pix_h = h; __sync_synchronize(); s->present_pixels = present_pix; s->present_capacity = bytes; }
    s->pixels = pix; s->pix_bytes = bytes;
    ++s->present_version;
    sync_irq_restore(flags);
    window_t *win = surf_win(s);
    if (win && !win->fullscreen) { win->w = w; win->h = h + TITLEBAR_H; }
    if (s_retired[idx]) kfree(s_retired[idx]);
    s_retired[idx] = old_present;
    if (old_pix) kfree(old_pix);
    gui_request_redraw();
fill_out:
    out->handle = handle;
    out->pixels = s->user_va;
    out->width = s->pix_w; out->height = s->pix_h; out->stride = s->pix_w;
    out->reserved = 0;
    return 0;
}
