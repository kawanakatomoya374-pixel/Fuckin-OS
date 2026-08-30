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

typedef struct {
    bool in_use;
    uint8_t dev_addr;
    uint8_t instance;
    uint8_t protocol; /* HID_ITF_PROTOCOL_KEYBOARD or _MOUSE */
    hid_keyboard_report_t prev_kb;
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

static void handle_mouse_report(const uint8_t* report, uint16_t len) {
    if (len < 3) return; /* need at least buttons+dx+dy */
    const hid_mouse_report_t* rep = (const hid_mouse_report_t*)report;
    int8_t wheel = (len >= 4) ? rep->wheel : 0;
    mouse_apply_usb_report(rep->buttons, rep->x, rep->y, wheel);
}

/* -------------------------------------------------------------- */
/* TinyUSB HID host callbacks                                      */
/* -------------------------------------------------------------- */

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, const uint8_t* desc_report, uint16_t desc_len) {
    (void)desc_report; (void)desc_len;

    uint8_t protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (protocol != HID_ITF_PROTOCOL_KEYBOARD && protocol != HID_ITF_PROTOCOL_MOUSE) {
        /* Not a boot-protocol keyboard or mouse (e.g. a gamepad, or a
         * composite device's other interface) - out of scope here. */
        return;
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

    serial_puts(protocol == HID_ITF_PROTOCOL_KEYBOARD ? "[USB] Keyboard connected.\n" : "[USB] Mouse connected.\n");

    /* Boot protocol gives a fixed, well-known report layout so this
     * bridge doesn't need to parse the device's HID report
     * descriptor at all. */
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
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

        } else if (inst->protocol == HID_ITF_PROTOCOL_MOUSE) {
            handle_mouse_report(report, len);
        }
    }

    /* Boot-protocol devices don't auto-repeat reports; each callback
     * must re-arm the next one. */
    tuh_hid_receive_report(dev_addr, instance);
}
