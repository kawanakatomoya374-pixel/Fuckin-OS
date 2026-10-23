/* netfw.c - a SYSTEM-level kernel extension on the network path: frame counters and a small firewall.
 *
 * It registers a frame filter (cosys_net_filter_register): the kernel shows it every Ethernet frame before it is sent and before the stack handles a received one.
 *   0x1034  stats(buf)      copies { rx_frames, rx_bytes, tx_frames, tx_bytes, dropped, rewritten, seen_redirected, bad_checksums } (8 x uint64) to buf. 0, or -1
 *   0x1035  rule(on)        on = 1: drop every outgoing IPv4/UDP frame; 0: off. Only a fully privileged (unsandboxed) caller may. 0, or -1
 *   0x1036  redirect(from, to)  rewrites the destination UDP port `from` of outgoing frames to `to` (and the source port `to` of received frames back to `from`); from = 0: off.
 *                           Fully privileged callers only. 0, or -1
 * The filter only reads the frame. The rewriter (registered too) changes it in place and fixes the checksums; the filter, which runs after it, then sees and checks the result. */
#include "cosys.h"

static uint64_t g_rx, g_rxb, g_tx, g_txb, g_drop, g_rewritten, g_seen_redirected, g_bad;
static volatile uint32_t g_from, g_to;
static volatile uint32_t g_block_udp_tx;
static cosys_lock_t g_lock;

/* the UDP header of an unfragmented IPv4/UDP frame, or NULL */
static uint8_t *udp_of(uint8_t *f, size_t len, size_t *ip_len) {
    if (len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4 || f[23] != 17) return 0;
    size_t ihl = (size_t)(f[14] & 0x0Fu) * 4u;
    if (ihl < 20 || 14 + ihl + 8 > len || (((f[20] << 8 | f[21]) & 0x3FFF) != 0)) return 0;
    *ip_len = ihl;
    return f + 14 + ihl;
}
static uint32_t sum16(uint32_t s, const uint8_t *p, size_t n) { for (; n > 1; p += 2, n -= 2) s += (uint32_t)p[0] << 8 | p[1]; if (n) s += (uint32_t)p[0] << 8; return s; }
static int checksums_ok(const uint8_t *f, size_t len, size_t ihl, const uint8_t *u) {
    uint32_t s = sum16(0, f + 14, ihl);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    if (s != 0xFFFF) return 0;
    size_t ul = (size_t)(u[4] << 8 | u[5]);
    if ((u[6] | u[7]) == 0 || 14 + ihl + ul > len) return 1;
    s = sum16(0, f + 14 + 12, 8) + 17 + (uint32_t)ul;
    s = sum16(s, u, ul);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return s == 0xFFFF;
}
static int64_t on_rewrite(uint32_t dir, uint8_t *f, size_t len, size_t cap) {
    (void)cap;
    uint32_t from = g_from, to = g_to;
    size_t ihl;
    if (!from) return (int64_t)len;
    uint8_t *u = udp_of(f, len, &ihl);
    if (!u) return (int64_t)len;
    uint32_t port_at = dir == COSYS_NET_TX ? 2u : 0u;                              /* TX: destination port; RX: source port */
    uint32_t cur = (uint32_t)(u[port_at] << 8 | u[port_at + 1]);
    uint32_t match = dir == COSYS_NET_TX ? from : to, repl = dir == COSYS_NET_TX ? to : from;
    if (cur != match) return (int64_t)len;
    u[port_at] = (uint8_t)(repl >> 8); u[port_at + 1] = (uint8_t)repl;
    cosys_net_fix_checksums(f, len);
    uint64_t fl = cosys_lock(&g_lock); g_rewritten++; cosys_unlock(&g_lock, fl);
    return (int64_t)len;
}
static int on_frame(uint32_t dir, const uint8_t *f, size_t len) {
    int drop = 0;
    {
        size_t ihl; uint8_t *u = udp_of((uint8_t *)(uintptr_t)f, len, &ihl);
        if (u && g_from) {
            uint64_t fl = cosys_lock(&g_lock);
            if (dir == COSYS_NET_TX && (u[2] << 8 | u[3]) == (int)g_to) { g_seen_redirected++; if (!checksums_ok(f, len, ihl, u)) g_bad++; }
            cosys_unlock(&g_lock, fl);
        }
    }
    if (dir == COSYS_NET_TX && g_block_udp_tx && len >= 24 && f[12] == 0x08 && f[13] == 0x00 && f[23] == 17) drop = 1;     /* ethertype IPv4, protocol UDP */
    uint64_t fl = cosys_lock(&g_lock);
    if (dir == COSYS_NET_RX) { g_rx++; g_rxb += len; } else { g_tx++; g_txb += len; }
    if (drop) g_drop++;
    cosys_unlock(&g_lock, fl);
    return drop;
}
static int64_t stats(uint64_t ubuf, uint64_t b, uint64_t c) {
    (void)b; (void)c;
    uint64_t out[8];
    uint64_t fl = cosys_lock(&g_lock);
    out[0] = g_rx; out[1] = g_rxb; out[2] = g_tx; out[3] = g_txb; out[4] = g_drop; out[5] = g_rewritten; out[6] = g_seen_redirected; out[7] = g_bad;
    cosys_unlock(&g_lock, fl);
    return cosys_copy_to_user(ubuf, out, sizeof out) == 0 ? 0 : -1;
}
static int64_t rule(uint64_t on, uint64_t b, uint64_t c) {
    (void)b; (void)c;
    if (cosys_current_caps() != 0xFFFFFFFFu) return -1;
    g_block_udp_tx = on ? 1u : 0u;
    return 0;
}
static int64_t redirect(uint64_t from, uint64_t to, uint64_t c) {
    (void)c;
    if (cosys_current_caps() != 0xFFFFFFFFu || from > 65535 || to > 65535 || (from && !to)) return -1;
    g_to = (uint32_t)to; g_from = (uint32_t)from;
    return 0;
}
static int nf_init(void) {
    if (cosys_net_filter_register(on_frame) != 0) return -1;
    if (cosys_net_rewrite_register(on_rewrite) != 0) { cosys_net_filter_unregister(on_frame); return -1; }
    if (cosys_register_syscall(0x1034, stats, 0) != 0 || cosys_register_syscall(0x1035, rule, 0) != 0 || cosys_register_syscall(0x1036, redirect, 0) != 0) {
        cosys_net_rewrite_unregister(on_rewrite); cosys_net_filter_unregister(on_frame); cosys_unregister_syscall(0x1034); cosys_unregister_syscall(0x1035); return -1;
    }
    cosys_log("network filter active");
    return 0;
}
static void nf_exit(void) {
    cosys_net_filter_unregister(on_frame); cosys_net_rewrite_unregister(on_rewrite);
    cosys_unregister_syscall(0x1034); cosys_unregister_syscall(0x1035); cosys_unregister_syscall(0x1036);
    cosys_log_dec("frames seen: ", g_rx + g_tx);
}
COSYS_MODULE("netfw", "1.0", COSYS_LEVEL_SYSTEM, nf_init, nf_exit);
