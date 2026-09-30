/**
 * tusb_hid_bridge.c - USB HID keyboard/mouse -> existing input pipeline
 *
 * TinyUSB's HID host class driver (class/hid/hid_host.c) already
 * does the USB-level work: finding HID interfaces, requesting
 * reports, re-arming reception. What it hands this file is raw
 * report bytes; what the rest of C-OS understands is PS/2 Set-1
 * scancodes (keyboard.c) and relative mouse packets (mouse.c). This
 * file is the translator between the two, using each device's HID
 * *boot protocol* report - a fixed, standardized 8-byte keyboard
 * report and a short button+dx+dy[+wheel] mouse report that every
 * USB keyboard/mouse supports, avoiding needing a full HID report-
 * descriptor parser for this first pass. Devices that only speak the
 * fuller "report protocol" (rare for basic keyboards/mice) won't be
 * understood here.
 */

#include "types.h"
#include "string.h"
#include "serial.h"
#include "mouse.h"
#include "keyboard.h"

#include "tusb.h"
#include "class/hid/hid_host.h"

/* -------------------------------------------------------------- */
/* USB HID keyboard usage ID -> PS/2 Set-1 scancode (+E0 flag)      */
/* -------------------------------------------------------------- */

typedef struct {
    uint8_t ps2;   /* 0 = no PS/2 equivalent, key is dropped */
    bool ext;      /* true = E0-prefixed (extended) key */
} hid_to_ps2_t;

/* Indexed by (HID usage - 0x04); covers the standard keyboard page
 * usage IDs 0x04-0x65. Irregular multi-byte PS/2 sequences
 * (PrintScreen, Pause) are deliberately left unmapped (0) rather than
 * approximated wrong. */
static const hid_to_ps2_t s_hid_kb_table[0x62] = {
    /* 0x04 */ {0x1E,false},{0x30,false},{0x2E,false},{0x20,false},{0x12,false},{0x21,false},{0x22,false},{0x23,false},
    /* 0x0C */ {0x17,false},{0x24,false},{0x25,false},{0x26,false},{0x32,false},{0x31,false},{0x18,false},{0x19,false},
    /* 0x14 */ {0x10,false},{0x13,false},{0x1F,false},{0x14,false},{0x16,false},{0x2F,false},{0x11,false},{0x2D,false},
    /* 0x1C */ {0x15,false},{0x2C,false},{0x02,false},{0x03,false},{0x04,false},{0x05,false},{0x06,false},{0x07,false},
    /* 0x24 */ {0x08,false},{0x09,false},{0x0A,false},{0x0B,false},{0x1C,false},{0x01,false},{0x0E,false},{0x0F,false},
    /* 0x2C */ {0x39,false},{0x0C,false},{0x0D,false},{0x1A,false},{0x1B,false},{0x2B,false},{0,    false},{0x27,false},
    /* 0x34 */ {0x28,false},{0x29,false},{0x33,false},{0x34,false},{0x35,false},{0x3A,false},{0x3B,false},{0x3C,false},
    /* 0x3C */ {0x3D,false},{0x3E,false},{0x3F,false},{0x40,false},{0x41,false},{0x42,false},{0x43,false},{0x44,false},
    /* 0x44 */ {0x57,false},{0x58,false},{0,    false},{0x46,false},{0,    false},{0x52,true }, {0x47,true }, {0x49,true },
    /* 0x4C */ {0x53,true }, {0x4F,true }, {0x51,true }, {0x4D,true }, {0x4B,true }, {0x50,true }, {0x48,true }, {0x45,false},
    /* 0x54 */ {0x35,true }, {0x37,false},{0x4A,false},{0x4E,false},{0x1C,true }, {0x4F,false},{0x50,false},{0x51,false},
    /* 0x5C */ {0x4B,false},{0x4C,false},{0x4D,false},{0x47,false},{0x48,false},{0x49,false},{0x52,false},{0x53,false},
    /* 0x64 */ {0x56,false},
};

/* -------------------------------------------------------------- */
/* Per-mounted-instance state                                      */
/* -------------------------------------------------------------- */

#define MAX_HID_INSTANCES 8

/* One input field located by parse_pointer_layout(): where it sits in
 * the report (bit offset/size, after any report-ID byte) and how to read
 * it (signedness from the logical range, absolute vs relative from the
 * Input item's flags). */
typedef struct {
    bool     valid;
    uint16_t bit_off;
    uint8_t  bits;
    bool     is_signed;
    bool     relative;
    int32_t  lmin, lmax;
} hid_field_t;

/* Report-protocol pointer layout (tablets, and mice that don't offer the
 * boot protocol) - see parse_pointer_layout(). */
typedef struct {
    bool        ok;
    bool        uses_report_ids;
    uint8_t     report_id;    /* the report that carries X/Y */
    hid_field_t x, y, wheel;
    uint16_t    button_bit_off;
    uint8_t     button_count;
} hid_pointer_layout_t;

#define HID_KIND_KEYBOARD      1
#define HID_KIND_BOOT_MOUSE    2
#define HID_KIND_REPORT_POINTER 3

typedef struct {
    bool in_use;
    uint8_t dev_addr;
    uint8_t instance;
    uint8_t protocol; /* HID_ITF_PROTOCOL_KEYBOARD / _MOUSE / _NONE */
    uint8_t kind;     /* HID_KIND_* */
    hid_keyboard_report_t prev_kb;
    hid_pointer_layout_t pointer;
} hid_instance_t;

static hid_instance_t s_instances[MAX_HID_INSTANCES];
static int s_device_count = 0;
/* Temporary bounded telemetry for the strict-QEMU HID bring-up. */
static unsigned int s_keyboard_report_trace_budget = 12;

static hid_instance_t* find_instance(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < MAX_HID_INSTANCES; i++) {
        if (s_instances[i].in_use && s_instances[i].dev_addr == dev_addr && s_instances[i].instance == instance) {
            return &s_instances[i];
        }
    }
    return NULL;
}

static hid_instance_t* alloc_instance(void) {
    for (int i = 0; i < MAX_HID_INSTANCES; i++) {
        if (!s_instances[i].in_use) return &s_instances[i];
    }
    return NULL;
}

int tusb_hid_bridge_device_count(void) {
    return s_device_count;
}

/* -------------------------------------------------------------- */
/* Keyboard report handling                                        */
/* -------------------------------------------------------------- */

static void apply_modifier_bit(uint8_t old_mod, uint8_t new_mod, uint8_t bit, uint8_t ps2, bool ext) {
    bool was = (old_mod & bit) != 0;
    bool now = (new_mod & bit) != 0;
    if (was == now) return;
    keyboard_inject_scancode(ps2, now, ext);
}

static void handle_keyboard_report(hid_instance_t* inst, const hid_keyboard_report_t* rep) {
    /* Modifiers arrive as a bitmask, not entries in keycode[], so
     * diff them against the previous report bit by bit. */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x01, 0x1D, false); /* L-Ctrl */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x02, 0x2A, false); /* L-Shift */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x04, 0x38, false); /* L-Alt */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x08, 0x5B, true);  /* L-GUI */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x10, 0x1D, true);  /* R-Ctrl */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x20, 0x36, false); /* R-Shift */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x40, 0x38, true);  /* R-Alt */
    apply_modifier_bit(inst->prev_kb.modifier, rep->modifier, 0x80, 0x5C, true);  /* R-GUI */

    /* Boot reports are a "which keys are down" snapshot (up to 6 at
     * once), not press/release events, so diff the two 6-key arrays:
     * a code in the new report but not the old one is a press, a
     * code in the old report but not the new one is a release. */
    for (int i = 0; i < 6; i++) {
        uint8_t code = rep->keycode[i];
        if (code == 0) continue;
        bool was_down = false;
        for (int j = 0; j < 6; j++) {
            if (inst->prev_kb.keycode[j] == code) { was_down = true; break; }
        }
        if (was_down) continue;
        if (code >= 0x04 && code < 0x04 + 0x62) {
            hid_to_ps2_t m = s_hid_kb_table[code - 0x04];
            if (m.ps2) keyboard_inject_scancode(m.ps2, true, m.ext);
        }
    }
    for (int i = 0; i < 6; i++) {
        uint8_t code = inst->prev_kb.keycode[i];
        if (code == 0) continue;
        bool still_down = false;
        for (int j = 0; j < 6; j++) {
            if (rep->keycode[j] == code) { still_down = true; break; }
        }
        if (still_down) continue;
        if (code >= 0x04 && code < 0x04 + 0x62) {
            hid_to_ps2_t m = s_hid_kb_table[code - 0x04];
            if (m.ps2) keyboard_inject_scancode(m.ps2, false, m.ext);
        }
    }

    inst->prev_kb = *rep;
}

static uint32_t s_mouse_reports = 0;
uint32_t tusb_hid_bridge_mouse_report_count(void) { return s_mouse_reports; }
static void handle_mouse_report(const uint8_t* report, uint16_t len) {
    if (len < 3) return; /* need at least buttons+dx+dy */
    if (++s_mouse_reports <= 3) {
        serial_puts("[USB] mouse report #");
        serial_putdec(s_mouse_reports);
        serial_puts(" len=");
        serial_putdec(len);
        serial_puts("\n");
    }
    const hid_mouse_report_t* rep = (const hid_mouse_report_t*)report;
    int8_t wheel = (len >= 4) ? rep->wheel : 0;

    mouse_apply_usb_report(rep->buttons, rep->x, rep->y, wheel);
}


/* -------------------------------------------------------------- */
/* HID report descriptor parsing (report protocol)                 */
/* -------------------------------------------------------------- */

/* A deliberately small HID report-descriptor walker: enough to find a
 * pointer's X, Y, wheel and buttons - their bit offsets, sizes,
 * signedness and absolute/relative flag - inside a Mouse or Pointer
 * application collection. That covers absolute tablets (QEMU's
 * usb-tablet, touch-style devices) and relative mice that don't offer
 * the boot protocol, without hardcoding any one device's layout.
 * Push/Pop, delimiters and long items are skipped; multi-report
 * devices are handled by tracking bit offsets per report ID. */
static int32_t hid_item_signed(const uint8_t* d, uint8_t size) {
    switch (size) {
        case 1: return (int8_t)d[0];
        case 2: return (int16_t)(d[0] | (d[1] << 8));
        case 4: return (int32_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24));
        default: return 0;
    }
}
static uint32_t hid_item_unsigned(const uint8_t* d, uint8_t size) {
    switch (size) {
        case 1: return d[0];
        case 2: return (uint32_t)(d[0] | (d[1] << 8));
        case 4: return (uint32_t)(d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24));
        default: return 0;
    }
}

static bool parse_pointer_layout(const uint8_t* d, uint16_t len, hid_pointer_layout_t* out) {
    memset(out, 0, sizeof(*out));
    uint32_t usage_page = 0, report_size = 0, report_count = 0;
    int32_t lmin = 0, lmax = 0;
    uint8_t report_id = 0;
    uint32_t usages[16]; int nusages = 0;
    uint32_t umin = 0, umax = 0; bool have_range = false;
    uint16_t offsets[256]; memset(offsets, 0, sizeof(offsets));
    int pointer_depth = 0, depth = 0;

    uint16_t i = 0;
    while (i < len) {
        uint8_t prefix = d[i];
        if (prefix == 0xFE) {                 /* long item: skip */
            if (i + 2 >= len) break;
            i += 3 + d[i + 1];
            continue;
        }
        uint8_t size = prefix & 0x3; if (size == 3) size = 4;
        uint8_t type = (prefix >> 2) & 0x3;   /* 0 main, 1 global, 2 local */
        uint8_t tag  = (prefix >> 4) & 0xF;
        if (i + 1 + size > len) break;
        const uint8_t* data = d + i + 1;
        uint32_t uval = hid_item_unsigned(data, size);
        int32_t  sval = hid_item_signed(data, size);

        if (type == 1) {                      /* global */
            switch (tag) {
                case 0x0: usage_page = uval; break;
                case 0x1: lmin = sval; break;
                case 0x2: lmax = sval; break;
                case 0x7: report_size = uval; break;
                case 0x8: report_id = (uint8_t)uval; out->uses_report_ids = true; break;
                case 0x9: report_count = uval; break;
                default: break;
            }
        } else if (type == 2) {               /* local */
            switch (tag) {
                case 0x0: if (nusages < 16) usages[nusages++] = (size == 4) ? uval : ((usage_page << 16) | uval); break;
                case 0x1: umin = (size == 4) ? uval : ((usage_page << 16) | uval); have_range = true; break;
                case 0x2: umax = (size == 4) ? uval : ((usage_page << 16) | uval); have_range = true; break;
                default: break;
            }
        } else if (type == 0) {               /* main */
            if (tag == 0xA) {                 /* Collection */
                ++depth;
                uint32_t u = nusages ? usages[0] : 0;
                if (uval == 1 && (u == 0x00010002u || u == 0x00010001u) && pointer_depth == 0) {
                    pointer_depth = depth;    /* Mouse or Pointer application collection */
                }
            } else if (tag == 0xC) {          /* End Collection */
                if (depth == pointer_depth) pointer_depth = 0;
                if (depth > 0) --depth;
            } else if (tag == 0x8) {          /* Input */
                bool constant = (uval & 0x1) != 0;
                bool variable = (uval & 0x2) != 0;
                bool relative = (uval & 0x4) != 0;
                uint16_t base = offsets[report_id];
                /* HID allows a logical max written as a negative
                 * number when the field is really unsigned (e.g.
                 * 0xFF for an 8-bit 0..255); treat that as unsigned. */
                int32_t fmin = lmin, fmax = lmax;
                if (fmax < fmin && report_size < 32) fmax = (int32_t)((1u << report_size) - 1u);
                if (!constant && variable && pointer_depth != 0) {
                    for (uint32_t k = 0; k < report_count && k < 64; ++k) {
                        uint32_t usage;
                        if (have_range) usage = umin + k;
                        else if ((int)k < nusages) usage = usages[k];
                        else usage = nusages ? usages[nusages - 1] : 0;
                        if (have_range && usage > umax) break;
                        uint16_t off = (uint16_t)(base + k * report_size);
                        hid_field_t f = { true, off, (uint8_t)report_size, fmin < 0, relative, fmin, fmax };
                        if (usage == 0x00010030u && !out->x.valid) { out->x = f; out->report_id = report_id; }
                        else if (usage == 0x00010031u && !out->y.valid) out->y = f;
                        else if (usage == 0x00010038u && !out->wheel.valid) out->wheel = f;
                        else if ((usage >> 16) == 0x0009u && (usage & 0xFFFF) == 1 && out->button_count == 0) {
                            out->button_bit_off = off;
                            out->button_count = (uint8_t)(report_count - k > 8 ? 8 : report_count - k);
                        }
                    }
                }
                offsets[report_id] = (uint16_t)(base + report_count * report_size);
            }
            nusages = 0; have_range = false; umin = umax = 0;   /* locals reset after every main item */
        }
        i += 1 + size;
    }
    out->ok = out->x.valid && out->y.valid;
    return out->ok;
}

static int32_t hid_read_field(const uint8_t* rep, uint16_t len_bits, const hid_field_t* f) {
    if (!f->valid || f->bits == 0 || f->bits > 32 || f->bit_off + f->bits > len_bits) return 0;
    uint32_t v = 0;
    for (uint8_t b = 0; b < f->bits; ++b) {
        uint16_t bit = (uint16_t)(f->bit_off + b);
        if (rep[bit >> 3] & (1u << (bit & 7))) v |= (1u << b);
    }
    if (f->is_signed && f->bits < 32 && (v & (1u << (f->bits - 1)))) v |= ~((1u << f->bits) - 1u);
    return (int32_t)v;
}

static void handle_report_pointer(const hid_instance_t* inst, const uint8_t* report, uint16_t len) {
    const hid_pointer_layout_t* L = &inst->pointer;
    if (L->uses_report_ids) {
        if (len < 1 || report[0] != L->report_id) return;   /* another report of this device */
        report++; len--;
    }
    uint16_t len_bits = (uint16_t)(len * 8u);
    uint8_t buttons = 0;
    for (uint8_t b = 0; b < L->button_count && b < 3; ++b) {
        uint16_t bit = (uint16_t)(L->button_bit_off + b);
        if (bit < len_bits && (report[bit >> 3] & (1u << (bit & 7)))) buttons |= (uint8_t)(1u << b);
    }
    int32_t wheel = hid_read_field(report, len_bits, &L->wheel);
    if (wheel > 127) wheel = 127;
    if (wheel < -127) wheel = -127;
    int32_t rx = hid_read_field(report, len_bits, &L->x);
    int32_t ry = hid_read_field(report, len_bits, &L->y);

    if (!L->x.relative && !L->y.relative) {
        extern uint64_t SCREEN_W, SCREEN_H;
        int64_t xr = (int64_t)L->x.lmax - L->x.lmin; if (xr <= 0) xr = 1;
        int64_t yr = (int64_t)L->y.lmax - L->y.lmin; if (yr <= 0) yr = 1;
        int32_t sx = (int32_t)(((int64_t)(rx - L->x.lmin) * (int64_t)(SCREEN_W ? SCREEN_W - 1 : 0)) / xr);
        int32_t sy = (int32_t)(((int64_t)(ry - L->y.lmin) * (int64_t)(SCREEN_H ? SCREEN_H - 1 : 0)) / yr);
        mouse_apply_usb_absolute(buttons, sx, sy, (int8_t)wheel);
    } else {
        /* Relative report-protocol mouse: deltas can exceed int8 - split. */
        while (rx != 0 || ry != 0) {
            int32_t sx = rx > 127 ? 127 : (rx < -127 ? -127 : rx);
            int32_t sy = ry > 127 ? 127 : (ry < -127 ? -127 : ry);
            mouse_apply_usb_report(buttons, (int8_t)sx, (int8_t)sy, (int8_t)wheel);
            rx -= sx; ry -= sy; wheel = 0;
        }
        if (rx == 0 && ry == 0) mouse_apply_usb_report(buttons, 0, 0, (int8_t)wheel);
    }
}

/* -------------------------------------------------------------- */
/* TinyUSB HID host callbacks                                      */
/* -------------------------------------------------------------- */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, const uint8_t* desc_report, uint16_t desc_len) {
    uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
    hid_pointer_layout_t layout;
    bool pointer_ok = false;
    if (protocol != HID_ITF_PROTOCOL_KEYBOARD && protocol != HID_ITF_PROTOCOL_MOUSE) {
        /* No boot protocol (tablets, many modern mice, composite devices'
         * extra interfaces): use the report descriptor itself. */
        pointer_ok = desc_report && desc_len && parse_pointer_layout(desc_report, desc_len, &layout);
        if (!pointer_ok) {
            serial_puts("[USB] HID: interface is neither boot keyboard/mouse nor a pointer - ignored.\n");
            return;
        }
    }

    hid_instance_t* inst = alloc_instance();
    if (!inst) {
        serial_puts("[USB] HID: too many devices mounted, ignoring one.\n");
        return;
    }
    memset(inst, 0, sizeof(*inst));
    inst->in_use = true;
    inst->dev_addr = dev_addr;
    inst->instance = instance;
    inst->protocol = protocol;
    s_device_count++;

    if (protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        inst->kind = HID_KIND_KEYBOARD;
        serial_puts("[USB] Keyboard connected.\n");
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    } else if (protocol == HID_ITF_PROTOCOL_MOUSE) {
        inst->kind = HID_KIND_BOOT_MOUSE;
        serial_puts("[USB] Mouse connected.\n");
        /* Boot protocol gives a fixed, well-known report layout. */
        tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    } else {
        inst->kind = HID_KIND_REPORT_POINTER;
        inst->pointer = layout;
        serial_puts(layout.x.relative ? "[USB] Mouse (report protocol) connected.\n"
                                      : "[USB] Tablet / absolute pointer connected.\n");
    }
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        serial_puts("[USB] HID: failed to start receiving reports.\n");
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    hid_instance_t* inst = find_instance(dev_addr, instance);
    if (!inst) return;
    inst->in_use = false;
    if (s_device_count > 0) s_device_count--;
    serial_puts("[USB] HID device disconnected.\n");
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len) {
    hid_instance_t* inst = find_instance(dev_addr, instance);
    	if (inst) {
		if (inst->protocol == HID_ITF_PROTOCOL_KEYBOARD && len >= sizeof(hid_keyboard_report_t)) {
			if (s_keyboard_report_trace_budget != 0) {
				serial_puts("[USB] HID keyboard report len=0x");
				serial_puthex(len);
				serial_puts(" modifier=0x");
				serial_puthex(report[0]);
				serial_puts(" key0=0x");
				serial_puthex(report[2]);
				serial_puts("\n");
				s_keyboard_report_trace_budget--;
			}
			handle_keyboard_report(inst, (const hid_keyboard_report_t*)report);

        } else if (inst->kind == HID_KIND_BOOT_MOUSE) {
            handle_mouse_report(report, len);
        } else if (inst->kind == HID_KIND_REPORT_POINTER) {
            handle_report_pointer(inst, report, len);
        }
    }

    /* Boot-protocol devices don't auto-repeat reports; each callback
     * must re-arm the next one. */
    tuh_hid_receive_report(dev_addr, instance);
}
