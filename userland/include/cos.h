/**
 * cos.h - the C API for .c-os userland programs.
 *
 * WHY THIS EXISTS
 * ----------------
 * Every .c-os program written before this had to be hand-written x86-64
 * assembly, because there was no C runtime at all: no _start that sets up
 * argc/argv per the ABI, no syscall wrappers, no malloc, no string or
 * printf functions. That made userland effectively write-only - fine for
 * small syscall validation tests, impossible for a real application.
 *
 * This is a freestanding C library (no host libc, no OS headers) built
 * against the kernel's actual syscall numbers. Link with cos_crt0.o and
 * libcos.a using validation/cos_programs/cos.ld.
 *
 * DELIBERATELY NOT A POSIX LIBC
 * ------------------------------
 * The names here mirror POSIX where the semantics genuinely match
 * (open/read/write/close/malloc/printf), but this does NOT aim to be a
 * drop-in libc - the kernel underneath does not provide POSIX semantics
 * for everything (no fd 0/1/2 by default, whole-file helpers alongside
 * streaming fds, spawn instead of fork). Pretending otherwise would make
 * ported code compile and then behave subtly wrong, which is worse than
 * an honestly different API.
 */
#ifndef COS_H
#define COS_H

typedef unsigned long  size_t;
typedef long           ssize_t;
typedef unsigned long  uint64_t;
typedef long           int64_t;
typedef unsigned int   uint32_t;
typedef int            int32_t;
typedef unsigned short uint16_t;
typedef short          int16_t;
typedef unsigned char  uint8_t;
typedef signed char    int8_t;
typedef unsigned long  uintptr_t;
typedef long           intptr_t;
typedef unsigned long  uintmax_t;
/* Compatible with <stdbool.h> in either include order: if it came first,
 * bool/true/false already exist; if it comes later, its own definitions
 * are token-identical to these. */
#ifndef __bool_true_false_are_defined
typedef _Bool          bool;
#define true  1
#define false 0
#endif
#ifndef NULL
#define NULL  ((void*)0)
#endif

/* ---- process ---------------------------------------------------------- */
void     cos_exit(int status) __attribute__((noreturn));
int      cos_getpid(void);
void     cos_yield(void);
void     cos_sleep_ms(uint64_t ms);
/* Returns the child's pid, or -1. */
int64_t  cos_spawn(const char *path);
/* Spawn with arguments and an environment, both NULL-terminated arrays.
 * cos_spawn() above cannot pass either - the syscall behind it takes
 * only a path - so a child started that way always saw argc == 1 and an
 * empty environment. */
int64_t  cos_spawn_argv(const char *path, const char *const *argv,
                        const char *const *envp);

/* ---- IPC: message passing and shared memory ---------------------------
 *
 * The kernel has had a full IPC implementation (src/kernel/ipc.c) for
 * some time with no syscall exposing it, which meant a userspace file
 * server or GUI server could not receive a request or answer one - the
 * single thing blocking useful userspace servers. These are that gate.
 *
 * All return an IPC_* status code (0 == success, negative == error),
 * deliberately NOT flattened into -1/errno: a server loop needs to tell
 * "timed out" apart from "queue full" apart from "no such pid", and
 * collapsing those is what turns a recoverable condition into a spin. */

#define COS_IPC_MAX_DATA_SIZE 1024        /* must match kernel IPC_MAX_DATA_SIZE */
#define COS_IPC_WAIT_FOREVER  0xFFFFFFFFULL

#define COS_IPC_SUCCESS                  0
#define COS_IPC_ERROR_NOT_INITIALIZED   -1
#define COS_IPC_ERROR_INVALID_PID       -2
#define COS_IPC_ERROR_DATA_TOO_LARGE    -3
#define COS_IPC_ERROR_QUEUE_FULL        -4
#define COS_IPC_ERROR_TIMEOUT           -5
#define COS_IPC_ERROR_PERMISSION_DENIED -6

/* Filled in by cos_ipc_recv(). Layout must stay exactly four uint64_t in
 * this order - the kernel writes it as a raw u64[4] (see SYS_IPC_RECV in
 * src/kernel/syscall.c), so reordering or resizing these fields silently
 * corrupts every received message rather than failing to compile. */
typedef struct {
    uint64_t src_pid;
    uint64_t msg_type;
    uint64_t length;    /* bytes actually written into the caller's buffer */
    uint64_t reserved;  /* always 0 today */
} cos_ipc_msg_t;

int cos_ipc_send(uint64_t dst_pid, uint64_t msg_type,
                 const void *data, uint64_t len);
/* Blocks until a message arrives or timeout_ms elapses; pass
 * COS_IPC_WAIT_FOREVER for a server's main loop. On success `out` carries
 * the sender, type and payload length; the payload itself is written into
 * `buf` (truncated to buf_cap, never overrunning it). */
int cos_ipc_recv(cos_ipc_msg_t *out, void *buf, uint64_t buf_cap,
                 uint64_t timeout_ms);
int cos_ipc_respond(uint64_t dst_pid, uint64_t msg_id, uint64_t status,
                    const void *data, uint64_t len);
int cos_ipc_notify(uint64_t dst_pid, uint64_t event_type,
                   const void *data, uint64_t len);
/* Shared memory, for payloads too large for the 1 KiB message limit -
 * a GUI server's framebuffer being the obvious case. */
int cos_shm_create(uint64_t size, uint64_t *out_shm_id);
int cos_shm_attach(uint64_t shm_id, void **out_addr);
/* blocking=false performs a WNOHANG-style single check. Returns the
 * collected child's pid, or -1. */
int64_t  cos_waitpid(int64_t pid, int *status, bool blocking);

/* ---- console (serial) -------------------------------------------------- */
ssize_t  cos_write(const char *buf, size_t len);
void     cos_puts(const char *s);
/* Supports %s %d %i %u %x %c %% and %ld/%lu/%lx. No floating point - the
 * kernel's own vsnprintf has no float support either, and silently
 * printing a literal "%f" (which is what that gap causes) would be worse
 * than not offering the specifier at all. */
/* A complete C99 formatter: flags (-+ #0), field width (including `*`),
 * precision (including `*`), the length modifiers hh h l ll z t j L, and
 * the conversions d i u o x X c s p f F e E g G %%.
 *
 * `%n` is REFUSED rather than ignored. It writes through a pointer from
 * the argument list, which is the classic format-string exploit
 * primitive; a program using it gets a visible marker in the output
 * instead of a silent arbitrary write.
 *
 * Floating point IS supported now. The previous formatter had none, and
 * emitted the literal text "%f" - which is worse than refusing, because
 * it turns a missing feature into corrupted output. */
int      cos_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int      cos_snprintf(char *buf, size_t size, const char *fmt, ...)
             __attribute__((format(printf, 3, 4)));
int      cos_vprintf(const char *fmt, __builtin_va_list ap);
int      cos_vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

/* ---- memory ------------------------------------------------------------ */
void    *cos_sbrk(long increment);
void    *cos_malloc(size_t size);
void    *cos_calloc(size_t count, size_t size);
void    *cos_realloc(void *ptr, size_t size);
void     cos_free(void *ptr);
void    *cos_aligned_alloc(size_t align, size_t size); /* release with cos_free()/free() */
/* Anonymous mmap. prot is a COS_PROT_* bitmask. */
#define COS_PROT_READ  1u
#define COS_PROT_WRITE 2u
#define COS_PROT_EXEC  4u
void    *cos_mmap(size_t length, uint32_t prot);
int      cos_munmap(void *addr, size_t length);
int      cos_mprotect(void *addr, size_t length, uint32_t prot);

/* ---- whole-file I/O ---------------------------------------------------- */
ssize_t  cos_read_file(const char *path, void *buf, size_t len);
ssize_t  cos_write_file(const char *path, const void *buf, size_t len);

/* ---- streaming file descriptors ---------------------------------------- */
#define COS_O_RDONLY        1
#define COS_O_WRONLY_CREATE 2
#define COS_O_WRONLY_APPEND 3
#define COS_O_RDWR_CREATE   4

#define COS_DIRENT_NAME_MAX 255
typedef struct {
    char     name[COS_DIRENT_NAME_MAX + 1];
    uint8_t  is_dir;
    uint64_t size;
} cos_dirent_t;

int      cos_open(const char *path, uint32_t flags);
int      cos_opendir(const char *path);
ssize_t  cos_fd_read(int fd, void *buf, size_t len);
ssize_t  cos_fd_write(int fd, const void *buf, size_t len);
int64_t  cos_lseek(int fd, int64_t offset);
/* Returns 1 (entry filled), 0 (end of directory - a real outcome, not an
 * error), or -1. */
int      cos_readdir(int fd, cos_dirent_t *out);
int      cos_close(int fd);

/* ---- shared libraries (.c-osll) ---------------------------------------- */
long     cos_syscall3(long nr, long a, long b, long c);   /* a call a kernel module provides (numbers 0x1000..0x103F) */
int64_t  cos_dlopen(const char *path);
void    *cos_dlsym(int64_t handle, const char *name);

/* ---- signals ----------------------------------------------------------- */
typedef void (*cos_sighandler_t)(int);
#define COS_SIG_DFL ((cos_sighandler_t)0)
#define COS_SIG_IGN ((cos_sighandler_t)1)
int      cos_signal(int signum, cos_sighandler_t handler);
int      cos_kill(int pid, int signum);

/* ---- GUI windows ------------------------------------------------------- */
#define COS_KEY_NONE      0
#define COS_KEY_ENTER     1
#define COS_KEY_BACKSPACE 2
#define COS_KEY_ESC       3
#define COS_KEY_UP        4
#define COS_KEY_DOWN      5
#define COS_KEY_LEFT      6
#define COS_KEY_RIGHT     7
#define COS_KEY_TAB       8
#define COS_KEY_DELETE    9
/* Navigation and function keys. Added so an editor can bind Home/End,
 * PageUp/PageDown and F1..F12 (F5 = run). Home/End/PageUp/PageDown are the
 * extended (E0-prefixed) keys only - the keypad 7/1/9/3 keys keep producing
 * their digits. F1..F12 are COS_KEY_F1 + n - 1. */
#define COS_KEY_HOME      10
#define COS_KEY_END       11
#define COS_KEY_PGUP      12
#define COS_KEY_PGDN      13
#define COS_KEY_INSERT    14
#define COS_KEY_F1        16
#define COS_KEY_F12       27

#define COS_MOD_SHIFT 0x01u
#define COS_MOD_CTRL  0x02u
#define COS_MOD_ALT   0x04u

typedef struct {
    char    ascii;
    uint8_t special;
    uint8_t modifiers;
} cos_key_event_t;

#define COS_MOUSE_BTN_LEFT   0x01u
#define COS_MOUSE_BTN_RIGHT  0x02u
#define COS_MOUSE_BTN_MIDDLE 0x04u

typedef struct {
    int32_t x, y;
    uint8_t button;
} cos_mouse_event_t;

int64_t  cos_win_create(const char *title, int32_t w, int32_t h);
int      cos_win_fill(int64_t win, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int      cos_win_text(int64_t win, int32_t x, int32_t y, uint32_t fg, uint32_t bg, const char *s);
int      cos_win_clear(int64_t win);
/* Returns 1 if a key was popped, 0 if none pending, -1 on error. */
int      cos_win_poll_key(int64_t win, cos_key_event_t *out);
/* Returns 1 if a click was popped, 0 if none pending, -1 on error. */
int      cos_win_poll_mouse(int64_t win, cos_mouse_event_t *out);

/* ---- threads ------------------------------------------------------------
 * The kernel always supported multiple threads per process; nothing
 * exposed it to ring3 until now. */
typedef void (*cos_thread_fn)(void *arg);
/* Returns a thread id (>0) or -1. */
int64_t  cos_thread_create(cos_thread_fn fn, void *arg);
void     cos_thread_exit(int code) __attribute__((noreturn));
/* Returns 0 once the thread has finished, -1 on timeout. */
int      cos_thread_join(int64_t tid, uint64_t timeout_ms);

/* ---- time --------------------------------------------------------------- */
uint64_t cos_time_ms(void);     /* monotonic milliseconds since boot */
typedef struct { uint32_t year, month, day, hour, minute, second; } cos_datetime_t;
int cos_rtc_now(cos_datetime_t *out);   /* real wall-clock date/time; cos_time_ms() above is boot-relative only */

/* Everything the ring-3 Settings app (userland/programs/settings) reads in one call,
 * and the small set of preferences it can write back one at a time via cos_settings_set().
 * Fixed-size char arrays rather than pointers: this crosses the syscall boundary as a flat
 * struct copy, the same way cos_stat_ex_t does. */
typedef struct {
    uint64_t mem_total, mem_used, mem_free;
    uint64_t storage_total, storage_used, storage_free;
    char     cpu_vendor[64];
    char     os_version[32];
    char     build_info[96];
    int32_t  screen_w, screen_h;
    int32_t  net_connected;
    char     net_ip[16];
    char     net_mac[24];
    char     net_iface[16];
    char     net_netmask[16];
    char     net_gateway[16];
    int32_t  theme_idx;       /* 1..4 */
    int32_t  font_scale;      /* 1..4 */
    int32_t  wallpaper_idx;
    int32_t  wallpaper_count;
    int32_t  dark_mode;       /* 0/1 */
    int32_t  japanese;        /* 0/1 */
    int32_t  icon_size;
    int32_t  file_count;
    /* ---- parity with the old kernel-drawn Settings panel's 15 tabs ---- */
    int32_t  developer_mode;        /* 0/1 */
    uint32_t auto_save_interval;    /* seconds */
    int32_t  font_family;           /* 0..5, see cos_font_family_name() */
    int32_t  font_pixel_width;      /* 1..32 */
    int32_t  render_backend;        /* 0=CPU, 1=GPU(VirtIO) */
    int32_t  render_backend_gpu_available;  /* 0/1 */
    int32_t  gpu_full_draw;         /* 0/1 */
    int32_t  virgl_available;       /* 0/1 - gates "GPU (Full)" being offered at all */
    int32_t  window_animations;     /* 0/1 */
    int32_t  notifications_enabled; /* 0/1 */
    int32_t  fps_overlay;           /* 0/1 */
    int32_t  autostart_terminal;    /* 0/1 */
    int32_t  autostart_file_manager;/* 0/1 */
    int32_t  autostart_browser;     /* 0/1 */
    int32_t  mouse_sensitivity;     /* 1..4 */
    int32_t  mouse_raw_input;       /* 0/1 */
    int32_t  mouse_drag_threshold;  /* 1..10 (px) */
    int32_t  multi_cursor;          /* 0/1 */
    int32_t  efm_show_hidden;       /* 0/1 - File Manager default */
    int32_t  efm_confirm_delete;    /* 0/1 - File Manager default */
    int32_t  efm_default_view;      /* 0=List 1=Icons 2=Details 3=Thumbnails */
    int32_t  terminal_autoscroll;   /* 0/1 */
    int32_t  has_password;          /* 0/1 - whether a password is set at all */
    int32_t  desktop_color;         /* 0 = the wallpaper, else 0x01RRGGBB: a plain colour (top of the gradient) instead of the wallpaper */
    int32_t  desktop_color2;        /* bottom of the gradient, same format (0 = same as the top) */
} cos_settings_t;
enum {
    COS_SET_THEME = 1, COS_SET_FONT_SCALE, COS_SET_WALLPAPER,
    COS_SET_DARK_MODE, COS_SET_LANGUAGE, COS_SET_ICON_SIZE,
    COS_SET_DEVELOPER_MODE, COS_SET_AUTO_SAVE_INTERVAL, COS_SET_FONT_FAMILY,
    COS_SET_FONT_PIXEL_WIDTH, COS_SET_RENDER_BACKEND, COS_SET_GPU_FULL_DRAW,
    COS_SET_WINDOW_ANIMATIONS, COS_SET_NOTIFICATIONS, COS_SET_FPS_OVERLAY,
    COS_SET_AUTOSTART_TERMINAL, COS_SET_AUTOSTART_FILE_MANAGER, COS_SET_AUTOSTART_BROWSER,
    COS_SET_MOUSE_SENSITIVITY, COS_SET_MOUSE_RAW_INPUT, COS_SET_MOUSE_DRAG_THRESHOLD,
    COS_SET_MULTI_CURSOR, COS_SET_EFM_SHOW_HIDDEN, COS_SET_EFM_CONFIRM_DELETE,
    COS_SET_EFM_DEFAULT_VIEW, COS_SET_TERMINAL_AUTOSCROLL,
    COS_SET_DESKTOP_COLOR, COS_SET_DESKTOP_COLOR2,      /* 0x01RRGGBB sets a plain/gradient background in place of the wallpaper, 0 gives the wallpaper back */
};
/* Fire-and-forget actions that aren't a simple scalar preference - see
 * SYS_SETTINGS_ACTION in src/kernel/syscall.c. `arg` is action-specific
 * (unused by most); returns 0 on success, -1 if the action could not be
 * carried out (e.g. ACPI sleep unsupported by this platform/QEMU config). */
enum {
    COS_ACTION_POWER_SLEEP = 1, COS_ACTION_POWER_RESTART, COS_ACTION_POWER_SHUTDOWN,
    COS_ACTION_NET_DHCP_RENEW, COS_ACTION_OPEN_TERMINAL, COS_ACTION_SECURITY_FULL_SCAN,
};
int cos_settings_get(cos_settings_t *out);
int cos_settings_set(int key, int value);
int cos_settings_action(int action, int arg);
/* Changes the single system account's password. Verifies old_password
 * first (same as the old kernel user_change_password()); returns 0 on
 * success, -1 if old_password is wrong or no account exists yet (use
 * old_password="" to set an initial password when !has_password). */
int cos_password_change(const char *old_password, const char *new_password);
/* Kernel modules (.c-osys). Loading and unloading need a fully privileged (never a sandboxed) caller and the administrator password (NULL when no account is
 * set). cos_kmod_unload: 0, -1 no such module, -2 in use. cos_kmod_list fills buf with "name version level\n" per module and returns the count. */
int cos_kmod_load(const char *path, const char *password);
int cos_kmod_unload(const char *name, const char *password);
int cos_kmod_list(char *buf, uint64_t cap);
/* Records of the audit.c-osys module (system call 0x1010, needs COS_CAP_SYSINFO) and of pci.c-osys (0x1020 count, 0x1021 get, needs COS_CAP_DEVICES). Layouts
 * mirror userland/drivers/audit/audit.c and pci/pci.c. Types: 0 process started (a = parent), 1 exited (b = status), 2 sandbox refusal (b = missing caps << 16 | call
 * number), 3 capabilities narrowed (b = caps now held). */
typedef struct { uint64_t seq, time_ms; uint32_t type, pid; uint64_t a, b; } cos_audit_rec_t;
typedef struct { uint8_t bus, dev, fn, hdr; uint16_t vendor, device; uint32_t class_rev; } cos_pci_info_t;
const char *cos_font_family_name(int family);  /* local table, 0..5 - no syscall needed */
uint64_t cos_time_unix(void);   /* wall-clock seconds from the RTC */

/* ---- filesystem mutation ------------------------------------------------- */
typedef struct {
    uint64_t size;
    uint8_t  is_dir;
} cos_stat_t;

int cos_chdir(const char *path);               /* working directory of this process: relative paths in every cos_* / stdio call are taken from it */
char *cos_getcwd(char *buf, size_t size);
int cos_mkdir(const char *path);
int cos_unlink(const char *path);
int cos_rename(const char *oldp, const char *newp);
int cos_stat(const char *path, cos_stat_t *out);

/* ---- file-manager support (kernel: cos_fm_support.c) -------------------- */
#define COS_ATTR_RDONLY  0x01
#define COS_ATTR_HIDDEN  0x02
#define COS_ATTR_SYSTEM  0x04
#define COS_ATTR_ARCHIVE 0x20
typedef struct { uint64_t size; uint64_t mtime; uint8_t is_dir; uint8_t attr; uint8_t pad[6]; } cos_stat_ex_t;
typedef struct { char name[256]; uint64_t size; uint64_t mtime; uint8_t is_dir; uint8_t attr; uint8_t pad[6]; } cos_dirent_ex_t;
#define COS_LISTDIR_MAX 1024
#define COS_CLIP_MAX    (256u * 1024u)
#define COS_UI_DARK     0x1u
#define COS_UI_JAPANESE 0x2u
int      cos_stat_ex(const char *path, cos_stat_ex_t *out);      /* size, mtime (unix s), attributes */
/* Changes a file/directory's read-only/hidden/system/archive bits (COS_ATTR_*
 * in cos_stat_ex_t.attr). `mask` selects which bits `attr` actually changes -
 * e.g. cos_set_attr(p, COS_ATTR_RDONLY, COS_ATTR_RDONLY) sets read-only
 * without touching hidden/system/archive. Returns 0/-1. */
int      cos_set_attr(const char *path, uint8_t attr, uint8_t mask);
/* A counter that increases every time mkdir/unlink/rename succeeds
 * anywhere on the volume - there is no real per-directory change
 * notification on FAT32, so this is the cheap substitute: poll it and
 * only re-list a directory when it has moved, instead of either doing
 * that every frame or never. */
uint64_t cos_fs_generation(void);
int      cos_listdir_ex(const char *path, cos_dirent_ex_t *out, int max);   /* whole directory in one call; count or -1 */
int      cos_fs_space(uint64_t *total, uint64_t *free_bytes);
int      cos_open_path(const char *path);                        /* open in the default app (queued to the desktop) */
uint32_t cos_ui_prefs(void);                                     /* COS_UI_DARK | COS_UI_JAPANESE */
int64_t  cos_clip_set(const void *data, size_t len);             /* the system clipboard, shared by every app */
int64_t  cos_clip_get(void *out, size_t cap);

typedef struct { uint64_t width, height; } cos_image_info_t;
int cos_image_decode(const char *path, void *rgb_pixels, size_t capacity, cos_image_info_t *out);

/* ---- setjmp / longjmp ----------------------------------------------------
 * Pure userland - no syscall involved. Saves the callee-saved registers,
 * RSP and the return address, which is the complete set the System V
 * x86-64 ABI requires a function to preserve; caller-saved registers are
 * deliberately NOT saved, exactly as in a real setjmp, because the ABI
 * already permits any call to clobber them. */
typedef unsigned long cos_jmp_buf[8];
int  cos_setjmp(cos_jmp_buf env) __attribute__((returns_twice));
void cos_longjmp(cos_jmp_buf env, int val) __attribute__((noreturn));

/* ---- networking ----------------------------------------------------------
 * Exposed at the HTTP/DNS level, which is what the kernel's network stack
 * actually provides - NOT BSD sockets. A socket API would be a new
 * abstraction layered on top with nothing proving it works; this is the
 * surface that genuinely exists and is already exercised by the bundled
 * browser. */
bool     cos_net_available(void);
/* Writes 4 bytes of IPv4 into ip_out. Returns 0 on success, -1 otherwise. */
int      cos_net_resolve(const char *hostname, uint8_t *ip_out);
/* Returns the number of body bytes received, or -1. */
ssize_t  cos_net_http_get(const char *url, void *buf, size_t len);   /* text only: the body ends at the first NUL byte */

/* Asynchronous, binary-safe download. cos_net_http_get() above blocks every ring-3 window for as long as the transfer
 * takes; this starts the transfer on a kernel thread instead, so the caller keeps running and polls.
 *   job = cos_net_http_start(url);                  (-1: no free job slot, at most 4 at a time)
 *   while (cos_net_http_poll(job, &res) == 0) ...   (0 running, 1 finished, -1 unknown job)
 *   n = cos_net_http_take(job, buf, cap);           (body bytes, byte-exact; frees the job)
 * res.status is the HTTP status (0 when no response arrived), res.error is non-zero when the transfer itself failed, and
 * res.body_len is the full body length (so a caller can see when its buffer was too small). Redirects and HTTPS work;
 * the kernel caps a body at 10 MiB. cos_net_http_cancel() abandons a job. Layout must match src/include/cos_sysinfo.h. */
typedef struct {
    int32_t  status, error;
    uint64_t body_len;
    uint32_t truncated, pad;
    char     content_type[64];
} cos_http_result_t;
int      cos_net_http_start(const char *url);
int      cos_net_http_poll(int job, cos_http_result_t *res);
ssize_t  cos_net_http_take(int job, void *buf, size_t cap);
void     cos_net_http_cancel(int job);

/* ---- sockets ---------------------------------------------------------------------------------------------------------
 * Berkeley-style TCP and UDP. Every call returns immediately; the negative values are -errno (COS_E* below). A program
 * waits with cos_sock_poll() / cos_sock_wait(), which sleep in user space, so no other window is ever held up by a slow
 * peer. Addresses are host-order integers: COS_IP(192,168,70,2).
 *
 *   server:  sd = cos_tcp_listen(8080, 4);  c = cos_tcp_accept_wait(sd, &peer, 5000);  cos_send_all(c, ..);  cos_sock_close(c);
 *   client:  sd = cos_tcp_connect(ip, 80, 5000);  cos_send_all(..);  n = cos_recv_wait(sd, buf, len, 5000);
 *   UDP:     sd = cos_socket(COS_SOCK_DGRAM);  cos_bind(sd, 0, 5353);  cos_recvfrom(...);  cos_sendto(...);
 * Layout of the structs matches src/include/cos_socket.h. */
enum { COS_SOCK_STREAM = 1, COS_SOCK_DGRAM = 2 };
enum { COS_EPERM = 1, COS_EBADF = 9, COS_EAGAIN = 11, COS_ENOMEM = 12, COS_EINVAL = 22, COS_EMFILE = 24, COS_EPIPE = 32,
       COS_EDESTADDRREQ = 89, COS_EMSGSIZE = 90, COS_EOPNOTSUPP = 95, COS_EADDRINUSE = 98, COS_ENETDOWN = 100,
       COS_ENETUNREACH = 101, COS_ECONNRESET = 104, COS_EISCONN = 106, COS_ENOTCONN = 107, COS_ETIMEDOUT = 110,
       COS_ECONNREFUSED = 111, COS_EINPROGRESS = 115 };
enum { COS_POLLIN = 1, COS_POLLOUT = 4, COS_POLLERR = 8, COS_POLLHUP = 16, COS_POLLNVAL = 32 };
#define COS_IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
typedef struct { uint32_t ip; uint16_t port; uint16_t family; } cos_sockaddr_t;
typedef struct { int32_t sd; int16_t events, revents; } cos_pollfd_t;
typedef struct { int32_t id, state; uint8_t ip[4]; } cos_dns_result_t;   /* state: 0 pending, 1 resolved, -1 failed */
typedef struct {
    uint8_t ip[4], mask[4], gateway[4], dns[4], mac[6];
    uint8_t link_up, has_lease;
    uint32_t tcp_retransmits, tcp_fast_retransmits, tcp_timeouts, tcp_resets_sent, tcp_resets_rcvd, tcp_out_of_order;
    uint32_t sockets_open, pad;
} cos_netinfo_t;

/* raw non-blocking calls */
int      cos_socket(int type);
int      cos_bind(int sd, uint32_t ip, uint16_t port);                 /* ip is ignored: there is one interface */
int      cos_listen(int sd, int backlog);                              /* backlog 1..8 */
int      cos_accept(int sd, cos_sockaddr_t *peer);                     /* new sd, or -COS_EAGAIN */
int      cos_connect(int sd, uint32_t ip, uint16_t port);              /* -COS_EINPROGRESS: poll for COS_POLLOUT, call again */
ssize_t  cos_send(int sd, const void *buf, size_t len);                /* -COS_EAGAIN when the send queue is full */
ssize_t  cos_recv(int sd, void *buf, size_t len);                      /* 0 = end of stream, -COS_EAGAIN = nothing yet */
ssize_t  cos_sendto(int sd, const void *buf, size_t len, uint32_t ip, uint16_t port);
ssize_t  cos_recvfrom(int sd, void *buf, size_t len, cos_sockaddr_t *from);
int      cos_sock_poll(cos_pollfd_t *fds, int n);                      /* up to 32 sockets, returns how many are ready, never waits */
int      cos_sock_close(int sd);
int      cos_getsockname(int sd, cos_sockaddr_t *out);
int      cos_getpeername(int sd, cos_sockaddr_t *out);
int      cos_dns_start(const char *host, cos_dns_result_t *out);       /* literals and cached names finish immediately */
int      cos_dns_poll(cos_dns_result_t *io);
int      cos_net_info(cos_netinfo_t *out);
                             /* address, gateway, DNS, MAC, TCP counters */

/* convenience: these wait (sleeping in user space) up to timeout_ms; a negative result is -errno, -COS_ETIMEDOUT on timeout */
int      cos_sock_wait(int sd, int events, int timeout_ms);            /* returns the ready events */
int      cos_tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);  /* connected sd */
int      cos_tcp_listen(uint16_t port, int backlog);                   /* listening sd */
int      cos_tcp_accept_wait(int sd, cos_sockaddr_t *peer, int timeout_ms);
ssize_t  cos_send_all(int sd, const void *buf, size_t len, int timeout_ms);
ssize_t  cos_recv_wait(int sd, void *buf, size_t len, int timeout_ms);
int      cos_resolve_wait(const char *host, uint32_t *ip, int timeout_ms);   /* name or dotted literal -> address */
int      cos_ip_parse(const char *dotted, uint32_t *ip);               /* 0 on success */

/* ---- FILE* stdio ----------------------------------------------------------
 * Buffered stream I/O over the fd syscalls. This is the single biggest
 * thing standing between "runs programs written for C-OS" and "a small
 * external C program can be ported with modest changes" - essentially
 * every real C program uses fopen/fgets/fprintf rather than raw
 * descriptors.
 *
 * Streams are buffered, so fflush()/fclose() matter: unflushed output is
 * lost if a program exits without closing, exactly as with a real stdio.
 */
typedef struct cos_FILE cos_FILE;

#define COS_EOF (-1)

cos_FILE *cos_fopen(const char *path, const char *mode);  /* "r", "w", "a", "r+" */
int       cos_fclose(cos_FILE *f);
size_t    cos_fread(void *ptr, size_t size, size_t count, cos_FILE *f);
size_t    cos_fwrite(const void *ptr, size_t size, size_t count, cos_FILE *f);
int       cos_fgetc(cos_FILE *f);
int       cos_fputc(int c, cos_FILE *f);
char     *cos_fgets(char *buf, int size, cos_FILE *f);
int       cos_fputs(const char *s, cos_FILE *f);
int       cos_fprintf(cos_FILE *f, const char *fmt, ...);
int       cos_fflush(cos_FILE *f);
long      cos_fhigh(cos_FILE *f);              /* the end of the furthest write through the stream (SEEK_END while the file is still open) */
int       cos_fseek(cos_FILE *f, long offset);
long      cos_ftell(cos_FILE *f);
int       cos_feof(cos_FILE *f);
int       cos_ungetc(int c, cos_FILE *f);

/* ---- stdlib staples -------------------------------------------------------
 * The handful of functions almost any ported C program reaches for. */
long      cos_strtol(const char *s, char **end, int base);
unsigned long cos_strtoul(const char *s, char **end, int base);
void      cos_qsort(void *base, size_t n, size_t size,
                    int (*cmp)(const void *, const void *));
char     *cos_strdup(const char *s);
char     *cos_strstr(const char *hay, const char *needle);
char     *cos_strrchr(const char *s, int c);
int       cos_abs(int v);
/* Deterministic PRNG - cos_srand() with cos_time_ms() for a varying seed.
 * Not cryptographically secure, and not presented as such. */
void      cos_srand(unsigned seed);
int       cos_rand(void);
#define COS_RAND_MAX 0x7fffffff

/* ---- strings ----------------------------------------------------------- */
size_t   cos_strlen(const char *s);
int      cos_strcmp(const char *a, const char *b);
int      cos_strncmp(const char *a, const char *b, size_t n);
char    *cos_strcpy(char *dst, const char *src);
char    *cos_strncpy(char *dst, const char *src, size_t n);
char    *cos_strcat(char *dst, const char *src);
char    *cos_strchr(const char *s, int c);
void    *cos_memset(void *dst, int c, size_t n);
void    *cos_memcpy(void *dst, const void *src, size_t n);
int      cos_memcmp(const void *a, const void *b, size_t n);
int      cos_atoi(const char *s);


/* ---- runtime, link map and thread-local storage --------------------------
 *
 * Provided by cos_rtld.c, which is the ring3 half of the loader: it walks
 * the auxiliary vector the kernel built, applies the ifunc relocations
 * the kernel deliberately did not (they call ring3 resolver functions),
 * runs constructors, and calls main().
 *
 * main() now takes three arguments - main(argc, argv, envp) - because the
 * initial stack finally carries an environment. A main() declared with
 * two still works: the third argument is simply ignored, exactly as on
 * any other System V system. */

extern int    cos_argc;
extern char **cos_argv;
extern char **cos_environ;

/* Auxiliary vector lookup. Useful tags: 3 (AT_PHDR), 5 (AT_PHNUM),
 * 6 (AT_PAGESZ), 9 (AT_ENTRY), 25 (AT_RANDOM), 31 (AT_EXECFN). Returns 0
 * for a tag the kernel did not supply. */
uint64_t cos_getauxval(uint64_t tag);
char    *cos_getenv(const char *name);

/* Destructors. Registered automatically for every loaded object's
 * DT_FINI/DT_FINI_ARRAY; a program may add its own. Run in reverse
 * registration order when main() returns. */
int  cos_atexit(void (*fn)(void));
void cos_run_atexit(void);

/* Builds a fresh thread-local storage block for the CALLING thread's use
 * and returns its thread pointer, or 0 if this process has no TLS.
 * cos_thread_create() does this for you; call it directly only if you
 * are starting a thread some other way. */
uint64_t cos_tls_create_block(void);

/* Installs `tp` as this thread's %fs base. Only ring0 can write that
 * MSR, so this is a syscall. Passing 0 clears it. */
int      cos_set_tls(uint64_t tp);
uint64_t cos_get_tls(void);

/* ---- dynamic loading, extended ------------------------------------------
 * cos_dlopen() above is kept as-is. These add what it could not express. */
#define COS_RTLD_LAZY     0x0001   /* accepted and ignored: binding is eager */
#define COS_RTLD_NOW      0x0002
#define COS_RTLD_GLOBAL   0x0100   /* join the process-wide symbol scope     */
#define COS_RTLD_LOCAL    0x0000
#define COS_RTLD_DEFAULT  ((int64_t)0)   /* dlsym across the whole scope     */

#define COS_RTLD_NOLOAD   0x0004   /* only if already loaded: returns a handle, loads nothing */
#define COS_RTLD_DEEPBIND 0x0008   /* the library's own definitions win over the global scope when it is relocated */
#define COS_RTLD_NODELETE 0x1000   /* cos_dlclose never unloads it */
#define COS_RTLD_NEXT     ((int64_t)-1)   /* cos_dlsym: the next definition after the calling object (to wrap a function) */

/* Opens a library: maps and relocates it (in the kernel), then runs its DT_INIT / init_array here, dependencies first - the library is usable on return, exactly like
 * after startup. Returns a handle (> 0) or -1; cos_dlerror() says why. Opening a library again returns a handle to the same copy. */
int64_t  cos_dlopen_flags(const char *path, uint32_t flags);
/* Closes a handle. When it was the last one and no other loaded object uses the library (it is nobody's dependency, nothing bound a symbol from it), its
 * destructors run (dependents first) and it is unloaded: its pages are unmapped and its frames freed, together with the dependencies only it needed. Function
 * pointers into an unloaded library dangle, as with any dlclose. Returns 0 or -1. */
int      cos_dlclose(int64_t handle);
/* The message of the last failed dl call in this process (cleared by reading it), or NULL. */
const char *cos_dlerror(void);
/* dladdr: which loaded object covers `addr`, where it was loaded, and the symbol that covers the address (sym is empty when there is none). */
typedef struct { char path[96]; uint64_t base; char sym[64]; uint64_t sym_addr; } cos_dlinfo_t;
int      cos_dladdr(const void *addr, cos_dlinfo_t *out);
/* dlsym(RTLD_NEXT, name): `caller` is any address inside the calling library (cos_dlsym passes its own return address for COS_RTLD_NEXT). */
void    *cos_dlsym_next(const void *caller, const char *name);
/* Walks the loaded objects (dl_iterate_phdr); the callback returns non-zero to stop. */
typedef struct { const char *name; uint64_t base, map_start, map_end; uint32_t serial, flags; } cos_dlobj_t;
int      cos_dl_iterate(int (*cb)(const cos_dlobj_t *obj, void *ctx), void *ctx);
/* Extra directories (colon separated) searched for libraries before /lib:/usr/lib:/ ; the environment variable COS_LIBRARY_PATH sets the same at startup. */
int      cos_dl_set_path(const char *dirs);
/* Raw access to the kernel's dl interface (what the functions above are built on). */
int64_t  cos_dl_open_raw(const char *path, uint32_t flags);
int      cos_dl_close_raw(int64_t handle);
int64_t  cos_dl_ctl(uint64_t op, uint64_t a, uint64_t b, uint64_t c);
/* Address of the read-only link map the kernel published, or 0. Same
 * value the auxiliary vector carries; this is for code that never saw
 * the initial stack. */
uint64_t cos_dl_info(void);


/* ---- math (cos_math.c) ---------------------------------------------------
 * There was no math library at all before this - not a reduced one, none
 * - so any program doing geometry, statistics, signal processing or
 * plotting simply could not be ported.
 *
 * Accuracy is a few ULP across the normal range, established by fuzzing
 * every function against the host libm in validation/userland rather
 * than asserted. No errno and no FP exception flags: this runtime has
 * neither, and a function that pretended to set errno would be lying.
 * Domain errors return NaN. */
int    cos_isnan(double x);
int    cos_isinf(double x);
int    cos_isfinite(double x);
int    cos_signbit(double x);
double cos_nan(void);
double cos_inf(void);

double cos_fabs(double x);
double cos_copysign(double x, double y);
double cos_fmin(double a, double b);
double cos_fmax(double a, double b);

double cos_floor(double x);
double cos_ceil(double x);
double cos_trunc(double x);
/* Ties away from zero, per round(). NOT the ties-to-even the printf
 * formatter uses - the two genuinely differ, and conflating them is a
 * classic source of off-by-one output. */
double cos_round(double x);
double cos_fmod(double x, double y);
double cos_modf(double x, double *ipart);
double cos_frexp(double x, int *exp_out);
double cos_ldexp(double x, int n);

double cos_sqrt(double x);
double cos_cbrt(double x);
double cos_expm1(double x); double cos_log1p(double x); double cos_erf(double x); double cos_erfc(double x); double cos_lgamma(double x); double cos_tgamma(double x);
double cos_hypot(double a, double b);
double cos_exp(double x);
double cos_exp2(double x);
double cos_log(double x);
double cos_log2(double x);
double cos_log10(double x);
double cos_pow(double x, double y);

double cos_sin(double x);
double cos_cos(double x);
double cos_tan(double x);
double cos_asin(double x);
double cos_acos(double x);
double cos_atan(double x);
double cos_atan2(double y, double x);
double cos_sinh(double x);
double cos_cosh(double x);
double cos_tanh(double x);


/* ---- thread synchronisation (cos_sync.c) ---------------------------------
 *
 * cos_thread_create() existed with nothing to synchronise those threads
 * with - no mutex, no condition variable, no semaphore, not even a
 * documented atomic. A program with two threads touching the same data
 * had no correct way to be written, and the obvious workaround (a spin
 * loop on a volatile flag) is both wrong - no memory ordering - and
 * actively harmful on a single-core schedule, where the spinner holds
 * the CPU that the thread it waits for needs to make progress.
 *
 * Every primitive is futex-backed: the state is a word in your own
 * memory manipulated with atomic instructions, and the kernel is entered
 * only when a thread genuinely has to sleep. An uncontended lock is one
 * atomic instruction and no syscall.
 *
 * ALL WAITS CAN RETURN SPURIOUSLY. Re-check your condition in a loop.
 * That is not defensive padding - a futex hash bucket is shared, so an
 * unrelated wake can reach your waiter, and condition variables are
 * specified to allow it regardless. */

typedef struct { uint32_t state; uint64_t owner; } cos_mutex_t;
typedef struct { uint32_t seq; uint32_t waiters; } cos_cond_t;
typedef struct { uint32_t count; } cos_sem_t;
typedef struct { uint32_t state; } cos_rwlock_t;
typedef struct { uint32_t state; } cos_once_t;

#define COS_MUTEX_INIT   { 0, 0 }
#define COS_COND_INIT    { 0, 0 }
#define COS_RWLOCK_INIT  { 0 }
#define COS_ONCE_INIT    { 0 }

void cos_mutex_init(cos_mutex_t *m);
int  cos_mutex_lock(cos_mutex_t *m);
int  cos_mutex_trylock(cos_mutex_t *m);           /* 0 on success, -1 if held */
int  cos_mutex_timedlock(cos_mutex_t *m, uint64_t timeout_ms);
int  cos_mutex_unlock(cos_mutex_t *m);

void cos_cond_init(cos_cond_t *c);
/* Must be called with `m` held. Releases it while waiting and reacquires
 * it before returning - which is why it takes the mutex at all: reading
 * the wait state and dropping the lock have to happen together, or a
 * signal in the window is lost. */
int  cos_cond_wait(cos_cond_t *c, cos_mutex_t *m);
int  cos_cond_timedwait(cos_cond_t *c, cos_mutex_t *m, uint64_t timeout_ms);
int  cos_cond_signal(cos_cond_t *c);
int  cos_cond_broadcast(cos_cond_t *c);

void cos_sem_init(cos_sem_t *s, uint32_t value);
int  cos_sem_wait(cos_sem_t *s);
int  cos_sem_trywait(cos_sem_t *s);
int  cos_sem_timedwait(cos_sem_t *s, uint64_t timeout_ms);
int  cos_sem_post(cos_sem_t *s);

/* Writers are not starved: a waiting writer blocks new readers from
 * acquiring, so a steady stream of readers cannot hold the lock forever. */
void cos_rwlock_init(cos_rwlock_t *l);
int  cos_rwlock_rdlock(cos_rwlock_t *l);
int  cos_rwlock_tryrdlock(cos_rwlock_t *l);
int  cos_rwlock_wrlock(cos_rwlock_t *l);
int  cos_rwlock_trywrlock(cos_rwlock_t *l);
int  cos_rwlock_unlock(cos_rwlock_t *l);

/* Runs `fn` exactly once. A thread arriving while another is still
 * inside `fn` WAITS for it to finish rather than proceeding with
 * half-built state. */
int  cos_once(cos_once_t *o, void (*fn)(void));

/* The raw primitive, exposed because a program may want to build
 * something these do not cover (a barrier, a channel, a work queue).
 *
 * cos_futex_wait() sleeps only if *addr still equals `expected`; that
 * comparison is what closes the lost-wakeup race, and it is the reason
 * you must pass the value you based your decision on rather than
 * re-reading it inside. Returns 0 on wake, -11 if the word had already
 * changed, -110 on timeout. timeout_ms of 0 waits forever. */
int64_t cos_futex_wait(volatile uint32_t *addr, uint32_t expected,
                       uint64_t timeout_ms);
int64_t cos_futex_wake(volatile uint32_t *addr, uint32_t count);

/* This thread's id. Distinct from cos_getpid(), which names the
 * process. */
int64_t cos_gettid(void);
int     cos_cpu_load(uint8_t *pct, int max);      /* percent busy of each online CPU over the last ~0.5 s; returns how many were written */
int     cos_getcpu(void);                         /* the logical CPU this thread is running on right now */



/* ---- compute jobs on other CPU cores (SYS_PAR_*) --------------------------
 * The scheduler runs user threads on one core only. These calls run a
 * function on an otherwise idle core instead, under this process's own
 * address space. The function must be pure computation: no system calls
 * (including anything that logs or grows the heap), no blocking, and no
 * first touch of memory - touch every page it will use beforehand, because
 * the other core cannot service a page fault. Break any of those rules
 * and the job stops with COS_PAR_FAULTED and the details in `fault`.
 * Poll or keep a shared flag in memory to see what it is doing; the usual
 * shape is a persistent worker that spins on a queue until told to quit.
 *
 * `stack` is memory you provide (touched, at least 64KiB advised). */
#define COS_PAR_FREE     0
#define COS_PAR_QUEUED   1
#define COS_PAR_RUNNING  2
#define COS_PAR_DONE     3
#define COS_PAR_FAULTED  4
#define COS_PAR_KILLED   5
typedef struct { uint64_t vec, err, rip, cr2; } cos_par_fault_t;
int  cos_par_cpus(void);                                  /* cores available for jobs */
/* Returns a job id or a negative errno (-16: no idle core, -12: no slot). */
int  cos_par_start(void (*fn)(void *), void *arg, void *stack, size_t stack_size);
int  cos_par_poll(int id, cos_par_fault_t *fault);        /* COS_PAR_* state or negative */
int  cos_par_stop(int id);                                /* ask the core to abandon the job */
int  cos_par_free(int id);                                /* release the job id once it ended */

/* ---- idle-work queue ------------------------------------------------------------------------------------
 * cos_job_push() queues a small job; cos_idle_work() runs one queued job on the calling core and says whether it
 * did. Worker cores (see below) call cos_idle_work() from every spin-wait, so queued jobs run on cores that would
 * otherwise be idle; any thread may also call it to help. Jobs must be pure computation and finish quickly. */
bool cos_job_push(void (*fn)(void *), void *arg);
bool cos_idle_work(void);

/* ---- worker arenas -------------------------------------------------------
 * A compute job on another core cannot make system calls, so it cannot grow
 * the heap. An arena is a pre-touched block holding that job's stack and a
 * private heap; malloc/free called from code running on the arena's stack use
 * it automatically (any thread may free such a block). Create it on the main
 * thread, start the job on the stack from cos_arena_stack(), destroy it after
 * the job has ended. */
typedef struct cos_arena cos_arena_t;
cos_arena_t *cos_arena_create(size_t heap_bytes, size_t stack_bytes);
void  cos_arena_stack(cos_arena_t *a, void **base, size_t *size);
void  cos_arena_destroy(cos_arena_t *a);
bool  cos_in_worker(void);   /* true when called from code running on an arena's stack */

/* ---- audio output (SYS_AUDIO_*) -------------------------------------
 * One process owns the audio device at a time. Open it with a sample rate
 * (8000..48000) and 1 or 2 channels, then write interleaved signed 16-bit
 * PCM. Writes never block: cos_audio_write() returns how many samples it
 * accepted (possibly 0 if the queue is full - try again after a short
 * sleep; cos_audio_status() reports free space). The device is released
 * by cos_audio_close() or automatically when the process exits. */
typedef struct {
    uint32_t available;        /* an audio device exists */
    uint32_t owned_by_caller;
    uint32_t busy;             /* another process owns it */
    uint32_t rate;
    uint32_t channels;
    uint32_t reserved;
    uint64_t free_samples;
    uint64_t pending_samples;  /* written but not yet heard */
    uint64_t underruns;
} cos_audio_status_t;
int     cos_audio_open(uint32_t rate, uint32_t channels);   /* 0, or -16 busy / -22 bad args / -2 no device */
int64_t cos_audio_write(const int16_t *pcm, uint64_t samples);
int     cos_audio_status(cos_audio_status_t *out);
int     cos_audio_drain(void);      /* play out what is queued, including a partial tail */
int     cos_audio_stop(void);       /* silence now, drop what is queued */
int     cos_audio_set_volume(uint32_t volume_0_100);
int     cos_audio_close(void);


/* ---- window API v2 (SYS_WIN2_*) ---------------------------------------
 * A window is a pixel buffer shared with the compositor: draw into
 * info.pixels (0x00RRGGBB, `stride` pixels per row) however you like, then
 * call cos_win2_present() to show it. Input arrives as cos_win_event_t.
 * Closing the window with its title-bar button sends COS_EV_CLOSE; the app
 * decides when to call cos_win2_close() (a second click forces it). */
#define COS_EV_NONE        0
#define COS_EV_KEY         1
#define COS_EV_MOUSE_MOVE  2
#define COS_EV_MOUSE_DOWN  3
#define COS_EV_MOUSE_UP    4
#define COS_EV_WHEEL       5
#define COS_EV_CLOSE       6
#define COS_EV_OPEN        7   /* the OS asks you to open a file (cos_win2_get_path) */
#define COS_EV_SHELL_SHOW  9    /* shell layer 2 (the start menu) was opened */
#define COS_EV_SHELL_HIDE  10   /* ... and was closed by the compositor */
#define COS_EV_DROP        8   /* a file was dropped on the window at x,y (cos_win2_get_path) */
typedef struct {
    uint32_t type;
    int32_t  x, y;            /* client coordinates (mouse events) */
    uint8_t  button, buttons, mods, special;   /* button: the one that changed; buttons: held mask */
    int32_t  wheel;           /* +up / -down */
    char     ascii;
    uint8_t  pad[3];
} cos_win_event_t;
typedef struct {
    int64_t  handle;
    uint32_t *pixels;
    int32_t  width, height, stride;
    int32_t  reserved;
} cos_win_info_t;
int64_t cos_win2_create(const char *title, int32_t width, int32_t height, cos_win_info_t *out);
int     cos_win2_present(int64_t handle);
/* Presents only the given client-area rectangles (at most 64): the kernel copies just those rows and the screen
 * update that follows is itself limited to them. Use it instead of cos_win2_present() whenever you know what changed. */
typedef struct { int32_t x, y, w, h; } cos_win_rect_t;

/* Detected PS/2 + USB devices (SYS_DEVICES_LIST). Layout must match src/include/cos_devices.h. */
enum { COS_DEV_NONE = 0, COS_DEV_PS2_CONTROLLER = 1, COS_DEV_PS2_KEYBOARD = 2, COS_DEV_PS2_MOUSE = 3,
       COS_DEV_USB_CONTROLLER = 4, COS_DEV_USB_DEVICE = 5 };
enum { COS_DEVROLE_KEYBOARD = 1, COS_DEVROLE_MOUSE = 2, COS_DEVROLE_POINTER_ABS = 4, COS_DEVROLE_STORAGE = 8,
       COS_DEVROLE_HUB = 16, COS_DEVROLE_WHEEL = 32 };
typedef struct {
    uint8_t kind, present, usb_speed, usb_ctrl;
    uint8_t usb_addr, usb_root_port, usb_hub_addr, usb_hub_port;
    uint8_t dev_class, dev_subclass, dev_protocol, roles;
    uint16_t vid, pid, bcd_usb, bcd_dev;
    uint32_t extra0, extra1;
    char name[64], vendor[48], serial[40], detail[96];
} cos_device_info_t;
int     cos_devices_list(cos_device_info_t *out, int max);

/* Desktop shell (COS_CAP_SHELL). Layout must match src/include/cos_shell.h. */
typedef struct { uint32_t uid, owner_pid; uint16_t kind; uint8_t minimized, maximized, focused, visible; uint16_t pad; char title[46]; } cos_winfo_t;
enum { COS_WINCTL_ACTIVATE = 1, COS_WINCTL_MINIMIZE, COS_WINCTL_TOGGLE, COS_WINCTL_CLOSE, COS_WINCTL_START_MENU, COS_WINCTL_LAUNCH,
       COS_WINCTL_APP, COS_WINCTL_SYSACT, COS_WINCTL_MENU_HIDE };
typedef struct { int32_t kind, x, y, size; } cos_shell_icon_t;
#define COS_SHELL_MAX_ICONS 96
enum { COS_APPK_FILE_MGR = 1, COS_APPK_TEXT_EDITOR = 2, COS_APPK_TERMINAL = 3, COS_APPK_SETTINGS = 4, COS_APPK_ABOUT = 5,
       COS_APPK_CALC = 6, COS_APPK_STORAGE = 7, COS_APPK_BROWSER = 8, COS_APPK_TASK_MGR = 9, COS_APPK_PAINT = 10,
       COS_APPK_MUSIC = 11, COS_APPK_CLOCK = 12, COS_APPK_SYSINFO = 13, COS_APPK_STUDIO = 14, COS_APPK_CALC_GRAPH = 15,
       COS_APPK_SHEET = 16, COS_APPK_VOXEL_GAME = 17, COS_APPK_JPEG = 18, COS_APPK_MEMORY_MGR = 19, COS_APPK_TCC_IDE = 20,
       COS_APPK_HTTP_DOWNLOADER = 21, COS_APPK_TINYGL = 22, COS_APPK_2DGAMES = 23, COS_APPK_GENERIC = 24, COS_APPK_MESSENGER = 26 /* = WIN_MESSENGER */ };
enum { COS_SA_SHOW_DESKTOP = 2, COS_SA_LOCK, COS_SA_RESTART, COS_SA_POWEROFF, COS_SA_LANG_EN, COS_SA_LANG_JA, COS_SA_DARK_MODE,
       COS_SA_ANIMATIONS, COS_SA_NOTIFICATIONS, COS_SA_MOUSE_RAW, COS_SA_ABOUT, COS_SA_REFRESH_DESKTOP, COS_SA_TEST_AUDIO,
       COS_SA_IMAGE_WALLPAPER, COS_SA_CALC_HISSAN };
#define COS_SHELL_MAX_ICONS_ 96
#define COS_DESKTOP_KEY 0x00FF00FFu
typedef struct { int32_t x, y, win_kind, is_file, is_dynamic, folder_id, box, cell_w, cell_h; char label[32]; } cos_dicon_t;
enum { COS_DESKCTL_OPEN = 1, COS_DESKCTL_DROP, COS_DESKCTL_MOVE, COS_DESKCTL_CONTEXT, COS_DESKCTL_FOCUS };
typedef struct { uint32_t id; int32_t type; char title[48]; char message[96]; } cos_notif_t;
#define COS_NOTIF_MAX 8
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
int      cos_ctx_list(cos_ctxsnap_t *out);                              /* the open context menu, if any (returns 1) */
/* Kernel updates (docs/app_update.md, stage 2). Fully privileged programs only; the kernel checks the signature itself. Layout must match src/kernel/cos_bootupd.h. */
typedef struct {
    int32_t  supported; char running_slot, active_slot, trial_slot, pad;
    uint64_t running_serial, trial_serial, bad_serial;
    char     why[96];
} cos_kupd_status_t;
int      cos_kernel_update_status(cos_kupd_status_t *out);
int      cos_kernel_update_install(const void *blob, size_t len);   /* 0 ok, negative: refused; cos_kernel_update_why() says why */
int      cos_kernel_update_why(char *buf96);
int      cos_notify(const char *title, const char *message, int type);  /* a desktop toast (type: 0 info, 1 warning, 2 error, 3 success); at most one per 0.4 s */
int      cos_notif_list(cos_notif_t *out, int max);                     /* the toasts on screen */
int      cos_desktop_list(cos_dicon_t *out, int max);                   /* up to 64 icons */
int      cos_desktop_ctl(int action, int a, int b, int c);
int      cos_shell_icons(int layer, const cos_shell_icon_t *icons, int n, uint32_t tag);   /* tag: any number; the compositor reads it back from the frame that went live */   /* drawn by the compositor over the layer, live at the next present */
enum { COS_LAUNCH_BROWSER = 1, COS_LAUNCH_CALC = 2 };
#define COS_SHELL_MAX_WINDOWS 64
int64_t  cos_shell_create(int layer, cos_win_info_t *out);              /* layer 1 = the taskbar strip at the bottom of the screen; one at a time */
int      cos_win_list(cos_winfo_t *out, int max);                       /* the open windows, bottom to top */
int      cos_win_ctl(int action, uint32_t uid, int arg);

/* Sandbox (SYS_SANDBOX). A process starts with COS_CAP_ALL; cos_sandbox() keeps only the capabilities in 'keep_caps'
 * (it can never get one back) and, when 'jail' is given, confines every path argument to that absolute directory.
 * Children inherit the restrictions. Returns the capabilities still held, or -1 on a bad jail. Pass COS_CAP_ALL and a
 * NULL jail to just query. */
enum { COS_CAP_FS_READ = 0x1, COS_CAP_FS_WRITE = 0x2, COS_CAP_NET = 0x4, COS_CAP_SPAWN = 0x8, COS_CAP_WINDOW = 0x10,
       COS_CAP_AUDIO = 0x20, COS_CAP_SETTINGS = 0x40, COS_CAP_DEVICES = 0x80, COS_CAP_IPC = 0x100, COS_CAP_COMPUTE = 0x200,
       COS_CAP_SYSINFO = 0x400, COS_CAP_SHELL = 0x800, COS_CAP_POWER = 0x1000, COS_CAP_ACCOUNT = 0x2000 };

/* System statistics (SYS_SYSTEM_INFO) and the process list (SYS_PROCLIST). Layout must match src/include/cos_sysinfo.h. */
typedef struct {
    uint64_t uptime_ticks, ctx_switches, load_now, load_1m, load_5m, load_15m;
    uint64_t ram_total, kheap_used, kheap_free, pages_total, pages_free;
    uint64_t stor_total, stor_used, stor_free;
    uint64_t ustor_total, ustor_free;
    uint32_t ustor_devices, has_password, cpus_online, cpus_possible, cpus_bios, has_apic, has_acpi, reserved;
    char cpu_vendor[16], bios_vendor[16], bios_version[16], bios_date[16];
    char os_name[48], os_version[32], boot_msg[96];
    uint32_t pcache_frames, pcache_maps;      /* shared read-only pages (cos_pcache.c): frames held, page-table entries that map them */
} cos_sysinfo_t;
enum { COS_PROC_STATE_CREATED = 1, COS_PROC_STATE_READY, COS_PROC_STATE_RUNNING, COS_PROC_STATE_BLOCKED,
       COS_PROC_STATE_SLEEPING, COS_PROC_STATE_ZOMBIE };
typedef struct {
    uint64_t pid, parent_pid, cpu_time, ctx_switches, page_faults, heap_bytes;
    uint32_t state, is_user, threads, caps;
    char name[32];
} cos_procinfo_t;
int     cos_sysinfo(cos_sysinfo_t *out);
int     cos_proclist(cos_procinfo_t *out, int max);   /* max <= 128 */
#define COS_CAP_ALL 0xFFFFFFFFu
int64_t cos_sandbox(uint32_t keep_caps, const char *jail);
int     cos_win2_present_rects(int64_t handle, const cos_win_rect_t *rects, uint32_t count);
/* Submit one decoded RGBA frame for GPU upload/scaling by the compositor. */
int     cos_win2_video_frame(int64_t handle, const uint32_t *pixels, int32_t width, int32_t height, int32_t stride, int32_t x, int32_t y, int32_t dst_w, int32_t dst_h);
/* Submit one decoded planar I420 (YUV 4:2:0) frame directly, skipping the
 * caller's own YUV->RGBA conversion: the kernel copies the three planes
 * and converts them to RGBA itself, parallelised across any idle AP
 * cores (see src/drivers/video/vga.c's vga_convert_yuv420_to_rgba_parallel()
 * and src/kernel/cos_app_window.c's cos_app_window_video_frame_yuv420()).
 * Prefer this over converting to RGBA yourself and calling
 * cos_win2_video_frame() - it is both less CPU work in your own process
 * and able to use cores your process cannot otherwise reach.
 * stride_c is the row stride of BOTH the U and V planes (standard for
 * I420 4:2:0 output from edge264/libvpx/dav1d). This crosses the syscall
 * boundary as a flat struct copy, the same way cos_stat_ex_t does.
 * bit_depth is 8 or 10: for 10-bit (AV1/VP9 Profile 2 HDR sources), each
 * sample is a little-endian uint16_t holding the value in its low 10
 * bits (dav1d's and libvpx's native high-bit-depth layout) and stride_y/
 * stride_c are still byte strides, so every existing 8-bit caller is
 * unaffected - only the stride numbers and sample width change. Always
 * set this field explicitly; it is not zero-initialised for you. */
typedef struct {
    const uint8_t *y, *u, *v;
    int32_t w, h;
    int32_t stride_y, stride_c;
    int32_t x, y_pos;
    int32_t dst_w, dst_h;
    int32_t bit_depth;
} cos_video_yuv420_t;
int     cos_win2_video_frame_yuv420(int64_t handle, const cos_video_yuv420_t *frame);

/* Zero-copy video. cos_win2_video_map() gives you two RGBA buffers (tightly packed rows of `width` pixels,
 * 0xAARRGGBB) for pictures up to width x height; the second is *buf1. Write the next picture into the one that
 * is not showing - from any thread or worker core, nothing is copied or converted by the kernel - and
 * cos_win2_video_commit() puts it on screen at (x, y) scaled to dst_w x dst_h. Returns NULL / -1 on failure. */
uint32_t *cos_win2_video_map(int64_t handle, int32_t width, int32_t height, uint32_t **buf1);
int       cos_win2_video_commit(int64_t handle, int idx, int32_t width, int32_t height,
                                int32_t x, int32_t y, int32_t dst_w, int32_t dst_h);
int     cos_win2_poll(int64_t handle, cos_win_event_t *ev);                 /* 1 event, 0 none */
int     cos_win2_wait(int64_t handle, cos_win_event_t *ev, uint32_t timeout_ms);
int     cos_win2_set_title(int64_t handle, const char *title);
int     cos_win2_close(int64_t handle);
int     cos_win2_screen_size(int32_t *w, int32_t *h);
/* Raw input for games. cos_win2_grab(h, 1) locks the pointer to the window: while the window has the keyboard focus the
 * cursor is hidden and parked in the middle, and cos_win2_input() reports the motion since its previous call (dx right,
 * dy down, in device counts) - no screen edges, no other windows in the way. Clicks and wheel still arrive as ordinary
 * events. cos_win2_grab(h, 0) gives the cursor back (do it for menus, on Esc, and before the program ends - it also
 * ends with the window). A window that loses the focus (Alt+Tab) is unlocked automatically while it is away.
 * cos_win2_input() also fills `keys`, a bitmap of the keys that are DOWN right now (events only carry presses): bit
 * (make code, +128 for the extended keys) - use COS_SC_* below, e.g. COS_KEY_DOWN_P(in, COS_SC_W). All zero when the
 * window is not focused. */
typedef struct { uint8_t keys[32]; int32_t dx, dy; uint8_t focused, grabbed, fullscreen, pad; } cos_win_input_t;
int     cos_win2_grab(int64_t handle, int on);
/* Real full screen: the window loses its title bar and the taskbar/desktop are not drawn; the surface is scaled to fit the screen
 * (aspect kept) and mouse coordinates are mapped back. mode 0 off, 1 on, 2 toggle; returns 1 if fullscreen now, 0 if not, -1 on error.
 * Ctrl+Alt+F12 always leaves it (in case the program stops answering). */
int     cos_win2_fullscreen(int64_t handle, int mode);
/* Replaces the window's pixel surface by one of w x h pixels (up to 16 MiB), mapped at the same address; `info` is filled in as by cos_win2_create (the old pointer is
 * invalid afterwards, the contents are cleared). A full-screen game uses it to draw at the screen's own resolution. 0 on success. */
int     cos_win2_resize(int64_t handle, int32_t w, int32_t h, cos_win_info_t *info);
int     cos_win2_input(int64_t handle, cos_win_input_t *out);
#define COS_KEY_DOWN_P(in, sc) (((in).keys[(sc) >> 3] >> ((sc) & 7)) & 1)
enum { COS_SC_ESC = 0x01, COS_SC_1 = 0x02, COS_SC_2, COS_SC_3, COS_SC_4, COS_SC_5, COS_SC_6, COS_SC_7, COS_SC_8, COS_SC_9, COS_SC_0,
       COS_SC_BACKSPACE = 0x0E, COS_SC_TAB = 0x0F, COS_SC_Q = 0x10, COS_SC_W, COS_SC_E, COS_SC_R, COS_SC_T, COS_SC_Y, COS_SC_U, COS_SC_I, COS_SC_O, COS_SC_P,
       COS_SC_ENTER = 0x1C, COS_SC_LCTRL = 0x1D, COS_SC_A = 0x1E, COS_SC_S, COS_SC_D, COS_SC_F, COS_SC_G, COS_SC_H, COS_SC_J, COS_SC_K, COS_SC_L,
       COS_SC_LSHIFT = 0x2A, COS_SC_Z = 0x2C, COS_SC_X, COS_SC_C, COS_SC_V, COS_SC_B, COS_SC_N, COS_SC_M, COS_SC_RSHIFT = 0x36,
       COS_SC_LALT = 0x38, COS_SC_SPACE = 0x39,
       COS_SC_UP = 0x80 | 0x48, COS_SC_LEFT = 0x80 | 0x4B, COS_SC_RIGHT = 0x80 | 0x4D, COS_SC_DOWN = 0x80 | 0x50 };
int     cos_file_drop(const char *path, int copy);   /* drop a file where the pointer is: on the app window under it (it gets COS_EV_DROP + cos_win2_get_path) or on the desktop; own windows are skipped */
int     cos_drag_ghost(const char *label);          /* a label that follows the pointer while a file is dragged; NULL hides it */
int     cos_win2_get_path(int64_t handle, char *buf, size_t cap);   /* path for COS_EV_OPEN / COS_EV_DROP */

#endif /* COS_H */
