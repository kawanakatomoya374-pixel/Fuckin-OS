/*
 * dns.c - Bounded, request-correlated DNS client for C-OS
 *
 * DNS is used by HTTP/TLS workers.  The former single global pending hostname
 * made concurrent resolves unsafe because the most recent caller overwrote the
 * name associated with every in-flight response.  This implementation retains
 * the compact synchronous public API but internally tracks independent query
 * IDs, names and terminal states in a bounded table.
 */
#include "sync.h"
#include "dns.h"
#include "net.h"
#include "udp.h"
#include "ip6.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "timer.h"
#include "task.h"

#define DNS_NEGATIVE_TTL_TICKS 30000u
#define DNS_MAX_INFLIGHT 8u
#define DNS_RESOLVE_WAIT_PASSES 400u

typedef enum {
    DNS_REQUEST_FREE = 0,
    DNS_REQUEST_PENDING,
    DNS_REQUEST_RESOLVED,
    DNS_REQUEST_FAILED
} dns_request_state_t;

typedef struct {
    uint16_t id;
    uint16_t source_port;
    char hostname[DNS_MAX_NAME_LEN];
    ip_addr_t result;
    uint8_t want6;               /* an AAAA query: the answer goes to result6 */
    ip6_addr_t result6;
    uint32_t submitted_at;
    uint8_t retries;             /* retransmissions done by the asynchronous poller */
    dns_request_state_t state;
} dns_request_t;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];
static dns_request_t dns_requests[DNS_MAX_INFLIGHT];
static uint16_t dns_next_id = 0x1234;
extern uint32_t net_rand32(void);
static ip_addr_t dns_server;
static ip6_addr_t dns_server6;                 /* recursive resolver learnt from a Router Advertisement (RDNSS) */
static volatile bool dns_server6_valid;
/* AAAA results: a small cache of their own (the A cache stores IPv4 addresses) */
typedef struct { char name[DNS_MAX_NAME_LEN]; ip6_addr_t ip; uint32_t expires; bool valid; } dns_cache6_t;
static dns_cache6_t dns_cache6[8];
/* The cache, resolver address, ID allocator and request table form one
 * consistency domain.  No network dispatcher call is made while this lock is
 * held, so it cannot deadlock with the single-owner net_poll() gate. */
static volatile uint32_t dns_state_lock = 0;

static uint16_t htons(uint16_t n) {
    return (uint16_t)(((n & 0xFFu) << 8) | ((n >> 8) & 0xFFu));
}
static uint16_t ntohs(uint16_t n) { return htons(n); }
static uint32_t htonl(uint32_t n) {
    return ((n & 0xFFu) << 24) | ((n & 0xFF00u) << 8) |
           ((n >> 8) & 0xFF00u) | ((n >> 24) & 0xFFu);
}
static uint32_t ntohl(uint32_t n) { return htonl(n); }

static void dns_lock_acquire(void) { cos_spin_lock(&dns_state_lock); }

static void dns_lock_release(void) { cos_spin_unlock(&dns_state_lock); }

static void dns_copy_name(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_sz - 1u);
    dst[dst_sz - 1u] = '\0';
}

/* Called only with dns_state_lock held. */
static bool dns_cache_entry_live_locked(dns_cache_entry_t* entry, uint32_t now) {
    if (!entry || !entry->valid) return false;
    if (entry->expires != 0u && now >= entry->expires) {
        entry->valid = 0;
        entry->negative = 0;
        return false;
    }
    return true;
}

/* Called only with dns_state_lock held. */
static void dns_cache_store_locked(const char* name, const ip_addr_t* ip,
                                   uint32_t ttl, bool negative) {
    if (!name || !name[0]) return;
    uint32_t now = (uint32_t)get_timer_ticks();
    int slot = -1;
    uint32_t oldest = 0;
    for (int i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].name, name) == 0) {
            slot = i;
            break;
        }
        if (!dns_cache_entry_live_locked(&dns_cache[i], now)) {
            slot = i;
            break;
        }
        if (slot < 0 || dns_cache[i].timestamp < oldest) {
            slot = i;
            oldest = dns_cache[i].timestamp;
        }
    }
    if (slot < 0) return;
    dns_copy_name(dns_cache[slot].name, sizeof(dns_cache[slot].name), name);
    if (ip) dns_cache[slot].ip = *ip;
    else memset(&dns_cache[slot].ip, 0, sizeof(dns_cache[slot].ip));
    dns_cache[slot].ttl = ttl;
    dns_cache[slot].expires = ttl ? now + ttl : 0u;
    dns_cache[slot].timestamp = now;
    dns_cache[slot].negative = negative ? 1 : 0;
    dns_cache[slot].valid = 1;
}

static const uint8_t* dns_skip_name(const uint8_t* ptr, const uint8_t* end) {
    int labels = 0;
    while (ptr && ptr < end) {
        uint8_t len = *ptr;
        if (len == 0u) return ptr + 1;
        if ((len & 0xC0u) == 0xC0u) {
            if (ptr + 2 > end) return NULL;
            return ptr + 2;
        }
        if (len > 63u) return NULL;
        ++ptr;
        if (ptr + len > end) return NULL;
        ptr += len;
        if (++labels > 128) return NULL;
    }
    return NULL;
}

static int dns_encode_name(uint8_t* buf, size_t buf_sz, const char* name) {
    if (!buf || !name || !name[0]) return -1;
    size_t len = 0;
    const char* pos = name;
    while (*pos) {
        const char* dot = strchr(pos, '.');
        size_t label_len = dot ? (size_t)(dot - pos) : strlen(pos);
        if (label_len == 0u || label_len > 63u || len + label_len + 2u > buf_sz) {
            return -1;
        }
        buf[len++] = (uint8_t)label_len;
        memcpy(buf + len, pos, label_len);
        len += label_len;
        if (!dot) break;
        pos = dot + 1;
    }
    if (len + 1u > buf_sz) return -1;
    buf[len++] = 0;
    return (int)len;
}

/* Called only with dns_state_lock held. */
static dns_request_t* dns_find_request_locked(uint16_t id) {
    for (size_t i = 0; i < DNS_MAX_INFLIGHT; ++i) {
        if (dns_requests[i].state != DNS_REQUEST_FREE && dns_requests[i].id == id) {
            return &dns_requests[i];
        }
    }
    return NULL;
}

/* Called only with dns_state_lock held. */
static dns_request_t* dns_allocate_request_locked(const char* hostname) {
    dns_request_t* slot = NULL;
    for (size_t i = 0; i < DNS_MAX_INFLIGHT; ++i) {
        /* A terminal result remains owned by its waiting dns_resolve() caller
         * until that caller consumes it. Reusing it here could turn a valid
         * response into an unrelated request before the original worker wakes. */
        /* A request nobody collected for 30 s (an asynchronous caller that went away) is reclaimed. */
        if (dns_requests[i].state != DNS_REQUEST_FREE &&
            (uint32_t)get_timer_ticks() - dns_requests[i].submitted_at > 30000u) dns_requests[i].state = DNS_REQUEST_FREE;
        if (dns_requests[i].state == DNS_REQUEST_FREE) {
            slot = &dns_requests[i];
            break;
        }
    }
    if (!slot) return NULL;

    uint16_t id = 0;
    for (size_t attempt = 0; attempt < 256u; ++attempt) {      /* random, so an off-path attacker cannot guess it */
        uint16_t cand = (uint16_t)net_rand32();
        if (cand != 0u && !dns_find_request_locked(cand)) { id = cand; break; }
    }
    if (id == 0u) return NULL;

    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->source_port = (uint16_t)(49152u + net_rand32() % 16384u);
    slot->submitted_at = (uint32_t)get_timer_ticks();
    slot->state = DNS_REQUEST_PENDING;
    dns_copy_name(slot->hostname, sizeof(slot->hostname), hostname);
    return slot;
}

static volatile bool dns_server_from_dhcp = false;
bool dns_server_configured(void) { return dns_server_from_dhcp; }

/* The resolver the kernel uses (learnt from DHCP, 8.8.8.8 without one): the Linux layer offers it to Linux programs as /etc/resolv.conf. */
void dns_get_server_bytes(uint8_t out[4]) {
    dns_lock_acquire();
    for (int i = 0; i < 4; ++i) out[i] = dns_server.addr[i];
    dns_lock_release();
}

void dns_set_server(const ip_addr_t* server) {
    if (!server || (server->addr[0] == 0 && server->addr[1] == 0 &&
                    server->addr[2] == 0 && server->addr[3] == 0)) {
        return;
    }
    dns_lock_acquire();
    dns_server = *server;
    memset(dns_cache, 0, sizeof(dns_cache));
    dns_lock_release();
    dns_server_from_dhcp = true;
    serial_puts("[DNS] Server updated from DHCP\n");
}

void dns_init(void) {
    extern ip_addr_t dhcp_get_dns(void);
    dns_lock_acquire();
    memset(dns_cache, 0, sizeof(dns_cache));
    memset(dns_requests, 0, sizeof(dns_requests));
    dns_next_id = 0x1234;
    dns_server = dhcp_get_dns();
    if (dns_server.addr[0] == 0u) dns_server = ip_make_addr(8, 8, 8, 8);
    dns_lock_release();
    serial_puts("[DNS] DNS client initialized with bounded request table\n");
}

/* Builds and sends one query for an already-allocated request. Returns 0 when the datagram left the machine. */
static int dns_transmit(uint16_t id, uint16_t source_port, const char* hostname, uint16_t type) {
    uint8_t packet[DNS_MAX_PACKET];
    memset(packet, 0, sizeof(packet));
    dns_lock_acquire();
    ip_addr_t server = dns_server;
    /* With no IPv4 lease but an IPv6 resolver from the router, ask over IPv6. */
    bool via6 = dns_server6_valid && !dns_server_from_dhcp;
    ip6_addr_t server6 = dns_server6;
    dns_lock_release();

    dns_header_t* hdr = (dns_header_t*)packet;
    hdr->id = htons(id);
    hdr->flags = htons(DNS_FLAG_RD);
    hdr->questions = htons(1);
    int offset = (int)sizeof(dns_header_t);
    int name_len = dns_encode_name(packet + offset, DNS_MAX_PACKET - (size_t)offset - 4u, hostname);
    if (name_len < 0) return -1;
    offset += name_len;
    *(uint16_t*)(packet + offset) = htons(type); offset += 2;
    *(uint16_t*)(packet + offset) = htons(DNS_CLASS_IN); offset += 2;
    return (via6 ? udp6_send(&server6, source_port, DNS_PORT, packet, (size_t)offset)
                 : udp_send(&server, source_port, DNS_PORT, packet, (size_t)offset)) == 0 ? 0 : -1;
}

/* True while a request is waiting for a reply addressed to this local port: a reply to any other port is not ours. */
int dns_expects_port(uint16_t port) {
    int expected = 0;
    dns_lock_acquire();
    for (size_t i = 0; i < DNS_MAX_INFLIGHT; ++i)
        if (dns_requests[i].state == DNS_REQUEST_PENDING && dns_requests[i].source_port == port) { expected = 1; break; }
    dns_lock_release();
    return expected;
}

/* Sends the query again (the first datagram or its reply may have been lost). */
static void dns_retransmit(uint16_t id) {
    char name[DNS_MAX_NAME_LEN];
    uint16_t port = 0, type = DNS_TYPE_A;
    dns_lock_acquire();
    dns_request_t* r = dns_find_request_locked(id);
    bool ok = r && r->state == DNS_REQUEST_PENDING;
    if (ok) { port = r->source_port; type = r->want6 ? DNS_TYPE_AAAA : DNS_TYPE_A; dns_copy_name(name, sizeof name, r->hostname); }
    dns_lock_release();
    if (ok) (void)dns_transmit(id, port, name, type);
}

int dns_query(const char* hostname, uint16_t type) {
    if (!hostname || !hostname[0] || (type != DNS_TYPE_A && type != DNS_TYPE_AAAA)) return -1;

    dns_lock_acquire();
    dns_request_t* request = dns_allocate_request_locked(hostname);
    if (request) request->want6 = (type == DNS_TYPE_AAAA);
    uint16_t id = request ? request->id : 0u;
    uint16_t source_port = request ? request->source_port : 0u;
    dns_lock_release();
    if (!request) {
        serial_puts("[DNS] request table full\n");
        return -1;
    }

    /* The first datagram to a resolver on another host usually cannot leave: the gateway's MAC is still being resolved
     * (ARP). Keep pumping the network and trying instead of failing the lookup. */
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (dns_transmit(id, source_port, hostname, type) == 0) return (int)id;
        net_poll();
        thread_yield();
    }

    dns_lock_acquire();
    dns_request_t* failed = dns_find_request_locked(id);
    /* dns_query() returns failure to its caller before a waiter can observe a terminal state, so release the
     * reservation immediately instead of consuming a bounded table slot indefinitely. */
    if (failed) failed->state = DNS_REQUEST_FREE;
    dns_lock_release();
    serial_puts("[DNS] query could not be sent\n");
    return -1;
}

void dns_handle_response(void* data, size_t len) {
    if (!data || len < sizeof(dns_header_t)) return;
    const uint8_t* start = (const uint8_t*)data;
    const uint8_t* end = start + len;
    const dns_header_t* hdr = (const dns_header_t*)data;
    uint16_t id = ntohs(hdr->id);
    uint16_t flags = ntohs(hdr->flags);

    dns_lock_acquire();
    dns_request_t* request = dns_find_request_locked(id);
    if (!request || request->state != DNS_REQUEST_PENDING) {
        dns_lock_release();
        return; /* stale or unrelated UDP/DNS packet */
    }

    uint16_t rcode = flags & 0x0Fu;
    if ((flags & DNS_FLAG_QR) == 0u || rcode != DNS_RCODE_NOERROR) {
        if (rcode == DNS_RCODE_NXDOMAIN) {
            dns_cache_store_locked(request->hostname, NULL, DNS_NEGATIVE_TTL_TICKS, true);
        }
        request->state = DNS_REQUEST_FAILED;
        dns_lock_release();
        return;
    }

    const uint8_t* ptr = start + sizeof(dns_header_t);
    uint16_t questions = ntohs(hdr->questions);
    for (uint16_t q = 0; q < questions; ++q) {
        ptr = dns_skip_name(ptr, end);
        if (!ptr || ptr + 4u > end) {
            request->state = DNS_REQUEST_FAILED;
            dns_lock_release();
            return;
        }
        ptr += 4;
    }

    uint16_t answers = ntohs(hdr->answers);
    for (uint16_t answer = 0; answer < answers; ++answer) {
        ptr = dns_skip_name(ptr, end);
        if (!ptr || ptr + 10u > end) {
            request->state = DNS_REQUEST_FAILED;
            dns_lock_release();
            return;
        }
        uint16_t type = ntohs(*(const uint16_t*)ptr); ptr += 2;
        uint16_t class = ntohs(*(const uint16_t*)ptr); ptr += 2;
        uint32_t ttl = ntohl(*(const uint32_t*)ptr); ptr += 4;
        uint16_t rdlen = ntohs(*(const uint16_t*)ptr); ptr += 2;
        if (ptr + rdlen > end) {
            request->state = DNS_REQUEST_FAILED;
            dns_lock_release();
            return;
        }
        if (request->want6 && type == DNS_TYPE_AAAA && class == DNS_CLASS_IN && rdlen == 16u) {
            memcpy(request->result6.b, ptr, 16);
            int slot = 0; uint32_t now = (uint32_t)get_timer_ticks();
            for (int i = 0; i < 8; ++i) if (!dns_cache6[i].valid || dns_cache6[i].expires < now) { slot = i; break; }
            dns_copy_name(dns_cache6[slot].name, sizeof(dns_cache6[slot].name), request->hostname);
            dns_cache6[slot].ip = request->result6; dns_cache6[slot].valid = true;
            dns_cache6[slot].expires = now + (ttl > 300 ? 300u : ttl) * 1000u;
            request->state = DNS_REQUEST_RESOLVED;
            dns_lock_release();
            return;
        }
        if (!request->want6 && type == DNS_TYPE_A && class == DNS_CLASS_IN && rdlen == 4u) {
            memcpy(request->result.addr, ptr, 4);
            dns_cache_store_locked(request->hostname, &request->result, ttl, false);
            request->state = DNS_REQUEST_RESOLVED;
            dns_lock_release();
            return;
        }
        ptr += rdlen;
    }

    request->state = DNS_REQUEST_FAILED;
    dns_lock_release();
}

void dns_set_server6(const ip6_addr_t* server) {
    if (!server) return;
    dns_lock_acquire();
    dns_server6 = *server;
    dns_server6_valid = true;
    dns_lock_release();
}

/* AAAA lookup: cache first, then ask (IPv6 transport when only the router-supplied resolver is known). */
int dns_resolve6(const char* hostname, ip6_addr_t* result) {
    if (!hostname || !hostname[0] || !result) return -1;
    dns_lock_acquire();
    uint32_t now = (uint32_t)get_timer_ticks();
    for (int i = 0; i < 8; ++i)
        if (dns_cache6[i].valid && dns_cache6[i].expires >= now && strcmp(dns_cache6[i].name, hostname) == 0) {
            *result = dns_cache6[i].ip; dns_lock_release(); return 0;
        }
    dns_lock_release();
    int query_id = dns_query(hostname, DNS_TYPE_AAAA);
    if (query_id < 0) return -1;
    for (uint32_t pass = 0; pass < DNS_RESOLVE_WAIT_PASSES; ++pass) {
        dns_lock_acquire();
        dns_request_t* request = dns_find_request_locked((uint16_t)query_id);
        if (!request) { dns_lock_release(); return -1; }
        if (request->state == DNS_REQUEST_RESOLVED) { *result = request->result6; request->state = DNS_REQUEST_FREE; dns_lock_release(); return 0; }
        if (request->state == DNS_REQUEST_FAILED) { request->state = DNS_REQUEST_FREE; dns_lock_release(); return -1; }
        dns_lock_release();
        if (pass && pass % 100u == 0u) dns_retransmit((uint16_t)query_id);
        net_poll();
        thread_yield();
    }
    dns_lock_acquire();
    dns_request_t* request = dns_find_request_locked((uint16_t)query_id);
    if (request) request->state = DNS_REQUEST_FREE;
    dns_lock_release();
    return -1;
}

int dns_resolve(const char* hostname, ip_addr_t* result) {
    if (!hostname || !hostname[0] || !result) return -1;

    dns_lock_acquire();
    uint32_t now = (uint32_t)get_timer_ticks();
    for (int i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (dns_cache_entry_live_locked(&dns_cache[i], now) &&
            strcmp(dns_cache[i].name, hostname) == 0) {
            int negative = dns_cache[i].negative;
            if (!negative) *result = dns_cache[i].ip;
            dns_lock_release();
            return negative ? -1 : 0;
        }
    }
    dns_lock_release();

    int query_id = dns_query(hostname, DNS_TYPE_A);
    if (query_id < 0) return -1;

    for (uint32_t pass = 0; pass < DNS_RESOLVE_WAIT_PASSES; ++pass) {
        dns_lock_acquire();
        dns_request_t* request = dns_find_request_locked((uint16_t)query_id);
        if (!request) {
            dns_lock_release();
            return -1;
        }
        if (request->state == DNS_REQUEST_RESOLVED) {
            *result = request->result;
            request->state = DNS_REQUEST_FREE;
            dns_lock_release();
            return 0;
        }
        if (request->state == DNS_REQUEST_FAILED) {
            request->state = DNS_REQUEST_FREE;
            dns_lock_release();
            return -1;
        }
        dns_lock_release();

        /* Request ingress from the single dispatcher, then give its owner and
         * other HTTP/TLS workers a chance to run. */
        if (pass && pass % 100u == 0u) dns_retransmit((uint16_t)query_id);
        net_poll();
        thread_yield();
    }

    dns_lock_acquire();
    dns_request_t* request = dns_find_request_locked((uint16_t)query_id);
    if (request) request->state = DNS_REQUEST_FREE;
    dns_lock_release();
    return -1;
}


/* ---- helpers for the non-blocking ring-3 resolver ------------------------------------------------------------------ */
/* 0 = cached positive answer in *out, -2 = cached "no such name", -1 = not cached. */
int dns_cache_lookup(const char* hostname, ip_addr_t* out) {
    if (!hostname || !out) return -1;
    int rc = -1;
    dns_lock_acquire();
    uint32_t now = (uint32_t)get_timer_ticks();
    for (int i = 0; i < DNS_CACHE_SIZE; ++i) {
        if (dns_cache_entry_live_locked(&dns_cache[i], now) && strcmp(dns_cache[i].name, hostname) == 0) {
            if (dns_cache[i].negative) rc = -2; else { *out = dns_cache[i].ip; rc = 0; }
            break;
        }
    }
    dns_lock_release();
    return rc;
}

void dns_get_server(ip_addr_t* out) {
    dns_lock_acquire();
    if (out) *out = dns_server;
    dns_lock_release();
}

/* 1 = resolved (*out set), 0 = still waiting, -1 = failed or timed out. Resends the query up to three times, 1.5 s apart. */
int dns_async_poll(int id, ip_addr_t* out) {
    uint16_t qid = (uint16_t)id;
    dns_lock_acquire();
    dns_request_t* r = dns_find_request_locked(qid);
    if (!r) { dns_lock_release(); return -1; }
    if (r->state == DNS_REQUEST_RESOLVED) { *out = r->result; r->state = DNS_REQUEST_FREE; dns_lock_release(); return 1; }
    if (r->state == DNS_REQUEST_FAILED) { r->state = DNS_REQUEST_FREE; dns_lock_release(); return -1; }
    uint32_t now = (uint32_t)get_timer_ticks();
    bool resend = false;
    if (now - r->submitted_at >= 1500u) {
        if (r->retries >= 3) { r->state = DNS_REQUEST_FREE; dns_lock_release(); return -1; }
        r->retries++; r->submitted_at = now; resend = true;
    }
    dns_lock_release();
    if (resend) dns_retransmit(qid);
    return 0;
}
