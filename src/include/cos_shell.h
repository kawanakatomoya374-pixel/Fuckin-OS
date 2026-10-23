/* cos_shell.h - what the ring-3 shell (taskbar) may ask of the window manager. Mirrored in userland/include/cos.h. */
#ifndef COS_SHELL_H
#define COS_SHELL_H

#include <stdint.h>

typedef struct {
    uint32_t uid, owner_pid;
    uint16_t kind;
    uint8_t  minimized, maximized, focused, visible;
    uint16_t pad;
    char     title[46];
} cos_winfo_t;                                  /* 64 bytes */

/* layers: 1 taskbar, 2 start menu, 3 desktop */
enum { COS_WINCTL_ACTIVATE = 1, COS_WINCTL_MINIMIZE, COS_WINCTL_TOGGLE, COS_WINCTL_CLOSE, COS_WINCTL_START_MENU, COS_WINCTL_LAUNCH,
       COS_WINCTL_APP,          /* arg = COS_APPK_*: open the app, or bring it to the front; hides the start menu */
       COS_WINCTL_SYSACT,       /* arg = COS_SA_*: a system action (lock, power, language, ...); hides the start menu */
       COS_WINCTL_MENU_HIDE };

/* One icon for the compositor to draw over a shell surface (SYS_SHELL_ICONS). kind is a COS_APPK_* value. */
typedef struct { int32_t kind, x, y, size; } cos_shell_icon_t;
#define COS_SHELL_MAX_ICONS 96   /* kind < 0 on layer 3 means desktop icon number (-kind - 1) */

/* The desktop (layer 3): the ring-3 desktop program draws labels/selection on a colour-keyed surface; the kernel owns the
 * icon list. SYS_DESKTOP_LIST returns these; SYS_DESKTOP_CTL acts on them. */
#define COS_DESKTOP_KEY 0x00FF00FFu
typedef struct { int32_t x, y, win_kind, is_file, is_dynamic, folder_id, box, cell_w, cell_h; char label[32]; } cos_dicon_t;
enum { COS_DESKCTL_OPEN = 1,       /* a = icon index */
       COS_DESKCTL_DROP,           /* a = icon index, b,c = pointer x,y at drop; the icon is at its dragged position (SYS_DESKTOP_MOVE first) */
       COS_DESKCTL_MOVE,           /* a = icon index, b,c = new x,y (no snapping: used while dragging) */
       COS_DESKCTL_CONTEXT,        /* a = icon index or -1, b,c = pointer x,y: the kernel context menu */
       COS_DESKCTL_FOCUS };        /* click on empty desktop: close menus */

/* App kinds (the kernel's WIN_* numbers) the shell may launch and draw icons for. */
enum { COS_APPK_FILE_MGR = 1, COS_APPK_TEXT_EDITOR = 2, COS_APPK_TERMINAL = 3, COS_APPK_SETTINGS = 4, COS_APPK_ABOUT = 5,
       COS_APPK_CALC = 6, COS_APPK_STORAGE = 7, COS_APPK_BROWSER = 8, COS_APPK_TASK_MGR = 9, COS_APPK_PAINT = 10,
       COS_APPK_MUSIC = 11, COS_APPK_CLOCK = 12, COS_APPK_SYSINFO = 13, COS_APPK_STUDIO = 14, COS_APPK_CALC_GRAPH = 15,
       COS_APPK_SHEET = 16, COS_APPK_VOXEL_GAME = 17, COS_APPK_JPEG = 18, COS_APPK_MEMORY_MGR = 19, COS_APPK_TCC_IDE = 20,
       COS_APPK_HTTP_DOWNLOADER = 21, COS_APPK_TINYGL = 22, COS_APPK_2DGAMES = 23, COS_APPK_GENERIC = 24, COS_APPK_MESSENGER = 26 /* = WIN_MESSENGER */ };

/* System actions, same numbering as the kernel start menu's START_ENTRY_*. */
enum { COS_SA_SHOW_DESKTOP = 2, COS_SA_LOCK, COS_SA_RESTART, COS_SA_POWEROFF, COS_SA_LANG_EN, COS_SA_LANG_JA, COS_SA_DARK_MODE,
       COS_SA_ANIMATIONS, COS_SA_NOTIFICATIONS, COS_SA_MOUSE_RAW, COS_SA_ABOUT, COS_SA_REFRESH_DESKTOP, COS_SA_TEST_AUDIO,
       COS_SA_IMAGE_WALLPAPER, COS_SA_CALC_HISSAN };
enum { COS_LAUNCH_BROWSER = 1, COS_LAUNCH_CALC = 2 };

#define COS_SHELL_MAX_WINDOWS 64

/* GUI-thread side (gui_shell_api.c) */
/* Toasts (layer 4): the kernel keeps the notification list and its expiry; the ring-3 notifier draws what is on screen. */
typedef struct { uint32_t id; int32_t type; char title[48]; char message[96]; } cos_notif_t;
#define COS_NOTIF_MAX 8
int  gui_notif_list(cos_notif_t *out, int max);
int  gui_notif_count(void);
/* Context menus (layer 5): input and actions stay in the kernel; the ring-3 program only draws what the kernel describes
 * here (geometry and hover included). Its seq changes whenever the menu's structure does (not on hover). */
typedef struct {
    int32_t seq, visible, x, y, w, h;
    char header[48];
    int32_t n;
    struct { int32_t y, h, icon; uint8_t enabled, checked, sep, sub, hover, pad[3]; char label[48]; } it[32];
    int32_t sm_visible, sm_x, sm_y, sm_w, sm_h;
    char sm_title[48];
    int32_t sm_n;
    struct { int32_t y, h; uint8_t hover, pad[3]; char label[48]; } sm[16];
} cos_ctxsnap_t;
int  gui_ctx_snapshot(cos_ctxsnap_t *out);    /* GUI thread only */
int  gui_ctx_list(cos_ctxsnap_t *out);        /* any thread: latest snapshot; returns visible */
int  gui_desktop_list(cos_dicon_t *out, int max);          /* latest snapshot */
int  gui_desktop_ctl(int action, int a, int b, int c);     /* queued to the GUI thread */
void gui_desktop_open_icon(int i);                         /* GUI thread */
void gui_desktop_drop_icon(int i, int mx, int my);
void gui_shell_tick(void);                       /* once per GUI tick: refresh the snapshot, run queued commands */
void gui_shell_note_panel_press(void);           /* the panel was pressed: close the kernel start menu, remember if it was open */
int  gui_shell_list(cos_winfo_t *out, int max);  /* any thread: copy the latest snapshot */
int  gui_shell_ctl(int action, uint32_t uid, int arg);   /* any thread: queue a command */

#endif
