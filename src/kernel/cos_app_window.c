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
#include "sync.h"
#include "gui.h"
#include "vga.h"

extern void gui_bring_to_front(int idx);

#define COS_APP_MAX_WINDOWS 16
#define COS_APP_MAX_RECTS   256
#define COS_APP_MAX_TEXT_CMDS 128
#define COS_APP_MAX_TEXT_LEN  63
#define COS_APP_MAX_INPUT_QUEUE 64

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
    uint32_t       win_uid;      /* window_t.uid - NOT a window_t*: see gui.h */
    /* ---- v2: shared pixel buffer (SYS_WIN2_*) ---- */
    uint32_t      *pixels;       /* kernel address (identity-mapped heap) */
    int32_t        pix_w, pix_h;
    uint64_t       user_va;      /* where the same pages are mapped for the owner */
    uint64_t       pix_bytes;    /* page-rounded allocation size */
    uint32_t       present_version;
    bool           closed_by_user;
    cos_win_event_t events[COS_APP_MAX_INPUT_QUEUE];
    uint32_t       ev_head, ev_tail, ev_dropped;
    int32_t        last_mx, last_my;
    uint8_t        last_buttons;
    char           path[256];    /* last COS_EV_OPEN / COS_EV_DROP path */
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

    for (int i = 0; i < COS_APP_MAX_WINDOWS; ++i) {
        if (g_surfaces[i].used) continue;

        window_t *win = gui_open_window(WIN_COS_APP,
                                        title ? title : "C-OS App",
                                        120 + i * 24, 90 + i * 24,
                                        width, height);
        if (!win) return -1;

        g_surfaces[i].used       = true;
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

    if (s->pixels) {
        /* v2: the app's own pixels. In GPU draw mode the buffer is a GPU
         * texture re-uploaded only when present_version changes. */
        int bw = s->pix_w < cw ? s->pix_w : cw;
        int bh = s->pix_h < ch ? s->pix_h : ch;
        if (!vga_gpu_draw_image(s, s->pixels, s->pix_w, s->pix_h, s->pix_w,
                                s->present_version, false, cx, cy)) {
            vga_copy_rect_strided(cx, cy, bw, bh, s->pixels, s->pix_w);
        }
        return;
    }

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
            g_surfaces[i].rect_count = 0;
            g_surfaces[i].text_count = 0;
            g_surfaces[i].input_head = 0;
            g_surfaces[i].input_tail = 0;
            g_surfaces[i].mouse_head = 0;
            g_surfaces[i].mouse_tail = 0;
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

/* GUI thread, once per frame. Closes windows whose app closed them, and
 * any WIN_COS_APP window whose surface no longer exists (owner exited). */
void cos_app_window_gui_tick(void)
{
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
    cos_app_surface_t *s = &g_surfaces[handle - 1];

    uint64_t bytes = ((uint64_t)w * (uint64_t)h * 4u + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    /* Page-aligned and page-rounded, so no other kernel object shares a
     * page that becomes user-visible. */
    uint32_t *pix = (uint32_t *)kmalloc_aligned(bytes, PAGE_SIZE);
    if (!pix) { cos_app_window_close(handle, owner_pid); return -1; }
    memset(pix, 0xF4, bytes);                       /* light neutral until the app draws */
    uint64_t va = COS_SURF_VA_BASE + (uint64_t)(handle - 1) * COS_SURF_VA_SLOT;
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        /* Runs in the owner's syscall, so "current" is its address space. */
        paging_unmap_page(va + off);                /* stale mapping from an earlier window in this slot */
        if (!paging_map_page(va + off, (uint64_t)(uintptr_t)pix + off, PAGE_PRESENT | PAGE_RW | PAGE_USER)) {
            for (uint64_t u = 0; u < off; u += PAGE_SIZE) paging_unmap_page(va + u);
            kfree(pix);
            cos_app_window_close(handle, owner_pid);
            return -1;
        }
    }
    uint64_t flags = sync_irq_save();
    s->pixels = pix;
    s->pix_w = w;
    s->pix_h = h;
    s->user_va = va;
    s->pix_bytes = bytes;
    s->present_version = 1;
    s->closed_by_user = false;
    s->ev_head = s->ev_tail = s->ev_dropped = 0;
    s->last_buttons = 0;
    sync_irq_restore(flags);

    { extern void cos_music_on_window_created(uint32_t pid); cos_music_on_window_created(owner_pid); }
    { extern void cos_studio_on_window_created(uint32_t pid); cos_studio_on_window_created(owner_pid); }
    { extern void cos_files_on_window_created(uint32_t pid); cos_files_on_window_created(owner_pid); }
    out->handle = handle;
    out->pixels = va;
    out->width = w;
    out->height = h;
    out->stride = w;
    out->reserved = 0;
    gui_request_redraw();
    return handle;
}

bool cos_app_window_present(int64_t handle, uint32_t caller_pid)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid || !s->pixels) return false;
    ++s->present_version;
    gui_request_redraw();
    return true;
}

int cos_app_window_poll_event(int64_t handle, uint32_t caller_pid, cos_win_event_t *out)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return -1;
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

bool cos_app_window_close(int64_t handle, uint32_t caller_pid)
{
    cos_app_surface_t *s = surface_from_handle(handle);
    if (!s || s->owner_pid != caller_pid) return false;
    /* Explicit close from the owner: unmap its view of the buffer (we are
     * in its address space), then free it and drop the GUI window. */
    if (s->pixels) {
        for (uint64_t off = 0; off < s->pix_bytes; off += PAGE_SIZE) paging_unmap_page(s->user_va + off);
    }
    uint32_t uid = s->win_uid;
    uint32_t *pix = s->pixels;
    uint64_t flags = sync_irq_save();
    s->used = false;
    s->win_uid = 0;
    s->pixels = NULL;
    sync_irq_restore(flags);
    if (pix) kfree(pix);
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
        while (path[n] && n < sizeof(s->path) - 1) { s->path[n] = path[n]; ++n; }
        s->path[n] = '\0';
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
    while (s->path[n] && n + 1 < cap) { out[n] = s->path[n]; ++n; }
    out[n] = '\0';
    sync_irq_restore(flags);
    return (int)n;
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
