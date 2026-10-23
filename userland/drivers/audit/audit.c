/* audit.c - a kernel extension: an append-only audit log of what the kernel's sandbox and process machinery does.
 *
 * It subscribes to kernel events (cosys_subscribe) and keeps the last 256 in a ring buffer:
 *   process started / exited, a system call the sandbox refused (with the capabilities it lacked), a process narrowing its capabilities.
 * Two system calls expose it to programs:
 *   0x1010  audit_read(buf, max, from_seq)   copies up to `max` records (struct audit_rec, 40 bytes) with seq >= from_seq, oldest first; returns how many
 *                                            (-1: bad buffer). Needs COS_CAP_SYSINFO.
 *   0x1011  audit_next()                     the sequence number the next event will get (= how many events were seen since the module loaded).
 * The log cannot be cleared or edited from user space; it ends when the module unloads. */
#include "cosys.h"

struct audit_rec { uint64_t seq, time_ms; uint32_t type, pid; uint64_t a, b; };      /* mirrored in userland/include/cos.h (cos_audit_rec_t) */
#define RING 256u
static struct audit_rec g_ring[RING];
static uint64_t g_next;                                    /* sequence number of the next event */
static cosys_lock_t g_lock;

static void on_event(uint32_t type, uint64_t a, uint64_t b, uint64_t c) {
    /* a = pid for every type; b/c as in cos_kevent.h. The record keeps one detail value: b, except for a sandbox refusal where it packs the call number
     * (low 16 bits) and the missing capabilities (high bits). */
    uint64_t detail = type == COSYS_EV_SANDBOX_DENY ? ((c << 16) | (b & 0xFFFFu)) : b;
    uint64_t fl = cosys_lock(&g_lock);
    struct audit_rec *r = &g_ring[g_next % RING];
    r->seq = g_next; r->time_ms = cosys_time_ms(); r->type = type; r->pid = (uint32_t)a; r->a = type == COSYS_EV_PROC_START ? b : 0; r->b = detail;
    g_next++;
    cosys_unlock(&g_lock, fl);
}

static int64_t audit_read(uint64_t user_buf, uint64_t max, uint64_t from_seq) {
    struct audit_rec tmp[32];
    if (max == 0) return 0;
    if (max > 32) max = 32;
    uint64_t n = 0;
    uint64_t fl = cosys_lock(&g_lock);
    uint64_t first = g_next > RING ? g_next - RING : 0;     /* the oldest record still in the ring */
    uint64_t s = from_seq > first ? from_seq : first;
    for (; s < g_next && n < max; s++) tmp[n++] = g_ring[s % RING];
    cosys_unlock(&g_lock, fl);
    if (n && cosys_copy_to_user(user_buf, tmp, n * sizeof tmp[0]) != 0) return -1;      /* outside the lock: it can fault */
    return (int64_t)n;
}
static int64_t audit_next(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; uint64_t fl = cosys_lock(&g_lock); uint64_t n = g_next; cosys_unlock(&g_lock, fl); return (int64_t)n; }
static int64_t audit_read_call(uint64_t a, uint64_t b, uint64_t c) { return audit_read(a, b, c); }

static int audit_init(void) {
    if (cosys_subscribe((1u << COSYS_EV_PROC_START) | (1u << COSYS_EV_PROC_EXIT) | (1u << COSYS_EV_SANDBOX_DENY) | (1u << COSYS_EV_SANDBOX_SET), on_event) != 0) return -1;
    if (cosys_register_syscall(0x1010, audit_read_call, 0x400 /* COS_CAP_SYSINFO */) != 0) return -1;
    if (cosys_register_syscall(0x1011, audit_next, 0x400) != 0) return -1;
    cosys_log("audit log running");
    return 0;
}
static void audit_exit(void) {
    cosys_unsubscribe(on_event);
    cosys_unregister_syscall(0x1010); cosys_unregister_syscall(0x1011);
    cosys_log_dec("events recorded: ", g_next);
}
COSYS_MODULE("audit", "1.0", COSYS_LEVEL_SERVICE, audit_init, audit_exit);
