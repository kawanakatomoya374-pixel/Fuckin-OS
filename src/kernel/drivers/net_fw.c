#include "sync.h"
#include "net_fw.h"
#include "string.h"
#include "serial.h"

static fw_rule_t g_rules[FW_MAX_RULES];
static int g_nrules;
static bool g_default_deny;
static volatile uint32_t g_allowed, g_denied;
static volatile uint32_t g_lock;

static void lock(void) { cos_spin_lock(&g_lock); }
static void unlock(void) { cos_spin_unlock(&g_lock); }

void fw_init(void) { g_nrules = 0; g_default_deny = false; g_allowed = g_denied = 0; g_lock = 0; }
void fw_clear(void) { lock(); g_nrules = 0; unlock(); }
void fw_set_default_deny(bool deny) { g_default_deny = deny; }
void fw_get_counters(uint32_t* a, uint32_t* d) { if (a) *a = g_allowed; if (d) *d = g_denied; }

int fw_add_rule(const fw_rule_t* r) {
    if (!r) return -1;
    lock();
    int idx = -1;
    if (g_nrules < FW_MAX_RULES) { g_rules[g_nrules] = *r; idx = g_nrules++; }
    unlock();
    return idx;
}

int fw_check(uint8_t dir, uint8_t proto, const ip_addr_t* remote, const void* payload, size_t len) {
    /* remote port: TCP/UDP headers start src,dst port (big endian); the remote's port is src when receiving, dst when sending */
    uint16_t rport = 0;
    if ((proto == FW_TCP || proto == FW_UDP) && payload && len >= 4) {
        const uint8_t* p = (const uint8_t*)payload;
        size_t off = (dir == FW_IN) ? 0 : 2;
        rport = (uint16_t)((p[off] << 8) | p[off + 1]);
    }
    int allow = !g_default_deny;
    lock();
    for (int i = 0; i < g_nrules; ++i) {
        const fw_rule_t* r = &g_rules[i];
        if (!(r->dir & dir)) continue;
        if (r->proto != FW_ANY && r->proto != proto) continue;
        if (r->port_hi && (rport < r->port_lo || rport > r->port_hi)) continue;
        bool net_ok = true;
        for (int k = 0; k < 4; ++k) if ((remote->addr[k] & r->mask.addr[k]) != (r->net.addr[k] & r->mask.addr[k])) { net_ok = false; break; }
        if (!net_ok) continue;
        allow = !r->deny;
        break;
    }
    unlock();
    if (allow) g_allowed++; else g_denied++;
    return allow;
}
