#include "cos_kevent.h"
#include "smp_user.h"
#include "cos_devices.h"
#include "cos_caps.h"
#include "cos_sysinfo.h"
#include "cos_shell.h"
#include "cos_socket.h"
#include "drivers/http.h"
extern int cos_httpjob_start(const char *url, uint32_t owner);
extern int cos_httpjob_poll(int id, uint32_t owner, cos_http_result_t *res);
extern int64_t cos_httpjob_take(int id, uint32_t owner, void *dst, uint64_t cap);
extern void cos_httpjob_cancel(int id, uint32_t owner);
#include "smp.h"
#include "percpu.h"
extern int unified_storage_get_device_count(void) __attribute__((weak));
extern uint64_t unified_storage_get_total_capacity(void) __attribute__((weak));
extern uint64_t unified_storage_get_free_capacity(void) __attribute__((weak));
#include "cos_audio.h"
#include "mm/memory.h"
#include "gui.h"
#include "acpi_power.h"
#include "net.h"
#include "dhcp.h"
#include "enhanced_file_manager.h"
#include "password_screen.h"
#include "settings_manager.h"
#include "vga.h"
#include "bios.h"
#define PHYS_TO_VIRT(phys) ((phys) + 0xFFFF800000000000ULL)
/* paging_free_pages / paging_virt_to_phys / paging_user_range_ok come
 * from mm/paging.h, included below.
 *
 * They were re-declared here by hand as `unsigned long` and `int` where
 * the real signatures are uint64_t and bool. That happens to be
 * call-compatible on x86-64 - which is why it went unnoticed - but it is
 * a genuine ODR violation, and a compiler is free to warn on or
 * mis-optimise the mismatch. Deleted in favour of the header now that it
 * declares them. */
/**
 * syscall.c - Real ring3 -> ring0 syscall entry point (int 0x80)
 *
 * This is the piece that was missing to make C-OS's existing ring3
 * scaffolding (GDT user segments, TSS.rsp0 switching on every context
 * switch, per-process page directories, PAGE_USER page mapping) into an
 * actually-usable userspace: without a syscall gate, a ring3 thread had
 * no way to ask the kernel for anything and no clean way to end itself.
 *
 * Convention (deliberately tiny - just enough for a real, verifiable
 * ring3 milestone, not a full ABI):
 *   rax = syscall number
 *   rdi, rsi = arg1, arg2
 *   SYS_WRITE (0): rdi = pointer (in the *caller's* mapped address space,
 *                  which is what's active when this handler runs, since
 *                  interrupts don't change CR3), rsi = length. Writes to
 *                  the serial console.
 *   SYS_EXIT  (1): rdi = exit code. Terminates the calling thread/process.
 */
#include "idt.h"
#include "serial.h"
#include "task.h"
#include "mm/paging.h"
void paging_release_user_range(uint64_t virtual_addr, uint64_t count);
#include "cos_elf.h"
#include "scheduler.h"
#include "cos_app_window.h"
#include "cos_fm_support.h"
#include "cos_elf_link.h"
#include "cos_futex.h"
#include "cos_fd.h"
#include "ipc.h"
#include "timer.h"
#include "rtc.h"
#include "api/net_api.h"
#include "memory.h"
#include "memory_physical.h"
#include "storage.h"
#include "net.h"
#include "cos_version.h"
#include "../bios/bios.h"
#include "../gui/core/gui.h"
#include "fs.h"
#include "string.h"

extern int cos_fs_read_file(const char* path, void* buffer, uint64_t size);
extern int cos_fs_write_file(const char* path, const void* data, uint64_t size);
extern int jpeg_viewer_decode_file_to_rgb(const char *filename, uint32_t *out, uint64_t cap, uint64_t *w, uint64_t *h);

#define SYS_WRITE      0
#define SYS_EXIT       1
/* GUI calls for ring3 .c-os programs. These do not draw: they queue
 * commands that the GUI owner thread replays (see cos_app_window.c). */
#define SYS_WIN_CREATE 2
#define SYS_WIN_FILL   3
#define SYS_WIN_CLEAR  4
/* Memory. Without these a .c-os program cannot allocate at all - no
 * malloc, no dynamic data structures - which is the single biggest thing
 * standing between "runs hand-written assembly" and "runs a real
 * program". */
#define SYS_BRK        5
#define SYS_SBRK       6
/* Whole-file I/O. Deliberately NOT a POSIX open/read/write/close file
 * descriptor set: that needs a per-process fd table, seek offsets, and
 * lifetime management across fork/exit, none of which exists yet, and a
 * half-built fd layer would be worse than an honest whole-file one.
 * These two cover what a small program actually needs (load a file, save
 * a file) and can be kept as the fast path if a real fd layer is added
 * later. */
#define SYS_READ_FILE  7
#define SYS_WRITE_FILE 8
/* Scheduling. A ring3 program had no way to wait: the only option was a
 * busy loop, which is preemptively scheduled and therefore burns a full
 * CPU share forever. That is not hypothetical - the first GUI demo
 * program in this tree ended in `jmp $` to stay resident and measurably
 * starved the rest of the system until it was changed to exit. Without
 * these, any program that wants to stay alive has that same problem. */
#define SYS_YIELD      9
#define SYS_SLEEP_MS   10
#define SYS_GETPID     11
/* Process creation. Together with the loader this is what lets a program
 * start another program - the practical equivalent of fork+exec here.
 * Implemented as spawn rather than fork because fork needs copy-on-write
 * address-space duplication, which does not exist, and a fork that
 * silently shared or eagerly copied everything would be a trap. */
#define SYS_SPAWN      12
/* Wait for a spawned child and collect its exit status. Genuinely blocks
 * the calling thread on a real wait queue (process_t::child_wait_queue),
 * woken by process_exit() the moment a child becomes a zombie - see the
 * implementation below for the full design note. An earlier version of
 * this polled process_try_reap_child() between 10ms sleeps; replaced once
 * the wait-queue primitives it originally said did not exist turned out
 * to already be present in scheduler.h (wait_queue_t/scheduler_sleep_on/
 * wait_queue_wake_all), just not yet wired up for this case. */
#define SYS_WAITPID    13
/* Shared libraries (.c-osll). Deliberately a kernel-mediated
 * load-and-resolve rather than a userspace dynamic linker: there is no
 * libc, no PLT/GOT setup in these freestanding programs, and no ld.so to
 * host one. The kernel maps the object into the calling process, applies
 * its relocations, and hands back the address of a named export - which
 * is what dlopen()/dlsym() give a caller, without needing the rest of a
 * dynamic linker to exist first. */
#define SYS_DLOPEN     14
#define SYS_DLSYM      15
/* Streaming file I/O: a persistent handle with a seek position, on top
 * of the earlier whole-file SYS_READ_FILE/SYS_WRITE_FILE. Needed for
 * anything that reads a file larger than it wants to buffer at once,
 * appends to a log, writes incrementally, or lists a directory. */
#define SYS_FD_OPEN     16
#define SYS_FD_OPENDIR  17
#define SYS_FD_READ     18
#define SYS_FD_WRITE    19
#define SYS_FD_LSEEK    20
#define SYS_FD_READDIR  21
#define SYS_FD_CLOSE    22
/* Signals.
 *
 * process_t already had pending_signals/blocked_signals/signal_handlers[]
 * fields (task.c), and signal_send()/signal_set_handler() correctly just
 * flip bits and store a pointer - those are reused as-is below. What did
 * NOT exist safely was delivery: task.c's signal_process_pending() (dead
 * code - zero callers anywhere) called `handler(sig)` as a DIRECT C
 * function call from kernel code. A registered handler is a RING3
 * function pointer; calling it that way does not cross any privilege
 * boundary - no ring transition, no stack switch, no CS change - so had
 * anything ever invoked that function, the handler would have executed
 * WITH RING0 PRIVILEGES on the kernel's own stack. That is a real
 * security-severity bug, currently inert only because nothing calls it.
 * It is not reused; a correct trampoline-based delivery replaces it,
 * implemented here where the syscall return path already has direct
 * access to the register frame that controls what CS/RIP/RSP the CPU
 * resumes with. */
#define SYS_SIGACTION  23
#define SYS_KILL       24
#define SYS_SIGRETURN  25
/* Anonymous private memory, independent of the heap. sbrk() can only grow
 * and shrink from a single moving edge; this is for a program that wants
 * an independently-addressed, independently-freeable block - the other
 * major gap versus a real malloc-capable environment now that heap/file/
 * process/signal syscalls all exist. File-backed and shared (MAP_SHARED)
 * mappings are NOT implemented - see the design note above
 * cos_mmap_alloc() for why anonymous-private was chosen as the correctly-
 * scoped first version rather than attempting all of mmap's modes at
 * once. */
#define SYS_MMAP       26
#define SYS_MUNMAP     27
/* Text rendering and keyboard input for a .c-os GUI window, extending
 * the SYS_WIN_* family (2-4) added earlier. Without these, an app window
 * could only ever be filled rectangles - no readable text, no way for
 * the user to type anything into it - which is not "a GUI app", just a
 * colored box. Reuses the SAME command-queue architecture as
 * SYS_WIN_FILL: syscalls only queue, the GUI owner thread does the
 * actual drawing, exactly as documented in cos_app_window.c. */
#define SYS_WIN_DRAW_TEXT 28
#define SYS_WIN_POLL_KEY  29

/* ---- Threads -------------------------------------------------------------
 * The kernel already had thread_create()/thread_exit() and a scheduler
 * that runs multiple threads per process - none of it was reachable from
 * ring3. A .c-os program was single-threaded not by design but because
 * nothing exposed the capability. */
#define SYS_THREAD_CREATE 30
#define SYS_THREAD_EXIT   31
#define SYS_THREAD_JOIN   32

/* ---- Time ----------------------------------------------------------------
 * Without these a program cannot measure elapsed time, animate, time out,
 * or seed anything - it can only sleep for a fixed duration and hope. */
#define SYS_TIME_MS       33   /* monotonic milliseconds since boot */
#define SYS_TIME_UNIX     34   /* wall-clock seconds from the RTC */

/* ---- Filesystem mutation --------------------------------------------------
 * Reading and writing file CONTENT was possible; changing the filesystem
 * itself was not. A program could not create a directory, delete a file it
 * had written, or rename anything. */
#define SYS_MKDIR         35
#define SYS_UNLINK        36
#define SYS_RENAME        37
#define SYS_STAT          38

/* ---- Networking ----------------------------------------------------------
 * The kernel already had a TCP/IP stack, DNS resolver and HTTP client
 * (src/kernel/api/net_api.*, used by the bundled browser) - none of it
 * reachable from ring3. A .c-os program could not make a network request
 * of any kind.
 *
 * Exposed at the HTTP/DNS level rather than as BSD sockets: net_api is
 * what actually exists and is exercised by the browser, whereas a socket
 * layer would be a new abstraction written on top of it with nothing yet
 * proving it works. This is the honest surface - it makes real network
 * requests possible now, and does not pretend to be POSIX sockets. */
#define SYS_NET_AVAILABLE 39
#define SYS_NET_RESOLVE   40   /* hostname -> 4-byte IPv4 */
#define SYS_NET_HTTP_GET  41   /* url -> response body */

/* ---- Thread-local storage and the dynamic link map -----------------------
 *
 * A program that wants `__thread` variables needs the CPU's %fs base to
 * point at its TLS block, and only ring0 can write that MSR. The kernel
 * sets it up for the main thread at exec time; SYS_SET_TLS is how a
 * thread the program created itself installs its own block, which is the
 * whole point of thread-LOCAL storage.
 *
 * SYS_DL_INFO hands back the address of the read-only link map the
 * loader published (see cos_elf_linkmap_t). It duplicates what the
 * auxiliary vector already carries in COS_AT_COS_LINKMAP, and exists for
 * the case the auxv cannot cover: code running long after startup, in a
 * library, that never saw the initial stack. */
#define SYS_SET_TLS       42   /* rdi = thread pointer                  */
#define SYS_GET_TLS       43
#define SYS_DL_INFO       44   /* -> address of the cos_elf_linkmap_t   */
/* dlopen with flags (RTLD_GLOBAL/LOCAL), and dlsym against the whole
 * global scope rather than a single handle - neither expressible in the
 * original two-argument SYS_DLOPEN/SYS_DLSYM, which are kept so existing
 * programs keep working. */
#define SYS_DLOPEN2       45   /* rdi = path, rsi = flags               */
#define SYS_DLCLOSE       46

/* ---- thread synchronisation and process arguments -----------------------
 *
 * SYS_FUTEX_WAIT/WAKE are the primitive every userland lock is built on.
 * Before these, .c-os had threads and nothing to synchronise them with;
 * see src/kernel/cos_futex.c for why a futex rather than kernel mutex
 * objects.
 *
 * SYS_SPAWN2 exists because SYS_SPAWN takes only a path - there was NO
 * way for a program to pass arguments or an environment to a child it
 * started. The kernel could already build a full argv/envp stack; the
 * syscall simply had no parameters to fill it from. */
#define SYS_FUTEX_WAIT    47   /* rdi=addr, rsi=expected, rdx=timeout_ms */
#define SYS_FUTEX_WAKE    48   /* rdi=addr, rsi=count                    */
#define SYS_GETTID        49
#define SYS_CPU_STATS     298   /* (uint8_t *pct, int max) -> number of CPUs; pct[i] = percent busy of CPU i over the last ~0.5 s */
#define SYS_GETCPU        299   /* the logical CPU this thread is running on */
#define SYS_SPAWN2        50   /* rdi=path, rsi=argv, rdx=envp           */

/* ---- IPC: the gate that makes userspace servers possible --------------
 *
 * src/kernel/ipc.c has had a complete message-passing and shared-memory
 * implementation (send/receive/respond/notify, per-process mailboxes,
 * semaphore-backed blocking receive, shared-memory create/attach) for
 * some time - but NOT ONE syscall exposed any of it. Every caller was
 * in-kernel, so from userspace the whole subsystem may as well not have
 * existed: a userspace file server or GUI server had no way to receive a
 * request, and no way to answer one.
 *
 * That is the actual blocker for taking the worthwhile part of a
 * microkernel design (isolating things that benefit from isolation)
 * without paying for the part that is not worth it here (moving the
 * scheduler, paging, interrupt handling or the graphics hot path out of
 * the kernel, which costs real performance and buys little on a machine
 * that runs one user's desktop).
 *
 * Every one of these validates user pointers with paging_user_range_ok()
 * before touching them, the same way SYS_SPAWN2 above already does -
 * messages cross a trust boundary, and a server that can be crashed by a
 * malformed request from a client is not isolation, just indirection. */
#define SYS_IPC_SEND      51   /* rdi=dst_pid, rsi=type, rdx=buf, r10=len   */
#define SYS_IPC_RECV      52   /* rdi=&hdr(4 u64: src,type,len,rsv), rsi=buf,
                                * rdx=buf_cap, r10=timeout_ms (~0 = forever) */
#define SYS_IPC_RESPOND   53   /* rdi=dst_pid, rsi=msg_id, rdx=status,
                                * r10=buf, r8=len                            */
#define SYS_IPC_NOTIFY    54   /* rdi=dst_pid, rsi=event, rdx=buf, r10=len  */
#define SYS_SHM_CREATE    55   /* rdi=size, rsi=&shm_id_out                  */
#define SYS_SHM_ATTACH    56   /* rdi=shm_id, rsi=&addr_out                  */

/* mprotect: rdi=addr, rsi=length, rdx=COS_PROT_* flags. Must exactly
 * match an existing mmap() region's bounds - the same "exact match only,
 * no splitting" simplification SYS_MUNMAP already makes (see
 * cos_mmap_free()'s own comment), for the same reason: a general
 * implementation needs the region table to support splitting one
 * tracked range into several, which the current fixed 16-entry array
 * does not. W^X is enforced HERE, not left to the page-fault path to
 * second-guess: a request for PROT_WRITE and PROT_EXEC together is
 * refused outright. The correct, standard pattern for a JIT under this
 * restriction is mmap(PROT_READ|PROT_WRITE), write the generated code,
 * then mprotect(PROT_READ|PROT_EXEC) to flip it non-writable and
 * executable - never both permissions at once. */
#define SYS_MPROTECT      57
#define SYS_WIN_POLL_MOUSE 58
/* Audio output - see cos_audio.c. Userland mirror: userland/lib/cos_lib.c. */
#define SYS_AUDIO_OPEN     170
#define SYS_AUDIO_WRITE    171
#define SYS_AUDIO_STATUS   172
#define SYS_AUDIO_DRAIN    173
#define SYS_AUDIO_STOP     174
#define SYS_AUDIO_VOLUME   175
#define SYS_AUDIO_CLOSE    176
/* Window API v2 - shared pixel buffer + unified events (cos_app_window.c) */
#define SYS_WIN2_CREATE    180   /* (const char* title, w|h<<32, cos_win_info_t* out) */
#define SYS_WIN2_PRESENT   181   /* (handle) */
#define SYS_WIN2_POLL      182   /* (handle, cos_win_event_t*) -> 1 event, 0 none, -1 bad handle */
#define SYS_WIN2_WAIT      183   /* (handle, cos_win_event_t*, timeout_ms) */
#define SYS_WIN2_SET_TITLE 184   /* (handle, const char*) */
#define SYS_WIN2_CLOSE     185   /* (handle) */
#define SYS_WIN2_SCREEN    186   /* (int32_t out[2]) -> screen width/height */
#define SYS_WIN2_INPUT     251   /* (handle, cos_win_input_t *out) -> 0 / -1: keys down, pointer motion since the last call, focus */
#define SYS_TIME_US        261   /* () -> microseconds since boot (TSC based; monotonic) */
#define SYS_LXSHM_MAP      260   /* (name of a /dev/shm object, uint64_t *size_out) -> user address of the Linux shared memory object (the Xvfb screen), or 0 */
#define SYS_FILE_DROP      255   /* (path, copy) -> 0 / -1: drop a file at the pointer: onto the app window under it (COS_EV_DROP) or onto the desktop */
#define SYS_DRAG_GHOST     256   /* (label or 0) -> 0: show / hide a file-drag label that follows the pointer */
#define SYS_WIN2_RESIZE    254   /* (handle, w | h << 32, cos_win_info_t *out) -> 0 / -1: replace the pixel surface by one of another size (same address) */
#define SYS_WIN2_FULLSCREEN 253  /* (handle, mode 0 off / 1 on / 2 toggle) -> 1 fullscreen, 0 windowed, -1 error */
#define SYS_WIN2_GRAB      252   /* (handle, on) -> 0 / -1: lock the pointer to the window (cursor hidden, raw motion via SYS_WIN2_INPUT) */
#define SYS_STAT_EX        190
#define SYS_FS_SPACE       191
#define SYS_OPEN_PATH      192
#define SYS_UI_PREFS       193
#define SYS_LISTDIR_EX     194
#define SYS_CLIP_SET       195
#define SYS_CLIP_GET       196
#define SYS_RTC_NOW        197   /* (cos_datetime_t *out) -> 0 on success. Real wall-clock date/time for ring-3 (cos_time_ms() is only monotonic ms since boot) */
#define SYS_SETTINGS_GET   198   /* (cos_settings_t *out) -> 0 on success. Everything the ring-3 Settings app reads. */
#define SYS_SETTINGS_SET   199   /* (key, value) -> 0 on success, -1 on an unknown key. See cos_settings_key_t. */
#define SYS_IMAGE_DECODE   200   /* (path, dst_rgb, capacity, &width_height) */
#define SYS_WIN2_VIDEO_YUV420 201  /* (handle, const cos_video_yuv420_t *frame) -> 0 on success.
                                    * Struct layout must match cos_video_yuv420_t in
                                    * userland/include/cos.h exactly - no shared header. */
#define SYS_SETTINGS_ACTION 202   /* (action, arg) -> 0 on success, -1 on failure.
                                   * See the COS_ACTION_* enum in userland/include/cos.h. */
#define SYS_PASSWORD_CHANGE 203   /* (const char *old_password, const char *new_password) -> 0/-1.
                                   * Each a NUL-terminated string, max 63 chars. */
#define SYS_SET_ATTR        204   /* (path, attr|mask<<8) -> 0/-1. Low byte = COS_ATTR_* bits to set,
                                   * next byte = which bits to change. See cos_fm_set_attr(). */
/* ---- compute jobs on application processors (see smp_user.h) ----------- */
#define SYS_PAR_INFO        206   /* () -> number of cores that can run jobs */
#define SYS_PAR_START       207   /* (const cos_par_start_t *) -> job id or -errno */
#define SYS_PAR_POLL        208   /* (id, cos_par_result_t *or NULL) -> state or -errno */
#define SYS_PAR_STOP        209   /* (id) -> 0 */
#define SYS_PAR_FREE        210   /* (id) -> 0 */
#define SYS_WIN2_VIDEO_MAP    211 /* (handle, w|h<<32, uint64_t *bytes_each) -> user address of two RGBA buffers (the second follows
                                   * page-rounded w*h*4 bytes later), or -1. Zero-copy video: convert into the
                                   * buffer that is not on screen, then SYS_WIN2_VIDEO_COMMIT it. */
#define SYS_SANDBOX 215 /* (keep_caps, jail_path|0) -> remaining caps, or -1. Drops capabilities / confines file paths; never widens. */
#define SYS_SYSTEM_INFO 216 /* (cos_sysinfo_t *out) -> 0 / -1. Memory, storage, scheduler, CPU and OS identity in one snapshot. */
#define SYS_NET_HTTP_START  218 /* (url) -> job id, or -1. The transfer runs on a kernel thread; it follows redirects and speaks HTTPS */
#define SYS_NET_HTTP_POLL   219 /* (job, cos_http_result_t *res) -> 0 running, 1 finished (res filled), -1 no such job */
#define SYS_NET_HTTP_TAKE   220 /* (job, buf, cap) -> body bytes copied (byte-exact), or -1; frees the job */
#define SYS_SOCKET     222 /* (type 1 stream / 2 datagram) -> sd or -errno. Every socket call is non-blocking: poll to wait. */
#define SYS_BIND       223 /* (sd, ip, port) */
#define SYS_LISTEN     224 /* (sd, backlog) */
#define SYS_ACCEPT     225 /* (sd, cos_sockaddr_t *peer|0) -> new sd, or -EAGAIN */
#define SYS_CONNECT    226 /* (sd, ip, port) -> 0, -EINPROGRESS (call again / poll for POLLOUT), or an error */
#define SYS_SEND       227 /* (sd, buf, len) -> bytes, -EAGAIN when the send queue is full */
#define SYS_RECV       228 /* (sd, buf, len) -> bytes, 0 = end of stream, -EAGAIN */
#define SYS_SENDTO     229 /* (sd, buf, len, ip, port) */
#define SYS_RECVFROM   230 /* (sd, buf, len, cos_sockaddr_t *from|0) */
#define SYS_SOCK_POLL  231 /* (cos_pollfd_t *fds, n) -> number ready; never waits */
#define SYS_SOCK_CLOSE 232
#define SYS_SOCK_NAME  233 /* (sd, peer?, cos_sockaddr_t *out) */
#define SYS_DNS_START  234 /* (host, cos_dns_result_t *out) */
#define SYS_DNS_POLL   235 /* (cos_dns_result_t *io) */
#define SYS_NET_INFO   236 /* (cos_netinfo_t *out) */
#define SYS_SHELL_CREATE 237 /* (layer, cos_win_info_t *out): the one taskbar panel; geometry is fixed by the kernel */
#define SYS_WIN_LIST   238 /* (cos_winfo_t *out, max) -> count */
#define SYS_WIN_CTL    239 /* (action, uid, arg) */
#define SYS_CTX_LIST     244 /* (cos_ctxsnap_t *out) -> 1 while a context menu is open */
#define SYS_KMOD_LOAD    245 /* (path, password) -> 0 / -1. Loads a signed .c-osys kernel module. Fully privileged callers only (never a sandboxed one). */
#define SYS_KMOD_UNLOAD  246 /* (name, password) -> 0 / -1 (no such module) / -2 (in use) */
#define SYS_DLCTL        248 /* (op, a, b, c): the rest of the dl interface - close plan, dladdr, RTLD_NEXT, last error, library path (COS_DLCTL_*) */
#define SYS_KMOD_LIST    247 /* (char *buf, cap) -> count; "name version level\n" per module (needs the system-information capability) */
#define SYS_NOTIF_LIST   243 /* (cos_notif_t *out, max) -> count */
#define SYS_KERNEL_UPDATE 250 /* (op, a, b): op 0 status(cos_kupd_status_t *a), op 1 install(blob a, len b) -> 0 / negative. Fully privileged callers only (never a sandboxed one); the kernel checks the signature itself */
#define SYS_NOTIF_SEND   249 /* (title, message, type 0 info / 1 warning / 2 error / 3 success) -> 0 / -1: a desktop toast (window capability) */
#define SYS_DESKTOP_LIST 241 /* (cos_dicon_t *out, max) -> count */
#define SYS_DESKTOP_CTL  242 /* (action, a, b, c) */
#define SYS_SHELL_ICONS 240 /* (layer, cos_shell_icon_t *icons, n): icons the compositor draws over the layer, live at its next present */
#define SYS_NET_HTTP_CANCEL 221 /* (job) -> 0. Abandons a running job or frees a finished one */
#define SYS_PROCLIST    217 /* (cos_procinfo_t *out, max<=128) -> count of live processes (or -1) */
#define SYS_DEVICES_LIST 214 /* (cos_device_info_t *out, max<=64) -> count of devices (or -1) */
#define SYS_WIN2_PRESENT_RECTS 213 /* (handle, const cos_win_rect_t *rects, count<=64) -> 0 / -1. Presents only those client rectangles. */
#define SYS_WIN2_VIDEO_COMMIT 212 /* (handle, idx, w|h<<32, x|y<<32, dw|dh<<32) -> 0 / -1 */
#define SYS_FS_GENERATION   205   /* () -> a counter that increases every time mkdir/unlink/rename
                                   * succeeds anywhere on the volume. Poll it instead of re-listing a
                                   * directory every frame - see cos_fs_generation()'s comment. */
/* Must match the enum in userland/include/cos.h exactly - no shared header between the
 * two builds here, same as the struct layout in the SYS_SETTINGS_GET handler below. */
#define COS_SET_THEME       1
#define COS_SET_FONT_SCALE  2
#define COS_SET_WALLPAPER   3
#define COS_SET_DARK_MODE   4
#define COS_SET_LANGUAGE    5
#define COS_SET_ICON_SIZE   6
#define COS_SET_DEVELOPER_MODE        7
#define COS_SET_AUTO_SAVE_INTERVAL    8
#define COS_SET_FONT_FAMILY           9
#define COS_SET_FONT_PIXEL_WIDTH      10
#define COS_SET_RENDER_BACKEND        11
#define COS_SET_GPU_FULL_DRAW         12
#define COS_SET_WINDOW_ANIMATIONS     13
#define COS_SET_NOTIFICATIONS         14
#define COS_SET_FPS_OVERLAY           15
#define COS_SET_AUTOSTART_TERMINAL    16
#define COS_SET_AUTOSTART_FILE_MANAGER 17
#define COS_SET_AUTOSTART_BROWSER     18
#define COS_SET_MOUSE_SENSITIVITY     19
#define COS_SET_MOUSE_RAW_INPUT       20
#define COS_SET_MOUSE_DRAG_THRESHOLD  21
#define COS_SET_MULTI_CURSOR          22
#define COS_SET_EFM_SHOW_HIDDEN       23
#define COS_SET_EFM_CONFIRM_DELETE    24
#define COS_SET_EFM_DEFAULT_VIEW      25
#define COS_SET_TERMINAL_AUTOSCROLL   26
#define COS_SET_DESKTOP_COLOR         27
#define COS_SET_DESKTOP_COLOR2        28
#define SYS_WIN2_GET_PATH  187
#define SYS_WIN2_VIDEO     188   /* (handle, rgba, w|h<<32, stride, x|y<<32, dw|dh<<32) */
#define SYS_WIN2_GET_PATH  187   /* (handle, char* buf, cap) -> path of the last OPEN/DROP event */

#define COS_PROT_READ  1u
#define COS_PROT_WRITE 2u
#define COS_PROT_EXEC  4u

/* Dedicated PML4 slot (PML4[5]) for mmap, matching the SAME pattern
 * already used for the program image (PML4[1]), stack (PML4[2]), heap
 * (PML4[3]) and libraries (PML4[4]) - a fixed, non-overlapping region per
 * purpose, rather than trying to carve mmap space out of any of those. */
#define COS_MMAP_BASE      0x0000028000000000ULL
#define COS_MMAP_MAX_TOTAL (1ULL * 1024ULL * 1024ULL * 1024ULL)   /* 1 GiB cap per process */

/**
 * cos_mmap_alloc - bump-allocate a fresh anonymous region for `proc`.
 *
 * DESIGN: anonymous-private only, bump allocation, no reuse of freed
 * virtual address ranges.
 *
 * Anonymous-private (no file, no MAP_SHARED) was chosen as the correctly-
 * scoped first version: file-backed mmap needs per-mapping fd+offset
 * metadata and lazy page-in from the filesystem inside the page-fault
 * path, and MAP_SHARED needs cross-process physical-page-sharing
 * tracking that the physical allocator does not have today. Both are
 * real future work, not silently promised by this syscall's shape.
 *
 * Bump allocation (mmap_next_offset only ever increases) rather than a
 * general first-fit/best-fit allocator with merge/split on munmap: this
 * keeps region tracking a fixed 16-entry array with no fragmentation
 * bookkeeping. The real cost is honest and stated - munmap() frees the
 * PHYSICAL pages backing a region but never reclaims the VIRTUAL range
 * for a future mmap - bounded by COS_MMAP_MAX_TOTAL (1 GiB) per process,
 * which is generous for a program's total lifetime mmap usage without
 * risking silent, unbounded growth.
 *
 * Demand-paged exactly like the heap: task_handle_page_fault() (task.c)
 * checks proc->mmap_regions[] the same way it already checks
 * heap_start/heap_end, so nothing is eagerly mapped here - only recorded.
 */
static int64_t cos_mmap_alloc(process_t *proc, uint64_t length, bool writable)
{
    if (length == 0) return -1;
    uint64_t rounded = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (proc->mmap_next_offset + rounded < proc->mmap_next_offset) return -1;   /* overflow */
    if (proc->mmap_next_offset + rounded > COS_MMAP_MAX_TOTAL) return -1;       /* cap */

    int slot = -1;
    for (int i = 0; i < COS_MMAP_MAX_REGIONS; ++i) {
        if (!proc->mmap_regions[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;   /* table full */

    uint64_t start = COS_MMAP_BASE + proc->mmap_next_offset;
    proc->mmap_regions[slot].start = start;
    proc->mmap_regions[slot].end = start + rounded;
    proc->mmap_regions[slot].used = true;
    proc->mmap_regions[slot].writable = writable;
    /* Explicitly cleared, not assumed zero: this slot may be a REUSED
     * one from an earlier region that munmap() freed (cos_mmap_free()
     * only clears `used`, leaving the other fields as they were - see
     * its own comment). Without this line, a new mmap() landing in a
     * slot a previous mmap()+mprotect(PROT_EXEC) had used would silently
     * inherit executable=true from that prior occupant: a mapping the
     * caller never asked to be executable, executable anyway. This is
     * the one line that makes "every region starts non-executable"
     * actually true rather than merely true-by-coincidence-so-far. */
    proc->mmap_regions[slot].executable = false;
    proc->mmap_next_offset += rounded;
    return (int64_t)start;
}

static bool cos_mmap_free(process_t *proc, uint64_t addr, uint64_t length)
{
    uint64_t rounded = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (int i = 0; i < COS_MMAP_MAX_REGIONS; ++i) {
        cos_mmap_region_t *r = &proc->mmap_regions[i];
        if (!r->used) continue;
        if (r->start != addr || (r->end - r->start) != rounded) continue;
        /* Exact-match only: a real munmap() supports partially unmapping
         * or splitting a region. Requiring an exact match to a
         * previously-returned mmap() region is a stated, deliberate
         * simplification - see the header note - rather than
         * implementing region splitting for this pass. */
        uint64_t pages = (r->end - r->start) / PAGE_SIZE;
        paging_release_user_range(r->start, pages);   /* user range: never enters the kernel's free list */
        r->used = false;
        return true;
    }
    return false;
}

#define SYSCALL_TITLE_MAX 64

/* Upper bound on how far SYS_BRK/SYS_SBRK will let one process grow its
 * heap. Policy, not an architectural limit: it bounds how much a single
 * runaway .c-os program can take from the system, and it is checked
 * independently of the heap-vs-stack collision check below (a program
 * could otherwise sit under the cap and still collide, or vice versa). */
#define COS_USER_HEAP_MAX (64ULL * 1024ULL * 1024ULL)

/* True if growing the heap to `target` cannot run into the stack region.
 *
 * This must NOT assume the heap sits below the stack. In this address-space
 * layout it does not: the stack region is at PML4[2] (0x100_0000_0000) and
 * the heap at PML4[3] (0x180_0000_0000), so the heap starts ABOVE the
 * stack. A naive "target >= stack_start" test - which is the usual check
 * when the heap grows up toward a stack growing down - is therefore true
 * for every possible heap address here, and rejected every single
 * allocation. That was a real bug caught by the first ring3 test of this
 * syscall (MEMFILE_FAIL sbrk failed), not by reading the code.
 *
 * The correct test is whether the heap's resulting span would overlap the
 * stack's span at all, in either direction. */
static bool cos_heap_growth_is_safe(const process_t *proc, uint64_t target)
{
    if (!proc->stack_start || !proc->stack_end) return true;   /* no stack to hit */
    if (target <= proc->heap_start) return true;               /* not growing */
    /* Heap occupies [heap_start, target); stack occupies
     * [stack_start, stack_end). Disjoint iff one ends at or before the
     * other begins. */
    if (target <= proc->stack_start) return true;
    if (proc->heap_start >= proc->stack_end) return true;
    return false;
}

#define COS_SYSCALL_PATH_MAX 256
/* Whole-file read/write syscalls. Was 8 MiB; C-OS Studio programs may now be up to 10 MB, so the
 * cap has to clear that with headroom. */
#define COS_SYSCALL_FILE_MAX (16ULL * 1024ULL * 1024ULL)
#define COS_SYSCALL_SLEEP_MAX_MS (60ULL * 1000ULL)   /* still used by SYS_SLEEP_MS */

/* Defined in userspace_demo.c alongside the other launch paths. Returns
 * the new pid, or -1. */
extern int64_t cos_spawn_elf_path(const char *path);
extern bool ipc_pid_related(uint64_t a, uint64_t b);
/* Wrong guesses of the administrator password (password change, module loading): after 5 failures every attempt is refused for a growing time
 * (30 s, doubling, at most ~1 h). With no account set nothing is asked. */
extern bool storage_has_password(void);
extern bool storage_verify_password(const char *password);
static uint32_t g_pw_fail; static uint64_t g_pw_until;
static uint64_t cos_now_ms(void) { return (get_timer_ticks() * 1000ULL) / TIMER_TICKS_PER_SEC; }
static bool cos_pw_locked(void) { return g_pw_fail >= 5 && cos_now_ms() < g_pw_until; }
static void cos_pw_note(bool ok) {
    if (ok) { g_pw_fail = 0; return; }
    if (g_pw_fail < 20) g_pw_fail++;
    if (g_pw_fail >= 5) { uint32_t sh = g_pw_fail - 5 > 7 ? 7 : g_pw_fail - 5; g_pw_until = cos_now_ms() + (30000ULL << sh); }
}
static bool cos_admin_auth(const char *pw) {
    if (!storage_has_password()) return true;
    if (cos_pw_locked()) return false;
    bool ok = storage_verify_password(pw);
    cos_pw_note(ok);
    return ok;
}
extern int cos_kmod_load_path(const char *path);
static uint32_t cos_syscall_caller_pid(void);
static bool cos_fs_gate(uint32_t op, const char *p1, const char *p2) { return cos_kmod_fs_allow(op, cos_syscall_caller_pid(), p1, p2) != 0; }
extern int cos_kmod_unload(const char *name);
extern int cos_kmod_list(char *out, size_t cap);

/* a confined process may message only a process of its own family (itself, ancestors, descendants, siblings) */
static bool cos_ipc_dst_ok(uint64_t dst) {
    process_t *me = process_get_current();
    if (!me || me->caps == COS_CAP_ALL) return true;
    return ipc_pid_related(me->pid, dst);
}
extern int64_t cos_dlopen_path(const char *path);
extern bool cos_fs_mkdir(const char *path);
extern bool cos_fs_unlink(const char *path);
extern bool cos_fs_rename(const char *oldp, const char *newp);
extern bool cos_fs_stat(const char *path, uint64_t *out_size, uint8_t *out_is_dir);

/* Mirrors cos_stat_t in userland/include/cos.h - kept in sync by hand
 * because the kernel cannot include the userland header. */
typedef struct { uint64_t size; uint8_t is_dir; } cos_stat_t;
extern uint64_t cos_dlsym_handle(int64_t handle, const char *name);

/* Copies a NUL-terminated path out of user memory into a kernel buffer.
 *
 * Validates page by page as it walks, rather than validating a fixed
 * length up front: the length is not known until the NUL is found, and
 * assuming a maximum would reject legitimate short paths that sit near
 * the end of a mapping. Rejects anything unterminated within the buffer,
 * so the kernel never ends up with a non-terminated string. */
/**
 * cos_syscall_touch_user_range - demand-page in any not-yet-mapped pages
 * in [uaddr, uaddr+len) that legitimately belong to the calling process,
 * BEFORE validating the range with paging_user_range_ok().
 *
 * Why this exists (a real bug, found by testing the ordinary case rather
 * than by reading the code): heap memory returned by SYS_SBRK is not
 * actually mapped until first touched - that is the whole point of the
 * demand paging in task.c. paging_user_range_ok() only accepts PAGES THAT
 * ARE ALREADY PRESENT, so a syscall receiving a freshly-grown, never-yet-
 * written heap buffer as a destination rejected it outright, even though
 * it is a completely ordinary, legitimate pointer inside
 * [heap_start, heap_end). "sbrk a buffer, then read() into it" is not an
 * edge case - it is the normal way a buffer gets used - so this was not a
 * corner case bug, it was a "the common path does not work" bug.
 *
 * This calls the EXACT SAME function the real page-fault handler calls
 * (task_handle_page_fault(), see task.c) with a synthesized error code
 * matching what the CPU would have generated for a real access of this
 * kind (user-mode, present-bit clear, write bit set iff `need_write`).
 * That means a syscall-validated buffer is faulted in with IDENTICAL
 * semantics to the process touching the memory itself: heap pages inside
 * the process's own heap window get mapped, stack pages just below the
 * current bottom grow the stack, and anything else is correctly left
 * unmapped for paging_user_range_ok() to then reject. This does not
 * relax validation - it only pre-does the mapping step a real touch would
 * have done anyway, so a genuinely invalid pointer is not made to look
 * valid by this call.
 */
static void cos_syscall_touch_user_range(uint64_t uaddr, uint64_t len, bool need_write)
{
    if (len == 0) return;
    uint64_t start = uaddr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = uaddr + len;
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (paging_virt_to_phys(page) != 0) continue;   /* already mapped */
        uint64_t synthetic_error_code = 0x4ULL | (need_write ? 0x2ULL : 0ULL);
        (void)task_handle_page_fault(page, synthetic_error_code);
        /* Ignored on purpose: if this could not fault the page in (a
         * genuinely invalid address), the page is simply still not
         * present, and paging_user_range_ok() right after this call is
         * what actually enforces that and rejects the syscall. */
    }
}

/* File-system jail (SYS_SANDBOX): a confined process may only name absolute paths under its jail prefix, and may not
 * climb out with "..". Applied here because every path-taking syscall copies its path through this function. */
/* The file system (FatFs) is more generous than "/" and "..": it also takes '\\' as a separator, ignores trailing dots and spaces in a name
 * (".. " and "..." mean the same as "..") and has no use for control characters. A path is therefore only accepted when no component
 * is made of nothing but dots and spaces, and there is no backslash and no control character at all. */
static bool cos_path_is_clean(const char *path)
{
    const char *comp = path; bool only_dots = true; size_t len = 0;
    for (const char *s = path; ; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || (c < 0x20 && c != 0) || c == 0x7F) return false;
        if (c == '/' || c == 0) {
            if (len > 0 && only_dots) return false;                 /* ".", "..", "...", ". ." ... */
            if (c == 0) break;
            comp = s + 1; only_dots = true; len = 0; (void)comp;
        } else {
            len++; if (c != '.' && c != ' ') only_dots = false;
        }
    }
    return true;
}
static bool cos_jail_allows(const char *path)
{
    process_t *p = process_get_current();
    if (!p || p->type != TASK_TYPE_USER || !p->jail[0]) return true;
    if (path[0] != '/') return false;
    if (!cos_path_is_clean(path)) return false;
    size_t n = strlen(p->jail);
    return strncmp(path, p->jail, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static bool cos_syscall_copy_path(char *dst, uint64_t dst_size, uint64_t uptr)
{
    if (!dst || dst_size == 0 || uptr == 0) return false;
    for (uint64_t i = 0; i < dst_size; ++i) {
        cos_syscall_touch_user_range(uptr + i, 1, false);
        if (!paging_user_range_ok(uptr + i, 1, false)) {
            serial_puts("[SYSCALL] path rejected: not in a valid user mapping\n");
            return false;
        }
        char c = *(const char *)(uintptr_t)(uptr + i);
        dst[i] = c;
        if (c == '\0') {
            if (!cos_jail_allows(dst)) { serial_puts("[SYSCALL] path rejected: outside the process's jail\n"); return false; }
            return true;
        }
    }
    serial_puts("[SYSCALL] path rejected: unterminated or too long\n");
    return false;
}

/* Caps on a spawn's argument and environment vectors. These bound a
 * kernel-side copy whose size is chosen by the caller, so they are
 * policy rather than an ABI limit; the initial stack builder has its own
 * 64 KiB budget on top. */
#define COS_SPAWN_MAX_ARGS   64      /* was 32: a make/compile line with many sources exceeded it */
#define COS_SPAWN_MAX_ARGLEN 256

/* Copies a NULL-terminated array of user string pointers into kernel
 * storage, returning the count or -1.
 *
 * Every pointer AND every string is validated, one page-checked byte at
 * a time, exactly as cos_syscall_copy_path does. The vector is a user
 * pointer to an array of user pointers, so there are two levels of
 * untrusted indirection here and both have to be checked - reading the
 * array itself without validating it is the easier half to forget.
 *
 * Copying happens before the caller switches address spaces: these
 * pointers are only meaningful in the PARENT's address space, and
 * dereferencing one after the switch would silently read the child's
 * memory instead. */
/* One argument or environment string of a spawn: raw bytes up to the NUL (a program may pass any bytes in an argument), never a path - the jail and the
 * path-cleanliness checks belong to paths, and applying them here refused ordinary arguments such as "a..b" or "x\\y". Fails (instead of truncating) when the
 * string is unterminated within `cap` or leaves user memory. */
static bool cos_syscall_copy_arg(char *dst, size_t cap, uint64_t uaddr)
{
    if (!dst || cap == 0 || uaddr == 0) return false;
    for (size_t i = 0; i < cap; ++i) {
        uint64_t a = uaddr + i;
        if (i == 0 || (a & (PAGE_SIZE - 1)) == 0) {
            cos_syscall_touch_user_range(a, 1, false);
            if (!paging_user_range_ok(a, 1, false)) return false;
        }
        char c = *(const char *)(uintptr_t)a;
        dst[i] = c;
        if (c == '\0') return true;
    }
    return false;
}

static int cos_syscall_copy_strvec(uint64_t uvec,
                                   char storage[][COS_SPAWN_MAX_ARGLEN],
                                   const char *out[], int max)
{
    if (uvec == 0) { out[0] = NULL; return 0; }
    if (uvec & 7u) return -1;                    /* pointer array must align */

    int n = 0;
    for (; n < max; ++n) {
        uint64_t slot = uvec + (uint64_t)n * 8u;
        cos_syscall_touch_user_range(slot, 8, false);
        if (!paging_user_range_ok(slot, 8, false)) return -1;

        uint64_t sp = *(const uint64_t *)(uintptr_t)slot;
        if (sp == 0) break;                       /* end of vector */
        if (!cos_syscall_copy_arg(storage[n], COS_SPAWN_MAX_ARGLEN, sp)) return -1;
        out[n] = storage[n];
    }
    if (n == max) {
        serial_puts("[SYSCALL] argument vector rejected: too many entries\n");
        return -1;
    }
    out[n] = NULL;
    return n;
}


/* Identity of the ring3 process that issued the current syscall. Used to
 * enforce window ownership, so one .c-os program cannot draw into
 * another's window by guessing a small handle value. */
static uint32_t cos_syscall_caller_pid(void)
{
    process_t *p = process_get_current();
    return p ? (uint32_t)p->pid : 0u;
}

/* Bound how much a single SYS_WRITE can dump, so a buggy or hostile user
 * program can't wedge the kernel into an unbounded serial write loop. */
#define SYSCALL_WRITE_MAX 4096

/* ---- Signal delivery -----------------------------------------------------
 *
 * DESIGN: a real trampoline on the process's own stack, not a direct call.
 *
 * When a signal is deliverable, this rewrites the interrupt return frame
 * (`r`) so that the CPU's own `iretq` - the SAME instruction that would
 * otherwise resume the process where it made its syscall - instead
 * resumes at the handler, with CS/SS/CPL unchanged (still ring3) and a
 * genuine ring transition already having happened via the ORIGINAL
 * int 0x80. That is what makes this safe: the handler runs at ring3
 * because the frame it is resumed from IS a ring3 frame, not because of
 * anything this code does to elevate or lower privilege.
 *
 * Stack layout built below (low to high address):
 *   new_rsp             -> [8 bytes] return address = trampoline_addr
 *   trampoline_addr     -> [7 bytes] trampoline machine code
 *   sigframe_addr       -> [saved register state, for SYS_SIGRETURN]
 *
 * The handler is entered with RSP = new_rsp, RDI = signum. When the
 * handler's own compiler-generated `ret` executes, it pops the 8-byte
 * return address (= trampoline_addr) into RIP and continues there. The
 * trampoline is `mov eax, SYS_SIGRETURN; int 0x80` - a normal, real
 * syscall, entered the normal way, which is what makes SYS_SIGRETURN's
 * own handler able to find its way back to the ORIGINAL sigframe: at
 * that point RSP is exactly trampoline_addr (the trampoline pushed
 * nothing of its own before trapping), so sigframe_addr is a fixed,
 * known offset from the RSP the SIGRETURN syscall observes.
 */
typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
    /* The process's blocked_signals mask AT THE MOMENT this signal was
     * delivered, restored by SYS_SIGRETURN. Without this, a signal
     * delivered while a DIFFERENT handler is already running would
     * overwrite proc->last_sigframe_addr with its own frame's location,
     * permanently losing the outer handler's return point - the outer
     * handler's own eventual `ret`-into-trampoline would then read the
     * WRONG (inner) frame, or a frame already reused, corrupting
     * resumption of whatever was interrupted before either signal fired.
     * See the design note in cos_deliver_pending_signal() for why this
     * is prevented by blocking further delivery for the duration of any
     * handler, rather than by trying to support a stack of frames. */
    uint64_t saved_blocked_signals;
} cos_sigframe_t;

/* mov eax, 25 ; int 0x80  (25 = SYS_SIGRETURN) */
static const uint8_t COS_SIG_TRAMPOLINE[7] = {
    0xB8, (uint8_t)SYS_SIGRETURN, 0x00, 0x00, 0x00, 0xCD, 0x80
};

#define COS_SIGFRAME_MAX_SIZE 256u   /* trampoline + sigframe, rounded up */

/* Local user-memory write, matching the pattern already used in cos_elf.c
 * (kept file-local here rather than sharing that file's static helpers
 * across translation units for a handful of small writes). Callers are
 * responsible for pre-touching the range with cos_syscall_touch_user_range()
 * so this cannot silently write into a not-yet-demand-paged page - see
 * the bug this exact class of mistake caused in SYS_READ_FILE/WRITE_FILE
 * before it was fixed. */
static bool cos_sig_write_user(uint64_t uaddr, const void *src, uint64_t len)
{
    if (!paging_user_range_ok(uaddr, len, true)) return false;       /* user-owned, writable pages only: RSP is the process's own value */
    const uint8_t *in = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; ++i) {
        uint64_t v = uaddr + i;
        uint64_t phys = paging_virt_to_phys(v & ~(uint64_t)(PAGE_SIZE - 1));
        if (!phys) return false;
        uint8_t *dst = (uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        dst[v & (PAGE_SIZE - 1)] = in[i];
    }
    return true;
}

static bool cos_sig_read_user(uint64_t uaddr, void *dst, uint64_t len)
{
    if (!paging_user_range_ok(uaddr, len, false)) return false;
    uint8_t *out = (uint8_t *)dst;
    for (uint64_t i = 0; i < len; ++i) {
        uint64_t v = uaddr + i;
        uint64_t phys = paging_virt_to_phys(v & ~(uint64_t)(PAGE_SIZE - 1));
        if (!phys) return false;
        const uint8_t *src = (const uint8_t *)(uintptr_t)PHYS_TO_VIRT(phys);
        out[i] = src[v & (PAGE_SIZE - 1)];
    }
    return true;
}

/* Called once at the tail of every syscall, before returning to ring3.
 * This is the ONLY place signals are delivered - see the header/design
 * note above SYS_FD_CLOSE for why the old code path was unsafe and is not
 * reused. Honest limitation stated once here rather than scattered: a
 * process that never makes a syscall (a pure ring3 compute loop with no
 * yield/sleep/read/write/etc.) will not see a pending signal delivered
 * until it eventually does. Every syscall added in this codebase so far
 * is a natural, frequent checkpoint, but this is not a full preemptive
 * signal model. */
static void cos_deliver_pending_signal(struct regs *r)
{
    process_t *proc = process_get_current();
    if (!proc) return;

    cos_sigset_t deliverable = proc->pending_signals & ~proc->blocked_signals;
    if (deliverable == 0) return;

    int signum = -1;
    for (int i = 0; i < MAX_SIGNALS; ++i) {
        if (deliverable & (1ULL << i)) { signum = i; break; }
    }
    if (signum < 0) return;
    proc->pending_signals &= ~(1ULL << (uint64_t)signum);

    signal_handler_t handler = proc->signal_handlers[signum];

    /* Signal 9 cannot be caught or ignored, matching real SIGKILL - a
     * process must not be able to make itself unkillable by registering a
     * handler (or SIG_ACTION_IGNORE) for it. */
    bool force_terminate = (signum == 9) ||
                           (handler == SIG_ACTION_DEFAULT);
    if (handler == SIG_ACTION_IGNORE && signum != 9) return;   /* discarded */

    if (force_terminate) {
        serial_puts("[SIGNAL] pid=");
        serial_putdec(proc->pid);
        serial_puts(" terminated by signal ");
        serial_putdec((uint64_t)signum);
        serial_puts("\n");
        process_exit(proc, -(1000 + signum));
        /* process_exit() does not return for the current process (see the
         * exiting_self path in task.c) - if it somehow did, fail safe
         * rather than falling through to deliver a signal to a process
         * that should already be gone. */
        for (;;) { __asm__ volatile("hlt"); }
    }

    /* Real handler: build the trampoline + sigframe on the process's own
     * stack, just below its current RSP - the same region a real call
     * or interrupt would use, so it must be pre-touched exactly like any
     * other syscall-visible user buffer. */
    uint64_t frame_base = (r->rsp - COS_SIGFRAME_MAX_SIZE) & ~0xFULL;
    cos_syscall_touch_user_range(frame_base, COS_SIGFRAME_MAX_SIZE, true);

    uint64_t trampoline_addr = frame_base + 8;
    uint64_t sigframe_addr = trampoline_addr + sizeof(COS_SIG_TRAMPOLINE);
    sigframe_addr = (sigframe_addr + 7ULL) & ~7ULL;   /* keep 8-byte aligned */

    cos_sigframe_t frame;
    frame.r15 = r->r15; frame.r14 = r->r14; frame.r13 = r->r13; frame.r12 = r->r12;
    frame.r11 = r->r11; frame.r10 = r->r10; frame.r9  = r->r9;  frame.r8  = r->r8;
    frame.rbp = r->rbp; frame.rdi = r->rdi; frame.rsi = r->rsi; frame.rdx = r->rdx;
    frame.rcx = r->rcx; frame.rbx = r->rbx; frame.rax = r->rax;
    frame.rip = r->rip; frame.rflags = r->rflags; frame.rsp = r->rsp;
    frame.saved_blocked_signals = proc->blocked_signals;

    bool ok = cos_sig_write_user(frame_base, &trampoline_addr, sizeof(uint64_t));
    ok = ok && cos_sig_write_user(trampoline_addr, COS_SIG_TRAMPOLINE, sizeof(COS_SIG_TRAMPOLINE));
    ok = ok && cos_sig_write_user(sigframe_addr, &frame, sizeof(frame));
    if (!ok) {
        /* Could not build the frame (should only happen under real
         * memory pressure, since the range was just touched above).
         * Re-queue the signal rather than silently dropping it, and
         * leave the process to resume normally this one time. */
        proc->pending_signals |= (1ULL << (uint64_t)signum);
        serial_puts("[SIGNAL] failed to deliver signal (could not build stack frame)\n");
        return;
    }

    /* sigframe_addr must be recoverable from RSP alone at SIGRETURN time
     * (see the design note above): stash it in a per-process field rather
     * than trying to derive it purely from arithmetic on the trampoline's
     * fixed size, so a future change to trampoline padding cannot silently
     * desynchronize the two sides. */
    proc->last_sigframe_addr = sigframe_addr;

    /* Block further signal delivery for the duration of this handler -
     * ALL signals, not just this one, and deliberately NOT signal 9
     * (real SIGKILL is never blockable, and this architecture should not
     * make it MORE blockable than it has to be just because a different
     * handler happens to be running).
     *
     * Without this, cos_deliver_pending_signal() would run again at the
     * end of ANY syscall the handler itself makes (writing to serial,
     * sleeping, anything) and - if a second signal is pending - would
     * overwrite last_sigframe_addr with a frame nested inside THIS
     * handler's own stack usage, permanently losing the way back to
     * whatever this handler itself needs to return to. Real Unix
     * sigaction() defaults to at least blocking the same signal during
     * its own handler (SA_NODEFER opts out); blocking everything else
     * too is a stricter, simpler, and here deliberately conservative
     * policy given there is no per-handler sa_mask tracked. */
    proc->blocked_signals = ~(1ULL << 9ULL);

    r->rdi = (uint64_t)signum;
    r->rsp = frame_base;
    r->rip = (uint64_t)handler;
    /* CS/SS/CPL are untouched - the process resumes at ring3 because the
     * frame it resumes from already was one, exactly as it would for an
     * ordinary syscall return. */
}

/* Copies a NUL-terminated string from user space into buf (cap bytes,
 * always NUL-terminated, truncating if longer). Every page the string
 * touches is validated before it is read, so a short string that ends
 * right before an unmapped page is fine and a bad pointer is rejected
 * rather than faulting in the kernel. Control characters are replaced
 * with spaces - these strings end up in title bars. */
static bool cos_syscall_copy_user_string(uint64_t uaddr, char *buf, size_t cap)
{
    if (!uaddr || !buf || cap == 0) return false;
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        uint64_t a = uaddr + i;
        if (i == 0 || (a & (PAGE_SIZE - 1)) == 0) {
            cos_syscall_touch_user_range(a, 1, false);
            if (!paging_user_range_ok(a, 1, false)) return false;
        }
        char c = *(const char *)(uintptr_t)a;
        if (c == '\0') break;
        buf[i] = ((unsigned char)c < 0x20 || c == 0x7F) ? ' ' : c;
    }
    buf[i] = '\0';
    return true;
}


/* Which capabilities a syscall needs. 0 = harmless for a confined process (memory, time, threads, own console). A syscall
 * that is not listed needs COS_CAP_UNKNOWN, which only a fully privileged process holds - so a new syscall is denied to
 * sandboxed code until someone decides what it needs. */
static uint32_t cos_syscall_needs(uint64_t nr, const struct regs *r)
{
    switch (nr) {
    case SYS_WRITE: case SYS_EXIT: case SYS_BRK: case SYS_SBRK: case SYS_YIELD: case SYS_SLEEP_MS: case SYS_GETPID:
    case SYS_WAITPID: case SYS_SIGACTION: case SYS_SIGRETURN: case SYS_MMAP: case SYS_MUNMAP: case SYS_MPROTECT:
    case SYS_THREAD_CREATE: case SYS_THREAD_EXIT: case SYS_THREAD_JOIN: case SYS_TIME_MS: case SYS_TIME_US: case SYS_TIME_UNIX:
    case SYS_SET_TLS: case SYS_GET_TLS: case SYS_FUTEX_WAIT: case SYS_FUTEX_WAKE: case SYS_GETTID: case SYS_GETCPU: case SYS_RTC_NOW:
    case SYS_NET_AVAILABLE: case SYS_SANDBOX: case SYS_FS_GENERATION:
        return 0;
    case SYS_FD_WRITE:
        return COS_CAP_FS_WRITE;                                     /* descriptors 1, 2 ... are ordinary file slots (console output is SYS_WRITE) */
    case SYS_FD_OPEN:                                                /* opening for writing or creating is a write */
        return r->rsi == COS_O_RDONLY ? COS_CAP_FS_READ : (COS_CAP_FS_READ | COS_CAP_FS_WRITE);
    case SYS_READ_FILE: case SYS_FD_OPENDIR: case SYS_FD_READ: case SYS_FD_LSEEK: case SYS_FD_READDIR:
    case SYS_FD_CLOSE: case SYS_STAT: case SYS_STAT_EX: case SYS_LISTDIR_EX: case SYS_FS_SPACE: case SYS_DLOPEN:
    case SYS_DLSYM: case SYS_DL_INFO: case SYS_DLOPEN2: case SYS_DLCLOSE: case SYS_DLCTL:
        return COS_CAP_FS_READ;
    case SYS_WRITE_FILE: case SYS_MKDIR: case SYS_UNLINK: case SYS_RENAME: case SYS_SET_ATTR:
        return COS_CAP_FS_WRITE;
    case SYS_NET_RESOLVE: case SYS_NET_HTTP_GET: case SYS_NET_HTTP_START: case SYS_NET_HTTP_POLL:
    case SYS_NET_HTTP_TAKE: case SYS_NET_HTTP_CANCEL: case SYS_SOCKET: case SYS_BIND: case SYS_LISTEN: case SYS_ACCEPT:
    case SYS_CONNECT: case SYS_SEND: case SYS_RECV: case SYS_SENDTO: case SYS_RECVFROM: case SYS_SOCK_POLL: case SYS_SOCK_CLOSE:
    case SYS_SOCK_NAME: case SYS_DNS_START: case SYS_DNS_POLL: case SYS_NET_INFO:
        return COS_CAP_NET;
    case SYS_SPAWN: case SYS_SPAWN2: case SYS_KILL: case SYS_OPEN_PATH:
        return COS_CAP_SPAWN;
    case SYS_IPC_SEND: case SYS_IPC_RECV: case SYS_IPC_RESPOND: case SYS_IPC_NOTIFY: case SYS_SHM_CREATE: case SYS_SHM_ATTACH:
        return COS_CAP_IPC;
    case SYS_AUDIO_OPEN: case SYS_AUDIO_WRITE: case SYS_AUDIO_STATUS: case SYS_AUDIO_DRAIN: case SYS_AUDIO_STOP:
    case SYS_AUDIO_VOLUME: case SYS_AUDIO_CLOSE:
        return COS_CAP_AUDIO;
    case SYS_SETTINGS_GET: case SYS_SETTINGS_SET:
        return COS_CAP_SETTINGS;
    case SYS_SETTINGS_ACTION:                                        /* actions 1..3 are sleep / restart / shut down; 5 opens a terminal (a program launcher) */
        if (r->rdi >= 1 && r->rdi <= 3) return COS_CAP_POWER;
        if (r->rdi == 5) return COS_CAP_SETTINGS | COS_CAP_SPAWN;
        if (r->rdi >= 7 && r->rdi <= 9) return COS_CAP_SETTINGS | COS_CAP_POWER;   /* ask for a format of the data volume (done at the next start) / cancel it */
        return COS_CAP_SETTINGS;
    case SYS_PASSWORD_CHANGE:
        return COS_CAP_ACCOUNT;
    case SYS_DEVICES_LIST:
        return COS_CAP_DEVICES;
    case SYS_SYSTEM_INFO: case SYS_PROCLIST: case SYS_KMOD_LIST: case SYS_CPU_STATS:
        return COS_CAP_SYSINFO;
    case SYS_SHELL_CREATE: case SYS_WIN_LIST: case SYS_WIN_CTL: case SYS_SHELL_ICONS: case SYS_DESKTOP_LIST: case SYS_DESKTOP_CTL: case SYS_NOTIF_LIST: case SYS_CTX_LIST:
        return COS_CAP_SHELL;
    case SYS_NOTIF_SEND:
        return COS_CAP_WINDOW;
    case SYS_PAR_INFO: case SYS_PAR_START: case SYS_PAR_POLL: case SYS_PAR_STOP: case SYS_PAR_FREE:
        return COS_CAP_COMPUTE;
    case SYS_WIN_CREATE: case SYS_WIN_FILL: case SYS_WIN_CLEAR: case SYS_WIN_DRAW_TEXT: case SYS_WIN_POLL_KEY:
    case SYS_WIN_POLL_MOUSE: case SYS_WIN2_CREATE: case SYS_WIN2_PRESENT: case SYS_WIN2_POLL: case SYS_WIN2_WAIT:
    case SYS_WIN2_SET_TITLE: case SYS_WIN2_CLOSE: case SYS_WIN2_SCREEN: case SYS_WIN2_GET_PATH: case SYS_WIN2_VIDEO:
    case SYS_WIN2_VIDEO_YUV420: case SYS_WIN2_VIDEO_MAP: case SYS_WIN2_VIDEO_COMMIT: case SYS_WIN2_PRESENT_RECTS:
    case SYS_WIN2_INPUT: case SYS_WIN2_GRAB: case SYS_WIN2_FULLSCREEN: case SYS_WIN2_RESIZE: case SYS_FILE_DROP: case SYS_DRAG_GHOST:
    case SYS_LXSHM_MAP: case SYS_UI_PREFS: case SYS_CLIP_SET: case SYS_CLIP_GET: case SYS_IMAGE_DECODE:
        return COS_CAP_WINDOW;
    default:
        { extern uint32_t cos_kmod_syscall_caps(uint64_t nr); if (nr >= 0x1000 && nr < 0x1040) return cos_kmod_syscall_caps(nr); }      /* a call a kernel module provides: its own capability list */
        return COS_CAP_UNKNOWN;
    }
}

static void syscall_handler_impl(struct regs* r) {
    {
        process_t *gp = process_get_current();
        if (gp && gp->type == TASK_TYPE_USER && gp->caps != COS_CAP_ALL) {
            uint32_t need = cos_syscall_needs(r->rax, r);
            /* a call nobody classified (COS_CAP_UNKNOWN) is for a fully privileged process only: a mask that merely keeps bit 31 along with others does not count */
            if ((need & ~gp->caps) || (need & COS_CAP_UNKNOWN)) {
                cos_kmod_event(COSYS_EV_SANDBOX_DENY, gp->pid, r->rax, need & ~gp->caps);
                serial_puts("[SANDBOX] syscall ");
                serial_putdec(r->rax);
                serial_puts(" denied for pid ");
                serial_putdec(gp->pid);
                serial_puts("\n");
                r->rax = (uint64_t)-1;
                return;
            }
        }
    }
    switch (r->rax) {
        case SYS_WRITE: {
            const char* buf = (const char*)(uintptr_t)r->rdi;
            uint64_t len = r->rsi;
            if (!buf) { r->rax = (uint64_t)-1; break; }
            if (len > SYSCALL_WRITE_MAX) len = SYSCALL_WRITE_MAX;

            /* Validate BEFORE dereferencing. r->rdi is fully attacker-
             * controlled: this handler runs at ring0 but on the caller's
             * address space, so without this check a ring3 program could
             * pass a kernel address and have the kernel dump kernel memory
             * to the serial console, or pass an unmapped address and take
             * a page fault inside an interrupt handler. The check covers
             * the clamped length actually used, not the requested one. */
            cos_syscall_touch_user_range((uint64_t)(uintptr_t)buf, len, false);
            if (!paging_user_range_ok((uint64_t)(uintptr_t)buf, len, false)) {
                serial_puts("[SYSCALL] SYS_WRITE rejected: buffer not in a "
                            "valid user mapping\n");
                r->rax = (uint64_t)-1;
                break;
            }

            uint64_t written = 0;
            for (uint64_t i = 0; i < len && buf[i]; ++i) {
                serial_putc(buf[i]);
                ++written;
            }
            /* Report bytes actually written, not the clamped request:
             * the old code returned `len` even when it stopped early at a
             * NUL, which would mislead any caller that used the result to
             * advance a buffer. */
            r->rax = written;
            break;
        }
        case SYS_WIN_CREATE: {
            /* rdi = title ptr (user), rsi = title len, rdx = w, r10 = h */
            const char *utitle = (const char *)(uintptr_t)r->rdi;
            uint64_t tlen = r->rsi;
            char title[SYSCALL_TITLE_MAX];
            title[0] = '\0';

            if (utitle && tlen) {
                if (tlen > SYSCALL_TITLE_MAX - 1) tlen = SYSCALL_TITLE_MAX - 1;
                /* Same rule as SYS_WRITE: validate before dereferencing.
                 * The title is copied into kernel memory immediately so
                 * the GUI never holds a pointer into a user address space
                 * that may be torn down or remapped underneath it. */
                cos_syscall_touch_user_range((uint64_t)(uintptr_t)utitle, tlen, false);
                if (!paging_user_range_ok((uint64_t)(uintptr_t)utitle, tlen, false)) {
                    serial_puts("[SYSCALL] SYS_WIN_CREATE rejected: bad title pointer\n");
                    r->rax = (uint64_t)-1;
                    break;
                }
                for (uint64_t i = 0; i < tlen; ++i) {
                    char c = utitle[i];
                    /* Strip control characters: this string goes straight
                     * into a title bar renderer. */
                    title[i] = (c >= 32 && c < 127) ? c : '?';
                }
                title[tlen] = '\0';
            }

            uint32_t pid = cos_syscall_caller_pid();
            r->rax = (uint64_t)cos_app_window_create(title,
                        (int32_t)(int64_t)r->rdx, (int32_t)(int64_t)r->r10, pid);
            break;
        }

        case SYS_WIN_FILL: {
            /* rdi = handle, rsi = x, rdx = y, r10 = w, r8 = h, r9 = colour.
             * All coordinates are clamped against the window's client area
             * inside cos_app_window_draw(), not trusted here. */
            uint32_t pid = cos_syscall_caller_pid();
            bool ok = cos_app_window_fill_rect((int64_t)r->rdi,
                        (int32_t)(int64_t)r->rsi, (int32_t)(int64_t)r->rdx,
                        (int32_t)(int64_t)r->r10, (int32_t)(int64_t)r->r8,
                        (uint32_t)r->r9, pid);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_WIN_CLEAR: {
            uint32_t pid = cos_syscall_caller_pid();
            cos_app_window_clear((int64_t)r->rdi, pid);
            r->rax = 0;
            break;
        }

        case SYS_BRK: {
            /* rdi = requested new break, or 0 to query the current one.
             * Returns the resulting break, or (uint64_t)-1 on failure -
             * note this deliberately does NOT follow Linux's "return the
             * old break on failure" convention, because that silently
             * looks like success to a caller that does not compare the
             * result against what it asked for. An explicit error value
             * is harder to misuse.
             *
             * Pages are not mapped here: the heap region already has
             * demand paging (task.c's fault handler maps any fault inside
             * [heap_start, heap_end) on first touch), so moving the break
             * is purely a bookkeeping change and an unused allocation
             * costs no physical memory. */
            process_t *proc = process_get_current();
            if (!proc || proc->heap_start == 0) { r->rax = (uint64_t)-1; break; }

            uint64_t requested = r->rdi;
            if (requested == 0) { r->rax = proc->heap_end; break; }

            /* Never shrink below the start, never grow past the cap, and
             * never let the heap run into the stack region. */
            if (requested < proc->heap_start) { r->rax = (uint64_t)-1; break; }
            uint64_t max_end = proc->heap_limit ? proc->heap_limit : (proc->heap_start + COS_USER_HEAP_MAX);
            if (requested > max_end) { r->rax = (uint64_t)-1; break; }
            if (!cos_heap_growth_is_safe(proc, requested)) {
                r->rax = (uint64_t)-1; break;
            }

            uint64_t new_end = (requested + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            /* Shrinking: release the physical pages that are no longer in
             * the heap, or a program that repeatedly grows and shrinks
             * would leak every page it ever touched. */
            if (new_end < proc->heap_end) {
                uint64_t pages = (proc->heap_end - new_end) / PAGE_SIZE;
                if (pages) paging_release_user_range(new_end, pages);   /* user range: never enters the kernel's free list */
            }
            proc->heap_end = new_end;
            r->rax = new_end;
            break;
        }

        case SYS_SBRK: {
            /* rdi = signed increment. Returns the PREVIOUS break (the
             * start of the newly-allocated region), which is what malloc
             * implementations expect, or (uint64_t)-1 on failure. */
            process_t *proc = process_get_current();
            if (!proc || proc->heap_start == 0) { r->rax = (uint64_t)-1; break; }

            int64_t delta = (int64_t)r->rdi;
            uint64_t old_end = proc->heap_end;
            if (delta == 0) { r->rax = old_end; break; }

            uint64_t target;
            if (delta > 0) {
                /* Overflow check before adding. */
                if (old_end + (uint64_t)delta < old_end) { r->rax = (uint64_t)-1; break; }
                target = old_end + (uint64_t)delta;
            } else {
                uint64_t shrink = (uint64_t)(-delta);
                if (shrink > old_end - proc->heap_start) { r->rax = (uint64_t)-1; break; }
                target = old_end - shrink;
            }

            uint64_t max_end = proc->heap_limit ? proc->heap_limit : (proc->heap_start + COS_USER_HEAP_MAX);
            if (target > max_end) { r->rax = (uint64_t)-1; break; }
            if (!cos_heap_growth_is_safe(proc, target)) {
                r->rax = (uint64_t)-1; break;
            }

            uint64_t new_end = (target + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
            if (new_end < proc->heap_end) {
                uint64_t pages = (proc->heap_end - new_end) / PAGE_SIZE;
                if (pages) paging_release_user_range(new_end, pages);   /* user range: never enters the kernel's free list */
            }
            proc->heap_end = new_end;
            r->rax = old_end;
            break;
        }

        case SYS_READ_FILE: {
            /* rdi = path (user), rsi = buffer (user), rdx = buffer size.
             * Returns bytes read, or (uint64_t)-1. */
            uint64_t upath = r->rdi, ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }

            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), upath)) {
                r->rax = (uint64_t)-1; break;
            }
            if (!cos_fs_gate(COSYS_FSOP_READ, path, NULL)) { r->rax = (uint64_t)-1; break; }
            /* The destination must be writable user memory for its whole
             * length, checked before the filesystem writes a single byte
             * into it - otherwise a bad pointer would be discovered only
             * partway through, after the kernel had already scribbled on
             * whatever was there. */
            cos_syscall_touch_user_range(ubuf, ulen, true);
            if (!paging_user_range_ok(ubuf, ulen, true)) {
                serial_puts("[SYSCALL] SYS_READ_FILE rejected: bad destination buffer\n");
                r->rax = (uint64_t)-1; break;
            }

            /* Read into a kernel bounce buffer rather than straight into
             * user memory: FatFs runs with the filesystem lock held and
             * can block, and the user mapping must not be relied on to
             * stay put across that. The copy out afterwards is a plain
             * memcpy into memory already validated above. */
            void *bounce = kmalloc((size_t)ulen);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            int got = cos_fs_read_file(path, bounce, ulen);
            if (got < 0) { kfree(bounce); r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)ubuf, bounce, (size_t)got);
            kfree(bounce);
            r->rax = (uint64_t)got;
            break;
        }

        case SYS_WRITE_FILE: {
            /* rdi = path (user), rsi = data (user), rdx = length.
             * Returns bytes written, or (uint64_t)-1. */
            uint64_t upath = r->rdi, ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }

            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), upath)) {
                r->rax = (uint64_t)-1; break;
            }
            if (!cos_fs_gate(COSYS_FSOP_WRITE, path, NULL)) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(ubuf, ulen, false);
            if (!paging_user_range_ok(ubuf, ulen, false)) {
                serial_puts("[SYSCALL] SYS_WRITE_FILE rejected: bad source buffer\n");
                r->rax = (uint64_t)-1; break;
            }

            void *bounce = kmalloc((size_t)ulen);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            memcpy(bounce, (const void *)(uintptr_t)ubuf, (size_t)ulen);
            int written = cos_fs_write_file(path, bounce, ulen);
            kfree(bounce);
            r->rax = (written < 0) ? (uint64_t)-1 : (uint64_t)written;
            break;
        }

        case SYS_YIELD: {
            scheduler_yield();
            r->rax = 0;
            break;
        }

        case SYS_SLEEP_MS: {
            /* rdi = milliseconds. Capped so a bad value cannot park a
             * process effectively forever with no way back. */
            uint64_t ms = r->rdi;
            if (ms > COS_SYSCALL_SLEEP_MAX_MS) ms = COS_SYSCALL_SLEEP_MAX_MS;
            if (ms == 0) {
                scheduler_yield();
            } else {
                scheduler_sleep(ms);
            }
            r->rax = 0;
            break;
        }

        case SYS_GETPID: {
            process_t *proc = process_get_current();
            r->rax = proc ? (uint64_t)proc->pid : (uint64_t)-1;
            break;
        }

        case SYS_SPAWN: {
            /* rdi = path to a .c-os file (user pointer).
             * Returns the new pid, or (uint64_t)-1.
             *
             * The image is read and launched entirely inside the kernel
             * via the same path the file manager's double-click uses, so
             * every validation the loader performs (ELF sanity, segment
             * overlap, user-half addresses, W^X) applies identically to a
             * program started by another program. */
            { process_t *sp_ = process_get_current();         /* a confined process may keep at most 8 children (finished ones count until waited for) */
              if (sp_ && sp_->caps != COS_CAP_ALL && process_count_children(sp_->pid) >= 8) { r->rax = (uint64_t)-1; break; } }
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = (uint64_t)cos_spawn_elf_path(path);
            break;
        }

        case SYS_WAITPID: {
            /* rdi = pid to wait for (<=0 means any child),
             * rsi = optional user pointer to receive the exit status,
             * rdx = 0 means WNOHANG (check once, do not block), nonzero
             * means block until a matching child exits.
             *
             * REPLACES an earlier polling implementation that woke up
             * every COS_WAIT_POLL_MS (10ms) via scheduler_sleep() to
             * re-check, whether or not anything had changed - burning
             * scheduler cycles the whole time a child was running and
             * adding up to 10ms of pure latency after it actually exited.
             * This blocks the calling thread properly: it is taken off
             * the run queue entirely (scheduler_sleep_on(), a genuine
             * wait_queue_t block, not a timer) and is only put back on
             * the run queue by process_exit()'s explicit wake of this
             * process's child_wait_queue at the moment a child actually
             * becomes a zombie - see the field comment on
             * process_t::child_wait_queue. No polling interval, no
             * latency floor.
             *
             * This also drops the arbitrary timeout the polling version
             * had (COS_SYSCALL_SLEEP_MAX_MS) in favour of matching real
             * POSIX waitpid, which has no timeout parameter either - a
             * caller wanting a bounded wait uses a signal (already
             * implemented this session) or WNOHANG plus its own retry
             * policy, not a kernel-enforced deadline baked into the
             * syscall itself. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }

            uint64_t status_ptr = r->rsi;
            if (status_ptr != 0) cos_syscall_touch_user_range(status_ptr, sizeof(int32_t), true);
            if (status_ptr != 0 &&
                !paging_user_range_ok(status_ptr, sizeof(int32_t), true)) {
                serial_puts("[SYSCALL] SYS_WAITPID rejected: bad status pointer\n");
                r->rax = (uint64_t)-1; break;
            }

            bool wnohang = (r->rdx == 0);
            int64_t got = -1;
            int status = 0;
            for (;;) {
                got = process_try_reap_child(proc->pid, (int64_t)r->rdi, &status);
                if (got > 0) break;              /* collected */
                if (got < 0) break;              /* no such child, ever */
                if (wnohang) { got = -1; break; } /* not ready, caller asked not to block */
                /* Blocks here - does not return to this point until
                 * process_exit() (on some future child) wakes
                 * proc->child_wait_queue and the scheduler reselects this
                 * thread. Loops back to re-check afterward rather than
                 * assuming the wake was for the exact child being waited
                 * on: with rdi<=0 (any child) the first exit to arrive is
                 * always right, but with a specific rdi a DIFFERENT
                 * sibling's exit can spuriously wake this thread too
                 * (wait_queue_wake_all() wakes every waiter on the
                 * parent's queue, not just ones whose target already
                 * matches), and re-checking is what makes that safe
                 * rather than returning the wrong child or a stale
                 * status. */
                scheduler_sleep_on(&proc->child_wait_queue);
            }

            /* the wait may have blocked: another thread can have unmapped the buffer since it was checked, so check again before writing */
            if (got > 0 && status_ptr != 0) cos_syscall_touch_user_range(status_ptr, sizeof(int32_t), true);
            if (got > 0 && status_ptr != 0 && paging_user_range_ok(status_ptr, sizeof(int32_t), true)) {
                *(int32_t *)(uintptr_t)status_ptr = (int32_t)status;
            }
            r->rax = (got > 0) ? (uint64_t)got : (uint64_t)-1;
            break;
        }

        case SYS_DLOPEN: {
            /* rdi = path to a .c-osll. Returns a handle (>0) or -1. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            process_t *proc = process_get_current();
            /* RTLD_GLOBAL to preserve the old behaviour exactly: the
             * previous implementation had no concept of a local scope, so
             * every library it loaded was globally visible. Changing that
             * silently would break a program that relied on one library
             * seeing another's symbols. */
            r->rax = (uint64_t)(proc ? cos_link_dlopen(proc, path, COS_RTLD_GLOBAL)
                                     : -1);
            break;
        }

        case SYS_DLSYM: {
            /* rdi = handle from SYS_DLOPEN, rsi = symbol name (user).
             * Returns the symbol's address, or 0 (not -1: an address of 0
             * is already the natural "no such symbol" value, and callers
             * test pointers against 0). */
            char name[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(name, sizeof(name), r->rsi)) {
                r->rax = 0; break;
            }
            process_t *proc = process_get_current();
            r->rax = proc ? cos_link_dlsym(proc, (int64_t)r->rdi, name) : 0;
            break;
        }

        case SYS_DLOPEN2: {
            /* rdi = path, rsi = COS_RTLD_* flags. The flags version: the
             * original SYS_DLOPEN cannot say RTLD_LOCAL, so every library
             * necessarily joined the process-wide symbol scope and could
             * interpose on names it had no business capturing. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            process_t *proc = process_get_current();
            r->rax = (uint64_t)(proc ? cos_link_dlopen(proc, path, (uint32_t)r->rsi)
                                     : -1);
            break;
        }

        case SYS_PAR_INFO:
            r->rax = smp_user_cpu_count();
            break;

        case SYS_PAR_START: {
            process_t *proc = process_get_current();
            if (!proc || !proc->page_dir) { r->rax = (uint64_t)-1; break; }
            if (!paging_user_range_ok(r->rdi, 40, false)) { r->rax = (uint64_t)-14; break; }
            const uint64_t *d = (const uint64_t *)(uintptr_t)r->rdi;
            uint64_t entry = d[0], arg = d[1], top = d[2], stub = d[3], fsb = d[4];
            if (!paging_user_range_ok(entry, 1, false) || !paging_user_range_ok(stub, 1, false) ||
                top < 64 || !paging_user_range_ok(top - 64, 64, true) || fsb >= (1ULL << 47)) {
                r->rax = (uint64_t)-14; break;
            }
            r->rax = (uint64_t)(int64_t)smp_user_start(proc, (uint64_t)(uintptr_t)proc->page_dir,
                                                       entry, arg, top, stub, fsb);
            break;
        }

        case SYS_PAR_POLL: {
            process_t *proc = process_get_current();
            smp_user_job_t snap;
            int st = proc ? smp_user_poll(proc, (int)r->rdi, &snap) : -1;
            if (st >= 0 && r->rsi) {
                if (paging_user_range_ok(r->rsi, 32, true)) {
                    uint64_t *o = (uint64_t *)(uintptr_t)r->rsi;
                    o[0] = snap.fault_vec; o[1] = snap.fault_err; o[2] = snap.fault_rip; o[3] = snap.fault_cr2;
                }
            }
            r->rax = (uint64_t)(int64_t)st;
            break;
        }

        case SYS_PAR_STOP: {
            process_t *proc = process_get_current();
            r->rax = (uint64_t)(int64_t)(proc ? smp_user_stop(proc, (int)r->rdi) : -1);
            break;
        }

        case SYS_PAR_FREE: {
            process_t *proc = process_get_current();
            r->rax = (uint64_t)(int64_t)(proc ? smp_user_free(proc, (int)r->rdi) : -1);
            break;
        }

        case SYS_FUTEX_WAIT:
            /* The address is validated inside cos_futex_wait(), which
             * must re-read the word with interrupts disabled anyway -
             * checking it here as well would be a TOCTOU window, not
             * extra safety. */
            r->rax = (uint64_t)cos_futex_wait(r->rdi, (uint32_t)r->rsi, r->rdx);
            break;

        case SYS_FUTEX_WAKE:
            r->rax = (uint64_t)cos_futex_wake(r->rdi, (uint32_t)r->rsi);
            break;

        case SYS_GETCPU:
            r->rax = cpu_id();
            break;

        case SYS_CPU_STATS: {
            int64_t n = (int64_t)smp_online_cpu_count(), mx = (int64_t)r->rsi;
            if (n > 64) n = 64;
            if (mx < n) n = mx;
            if (n < 0 || (n && (!r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)n, true)))) { r->rax = (uint64_t)-1; break; }
            extern uint32_t scheduler_cpu_load(uint32_t cpu);
            uint8_t tmp[64];
            for (int64_t i = 0; i < n; ++i) { uint32_t v = scheduler_cpu_load((uint32_t)i); tmp[i] = (uint8_t)(v > 100 ? 100 : v); }
            if (n) memcpy((void *)(uintptr_t)r->rdi, tmp, (size_t)n);
            r->rax = (uint64_t)n;
            break;
        }

        case SYS_GETTID: {
            thread_t *th = thread_get_current();
            r->rax = th ? th->tid : 0;
            break;
        }

        case SYS_SPAWN2: {
            /* rdi = path, rsi = argv (NULL-terminated array of user
             * pointers), rdx = envp (same, may be NULL).
             *
             * Both vectors are copied into the kernel BEFORE the child's
             * address space is built. They live in the PARENT's address
             * space, and the launcher switches to the child's to write
             * the stack - so a pointer read after that switch would
             * resolve against the wrong address space entirely. */
            { process_t *sp_ = process_get_current();
              if (sp_ && sp_->caps != COS_CAP_ALL && process_count_children(sp_->pid) >= 8) { r->rax = (uint64_t)-1; break; } }
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }

            /* Per call, not static: two threads (or CPUs) spawning at once must not share argument storage, and nothing of one caller's strings may be
             * left behind for the next. */
            char (*argbuf)[COS_SPAWN_MAX_ARGLEN] = (char (*)[COS_SPAWN_MAX_ARGLEN])kmalloc(2u * COS_SPAWN_MAX_ARGS * COS_SPAWN_MAX_ARGLEN);
            if (!argbuf) { r->rax = (uint64_t)-1; break; }
            char (*envbuf)[COS_SPAWN_MAX_ARGLEN] = argbuf + COS_SPAWN_MAX_ARGS;
            memset(argbuf, 0, 2u * COS_SPAWN_MAX_ARGS * COS_SPAWN_MAX_ARGLEN);
            const char *argv[COS_SPAWN_MAX_ARGS + 1];
            const char *envp[COS_SPAWN_MAX_ARGS + 1];

            int argc = cos_syscall_copy_strvec(r->rsi, argbuf, argv, COS_SPAWN_MAX_ARGS);
            int envc = cos_syscall_copy_strvec(r->rdx, envbuf, envp, COS_SPAWN_MAX_ARGS);
            if (argc < 0 || envc < 0) {
                serial_puts("[SYSCALL] SYS_SPAWN2 rejected: bad argv/envp\n");
                kfree(argbuf);
                r->rax = (uint64_t)-1; break;
            }
            /* argv[0] defaults to the program path, which is what a
             * program reads to identify itself. */
            if (argc == 0) { argv[0] = path; argc = 1; }

            r->rax = (uint64_t)cos_spawn_elf_path_args(path, argv, argc,
                                                       envc ? envp : NULL, envc);
            kfree(argbuf);
            break;
        }

        /* ---- IPC ----------------------------------------------------
         * See the SYS_IPC_* defines above for why these exist. Each one
         * validates every user pointer with paging_user_range_ok()
         * before dereferencing it: these calls cross a trust boundary,
         * and a server that a malformed client request can crash is not
         * isolation, just indirection. */
        case SYS_IPC_SEND: {
            if (!cos_ipc_dst_ok(r->rdi)) { r->rax = (uint64_t)(int64_t)IPC_ERROR_PERMISSION_DENIED; break; }
            uint64_t len = r->r10;
            if (len > IPC_MAX_DATA_SIZE) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_DATA_TOO_LARGE; break;
            }
            uint8_t buf[IPC_MAX_DATA_SIZE];
            if (len != 0) {
                if (!r->rdx || !paging_user_range_ok(r->rdx, len, false)) {
                    r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
                }
                memcpy(buf, (const void *)(uintptr_t)r->rdx, (size_t)len);
            }
            r->rax = (uint64_t)(int64_t)ipc_send(r->rdi, r->rsi,
                                                 len ? buf : NULL, len);
            break;
        }

        case SYS_IPC_RECV: {
            /* rdi points at a 4-entry u64 header the kernel fills in:
             * [0]=src_pid, [1]=msg_type, [2]=payload length actually
             * written, [3]=reserved (0). Returning the length through
             * the header rather than through rax keeps rax free to carry
             * the IPC_* status, so "received a zero-length message" and
             * "timed out" stay distinguishable - collapsing those two is
             * exactly the kind of ambiguity that makes a server loop
             * spin. */
            if (!r->rdi || !paging_user_range_ok(r->rdi, 4 * sizeof(uint64_t), true)) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
            }
            uint64_t cap = r->rdx;
            if (cap > IPC_MAX_DATA_SIZE) cap = IPC_MAX_DATA_SIZE;
            if (cap != 0 && (!r->rsi || !paging_user_range_ok(r->rsi, cap, true))) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
            }

            uint8_t buf[IPC_MAX_DATA_SIZE];
            uint64_t src_pid = 0, msg_type = 0, data_size = cap;
            int rc = ipc_receive(&src_pid, &msg_type, cap ? buf : NULL,
                                 &data_size, r->r10);
            /* ipc_receive may have blocked: check both user buffers again before writing into them */
            if (rc == IPC_SUCCESS) {
                cos_syscall_touch_user_range(r->rdi, 4 * sizeof(uint64_t), true);
                if (cap != 0) cos_syscall_touch_user_range(r->rsi, cap, true);
            }
            if (rc == IPC_SUCCESS &&
                (!paging_user_range_ok(r->rdi, 4 * sizeof(uint64_t), true) ||
                 (cap != 0 && !paging_user_range_ok(r->rsi, cap, true)))) {
                rc = IPC_ERROR_INVALID_PID;
            }
            if (rc == IPC_SUCCESS) {
                if (data_size > cap) data_size = cap;   /* never overrun the caller */
                if (data_size != 0) {
                    memcpy((void *)(uintptr_t)r->rsi, buf, (size_t)data_size);
                }
                uint64_t *hdr = (uint64_t *)(uintptr_t)r->rdi;
                hdr[0] = src_pid; hdr[1] = msg_type; hdr[2] = data_size; hdr[3] = 0;
            }
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_IPC_RESPOND: {
            if (!cos_ipc_dst_ok(r->rdi)) { r->rax = (uint64_t)(int64_t)IPC_ERROR_PERMISSION_DENIED; break; }
            uint64_t len = r->r8;
            if (len > IPC_MAX_DATA_SIZE) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_DATA_TOO_LARGE; break;
            }
            uint8_t buf[IPC_MAX_DATA_SIZE];
            if (len != 0) {
                if (!r->r10 || !paging_user_range_ok(r->r10, len, false)) {
                    r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
                }
                memcpy(buf, (const void *)(uintptr_t)r->r10, (size_t)len);
            }
            r->rax = (uint64_t)(int64_t)ipc_respond(r->rdi, r->rsi, r->rdx,
                                                    len ? buf : NULL, len);
            break;
        }

        case SYS_IPC_NOTIFY: {
            if (!cos_ipc_dst_ok(r->rdi)) { r->rax = (uint64_t)(int64_t)IPC_ERROR_PERMISSION_DENIED; break; }
            uint64_t len = r->r10;
            if (len > IPC_MAX_DATA_SIZE) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_DATA_TOO_LARGE; break;
            }
            uint8_t buf[IPC_MAX_DATA_SIZE];
            if (len != 0) {
                if (!r->rdx || !paging_user_range_ok(r->rdx, len, false)) {
                    r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
                }
                memcpy(buf, (const void *)(uintptr_t)r->rdx, (size_t)len);
            }
            r->rax = (uint64_t)(int64_t)ipc_notify(r->rdi, r->rsi,
                                                   len ? buf : NULL, len);
            break;
        }

        case SYS_SHM_CREATE: {
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(uint64_t), true)) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
            }
            uint64_t shm_id = 0;
            int rc = ipc_request_shared_memory(r->rdi, &shm_id);
            if (rc == IPC_SUCCESS) {
                *(uint64_t *)(uintptr_t)r->rsi = shm_id;
            }
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_SHM_ATTACH: {
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(uint64_t), true)) {
                r->rax = (uint64_t)(int64_t)IPC_ERROR_INVALID_PID; break;
            }
            uint64_t addr = 0;
            int rc = ipc_attach_shared_memory(r->rdi, &addr);
            if (rc == IPC_SUCCESS) {
                *(uint64_t *)(uintptr_t)r->rsi = addr;
            }
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_DLCLOSE: {
            process_t *proc = process_get_current();
            r->rax = (uint64_t)(int64_t)(proc ? cos_link_dlclose(proc, (int64_t)r->rdi)
                                              : -1);
            break;
        }

        case SYS_DLCTL: {
            /* rdi = op, rsi = a, rdx = b, r10 = c. Strings and result blocks are copied here; cos_link_dlctl() only sees kernel memory. */
            process_t *proc = process_get_current();
            int64_t rc = -1;
            if (!proc) { r->rax = (uint64_t)-1; break; }
            switch (r->rdi) {
            case COS_DLCTL_PLAN_CLOSE: {
                uint32_t ser[24];
                if (!r->rdx || !paging_user_range_ok(r->rdx, sizeof ser, true)) break;
                rc = cos_link_dlctl(proc, COS_DLCTL_PLAN_CLOSE, r->rsi, (uint64_t)(uintptr_t)ser);
                if (rc > 0) memcpy((void *)(uintptr_t)r->rdx, ser, (size_t)rc * sizeof(uint32_t));
                break;
            }
            case COS_DLCTL_ADDR: {
                cos_dlinfo_t info;
                if (!r->rdx || !paging_user_range_ok(r->rdx, sizeof info, true)) break;
                rc = cos_link_dlctl(proc, COS_DLCTL_ADDR, r->rsi, (uint64_t)(uintptr_t)&info);
                if (rc == 0) memcpy((void *)(uintptr_t)r->rdx, &info, sizeof info);
                break;
            }
            case COS_DLCTL_NEXT: {
                char name[COS_SYSCALL_PATH_MAX];
                if (!cos_syscall_copy_path(name, sizeof(name), r->rdx)) break;
                rc = cos_link_dlctl(proc, COS_DLCTL_NEXT, r->rsi, (uint64_t)(uintptr_t)name);
                break;
            }
            case COS_DLCTL_ERROR: {
                char msg[160];
                uint64_t cap = r->rdx;
                if (!r->rsi || cap == 0 || cap > 4096 || !paging_user_range_ok(r->rsi, cap, true)) break;
                rc = cos_link_dlctl(proc, COS_DLCTL_ERROR, 0, (uint64_t)(uintptr_t)msg);
                if (rc >= 0) { uint64_t n = (uint64_t)rc + 1 < cap ? (uint64_t)rc + 1 : cap; memcpy((void *)(uintptr_t)r->rsi, msg, (size_t)n); ((char *)(uintptr_t)r->rsi)[n - 1] = '\0'; }
                break;
            }
            case COS_DLCTL_SET_PATH: {
                char path[COS_SYSCALL_PATH_MAX];
                if (!cos_syscall_copy_path(path, sizeof(path), r->rsi)) break;
                rc = cos_link_dlctl(proc, COS_DLCTL_SET_PATH, 0, (uint64_t)(uintptr_t)path);
                break;
            }
            default: break;
            }
            r->rax = (uint64_t)rc;
            break;
        }

        case SYS_DL_INFO: {
            /* Address of the read-only link map, or 0 if this process was
             * not started through the dynamic linker. */
            process_t *proc = process_get_current();
            const cos_link_ns_t *ns = proc ? cos_link_ns_peek(proc->pid) : NULL;
            r->rax = ns ? ns->linkmap_addr : 0;
            break;
        }

        case SYS_SET_TLS: {
            /* rdi = the thread pointer to install as this thread's %fs
             * base.
             *
             * Validated, not trusted: %fs.base is the origin every
             * `%fs:offset` access in ring3 is relative to, so a base in
             * the kernel half would let a ring3 instruction address
             * kernel memory directly, with no syscall involved and
             * nothing to check it. It must be a mapped, writable USER
             * address - and the ABI requires *(void**)tp == tp, so the
             * first word has to be readable too. */
            uint64_t tp = r->rdi;
            thread_t *th = thread_get_current();
            if (!th) { r->rax = (uint64_t)-1; break; }
            if (tp == 0) {
                /* Clearing is legitimate: a thread that is done with TLS
                 * says so, and the switch path then skips it. */
                scheduler_set_thread_tls(th, 0);
                r->rax = 0; break;
            }
            if (tp >= COS_ELF_USER_LIMIT || (tp & 7u) != 0) {
                serial_puts("[SYSCALL] SYS_SET_TLS rejected: thread pointer is not "
                            "an aligned user address\n");
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(tp, 8, true);
            if (!paging_user_range_ok(tp, 8, true)) {
                serial_puts("[SYSCALL] SYS_SET_TLS rejected: thread pointer is not "
                            "mapped writable\n");
                r->rax = (uint64_t)-1; break;
            }
            scheduler_set_thread_tls(th, tp);
            r->rax = 0;
            break;
        }

        case SYS_GET_TLS: {
            thread_t *th = thread_get_current();
            r->rax = th ? th->fs_base : 0;
            break;
        }

        case SYS_FD_OPEN: {
            /* rdi = path (user), rsi = COS_O_* flags. Returns fd, or -1. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            if (!cos_fs_gate(r->rsi == COS_O_RDONLY ? COSYS_FSOP_READ : COSYS_FSOP_WRITE, path, NULL)) { r->rax = (uint64_t)-1; break; }
            r->rax = (uint64_t)cos_fd_open(cos_syscall_caller_pid(), path, (uint32_t)r->rsi);
            break;
        }

        case SYS_FD_OPENDIR: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = (uint64_t)cos_fd_opendir(cos_syscall_caller_pid(), path);
            break;
        }

        case SYS_FD_READ: {
            /* rdi = fd, rsi = user buffer, rdx = length. Returns bytes
             * read (0 at EOF), or -1. Bounded to COS_FD_IO_CHUNK_MAX
             * inside cos_fd_read() itself - a short count is a normal,
             * expected outcome here, exactly like a real read(). */
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0) { r->rax = 0; break; }
            uint64_t chunk = ulen > COS_SYSCALL_FILE_MAX ? COS_SYSCALL_FILE_MAX : ulen;
            cos_syscall_touch_user_range(ubuf, chunk, true);
            if (!paging_user_range_ok(ubuf, chunk, true)) {
                serial_puts("[SYSCALL] SYS_FD_READ rejected: bad destination buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            void *bounce = kmalloc((size_t)chunk);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            int64_t got = cos_fd_read(cos_syscall_caller_pid(), (int64_t)r->rdi, bounce, chunk);
            if (got > 0) memcpy((void *)(uintptr_t)ubuf, bounce, (size_t)got);
            kfree(bounce);
            r->rax = (uint64_t)got;
            break;
        }

        case SYS_FD_WRITE: {
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0) { r->rax = 0; break; }
            uint64_t chunk = ulen > COS_SYSCALL_FILE_MAX ? COS_SYSCALL_FILE_MAX : ulen;
            cos_syscall_touch_user_range(ubuf, chunk, false);
            if (!paging_user_range_ok(ubuf, chunk, false)) {
                serial_puts("[SYSCALL] SYS_FD_WRITE rejected: bad source buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            void *bounce = kmalloc((size_t)chunk);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            memcpy(bounce, (const void *)(uintptr_t)ubuf, (size_t)chunk);
            int64_t written = cos_fd_write(cos_syscall_caller_pid(), (int64_t)r->rdi, bounce, chunk);
            kfree(bounce);
            r->rax = (uint64_t)written;
            break;
        }

        case SYS_FD_LSEEK: {
            /* rdi = fd, rsi = absolute offset (no SEEK_CUR/SEEK_END yet -
             * see the header note). Returns the new offset, or -1. */
            r->rax = (uint64_t)cos_fd_lseek(cos_syscall_caller_pid(), (int64_t)r->rdi,
                                            (int64_t)r->rsi);
            break;
        }

        case SYS_FD_READDIR: {
            /* rdi = fd, rsi = user pointer to a cos_dirent_t.
             * Returns 1 (entry filled), 0 (end of directory), or -1. */
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_dirent_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_dirent_t), true)) {
                serial_puts("[SYSCALL] SYS_FD_READDIR rejected: bad dirent buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            cos_dirent_t entry;
            int64_t rc = cos_fd_readdir(cos_syscall_caller_pid(), (int64_t)r->rdi, &entry);
            if (rc == 1) memcpy((void *)(uintptr_t)r->rsi, &entry, sizeof(entry));
            r->rax = (uint64_t)rc;
            break;
        }

        case SYS_FD_CLOSE: {
            bool ok = cos_fd_close(cos_syscall_caller_pid(), (int64_t)r->rdi);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_SIGACTION: {
            /* rdi = signal number (1..31; 0 reserved/invalid, matching
             * real Unix convention that signal 0 is not a real signal),
             * rsi = handler address, or 0 for SIG_ACTION_DEFAULT, or 1
             * for SIG_ACTION_IGNORE (both sentinels already defined in
             * task.h, reused here so the meaning is defined in exactly
             * one place). Signal 9 is accepted here (a program is allowed
             * to WANT to catch it) but is still force-terminated at
             * delivery time in cos_deliver_pending_signal() - real Unix
             * SIGKILL also permits sigaction() to "succeed" while the
             * kernel ignores the registration when it matters. */
            process_t *proc = process_get_current();
            if (!proc || r->rdi == 0 || r->rdi >= MAX_SIGNALS) {
                r->rax = (uint64_t)-1; break;
            }
            if (r->rsi > 1 && r->rsi >= 0x0000800000000000ULL) { r->rax = (uint64_t)-1; break; }      /* a handler is a user address (or one of the two sentinels) */
            signal_set_handler(proc, (int)r->rdi, (signal_handler_t)(uintptr_t)r->rsi);
            r->rax = 0;
            break;
        }

        case SYS_KILL: {
            /* rdi = target pid, rsi = signal number. This OS is
             * single-user, so no permission check beyond "does this pid
             * exist" is applied - any process may signal any other. */
            if (r->rsi == 0 || r->rsi >= MAX_SIGNALS) { r->rax = (uint64_t)-1; break; }
            process_t *target = process_get_by_pid(r->rdi);
            if (!target) { r->rax = (uint64_t)-1; break; }
            {   /* A sandboxed process (it gave up some capability) may signal itself and its own descendants only: holding the spawn capability
                 * must not let it stop the desktop, the taskbar or another program. */
                process_t *me = process_get_current();
                if (me && me->type == TASK_TYPE_USER && me->caps != COS_CAP_ALL && target != me) {
                    bool mine = false; process_t *t = target;
                    for (int depth = 0; t && depth < 32 && !mine; depth++) { if (t->parent_pid == me->pid) mine = true; else t = process_get_by_pid(t->parent_pid); }
                    if (!mine) { serial_puts("[SANDBOX] kill denied for pid "); serial_putdec(me->pid); serial_puts(": not its own child\n"); r->rax = (uint64_t)-1; break; }
                }
            }
            r->rax = (signal_send(target, (int)r->rsi) == 0) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_SIGRETURN: {
            /* No rdi/rsi arguments: this is only ever reached via the
             * trampoline this same kernel wrote, immediately after the
             * signal handler's `ret` landed on it - see the design note
             * above cos_deliver_pending_signal(). RSP at this exact point
             * is trampoline_addr, and last_sigframe_addr (stashed at
             * delivery time) is where the original register state lives. */
            process_t *proc = process_get_current();
            cos_sigframe_t frame;
            if (!proc || proc->last_sigframe_addr == 0 ||
                !cos_sig_read_user(proc->last_sigframe_addr, &frame, sizeof(frame))) {
                serial_puts("[SIGNAL] SYS_SIGRETURN with no valid pending frame - "
                            "terminating process\n");
                if (proc) process_exit(proc, -1);
                for (;;) { __asm__ volatile("hlt"); }
            }
            proc->last_sigframe_addr = 0;
            /* The frame sits on the user stack, so the handler may have changed anything in it. What goes back into the interrupt frame must still be
             * a user context: user addresses, no I/O privilege level, interrupts on, no nested-task/virtual-8086/alignment-check bits. */
            if (frame.rip >= 0x0000800000000000ULL || frame.rsp >= 0x0000800000000000ULL) {
                serial_puts("[SIGNAL] SYS_SIGRETURN with a context outside user space - terminating process\n");
                process_exit(proc, -1);
                for (;;) { __asm__ volatile("hlt"); }
            }
            frame.rflags = (frame.rflags & 0xCD5ULL) | 0x202ULL;      /* CF PF AF ZF SF DF OF kept; IF set; IOPL, NT, RF, VM, AC, TF cleared */
            /* Restore the mask that was in effect BEFORE this signal was
             * delivered, undoing the "block everything but 9" applied at
             * delivery time - see the design note there. This is what
             * lets a signal that arrived and was correctly deferred while
             * this handler ran become deliverable again right away: the
             * fall-through to cos_deliver_pending_signal() at the end of
             * this same syscall (every syscall gets that check,
             * including this one) will now see it as no longer blocked. */
            proc->blocked_signals = frame.saved_blocked_signals;
            r->r15 = frame.r15; r->r14 = frame.r14; r->r13 = frame.r13; r->r12 = frame.r12;
            r->r11 = frame.r11; r->r10 = frame.r10; r->r9  = frame.r9;  r->r8  = frame.r8;
            r->rbp = frame.rbp; r->rdi = frame.rdi; r->rsi = frame.rsi; r->rdx = frame.rdx;
            r->rcx = frame.rcx; r->rbx = frame.rbx; r->rax = frame.rax;
            r->rip = frame.rip; r->rflags = frame.rflags; r->rsp = frame.rsp;
            /* Falls through to the normal end-of-syscall signal check
             * below with the ORIGINAL context restored, exactly as if the
             * interrupted syscall were the one returning - a second
             * signal that arrived while the first was being handled is
             * correctly picked up here rather than lost. */
            break;
        }

        case SYS_MMAP: {
            /* rdi = length, rsi = COS_PROT_* flags (READ implied always;
             * WRITE controls whether the mapping is genuinely writable -
             * see task_alloc_page()'s flag handling, fixed this session
             * to actually respect this rather than forcing every
             * demand-paged page writable regardless of what was asked).
             * Returns the mapped address, or (uint64_t)-1. No addr hint,
             * no fd/offset - anonymous-private only, see the design note
             * above cos_mmap_alloc(). */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            bool writable = (r->rsi & COS_PROT_WRITE) != 0;
            int64_t addr = cos_mmap_alloc(proc, r->rdi, writable);
            r->rax = (addr < 0) ? (uint64_t)-1 : (uint64_t)addr;
            break;
        }

        case SYS_MUNMAP: {
            /* rdi = addr, rsi = length. Must exactly match a previously
             * returned mmap() region (see cos_mmap_free()'s exact-match
             * note). Returns 0 or -1. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            bool ok = cos_mmap_free(proc, r->rdi, r->rsi);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_MPROTECT: {
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }

            uint32_t prot = (uint32_t)r->rdx;
            bool want_write = (prot & COS_PROT_WRITE) != 0;
            bool want_exec  = (prot & COS_PROT_EXEC)  != 0;

            /* W^X, enforced at the one place a caller can ever ask for
             * both: refuse outright rather than silently picking one
             * permission to honour and dropping the other, which would
             * leave the caller believing it got something it did not. */
            if (want_write && want_exec) {
                r->rax = (uint64_t)-1;
                break;
            }

            uint64_t length = r->rsi;
            /* rounding up must not wrap to 0 (an empty range inside a region would be taken for a hit), and the start must be page aligned */
            if (length == 0 || length > (uint64_t)1 << 40 || (r->rdi & (PAGE_SIZE - 1)) != 0) { r->rax = (uint64_t)-1; break; }
            uint64_t rounded = (length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

            /* The request may land anywhere WITHIN an existing region, not only exactly on
             * its bounds - tccrun.c's -run relocator is the reason this matters: it mmaps
             * one block for a whole relocated program image, then mprotects the code
             * portion to RX while leaving the rest of that same block RW, exactly the
             * "mmap once, protect a sub-range" pattern a real OS's mprotect (which works
             * on the page table directly, independent of how memory was obtained) allows
             * and this syscall originally did not. A containing region is split into up to
             * three: an unchanged prefix, the requested (now re-protected) middle, and an
             * unchanged suffix - real OS mprotect does the same VMA split internally. Only
             * the middle piece's flags actually change; a prefix/suffix keeps the
             * region's previous writable/executable exactly as they were. */
            int found = -1;
            for (int i = 0; i < COS_MMAP_MAX_REGIONS; ++i) {
                cos_mmap_region_t *reg = &proc->mmap_regions[i];
                if (!reg->used) continue;
                if (reg->start <= r->rdi && r->rdi + rounded <= reg->end) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                cos_mmap_region_t *reg = &proc->mmap_regions[found];
                uint64_t prefix = r->rdi - reg->start;
                uint64_t suffix = reg->end - (r->rdi + rounded);
                if (prefix > 0 || suffix > 0) {
                    /* Splitting needs a free slot per non-empty side, found and validated
                     * BEFORE anything is mutated - a request that cannot be fully honoured
                     * must change nothing, not leave the region table half-updated. */
                    int prefix_slot = -1, suffix_slot = -1;
                    for (int i = 0; i < COS_MMAP_MAX_REGIONS && (prefix > 0 && prefix_slot < 0); ++i)
                        if (!proc->mmap_regions[i].used && i != found) prefix_slot = i;
                    for (int i = 0; i < COS_MMAP_MAX_REGIONS && (suffix > 0 && suffix_slot < 0); ++i)
                        if (!proc->mmap_regions[i].used && i != found && i != prefix_slot) suffix_slot = i;
                    if ((prefix > 0 && prefix_slot < 0) || (suffix > 0 && suffix_slot < 0)) {
                        r->rax = (uint64_t)-1;   /* region table full: refuse rather than split partially */
                        break;
                    }
                    uint64_t old_start = reg->start, old_end = reg->end;
                    bool old_w = reg->writable, old_x = reg->executable;
                    if (prefix > 0) {
                        cos_mmap_region_t *p = &proc->mmap_regions[prefix_slot];
                        p->start = old_start; p->end = r->rdi; p->used = true; p->writable = old_w; p->executable = old_x;
                    }
                    if (suffix > 0) {
                        cos_mmap_region_t *sfx = &proc->mmap_regions[suffix_slot];
                        sfx->start = r->rdi + rounded; sfx->end = old_end; sfx->used = true; sfx->writable = old_w; sfx->executable = old_x;
                    }
                    reg->start = r->rdi; reg->end = r->rdi + rounded;   /* this slot becomes exactly the requested middle */
                }
            }
            if (found < 0) { r->rax = (uint64_t)-1; break; }

            proc->mmap_regions[found].writable   = want_write;
            proc->mmap_regions[found].executable = want_exec;

            /* Re-protect whatever pages in this range are ALREADY
             * demand-paged in - task_handle_page_fault() reading the
             * updated region flags above only takes effect for pages
             * faulted in AFTER this call, but a JIT's whole point is
             * writing to pages that are already resident (mmap'd RW,
             * written to, THEN mprotected to RX), so those existing
             * mappings must be updated immediately, in place, preserving
             * their physical backing and contents - exactly what
             * paging_protect_page() does (it keeps the frame, replaces
             * only the flags, and flushes that one TLB entry). A page in
             * this range that has not been touched yet simply is not
             * present, and paging_protect_page() correctly no-ops for
             * it - task_handle_page_fault() will apply the now-updated
             * region flags whenever it eventually is. */
            uint64_t new_flags = PAGE_PRESENT | PAGE_USER;
            if (want_write) new_flags |= PAGE_RW;
            if (!want_exec && cos_elf_nx_available()) new_flags |= PAGE_NX;
            for (uint64_t off = 0; off < rounded; off += PAGE_SIZE) {
                paging_protect_page(r->rdi + off, new_flags);
            }

            r->rax = 0;
            break;
        }

        case SYS_WIN_DRAW_TEXT: {
            /* rdi = handle, rsi = x, rdx = y, r10 = fg color, r8 = bg
             * color, r9 = user pointer to a UTF-8-ish byte string
             * (rendered byte-for-byte via vga_draw_string_len, which
             * decodes UTF-8 continuation bytes; plain ASCII is the
             * common case and needs no special handling). Length is
             * read as a NUL-terminated string up to COS_APP_MAX_TEXT_LEN
             * - there is no separate length argument, matching how a
             * small C program most naturally has a string to hand this. */
            char text[64];
            if (!cos_syscall_copy_path(text, sizeof(text), r->r9)) {
                r->rax = (uint64_t)-1; break;
            }
            uint32_t pid = cos_syscall_caller_pid();
            bool ok = cos_app_window_draw_text((int64_t)r->rdi,
                        (int32_t)(int64_t)r->rsi, (int32_t)(int64_t)r->rdx,
                        (uint32_t)r->r10, (uint32_t)r->r8, text,
                        (uint32_t)strlen(text), pid);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_WIN_POLL_KEY: {
            /* rdi = handle, rsi = user pointer to a cos_app_key_event_t
             * (2 bytes: char ascii, uint8_t special). Returns 1 if a key
             * was popped, 0 if the queue is empty (not an error - just
             * nothing typed yet), or (uint64_t)-1 for a bad handle/
             * pointer. */
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_app_key_event_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_app_key_event_t), true)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_app_key_event_t ev;
            bool got = cos_app_window_poll_key((int64_t)r->rdi, cos_syscall_caller_pid(), &ev);
            if (got) {
                memcpy((void *)(uintptr_t)r->rsi, &ev, sizeof(ev));
                r->rax = 1;
            } else {
                r->rax = 0;
            }
            break;
        }

        case SYS_WIN2_CREATE: {
            char title[64];
            if (!cos_syscall_copy_user_string(r->rdi, title, sizeof(title))) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(r->rdx, sizeof(cos_win_info_t), true);
            if (!r->rdx || !paging_user_range_ok(r->rdx, sizeof(cos_win_info_t), true)) { r->rax = (uint64_t)-1; break; }
            cos_win_info_t info;
            memset(&info, 0, sizeof(info));
            int64_t h = cos_app_window_create_v2(title, (int32_t)(r->rsi & 0xFFFFFFFFu), (int32_t)(r->rsi >> 32),
                                                 cos_syscall_caller_pid(), &info);
            if (h > 0) memcpy((void*)(uintptr_t)r->rdx, &info, sizeof(info));
            r->rax = (uint64_t)h;
            break;
        }
        case SYS_WIN2_PRESENT:
            r->rax = cos_app_window_present((int64_t)r->rdi, cos_syscall_caller_pid()) ? 0 : (uint64_t)-1;
            break;
        case SYS_WIN2_VIDEO: {
            int32_t w = (int32_t)(r->rdx & 0xFFFFFFFFu), h = (int32_t)(r->rdx >> 32);
            int32_t stride = (int32_t)r->r10;
            int32_t x = (int32_t)(r->r8 & 0xFFFFFFFFu), y = (int32_t)(r->r8 >> 32);
            int32_t dw = (int32_t)(r->r9 & 0xFFFFFFFFu), dh = (int32_t)(r->r9 >> 32);
            uint64_t bytes = (uint64_t)(stride > 0 ? stride : 0) * (uint64_t)(h > 0 ? h : 0) * 4u;
            if (!r->rsi || !bytes || bytes > 64u * 1024u * 1024u ||
                !paging_user_range_ok(r->rsi, bytes, false)) { r->rax = (uint64_t)-1; break; }
            r->rax = cos_app_window_video_frame((int64_t)r->rdi, cos_syscall_caller_pid(), (const uint32_t*)(uintptr_t)r->rsi, w, h, stride, x, y, dw, dh) ? 0 : (uint64_t)-1;
            break;
        }
        case SYS_WIN2_VIDEO_MAP: {
            uint64_t va = 0, bytes = 0;
            /* rdx = user pointer receiving the size in bytes of each of the two buffers */
            if (!r->rdx || !paging_user_range_ok(r->rdx, sizeof(uint64_t), true)) { r->rax = (uint64_t)-1; break; }
            bool ok = cos_app_window_video_map((int64_t)r->rdi, cos_syscall_caller_pid(),
                                               (int32_t)(r->rsi & 0xFFFFFFFFu), (int32_t)(r->rsi >> 32), &va, &bytes);
            if (ok) *(uint64_t *)(uintptr_t)r->rdx = bytes;
            r->rax = ok ? va : (uint64_t)-1;
            break;
        }
        case SYS_SANDBOX: {
            process_t *sp = process_get_current();
            if (!sp || sp->type != TASK_TYPE_USER) { r->rax = (uint64_t)-1; break; }
            if (r->rsi) {
                char j[sizeof(sp->jail)];
                /* the new jail is itself a path argument: copy_path applies the CURRENT jail, so it can only narrow */
                if (!cos_syscall_copy_path(j, sizeof(j), r->rsi) || j[0] != '/' || !cos_path_is_clean(j)) { r->rax = (uint64_t)-1; break; }
                size_t n = strlen(j);
                while (n > 1 && j[n - 1] == '/') j[--n] = '\0';
                for (const char *q = j; *q; ++q)
                    if (q[0] == '/' && q[1] == '.' && q[2] == '.' && (q[3] == '/' || q[3] == '\0')) { r->rax = (uint64_t)-1; break; }
                if (r->rax == (uint64_t)-1) break;
                if (strcmp(j, "/") == 0) j[0] = '\0';                  /* "/" = no extra confinement */
                if (j[0] || !sp->jail[0]) memcpy(sp->jail, j, sizeof(sp->jail));
            }
            sp->caps &= (uint32_t)r->rdi;                             /* never regains a capability */
            cos_kmod_event(COSYS_EV_SANDBOX_SET, sp->pid, sp->caps, 0);
            r->rax = sp->caps;
            break;
        }
        case SYS_DEVICES_LIST: {
            uint32_t n = (uint32_t)r->rsi;
            if (n == 0 || n > 64u || !r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)n * sizeof(cos_device_info_t), true)) {
                r->rax = (uint64_t)-1;
                break;
            }
            static cos_device_info_t kdev[64];   /* syscalls run with interrupts off on one core at a time per caller; static keeps the stack small */
            int cnt = cos_devices_list(kdev, (int)n);
            memcpy((void *)(uintptr_t)r->rdi, kdev, (size_t)cnt * sizeof(cos_device_info_t));
            r->rax = (uint64_t)cnt;
            break;
        }
        case SYS_SOCKET:
            r->rax = (uint64_t)(int64_t)cos_sock_create(cos_syscall_caller_pid(), (int)r->rdi);
            break;
        case SYS_BIND:
            r->rax = (uint64_t)(int64_t)cos_sock_bind(cos_syscall_caller_pid(), (int)r->rdi, (uint32_t)r->rsi, (uint16_t)r->rdx);
            break;
        case SYS_LISTEN:
            r->rax = (uint64_t)(int64_t)cos_sock_listen(cos_syscall_caller_pid(), (int)r->rdi, (int)r->rsi);
            break;
        case SYS_ACCEPT: {
            cos_sockaddr_t peer; memset(&peer, 0, sizeof peer);
            int rc = cos_sock_accept(cos_syscall_caller_pid(), (int)r->rdi, &peer);
            if (rc >= 0 && r->rsi && paging_user_range_ok(r->rsi, sizeof peer, true)) memcpy((void *)(uintptr_t)r->rsi, &peer, sizeof peer);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_CONNECT:
            r->rax = (uint64_t)(int64_t)cos_sock_connect(cos_syscall_caller_pid(), (int)r->rdi, (uint32_t)r->rsi, (uint16_t)r->rdx);
            break;
        case SYS_SEND: case SYS_SENDTO: {
            uint64_t len = r->rdx;
            if (r->rax == SYS_SEND && len > 65536u) len = 65536u;      /* a stream send may be partial; the caller loops */
            if (len > 65536u || (len && (!r->rsi || !paging_user_range_ok(r->rsi, len, false)))) { r->rax = (uint64_t)-(int64_t)COS_EINVAL; break; }
            static uint8_t sbounce[65536];                     /* syscalls do not overlap, so one bounce buffer is enough */
            if (len) memcpy(sbounce, (const void *)(uintptr_t)r->rsi, (size_t)len);
            int rc = r->rax == SYS_SEND
                ? cos_sock_send(cos_syscall_caller_pid(), (int)r->rdi, sbounce, (size_t)len)
                : cos_sock_sendto(cos_syscall_caller_pid(), (int)r->rdi, sbounce, (size_t)len, (uint32_t)r->r10, (uint16_t)r->r8);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_RECV: case SYS_RECVFROM: {
            uint64_t len = r->rdx;
            if (len > 65536u) len = 65536u;
            if (len && (!r->rsi || !paging_user_range_ok(r->rsi, len, true))) { r->rax = (uint64_t)-(int64_t)COS_EINVAL; break; }
            static uint8_t rbounce[65536];
            cos_sockaddr_t from; memset(&from, 0, sizeof from);
            int rc = r->rax == SYS_RECV
                ? cos_sock_recv(cos_syscall_caller_pid(), (int)r->rdi, rbounce, (size_t)len)
                : cos_sock_recvfrom(cos_syscall_caller_pid(), (int)r->rdi, rbounce, (size_t)len, &from);
            if (rc > 0) memcpy((void *)(uintptr_t)r->rsi, rbounce, (size_t)rc);
            if (rc >= 0 && r->rax == SYS_RECVFROM && r->r10 && paging_user_range_ok(r->r10, sizeof from, true)) memcpy((void *)(uintptr_t)r->r10, &from, sizeof from);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_SOCK_POLL: {
            uint32_t n = (uint32_t)r->rsi;
            if (n == 0 || n > 32u || !r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)n * sizeof(cos_pollfd_t), true)) { r->rax = (uint64_t)-(int64_t)COS_EINVAL; break; }
            cos_pollfd_t kp[32];
            memcpy(kp, (const void *)(uintptr_t)r->rdi, (size_t)n * sizeof(cos_pollfd_t));
            int rc = cos_sock_poll(cos_syscall_caller_pid(), kp, (int)n);
            memcpy((void *)(uintptr_t)r->rdi, kp, (size_t)n * sizeof(cos_pollfd_t));
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_SOCK_CLOSE:
            r->rax = (uint64_t)(int64_t)cos_sock_close(cos_syscall_caller_pid(), (int)r->rdi);
            break;
        case SYS_SOCK_NAME: {
            cos_sockaddr_t a;
            int rc = cos_sock_name(cos_syscall_caller_pid(), (int)r->rdi, (int)r->rsi, &a);
            if (rc == 0 && r->rdx && paging_user_range_ok(r->rdx, sizeof a, true)) memcpy((void *)(uintptr_t)r->rdx, &a, sizeof a);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_DNS_START: {
            char host[256];
            if (!cos_syscall_copy_path(host, sizeof host, r->rdi) || !r->rsi || !paging_user_range_ok(r->rsi, sizeof(cos_dns_result_t), true)) { r->rax = (uint64_t)-(int64_t)COS_EINVAL; break; }
            cos_dns_result_t res;
            int rc = cos_dns_start(host, &res);
            memcpy((void *)(uintptr_t)r->rsi, &res, sizeof res);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_DNS_POLL: {
            if (!r->rdi || !paging_user_range_ok(r->rdi, sizeof(cos_dns_result_t), true)) { r->rax = (uint64_t)-(int64_t)COS_EINVAL; break; }
            cos_dns_result_t res;
            memcpy(&res, (const void *)(uintptr_t)r->rdi, sizeof res);
            int rc = cos_dns_poll(&res);
            memcpy((void *)(uintptr_t)r->rdi, &res, sizeof res);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_SHELL_CREATE: {
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_win_info_t), true);
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(cos_win_info_t), true)) { r->rax = (uint64_t)-1; break; }
            cos_win_info_t info;
            memset(&info, 0, sizeof(info));
            int64_t h;
            if ((int32_t)r->rdi == 6) {            /* the tray balloon: a small colour-keyed surface above the right end of the taskbar */
                h = cos_shell_surface_create(6, (int32_t)SCREEN_W - 380, (int32_t)SCREEN_H - TASKBAR_H - 92, 372, 86, cos_syscall_caller_pid(), &info);
            } else if ((int32_t)r->rdi == 5) {            /* context menus: the whole screen above the taskbar, colour-keyed */
                h = cos_shell_surface_create(5, 0, 0, (int32_t)SCREEN_W, (int32_t)SCREEN_H, cos_syscall_caller_pid(), &info);
            } else if ((int32_t)r->rdi == 4) {            /* toasts: top right */
                h = cos_shell_surface_create(4, (int32_t)SCREEN_W - 320, 16, 316, COS_NOTIF_MAX * 60, cos_syscall_caller_pid(), &info);
            } else if ((int32_t)r->rdi == 3) {            /* desktop: everything above the taskbar */
                h = cos_shell_surface_create(3, 0, 0, (int32_t)SCREEN_W, (int32_t)SCREEN_H - TASKBAR_H, cos_syscall_caller_pid(), &info);
            } else if ((int32_t)r->rdi == 2) {            /* start menu: above the taskbar, left-aligned (clamped on small screens) */
                int32_t mw = 620, mh = 430;
                if (mw > (int32_t)SCREEN_W - 8) mw = (int32_t)SCREEN_W - 8;
                if (mh > (int32_t)SCREEN_H - TASKBAR_H - 8) mh = (int32_t)SCREEN_H - TASKBAR_H - 8;
                h = cos_shell_surface_create(2, 12 > (int32_t)SCREEN_W - mw - 4 ? 4 : 12, (int32_t)SCREEN_H - TASKBAR_H - mh - 8, mw, mh,
                                             cos_syscall_caller_pid(), &info);
            } else {
                h = cos_shell_surface_create((int32_t)r->rdi, 0, (int32_t)SCREEN_H - TASKBAR_H, (int32_t)SCREEN_W, TASKBAR_H,
                                             cos_syscall_caller_pid(), &info);
            }
            if (h > 0) memcpy((void*)(uintptr_t)r->rsi, &info, sizeof(info));
            r->rax = (uint64_t)h;
            break;
        }
        case SYS_WIN_LIST: {
            int mx = (int)r->rsi;
            if (mx < 0 || mx > COS_SHELL_MAX_WINDOWS || !r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)mx * sizeof(cos_winfo_t), true)) { r->rax = (uint64_t)-1; break; }
            static cos_winfo_t kl[COS_SHELL_MAX_WINDOWS];
            int n = gui_shell_list(kl, mx);
            if (n > 0) memcpy((void*)(uintptr_t)r->rdi, kl, (size_t)n * sizeof(cos_winfo_t));
            r->rax = (uint64_t)(int64_t)n;
            break;
        }
        case SYS_CTX_LIST: {
            if (!r->rdi || !paging_user_range_ok(r->rdi, sizeof(cos_ctxsnap_t), true)) { r->rax = (uint64_t)-1; break; }
            static cos_ctxsnap_t kc;
            int vis = gui_ctx_list(&kc);
            memcpy((void*)(uintptr_t)r->rdi, &kc, sizeof kc);
            r->rax = (uint64_t)(int64_t)vis;
            break;
        }
        case SYS_KERNEL_UPDATE: {
            extern int cos_bootupd_status(void *out); extern int cos_bootupd_install(const uint8_t *blob, size_t len, char *why, size_t whycap);
            if (r->rdi == 0) {
                static uint8_t kst[256];
                if (!r->rsi || !paging_user_range_ok(r->rsi, 128, true)) { r->rax = (uint64_t)-1; break; }
                cos_bootupd_status(kst);
                memcpy((void *)(uintptr_t)r->rsi, kst, 128);
                r->rax = 0;
            } else if (r->rdi == 1) {
                if (!r->rsi || r->rdx < 1024 || r->rdx > (64ull << 20) || !paging_user_range_ok(r->rsi, r->rdx, false)) { r->rax = (uint64_t)-1; break; }
                char why[96];
                r->rax = (uint64_t)(int64_t)cos_bootupd_install((const uint8_t *)(uintptr_t)r->rsi, (size_t)r->rdx, why, sizeof why);
            } else if (r->rdi == 3) {                              /* forget the serial of a failed start (cosupdate reinstall) */
                extern int cos_bootupd_clear_bad(void);
                r->rax = (uint64_t)(int64_t)cos_bootupd_clear_bad();
            } else if (r->rdi == 2) {                              /* the reason of the last install */
                extern const char *cos_bootupd_last_why(void);
                if (!r->rsi || !paging_user_range_ok(r->rsi, 96, true)) { r->rax = (uint64_t)-1; break; }
                memcpy((void *)(uintptr_t)r->rsi, cos_bootupd_last_why(), 96);
                r->rax = 0;
            } else r->rax = (uint64_t)-1;
            break;
        }
        case SYS_NOTIF_SEND: {
            char nt[48], nm[96];
            extern uint32_t notification_send_with_duration(const char* title, const char* message, int type, int priority, int duration_ms);
            static uint64_t last_toast_ms;
            if (!cos_syscall_copy_user_string(r->rdi, nt, sizeof nt) || !cos_syscall_copy_user_string(r->rsi, nm, sizeof nm)) { r->rax = (uint64_t)-1; break; }
            uint64_t now_ms = cos_now_ms();
            if (now_ms - last_toast_ms < 400) { r->rax = (uint64_t)-1; break; }          /* a program cannot flood the screen */
            last_toast_ms = now_ms;
            int ty = (int)r->rdx; if (ty < 0 || ty > 3) ty = 0;
            notification_send_with_duration(nt, nm, ty, 1, 80000);                /* the expiry thread keeps a toast for duration_ms / 10 ticks: 8 s */
            serial_puts("[NOTIFY] toast: "); serial_puts(nt); serial_puts(" - "); serial_puts(nm); serial_puts("\n");
            r->rax = 0;
            break;
        }
        case SYS_NOTIF_LIST: {
            int mx = (int)r->rsi;
            if (mx < 0 || mx > COS_NOTIF_MAX || !r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)mx * sizeof(cos_notif_t), true)) { r->rax = (uint64_t)-1; break; }
            static cos_notif_t kn[COS_NOTIF_MAX];
            int n = gui_notif_list(kn, mx);
            if (n > 0) memcpy((void*)(uintptr_t)r->rdi, kn, (size_t)n * sizeof(cos_notif_t));
            r->rax = (uint64_t)(int64_t)n;
            break;
        }
        case SYS_DESKTOP_LIST: {
            int mx = (int)r->rsi;
            if (mx < 0 || mx > 64 || !r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)mx * sizeof(cos_dicon_t), true)) { r->rax = (uint64_t)-1; break; }
            static cos_dicon_t kd[64];
            int n = gui_desktop_list(kd, mx);
            if (n > 0) memcpy((void*)(uintptr_t)r->rdi, kd, (size_t)n * sizeof(cos_dicon_t));
            r->rax = (uint64_t)(int64_t)n;
            break;
        }
        case SYS_DESKTOP_CTL:
            r->rax = (uint64_t)(int64_t)gui_desktop_ctl((int)r->rdi, (int)r->rsi, (int)r->rdx, (int)r->r10);
            break;
        case SYS_SHELL_ICONS: {
            int n = (int)r->rdx;
            if (n < 0 || n > COS_SHELL_MAX_ICONS || (n && (!r->rsi || !paging_user_range_ok(r->rsi, (uint64_t)n * sizeof(cos_shell_icon_t), false)))) { r->rax = (uint64_t)-1; break; }
            cos_shell_icon_t ki[COS_SHELL_MAX_ICONS];
            if (n) memcpy(ki, (void*)(uintptr_t)r->rsi, (size_t)n * sizeof(cos_shell_icon_t));
            r->rax = (uint64_t)(int64_t)cos_shell_set_icons((int)r->rdi, cos_syscall_caller_pid(), ki, n, (uint32_t)r->r10);
            break;
        }
        case SYS_WIN_CTL:
            r->rax = (uint64_t)(int64_t)gui_shell_ctl((int)r->rdi, (uint32_t)r->rsi, (int)r->rdx);
            break;
        case SYS_NET_INFO: {
            if (!r->rdi || !paging_user_range_ok(r->rdi, sizeof(cos_netinfo_t), true)) { r->rax = (uint64_t)-(int64_t)COS_EINVAL; break; }
            cos_netinfo_t ni;
            int rc = cos_net_info(&ni);
            if (rc == 0) memcpy((void *)(uintptr_t)r->rdi, &ni, sizeof ni);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_NET_HTTP_START: {
            char url[HTTP_MAX_URL];
            if (!cos_syscall_copy_path(url, sizeof(url), r->rdi)) { r->rax = (uint64_t)-1; break; }
            r->rax = (uint64_t)(int64_t)cos_httpjob_start(url, cos_syscall_caller_pid());
            break;
        }
        case SYS_NET_HTTP_POLL: {
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(cos_http_result_t), true)) { r->rax = (uint64_t)-1; break; }
            cos_http_result_t kres;
            memset(&kres, 0, sizeof kres);
            int st = cos_httpjob_poll((int)r->rdi, cos_syscall_caller_pid(), &kres);
            if (st == 1) memcpy((void *)(uintptr_t)r->rsi, &kres, sizeof kres);
            r->rax = (uint64_t)(int64_t)st;
            break;
        }
        case SYS_NET_HTTP_TAKE: {
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(ubuf, ulen, true);
            if (!paging_user_range_ok(ubuf, ulen, true)) { r->rax = (uint64_t)-1; break; }
            r->rax = (uint64_t)cos_httpjob_take((int)r->rdi, cos_syscall_caller_pid(), (void *)(uintptr_t)ubuf, ulen);
            break;
        }
        case SYS_NET_HTTP_CANCEL:
            cos_httpjob_cancel((int)r->rdi, cos_syscall_caller_pid());
            r->rax = 0;
            break;
        case SYS_SYSTEM_INFO: {
            if (!r->rdi || !paging_user_range_ok(r->rdi, sizeof(cos_sysinfo_t), true)) {
                r->rax = (uint64_t)-1;
                break;
            }
            static cos_sysinfo_t ksi;
            memset(&ksi, 0, sizeof(ksi));
            sched_stats_t ss = {0};
            scheduler_get_stats(&ss);
            ksi.uptime_ticks = scheduler_get_uptime();
            ksi.ctx_switches = ss.context_switches;
            ksi.load_now = ss.current_load;
            ksi.load_1m = ss.avg_load_1m;
            ksi.load_5m = ss.avg_load_5m;
            ksi.load_15m = ss.avg_load_15m;
            ksi.ram_total = phys_get_total_memory();
            ksi.kheap_used = kmemory_used();
            ksi.kheap_free = kmemory_free();
            phys_mem_stats_t *pm = phys_memory_get_stats();
            if (pm) { ksi.pages_total = pm->total_pages; ksi.pages_free = pm->free_pages; }
            { extern void cos_pcache_stats(uint32_t *f, uint32_t *m); cos_pcache_stats(&ksi.pcache_frames, &ksi.pcache_maps); }
            ksi.stor_total = storage_get_total_space();
            ksi.stor_used = storage_get_used_space();
            ksi.stor_free = storage_get_free_space();
            if (unified_storage_get_total_capacity) ksi.ustor_total = unified_storage_get_total_capacity();
            if (unified_storage_get_free_capacity) ksi.ustor_free = unified_storage_get_free_capacity();
            if (unified_storage_get_device_count) ksi.ustor_devices = (uint32_t)unified_storage_get_device_count();
            ksi.has_password = storage_has_password();
            ksi.cpus_online = smp_online_cpu_count();
            ksi.cpus_possible = smp_possible_cpu_count();
            bios_system_info_t bi;
            bios_get_system_info_copy(&bi);
            ksi.cpus_bios = bi.num_cpus;
            ksi.has_apic = bi.has_apic;
            ksi.has_acpi = bi.has_acpi;
            {   /* the battery (ACPI) rides in `reserved` so the structure keeps its size for programs built before: bit 0 present, bit 1 charging, bits 8..15 percent, bits 16..31 minutes left */
                extern uint64_t acpi_power_get_battery_percent(void); extern bool acpi_power_is_available(void); extern bool acpi_power_is_charging(void); extern uint64_t acpi_power_get_estimated_minutes_remaining(void);
                uint64_t pct = acpi_power_get_battery_percent(), mins = acpi_power_get_estimated_minutes_remaining();
                if (pct > 100) pct = 100; if (mins > 65535) mins = 65535;
                ksi.reserved = (acpi_power_is_available() ? 1u : 0u) | (acpi_power_is_charging() ? 2u : 0u) | ((uint32_t)pct << 8) | ((uint32_t)mins << 16);
            }
            memcpy(ksi.cpu_vendor, bi.cpu_vendor, sizeof(ksi.cpu_vendor) - 1);
            memcpy(ksi.bios_vendor, bi.bios_vendor, sizeof(ksi.bios_vendor) - 1);
            memcpy(ksi.bios_version, bi.bios_version, sizeof(ksi.bios_version) - 1);
            memcpy(ksi.bios_date, bi.bios_date, sizeof(ksi.bios_date) - 1);
            strncpy(ksi.os_name, COS_FULL_NAME, sizeof(ksi.os_name) - 1);
            strncpy(ksi.os_version, COS_VERSION_STRING, sizeof(ksi.os_version) - 1);
            strncpy(ksi.boot_msg, COS_BOOT_MESSAGE, sizeof(ksi.boot_msg) - 1);
            memcpy((void *)(uintptr_t)r->rdi, &ksi, sizeof(ksi));
            r->rax = 0;
            break;
        }
        case SYS_PROCLIST: {
            uint32_t n = (uint32_t)r->rsi;
            if (n == 0 || n > 128u || !r->rdi || !paging_user_range_ok(r->rdi, (uint64_t)n * sizeof(cos_procinfo_t), true)) {
                r->rax = (uint64_t)-1;
                break;
            }
            static cos_procinfo_t kpl[128];
            int cnt = process_snapshot(kpl, (int)n);
            memcpy((void *)(uintptr_t)r->rdi, kpl, (size_t)cnt * sizeof(cos_procinfo_t));
            r->rax = (uint64_t)cnt;
            break;
        }
        case SYS_LXSHM_MAP: {
            extern uint64_t cos_linux_shm_map(const char *name, uint64_t *size_out);
            char nm[64]; uint64_t sz = 0;
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(uint64_t), true) || !cos_syscall_copy_path(nm, sizeof nm, r->rdi) || ({ bool sl = false; for (const char *q = nm; *q; ++q) if (*q == '/') sl = true; sl; })) { r->rax = 0; break; }
            r->rax = cos_linux_shm_map(nm, &sz);
            if (r->rax) *(uint64_t *)(uintptr_t)r->rsi = sz;
            break;
        }
        case SYS_WIN2_PRESENT_RECTS: {
            uint32_t n = (uint32_t)r->rdx;
            if (n == 0 || n > 64u || !r->rsi || !paging_user_range_ok(r->rsi, (uint64_t)n * sizeof(cos_win_rect_t), false)) {
                r->rax = (uint64_t)-1;
                break;
            }
            cos_win_rect_t kr[64];
            memcpy(kr, (const void *)(uintptr_t)r->rsi, (size_t)n * sizeof(cos_win_rect_t));
            r->rax = cos_app_window_present_rects((int64_t)r->rdi, cos_syscall_caller_pid(), kr, n) ? 0 : (uint64_t)-1;
            break;
        }
        case SYS_WIN2_VIDEO_COMMIT:
            r->rax = cos_app_window_video_commit((int64_t)r->rdi, cos_syscall_caller_pid(), (int32_t)r->rsi,
                                                 (int32_t)(r->rdx & 0xFFFFFFFFu), (int32_t)(r->rdx >> 32),
                                                 (int32_t)(r->r10 & 0xFFFFFFFFu), (int32_t)(r->r10 >> 32),
                                                 (int32_t)(r->r8 & 0xFFFFFFFFu), (int32_t)(r->r8 >> 32)) ? 0 : (uint64_t)-1;
            break;
        case SYS_WIN2_VIDEO_YUV420: {
            /* Layout must match cos_video_yuv420_t in userland/include/cos.h
             * exactly - see this file's other hand-mirrored structs
             * (SYS_SETTINGS_GET, SYS_STAT_EX) for why there's no shared
             * header. rdi=handle, rsi=pointer to this struct. */
            struct {
                const uint8_t *y, *u, *v;
                int32_t w, h;
                int32_t stride_y, stride_c;
                int32_t x, y_pos;
                int32_t dst_w, dst_h;
                int32_t bit_depth;
            } frame;
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(frame), false)) {
                r->rax = (uint64_t)-1; break;
            }
            memcpy(&frame, (const void*)(uintptr_t)r->rsi, sizeof(frame));

            if (frame.bit_depth < 8 || frame.bit_depth > 12) { r->rax = (uint64_t)-1; break; }
            int32_t sample_bytes = frame.bit_depth > 8 ? 2 : 1;
            if (frame.w <= 0 || frame.h <= 0 || frame.w > 3840 || frame.h > 2160 ||
                frame.stride_y < frame.w * sample_bytes || frame.stride_c < ((frame.w + 1) / 2) * sample_bytes) {
                r->rax = (uint64_t)-1; break;
            }
            int32_t chroma_h = (frame.h + 1) / 2;
            uint64_t y_bytes = (uint64_t)frame.stride_y * (uint64_t)frame.h;
            uint64_t c_bytes = (uint64_t)frame.stride_c * (uint64_t)chroma_h;
            uint64_t uy = (uint64_t)(uintptr_t)frame.y;
            uint64_t uu = (uint64_t)(uintptr_t)frame.u;
            uint64_t uv = (uint64_t)(uintptr_t)frame.v;
            if (!uy || !uu || !uv) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(uy, y_bytes, false);
            cos_syscall_touch_user_range(uu, c_bytes, false);
            cos_syscall_touch_user_range(uv, c_bytes, false);
            if (!paging_user_range_ok(uy, y_bytes, false) ||
                !paging_user_range_ok(uu, c_bytes, false) ||
                !paging_user_range_ok(uv, c_bytes, false)) {
                r->rax = (uint64_t)-1; break;
            }
            r->rax = cos_app_window_video_frame_yuv420(
                (int64_t)r->rdi, cos_syscall_caller_pid(),
                frame.y, frame.u, frame.v, frame.w, frame.h,
                frame.stride_y, frame.stride_c, frame.x, frame.y_pos,
                frame.dst_w, frame.dst_h, frame.bit_depth) ? 0 : (uint64_t)-1;
            break;
        }
        case SYS_WIN2_POLL:
        case SYS_WIN2_WAIT: {
            const uint64_t nr = r->rax;
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_win_event_t), true);
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof(cos_win_event_t), true)) { r->rax = (uint64_t)-1; break; }
            cos_win_event_t ev;
            int got = cos_app_window_poll_event((int64_t)r->rdi, cos_syscall_caller_pid(), &ev);
            if (got == 0 && nr == SYS_WIN2_WAIT) {
                /* Sleep until an event arrives (it wakes us) or the timeout passes - an idle app costs nothing.
                 * The slice is only a safety net for the wake-up path, not a polling interval. */
                uint64_t remaining = r->rdx ? r->rdx : 0xFFFFFFFFu;
                while (got == 0 && remaining > 0) {
                    uint64_t step = remaining < 1000 ? remaining : 1000;
                    uint64_t t0 = get_timer_ticks();
                    got = cos_app_window_wait_event((int64_t)r->rdi, cos_syscall_caller_pid(), &ev, step);
                    if (got < 0) break;
                    uint64_t spent = get_timer_ticks() - t0;
                    remaining = spent >= remaining ? 0 : remaining - spent;
                }
            }
            /* the wait may have blocked: the buffer can have been unmapped by another thread meanwhile */
            if (got == 1) cos_syscall_touch_user_range(r->rsi, sizeof(ev), true);
            if (got == 1 && !paging_user_range_ok(r->rsi, sizeof(ev), true)) got = -1;
            if (got == 1) memcpy((void*)(uintptr_t)r->rsi, &ev, sizeof(ev));
            r->rax = (uint64_t)(int64_t)got;
            break;
        }
        case SYS_WIN2_SET_TITLE: {
            char title[64];
            if (!cos_syscall_copy_user_string(r->rsi, title, sizeof(title))) { r->rax = (uint64_t)-1; break; }
            r->rax = cos_app_window_set_title((int64_t)r->rdi, cos_syscall_caller_pid(), title) ? 0 : (uint64_t)-1;
            break;
        }
        case SYS_WIN2_CLOSE:
            r->rax = cos_app_window_close((int64_t)r->rdi, cos_syscall_caller_pid()) ? 0 : (uint64_t)-1;
            break;
        case SYS_FILE_DROP: {
            char dp[256];
            extern int gui_file_drop(const char *path, uint32_t pid, int copy);
            if (!cos_syscall_copy_user_string(r->rdi, dp, sizeof dp)) { r->rax = (uint64_t)-1; break; }
            r->rax = (uint64_t)(int64_t)gui_file_drop(dp, cos_syscall_caller_pid(), r->rsi != 0);
            break;
        }
        case SYS_DRAG_GHOST: {
            char gl[48];
            extern void gui_drag_ghost_set(const char *label);
            if (!r->rdi) { gui_drag_ghost_set(NULL); r->rax = 0; break; }
            if (!cos_syscall_copy_user_string(r->rdi, gl, sizeof gl)) { r->rax = (uint64_t)-1; break; }
            gui_drag_ghost_set(gl);
            r->rax = 0;
            break;
        }
        case SYS_WIN2_RESIZE: {
            cos_win_info_t info;
            cos_syscall_touch_user_range(r->rdx, sizeof info, true);
            if (!r->rdx || !paging_user_range_ok(r->rdx, sizeof info, true)) { r->rax = (uint64_t)-1; break; }
            memset(&info, 0, sizeof info);
            int rc = cos_app_window_resize((int64_t)r->rdi, cos_syscall_caller_pid(), (int32_t)(r->rsi & 0xFFFFFFFFu), (int32_t)(r->rsi >> 32), &info);
            if (rc == 0) memcpy((void *)(uintptr_t)r->rdx, &info, sizeof info);
            r->rax = (uint64_t)(int64_t)rc;
            break;
        }
        case SYS_WIN2_FULLSCREEN:
            r->rax = (uint64_t)(int64_t)cos_app_window_set_fullscreen((int64_t)r->rdi, cos_syscall_caller_pid(), (int)r->rsi);
            break;
        case SYS_WIN2_GRAB:
            r->rax = cos_app_window_set_grab((int64_t)r->rdi, cos_syscall_caller_pid(), r->rsi != 0) ? 0 : (uint64_t)-1;
            break;
        case SYS_WIN2_INPUT: {
            cos_win_input_t in;
            cos_syscall_touch_user_range(r->rsi, sizeof in, true);
            if (!r->rsi || !paging_user_range_ok(r->rsi, sizeof in, true)) { r->rax = (uint64_t)-1; break; }
            if (cos_app_window_input_state((int64_t)r->rdi, cos_syscall_caller_pid(), &in) != 0) { r->rax = (uint64_t)-1; break; }
            memcpy((void*)(uintptr_t)r->rsi, &in, sizeof in);
            r->rax = 0;
            break;
        }
        case SYS_WIN2_GET_PATH: {
            uint64_t cap = r->rdx;
            if (cap > 256) cap = 256;
            cos_syscall_touch_user_range(r->rsi, cap, true);
            if (!r->rsi || !cap || !paging_user_range_ok(r->rsi, cap, true)) { r->rax = (uint64_t)-1; break; }
            char tmp[256];
            int n = cos_app_window_get_path((int64_t)r->rdi, cos_syscall_caller_pid(), tmp, cap);
            if (n >= 0) memcpy((void*)(uintptr_t)r->rsi, tmp, (size_t)n + 1);
            r->rax = (uint64_t)(int64_t)n;
            break;
        }
        /* ---- file-manager support (cos_fm_support.c) ---- */
        case SYS_STAT_EX: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_stat_ex_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_stat_ex_t), true)) { r->rax = (uint64_t)-1; break; }
            cos_stat_ex_t st;
            if (!cos_fm_stat_ex(path, &st)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rsi, &st, sizeof(st));
            r->rax = 0;
            break;
        }
        case SYS_SET_ATTR: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) { r->rax = (uint64_t)-1; break; }
            uint8_t attr = (uint8_t)(r->rsi & 0xFFu);
            uint8_t mask = (uint8_t)((r->rsi >> 8) & 0xFFu);
            r->rax = cos_fm_set_attr(path, attr, mask) ? 0 : (uint64_t)-1;
            break;
        }
        case SYS_FS_GENERATION: {
            extern uint64_t cos_fs_generation(void);
            r->rax = cos_fs_generation();
            break;
        }
        case SYS_FS_SPACE: {
            cos_syscall_touch_user_range(r->rdi, 2 * sizeof(uint64_t), true);
            if (!paging_user_range_ok(r->rdi, 2 * sizeof(uint64_t), true)) { r->rax = (uint64_t)-1; break; }
            uint64_t sp[2];
            if (!cos_fm_space(sp)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rdi, sp, sizeof(sp));
            r->rax = 0;
            break;
        }
        case SYS_OPEN_PATH: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) { r->rax = (uint64_t)-1; break; }
            /* the file manager opens it with full privileges: a confined caller must not get a program launched that way */
            process_t *op = process_get_current();
            if (op && op->caps != COS_CAP_ALL) {
                size_t n = strlen(path);
                static const char *const ext[] = { ".c-os", ".c-osll", ".c-osys" };
                bool exec = false;
                for (unsigned i = 0; i < 3; i++) {
                    size_t el = strlen(ext[i]);
                    if (n >= el) {
                        size_t k = 0;
                        while (k < el && (path[n - el + k] | 0x20) == ext[i][k]) k++;
                        if (k == el) exec = true;
                    }
                }
                if (exec) { r->rax = (uint64_t)-1; break; }
            }
            r->rax = cos_fm_queue_open(path) ? 0 : (uint64_t)-1;
            break;
        }
        case SYS_KMOD_LOAD: case SYS_KMOD_UNLOAD: {
            char arg[COS_SYSCALL_PATH_MAX], pw[64];
            if (r->rax == SYS_KMOD_LOAD ? !cos_syscall_copy_path(arg, sizeof(arg), r->rdi)
                                        : !cos_syscall_copy_user_string(r->rdi, arg, 32)) { r->rax = (uint64_t)-1; break; }
            pw[0] = 0;
            if (r->rsi && !cos_syscall_copy_user_string(r->rsi, pw, sizeof(pw))) { r->rax = (uint64_t)-1; break; }
            { process_t *kp = process_get_current(); if (!kp || kp->caps != COS_CAP_ALL) { r->rax = (uint64_t)-1; break; } }      /* belt and braces: the gate above already refuses a confined caller */
            if (!cos_admin_auth(pw)) { serial_puts("[KMOD] refused: wrong or locked-out administrator password\n"); r->rax = (uint64_t)-1; break; }
            r->rax = r->rax == SYS_KMOD_LOAD ? (uint64_t)(int64_t)cos_kmod_load_path(arg) : (uint64_t)(int64_t)cos_kmod_unload(arg);
            break;
        }
        case SYS_KMOD_LIST: {
            char kb[512]; uint64_t cap = r->rsi > sizeof kb ? sizeof kb : r->rsi;
            if (!r->rdi || cap == 0) { r->rax = (uint64_t)-1; break; }
            int cnt = cos_kmod_list(kb, cap);
            cos_syscall_touch_user_range(r->rdi, cap, true);
            if (!paging_user_range_ok(r->rdi, cap, true)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rdi, kb, (size_t)cap);
            r->rax = (uint64_t)cnt;
            break;
        }
        case SYS_UI_PREFS:
            r->rax = cos_fm_ui_prefs();
            break;
        case SYS_LISTDIR_EX: {
            /* rdi = path, rsi = user array of cos_dirent_ex_t, rdx = capacity. Returns the count. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) { r->rax = (uint64_t)-1; break; }
            uint64_t cap = r->rdx;
            if (cap == 0 || cap > COS_LISTDIR_MAX) cap = COS_LISTDIR_MAX;
            uint64_t bytes = cap * sizeof(cos_dirent_ex_t);
            cos_syscall_touch_user_range(r->rsi, bytes, true);
            if (!r->rsi || !paging_user_range_ok(r->rsi, bytes, true)) { r->rax = (uint64_t)-1; break; }
            /* list into a kernel buffer first: the user pages may fault, and the FS lock must not be held across that */
            cos_dirent_ex_t *tmp = (cos_dirent_ex_t *)kmalloc((size_t)bytes);
            if (!tmp) { r->rax = (uint64_t)-1; break; }
            int n = cos_fm_listdir_ex(path, tmp, (int)cap);
            if (n > 0) memcpy((void *)(uintptr_t)r->rsi, tmp, (size_t)n * sizeof(cos_dirent_ex_t));
            kfree(tmp);
            r->rax = (uint64_t)(int64_t)n;
            break;
        }
        case SYS_CLIP_SET: {
            uint64_t len = r->rsi;
            if (len > COS_CLIP_MAX) { r->rax = (uint64_t)-1; break; }
            if (len) {
                cos_syscall_touch_user_range(r->rdi, len, false);
                if (!paging_user_range_ok(r->rdi, len, false)) { r->rax = (uint64_t)-1; break; }
            }
            uint8_t *tmp = len ? (uint8_t *)kmalloc((size_t)len) : NULL;
            if (len && !tmp) { r->rax = (uint64_t)-1; break; }
            if (len) memcpy(tmp, (void *)(uintptr_t)r->rdi, (size_t)len);
            r->rax = (uint64_t)cos_fm_clip_set(tmp, len);
            if (tmp) kfree(tmp);
            break;
        }
        case SYS_CLIP_GET: {
            uint64_t cap = r->rsi;
            if (!r->rdi) { r->rax = (uint64_t)cos_fm_clip_get(NULL, 0); break; }      /* length query */
            if (cap > COS_CLIP_MAX) cap = COS_CLIP_MAX;
            cos_syscall_touch_user_range(r->rdi, cap, true);
            if (!paging_user_range_ok(r->rdi, cap, true)) { r->rax = (uint64_t)-1; break; }
            uint8_t *tmp = (uint8_t *)kmalloc((size_t)cap ? (size_t)cap : 1);
            if (!tmp) { r->rax = (uint64_t)-1; break; }
            int64_t n = cos_fm_clip_get(tmp, cap);
            if (n > 0) memcpy((void *)(uintptr_t)r->rdi, tmp, (size_t)n);
            kfree(tmp);
            r->rax = (uint64_t)n;
            break;
        }

        case SYS_RTC_NOW: {
            /* 6 x uint32_t: year, month, day, hour, minute, second - a stable, ABI-owned
             * layout for ring-3 rather than handing out rtc_time_t (src/kernel/rtc.h)
             * itself, which is free to change shape since nothing outside the kernel
             * currently sees it. */
            cos_syscall_touch_user_range(r->rdi, 6 * sizeof(uint32_t), true);
            if (!paging_user_range_ok(r->rdi, 6 * sizeof(uint32_t), true)) { r->rax = (uint64_t)-1; break; }
            rtc_time_t t = rtc_get_datetime();
            uint32_t out[6] = { (uint32_t)t.year, t.month, t.day, t.hour, t.minute, t.second };
            memcpy((void *)(uintptr_t)r->rdi, out, sizeof(out));
            r->rax = 0;
            break;
        }

        case SYS_SETTINGS_GET: {
            /* Layout must match cos_settings_t in userland/include/cos.h exactly - see
             * there for the field-by-field rationale. There is no shared header between
             * kernel and userland here (the two build with different include paths, the
             * same reason cos_stat_ex_t is hand-mirrored rather than #included), so this
             * struct is written out by hand to match, field for field, in the same order. */
            struct {
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
                int32_t  theme_idx;
                int32_t  font_scale;
                int32_t  wallpaper_idx;
                int32_t  wallpaper_count;
                int32_t  dark_mode;
                int32_t  japanese;
                int32_t  icon_size;
                int32_t  file_count;
                int32_t  developer_mode;
                uint32_t auto_save_interval;
                int32_t  font_family;
                int32_t  font_pixel_width;
                int32_t  render_backend;
                int32_t  render_backend_gpu_available;
                int32_t  gpu_full_draw;
                int32_t  virgl_available;
                int32_t  window_animations;
                int32_t  notifications_enabled;
                int32_t  fps_overlay;
                int32_t  autostart_terminal;
                int32_t  autostart_file_manager;
                int32_t  autostart_browser;
                int32_t  mouse_sensitivity;
                int32_t  mouse_raw_input;
                int32_t  mouse_drag_threshold;
                int32_t  multi_cursor;
                int32_t  efm_show_hidden;
                int32_t  efm_confirm_delete;
                int32_t  efm_default_view;
                int32_t  terminal_autoscroll;
                int32_t  has_password;
                int32_t  desktop_color;
                int32_t  desktop_color2;
            } out;
            memset(&out, 0, sizeof(out));

            out.mem_total = phys_get_total_memory();
            out.mem_used  = kmemory_used();
            out.mem_free  = kmemory_free();
            out.storage_total = storage_get_total_space();
            out.storage_used  = storage_get_used_space();
            out.storage_free  = storage_get_free_space();

            bios_system_info_t sys;
            bios_get_system_info_copy(&sys);
            snprintf(out.cpu_vendor, sizeof(out.cpu_vendor), "%s", sys.cpu_vendor[0] ? sys.cpu_vendor : "x86 Compatible");
            snprintf(out.os_version, sizeof(out.os_version), "%s", COS_VERSION_STRING);
            snprintf(out.build_info, sizeof(out.build_info), "%s", COS_BOOT_MESSAGE);

            out.screen_w = (int32_t)SCREEN_W;
            out.screen_h = (int32_t)SCREEN_H;

            net_iface_t *iface = net_get_default_iface();
            out.net_connected = net_is_connected() ? 1 : 0;
            if (iface) {
                snprintf(out.net_iface, sizeof(out.net_iface), "%s", iface->name);
                snprintf(out.net_ip, sizeof(out.net_ip), "%d.%d.%d.%d",
                         iface->ip.addr[0], iface->ip.addr[1], iface->ip.addr[2], iface->ip.addr[3]);
                snprintf(out.net_mac, sizeof(out.net_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                         iface->mac.addr[0], iface->mac.addr[1], iface->mac.addr[2],
                         iface->mac.addr[3], iface->mac.addr[4], iface->mac.addr[5]);
                snprintf(out.net_netmask, sizeof(out.net_netmask), "%d.%d.%d.%d",
                         iface->netmask.addr[0], iface->netmask.addr[1], iface->netmask.addr[2], iface->netmask.addr[3]);
                snprintf(out.net_gateway, sizeof(out.net_gateway), "%d.%d.%d.%d",
                         iface->gateway.addr[0], iface->gateway.addr[1], iface->gateway.addr[2], iface->gateway.addr[3]);
            }

            out.theme_idx       = (int32_t)gui_get_theme_idx();
            out.font_scale      = (int32_t)gui_get_font_scale();
            out.wallpaper_idx   = 0;
            out.wallpaper_count = (int32_t)gui_get_wallpaper_count();
            out.dark_mode       = gui_dark_mode ? 1 : 0;
            out.japanese        = gui_is_japanese() ? 1 : 0;
            out.icon_size       = 64;
            out.file_count      = (int32_t)fs_entry_count();

            out.developer_mode               = settings_get_developer_mode() ? 1 : 0;
            out.auto_save_interval           = settings_get_auto_save_interval();
            out.font_family                  = (int32_t)gui_get_font_family();
            out.font_pixel_width             = (int32_t)gui_get_font_pixel_width();
            out.render_backend               = (int32_t)gui_get_render_backend();
            out.render_backend_gpu_available = gui_render_backend_gpu_available() ? 1 : 0;
            out.gpu_full_draw                = gui_get_gpu_full_draw() ? 1 : 0;
            out.virgl_available              = vga_gpu_virgl_available() ? 1 : 0;
            out.window_animations            = gui_get_window_animations() ? 1 : 0;
            out.notifications_enabled        = gui_get_notifications_enabled() ? 1 : 0;
            out.fps_overlay                  = gui_get_fps_overlay() ? 1 : 0;
            out.autostart_terminal           = gui_get_autostart_terminal() ? 1 : 0;
            out.autostart_file_manager       = gui_get_autostart_file_manager() ? 1 : 0;
            out.autostart_browser            = gui_get_autostart_browser() ? 1 : 0;
            out.mouse_sensitivity            = (int32_t)gui_get_mouse_sensitivity();
            out.mouse_raw_input              = gui_get_mouse_raw_input() ? 1 : 0;
            out.mouse_drag_threshold         = (int32_t)gui_get_mouse_drag_threshold();
            out.multi_cursor                 = gui_get_multi_cursor_enabled() ? 1 : 0;
            out.efm_show_hidden              = efm_get_default_show_hidden() ? 1 : 0;
            out.efm_confirm_delete           = efm_get_confirm_delete() ? 1 : 0;
            out.efm_default_view             = (int32_t)efm_get_default_view_mode();
            out.terminal_autoscroll          = gui_get_terminal_autoscroll() ? 1 : 0;
            out.has_password                 = user_has_account() ? 1 : 0;
            { extern uint32_t gui_desktop_color_top, gui_desktop_color_bottom; out.desktop_color = (int32_t)gui_desktop_color_top; out.desktop_color2 = (int32_t)gui_desktop_color_bottom; }

            cos_syscall_touch_user_range(r->rdi, sizeof(out), true);
            if (!paging_user_range_ok(r->rdi, sizeof(out), true)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rdi, &out, sizeof(out));
            r->rax = 0;
            break;
        }

        case SYS_SETTINGS_SET: {
            extern void gui_normalize_desktop_icons(void) __attribute__((weak));
            int key = (int)r->rdi, value = (int)r->rsi;
            bool ok = true;
            switch (key) {
                case COS_SET_THEME:      gui_set_theme_idx(value); break;
                case COS_SET_FONT_SCALE: gui_set_font_scale(value); break;
                case COS_SET_WALLPAPER:  gui_set_wallpaper(value); break;
                case COS_SET_DARK_MODE:  gui_dark_mode = value ? 1 : 0; break;
                case COS_SET_LANGUAGE:   gui_set_language_idx(value); break;
                case COS_SET_ICON_SIZE:
                    settings_set_desktop_icon_size(value);
                    if (gui_normalize_desktop_icons) gui_normalize_desktop_icons();
                    break;
                case COS_SET_DEVELOPER_MODE:        settings_set_developer_mode(value != 0); break;
                case COS_SET_AUTO_SAVE_INTERVAL:     settings_set_auto_save_interval((uint32_t)value); break;
                case COS_SET_FONT_FAMILY:            gui_set_font_family(value); break;
                case COS_SET_FONT_PIXEL_WIDTH:       gui_set_font_pixel_width(value); break;
                case COS_SET_RENDER_BACKEND:         ok = gui_set_render_backend(value); break;
                case COS_SET_GPU_FULL_DRAW:          ok = gui_set_gpu_full_draw(value != 0); break;
                case COS_SET_WINDOW_ANIMATIONS:      gui_set_window_animations(value != 0); break;
                case COS_SET_NOTIFICATIONS:          gui_set_notifications_enabled(value != 0); break;
                case COS_SET_FPS_OVERLAY:            gui_set_fps_overlay(value != 0); break;
                case COS_SET_AUTOSTART_TERMINAL:     gui_set_autostart_terminal(value != 0); break;
                case COS_SET_AUTOSTART_FILE_MANAGER: gui_set_autostart_file_manager(value != 0); break;
                case COS_SET_AUTOSTART_BROWSER:      gui_set_autostart_browser(value != 0); break;
                case COS_SET_MOUSE_SENSITIVITY:      gui_set_mouse_sensitivity(value); break;
                case COS_SET_MOUSE_RAW_INPUT:        gui_set_mouse_raw_input(value != 0); break;
                case COS_SET_MOUSE_DRAG_THRESHOLD:   gui_set_mouse_drag_threshold(value); break;
                case COS_SET_MULTI_CURSOR:           gui_toggle_multi_cursor_mode(); break;
                case COS_SET_EFM_SHOW_HIDDEN:        efm_set_default_show_hidden(value != 0); break;
                case COS_SET_EFM_CONFIRM_DELETE:     efm_set_confirm_delete(value != 0); break;
                case COS_SET_EFM_DEFAULT_VIEW:       efm_set_default_view_mode(value); break;
                case COS_SET_TERMINAL_AUTOSCROLL:    gui_set_terminal_autoscroll(value != 0); break;
                case COS_SET_DESKTOP_COLOR: case COS_SET_DESKTOP_COLOR2: {
                    extern void gui_set_desktop_color(int which, uint32_t value);
                    gui_set_desktop_color(key == COS_SET_DESKTOP_COLOR ? 0 : 1, (uint32_t)value);
                    break; }
                default: ok = false; break;
            }
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_SETTINGS_ACTION: {
            /* Must match the COS_ACTION_* enum in userland/include/cos.h. */
            enum { ACT_POWER_SLEEP = 1, ACT_POWER_RESTART, ACT_POWER_SHUTDOWN,
                   ACT_NET_DHCP_RENEW, ACT_OPEN_TERMINAL, ACT_SECURITY_FULL_SCAN,
                   ACT_FS_FORMAT_FAT32, ACT_FS_FORMAT_EXFAT, ACT_FS_FORMAT_CANCEL, ACT_FS_QUERY };
            extern void gui_save_settings_snapshot(void);
            int action = (int)r->rdi;
            bool ok = true;
            switch (action) {
                case ACT_POWER_SLEEP:
                    ok = acpi_power_suspend_to_state(3u);
                    break;
                case ACT_POWER_RESTART:
                    gui_save_settings_snapshot();
                    bios_reboot();
                    break;
                case ACT_POWER_SHUTDOWN:
                    gui_save_settings_snapshot();
                    bios_shutdown();
                    break;
                case ACT_NET_DHCP_RENEW:
                    ok = net_get_default_iface() != NULL;
                    if (ok) (void)dhcp_discover();
                    break;
                case ACT_OPEN_TERMINAL: {
                    extern void cos_terminal_open(void);
                    cos_terminal_open();
                    break;
                }
                case ACT_SECURITY_FULL_SCAN:
                    /* No real scanner exists (same as the old kernel-drawn
                     * panel - see gui_apps_settings.c's case 5, a notify
                     * and nothing else). Accepted as a no-op rather than
                     * rejected, so the UI can show "scan started". */
                    break;
                case ACT_FS_FORMAT_FAT32: ok = fs_format_request_set(FS_FORMAT_FAT32); break;
                case ACT_FS_FORMAT_EXFAT: ok = fs_format_request_set(FS_FORMAT_EXFAT); break;
                case ACT_FS_FORMAT_CANCEL: ok = fs_format_request_set(0); break;
                case ACT_FS_QUERY:       /* the volume now (bits 0-1: 0 none, 1 FAT32, 2 exFAT) and the format asked for at the next start (bits 4-5) */
                    r->rax = (uint64_t)((unsigned)fs_current_type() | ((unsigned)fs_format_request_get() << 4));
                    goto settings_action_done;
                default: ok = false; break;
            }
            r->rax = ok ? 0 : (uint64_t)-1;
            settings_action_done:
            break;
        }

        case SYS_PASSWORD_CHANGE: {
            char old_pw[64], new_pw[64];
            if (!cos_syscall_copy_user_string(r->rdi, old_pw, sizeof(old_pw)) ||
                !cos_syscall_copy_user_string(r->rsi, new_pw, sizeof(new_pw)) ||
                !new_pw[0]) {
                r->rax = (uint64_t)-1; break;
            }
            /* guessing the old password: see cos_pw_locked() */
            if (cos_pw_locked()) { r->rax = (uint64_t)-1; break; }
            bool had = user_has_account();
            bool ok = had ? user_change_password(old_pw, new_pw)
                          : user_create_account("Administrator", new_pw);
            if (had) cos_pw_note(ok); else if (ok) cos_pw_note(true);
            r->rax = ok ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_WIN2_SCREEN: {
            cos_syscall_touch_user_range(r->rdi, 2 * sizeof(int32_t), true);
            if (!r->rdi || !paging_user_range_ok(r->rdi, 2 * sizeof(int32_t), true)) { r->rax = (uint64_t)-1; break; }
            extern uint64_t SCREEN_W, SCREEN_H;
            int32_t wh[2] = { (int32_t)SCREEN_W, (int32_t)SCREEN_H };
            memcpy((void*)(uintptr_t)r->rdi, wh, sizeof(wh));
            r->rax = 0;
            break;
        }

        case SYS_IMAGE_DECODE: {
            /* rdi=path, rsi=packed RGB destination, rdx=capacity bytes,
             * r10=two uint64_t output dimensions.  The decode is performed by
             * the same kernel decoder used by the desktop Image Viewer. */
            uint64_t upath = r->rdi, udst = r->rsi, cap = r->rdx, uinfo = r->r10;
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), upath) ||
                cap == 0 || cap > (16ULL * 1024ULL * 1024ULL)) {
                r->rax = (uint64_t)-1; break;
            }
            /* Photo allocates its pixel surface with sbrk-backed malloc;
             * those pages are demand-mapped and must be materialized before
             * the kernel decoder writes into the user destination. */
            cos_syscall_touch_user_range(udst, cap, true);
            cos_syscall_touch_user_range(uinfo, sizeof(uint64_t) * 2, true);
            if (!paging_user_range_ok(udst, cap, true) ||
                !paging_user_range_ok(uinfo, sizeof(uint64_t) * 2, true)) {
                r->rax = (uint64_t)-1; break;
            }
            uint64_t w = 0, h = 0;
            int rc = jpeg_viewer_decode_file_to_rgb(path, (uint32_t *)(uintptr_t)udst, cap, &w, &h);
            if (rc == 0) {
                uint64_t *info = (uint64_t *)(uintptr_t)uinfo;
                info[0] = w; info[1] = h;
            }
            r->rax = (uint64_t)rc;
            break;
        }
        case SYS_AUDIO_OPEN:
            r->rax = (uint64_t)cos_audio_open(cos_syscall_caller_pid(), (uint32_t)r->rdi, (uint32_t)r->rsi);
            break;
        case SYS_AUDIO_WRITE: {
            uint64_t samples = r->rsi;
            if (samples > 65536u) samples = 65536u;      /* bound one call */
            uint64_t bytes = samples * sizeof(int16_t);
            cos_syscall_touch_user_range(r->rdi, bytes, false);
            if (!r->rdi || !paging_user_range_ok(r->rdi, bytes, false)) { r->rax = (uint64_t)-1; break; }
            r->rax = (uint64_t)cos_audio_write(cos_syscall_caller_pid(), (const int16_t*)(uintptr_t)r->rdi, samples);
            break;
        }
        case SYS_AUDIO_STATUS: {
            cos_syscall_touch_user_range(r->rdi, sizeof(cos_audio_status_t), true);
            if (!r->rdi || !paging_user_range_ok(r->rdi, sizeof(cos_audio_status_t), true)) { r->rax = (uint64_t)-1; break; }
            cos_audio_status_t st;
            cos_audio_status(cos_syscall_caller_pid(), &st);
            memcpy((void*)(uintptr_t)r->rdi, &st, sizeof(st));
            r->rax = 0;
            break;
        }
        case SYS_AUDIO_DRAIN:  r->rax = (uint64_t)cos_audio_drain(cos_syscall_caller_pid()); break;
        case SYS_AUDIO_STOP:   r->rax = (uint64_t)cos_audio_stop(cos_syscall_caller_pid()); break;
        case SYS_AUDIO_VOLUME: r->rax = (uint64_t)cos_audio_set_volume(cos_syscall_caller_pid(), (uint32_t)r->rdi); break;
        case SYS_AUDIO_CLOSE:  r->rax = (uint64_t)cos_audio_close(cos_syscall_caller_pid()); break;

        case SYS_WIN_POLL_MOUSE: {
            /* Same convention as SYS_WIN_POLL_KEY just above, for
             * cos_app_mouse_event_t (x, y, button) instead - one queued
             * click at a time, oldest first. */
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_app_mouse_event_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_app_mouse_event_t), true)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_app_mouse_event_t ev;
            bool got = cos_app_window_poll_mouse((int64_t)r->rdi, cos_syscall_caller_pid(), &ev);
            if (got) {
                memcpy((void *)(uintptr_t)r->rsi, &ev, sizeof(ev));
                r->rax = 1;
            } else {
                r->rax = 0;
            }
            break;
        }

        case SYS_THREAD_CREATE: {
            /* rdi = ring3 entry point, rsi = argument passed in RDI to
             * that entry. Returns a thread id (>0) or -1.
             *
             * The entry point is validated as executable USER memory
             * before use: it comes from ring3, and thread_create() would
             * otherwise happily start a thread at a kernel address, which
             * is the same class of bug as the old signal handler path
             * that called ring3 pointers from ring0. */
            process_t *proc = process_get_current();
            if (!proc || proc->thread_count >= 32) { r->rax = (uint64_t)-1; break; }
            if (!paging_user_range_ok(r->rdi, 1, false)) {
                serial_puts("[SYSCALL] SYS_THREAD_CREATE rejected: entry not user memory\n");
                r->rax = (uint64_t)-1; break;
            }
            /* rdx = top of a stack the caller allocated for the thread (0: the process stack top, which only one thread can use). */
            uint64_t user_sp = r->rdx;
            if (user_sp && (user_sp < 4096 || !paging_user_range_ok(user_sp - 16, 16, true))) {
                serial_puts("[SYSCALL] SYS_THREAD_CREATE rejected: stack not user memory\n");
                r->rax = (uint64_t)-1; break;
            }
            thread_t *th = thread_create_user_sp(proc, (void *)(uintptr_t)r->rdi,
                                                 (void *)(uintptr_t)r->rsi, KERNEL_STACK_SIZE, user_sp);
            r->rax = th ? (uint64_t)th->tid : (uint64_t)-1;
            break;
        }

        case SYS_THREAD_EXIT: {
            thread_t *cur = scheduler_get_current_thread();
            if (cur) thread_exit(cur, (int)r->rdi);
            /* thread_exit() does not return for the current thread. */
            for (;;) { __asm__ volatile("hlt"); }
        }

        case SYS_THREAD_JOIN: {
            /* rdi = thread id, rsi = timeout ms (0 = poll once).
             * Returns 0 when the thread has finished, -1 on timeout or a
             * bad id.
             *
             * Polls rather than blocking on a wait queue: unlike
             * SYS_WAITPID (which the process exit path can wake, because
             * process_exit() runs at a single well-defined point), thread
             * teardown has no equivalent single hook to wake a waiter
             * from, and adding one touches the scheduler's thread
             * lifecycle. Polling is the honest interim - stated here
             * rather than implied - and bounded by the caller's timeout
             * so it cannot spin forever. */
            process_t *proc = process_get_current();
            if (!proc) { r->rax = (uint64_t)-1; break; }
            uint64_t timeout = r->rsi;
            if (timeout > COS_SYSCALL_SLEEP_MAX_MS) timeout = COS_SYSCALL_SLEEP_MAX_MS;
            uint64_t waited = 0;
            for (;;) {
                bool alive = false;
                for (thread_t *t = proc->thread_list; t; t = t->proc_next) {
                    if (t->tid == (uint64_t)r->rdi &&
                        t->state != TASK_ZOMBIE && t->state != TASK_UNUSED) {
                        alive = true;
                        break;
                    }
                }
                if (!alive) { r->rax = 0; break; }
                if (waited >= timeout) { r->rax = (uint64_t)-1; break; }
                uint64_t step = (timeout - waited > 5) ? 5 : (timeout - waited);
                if (step == 0) step = 1;
                scheduler_sleep(step);
                waited += step;
            }
            break;
        }

        case SYS_TIME_MS: {
            /* TIMER_TICKS_PER_SEC is 1000 (timer.h), so ticks are already
             * milliseconds - converted explicitly anyway so this stays
             * correct if that rate is ever changed. */
            extern uint64_t timer_get_ms_exact(void);
            r->rax = timer_get_ms_exact();            /* tick count corrected by the RTC/TSC clock (timer.c): ticks are lost under load */
            break;
        }

        case SYS_TIME_US: { extern uint64_t timer_get_us_exact(void); r->rax = timer_get_us_exact(); break; }

        case SYS_TIME_UNIX: {
            r->rax = rtc_get_seconds_since_epoch();      /* real Unix time (it used to be the time of day only) */
            break;
        }

        case SYS_MKDIR: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            if (!cos_fs_gate(COSYS_FSOP_MKDIR, path, NULL)) { r->rax = (uint64_t)-1; break; }
            r->rax = cos_fs_mkdir(path) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_UNLINK: {
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            if (!cos_fs_gate(COSYS_FSOP_UNLINK, path, NULL)) { r->rax = (uint64_t)-1; break; }
            r->rax = cos_fs_unlink(path) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_RENAME: {
            char oldp[COS_SYSCALL_PATH_MAX], newp[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(oldp, sizeof(oldp), r->rdi) ||
                !cos_syscall_copy_path(newp, sizeof(newp), r->rsi)) {
                r->rax = (uint64_t)-1; break;
            }
            if (!cos_fs_gate(COSYS_FSOP_RENAME, oldp, newp)) { r->rax = (uint64_t)-1; break; }
            r->rax = cos_fs_rename(oldp, newp) ? 0 : (uint64_t)-1;
            break;
        }

        case SYS_STAT: {
            /* rdi = path, rsi = user pointer to a cos_stat_t.
             * Returns 0, or -1 if the path does not exist. */
            char path[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(path, sizeof(path), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(r->rsi, sizeof(cos_stat_t), true);
            if (!paging_user_range_ok(r->rsi, sizeof(cos_stat_t), true)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_stat_t st;
            if (!cos_fs_stat(path, &st.size, &st.is_dir)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rsi, &st, sizeof(st));
            r->rax = 0;
            break;
        }

        case SYS_NET_AVAILABLE: {
            r->rax = net_api_is_available() ? 1 : 0;
            break;
        }

        case SYS_NET_RESOLVE: {
            /* rdi = hostname (user), rsi = user pointer to 4 bytes.
             * Returns 0 on success, -1 on failure. */
            char host[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(host, sizeof(host), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            cos_syscall_touch_user_range(r->rsi, 4, true);
            if (!paging_user_range_ok(r->rsi, 4, true)) {
                r->rax = (uint64_t)-1; break;
            }
            uint8_t ip[4];
            if (!net_api_dns_resolve(host, ip)) { r->rax = (uint64_t)-1; break; }
            memcpy((void *)(uintptr_t)r->rsi, ip, 4);
            r->rax = 0;
            break;
        }

        case SYS_NET_HTTP_GET: {
            /* rdi = url (user), rsi = destination buffer (user),
             * rdx = buffer size. Returns bytes received, or -1.
             *
             * Goes through a kernel bounce buffer for the same reason the
             * file syscalls do: the request blocks for a potentially long
             * time inside the network stack, and the user mapping must not
             * be relied on to stay put across that. */
            char url[COS_SYSCALL_PATH_MAX];
            if (!cos_syscall_copy_path(url, sizeof(url), r->rdi)) {
                r->rax = (uint64_t)-1; break;
            }
            uint64_t ubuf = r->rsi, ulen = r->rdx;
            if (ulen == 0 || ulen > COS_SYSCALL_FILE_MAX) { r->rax = (uint64_t)-1; break; }
            cos_syscall_touch_user_range(ubuf, ulen, true);
            if (!paging_user_range_ok(ubuf, ulen, true)) {
                serial_puts("[SYSCALL] SYS_NET_HTTP_GET rejected: bad destination buffer\n");
                r->rax = (uint64_t)-1; break;
            }
            void *bounce = kmalloc((size_t)ulen);
            if (!bounce) { r->rax = (uint64_t)-1; break; }
            uint64_t got = 0;
            bool ok = net_api_http_get(url, (char *)bounce, ulen, &got);
            if (ok && got > 0) {
                if (got > ulen) got = ulen;
                cos_syscall_touch_user_range(ubuf, got, true);
                if (!paging_user_range_ok(ubuf, got, true)) { kfree(bounce); r->rax = (uint64_t)-1; break; }   /* the request blocked: check the buffer again */
                memcpy((void *)(uintptr_t)ubuf, bounce, (size_t)got);
            }
            kfree(bounce);
            r->rax = ok ? got : (uint64_t)-1;
            break;
        }

        case SYS_EXIT: {
            int code = (int)r->rdi;
            serial_puts("[SYSCALL] ring3 thread exiting via SYS_EXIT, code=");
            serial_putdec((uint64_t)(int64_t)code);
            serial_puts("\n");
            /* exit() ends the whole process, whatever other threads it has (a thread alone ends with SYS_THREAD_EXIT): before, the process stayed
             * alive for as long as any thread of it ran, and a waiting parent waited with it. */
            { process_t *ep = process_get_current(); if (ep && ep->type == TASK_TYPE_USER) process_exit(ep, code); }
            thread_exit(scheduler_get_current_thread(), code);
            /* thread_exit() does not return for the current thread - it
             * preempts into another task. If we ever do get here, the
             * process/thread bookkeeping is in an unexpected state; fail
             * safe rather than falling through to arbitrary ring3 code. */
            for (;;) { __asm__ volatile("hlt"); }
        }
        default:
            {   extern bool cos_kmod_syscall_known(uint64_t nr); extern int64_t cos_kmod_syscall(uint64_t nr, uint64_t a, uint64_t b, uint64_t c);
                if (cos_kmod_syscall_known(r->rax)) { r->rax = (uint64_t)cos_kmod_syscall(r->rax, r->rdi, r->rsi, r->rdx); break; }   /* a kernel module (.c-osys) provides it */
            }
            serial_puts("[SYSCALL] unknown syscall number=");
            serial_putdec(r->rax);
            serial_puts("\n");
            r->rax = (uint64_t)-1;
            break;
    }

    /* Checked on every syscall return, including SYS_SIGRETURN's own -
     * see the comment there for why a second signal arriving during
     * handling must not be lost. */
    cos_deliver_pending_signal(r);
}

/* SMAP: the interrupt stub executed clac on entry, so user memory is off limits to the kernel. A syscall handler
 * deliberately reads and writes the caller's buffers (after paging_user_range_ok), so it runs with the AC flag set. */
extern volatile uint8_t cos_smap_active;
static void syscall_handler(struct regs* r) {
    if (cos_smap_active) __asm__ volatile("stac" ::: "cc");
    syscall_handler_impl(r);
    if (cos_smap_active) __asm__ volatile("clac" ::: "cc");
}

void syscall_init(void) {
    register_interrupt_handler(128, syscall_handler);
    serial_puts("[SYSCALL] int 0x80 syscall gate registered (SYS_WRITE, SYS_EXIT, SYS_WIN_*)\n");
}
