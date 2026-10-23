/**
 * xhci_hcd.c - xHCI host controller driver for TinyUSB (see xhci_hcd.h).
 *
 * Structure follows the xHCI 1.x specification; the ring/context handling is modelled on MikanOS's small xHCI
 * driver (command ring, one event ring segment, per-endpoint transfer rings, Enable Slot / Address Device /
 * Configure Endpoint), extended with what TinyUSB needs and MikanOS leaves out: hubs (route strings, parent slot,
 * hub slot-context fields), bulk and multi-TRB transfers, endpoint reset / stall recovery, device removal, BIOS
 * ownership hand-off and 32/64-byte context sizes.
 */
#include "xhci_hcd.h"
#include "memory.h"
#include "string.h"
#include "serial.h"
#include "pci.h"
#include "mm/paging.h"
#include "memory_physical.h"
#include "timer.h"
#include "host/usbh.h"

#define XLOG 0
#if XLOG
#define XDBG(...) xlog(__VA_ARGS__)
static void xlog(const char *tag, uint64_t a, uint64_t b) { serial_puts("[XHCI] "); serial_puts(tag); serial_puts(" "); serial_puthex(a); serial_puts(" "); serial_puthex(b); serial_puts("\n"); }
#else
#define XDBG(...) ((void)0)
#endif

/* ------------------------------------------------------------------ limits and constants */
#define RING_TRBS      256u            /* one 4 KiB page; the last entry is a Link TRB */
#define MAX_SLOTS_USED 32u
#define MAX_CHUNKS     64u
#define ISO_PAGES      4u              /* 4 pages x 16 slots of 256 bytes = up to 64 ms of 1 ms packets in flight */
#define ISO_SLOT       256u
#define ISO_SLOTS      (ISO_PAGES * 4096u / ISO_SLOT)

/* TRB types */
#define TRB_NORMAL 1
#define TRB_ISOCH 5
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT 14
#define TRB_STOP_ENDPOINT 15
#define TRB_SET_TR_DEQUEUE 16
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND 33
#define TRB_EV_PORT 34

/* completion codes */
#define CC_SUCCESS 1
#define CC_STALL 6
#define CC_SHORT_PACKET 13

#define TRB_C   (1u << 0)
#define TRB_ENT (1u << 1)              /* Link: toggle cycle */
#define TRB_ISP (1u << 2)
#define TRB_CH  (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)

/* PORTSC */
#define PORTSC_CCS (1u << 0)
#define PORTSC_PED (1u << 1)
#define PORTSC_PR  (1u << 4)
#define PORTSC_PP  (1u << 9)
#define PORTSC_SPEED(v) (((v) >> 10) & 0xF)
#define PORTSC_PIC (3u << 14)
#define PORTSC_CHANGE_MASK (0x7Fu << 17)       /* CSC PEC WRC OCC PRC PLC CEC: write 1 to clear */
#define PORTSC_WAKE (7u << 25)
#define PORTSC_PRESERVE (PORTSC_PP | PORTSC_PIC | PORTSC_WAKE)
#define PORTSC_PRC (1u << 21)
#define PORTSC_WRC (1u << 19)

typedef struct { uint32_t d[4]; } trb_t;

typedef struct {
    trb_t *trbs;
    uint64_t phys;
    uint32_t enq;
    uint32_t cycle;
} xring_t;

typedef struct {
    bool open;
    uint8_t type;                      /* xHCI endpoint type field */
    uint16_t mps;
    uint8_t burst;
    uint8_t ep_addr;
    xring_t ring;
    bool busy;
    bool needs_reset;                  /* halted: Reset Endpoint + Set TR Dequeue is due */
    uint32_t req_len;
    uint8_t *buf;                      /* the caller's buffer (for hub descriptor snooping) */
    uint8_t nchunks;
    uint16_t chunk_len[MAX_CHUNKS];
    uint16_t chunk_trb[MAX_CHUNKS];    /* ring index of each chunk's TRB */
    bool is_control_setup;             /* unused for control now (kept for the non-control path) */
    /* control transfers: TinyUSB drives the three stages one by one, but an xHCI control TD (Setup + Data + Status)
     * has to be on the ring as a whole, so the stages are collected here and queued together at the data stage. */
    uint8_t ctl_setup[8];
    bool ctl_active, ctl_hasdata, ctl_in, ctl_status_done, ctl_status_waiting;
    uint16_t ctl_data_trb, ctl_status_trb;
    uint8_t ctl_status_code;
    /* isochronous OUT: packets are copied into DMA slots and queued as one single-TRB TD each */
    bool iso;
    uint8_t *iso_pg[ISO_PAGES]; uint64_t iso_phys[ISO_PAGES];
    uint16_t iso_inflight, iso_next;
    uint32_t iso_done, iso_err;
    uint8_t *ctl_bounce; uint64_t ctl_bounce_phys;
} xep_t;

typedef struct {
    bool used;
    uint8_t slot_id;
    uint8_t rhport;                    /* TinyUSB root port this device hangs off */
    uint8_t xport;                     /* xHCI root port number (1-based) */
    uint8_t speed_id;                  /* xHCI port speed ID for the slot context */
    uint8_t daddr;                     /* TinyUSB address (0 while still enumerating) */
    uint32_t route;
    uint8_t depth;                     /* 0 = directly on a root port */
    uint8_t parent_slot, parent_port;
    bool is_hub; uint8_t hub_ports, hub_ttt;
    bool hub_dirty;                    /* hub slot-context fields need a Configure Endpoint */
    bool snoop_hub_desc;
    bool fake_status;                  /* SET_ADDRESS was absorbed: the status stage completes without hardware */
    uint8_t *dev_ctx; uint64_t dev_ctx_phys;
    uint8_t *in_ctx;  uint64_t in_ctx_phys;
    xep_t ep[32];
    /* SuperSpeed devices: endpoint sizes/bursts taken from the real descriptors (TinyUSB is shown a USB2-style one) */
    uint16_t ss_mps[32]; uint8_t ss_burst[32];
} xslot_t;

/* ------------------------------------------------------------------ controller state */
static bool s_probed, s_inited, s_running;
static const char *s_err;
static uint64_t s_mmio_phys;
static uint16_t s_pci_vendor, s_pci_device;
static volatile uint8_t *s_cap, *s_op, *s_rt, *s_db;
static uint32_t s_ctx_size = 32, s_max_ports, s_max_slots, s_slots_en;
static uint16_t s_hciversion;
static uint8_t s_port_usb3[256];
static uint8_t s_rh_to_port[XHCI_MAX_ROOT_PORTS + 1];      /* TinyUSB rhport -> xHCI port number (0 = unused) */
static uint8_t s_nrh;
static bool s_last_ccs[XHCI_MAX_ROOT_PORTS + 1];

static uint64_t *s_dcbaa; static uint64_t s_dcbaa_phys;
static xring_t s_cmd;
static trb_t *s_ev; static uint64_t s_ev_phys; static uint32_t s_ev_deq, s_ev_cycle;
static xslot_t s_slots[MAX_SLOTS_USED + 1];
static xslot_t *s_dev0;                                    /* slot of the device currently being enumerated (daddr 0) */

/* ------------------------------------------------------------------ small helpers */
static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v) { wr32(b, off, (uint32_t)v); wr32(b, off + 4, (uint32_t)(v >> 32)); }

static void ms_delay(uint32_t ms) {
    uint64_t t0 = get_timer_ticks();
    uint64_t spins = 0;
    /* ticks are milliseconds; the spin bound keeps this finite if the tick source stalls */
    while (get_timer_ticks() - t0 < ms && spins < (uint64_t)ms * 4000000ull) { __asm__ volatile("pause"); ++spins; }
}

static void *dma_page(uint64_t *phys) {
    void *p = kmalloc_aligned(4096, 4096);
    if (!p) return NULL;
    memset(p, 0, 4096);
    if (phys) *phys = paging_virt_to_phys((uint64_t)(uintptr_t)p);
    return p;
}
static void dma_free(void *p) { if (p) kfree(p); }

static xslot_t *slot_by_daddr(uint8_t daddr) {
    if (daddr == 0) return s_dev0;
    for (uint32_t i = 1; i <= MAX_SLOTS_USED; ++i) if (s_slots[i].used && s_slots[i].daddr == daddr) return &s_slots[i];
    return NULL;
}
static inline uint32_t dci_of(uint8_t ep_addr) { uint32_t n = ep_addr & 0x0F; return n == 0 ? 1u : (n * 2u + ((ep_addr & 0x80) ? 1u : 0u)); }

/* ------------------------------------------------------------------ rings */
static bool ring_init(xring_t *r) {
    r->trbs = (trb_t *)dma_page(&r->phys);
    if (!r->trbs) return false;
    r->enq = 0; r->cycle = 1;
    /* trailing Link TRB back to the start, toggling the cycle bit */
    trb_t *l = &r->trbs[RING_TRBS - 1];
    l->d[0] = (uint32_t)r->phys; l->d[1] = (uint32_t)(r->phys >> 32); l->d[2] = 0;
    l->d[3] = TRB_TYPE(TRB_LINK) | TRB_ENT;
    return true;
}
static void ring_free(xring_t *r) { dma_free(r->trbs); r->trbs = NULL; }

/* Writes one TRB at the enqueue pointer (the cycle bit is set last) and returns its ring index. */
static uint32_t ring_push(xring_t *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    uint32_t idx = r->enq;
    trb_t *t = &r->trbs[idx];
    t->d[0] = d0; t->d[1] = d1; t->d[2] = d2;
    __sync_synchronize();
    t->d[3] = (d3 & ~TRB_C) | (r->cycle ? TRB_C : 0);
    __sync_synchronize();
    if (++r->enq == RING_TRBS - 1) {
        trb_t *l = &r->trbs[RING_TRBS - 1];
        l->d[3] = (l->d[3] & ~TRB_C) | (r->cycle ? TRB_C : 0);       /* hand the link over with the current cycle */
        __sync_synchronize();
        r->enq = 0;
        r->cycle ^= 1u;
    }
    return idx;
}

static inline void ring_db(uint32_t slot, uint32_t target) { __sync_synchronize(); wr32(s_db, slot * 4u, target); }

/* ------------------------------------------------------------------ events */
static void handle_transfer_event(const trb_t *e);
static volatile bool s_cmd_done; static uint32_t s_cmd_code; static uint8_t s_cmd_slot; static uint64_t s_cmd_ptr_wait;

static void event_ring_poll(void) {
    bool any = false;
    for (;;) {
        trb_t *e = &s_ev[s_ev_deq];
        if (((e->d[3] & TRB_C) ? 1u : 0u) != s_ev_cycle) break;
        __sync_synchronize();
        uint32_t type = (e->d[3] >> 10) & 0x3F;
        if (type == TRB_EV_COMMAND) {
            uint64_t ptr = ((uint64_t)e->d[1] << 32) | (e->d[0] & ~0xFu);
            if (ptr == s_cmd_ptr_wait) { s_cmd_code = e->d[2] >> 24; s_cmd_slot = (uint8_t)(e->d[3] >> 24); s_cmd_done = true; }
        } else if (type == TRB_EV_TRANSFER) {
            trb_t copy = *e;
            if (++s_ev_deq == RING_TRBS) { s_ev_deq = 0; s_ev_cycle ^= 1u; }
            any = true;
            handle_transfer_event(&copy);
            continue;
        }
        if (++s_ev_deq == RING_TRBS) { s_ev_deq = 0; s_ev_cycle ^= 1u; }
        any = true;
    }
    if (any) {
        uint64_t erdp = s_ev_phys + (uint64_t)s_ev_deq * sizeof(trb_t);
        wr64(s_rt, 0x20 + 0x18, erdp | (1u << 3));                    /* EHB: clear event handler busy */
    }
}

/* Issues one command and waits for its completion event (polling; at most ~1 s). */
static bool xhci_cmd(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *code, uint8_t *slot_out) {
    if (!s_running) return false;
    uint64_t at = s_cmd.phys + (uint64_t)s_cmd.enq * sizeof(trb_t);
    s_cmd_done = false; s_cmd_ptr_wait = at;
    ring_push(&s_cmd, d0, d1, d2, d3);
    ring_db(0, 0);
    for (uint64_t t0 = get_timer_ticks(), spins = 0; !s_cmd_done; ++spins) {
        event_ring_poll();
        if (s_cmd_done) break;
        if (get_timer_ticks() - t0 > 1000 || spins > 400000000ull) { serial_puts("[XHCI] command timed out\n"); return false; }
        __asm__ volatile("pause");
    }
    s_cmd_ptr_wait = 0;
    if (code) *code = s_cmd_code;
    if (slot_out) *slot_out = s_cmd_slot;
    return s_cmd_code == CC_SUCCESS;
}

/* ------------------------------------------------------------------ contexts */
static inline uint8_t *ictx(xslot_t *s, uint32_t index) { return s->in_ctx + (size_t)index * s_ctx_size; }      /* 0 = input control, 1 = slot, 2.. = EP dci+1 */
static inline uint32_t *ictx32(xslot_t *s, uint32_t index) { return (uint32_t *)ictx(s, index); }

static uint8_t xspeed_id_from_tusb(uint8_t sp) { return sp == TUSB_SPEED_LOW ? 2 : sp == TUSB_SPEED_HIGH ? 3 : 1; }

static uint16_t default_ep0_mps(uint8_t speed_id) { return speed_id == 2 ? 8 : speed_id == 1 ? 8 : speed_id == 3 ? 64 : 512; }

/* Fills the slot context part of the input context from the slot record. */
static void fill_slot_ctx(xslot_t *s, uint32_t context_entries) {
    uint32_t *c = ictx32(s, 1);
    c[0] = (s->route & 0xFFFFF) | ((uint32_t)s->speed_id << 20) | (s->is_hub ? (1u << 26) : 0) | (context_entries << 27);
    c[1] = ((uint32_t)s->xport << 16) | (s->is_hub ? ((uint32_t)s->hub_ports << 24) : 0);
    c[2] = (uint32_t)s->parent_slot | ((uint32_t)s->parent_port << 8) | (s->is_hub ? ((uint32_t)(s->hub_ttt & 3) << 16) : 0);
    c[3] = 0;
}

static uint32_t max_dci(xslot_t *s) {
    uint32_t m = 1;
    for (uint32_t d = 1; d < 32; ++d) if (s->ep[d].open) m = d;
    return m;
}

/* Programs the endpoint context for dci from the endpoint record. */
static void fill_ep_ctx(xslot_t *s, uint32_t dci) {
    xep_t *e = &s->ep[dci];
    uint32_t *c = ictx32(s, dci + 1);
    memset(c, 0, s_ctx_size);
    uint32_t interval = 0;
    uint16_t mps = e->mps;
    c[1] = ((e->type == 1 || e->type == 5 ? 0u : 3u) << 1) | ((uint32_t)e->type << 3) | ((uint32_t)e->burst << 8) | ((uint32_t)mps << 16);       /* CErr = 3 */
    uint64_t deq = e->ring.phys | 1u;                                           /* dequeue cycle state = 1 */
    c[2] = (uint32_t)deq; c[3] = (uint32_t)(deq >> 32);
    uint32_t avg = (e->type == 4) ? 8u : mps;
    c[4] = avg | ((e->type == 3 || e->type == 7 || e->type == 1 || e->type == 5) ? ((uint32_t)(mps * (e->burst + 1u)) << 16) : 0);
    c[0] = (interval << 16);
    (void)interval;
}

static void set_ep_interval(xslot_t *s, uint32_t dci, uint8_t binterval, uint8_t tusb_speed, bool iso) {
    uint32_t *c = ictx32(s, dci + 1);
    uint32_t iv;
    if (iso) { iv = binterval ? (uint32_t)binterval - 1u : 0u; if (tusb_speed != TUSB_SPEED_HIGH) iv += 3u; }       /* bInterval is an exponent */
    else if (tusb_speed == TUSB_SPEED_HIGH) { iv = binterval ? (uint32_t)binterval - 1u : 0u; }
    else {                                                  /* FS / LS: bInterval is in ms; field is 2^n x 125 us */
        uint32_t ms = binterval ? binterval : 1u, units = ms * 8u;
        iv = 3;
        while ((1u << iv) < units && iv < 10) ++iv;
    }
    if (iv > 15) iv = 15;
    c[0] = (c[0] & ~(0xFFu << 16)) | (iv << 16);
}

/* ------------------------------------------------------------------ slot lifecycle */
static void iso_free(xep_t *e) {
    for (uint32_t i = 0; i < ISO_PAGES; ++i) { dma_free(e->iso_pg[i]); e->iso_pg[i] = NULL; }
    e->iso = false;
}

static void slot_free(xslot_t *s) {
    for (uint32_t d = 1; d < 32; ++d) iso_free(&s->ep[d]);
    for (uint32_t d = 1; d < 32; ++d) if (s->ep[d].ring.trbs) ring_free(&s->ep[d].ring);
    dma_free(s->ep[1].ctl_bounce);
    dma_free(s->dev_ctx); dma_free(s->in_ctx);
    memset(s, 0, sizeof *s);
}

static void slot_disable(xslot_t *s) {
    if (!s->used) return;
    if (s->slot_id) {
        xhci_cmd(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)s->slot_id << 24), NULL, NULL);
        s_dcbaa[s->slot_id] = 0;
    }
    if (s_dev0 == s) s_dev0 = NULL;
    slot_free(s);
}

/* Enables a slot for the device that was just reset on `rhport` and addresses it (this performs SET_ADDRESS). */
static xslot_t *slot_create_and_address(uint8_t rhport, uint16_t ep0_mps_hint) {
    tuh_bus_info_t bus;
    memset(&bus, 0, sizeof bus);
    tuh_bus_info_get(0, &bus);

    uint32_t code; uint8_t sid = 0;
    XDBG("enable slot for rhport/speed", rhport, bus.speed);
    if (!xhci_cmd(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &code, &sid) || sid == 0 || sid > MAX_SLOTS_USED) {
        serial_puts("[XHCI] Enable Slot failed\n");
        return NULL;
    }
    xslot_t *s = &s_slots[sid];
    memset(s, 0, sizeof *s);
    s->used = true; s->slot_id = sid; s->rhport = rhport;
    s->xport = s_rh_to_port[rhport];

    uint32_t portsc = rd32(s_op, 0x400 + (uint32_t)(s->xport - 1) * 0x10);
    if (bus.hub_addr == 0) {
        s->speed_id = (uint8_t)PORTSC_SPEED(portsc);
        if (s->speed_id == 0) s->speed_id = xspeed_id_from_tusb(bus.speed);
    } else {
        s->speed_id = xspeed_id_from_tusb(bus.speed);
        xslot_t *hub = slot_by_daddr(bus.hub_addr);
        if (!hub) { serial_puts("[XHCI] parent hub slot missing\n"); slot_disable(s); return NULL; }
        s->depth = (uint8_t)(hub->depth + 1);
        s->route = hub->route | ((uint32_t)(bus.hub_port & 0xF) << (4u * hub->depth));
        s->xport = hub->xport;
        if (s->speed_id < 3) { s->parent_slot = hub->slot_id; s->parent_port = bus.hub_port; }   /* LS/FS behind a HS hub: via its TT */
    }

    s->dev_ctx = (uint8_t *)dma_page(&s->dev_ctx_phys);
    s->in_ctx = (uint8_t *)dma_page(&s->in_ctx_phys);
    xep_t *e0 = &s->ep[1];
    if (!s->dev_ctx || !s->in_ctx || !ring_init(&e0->ring)) { serial_puts("[XHCI] out of memory for slot\n"); slot_disable(s); return NULL; }
    e0->open = true; e0->type = 4; e0->ep_addr = 0;
    e0->mps = ep0_mps_hint ? ep0_mps_hint : default_ep0_mps(s->speed_id);
    if (s->speed_id == 4 || s->speed_id == 5) e0->mps = 512;
    s_dcbaa[sid] = s->dev_ctx_phys;

    ictx32(s, 0)[0] = 0;
    ictx32(s, 0)[1] = (1u << 0) | (1u << 1);                         /* add slot + EP0 */
    fill_slot_ctx(s, 1);
    fill_ep_ctx(s, 1);
    __sync_synchronize();
    if (!xhci_cmd((uint32_t)s->in_ctx_phys, (uint32_t)(s->in_ctx_phys >> 32), 0,
                  TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)sid << 24), &code, NULL)) {
        serial_puts("[XHCI] Address Device failed, code=");
        serial_putdec(code);
        serial_puts("\n");
        slot_disable(s);
        return NULL;
    }
    XDBG("addressed slot/speed_id", s->slot_id, s->speed_id);
    return s;
}

/* ------------------------------------------------------------------ transfer completion */
static void post_complete(xslot_t *s, uint32_t dci, uint32_t xferred, xfer_result_t res) {
    xep_t *e = &s->ep[dci];
    e->busy = false;
    hcd_event_xfer_complete(s->daddr, e->ep_addr, xferred, res, false);
}

/* TinyUSB has no SuperSpeed host support, so a SuperSpeed device is shown a USB2-style configuration descriptor:
 * SuperSpeed Endpoint Companion descriptors are dropped and bulk sizes above 512 clamped. The real sizes and burst
 * counts are kept per endpoint for the endpoint contexts. Returns the new length. */
static uint32_t ss_filter_config(xslot_t *s, uint8_t *d, uint32_t len) {
    if (len < 9 || d[1] != 2) return len;
    uint32_t total = (uint32_t)(d[2] | (d[3] << 8));
    if (len < total) return len;                                /* header-only read: leave wTotalLength alone */
    uint32_t in = 9, out = 9;
    uint8_t last_ep = 0;
    while (in + 2 <= len) {
        uint8_t bl = d[in], bt = d[in + 1];
        if (bl < 2 || in + bl > len) break;
        if (bt == 0x30 || bt == 0x31) {                         /* endpoint companion: bMaxBurst is byte 2 */
            if (last_ep && bl >= 3) s->ss_burst[dci_of(last_ep)] = d[in + 2];
            in += bl;
            continue;
        }
        if (bt == 5 && bl >= 7) {
            uint8_t addr = d[in + 2];
            uint16_t mps = (uint16_t)(d[in + 4] | (d[in + 5] << 8));
            last_ep = addr;
            s->ss_mps[dci_of(addr)] = mps & 0x7FF;
            s->ss_burst[dci_of(addr)] = 0;
            memmove(d + out, d + in, bl);
            if ((d[in + 3] & 3) == 2 && (mps & 0x7FF) > 512) { d[out + 4] = 0x00; d[out + 5] = 0x02; }   /* bulk: 512 */
            out += bl; in += bl;
            continue;
        }
        if (bt == 4) last_ep = 0;
        memmove(d + out, d + in, bl);
        out += bl; in += bl;
    }
    d[2] = (uint8_t)out; d[3] = (uint8_t)(out >> 8);
    return out;
}

static void handle_transfer_event(const trb_t *ev) {
    uint32_t slot_id = ev->d[3] >> 24, dci = (ev->d[3] >> 16) & 0x1F, code = ev->d[2] >> 24, resid = ev->d[2] & 0xFFFFFF;
    if (slot_id == 0 || slot_id > MAX_SLOTS_USED || dci == 0 || dci > 31) return;
    xslot_t *s = &s_slots[slot_id];
    if (!s->used) return;
    xep_t *e = &s->ep[dci];
    if (e->open && e->iso) {                                         /* isochronous packet done: only accounting */
        if (e->iso_inflight) e->iso_inflight--;
        if (code == CC_SUCCESS || code == CC_SHORT_PACKET) e->iso_done++; else e->iso_err++;
        return;
    }
    if (!e->open || (!e->busy && dci != 1)) { XDBG("event for idle ep slot-dci", slot_id, dci); return; }

    XDBG("xfer event slot-dci/code", (uint64_t)slot_id << 8 | dci, ((uint64_t)code << 32) | resid);
    uint64_t ptr = ((uint64_t)ev->d[1] << 32) | (ev->d[0] & ~0xFu);
    uint32_t idx = (uint32_t)((ptr - e->ring.phys) / sizeof(trb_t));

    if (dci == 1) {                                                  /* control: data and status stages of the queued TD */
        if (e->ctl_hasdata && idx == e->ctl_data_trb) {
            if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
                uint32_t done = e->req_len > resid ? e->req_len - resid : 0;
                if (e->ctl_in && e->buf && done) memcpy(e->buf, e->ctl_bounce, done);
                if (s->speed_id >= 4 && e->ctl_in && e->buf && e->ctl_setup[1] == 6 && e->ctl_setup[3] == 2) done = ss_filter_config(s, e->buf, done);
                if (s->snoop_hub_desc && e->ctl_in && e->buf && done >= 4 && e->buf[1] == 0x29) {
                    s->is_hub = true; s->hub_ports = e->buf[2]; s->hub_ttt = (uint8_t)((e->buf[3] >> 5) & 3);
                    s->hub_dirty = true;
                }
                s->snoop_hub_desc = false;
                post_complete(s, 1, done, XFER_RESULT_SUCCESS);
            } else {
                e->ctl_active = false; e->needs_reset = true;
                post_complete(s, 1, 0, code == CC_STALL ? XFER_RESULT_STALLED : XFER_RESULT_FAILED);
            }
        } else if (idx == e->ctl_status_trb) {
            e->ctl_status_done = true; e->ctl_status_code = (uint8_t)code;
            e->ctl_active = false;
            if (code != CC_SUCCESS && code != CC_SHORT_PACKET) e->needs_reset = true;
            if (e->ctl_status_waiting) {
                e->ctl_status_waiting = false;
                post_complete(s, 1, 0, (code == CC_SUCCESS || code == CC_SHORT_PACKET) ? XFER_RESULT_SUCCESS :
                                        code == CC_STALL ? XFER_RESULT_STALLED : XFER_RESULT_FAILED);
            }
        }
        return;
    }

    if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
        uint32_t done = 0;
        if (code == CC_SHORT_PACKET) {
            /* bytes of the chunks before the one that ended short, plus what that chunk delivered */
            for (uint32_t i = 0; i < e->nchunks; ++i) {
                if (e->chunk_trb[i] == idx) { done += e->chunk_len[i] - (resid > e->chunk_len[i] ? e->chunk_len[i] : resid); break; }
                done += e->chunk_len[i];
            }
        } else {
            done = e->req_len > resid ? e->req_len - resid : 0;
            if (e->nchunks > 1) done = e->req_len;
        }
        post_complete(s, dci, done, XFER_RESULT_SUCCESS);
    } else if (code == CC_STALL) {
        e->needs_reset = true;
        post_complete(s, dci, 0, XFER_RESULT_STALLED);
    } else {
        e->needs_reset = true;
        post_complete(s, dci, 0, XFER_RESULT_FAILED);
    }
}

/* Reset a halted endpoint and point its ring at the current enqueue position. Top-level context only. */
static void endpoint_recover(xslot_t *s, uint32_t dci) {
    xep_t *e = &s->ep[dci];
    e->needs_reset = false;
    xhci_cmd(0, 0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)s->slot_id << 24), NULL, NULL);
    uint64_t deq = e->ring.phys + (uint64_t)e->ring.enq * sizeof(trb_t);
    deq |= e->ring.cycle ? 1u : 0u;
    xhci_cmd((uint32_t)deq, (uint32_t)(deq >> 32), 0,
             TRB_TYPE(TRB_SET_TR_DEQUEUE) | ((uint32_t)dci << 16) | ((uint32_t)s->slot_id << 24), NULL, NULL);
}

/* Hub slot-context update (Hub flag, port count, TT think time) once the hub descriptor has been read. */
static void hub_update(xslot_t *s) {
    s->hub_dirty = false;
    memset(ictx(s, 0), 0, s_ctx_size);
    ictx32(s, 0)[1] = 1u;                                            /* A0: slot context only */
    fill_slot_ctx(s, max_dci(s));
    __sync_synchronize();
    xhci_cmd((uint32_t)s->in_ctx_phys, (uint32_t)(s->in_ctx_phys >> 32), 0,
             TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)s->slot_id << 24), NULL, NULL);
}

/* ------------------------------------------------------------------ controller bring-up */
bool xhci_probe(void) {
    if (s_probed) return s_mmio_phys != 0;
    s_probed = true;
    pci_dev_t *dev = pci_get_device_by_class_and_prog_if(PCI_CLASS_SERIAL, 0x03, 0x30);
    if (!dev) { s_err = "No xHCI controller on the PCI bus"; return false; }
    uint64_t bar = dev->bar[0];
    if (bar & 1) { s_err = "xHCI BAR0 is I/O space"; return false; }
    uint64_t base = bar & ~0xFULL;
    if (((bar >> 1) & 3) == 2) base |= ((uint64_t)pci_read_dword(dev->bus, dev->slot, dev->func, PCI_BAR1) << 32);
    if (!base) { s_err = "xHCI BAR0 unassigned"; return false; }
    pci_enable_bus_mastering(dev);
    pci_enable_mmio(dev);
    /* 64 KiB covers capability/operational/runtime/doorbell registers on every controller seen so far */
    for (uint64_t off = 0; off < 0x10000; off += 4096) {
        uint64_t virt = PHYS_TO_VIRT(base + off);
        if (!paging_map_range(virt, base + off, 4096, PAGE_PRESENT | PAGE_RW | PAGE_NOCACHE) && !paging_is_present(virt)) {
            s_err = "Failed to map xHCI registers";
            return false;
        }
    }
    phys_memory_reserve_range((phys_addr_t)base, 0x10000);
    s_mmio_phys = base;
    s_pci_vendor = (uint16_t)dev->vendor; s_pci_device = (uint16_t)dev->device;
    s_cap = (volatile uint8_t *)(uintptr_t)PHYS_TO_VIRT(base);
    serial_puts("[USB] xHCI controller found, MMIO base 0x");
    serial_puthex(base);
    serial_puts("\n");
    return true;
}

bool xhci_present(void) { return s_mmio_phys != 0; }
uint8_t xhci_root_port_count(void) { return s_nrh; }
const char *xhci_last_error(void) { return s_err; }

void xhci_get_info(xhci_info_t *o) {
    memset(o, 0, sizeof *o);
    o->present = s_mmio_phys != 0; o->running = s_running;
    o->usb_major = (uint8_t)(s_hciversion >> 8); o->usb_minor = (uint8_t)((s_hciversion >> 4) & 0xF);
    o->ports = (uint8_t)s_max_ports; o->slots = (uint8_t)s_slots_en; o->ctx64 = s_ctx_size == 64;
    o->vendor = s_pci_vendor; o->device = s_pci_device; o->mmio = s_mmio_phys;
    for (uint32_t p = 1; p <= XHCI_MAX_ROOT_PORTS && p <= s_nrh; ++p) {
        uint32_t xp = s_rh_to_port[p];
        if (!xp) continue;
        o->port_usb3[p] = s_port_usb3[xp];
        o->port_connected[p] = (rd32(s_op, 0x400 + (xp - 1) * 0x10) & PORTSC_CCS) ? 1 : 0;
    }
}

static bool wait_status(uint32_t mask, uint32_t want, uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        if ((rd32(s_op, 0x04) & mask) == want) return true;
        ms_delay(1);
    }
    return (rd32(s_op, 0x04) & mask) == want;
}

bool xhci_hcd_init(uint8_t rhport) {
    (void)rhport;
    if (s_inited) return s_running;
    s_inited = true;
    if (!xhci_probe()) return false;

    uint32_t cap0 = rd32(s_cap, 0);
    uint32_t caplen = cap0 & 0xFF;
    s_hciversion = (uint16_t)(cap0 >> 16);
    uint32_t hcs1 = rd32(s_cap, 0x04), hcs2 = rd32(s_cap, 0x08), hcc1 = rd32(s_cap, 0x10);
    s_op = s_cap + caplen;
    s_db = s_cap + (rd32(s_cap, 0x14) & ~3u);
    s_rt = s_cap + (rd32(s_cap, 0x18) & ~0x1Fu);
    s_max_slots = hcs1 & 0xFF; s_max_ports = (hcs1 >> 24) & 0xFF;
    s_ctx_size = (hcc1 & (1u << 2)) ? 64u : 32u;

    /* extended capabilities: BIOS hand-off and the port -> protocol map */
    uint32_t xecp = (hcc1 >> 16) & 0xFFFF;
    memset(s_port_usb3, 0, sizeof s_port_usb3);
    for (int guard = 0; xecp && guard < 64; ++guard) {
        volatile uint8_t *cp = s_cap + (size_t)xecp * 4u;
        uint32_t c0 = rd32(cp, 0), id = c0 & 0xFF;
        if (id == 1) {                                       /* USB Legacy Support */
            if (c0 & (1u << 16)) {                           /* BIOS owns it: ask for the controller */
                wr32(cp, 0, c0 | (1u << 24));
                for (int i = 0; i < 1000 && (rd32(cp, 0) & (1u << 16)); ++i) ms_delay(1);
            }
            wr32(cp, 4, (rd32(cp, 4) & 0x1F1FEEu) | 0xE0000000u);       /* SMI enables off, status bits cleared */
        } else if (id == 2) {                                /* Supported Protocol */
            uint32_t c2 = rd32(cp, 8), first = c2 & 0xFF, count = (c2 >> 8) & 0xFF, major = c0 >> 24;
            for (uint32_t p = first; p < first + count && p < 256; ++p) s_port_usb3[p] = (major >= 3) ? 1 : 0;
        }
        uint32_t nxt = (c0 >> 8) & 0xFF;
        if (!nxt) break;
        xecp += nxt;
    }

    /* halt, reset */
    wr32(s_op, 0x00, rd32(s_op, 0x00) & ~1u);
    if (!wait_status(1u << 0, 1u << 0, 100)) serial_puts("[XHCI] warning: controller did not halt\n");
    wr32(s_op, 0x00, rd32(s_op, 0x00) | (1u << 1));
    for (int i = 0; i < 1000 && (rd32(s_op, 0x00) & (1u << 1)); ++i) ms_delay(1);
    if (!wait_status(1u << 11, 0, 1000)) { s_err = "xHCI controller not ready after reset"; serial_puts("[XHCI] controller not ready after reset\n"); return false; }

    s_slots_en = s_max_slots < MAX_SLOTS_USED ? s_max_slots : MAX_SLOTS_USED;
    wr32(s_op, 0x38, s_slots_en);

    s_dcbaa = (uint64_t *)dma_page(&s_dcbaa_phys);
    if (!s_dcbaa) { s_err = "out of memory"; return false; }
    uint32_t nscratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nscratch) {
        uint64_t arr_phys;
        uint64_t *arr = (uint64_t *)dma_page(&arr_phys);
        if (!arr || nscratch > 512) { s_err = "scratchpad allocation failed"; return false; }
        for (uint32_t i = 0; i < nscratch; ++i) {
            uint64_t ph;
            void *pg = dma_page(&ph);
            if (!pg) { s_err = "scratchpad allocation failed"; return false; }
            arr[i] = ph;
        }
        s_dcbaa[0] = arr_phys;
    }
    wr64(s_op, 0x30, s_dcbaa_phys);

    if (!ring_init(&s_cmd)) { s_err = "out of memory"; return false; }
    wr64(s_op, 0x18, s_cmd.phys | 1u);

    uint64_t erst_phys;
    uint64_t *erst = (uint64_t *)dma_page(&erst_phys);
    s_ev = (trb_t *)dma_page(&s_ev_phys);
    if (!erst || !s_ev) { s_err = "out of memory"; return false; }
    erst[0] = s_ev_phys; erst[1] = RING_TRBS;
    s_ev_deq = 0; s_ev_cycle = 1;
    wr32(s_rt, 0x20 + 0x08, 1);                                 /* ERSTSZ */
    wr64(s_rt, 0x20 + 0x18, s_ev_phys);                         /* ERDP */
    wr64(s_rt, 0x20 + 0x10, erst_phys);                         /* ERSTBA */

    /* run (no interrupts: the event ring is polled) */
    wr32(s_op, 0x00, rd32(s_op, 0x00) | 1u);
    if (!wait_status(1u << 0, 0, 100)) { s_err = "xHCI controller failed to start"; serial_puts("[XHCI] controller failed to start\n"); return false; }
    s_running = true;

    /* expose every root port to TinyUSB (USB3 ports carry SuperSpeed devices, shown to TinyUSB as high speed) */
    s_nrh = 0;
    memset(s_rh_to_port, 0, sizeof s_rh_to_port);
    for (uint32_t p = 1; p <= s_max_ports && s_nrh < XHCI_MAX_ROOT_PORTS; ++p) {
        s_rh_to_port[XHCI_RHPORT_BASE + s_nrh] = (uint8_t)p;
        ++s_nrh;
        uint32_t off = 0x400 + (p - 1) * 0x10, v = rd32(s_op, off);
        wr32(s_op, off, (v & PORTSC_PRESERVE) | PORTSC_PP | PORTSC_CHANGE_MASK);      /* power on, clear stale change bits */
    }
    serial_puts("[USB] xHCI running: version ");
    serial_putdec(s_hciversion >> 8); serial_puts("."); serial_putdec((s_hciversion >> 4) & 0xF);
    serial_puts(", ports=");  serial_putdec(s_max_ports);
    serial_puts(" (exposed to TinyUSB: "); serial_putdec(s_nrh);
    serial_puts("), slots="); serial_putdec(s_slots_en);
    serial_puts(", ctx="); serial_putdec(s_ctx_size);
    serial_puts("\n");
    for (uint32_t p = 1; p <= s_max_ports; ++p) XDBG("port/usb3+portsc", p, ((uint64_t)s_port_usb3[p] << 32) | rd32(s_op, 0x400 + (p - 1) * 0x10));
    ms_delay(20);
    return true;
}

/* ------------------------------------------------------------------ root port handling */
static inline uint32_t portsc_off(uint8_t rhport) { return 0x400 + ((uint32_t)s_rh_to_port[rhport] - 1u) * 0x10u; }
static inline bool rh_valid(uint8_t rhport) { return rhport >= XHCI_RHPORT_BASE && rhport < XHCI_RHPORT_BASE + XHCI_MAX_ROOT_PORTS && s_rh_to_port[rhport]; }

void xhci_hcd_int_handler(uint8_t rhport) {
    if (!s_running) return;
    event_ring_poll();
    /* deferred recovery work that must not run inside the event handler */
    for (uint32_t i = 1; i <= MAX_SLOTS_USED; ++i) {
        xslot_t *s = &s_slots[i];
        if (!s->used) continue;
        for (uint32_t d = 1; d < 32; ++d) if (s->ep[d].open && s->ep[d].needs_reset) endpoint_recover(s, d);
        if (s->hub_dirty) hub_update(s);
    }
    if (!rh_valid(rhport)) return;
    uint32_t off = portsc_off(rhport), v = rd32(s_op, off);
    bool ccs = (v & PORTSC_CCS) != 0;
    if (v & PORTSC_CHANGE_MASK) {
        /* CSC/PEC/... are acknowledged here; PRC/WRC belong to the reset sequence in port_reset_end */
        uint32_t ack = v & PORTSC_CHANGE_MASK & ~(PORTSC_PRC | PORTSC_WRC);
        if (ack) wr32(s_op, off, (v & PORTSC_PRESERVE) | ack);
    }
    if (ccs && !s_last_ccs[rhport]) { s_last_ccs[rhport] = true; XDBG("attach rhport/portsc", rhport, v); hcd_event_device_attach(rhport, false); }
    else if (!ccs && s_last_ccs[rhport]) { s_last_ccs[rhport] = false; XDBG("remove rhport/portsc", rhport, v); hcd_event_device_remove(rhport, false); }
}

uint32_t xhci_hcd_frame_number(uint8_t rhport) { (void)rhport; return s_running ? (rd32(s_rt, 0) & 0x3FFF) >> 3 : 0; }

bool xhci_hcd_port_connect_status(uint8_t rhport) { return rh_valid(rhport) && (rd32(s_op, portsc_off(rhport)) & PORTSC_CCS); }

void xhci_hcd_port_reset(uint8_t rhport) {
    if (!rh_valid(rhport)) return;
    if (s_port_usb3[s_rh_to_port[rhport]]) return;              /* SuperSpeed ports train their link on their own */
    uint32_t off = portsc_off(rhport), v = rd32(s_op, off);
    wr32(s_op, off, (v & PORTSC_PRESERVE) | PORTSC_PR);
}

void xhci_hcd_port_reset_end(uint8_t rhport) {
    if (!rh_valid(rhport)) return;
    if (s_port_usb3[s_rh_to_port[rhport]]) return;
    uint32_t off = portsc_off(rhport);
    for (int i = 0; i < 100 && (rd32(s_op, off) & PORTSC_PR); ++i) ms_delay(1);      /* the reset itself finishes on its own */
    uint32_t v = rd32(s_op, off);
    wr32(s_op, off, (v & PORTSC_PRESERVE) | (v & (PORTSC_PRC | PORTSC_WRC)));         /* acknowledge */
}

tusb_speed_t xhci_hcd_port_speed_get(uint8_t rhport) {
    if (!rh_valid(rhport)) return TUSB_SPEED_FULL;
    uint32_t sp = PORTSC_SPEED(rd32(s_op, portsc_off(rhport)));
    return sp == 2 ? TUSB_SPEED_LOW : sp == 1 ? TUSB_SPEED_FULL : TUSB_SPEED_HIGH;     /* 3 = HS; SuperSpeed is reported as high */
}

/* ------------------------------------------------------------------ device and endpoint management */
void xhci_hcd_device_close(uint8_t rhport, uint8_t daddr) {
    (void)rhport;
    xslot_t *s = slot_by_daddr(daddr);
    if (daddr == 0 && !s) return;
    if (s) slot_disable(s);
}

bool xhci_hcd_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const *d) {
    if (!s_running || !rh_valid(rhport)) return false;
    uint8_t ep_addr = d->bEndpointAddress;
    uint16_t mps = (uint16_t)(tu_edpt_packet_size(d));
    uint32_t dci = dci_of(ep_addr);

    if (dci == 1) {                                              /* control endpoint */
        if (daddr == 0) {
            if (s_dev0) slot_disable(s_dev0);                    /* an earlier enumeration never finished */
            s_dev0 = slot_create_and_address(rhport, 0);
            return s_dev0 != NULL;
        }
        xslot_t *s = slot_by_daddr(daddr);
        if (!s) return false;
        if (s->speed_id < 4 && s->ep[1].mps != mps && mps >= 8) {                   /* the real EP0 size is known now: tell the controller */
            s->ep[1].mps = mps;
            memset(ictx(s, 0), 0, s_ctx_size);
            ictx32(s, 0)[1] = (1u << 1);
            fill_ep_ctx(s, 1);
            uint32_t *c = ictx32(s, 2);                          /* keep the live dequeue state, only MPS changes */
            (void)c;
            __sync_synchronize();
            xhci_cmd((uint32_t)s->in_ctx_phys, (uint32_t)(s->in_ctx_phys >> 32), 0,
                     TRB_TYPE(TRB_EVALUATE_CONTEXT) | ((uint32_t)s->slot_id << 24), NULL, NULL);
        }
        return true;
    }

    xslot_t *s = slot_by_daddr(daddr);
    XDBG("edpt_open daddr/ep+mps", daddr, ((uint64_t)ep_addr << 16) | mps);
    if (!s || dci >= 32) return false;
    xep_t *e = &s->ep[dci];
    if (e->open) return true;
    if (!ring_init(&e->ring)) return false;
    e->open = true; e->ep_addr = ep_addr; e->mps = mps; e->burst = 0;
    if (s->speed_id >= 4 && s->ss_mps[dci]) { e->mps = s->ss_mps[dci]; e->burst = s->ss_burst[dci]; }
    bool in = (ep_addr & 0x80) != 0;
    switch (d->bmAttributes.xfer) {
    case TUSB_XFER_BULK:      e->type = in ? 6 : 2; break;
    case TUSB_XFER_INTERRUPT: e->type = in ? 7 : 3; break;
    default:                  e->type = in ? 5 : 1; break;
    }
    tuh_bus_info_t bus; memset(&bus, 0, sizeof bus);
    tuh_bus_info_get(daddr, &bus);
    if (d->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS) {
        e->iso = true; e->iso_inflight = 0; e->iso_next = 0; e->iso_done = e->iso_err = 0;
        for (uint32_t i = 0; i < ISO_PAGES; ++i) {
            e->iso_pg[i] = (uint8_t *)dma_page(&e->iso_phys[i]);
            if (!e->iso_pg[i]) { iso_free(e); ring_free(&e->ring); e->open = false; return false; }
        }
    }

    memset(ictx(s, 0), 0, s_ctx_size);
    ictx32(s, 0)[1] = (1u << 0) | (1u << dci);
    fill_slot_ctx(s, max_dci(s));
    fill_ep_ctx(s, dci);
    if (d->bmAttributes.xfer == TUSB_XFER_INTERRUPT || d->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS)
        set_ep_interval(s, dci, d->bInterval, bus.speed, d->bmAttributes.xfer == TUSB_XFER_ISOCHRONOUS);
    __sync_synchronize();
    uint32_t code;
    if (!xhci_cmd((uint32_t)s->in_ctx_phys, (uint32_t)(s->in_ctx_phys >> 32), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)s->slot_id << 24), &code, NULL)) {
        serial_puts("[XHCI] Configure Endpoint failed, code=");
        serial_putdec(code);
        serial_puts("\n");
        ring_free(&e->ring);
        iso_free(e);
        e->open = false;
        return false;
    }
    return true;
}

bool xhci_hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
    (void)rhport;
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (!s || dci < 2 || dci >= 32 || !s->ep[dci].open) return false;
    memset(ictx(s, 0), 0, s_ctx_size);
    ictx32(s, 0)[0] = (1u << dci);                              /* drop */
    ictx32(s, 0)[1] = (1u << 0);
    s->ep[dci].open = false;
    fill_slot_ctx(s, max_dci(s));
    __sync_synchronize();
    xhci_cmd((uint32_t)s->in_ctx_phys, (uint32_t)(s->in_ctx_phys >> 32), 0,
             TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)s->slot_id << 24), NULL, NULL);
    ring_free(&s->ep[dci].ring);
    iso_free(&s->ep[dci]);
    return true;
}

static void control_queue(xslot_t *s, xep_t *e, uint8_t *buffer, uint32_t len);

bool xhci_hcd_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const setup[8]) {
    (void)rhport;
    xslot_t *s = slot_by_daddr(daddr);
    if (!s || !s->used) return false;
    xep_t *e = &s->ep[1];
    if (e->needs_reset) endpoint_recover(s, 1);

    uint8_t bm = setup[0], req = setup[1];
    XDBG("setup bm/req", bm, req);
    uint16_t wvalue = (uint16_t)(setup[2] | (setup[3] << 8)), wlength = (uint16_t)(setup[6] | (setup[7] << 8));

    if (bm == 0x00 && req == 5) {
        /* SET_ADDRESS: the Address Device command already assigned one. Bind the slot to the address TinyUSB chose
         * and complete the stages without touching the bus. */
        s->daddr = (uint8_t)wvalue;
        s->fake_status = true;
        e->ep_addr = 0;
        hcd_event_xfer_complete(0, 0x00, 8, XFER_RESULT_SUCCESS, false);
        return true;
    }
    s->snoop_hub_desc = (bm == 0xA0 && req == 6 && (wvalue >> 8) == 0x29);

    /* The Setup stage is only remembered here and reported complete; the whole TD is queued with the data stage
     * (or immediately when there is none), because the controller needs all of a control TD on the ring. */
    memcpy(e->ctl_setup, setup, 8);
    e->ctl_active = true; e->ctl_hasdata = wlength != 0; e->ctl_in = (bm & 0x80) != 0;
    e->ctl_status_done = false; e->ctl_status_waiting = false; e->ep_addr = 0;
    if (!e->ctl_bounce) e->ctl_bounce = (uint8_t *)dma_page(&e->ctl_bounce_phys);
    if (!e->ctl_bounce) return false;
    if (!e->ctl_hasdata) control_queue(s, e, NULL, 0);
    hcd_event_xfer_complete(s->daddr, 0x00, 8, XFER_RESULT_SUCCESS, false);
    return true;
}

/* Queues Setup [+ Data] + Status as one TD. Direction of the status stage is opposite to the data stage (IN if none). */
static void control_queue(xslot_t *s, xep_t *e, uint8_t *buffer, uint32_t len) {
    const uint8_t *su = e->ctl_setup;
    uint32_t d0 = (uint32_t)su[0] | ((uint32_t)su[1] << 8) | ((uint32_t)su[2] << 16) | ((uint32_t)su[3] << 24);
    uint32_t d1 = (uint32_t)su[4] | ((uint32_t)su[5] << 8) | ((uint32_t)su[6] << 16) | ((uint32_t)su[7] << 24);
    uint32_t trt = len == 0 ? 0u : (e->ctl_in ? 3u : 2u);
    ring_push(&e->ring, d0, d1, 8u, TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    e->req_len = len; e->buf = buffer;
    if (len) {
        if (!e->ctl_in) memcpy(e->ctl_bounce, buffer, len);
        e->ctl_data_trb = (uint16_t)ring_push(&e->ring, (uint32_t)e->ctl_bounce_phys, (uint32_t)(e->ctl_bounce_phys >> 32), len,
                                              TRB_TYPE(TRB_DATA) | TRB_IOC | (e->ctl_in ? (1u << 16) : 0u));
    }
    bool status_in = (len == 0) || !e->ctl_in;
    e->ctl_status_trb = (uint16_t)ring_push(&e->ring, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC | (status_in ? (1u << 16) : 0u));
    ring_db(s->slot_id, 1);
}

bool xhci_hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
    (void)rhport;
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (!s || !s->used || dci >= 32) return false;
    xep_t *e = &s->ep[dci];
    if (!e->open) return false;
    bool in = (ep_addr & 0x80) != 0;
    XDBG("xfer ep/len", ep_addr, buflen);

    if (dci == 1) {
        if (s->fake_status) {                                   /* status stage of the absorbed SET_ADDRESS */
            s->fake_status = false;
            if (s_dev0 == s) s_dev0 = NULL;
            hcd_event_xfer_complete(0, ep_addr, 0, XFER_RESULT_SUCCESS, false);
            return true;
        }
        e->ep_addr = ep_addr;
        if (buflen == 0) {                                      /* status stage: the TD is already queued */
            if (e->ctl_status_done) {
                e->ctl_status_done = false;
                uint8_t code = e->ctl_status_code;
                post_complete(s, 1, 0, (code == CC_SUCCESS || code == CC_SHORT_PACKET) ? XFER_RESULT_SUCCESS :
                                        code == CC_STALL ? XFER_RESULT_STALLED : XFER_RESULT_FAILED);
            } else {
                e->ctl_status_waiting = true;
            }
            return true;
        }
        if (buflen > 4096 || !e->ctl_active || !e->ctl_bounce) return false;
        e->busy = true;
        control_queue(s, e, buffer, buflen);
        return true;
    }

    if (e->needs_reset) endpoint_recover(s, dci);
    /* split the buffer at page boundaries: each chunk is one physically contiguous TRB */
    uint32_t n = 0, off = 0;
    e->nchunks = 0;
    e->ep_addr = ep_addr; e->busy = true; e->is_control_setup = false; e->buf = buffer; e->req_len = buflen;
    if (buflen == 0) {                                          /* zero-length packet */
        e->chunk_len[0] = 0; e->nchunks = 1;
        e->chunk_trb[0] = (uint16_t)ring_push(&e->ring, 0, 0, 0, TRB_TYPE(TRB_NORMAL) | TRB_IOC | (in ? TRB_ISP : 0));
        ring_db(s->slot_id, dci);
        return true;
    }
    while (off < buflen && n < MAX_CHUNKS) {
        uint64_t va = (uint64_t)(uintptr_t)buffer + off;
        uint32_t room = 4096u - (uint32_t)(va & 4095u), len = buflen - off < room ? buflen - off : room;
        e->chunk_len[n] = (uint16_t)len;
        ++n; off += len;
    }
    if (off < buflen) { e->busy = false; return false; }          /* longer than MAX_CHUNKS pages */
    e->nchunks = (uint8_t)n;
    uint32_t remaining = buflen;
    off = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t va = (uint64_t)(uintptr_t)buffer + off, ph = paging_virt_to_phys(va);
        remaining -= e->chunk_len[i];
        uint32_t packets = e->mps ? (remaining + e->mps - 1) / e->mps : 0;
        if (packets > 31) packets = 31;
        uint32_t flags = TRB_TYPE(TRB_NORMAL) | (in ? TRB_ISP : 0);
        if (i + 1 < n) flags |= TRB_CH; else flags |= TRB_IOC;
        e->chunk_trb[i] = (uint16_t)ring_push(&e->ring, (uint32_t)ph, (uint32_t)(ph >> 32),
                                              (uint32_t)e->chunk_len[i] | (packets << 17), flags);
        off += e->chunk_len[i];
    }
    ring_db(s->slot_id, dci);
    return true;
}

bool xhci_hcd_edpt_abort_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
    (void)rhport;
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (!s || dci >= 32 || !s->ep[dci].open) return false;
    xep_t *e = &s->ep[dci];
    if (!e->busy) return true;
    xhci_cmd(0, 0, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)s->slot_id << 24), NULL, NULL);
    uint64_t deq = e->ring.phys + (uint64_t)e->ring.enq * sizeof(trb_t);
    deq |= e->ring.cycle ? 1u : 0u;
    xhci_cmd((uint32_t)deq, (uint32_t)(deq >> 32), 0,
             TRB_TYPE(TRB_SET_TR_DEQUEUE) | ((uint32_t)dci << 16) | ((uint32_t)s->slot_id << 24), NULL, NULL);
    e->busy = false;
    return true;
}

bool xhci_hcd_edpt_clear_stall(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
    (void)rhport;
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (!s || dci >= 32 || !s->ep[dci].open) return false;
    endpoint_recover(s, dci);
    return true;
}

/* Real link speed of an enumerated device (xHCI port speed ID: 1 full, 2 low, 3 high, 4+ SuperSpeed); 0 if unknown. */
uint8_t xhci_device_speed_id(uint8_t daddr) {
    xslot_t *s = slot_by_daddr(daddr);
    if (!s) return 0;
    return s->speed_id >= 4 ? 4 : s->speed_id;
}

/* ------------------------------------------------------------------ isochronous OUT streaming */
/* Queues one packet (<= 256 bytes, copied) on an open isochronous OUT endpoint. False when the pipeline is full. */
bool xhci_iso_submit(uint8_t daddr, uint8_t ep_addr, const void *data, uint16_t len) {
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (!s || dci < 2 || dci >= 32) return false;
    xep_t *e = &s->ep[dci];
    if (!e->open || !e->iso || (ep_addr & 0x80) || len > ISO_SLOT || len > e->mps) return false;
    if (e->iso_inflight >= ISO_SLOTS - 1u) return false;
    uint32_t slot = e->iso_next;
    uint8_t *dst = e->iso_pg[slot / 16u] + (slot % 16u) * ISO_SLOT;
    uint64_t ph = e->iso_phys[slot / 16u] + (slot % 16u) * ISO_SLOT;
    memcpy(dst, data, len);
    e->iso_next = (uint16_t)((slot + 1u) % ISO_SLOTS);
    e->iso_inflight++;
    ring_push(&e->ring, (uint32_t)ph, (uint32_t)(ph >> 32), len, TRB_TYPE(TRB_ISOCH) | TRB_IOC | (1u << 31));
    ring_db(s->slot_id, dci);
    return true;
}

uint32_t xhci_iso_inflight(uint8_t daddr, uint8_t ep_addr) {
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (!s || dci < 2 || dci >= 32 || !s->ep[dci].iso) return 0;
    return s->ep[dci].iso_inflight;
}

void xhci_iso_stats(uint8_t daddr, uint8_t ep_addr, uint32_t *done, uint32_t *errors) {
    xslot_t *s = slot_by_daddr(daddr);
    uint32_t dci = dci_of(ep_addr);
    if (done) *done = 0;
    if (errors) *errors = 0;
    if (!s || dci < 2 || dci >= 32) return;
    if (done) *done = s->ep[dci].iso_done;
    if (errors) *errors = s->ep[dci].iso_err;
}
