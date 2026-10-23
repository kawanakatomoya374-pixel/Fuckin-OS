/**
 * cos_devices.c - detected PS/2 and USB devices, as shown by Settings > Devices.
 *
 * Drivers register what they found (PS/2 probe results, TinyUSB mounts); the USB side collects the device
 * descriptor, the manufacturer/product/serial strings (asynchronously, through TinyUSB's control queue - never
 * blocking inside a callback) and the roles the class drivers claimed (keyboard, mouse, storage, hub). The ring-3
 * Settings app reads a snapshot through SYS_DEVICES_LIST.
 */
#include "cos_devices.h"
#include "string.h"
#include "serial.h"
#include "tusb.h"
#include "host/usbh.h"
#include "xhci_hcd.h"
#include "tusb_port_pc.h"

extern bool usb_is_initialized(void);
extern bool usb_ehci_active(void);
extern bool usb_xhci_active(void);
extern uint8_t xhci_device_speed_id(uint8_t daddr);

#define USB_MAX (CFG_TUH_DEVICE_MAX + 1)

typedef struct {
    bool used, mounted;
    cos_device_info_t d;
    /* string fetching */
    uint8_t stage;                       /* 0 idle, 1 manufacturer, 2 product, 3 serial, 4 done */
    uint8_t iMfr, iProd, iSerial;
    CFG_TUH_MEM_ALIGN uint16_t sbuf[64];
} usbdev_t;

static usbdev_t s_usb[USB_MAX];
static struct { bool present; uint8_t id0, id1; } s_kbd;
static struct { bool present; uint8_t id; bool wheel; uint8_t buttons; } s_mouse;

/* ------------------------------------------------------------------ PS/2 */
void cos_devices_ps2_keyboard(bool present, uint8_t id0, uint8_t id1) { s_kbd.present = present; s_kbd.id0 = id0; s_kbd.id1 = id1; }
void cos_devices_ps2_mouse(bool present, uint8_t id, bool wheel, uint8_t buttons) { s_mouse.present = present; s_mouse.id = id; s_mouse.wheel = wheel; s_mouse.buttons = buttons; }

/* ------------------------------------------------------------------ USB bookkeeping */
static usbdev_t *slot(uint8_t daddr) {
    if (daddr == 0 || daddr >= USB_MAX) return NULL;
    usbdev_t *u = &s_usb[daddr];
    if (!u->used) { memset(u, 0, sizeof *u); u->used = true; u->d.kind = COS_DEV_USB_DEVICE; u->d.usb_addr = daddr; }
    return u;
}
void cos_devices_usb_role(uint8_t daddr, uint8_t role) { usbdev_t *u = slot(daddr); if (u) u->d.roles |= role; }
void cos_devices_usb_storage(uint8_t daddr, uint32_t blocks, uint32_t bs) {
    usbdev_t *u = slot(daddr);
    if (!u) return;
    u->d.roles |= COS_DEVROLE_STORAGE; u->d.extra0 = blocks; u->d.extra1 = bs;
}

static const struct { uint16_t vid; const char *name; } k_vendors[] = {
    {0x046d, "Logitech"}, {0x045e, "Microsoft"}, {0x05ac, "Apple"}, {0x413c, "Dell"}, {0x0951, "Kingston"},
    {0x0781, "SanDisk"}, {0x1d6b, "Linux Foundation"}, {0x0627, "QEMU / Adomax"}, {0x1532, "Razer"},
    {0x04f2, "Chicony"}, {0x046a, "Cherry"}, {0x04ca, "Lite-On"}, {0x0bda, "Realtek"}, {0x8087, "Intel"},
    {0x090c, "Silicon Motion"}, {0x0930, "Toshiba"}, {0x154b, "PNY"}, {0x058f, "Alcor Micro"}, {0x04d9, "Holtek"},
    {0x1a81, "Holtek"}, {0x093a, "Pixart"}, {0x0461, "Primax"}, {0x03f0, "HP"}, {0x04b3, "IBM / Lenovo"},
    {0x17ef, "Lenovo"}, {0x0b05, "ASUS"}, {0x1c4f, "SiGma Micro"}, {0x258a, "SINOWEALTH"}, {0x0c45, "Microdia"},
    {0x04e8, "Samsung"}, {0x0bc2, "Seagate"}, {0x1058, "Western Digital"}, {0x0480, "Toshiba"}, {0x2109, "VIA Labs"},
    {0x05e3, "Genesys Logic"}, {0x0424, "Microchip / SMSC"}, {0x1a40, "Terminus"}, {0x0a5c, "Broadcom"},
};
static const char *vendor_name(uint16_t vid) {
    for (size_t i = 0; i < sizeof k_vendors / sizeof k_vendors[0]; ++i) if (k_vendors[i].vid == vid) return k_vendors[i].name;
    return NULL;
}

static void utf16_to_ascii(const uint16_t *in, char *out, size_t cap) {
    /* in[0] = bLength | bDescriptorType << 8, then UTF-16LE characters */
    size_t n = (in[0] & 0xFF) >= 2 ? ((in[0] & 0xFF) - 2) / 2 : 0, o = 0;
    for (size_t i = 0; i < n && o + 1 < cap; ++i) {
        uint16_t ch = in[1 + i];
        out[o++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
    }
    out[o] = 0;
}

static void fetch_next(uint8_t daddr);
static void string_done(tuh_xfer_t *x) {
    uint8_t daddr = x->daddr;
    usbdev_t *u = slot(daddr);
    if (!u || !u->mounted) return;
    if (x->result == XFER_RESULT_SUCCESS) {
        if (u->stage == 1) utf16_to_ascii(u->sbuf, u->d.vendor, sizeof u->d.vendor);
        else if (u->stage == 2) utf16_to_ascii(u->sbuf, u->d.name, sizeof u->d.name);
        else if (u->stage == 3) utf16_to_ascii(u->sbuf, u->d.serial, sizeof u->d.serial);
    }
    fetch_next(daddr);
}
static void fetch_next(uint8_t daddr) {
    usbdev_t *u = slot(daddr);
    if (!u) return;
    for (;;) {
        ++u->stage;
        if (u->stage > 3) { u->stage = 4; return; }
        uint8_t idx = u->stage == 1 ? u->iMfr : u->stage == 2 ? u->iProd : u->iSerial;
        if (!idx) continue;
        memset(u->sbuf, 0, sizeof u->sbuf);
        bool ok = tuh_descriptor_get_string(daddr, idx, 0x0409, u->sbuf, sizeof u->sbuf, string_done, 0);
        if (ok) return;                                    /* continues in string_done() */
    }
}

void cos_devices_usb_mounted(uint8_t daddr) {
    usbdev_t *u = slot(daddr);
    if (!u) return;
    tusb_desc_device_t dd;
    memset(&dd, 0, sizeof dd);
    tuh_bus_info_t bus;
    memset(&bus, 0, sizeof bus);
    tuh_bus_info_get(daddr, &bus);
    u->mounted = true; u->d.present = 1;
    if (tuh_descriptor_get_device_local(daddr, &dd)) {
        u->d.vid = dd.idVendor; u->d.pid = dd.idProduct; u->d.bcd_usb = dd.bcdUSB; u->d.bcd_dev = dd.bcdDevice;
        u->d.dev_class = dd.bDeviceClass; u->d.dev_subclass = dd.bDeviceSubClass; u->d.dev_protocol = dd.bDeviceProtocol;
        u->iMfr = dd.iManufacturer; u->iProd = dd.iProduct; u->iSerial = dd.iSerialNumber;
        if (dd.bDeviceClass == 9) u->d.roles |= COS_DEVROLE_HUB;
    }
    u->d.usb_hub_addr = bus.hub_addr; u->d.usb_hub_port = bus.hub_port;
    if (bus.rhport >= XHCI_RHPORT_BASE) {
        u->d.usb_ctrl = 2;
        u->d.usb_root_port = (uint8_t)(bus.rhport - XHCI_RHPORT_BASE + 1);
        uint8_t sp = xhci_device_speed_id(daddr);
        u->d.usb_speed = sp ? sp : (bus.speed == TUSB_SPEED_HIGH ? 3 : bus.speed == TUSB_SPEED_LOW ? 2 : 1);
    } else {
        u->d.usb_ctrl = 1;
        u->d.usb_root_port = 1;
        u->d.usb_speed = bus.speed == TUSB_SPEED_HIGH ? 3 : bus.speed == TUSB_SPEED_LOW ? 2 : 1;
    }
    u->stage = 0;
    fetch_next(daddr);
}

void cos_devices_usb_unmounted(uint8_t daddr) {
    if (daddr == 0 || daddr >= USB_MAX) return;
    memset(&s_usb[daddr], 0, sizeof s_usb[daddr]);
}

/* ------------------------------------------------------------------ snapshot */
static void sset(char *dst, size_t cap, const char *src) { strncpy(dst, src, cap - 1); dst[cap - 1] = 0; }

static void fill_usb_text(cos_device_info_t *d) {
    const char *role = (d->roles & COS_DEVROLE_KEYBOARD) ? "Keyboard" : (d->roles & COS_DEVROLE_POINTER_ABS) ? "Tablet / touch pointer" :
                       (d->roles & COS_DEVROLE_MOUSE) ? "Mouse" : (d->roles & COS_DEVROLE_STORAGE) ? "Mass storage" :
                       (d->roles & COS_DEVROLE_HUB) ? "Hub" : (d->roles & COS_DEVROLE_AUDIO) ? "Audio" : NULL;
    if (!d->name[0]) {
        if (role) { char b[64]; snprintf(b, sizeof b, "USB %s", role); sset(d->name, sizeof d->name, b); }
        else { char b[64]; snprintf(b, sizeof b, "USB device %04x:%04x", d->vid, d->pid); sset(d->name, sizeof d->name, b); }
    }
    if (!d->vendor[0]) { const char *v = vendor_name(d->vid); if (v) sset(d->vendor, sizeof d->vendor, v); }
    const char *sp = d->usb_speed == 4 ? "SuperSpeed 5 Gbit/s" : d->usb_speed == 3 ? "High speed 480 Mbit/s" :
                     d->usb_speed == 2 ? "Low speed 1.5 Mbit/s" : "Full speed 12 Mbit/s";
    char loc[48];
    if (d->usb_hub_addr) snprintf(loc, sizeof loc, "downstream port %u of a hub", d->usb_hub_port);
    else snprintf(loc, sizeof loc, "root port %u", d->usb_root_port);
    snprintf(d->detail, sizeof d->detail, "%s, %s, %s, address %u", d->usb_ctrl == 2 ? "xHCI" : "EHCI", sp, loc, d->usb_addr);
}

int cos_devices_list(cos_device_info_t *out, int max) {
    int n = 0;
    if (!out || max <= 0) return 0;
    #define NEXT() (n < max ? &out[n++] : NULL)
    cos_device_info_t *d;

    if ((d = NEXT())) {                                          /* PS/2 controller + devices */
        memset(d, 0, sizeof *d); d->kind = COS_DEV_PS2_CONTROLLER; d->present = 1;
        sset(d->name, sizeof d->name, "PS/2 controller (i8042)");
        sset(d->detail, sizeof d->detail, "ports 0x60 / 0x64, IRQ 1 (keyboard) and IRQ 12 (mouse)");
    }
    if ((d = NEXT())) {
        memset(d, 0, sizeof *d); d->kind = COS_DEV_PS2_KEYBOARD; d->present = s_kbd.present;
        d->roles = COS_DEVROLE_KEYBOARD; d->extra0 = s_kbd.id0; d->extra1 = s_kbd.id1;
        if (s_kbd.present) {
            const char *nm = (s_kbd.id0 == 0xAB && (s_kbd.id1 == 0x83 || s_kbd.id1 == 0xC1 || s_kbd.id1 == 0x41)) ? "PS/2 keyboard (MF2)" :
                             (s_kbd.id0 == 0xAB) ? "PS/2 keyboard" : "PS/2 keyboard (AT, no ID)";
            sset(d->name, sizeof d->name, nm);
            char b[96];
            if (s_kbd.id0) snprintf(b, sizeof b, "ID %02x %02x, port 1 of the i8042", s_kbd.id0, s_kbd.id1);
            else snprintf(b, sizeof b, "no ID bytes, port 1 of the i8042");
            sset(d->detail, sizeof d->detail, b);
        } else {
            sset(d->name, sizeof d->name, "PS/2 keyboard");
            sset(d->detail, sizeof d->detail, "no response from port 1");
        }
    }
    if ((d = NEXT())) {
        memset(d, 0, sizeof *d); d->kind = COS_DEV_PS2_MOUSE; d->present = s_mouse.present;
        d->roles = COS_DEVROLE_MOUSE | (s_mouse.wheel ? COS_DEVROLE_WHEEL : 0);
        d->extra0 = s_mouse.id; d->extra1 = s_mouse.buttons;
        if (s_mouse.present) {
            sset(d->name, sizeof d->name, s_mouse.id == 4 ? "PS/2 mouse (IntelliMouse Explorer)" : s_mouse.wheel ? "PS/2 mouse (IntelliMouse, wheel)" : "PS/2 mouse");
            char b[96]; snprintf(b, sizeof b, "device ID %u, %u buttons%s, port 2 of the i8042", s_mouse.id, s_mouse.buttons, s_mouse.wheel ? ", scroll wheel" : "");
            sset(d->detail, sizeof d->detail, b);
        } else {
            sset(d->name, sizeof d->name, "PS/2 mouse");
            sset(d->detail, sizeof d->detail, "no response from port 2");
        }
    }
    if (usb_ehci_active() && (d = NEXT())) {
        memset(d, 0, sizeof *d); d->kind = COS_DEV_USB_CONTROLLER; d->present = 1; d->usb_ctrl = 1;
        sset(d->name, sizeof d->name, "USB 2.0 host controller (EHCI)");
        char b[96]; snprintf(b, sizeof b, "MMIO 0x%llx", (unsigned long long)tusb_port_pc_base_addr());
        sset(d->detail, sizeof d->detail, b);
    }
    if (usb_xhci_active() && (d = NEXT())) {
        xhci_info_t xi;
        xhci_get_info(&xi);
        memset(d, 0, sizeof *d); d->kind = COS_DEV_USB_CONTROLLER; d->present = 1; d->usb_ctrl = 2;
        d->vid = xi.vendor; d->pid = xi.device; d->extra0 = xi.ports; d->extra1 = xi.slots;
        sset(d->name, sizeof d->name, "USB 3 host controller (xHCI)");
        uint32_t usb3 = 0, conn = 0;
        for (uint32_t p = 1; p <= xi.ports && p <= XHCI_MAX_ROOT_PORTS; ++p) { usb3 += xi.port_usb3[p]; conn += xi.port_connected[p]; }
        char b[96]; snprintf(b, sizeof b, "xHCI %u.%u, %u root ports (%u SuperSpeed), %u connected, %u slots, MMIO 0x%llx",
                             xi.usb_major, xi.usb_minor, xi.ports, usb3, conn, xi.slots, (unsigned long long)xi.mmio);
        sset(d->detail, sizeof d->detail, b);
    }
    for (uint32_t a = 1; a < USB_MAX; ++a) {
        usbdev_t *u = &s_usb[a];
        if (!u->used || !u->mounted || !(d = NEXT())) continue;
        *d = u->d;
        fill_usb_text(d);
    }
    #undef NEXT
    return n;
}
