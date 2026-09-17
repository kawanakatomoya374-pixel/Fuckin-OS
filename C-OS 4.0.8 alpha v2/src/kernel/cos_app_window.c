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
#include "serial.h"
#include "string.h"
#include "task.h"
#include "sync.h"
#include "gui.h"
#include "vga.h"

extern void gui_bring_to_front(int idx);

#define COS_APP_MAX_WINDOWS 4
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
    window_t      *win;
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
} cos_app_surface_t;

static cos_app_surface_t g_surfaces[COS_APP_MAX_WINDOWS];

/* Handles are 1-based so 0 is never a valid handle and an uninitialised
 * variable in a user program cannot accidentally address a window. */
static cos_app_surface_t *surface_from_handle(int64_t handle)
{
    if (handle < 1 || handle > COS_APP_MAX_WINDOWS) return NULL;
    cos_app_surface_t *s = &g_surfaces[handle - 1];
    if (!s->used || s->win == NULL) return NULL;
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
        g_surfaces[i].win        = win;
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
        if (!s->used || s->win != win) continue;
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
        if (g_surfaces[i].used && g_surfaces[i].win == w) { s = &g_surfaces[i]; break; }
    }
    if (!s) return;

    int cx = w->x;
    int cy = w->y + TITLEBAR_H;
    int cw = w->w;
    int ch = w->h - TITLEBAR_H;
    if (cw <= 0 || ch <= 0) return;

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
            g_surfaces[i].win = NULL;
            g_surfaces[i].rect_count = 0;
            g_surfaces[i].text_count = 0;
            g_surfaces[i].input_head = 0;
            g_surfaces[i].input_tail = 0;
        }
    }
    sync_irq_restore(flags);
}
