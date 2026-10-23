/* cos_kevent.h - events the kernel reports to subscribed .c-osys modules (cosys_subscribe in cosys.h). The kernel calls cos_kmod_event() at the places listed
 * here; a module sees (type, a, b, c). Callbacks run in the context of whoever caused the event (a system call, process creation or exit), possibly with
 * interrupts off and on any CPU: they must be short, must not block or sleep, and must protect their own data (cosys_lock). */
#ifndef COS_KEVENT_H
#define COS_KEVENT_H
#include <stdint.h>
#include <stddef.h>
#define COSYS_EV_PROC_START   0   /* a = pid, b = parent pid                                   (process creation) */
#define COSYS_EV_PROC_EXIT    1   /* a = pid, b = exit status                                  (process_exit) */
#define COSYS_EV_SANDBOX_DENY 2   /* a = pid, b = system call number, c = capabilities it lacked   (the sandbox refused a call) */
#define COSYS_EV_SANDBOX_SET  3   /* a = pid, b = capabilities now held                         (SYS_SANDBOX narrowed them) */
#define COSYS_EV_COUNT        4
void cos_kmod_event(uint32_t type, uint64_t a, uint64_t b, uint64_t c);       /* kernel side: cheap when nobody subscribed */

/* ---- filters (level SYSTEM): the kernel asks the subscribed modules before it acts, and a module may refuse ---- */
/* File operations of programs (path-based system calls). op: */
#define COSYS_FSOP_READ    1   /* SYS_READ_FILE, SYS_FD_OPEN for reading                        path */
#define COSYS_FSOP_WRITE   2   /* SYS_WRITE_FILE, SYS_FD_OPEN for writing or creating           path */
#define COSYS_FSOP_MKDIR   3   /* SYS_MKDIR                                                     path */
#define COSYS_FSOP_UNLINK  4   /* SYS_UNLINK                                                    path */
#define COSYS_FSOP_RENAME  5   /* SYS_RENAME                                                    path = old, path2 = new */
/* 1 = allow, 0 = a module refused (the system call then fails). The paths are kernel copies, valid for the call. */
int cos_kmod_fs_allow(uint32_t op, uint64_t pid, const char *path, const char *path2);
/* Ethernet frames, before they are sent (dir = COSYS_NET_TX) and before the stack sees a received frame (COSYS_NET_RX). 1 = pass, 0 = drop. */
#define COSYS_NET_RX 0
#define COSYS_NET_TX 1
int cos_kmod_net_pass(uint32_t dir, const void *frame, size_t len);
/* Rewriters (SYSTEM): may change the frame in place (buffer of `cap` bytes, *len updated). 1 = go on, 0 = a module dropped it. cos_kmod_net_pass sees the rewritten frame. */
int cos_kmod_net_rewrite_active(void);
int cos_kmod_net_rewrite(uint32_t dir, void *frame, size_t *len, size_t cap);
#define COSYS_NET_SLACK 256
#endif
