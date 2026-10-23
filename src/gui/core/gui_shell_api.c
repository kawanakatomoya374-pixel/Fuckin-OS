/* gui_shell_api.c - the window-manager side of the ring-3 taskbar.
 *
 * Syscalls run on other threads, but windows[] belongs to the GUI thread (gui_bring_to_front() rotates it). So the GUI
 * thread publishes a snapshot once per tick, and commands from the shell are queued and run on the GUI thread too. */
#include "gui.h"
#include "gui_internal.h"
#include "cos_shell.h"
#include "sync.h"
#include "notification_center.h"
#include <string.h>

extern bool cos_app_window_user_close(void *win_ptr);
extern uint32_t cos_app_window_owner_pid(uint32_t uid);

#define CMD_MAX 16
typedef struct { uint8_t action; uint32_t uid; int arg; } cmd_t;

static cos_winfo_t s_snap[COS_SHELL_MAX_WINDOWS];
static int         s_snap_n;
static cmd_t       s_cmds[CMD_MAX];
static int         s_cmd_head, s_cmd_tail;
static bool        s_menu_was_open;

/* Ring-3 apps are all WIN_COS_APP windows; the taskbar icon is chosen from the title instead. */
static bool has(const char *t, const char *k)
{
    size_t kl = strlen(k);
    for (; *t; t++) {
        size_t i = 0;
        while (i < kl && t[i] && ((t[i] | 0x20) == (k[i] | 0x20))) i++;
        if (i == kl) return true;
    }
    return false;
}

static int icon_kind_for_title(const char *t)
{
    static const struct { const char *key; int kind; } map[] = {
        {"task", WIN_TASK_MGR}, {"memory", WIN_MEMORY_MGR}, {"storage", WIN_STORAGE}, {"system info", WIN_SYSINFO},
        {"sysinfo", WIN_SYSINFO}, {"about", WIN_ABOUT}, {"graph", WIN_CALC_GRAPH}, {"calc", WIN_CALC},
        {"text editor", WIN_TEXT_EDITOR}, {"editor", WIN_TEXT_EDITOR}, {"sheet", WIN_SHEET}, {"download", WIN_HTTP_DOWNLOADER},
        {"clock", WIN_CLOCK}, {"setting", WIN_SETTINGS}, {"terminal", WIN_TERMINAL}, {"file", WIN_FILE_MGR},
        {"paint", WIN_PAINT}, {"music", WIN_MUSIC}, {"media", WIN_MUSIC}, {"studio", WIN_STUDIO}, {"py2c", WIN_TCC_IDE},
        {"photo", WIN_JPEG}, {"game", WIN_VOXEL_GAME}, {"tinygl", WIN_VOXEL_GAME}, {"cyber", WIN_VOXEL_GAME},
        {"netsurf", WIN_BROWSER}, {"message", WIN_MESSENGER},
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (has(t, map[i].key)) return map[i].kind;
    return WIN_COS_APP;
}

extern bool cos_shell_layer_visible(int layer);
extern bool cos_shell_layer_set_visible(int layer, bool visible);
extern void gui_start_launch_kind(int kind);
extern void gui_start_action(int action);

static bool start_menu_open(void) { return (ctx_menu.visible && ctx_menu.context_type == 2) || cos_shell_layer_visible(2); }
static void start_menu_hide(void)
{
    if (ctx_menu.visible && ctx_menu.context_type == 2) { ctx_menu.visible = FALSE; submenu_close(); start_menu_clear_search(); }
    if (cos_shell_layer_visible(2)) cos_shell_layer_set_visible(2, false);
    gui_request_redraw();
}

void gui_shell_note_panel_press(void)
{
    s_menu_was_open = start_menu_open();
    if (s_menu_was_open) start_menu_hide();
}

int gui_shell_ctl(int action, uint32_t uid, int arg)
{
    if (action < COS_WINCTL_ACTIVATE || action > COS_WINCTL_MENU_HIDE) return -1;
    uint64_t f = sync_irq_save();
    int next = (s_cmd_head + 1) % CMD_MAX;
    if (next == s_cmd_tail) { sync_irq_restore(f); return -1; }
    s_cmds[s_cmd_head].action = (uint8_t)action;
    s_cmds[s_cmd_head].uid = uid;
    s_cmds[s_cmd_head].arg = arg;
    s_cmd_head = next;
    sync_irq_restore(f);
    return 0;
}

int gui_shell_list(cos_winfo_t *out, int max)
{
    uint64_t f = sync_irq_save();
    int n = s_snap_n < max ? s_snap_n : max;
    if (n > 0) memcpy(out, s_snap, (size_t)n * sizeof(cos_winfo_t));
    sync_irq_restore(f);
    return n;
}

static void take_snapshot(void)
{
    static cos_winfo_t tmp[COS_SHELL_MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < window_count && n < COS_SHELL_MAX_WINDOWS; i++) {
        window_t *w = &windows[i];
        if (!w->visible) continue;
        cos_winfo_t *o = &tmp[n++];
        memset(o, 0, sizeof *o);
        o->uid = w->uid;
        o->kind = (uint16_t)(w->kind == WIN_COS_APP ? icon_kind_for_title(w->title) : w->kind);
        o->minimized = w->minimized; o->maximized = w->maximized;
        o->focused = (i == active_window && !w->minimized);
        o->visible = 1;
        if (w->kind == WIN_COS_APP) o->owner_pid = cos_app_window_owner_pid(w->uid);
        strncpy(o->title, w->title, sizeof(o->title) - 1);
    }
    uint64_t f = sync_irq_save();
    memcpy(s_snap, tmp, (size_t)n * sizeof(cos_winfo_t));
    s_snap_n = n;
    sync_irq_restore(f);
}

static void activate(int idx)
{
    if (idx < 0) return;
    if (windows[idx].minimized) windows[idx].minimized = FALSE;
    gui_bring_to_front(idx);
}

static void run_cmd(const cmd_t *c)
{
    int idx = c->uid ? gui_window_index_by_uid(c->uid) : -1;
    switch (c->action) {
    case COS_WINCTL_ACTIVATE: activate(idx); break;
    case COS_WINCTL_MINIMIZE: if (idx >= 0) windows[idx].minimized = TRUE; break;
    case COS_WINCTL_TOGGLE:
        if (idx < 0) break;
        if (windows[idx].minimized) activate(idx);
        else if (idx == active_window) windows[idx].minimized = TRUE;
        else activate(idx);
        break;
    case COS_WINCTL_CLOSE:
        if (idx < 0) break;
        if (windows[idx].kind == WIN_COS_APP && cos_app_window_user_close(&windows[idx])) break;
        gui_close_window(idx);
        break;
    case COS_WINCTL_START_MENU:
        if (!s_menu_was_open && !cos_shell_layer_set_visible(2, true)) open_start_menu(8, (int)SCREEN_H - TASKBAR_H - 220);   /* no ring-3 menu: the kernel one */
        s_menu_was_open = false;
        break;
    case COS_WINCTL_APP:      start_menu_hide(); gui_start_launch_kind(c->arg); break;
    case COS_WINCTL_SYSACT:   start_menu_hide(); gui_start_action(c->arg); break;
    case COS_WINCTL_MENU_HIDE: start_menu_hide(); break;
    case COS_WINCTL_LAUNCH:
        if (c->arg == COS_LAUNCH_BROWSER) {
            int ex = gui_find_window(WIN_BROWSER);
            if (ex >= 0) activate(ex);
            else gui_open_window(WIN_BROWSER, gui_text("NetSurf", "NetSurf"), 96, 64, 1160, 760);
        } else if (c->arg == COS_LAUNCH_CALC) {
            int ex = gui_find_window(WIN_CALC);
            if (ex >= 0) activate(ex);
            else gui_open_window(WIN_CALC, gui_text("Calculator", "電卓"), 200, 100, 400, 500);
        }
        break;
    }
    gui_request_redraw();
}

/* ---- context menu snapshot ---- */
static cos_ctxsnap_t s_csnap;
int gui_ctx_list(cos_ctxsnap_t *out)
{
    uint64_t f = sync_irq_save();
    memcpy(out, &s_csnap, sizeof *out);
    int v = s_csnap.visible;
    sync_irq_restore(f);
    return v;
}

/* ---- notifications ---- */
static cos_notif_t s_nsnap[COS_NOTIF_MAX];
static int         s_nsnap_n;

int gui_notif_list(cos_notif_t *out, int max)
{
    uint64_t f = sync_irq_save();
    int n = s_nsnap_n < max ? s_nsnap_n : max;
    if (n > 0) memcpy(out, s_nsnap, (size_t)n * sizeof(cos_notif_t));
    sync_irq_restore(f);
    return n;
}
int gui_notif_count(void) { return s_nsnap_n; }

static void notif_snapshot(void)
{
    static cos_notif_t tmp[COS_NOTIF_MAX];
    int n = 0, count = 0;
    notification_center_begin_read();
    notification_t *l = notification_get_all(&count);
    for (int i = 0; l && i < count && n < COS_NOTIF_MAX; i++) {
        cos_notif_t *o = &tmp[n++];
        memset(o, 0, sizeof *o);
        o->id = l[i].id; o->type = (int32_t)l[i].type;
        strncpy(o->title, l[i].title, sizeof(o->title) - 1);
        strncpy(o->message, l[i].message, sizeof(o->message) - 1);
    }
    notification_center_end_read();
    uint64_t f = sync_irq_save();
    memcpy(s_nsnap, tmp, (size_t)n * sizeof(cos_notif_t));
    s_nsnap_n = n;
    sync_irq_restore(f);
}

/* ---- desktop icons ---- */
typedef struct { uint8_t action; int a, b, c; } dcmd_t;
static cos_dicon_t s_dsnap[MAX_ICONS];
static int         s_dsnap_n;
static dcmd_t      s_dcmds[CMD_MAX];
static int         s_dcmd_head, s_dcmd_tail;

int gui_desktop_list(cos_dicon_t *out, int max)
{
    uint64_t f = sync_irq_save();
    int n = s_dsnap_n < max ? s_dsnap_n : max;
    if (n > 0) memcpy(out, s_dsnap, (size_t)n * sizeof(cos_dicon_t));
    sync_irq_restore(f);
    return n;
}

int gui_desktop_ctl(int action, int a, int b, int c)
{
    if (action < COS_DESKCTL_OPEN || action > COS_DESKCTL_FOCUS) return -1;
    uint64_t f = sync_irq_save();
    int next = (s_dcmd_head + 1) % CMD_MAX;
    if (next == s_dcmd_tail) { sync_irq_restore(f); return -1; }
    s_dcmds[s_dcmd_head] = (dcmd_t){ (uint8_t)action, a, b, c };
    s_dcmd_head = next;
    sync_irq_restore(f);
    return 0;
}

static void desktop_snapshot(void)
{
    static cos_dicon_t tmp[MAX_ICONS];
    int box = gui_get_desktop_icon_render_size();
    int n = desktop_icon_count < MAX_ICONS ? desktop_icon_count : MAX_ICONS;
    for (int i = 0; i < n; i++) {
        desktop_icon_t *ic = &desktop_icons[i];
        cos_dicon_t *o = &tmp[i];
        memset(o, 0, sizeof *o);
        o->x = ic->x; o->y = ic->y; o->win_kind = ic->win_kind; o->is_file = ic->is_file; o->is_dynamic = ic->is_dynamic;
        o->folder_id = ic->folder_id; o->box = box; o->cell_w = box + 24; o->cell_h = box + 30;
        strncpy(o->label, ic->label, sizeof(o->label) - 1);
    }
    uint64_t f = sync_irq_save();
    memcpy(s_dsnap, tmp, (size_t)n * sizeof(cos_dicon_t));
    s_dsnap_n = n;
    sync_irq_restore(f);
}

static void desktop_run(const dcmd_t *c)
{
    switch (c->action) {
    case COS_DESKCTL_OPEN: gui_desktop_open_icon(c->a); break;
    case COS_DESKCTL_MOVE:
        if (c->a >= 0 && c->a < desktop_icon_count) { desktop_icons[c->a].x = c->b; desktop_icons[c->a].y = c->c; }
        break;
    case COS_DESKCTL_DROP: gui_desktop_drop_icon(c->a, c->b, c->c); break;
    case COS_DESKCTL_CONTEXT:
        open_desktop_context_menu(c->b, c->c, c->a >= 0 && c->a < desktop_icon_count ? c->a : -1);
        break;
    case COS_DESKCTL_FOCUS:
        if (ctx_menu.visible) ctx_menu.visible = FALSE;
        break;
    }
    gui_request_redraw();
}

extern void cos_shell_supervise(void);

void gui_shell_tick(void)
{
    cos_shell_supervise();
    take_snapshot();
    desktop_snapshot();
    notif_snapshot();
    {
        static cos_ctxsnap_t tmp;
        gui_ctx_snapshot(&tmp);
        uint64_t f = sync_irq_save();
        memcpy(&s_csnap, &tmp, sizeof tmp);
        sync_irq_restore(f);
    }
    for (;;) {
        dcmd_t d;
        uint64_t f = sync_irq_save();
        if (s_dcmd_tail == s_dcmd_head) { sync_irq_restore(f); break; }
        d = s_dcmds[s_dcmd_tail];
        s_dcmd_tail = (s_dcmd_tail + 1) % CMD_MAX;
        sync_irq_restore(f);
        desktop_run(&d);
    }
    for (;;) {
        cmd_t c;
        uint64_t f = sync_irq_save();
        if (s_cmd_tail == s_cmd_head) { sync_irq_restore(f); break; }
        c = s_cmds[s_cmd_tail];
        s_cmd_tail = (s_cmd_tail + 1) % CMD_MAX;
        sync_irq_restore(f);
        run_cmd(&c);
    }
}

