/**
 * acpi_power.c - ACPI / AML / EC / SMBus power discovery
 *
 * This is a practical, freestanding implementation intended for battery
 * discovery on mobile hardware. It discovers ACPI tables, probes a smart
 * battery over SMBus when available, exposes a small EC transport, and
 * includes a compact AML interpreter subset for common battery methods.
 *
 * The AML engine is intentionally conservative: it supports the common
 * object forms used by battery methods (_BIF/_BST/_STA/_PSR) and falls back
 * cleanly if a BIOS uses more exotic AML.
 */

#include "acpi_power.h"
#include "types.h"
#include "string.h"
#include "io.h"
#include "serial.h"
#include "pci.h"
#include "mm/paging.h"
#include "sync.h"
#include "timer.h"

typedef struct acpi_value acpi_value_t; static acpi_value_t acpi_try_eval_first_method(const char* const* methods, const acpi_value_t* args, uint8_t arg_count, const char** matched_name, bool* success);

/* ACPICA bridge hooks */
extern bool acpi_power_acpica_available(void);
extern bool acpi_power_acpica_is_ready(void);
extern bool acpi_power_acpica_initialize(void);
extern bool acpi_power_acpica_query_battery(uint64_t* percent, bool* charging, uint64_t* minutes);
extern bool acpi_power_acpica_query_thermal(uint64_t* celsius, uint64_t* fan_rpm, uint64_t* cstate_count);
extern bool acpi_power_acpica_ec_is_available(void);
extern bool acpi_power_acpica_ec_read8(uint8_t reg, uint8_t* out);
extern bool acpi_power_acpica_ec_write8(uint8_t reg, uint8_t value);
extern bool acpi_power_acpica_suspend(uint8_t sleep_state);
extern bool acpi_power_acpica_resume(uint8_t sleep_state);

#define ACPI_MAX_OBJECTS        256u
#define ACPI_MAX_METHOD_ARGS      7u
#define ACPI_MAX_LOCALS           8u
#define ACPI_MAX_STACK           64u
#define ACPI_MAX_PKG_ELEMS       16u
#define ACPI_MAX_PATH           128u
#define ACPI_MAX_NAME            32u
#define ACPI_MAX_SOURCE_LABEL    32u
#define ACPI_SCAN_START    0x000E0000ULL
#define ACPI_SCAN_END      0x00100000ULL
#define ACPI_SCAN_STEP     16ULL
#define ACPI_EBDA_PTR      0x0000040EULL

#define SMBUS_HOST_STS      0x00
#define SMBUS_HOST_CNT      0x02
#define SMBUS_HOST_CMD      0x03
#define SMBUS_XMIT_SLVA     0x04
#define SMBUS_HST_D0        0x05
#define SMBUS_HST_D1        0x06

#define SMBUS_STS_HOST_BUSY 0x01
#define SMBUS_STS_INTR      0x02
#define SMBUS_STS_DEV_ERR   0x04
#define SMBUS_STS_BUS_ERR   0x08
#define SMBUS_STS_FAILED    0x10
#define SMBUS_STS_BYTE_DONE 0x80

#define SMBUS_CNT_START     0x40
#define SMBUS_CNT_PROTO_BYTE 0x08
#define SMBUS_CNT_PROTO_WORD 0x0C

#define EC_STATUS_OBF       0x01
#define EC_STATUS_IBF       0x02
#define EC_CMD_READ         0x80
#define EC_CMD_WRITE        0x81
#define EC_CMD_BURST_ENABLE 0x82
#define EC_CMD_BURST_DISABLE 0x83
#define EC_CMD_QUERY        0x84

#define ACPI_SPACE_SYSTEM_MEMORY  0x00
#define ACPI_SPACE_SYSTEM_IO      0x01
#define ACPI_SPACE_PCI_CONFIG     0x02
#define ACPI_SPACE_EMBEDDED_CTRL  0x03
#define ACPI_SPACE_SMBUS          0x04

#define AML_ZERO_OP       0x00
#define AML_ONE_OP        0x01
#define AML_ALIAS_OP      0x06
#define AML_NAME_OP       0x08
#define AML_BYTE_PREFIX   0x0A
#define AML_WORD_PREFIX   0x0B
#define AML_DWORD_PREFIX  0x0C
#define AML_STRING_PREFIX 0x0D
#define AML_QWORD_PREFIX  0x0E
#define AML_SCOPE_OP      0x10
#define AML_BUFFER_OP     0x11
#define AML_PACKAGE_OP    0x12
#define AML_VAR_PACKAGE_OP 0x13
#define AML_METHOD_OP     0x14
#define AML_EXT_OP_PREFIX 0x5B
#define AML_STORE_OP      0x70
#define AML_REF_OF_OP     0x71
#define AML_ADD_OP        0x72
#define AML_CONCAT_OP     0x73
#define AML_SHIFT_LEFT_OP  0x74
#define AML_SHIFT_RIGHT_OP 0x75
#define AML_AND_OP        0x76
#define AML_NAND_OP       0x77
#define AML_OR_OP         0x78
#define AML_NOR_OP        0x79
#define AML_XOR_OP        0x7A
#define AML_NOT_OP        0x7B
#define AML_FIND_LS_BIT   0x7C
#define AML_FIND_RS_BIT   0x7D
#define AML_DEREF_OF_OP   0x7E
#define AML_INDEX_OP      0x88
#define AML_LAND_OP       0x90
#define AML_LOR_OP        0x91
#define AML_LNOT_OP       0x92
#define AML_LEQUAL_OP     0x93
#define AML_LGREATER_OP   0x94
#define AML_LLESS_OP      0x95
#define AML_IF_OP         0xA0
#define AML_ELSE_OP       0xA1
#define AML_WHILE_OP      0xA2
#define AML_RETURN_OP     0xA4
#define AML_BREAK_OP      0xA5
#define AML_SIZEOF_OP     0x87
#define AML_MUTEX_OP      0xA2 /* not used; compatibility placeholder */
#define AML_OPREGION_OP   0x80
#define AML_FIELD_OP      0x81
#define AML_DEVICE_OP     0x82
#define AML_POWERRES_OP   0x84
#define AML_THERMAL_OP    0x85
#define AML_PROCESSOR_OP  0x83
#define AML_INDEXFIELD_OP 0x86
#define AML_BANKFIELD_OP  0x87

#define ACPI_BATTERY_SMBUS_ADDR 0x0B
#define ACPI_SMBUS_CMD_REMAINING_CAPACITY 0x0F
#define ACPI_SMBUS_CMD_FULL_CHARGE_CAPACITY 0x10
#define ACPI_SMBUS_CMD_RELATIVE_SOC 0x0D
#define ACPI_SMBUS_CMD_VOLTAGE 0x09
#define ACPI_SMBUS_CMD_TEMPERATURE 0x08
#define ACPI_SMBUS_CMD_CURRENT 0x0A
#define ACPI_SMBUS_CMD_STATUS 0x16

/* ------------------------------------------------------------
 * ACPI tables
 * ------------------------------------------------------------ */

typedef struct PACKED {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_phys;
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} acpi_rsdp_t;

typedef struct PACKED {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_sdt_header_t;

typedef struct PACKED {
    acpi_sdt_header_t header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t  reserved1;
    uint8_t  preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_cnt;
    uint32_t pm1a_evt_blk;
    uint32_t pm1b_evt_blk;
    uint32_t pm1a_cnt_blk;
    uint32_t pm1b_cnt_blk;
    uint32_t pm2_cnt_blk;
    uint32_t pm_tmr_blk;
    uint32_t gpe0_blk;
    uint32_t gpe1_blk;
    uint8_t  pm1_evt_len;
    uint8_t  pm1_cnt_len;
    uint8_t  pm2_cnt_len;
    uint8_t  pm_tmr_len;
    uint8_t  gpe0_blk_len;
    uint8_t  gpe1_blk_len;
    uint8_t  gpe1_base;
    uint8_t  cst_cnt;
    uint16_t p_lvl2_lat;
    uint16_t p_lvl3_lat;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alrm;
    uint8_t  mon_alrm;
    uint8_t  century;
    uint16_t iapc_boot_arch;
    uint8_t  reserved2;
    uint32_t flags;
    uint8_t  reset_reg[12];
    uint8_t  reset_value;
    uint8_t  reserved3[3];
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
} acpi_fadt_t;

typedef struct PACKED {
    uint8_t  space_id;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} acpi_gas_t;

typedef struct PACKED {
    acpi_sdt_header_t header;
    uint8_t  hardware_signature[8];
    uint8_t  firmware_control[4];
    uint8_t  dsdt_physical_address[4];
    uint8_t  smi_command_port[4];
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint8_t  s4bios_req;
    uint8_t  pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm1b_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t  pm1_event_length;
    uint8_t  pm1_control_length;
    uint8_t  pm2_control_length;
    uint8_t  pm_timer_length;
    uint8_t  gpe0_length;
    uint8_t  gpe1_length;
    uint8_t  gpe1_base;
    uint8_t  cst_control;
    uint16_t p_lvl2_latency;
    uint16_t p_lvl3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t  duty_offset;
    uint8_t  duty_width;
    uint8_t  day_alarm;
    uint8_t  month_alarm;
    uint8_t  century;
    uint16_t boot_arch_flags;
    uint8_t  reserved2;
    uint32_t flags;
    acpi_gas_t reset_reg;
    uint8_t  reset_value;
    uint8_t  reserved3[3];
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
    acpi_gas_t x_pm1a_event_block;
    acpi_gas_t x_pm1b_event_block;
    acpi_gas_t x_pm1a_control_block;
    acpi_gas_t x_pm1b_control_block;
    acpi_gas_t x_pm2_control_block;
    acpi_gas_t x_pm_timer_block;
    acpi_gas_t x_gpe0_block;
    acpi_gas_t x_gpe1_block;
} acpi_fadt_ex_t;

/* ------------------------------------------------------------
 * AML objects / values
 * ------------------------------------------------------------ */

typedef enum {
    ACPI_VAL_NONE = 0,
    ACPI_VAL_INT,
    ACPI_VAL_STRING,
    ACPI_VAL_BUFFER,
    ACPI_VAL_PACKAGE,
    ACPI_VAL_REFERENCE,
    ACPI_VAL_METHOD,
} acpi_val_kind_t;

typedef struct acpi_value acpi_value_t;

typedef struct {
    acpi_val_kind_t kind;
    uint64_t integer;
    char string[64];
    uint8_t buffer[64];
    uint64_t buffer_len;
} acpi_package_elem_t;

typedef struct {
    acpi_package_elem_t elems[ACPI_MAX_PKG_ELEMS];
    uint64_t count;
} acpi_package_t;

struct acpi_value {
    acpi_val_kind_t kind;
    uint64_t integer;
    char string[64];
    uint8_t buffer[64];
    uint64_t buffer_len;
    acpi_package_t package;
    const void* reference;
};

typedef enum {
    ACPI_OBJ_EMPTY = 0,
    ACPI_OBJ_INTEGER,
    ACPI_OBJ_STRING,
    ACPI_OBJ_BUFFER,
    ACPI_OBJ_METHOD,
    ACPI_OBJ_REGION,
    ACPI_OBJ_FIELD
} acpi_obj_kind_t;

typedef struct {
    char path[ACPI_MAX_PATH];
    char name[ACPI_MAX_NAME];
    acpi_obj_kind_t kind;
    bool present;
    union {
        uint64_t integer;
        char string[64];
        struct {
            const uint8_t* body;
            uint64_t length;
            uint8_t arg_count;
        } method;
        struct {
            uint8_t space_id;
            uint64_t base;
            uint64_t length;
        } region;
        struct {
            char region_path[ACPI_MAX_PATH];
            uint64_t bit_offset;
            uint64_t bit_length;
            uint8_t access_size;
        } field;
        struct {
            uint8_t bytes[64];
            uint64_t length;
        } buffer;
    } u;
} acpi_object_t;

typedef struct {
    acpi_value_t args[ACPI_MAX_METHOD_ARGS];
    acpi_value_t locals[ACPI_MAX_LOCALS];
} acpi_method_env_t;

typedef struct {
    const uint8_t* ptr;
    const uint8_t* end;
} acpi_cursor_t;

/* ------------------------------------------------------------
 * Global state
 * ------------------------------------------------------------ */

static bool g_acpi_ready = false;
static bool g_acpi_available = false;
static bool g_smbus_ready = false;
static bool g_ec_ready = false;
static bool g_aml_ready = false;
static uint64_t g_smbus_base = 0;
/* Re-entry guard: the GUI taskbar calls into our public API on EVERY paint
 * frame (and once preemptive multitasking is enabled, the heartbeat thread
 * and apps can both reach us concurrently), so without this we used to
 * re-parse the ACPI tables / AML namespace dozens of times per second
 * whenever a window was open, which is the entire reason the OS felt
 * "heavy" the moment any app was launched. */
static volatile bool g_acpi_init_in_progress = false;
/* Once we've done one heavy refresh attempt, don't keep retrying it on
 * every paint frame when there is no real battery source (QEMU/emulator
 * case). The fallback model then just returns the cached battery. */
static bool g_heavy_refresh_done = false;
/* Boot log suppression: in 4.0.7 the per-section "[ACPI] tables discovered",
 * "[ACPI] AML namespace parsed (subset)" and "[ACPI] No real battery source"
 * messages used to print every time a different code path re-entered
 * acpi_power_init() -- the GUI paint thread, the heartbeat thread and the
 * notification-GC thread each triggered a fresh copy on top of the boot
 * path, so 3-5 copies of the entire ACPI banner would appear at random
 * in the log. We now keep the messages identical but print each of them at
 * most once per boot, so the log stays clean and the per-frame cost of
 * bordered printk calls is paid only on the very first invocation. */
static bool g_acpi_log_tables_discovered = false;
static bool g_acpi_log_aml_parsed = false;
static bool g_acpi_log_no_real_battery = false;
static bool g_acpi_log_tables_not_found = false;
static bool g_acpi_log_acpica_active = false;
static bool g_acpi_log_smbus_active = false;
static bool g_acpi_log_battery_aml = false;
static uint64_t g_last_heavy_refresh_ms = 0;
static uint16_t g_ec_cmd_port = 0x0066;
static uint16_t g_ec_data_port = 0x0062;
static const acpi_rsdp_t* g_rsdp = NULL;

/* Provided by kernel.c: physical address of the RSDP as forwarded by the
   bootloader's Multiboot2 tag (type 14/15), if any. This works
   identically whether the machine booted via legacy BIOS or UEFI, since
   GRUB itself resolves the RSDP from the right source (BIOS EBDA scan vs
   EFI configuration table) before handing it to us -- so this kernel does
   not need any UEFI-specific lookup path of its own. */
extern uint64_t cos_mb2_get_acpi_rsdp(void);
static const acpi_fadt_t* g_fadt = NULL;
static const acpi_sdt_header_t* g_dsdt = NULL;
static const uint8_t* g_dsdt_aml = NULL;
static uint64_t g_battery_percent = 100;
static bool g_battery_charging = false;
static uint64_t g_battery_minutes = 0;
static bool g_have_battery = false;
static char g_source_label[ACPI_MAX_SOURCE_LABEL] = "fallback";
static acpi_object_t g_objects[ACPI_MAX_OBJECTS];
static uint64_t g_object_count = 0;
static const char* g_method_bif = NULL;
static const char* g_method_bst = NULL;
static const char* g_method_sta = NULL;
static const char* g_method_psr = NULL;

/* ------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------ */

static void acpi_strncpy0(char* dst, const char* src, size_t size) {
    if (!dst || size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < size && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static bool acpi_streq(const char* a, const char* b) {
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static uint8_t acpi_checksum(const uint8_t* data, uint64_t len) {
    uint32_t sum = 0;
    for (uint64_t i = 0; i < len; ++i) sum = (sum + data[i]) & 0xFFu;
    return (uint8_t)sum;
}

/* IMPORTANT: these used to cast the raw physical address straight to a
 * pointer, which only works if physical address == virtual address (an
 * identity mapping) for that address. That was true early on when the
 * bootloader's own page tables were still active, but paging_init() later
 * installs a much narrower map: identities only 0.._kernel_end (the kernel
 * image + heap, a few tens of MB) plus a separate higher-half mapping of
 * *all* physical RAM at PHYS_TO_VIRT(phys). ACPI tables (RSDP/FADT/DSDT
 * etc.) are placed by the firmware wherever it likes - on this BIOS/QEMU
 * combination that is typically up near the top of low memory (close to
 * the 1GB mark), well above _kernel_end - so treating "phys" as a usable
 * pointer directly faulted with "page not present" the first time ACPI
 * was touched (acpi_power_init(), lazily triggered by the taskbar's
 * battery gauge on the very first GUI frame after boot). Going through
 * PHYS_TO_VIRT() uses the mapping that actually covers all of RAM. */
static const uint8_t* acpi_phys_ptr(uint64_t phys) {
    return (const uint8_t*)(uintptr_t)PHYS_TO_VIRT(phys);
}

static uint8_t acpi_read_u8(uint64_t phys) {
    return *(volatile const uint8_t*)(uintptr_t)PHYS_TO_VIRT(phys);
}

static uint16_t acpi_read_u16(uint64_t phys) {
    uint16_t v;
    memcpy(&v, (const void*)(uintptr_t)PHYS_TO_VIRT(phys), sizeof(v));
    return v;
}

static uint32_t acpi_read_u32(uint64_t phys) {
    uint32_t v;
    memcpy(&v, (const void*)(uintptr_t)PHYS_TO_VIRT(phys), sizeof(v));
    return v;
}

static uint64_t acpi_read_u64(uint64_t phys) {
    uint64_t v;
    memcpy(&v, (const void*)(uintptr_t)PHYS_TO_VIRT(phys), sizeof(v));
    return v;
}

static uint64_t acpi_cursor_read_pkglen(acpi_cursor_t* c) {
    if (!c || c->ptr >= c->end) return 0;
    uint8_t lead = *c->ptr++;
    uint64_t len = lead & 0x3Fu;
    uint8_t byte_count = (lead >> 6) & 0x03u;
    for (uint8_t i = 0; i < byte_count && c->ptr < c->end; ++i) {
        len |= ((uint64_t)(*c->ptr++)) << (4u + (8u * i));
    }
    return len;
}

static bool acpi_cursor_read_name4(acpi_cursor_t* c, char out[5]) {
    if (!c || !out || c->ptr + 4 > c->end) return false;
    out[0] = (char)c->ptr[0];
    out[1] = (char)c->ptr[1];
    out[2] = (char)c->ptr[2];
    out[3] = (char)c->ptr[3];
    out[4] = '\0';
    c->ptr += 4;
    return true;
}

static void acpi_make_path(char* out, size_t out_size, const char* scope, const char* name) {
    if (!out || out_size == 0) return;
    if (!scope || scope[0] == '\0' || strcmp(scope, "\\") == 0) {
        if (name && name[0] == '\\') {
            acpi_strncpy0(out, name, out_size);
        } else {
            out[0] = '\\';
            out[1] = '\0';
            if (name && name[0]) {
                size_t len = strlen(out);
                if (len + 1 < out_size) {
                    if (len > 1 && out[len - 1] != '\\') {
                        out[len++] = '.';
                        out[len] = '\0';
                    }
                    strncat(out, name, out_size - len - 1);
                }
            }
        }
        return;
    }

    acpi_strncpy0(out, scope, out_size);
    size_t len = strlen(out);
    if (len + 1 < out_size) {
        if (out[len - 1] != '.') {
            out[len++] = '.';
            out[len] = '\0';
        }
        strncat(out, name, out_size - len - 1);
    }
}

static void acpi_path_parent(char* path) {
    if (!path) return;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '.') path[--len] = '\0';
    char* last = strrchr(path, '.');
    if (last && last > path) {
        *last = '\0';
    } else {
        acpi_strncpy0(path, "\\", ACPI_MAX_PATH);
    }
}

static acpi_object_t* acpi_find_object_exact(const char* path) {
    if (!path) return NULL;
    for (uint64_t i = 0; i < g_object_count; ++i) {
        if (g_objects[i].present && strcmp(g_objects[i].path, path) == 0) return &g_objects[i];
    }
    return NULL;
}

static acpi_object_t* acpi_find_object_relative(const char* scope, const char* name) {
    if (!name || !name[0]) return NULL;
    if (name[0] == '\\') return acpi_find_object_exact(name);

    char candidate[ACPI_MAX_PATH];
    char scope_buf[ACPI_MAX_PATH];
    acpi_strncpy0(scope_buf, scope ? scope : "\\", sizeof(scope_buf));

    /* Try current scope and walk upwards. */
    for (int depth = 0; depth < 8; ++depth) {
        candidate[0] = '\0';
        if (strcmp(scope_buf, "\\") == 0) {
            snprintf(candidate, sizeof(candidate), "\\%s", name);
        } else {
            snprintf(candidate, sizeof(candidate), "%s.%s", scope_buf, name);
        }
        acpi_object_t* obj = acpi_find_object_exact(candidate);
        if (obj) return obj;
        if (strcmp(scope_buf, "\\") == 0) break;
        acpi_path_parent(scope_buf);
    }

    /* Final attempt: raw name in root. */
    snprintf(candidate, sizeof(candidate), "\\%s", name);
    return acpi_find_object_exact(candidate);
}

static acpi_object_t* acpi_new_object(const char* path, const char* name, acpi_obj_kind_t kind) {
    if (g_object_count >= ACPI_MAX_OBJECTS) return NULL;
    acpi_object_t* obj = &g_objects[g_object_count++];
    memset(obj, 0, sizeof(*obj));
    obj->present = true;
    obj->kind = kind;
    acpi_strncpy0(obj->path, path, sizeof(obj->path));
    acpi_strncpy0(obj->name, name, sizeof(obj->name));
    return obj;
}

static void acpi_add_builtin_methods(void) {
    g_method_bif = "\\_BIF";
    g_method_bst = "\\_BST";
    g_method_sta = "\\_STA";
    g_method_psr = "\\_PSR";
}

/* ------------------------------------------------------------
 * RSDP / table discovery
 * ------------------------------------------------------------ */

static const acpi_rsdp_t* acpi_find_rsdp(void) {
    /* Prefer the RSDP the bootloader already found for us via Multiboot2.
       This is the piece that actually matters for UEFI: on UEFI firmware
       there may be no valid legacy BIOS EBDA/0xE0000-0xFFFFF region to
       scan at all, but GRUB always resolves the RSDP correctly for the
       firmware it's running under and passes it along identically either
       way, so checking this first makes RSDP discovery firmware-agnostic
       without this kernel needing to touch the EFI System Table itself. */
    uint64_t mb2_rsdp_phys = cos_mb2_get_acpi_rsdp();
    if (mb2_rsdp_phys != 0) {
        const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)acpi_phys_ptr(mb2_rsdp_phys);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0) {
            if (acpi_checksum((const uint8_t*)rsdp, 20) == 0) return rsdp;
            if (rsdp->revision >= 2 && rsdp->length >= sizeof(acpi_rsdp_t) &&
                acpi_checksum((const uint8_t*)rsdp, rsdp->length) == 0) return rsdp;
        }
        /* Tag was present but didn't validate -- fall through to the
           legacy scan rather than trusting it blindly. */
    }

    uint16_t ebda_seg = acpi_read_u16(ACPI_EBDA_PTR);
    uint64_t ebda_phys = ((uint64_t)ebda_seg) << 4;
    if (ebda_phys >= ACPI_SCAN_START && ebda_phys < ACPI_SCAN_END) {
        for (uint64_t off = 0; off < 1024; off += ACPI_SCAN_STEP) {
            const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)acpi_phys_ptr(ebda_phys + off);
            if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0) {
                if (acpi_checksum((const uint8_t*)rsdp, 20) == 0) return rsdp;
                if (rsdp->revision >= 2 && rsdp->length >= sizeof(acpi_rsdp_t) &&
                    acpi_checksum((const uint8_t*)rsdp, rsdp->length) == 0) return rsdp;
            }
        }
    }

    for (uint64_t phys = ACPI_SCAN_START; phys < ACPI_SCAN_END; phys += ACPI_SCAN_STEP) {
        const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)acpi_phys_ptr(phys);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0) {
            if (acpi_checksum((const uint8_t*)rsdp, 20) == 0) return rsdp;
            if (rsdp->revision >= 2 && rsdp->length >= sizeof(acpi_rsdp_t) &&
                acpi_checksum((const uint8_t*)rsdp, rsdp->length) == 0) return rsdp;
        }
    }
    return NULL;
}

static const acpi_sdt_header_t* acpi_find_table_by_sig(const char sig[4]) {
    if (!g_rsdp) return NULL;

    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_phys) {
        const acpi_sdt_header_t* xsdt = (const acpi_sdt_header_t*)acpi_phys_ptr(g_rsdp->xsdt_phys);
        if (memcmp(xsdt->signature, "XSDT", 4) != 0) return NULL;
        uint64_t entries = (xsdt->length - sizeof(acpi_sdt_header_t)) / 8u;
        const uint64_t* table = (const uint64_t*)((const uint8_t*)xsdt + sizeof(acpi_sdt_header_t));
        for (uint64_t i = 0; i < entries; ++i) {
            const acpi_sdt_header_t* hdr = (const acpi_sdt_header_t*)acpi_phys_ptr(table[i]);
            if (memcmp(hdr->signature, sig, 4) == 0) return hdr;
        }
    }

    if (g_rsdp->rsdt_phys) {
        const acpi_sdt_header_t* rsdt = (const acpi_sdt_header_t*)acpi_phys_ptr(g_rsdp->rsdt_phys);
        if (memcmp(rsdt->signature, "RSDT", 4) != 0) return NULL;
        uint64_t entries = (rsdt->length - sizeof(acpi_sdt_header_t)) / 4u;
        const uint32_t* table = (const uint32_t*)((const uint8_t*)rsdt + sizeof(acpi_sdt_header_t));
        for (uint64_t i = 0; i < entries; ++i) {
            const acpi_sdt_header_t* hdr = (const acpi_sdt_header_t*)acpi_phys_ptr(table[i]);
            if (memcmp(hdr->signature, sig, 4) == 0) return hdr;
        }
    }

    return NULL;
}

static bool acpi_discover_tables(void) {
    g_rsdp = acpi_find_rsdp();
    if (!g_rsdp) return false;

    g_fadt = (const acpi_fadt_t*)acpi_find_table_by_sig("FACP");
    if (!g_fadt) g_fadt = (const acpi_fadt_t*)acpi_find_table_by_sig("FADT");
    if (!g_fadt) return false;

    /* The x_dsdt (and x_firmware_ctrl) fields were added in ACPI 2.0 and
     * sit at the very end of the FADT. A firmware that only implements
     * ACPI 1.0 emits a shorter FADT that simply doesn't have these bytes
     * at all -- reading g_fadt->x_dsdt on such a table is an out-of-bounds
     * read into whatever memory happens to follow (typically the next
     * ACPI table's header), which is exactly why it came back looking
     * like garbage/ASCII noise instead of a real address. Only trust
     * x_dsdt if the table is actually long enough to contain it; fall
     * back to the 32-bit `dsdt` field (always present) otherwise. */
    uint32_t dsdt_field_end = (uint32_t)(offsetof(acpi_fadt_t, x_dsdt) + sizeof(g_fadt->x_dsdt));
    bool has_x_dsdt = (g_fadt->header.length >= dsdt_field_end) && (g_fadt->x_dsdt != 0);
    if (has_x_dsdt) g_dsdt = (const acpi_sdt_header_t*)acpi_phys_ptr(g_fadt->x_dsdt);
    else if (g_fadt->dsdt) g_dsdt = (const acpi_sdt_header_t*)acpi_phys_ptr(g_fadt->dsdt);
    if (!g_dsdt) return false;

    if (memcmp(g_dsdt->signature, "DSDT", 4) != 0) return false;
    g_dsdt_aml = ((const uint8_t*)g_dsdt) + sizeof(acpi_sdt_header_t);
    return true;
}

/* ------------------------------------------------------------
 * SMBus controller / smart battery
 * ------------------------------------------------------------ */

static pci_dev_t* acpi_find_smbus_controller(void) {
    pci_dev_t* dev = pci_get_first_device();
    while (dev) {
        if (dev->class_code == 0x0C && dev->subclass == 0x05) {
            return dev;
        }
        dev = pci_get_next_device(dev);
    }
    return NULL;
}

static bool acpi_pci_bar_is_io(uint64_t bar) {
    return (bar & 0x1u) != 0;
}

static uint64_t acpi_pci_bar_addr(uint64_t bar) {
    if (acpi_pci_bar_is_io(bar)) return bar & ~0x3u;
    return bar & ~0xFu;
}

static bool acpi_smbus_wait_ready(uint64_t base) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint8_t st = inb((uint16_t)(base + SMBUS_HOST_STS));
        if ((st & SMBUS_STS_HOST_BUSY) == 0) return true;
    }
    return false;
}

static void acpi_smbus_clear_status(uint64_t base) {
    outb((uint16_t)(base + SMBUS_HOST_STS), 0xFFu);
}

static bool acpi_smbus_read_word(uint64_t base, uint8_t addr, uint8_t cmd, uint16_t* out) {
    if (!out) return false;
    if (!acpi_smbus_wait_ready(base)) return false;
    acpi_smbus_clear_status(base);

    outb((uint16_t)(base + SMBUS_HOST_CMD), cmd);
    outb((uint16_t)(base + SMBUS_XMIT_SLVA), (uint8_t)((addr << 1) | 1u));
    outb((uint16_t)(base + SMBUS_HOST_CNT), (uint8_t)(SMBUS_CNT_START | SMBUS_CNT_PROTO_WORD));

    for (uint32_t i = 0; i < 100000u; ++i) {
        uint8_t st = inb((uint16_t)(base + SMBUS_HOST_STS));
        if (st & (SMBUS_STS_DEV_ERR | SMBUS_STS_BUS_ERR | SMBUS_STS_FAILED)) {
            acpi_smbus_clear_status(base);
            return false;
        }
        if (st & SMBUS_STS_INTR) {
            uint8_t lo = inb((uint16_t)(base + SMBUS_HST_D0));
            uint8_t hi = inb((uint16_t)(base + SMBUS_HST_D1));
            *out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
            acpi_smbus_clear_status(base);
            return true;
        }
    }
    acpi_smbus_clear_status(base);
    return false;
}

static bool acpi_smbus_read_byte(uint64_t base, uint8_t addr, uint8_t cmd, uint8_t* out) {
    uint16_t word = 0;
    if (!acpi_smbus_read_word(base, addr, cmd, &word)) return false;
    if (out) *out = (uint8_t)(word & 0xFFu);
    return true;
}

static void acpi_detect_smbus(void) {
    pci_dev_t* dev = acpi_find_smbus_controller();
    if (!dev) return;

    for (int i = 0; i < 6; ++i) {
        uint64_t bar = dev->bar[i];
        if (bar == 0) continue;
        if (acpi_pci_bar_is_io(bar)) {
            g_smbus_base = acpi_pci_bar_addr(bar);
            g_smbus_ready = true;
            acpi_strncpy0(g_source_label, "SMBus", sizeof(g_source_label));
            return;
        }
    }
}

static bool acpi_smart_battery_probe(void) {
    if (!g_smbus_ready || g_smbus_base == 0) return false;

    uint16_t remaining = 0, full = 0, voltage = 0, current_raw = 0, status = 0;
    uint8_t rel_soc = 0;
    bool ok = false;

    ok = acpi_smbus_read_word(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, ACPI_SMBUS_CMD_REMAINING_CAPACITY, &remaining);
    if (!ok) remaining = 0;
    ok = acpi_smbus_read_word(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, ACPI_SMBUS_CMD_FULL_CHARGE_CAPACITY, &full);
    if (!ok) full = 0;
    ok = acpi_smbus_read_word(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, ACPI_SMBUS_CMD_VOLTAGE, &voltage);
    if (!ok) voltage = 0;
    ok = acpi_smbus_read_word(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, ACPI_SMBUS_CMD_CURRENT, &current_raw);
    if (!ok) current_raw = 0;
    ok = acpi_smbus_read_word(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, ACPI_SMBUS_CMD_STATUS, &status);
    if (!ok) status = 0;
    ok = acpi_smbus_read_byte(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, ACPI_SMBUS_CMD_RELATIVE_SOC, &rel_soc);
    if (!ok) rel_soc = 0;

    uint64_t percent = 0;
    if (remaining && full) percent = (remaining * 100u) / full;
    else if (rel_soc) percent = rel_soc;
    else percent = g_battery_percent;

    if (percent > 100u) percent = 100u;
    g_battery_percent = percent;

    int16_t signed_current = (int16_t)current_raw;
    uint64_t current_abs = (signed_current < 0)
        ? (uint64_t)(-(int64_t)signed_current)
        : (uint64_t)signed_current;
    g_battery_charging = (signed_current > 0) || ((status & 0x0010u) != 0);
    if (g_battery_charging) g_battery_minutes = 0;
    else if (signed_current < 0 && remaining && current_abs) g_battery_minutes = (uint64_t)((remaining * 60u) / current_abs);
    else if (full && signed_current < 0 && current_abs) g_battery_minutes = (uint64_t)((full * 60u) / current_abs);

    (void)voltage;
    g_have_battery = true;
    acpi_strncpy0(g_source_label, "SMBus", sizeof(g_source_label));
    return true;
}

/* ------------------------------------------------------------
 * EC transport
 * ------------------------------------------------------------ */

static bool acpi_ec_wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((inb(g_ec_cmd_port) & EC_STATUS_IBF) == 0) return true;
    }
    return false;
}

static bool acpi_ec_wait_output_full(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((inb(g_ec_cmd_port) & EC_STATUS_OBF) != 0) return true;
    }
    return false;
}

static bool acpi_ec_read(uint8_t reg, uint8_t* out) {
    if (!out) return false;
    if (!acpi_ec_wait_input_clear()) return false;
    outb(g_ec_cmd_port, EC_CMD_READ);
    if (!acpi_ec_wait_input_clear()) return false;
    outb(g_ec_data_port, reg);
    if (!acpi_ec_wait_output_full()) return false;
    *out = inb(g_ec_data_port);
    return true;
}

static bool acpi_ec_write(uint8_t reg, uint8_t value) {
    if (!acpi_ec_wait_input_clear()) return false;
    outb(g_ec_cmd_port, EC_CMD_WRITE);
    if (!acpi_ec_wait_input_clear()) return false;
    outb(g_ec_data_port, reg);
    if (!acpi_ec_wait_input_clear()) return false;
    outb(g_ec_data_port, value);
    return true;
}

static void acpi_detect_ec(void) {
    /* Portable fallback ports. Many BIOSes expose EC at 0x66/0x62. */
    uint8_t st = inb(g_ec_cmd_port);
    if (st == 0xFFu) return;
    g_ec_ready = true;
    acpi_strncpy0(g_source_label, "EC", sizeof(g_source_label));
}

/* ------------------------------------------------------------
 * AML parsing and evaluation (compact subset)
 * ------------------------------------------------------------ */

static uint64_t aml_read_pkglen(acpi_cursor_t* c) { return acpi_cursor_read_pkglen(c); }

static uint64_t aml_read_integer(acpi_cursor_t* c, uint8_t prefix) {
    if (!c || c->ptr >= c->end) return 0;
    switch (prefix) {
        case AML_ZERO_OP: return 0;
        case AML_ONE_OP: return 1;
        case AML_BYTE_PREFIX:
            return (c->ptr < c->end) ? *c->ptr++ : 0;
        case AML_WORD_PREFIX: {
            if (c->ptr + 2 > c->end) return 0;
            uint64_t v = (uint64_t)c->ptr[0] | ((uint64_t)c->ptr[1] << 8);
            c->ptr += 2;
            return v;
        }
        case AML_DWORD_PREFIX: {
            if (c->ptr + 4 > c->end) return 0;
            uint64_t v = (uint64_t)c->ptr[0] | ((uint64_t)c->ptr[1] << 8) | ((uint64_t)c->ptr[2] << 16) | ((uint64_t)c->ptr[3] << 24);
            c->ptr += 4;
            return v;
        }
        case AML_QWORD_PREFIX: {
            if (c->ptr + 8 > c->end) return 0;
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) v |= ((uint64_t)c->ptr[i]) << (8 * i);
            c->ptr += 8;
            return v;
        }
        default:
            return 0;
    }
}

static bool aml_parse_nameseg(acpi_cursor_t* c, char out[5]) {
    if (!c || !out || c->ptr + 4 > c->end) return false;
    out[0] = (char)c->ptr[0];
    out[1] = (char)c->ptr[1];
    out[2] = (char)c->ptr[2];
    out[3] = (char)c->ptr[3];
    out[4] = '\0';
    c->ptr += 4;
    return true;
}

static bool aml_parse_namepath(acpi_cursor_t* c, char* out, size_t out_size, const char* scope) {
    if (!c || !out || out_size == 0 || c->ptr >= c->end) return false;

    char tmp[ACPI_MAX_PATH];
    tmp[0] = '\0';

    uint8_t b = *c->ptr++;
    if (b == '\\') {
        acpi_strncpy0(tmp, "\\", sizeof(tmp));
        if (c->ptr < c->end) {
            uint8_t next = *c->ptr;
            if (next == AML_DWORD_PREFIX) {
                c->ptr++;
                char a[5], c2[5], d[5], e[5];
                if (!aml_parse_nameseg(c, a) || !aml_parse_nameseg(c, c2) || !aml_parse_nameseg(c, d) || !aml_parse_nameseg(c, e)) return false;
                snprintf(tmp, sizeof(tmp), "\\%s.%s.%s.%s", a, c2, d, e);
                acpi_strncpy0(out, tmp, out_size);
                return true;
            }
        }
    } else if (b == '^') {
        char scope_buf[ACPI_MAX_PATH];
        acpi_strncpy0(scope_buf, scope ? scope : "\\", sizeof(scope_buf));
        acpi_path_parent(scope_buf);
        if (!aml_parse_namepath(c, out, out_size, scope_buf)) return false;
        return true;
    } else if (b == AML_DWORD_PREFIX) {
        char a[5], b2[5], c2[5], d[5];
        if (!aml_parse_nameseg(c, a) || !aml_parse_nameseg(c, b2) || !aml_parse_nameseg(c, c2) || !aml_parse_nameseg(c, d)) return false;
        snprintf(tmp, sizeof(tmp), "%s.%s.%s.%s", a, b2, c2, d);
        acpi_strncpy0(out, tmp, out_size);
        return true;
    } else if (b == AML_VAR_PACKAGE_OP) {
        /* Not actually a namepath prefix here; fallback. */
        return false;
    } else {
        c->ptr--;
        char seg[5];
        if (!aml_parse_nameseg(c, seg)) return false;
        if (scope && scope[0] && strcmp(scope, "\\") != 0) {
            snprintf(tmp, sizeof(tmp), "%s.%s", scope, seg);
        } else {
            snprintf(tmp, sizeof(tmp), "\\%s", seg);
        }
        acpi_strncpy0(out, tmp, out_size);
        return true;
    }

    /* Root or relative name with one or more segments. */
    if (c->ptr < c->end) {
        uint8_t next = *c->ptr;
        if (next == AML_DWORD_PREFIX) {
            c->ptr++;
            char a[5], b2[5], c2[5], d[5];
            if (!aml_parse_nameseg(c, a) || !aml_parse_nameseg(c, b2) || !aml_parse_nameseg(c, c2) || !aml_parse_nameseg(c, d)) return false;
            snprintf(tmp, sizeof(tmp), "\\%s.%s.%s.%s", a, b2, c2, d);
            acpi_strncpy0(out, tmp, out_size);
            return true;
        }
    }

    acpi_strncpy0(out, tmp, out_size);
    return true;
}

static acpi_value_t aml_make_int(uint64_t v) {
    acpi_value_t out;
    memset(&out, 0, sizeof(out));
    out.kind = ACPI_VAL_INT;
    out.integer = v;
    return out;
}

static acpi_value_t aml_make_none(void) {
    acpi_value_t out;
    memset(&out, 0, sizeof(out));
    out.kind = ACPI_VAL_NONE;
    return out;
}

static acpi_value_t aml_make_string(const char* s) {
    acpi_value_t out;
    memset(&out, 0, sizeof(out));
    out.kind = ACPI_VAL_STRING;
    if (s) acpi_strncpy0(out.string, s, sizeof(out.string));
    return out;
}

static acpi_value_t aml_make_buffer(const uint8_t* data, uint64_t len) {
    acpi_value_t out;
    memset(&out, 0, sizeof(out));
    out.kind = ACPI_VAL_BUFFER;
    if (data && len) {
        if (len > sizeof(out.buffer)) len = sizeof(out.buffer);
        memcpy(out.buffer, data, len);
        out.buffer_len = len;
    }
    return out;
}

static acpi_value_t aml_make_reference(const void* ref) {
    acpi_value_t out;
    memset(&out, 0, sizeof(out));
    out.kind = ACPI_VAL_REFERENCE;
    out.reference = ref;
    return out;
}

static acpi_package_elem_t aml_value_to_pkg_elem(const acpi_value_t* v) {
    acpi_package_elem_t e;
    memset(&e, 0, sizeof(e));
    if (!v) return e;
    e.kind = v->kind;
    e.integer = v->integer;
    acpi_strncpy0(e.string, v->string, sizeof(e.string));
    if (v->buffer_len > sizeof(e.buffer)) e.buffer_len = sizeof(e.buffer);
    else e.buffer_len = v->buffer_len;
    if (e.buffer_len) memcpy(e.buffer, v->buffer, e.buffer_len);
    return e;
}

static acpi_value_t aml_pkg_elem_to_value(const acpi_package_elem_t* e) {
    acpi_value_t v;
    memset(&v, 0, sizeof(v));
    if (!e) return v;
    v.kind = e->kind;
    v.integer = e->integer;
    acpi_strncpy0(v.string, e->string, sizeof(v.string));
    v.buffer_len = e->buffer_len;
    if (v.buffer_len) memcpy(v.buffer, e->buffer, v.buffer_len);
    return v;
}

static bool aml_value_is_true(const acpi_value_t* v) {
    if (!v) return false;
    switch (v->kind) {
        case ACPI_VAL_INT: return v->integer != 0;
        case ACPI_VAL_STRING: return v->string[0] != '\0';
        case ACPI_VAL_BUFFER: return v->buffer_len != 0;
        case ACPI_VAL_PACKAGE: return v->package.count != 0;
        case ACPI_VAL_REFERENCE: return v->reference != NULL;
        default: return false;
    }
}

static uint64_t aml_value_as_int(const acpi_value_t* v) {
    if (!v) return 0;
    switch (v->kind) {
        case ACPI_VAL_INT: return v->integer;
        case ACPI_VAL_STRING: return (uint64_t)(uintptr_t)v->string;
        case ACPI_VAL_BUFFER: {
            uint64_t x = 0;
            uint64_t n = v->buffer_len < 8 ? v->buffer_len : 8;
            for (uint64_t i = 0; i < n; ++i) x |= ((uint64_t)v->buffer[i]) << (8u * i);
            return x;
        }
        default: return 0;
    }
}

static acpi_value_t aml_ref_deref(acpi_value_t ref);
static bool aml_store_to_ref(acpi_value_t ref, acpi_value_t value);

static acpi_object_t* aml_resolve_object(const char* scope, const char* path) {
    if (!path || !path[0]) return NULL;
    if (path[0] == '\\') return acpi_find_object_exact(path);
    return acpi_find_object_relative(scope, path);
}

static acpi_value_t aml_object_to_value(const char* scope, const char* name) {
    acpi_object_t* obj = aml_resolve_object(scope, name);
    if (!obj) return aml_make_none();

    switch (obj->kind) {
        case ACPI_OBJ_INTEGER: return aml_make_int(obj->u.integer);
        case ACPI_OBJ_STRING: return aml_make_string(obj->u.string);
        case ACPI_OBJ_BUFFER: return aml_make_buffer(obj->u.buffer.bytes, obj->u.buffer.length);
        case ACPI_OBJ_METHOD: return aml_make_reference(obj);
        case ACPI_OBJ_REGION: return aml_make_reference(obj);
        case ACPI_OBJ_FIELD: return aml_make_reference(obj);
        default: return aml_make_none();
    }
}

static uint64_t acpi_region_read(const acpi_object_t* region, uint64_t bit_offset, uint64_t bit_length);
static bool acpi_region_write(const acpi_object_t* region, uint64_t bit_offset, uint64_t bit_length, uint64_t value);

static acpi_value_t aml_ref_deref(acpi_value_t ref) {
    if (ref.kind != ACPI_VAL_REFERENCE || !ref.reference) return aml_make_none();
    const acpi_object_t* obj = (const acpi_object_t*)ref.reference;
    switch (obj->kind) {
        case ACPI_OBJ_INTEGER: return aml_make_int(obj->u.integer);
        case ACPI_OBJ_STRING: return aml_make_string(obj->u.string);
        case ACPI_OBJ_BUFFER: return aml_make_buffer(obj->u.buffer.bytes, obj->u.buffer.length);
        case ACPI_OBJ_REGION: return aml_make_int(obj->u.region.base);
        case ACPI_OBJ_FIELD: {
            const acpi_object_t* region = acpi_find_object_exact(obj->u.field.region_path);
            if (!region) return aml_make_none();
            uint64_t val = acpi_region_read(region, obj->u.field.bit_offset, obj->u.field.bit_length);
            return aml_make_int(val);
        }
        case ACPI_OBJ_METHOD: return aml_make_reference(obj);
        default: return aml_make_none();
    }
}

static bool aml_store_to_ref(acpi_value_t ref, acpi_value_t value) {
    if (ref.kind != ACPI_VAL_REFERENCE || !ref.reference) return false;
    acpi_object_t* obj = (acpi_object_t*)ref.reference;
    switch (obj->kind) {
        case ACPI_OBJ_INTEGER:
            obj->u.integer = aml_value_as_int(&value);
            return true;
        case ACPI_OBJ_STRING:
            if (value.kind == ACPI_VAL_STRING) {
                acpi_strncpy0(obj->u.string, value.string, sizeof(obj->u.string));
                return true;
            }
            return false;
        case ACPI_OBJ_BUFFER:
            if (value.kind == ACPI_VAL_BUFFER) {
                uint64_t n = value.buffer_len < sizeof(obj->u.buffer.bytes) ? value.buffer_len : sizeof(obj->u.buffer.bytes);
                memcpy(obj->u.buffer.bytes, value.buffer, n);
                obj->u.buffer.length = n;
                return true;
            }
            return false;
        case ACPI_OBJ_FIELD: {
            const acpi_object_t* region = acpi_find_object_exact(obj->u.field.region_path);
            if (!region) return false;
            return acpi_region_write(region, obj->u.field.bit_offset, obj->u.field.bit_length, aml_value_as_int(&value));
        }
        default:
            return false;
    }
}

static uint64_t aml_binary_op(uint8_t op, uint64_t a, uint64_t b) {
    switch (op) {
        case AML_ADD_OP: return a + b;
        case AML_CONCAT_OP: return (a << 32) ^ b;
        case AML_SHIFT_LEFT_OP: return a << (b & 63u);
        case AML_SHIFT_RIGHT_OP: return a >> (b & 63u);
        case AML_AND_OP: return a & b;
        case AML_NAND_OP: return ~(a & b);
        case AML_OR_OP: return a | b;
        case AML_NOR_OP: return ~(a | b);
        case AML_XOR_OP: return a ^ b;
        case AML_LEQUAL_OP: return (a == b);
        case AML_LGREATER_OP: return (a > b);
        case AML_LLESS_OP: return (a < b);
        case AML_LAND_OP: return (a != 0 && b != 0);
        case AML_LOR_OP: return (a != 0 || b != 0);
        default: return 0;
    }
}

static acpi_value_t aml_eval_term(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok);

static bool aml_copy_value(acpi_value_t* dst, const acpi_value_t* src) {
    if (!dst || !src) return false;
    memcpy(dst, src, sizeof(*dst));
    return true;
}

static acpi_value_t aml_eval_arg_local(uint8_t op, acpi_method_env_t* env) {
    if (!env) return aml_make_none();
    if (op >= 0x68 && op <= 0x6E) return env->args[op - 0x68];
    if (op >= 0x60 && op <= 0x67) return env->locals[op - 0x60];
    return aml_make_none();
}

static void aml_assign_arg_local(uint8_t op, acpi_method_env_t* env, acpi_value_t v) {
    if (!env) return;
    if (op >= 0x68 && op <= 0x6E) env->args[op - 0x68] = v;
    else if (op >= 0x60 && op <= 0x67) env->locals[op - 0x60] = v;
}

static acpi_value_t aml_parse_object_or_ref(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    if (!c || c->ptr >= c->end) {
        if (ok) *ok = false;
        return aml_make_none();
    }

    uint8_t op = *c->ptr++;
    if (op >= 0x60 && op <= 0x6E) return aml_eval_arg_local(op, env);

    switch (op) {
        case AML_ZERO_OP:
        case AML_ONE_OP:
        case AML_BYTE_PREFIX:
        case AML_WORD_PREFIX:
        case AML_DWORD_PREFIX:
        case AML_QWORD_PREFIX:
            return aml_make_int(aml_read_integer(c, op));
        case AML_STRING_PREFIX: {
            char s[64];
            size_t i = 0;
            while (c->ptr < c->end && *c->ptr != '\0' && i + 1 < sizeof(s)) s[i++] = (char)*c->ptr++;
            s[i] = '\0';
            if (c->ptr < c->end && *c->ptr == '\0') ++c->ptr;
            return aml_make_string(s);
        }
        case AML_REF_OF_OP: {
            acpi_value_t inner = aml_parse_object_or_ref(c, scope, env, ok);
            return aml_make_reference(inner.reference ? inner.reference : NULL);
        }
        case AML_DEREF_OF_OP: {
            acpi_value_t inner = aml_parse_object_or_ref(c, scope, env, ok);
            return aml_ref_deref(inner);
        }
        case AML_INDEX_OP: {
            acpi_value_t src = aml_parse_object_or_ref(c, scope, env, ok);
            acpi_value_t idx = aml_parse_object_or_ref(c, scope, env, ok);
            (void)aml_parse_object_or_ref(c, scope, env, ok); /* target (ignored) */
            uint64_t n = aml_value_as_int(&idx);
            if (src.kind == ACPI_VAL_PACKAGE && n < src.package.count) return aml_pkg_elem_to_value(&src.package.elems[n]);
            if (src.kind == ACPI_VAL_BUFFER && n < src.buffer_len) return aml_make_int(src.buffer[n]);
            return aml_make_none();
        }
        case AML_STORE_OP: {
            acpi_value_t src = aml_parse_object_or_ref(c, scope, env, ok);
            acpi_value_t dst = aml_parse_object_or_ref(c, scope, env, ok);
            if (dst.kind == ACPI_VAL_REFERENCE) aml_store_to_ref(dst, src);
            return src;
        }
        case AML_ADD_OP: case AML_CONCAT_OP: case AML_SHIFT_LEFT_OP: case AML_SHIFT_RIGHT_OP:
        case AML_AND_OP: case AML_NAND_OP: case AML_OR_OP: case AML_NOR_OP: case AML_XOR_OP:
        case AML_LEQUAL_OP: case AML_LGREATER_OP: case AML_LLESS_OP: case AML_LAND_OP: case AML_LOR_OP: {
            acpi_value_t a = aml_parse_object_or_ref(c, scope, env, ok);
            acpi_value_t b = aml_parse_object_or_ref(c, scope, env, ok);
            return aml_make_int(aml_binary_op(op, aml_value_as_int(&a), aml_value_as_int(&b)));
        }
        case AML_NOT_OP: {
            acpi_value_t a = aml_parse_object_or_ref(c, scope, env, ok);
            return aml_make_int(~aml_value_as_int(&a));
        }
        case AML_LNOT_OP: {
            acpi_value_t a = aml_parse_object_or_ref(c, scope, env, ok);
            return aml_make_int(!aml_value_is_true(&a));
        }
        case AML_RETURN_OP: {
            acpi_value_t v = aml_parse_object_or_ref(c, scope, env, ok);
            return v;
        }
        default:
            break;
    }

    /* Names / methods / fields. */
    c->ptr--; /* rewind so we can parse namepath. */
    char name[ACPI_MAX_PATH];
    if (!aml_parse_namepath(c, name, sizeof(name), scope)) {
        if (ok) *ok = false;
        return aml_make_none();
    }

    acpi_object_t* obj = aml_resolve_object(scope, name);
    if (!obj) return aml_make_none();

    switch (obj->kind) {
        case ACPI_OBJ_INTEGER: return aml_make_int(obj->u.integer);
        case ACPI_OBJ_STRING: return aml_make_string(obj->u.string);
        case ACPI_OBJ_BUFFER: return aml_make_buffer(obj->u.buffer.bytes, obj->u.buffer.length);
        case ACPI_OBJ_REGION: return aml_make_reference(obj);
        case ACPI_OBJ_FIELD: return aml_make_reference(obj);
        case ACPI_OBJ_METHOD: return aml_make_reference(obj);
        default: return aml_make_none();
    }
}

static acpi_value_t aml_eval_package(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    acpi_value_t out = aml_make_none();
    out.kind = ACPI_VAL_PACKAGE;
    if (!c || c->ptr >= c->end) {
        if (ok) *ok = false;
        return out;
    }
    uint8_t count = *c->ptr++;
    if (count > ACPI_MAX_PKG_ELEMS) count = ACPI_MAX_PKG_ELEMS;
    out.package.count = count;
    for (uint64_t i = 0; i < count; ++i) {
        acpi_value_t item = aml_parse_object_or_ref(c, scope, env, ok);
        out.package.elems[i] = aml_value_to_pkg_elem(&item);
    }
    return out;
}

static acpi_value_t aml_eval_buffer(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    (void)scope; (void)env;
    acpi_value_t out = aml_make_none();
    out.kind = ACPI_VAL_BUFFER;
    acpi_value_t sizev = aml_parse_object_or_ref(c, scope, env, ok);
    uint64_t size = aml_value_as_int(&sizev);
    if (c->ptr >= c->end) {
        if (ok) *ok = false;
        return out;
    }
    uint64_t consumed = 0;
    uint8_t tmp[64];
    if (size > sizeof(tmp)) size = sizeof(tmp);
    /* AML buffer contents are prefixed with a pkglen block. */
    uint64_t pkglen = aml_read_pkglen(c);
    const uint8_t* limit = c->ptr + pkglen;
    while (c->ptr < limit && consumed < size && consumed < sizeof(tmp)) {
        tmp[consumed++] = *c->ptr++;
    }
    memcpy(out.buffer, tmp, consumed);
    out.buffer_len = consumed;
    return out;
}

static acpi_value_t aml_eval_block(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    acpi_value_t last = aml_make_none();
    while (c && c->ptr < c->end) {
        const uint8_t* save = c->ptr;
        uint8_t op = *c->ptr;
        if (op == AML_ELSE_OP || op == AML_BREAK_OP) break;
        last = aml_eval_term(c, scope, env, ok);
        if (!ok || !*ok) return last;
        if (c->ptr == save) {
            /* Safety: ensure forward progress. */
            ++c->ptr;
        }
        if (last.kind == ACPI_VAL_REFERENCE && last.reference == NULL) {
            /* harmless */
        }
    }
    return last;
}

static acpi_value_t aml_eval_if(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    acpi_value_t pred = aml_parse_object_or_ref(c, scope, env, ok);
    uint64_t body_len = aml_read_pkglen(c);
    const uint8_t* body_end = c->ptr + body_len;
    acpi_cursor_t body = { c->ptr, body_end };
    acpi_value_t result = aml_make_none();
    if (aml_value_is_true(&pred)) {
        result = aml_eval_block(&body, scope, env, ok);
    }
    c->ptr = body_end;
    if (c->ptr < c->end && *c->ptr == AML_ELSE_OP) {
        ++c->ptr;
        uint64_t else_len = aml_read_pkglen(c);
        const uint8_t* else_end = c->ptr + else_len;
        if (!aml_value_is_true(&pred)) {
            acpi_cursor_t els = { c->ptr, else_end };
            result = aml_eval_block(&els, scope, env, ok);
        }
        c->ptr = else_end;
    }
    return result;
}

static acpi_value_t aml_eval_while(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    uint64_t body_len = aml_read_pkglen(c);
    const uint8_t* body_end = c->ptr + body_len;
    const uint8_t* cond_start = c->ptr;
    acpi_value_t result = aml_make_none();
    for (uint32_t iter = 0; iter < 64u; ++iter) {
        acpi_cursor_t cond = { cond_start, body_end };
        acpi_value_t pred = aml_parse_object_or_ref(&cond, scope, env, ok);
        if (!aml_value_is_true(&pred)) break;
        acpi_cursor_t body = { cond.ptr, body_end };
        result = aml_eval_block(&body, scope, env, ok);
        if (!ok || !*ok) break;
        if (body.ptr >= body_end) break;
    }
    c->ptr = body_end;
    return result;
}

static acpi_value_t aml_eval_term(acpi_cursor_t* c, const char* scope, acpi_method_env_t* env, bool* ok) {
    if (!c || c->ptr >= c->end) {
        if (ok) *ok = false;
        return aml_make_none();
    }

    uint8_t op = *c->ptr++;

    if (op >= 0x60 && op <= 0x6E) return aml_eval_arg_local(op, env);

    switch (op) {
        case AML_ZERO_OP:
        case AML_ONE_OP:
        case AML_BYTE_PREFIX:
        case AML_WORD_PREFIX:
        case AML_DWORD_PREFIX:
        case AML_QWORD_PREFIX:
            return aml_make_int(aml_read_integer(c, op));
        case AML_STRING_PREFIX: {
            char s[64];
            size_t i = 0;
            while (c->ptr < c->end && *c->ptr != '\0' && i + 1 < sizeof(s)) s[i++] = (char)*c->ptr++;
            s[i] = '\0';
            if (c->ptr < c->end && *c->ptr == '\0') ++c->ptr;
            return aml_make_string(s);
        }
        case AML_PACKAGE_OP:
            (void)aml_read_pkglen(c);
            return aml_eval_package(c, scope, env, ok);
        case AML_BUFFER_OP:
            (void)aml_read_pkglen(c);
            return aml_eval_buffer(c, scope, env, ok);
        case AML_RETURN_OP:
            return aml_parse_object_or_ref(c, scope, env, ok);
        case AML_IF_OP:
            return aml_eval_if(c, scope, env, ok);
        case AML_WHILE_OP:
            return aml_eval_while(c, scope, env, ok);
        case AML_STORE_OP:
        case AML_ADD_OP:
        case AML_CONCAT_OP:
        case AML_SHIFT_LEFT_OP:
        case AML_SHIFT_RIGHT_OP:
        case AML_AND_OP:
        case AML_NAND_OP:
        case AML_OR_OP:
        case AML_NOR_OP:
        case AML_XOR_OP:
        case AML_NOT_OP:
        case AML_LAND_OP:
        case AML_LOR_OP:
        case AML_LNOT_OP:
        case AML_LEQUAL_OP:
        case AML_LGREATER_OP:
        case AML_LLESS_OP:
        case AML_REF_OF_OP:
        case AML_DEREF_OF_OP:
        case AML_INDEX_OP:
            c->ptr--; /* parse as generic object/ref */
            return aml_parse_object_or_ref(c, scope, env, ok);
        default:
            break;
    }

    c->ptr--; /* name/object path */
    char name[ACPI_MAX_PATH];
    if (!aml_parse_namepath(c, name, sizeof(name), scope)) {
        if (ok) *ok = false;
        return aml_make_none();
    }

    acpi_object_t* obj = aml_resolve_object(scope, name);
    if (!obj) return aml_make_none();

    switch (obj->kind) {
        case ACPI_OBJ_INTEGER: return aml_make_int(obj->u.integer);
        case ACPI_OBJ_STRING: return aml_make_string(obj->u.string);
        case ACPI_OBJ_BUFFER: return aml_make_buffer(obj->u.buffer.bytes, obj->u.buffer.length);
        case ACPI_OBJ_METHOD: return aml_make_reference(obj);
        case ACPI_OBJ_REGION: return aml_make_reference(obj);
        case ACPI_OBJ_FIELD: return aml_make_reference(obj);
        default: return aml_make_none();
    }
}

static acpi_value_t aml_call_method(acpi_object_t* method, const acpi_value_t* args, uint8_t arg_count, const char* scope, bool* ok) {
    acpi_method_env_t env;
    memset(&env, 0, sizeof(env));
    for (uint8_t i = 0; i < arg_count && i < ACPI_MAX_METHOD_ARGS; ++i) env.args[i] = args[i];

    acpi_cursor_t c = { method->u.method.body, method->u.method.body + method->u.method.length };
    acpi_value_t ret = aml_make_none();
    while (c.ptr < c.end) {
        const uint8_t* save = c.ptr;
        ret = aml_eval_term(&c, scope, &env, ok);
        if (!ok || !*ok) break;
        if (c.ptr == save) ++c.ptr;
        if (ret.kind != ACPI_VAL_NONE && (save < c.end && save[0] == AML_RETURN_OP)) {
            break;
        }
        if (c.ptr >= c.end) break;
    }
    return ret;
}

/* ------------------------------------------------------------
 * AML namespace scanner
 * ------------------------------------------------------------ */

static bool aml_scan_namestring(acpi_cursor_t* c, const char* scope, char* out, size_t out_size) {
    return aml_parse_namepath(c, out, out_size, scope);
}

static void aml_register_simple_object(const char* scope, const char* name, acpi_obj_kind_t kind, const void* value, uint64_t len) {
    char path[ACPI_MAX_PATH];
    acpi_make_path(path, sizeof(path), scope, name);
    acpi_object_t* obj = acpi_new_object(path, name, kind);
    if (!obj) return;
    switch (kind) {
        case ACPI_OBJ_INTEGER:
            obj->u.integer = (uint64_t)(uintptr_t)value;
            break;
        case ACPI_OBJ_STRING:
            acpi_strncpy0(obj->u.string, (const char*)value, sizeof(obj->u.string));
            break;
        case ACPI_OBJ_BUFFER:
            if (value && len) {
                if (len > sizeof(obj->u.buffer.bytes)) len = sizeof(obj->u.buffer.bytes);
                memcpy(obj->u.buffer.bytes, value, len);
                obj->u.buffer.length = len;
            }
            break;
        default:
            break;
    }
}

static void aml_scan_field_list(acpi_cursor_t* c, const char* scope, const char* region_path) {
    uint64_t bit_offset = 0;
    while (c->ptr < c->end) {
        if (*c->ptr == 0x00) {
            ++c->ptr;
            uint64_t reserved_bits = aml_read_pkglen(c);
            bit_offset += reserved_bits;
            continue;
        }
        if (*c->ptr == 0x01 || *c->ptr == 0x02 || *c->ptr == 0x03) {
            /* Access/Connect/ExtendedAccess fields – best-effort skip. */
            ++c->ptr;
            (void)aml_read_pkglen(c);
            continue;
        }

        char field_name[5];
        if (!aml_parse_nameseg(c, field_name)) break;
        uint64_t field_bits = aml_read_pkglen(c);
        char path[ACPI_MAX_PATH];
        acpi_make_path(path, sizeof(path), scope, field_name);
        acpi_object_t* obj = acpi_new_object(path, field_name, ACPI_OBJ_FIELD);
        if (obj) {
            acpi_strncpy0(obj->u.field.region_path, region_path, sizeof(obj->u.field.region_path));
            obj->u.field.bit_offset = bit_offset;
            obj->u.field.bit_length = field_bits;
            obj->u.field.access_size = 1;
        }
        bit_offset += field_bits;
    }
}

static void aml_scan_term_list(acpi_cursor_t* c, const char* scope);

static void aml_scan_container(acpi_cursor_t* c, const char* parent_scope, uint8_t op) {
    char name[ACPI_MAX_PATH];
    if (!aml_scan_namestring(c, parent_scope, name, sizeof(name))) return;
    uint64_t len = aml_read_pkglen(c);
    const uint8_t* end = c->ptr + len;

    char scope[ACPI_MAX_PATH];
    acpi_strncpy0(scope, name, sizeof(scope));
    if (op == AML_METHOD_OP) {
        if (c->ptr < c->end) {
            uint8_t flags = *c->ptr++;
            acpi_object_t* method = acpi_new_object(scope, name, ACPI_OBJ_METHOD);
            if (method) {
                method->u.method.body = c->ptr;
                method->u.method.length = (uint64_t)(end - c->ptr);
                method->u.method.arg_count = flags & 0x07u;
            }
        }
        c->ptr = end;
        return;
    }

    if (op == AML_OPREGION_OP) {
        if (c->ptr >= end) { c->ptr = end; return; }
        uint8_t space = *c->ptr++;
        uint64_t base = aml_parse_object_or_ref(c, parent_scope, NULL, NULL).integer;
        uint64_t size = aml_parse_object_or_ref(c, parent_scope, NULL, NULL).integer;
        acpi_object_t* region = acpi_new_object(scope, name, ACPI_OBJ_REGION);
        if (region) {
            region->u.region.space_id = space;
            region->u.region.base = base;
            region->u.region.length = size;
        }
        c->ptr = end;
        return;
    }

    if (op == AML_FIELD_OP || op == AML_INDEXFIELD_OP || op == AML_BANKFIELD_OP) {
        char region_name[ACPI_MAX_PATH];
        if (!aml_scan_namestring(c, parent_scope, region_name, sizeof(region_name))) {
            c->ptr = end; return;
        }
        (void)aml_read_pkglen(c); /* field flags */
        aml_scan_field_list(c, parent_scope, region_name);
        c->ptr = end;
        return;
    }

    aml_scan_term_list(c, scope);
    c->ptr = end;
}

static void aml_scan_term_list(acpi_cursor_t* c, const char* scope) {
    while (c->ptr < c->end) {
        const uint8_t* term_start = c->ptr;
        uint8_t op = *c->ptr++;
        if (op == AML_RETURN_OP || op == AML_BREAK_OP || op == AML_ELSE_OP) {
            c->ptr = term_start;
            return;
        }

        switch (op) {
            case AML_ZERO_OP:
            case AML_ONE_OP:
            case AML_BYTE_PREFIX:
            case AML_WORD_PREFIX:
            case AML_DWORD_PREFIX:
            case AML_QWORD_PREFIX:
            case AML_STRING_PREFIX:
            case AML_REF_OF_OP:
            case AML_DEREF_OF_OP:
            case AML_STORE_OP:
            case AML_ADD_OP:
            case AML_CONCAT_OP:
            case AML_SHIFT_LEFT_OP:
            case AML_SHIFT_RIGHT_OP:
            case AML_AND_OP:
            case AML_NAND_OP:
            case AML_OR_OP:
            case AML_NOR_OP:
            case AML_XOR_OP:
            case AML_NOT_OP:
            case AML_LAND_OP:
            case AML_LOR_OP:
            case AML_LNOT_OP:
            case AML_LEQUAL_OP:
            case AML_LGREATER_OP:
            case AML_LLESS_OP:
            case AML_INDEX_OP:
                c->ptr = term_start;
                (void)aml_eval_term(c, scope, NULL, NULL);
                break;
            case AML_NAME_OP: {
                char name[ACPI_MAX_PATH];
                if (!aml_scan_namestring(c, scope, name, sizeof(name))) { c->ptr = c->end; return; }
                acpi_value_t value = aml_parse_object_or_ref(c, scope, NULL, NULL);
                acpi_object_t* obj = acpi_new_object(name, name, ACPI_OBJ_INTEGER);
                if (obj) {
                    if (value.kind == ACPI_VAL_STRING) {
                        obj->kind = ACPI_OBJ_STRING;
                        acpi_strncpy0(obj->u.string, value.string, sizeof(obj->u.string));
                    } else if (value.kind == ACPI_VAL_BUFFER) {
                        obj->kind = ACPI_OBJ_BUFFER;
                        obj->u.buffer.length = value.buffer_len;
                        memcpy(obj->u.buffer.bytes, value.buffer, value.buffer_len);
                    } else {
                        obj->u.integer = aml_value_as_int(&value);
                    }
                }
                break;
            }
            case AML_PACKAGE_OP:
            case AML_BUFFER_OP:
                c->ptr = term_start;
                (void)aml_eval_term(c, scope, NULL, NULL);
                break;
            case AML_SCOPE_OP:
            case AML_METHOD_OP:
            case AML_OPREGION_OP:
            case AML_FIELD_OP:
            case AML_INDEXFIELD_OP:
            case AML_BANKFIELD_OP:
            case AML_DEVICE_OP:
            case AML_PROCESSOR_OP:
            case AML_POWERRES_OP:
            case AML_THERMAL_OP:
                aml_scan_container(c, scope, op);
                break;
            case AML_EXT_OP_PREFIX: {
                if (c->ptr >= c->end) return;
                uint8_t ext = *c->ptr++;
                if (ext == AML_DEVICE_OP || ext == AML_PROCESSOR_OP || ext == AML_POWERRES_OP || ext == AML_THERMAL_OP) {
                    aml_scan_container(c, scope, ext);
                }
                break;
            }
            default: {
                /* The stream is complex; best effort skip using pkglen for block-like ops. */
                if (op == AML_IF_OP || op == AML_WHILE_OP) {
                    (void)aml_read_pkglen(c);
                    c->ptr = c->end; /* conservative: stop on control blocks during scan */
                }
                break;
            }
        }
    }
}

static void aml_scan_namespace(void) {
    g_object_count = 0;
    acpi_add_builtin_methods();
    if (!g_dsdt_aml || !g_dsdt) return;
    acpi_cursor_t c = { g_dsdt_aml, ((const uint8_t*)g_dsdt) + g_dsdt->length };
    aml_scan_term_list(&c, "\\");
}

/* ------------------------------------------------------------
 * AML field / region access helpers
 * ------------------------------------------------------------ */

static uint64_t acpi_io_read(uint64_t port, uint64_t bytes) {
    switch (bytes) {
        case 1: return inb((uint16_t)port);
        case 2: return inw((uint16_t)port);
        case 4: return inl((uint16_t)port);
        default: return 0;
    }
}

static void acpi_io_write(uint64_t port, uint64_t bytes, uint64_t value) {
    switch (bytes) {
        case 1: outb((uint16_t)port, (uint8_t)value); break;
        case 2: outw((uint16_t)port, (uint16_t)value); break;
        case 4: outl((uint16_t)port, (uint32_t)value); break;
        default: break;
    }
}

static uint64_t acpi_region_read(const acpi_object_t* region, uint64_t bit_offset, uint64_t bit_length) {
    if (!region || region->kind != ACPI_OBJ_REGION || bit_length == 0) return 0;
    uint64_t byte_offset = bit_offset / 8u;
    uint64_t bytes = (bit_length + 7u) / 8u;
    uint64_t addr = region->u.region.base + byte_offset;

    if (region->u.region.space_id == ACPI_SPACE_SYSTEM_IO) {
        return acpi_io_read(addr, bytes <= 1 ? 1u : (bytes <= 2 ? 2u : 4u));
    }
    if (region->u.region.space_id == ACPI_SPACE_EMBEDDED_CTRL) {
        uint8_t val = 0;
        if (acpi_ec_read((uint8_t)byte_offset, &val)) return val;
        return 0;
    }
    if (region->u.region.space_id == ACPI_SPACE_SYSTEM_MEMORY) {
        uint64_t v = 0;
        for (uint64_t i = 0; i < bytes && i < 8; ++i) v |= ((uint64_t)acpi_read_u8(addr + i)) << (8u * i);
        return v;
    }
    if (region->u.region.space_id == ACPI_SPACE_SMBUS) {
        uint16_t word = 0;
        if (acpi_smbus_read_word(g_smbus_base, ACPI_BATTERY_SMBUS_ADDR, (uint8_t)byte_offset, &word)) return word;
        return 0;
    }
    return 0;
}

static bool acpi_region_write(const acpi_object_t* region, uint64_t bit_offset, uint64_t bit_length, uint64_t value) {
    if (!region || region->kind != ACPI_OBJ_REGION || bit_length == 0) return false;
    uint64_t byte_offset = bit_offset / 8u;
    uint64_t bytes = (bit_length + 7u) / 8u;
    uint64_t addr = region->u.region.base + byte_offset;

    if (region->u.region.space_id == ACPI_SPACE_SYSTEM_IO) {
        acpi_io_write(addr, bytes <= 1 ? 1u : (bytes <= 2 ? 2u : 4u), value);
        return true;
    }
    if (region->u.region.space_id == ACPI_SPACE_EMBEDDED_CTRL) {
        return acpi_ec_write((uint8_t)byte_offset, (uint8_t)value);
    }
    if (region->u.region.space_id == ACPI_SPACE_SYSTEM_MEMORY) {
        for (uint64_t i = 0; i < bytes && i < 8; ++i) {
            ((volatile uint8_t*)(uintptr_t)PHYS_TO_VIRT(addr))[i] = (uint8_t)(value >> (8u * i));
        }
        return true;
    }
    return false;
}

/* ------------------------------------------------------------
 * AML method execution helpers
 * ------------------------------------------------------------ */

static acpi_value_t acpi_eval_object_by_name(const char* scope, const char* name, const acpi_value_t* args, uint8_t arg_count, bool* ok) {
    acpi_object_t* obj = aml_resolve_object(scope, name);
    if (!obj) return aml_make_none();
    if (obj->kind == ACPI_OBJ_METHOD) return aml_call_method(obj, args, arg_count, obj->path, ok);
    if (obj->kind == ACPI_OBJ_FIELD) return aml_make_int(acpi_region_read(acpi_find_object_exact(obj->u.field.region_path), obj->u.field.bit_offset, obj->u.field.bit_length));
    if (obj->kind == ACPI_OBJ_INTEGER) return aml_make_int(obj->u.integer);
    if (obj->kind == ACPI_OBJ_STRING) return aml_make_string(obj->u.string);
    if (obj->kind == ACPI_OBJ_BUFFER) return aml_make_buffer(obj->u.buffer.bytes, obj->u.buffer.length);
    return aml_make_none();
}

static acpi_value_t acpi_try_eval_method(const char* method_name) {
    if (!g_aml_ready || !method_name) return aml_make_none();
    acpi_object_t* obj = acpi_find_object_exact(method_name);
    if (!obj || obj->kind != ACPI_OBJ_METHOD) return aml_make_none();
    bool ok = true;
    acpi_value_t none_args[1];
    none_args[0] = aml_make_none();
    return aml_call_method(obj, none_args, 0, obj->path, &ok);
}

static void acpi_update_from_aml(void) {
    if (!g_aml_ready) return;

    /* Try common battery methods in a few likely locations. */
    const char* methods[] = {
        "\\_SB.BAT0._BST",
        "\\_SB.PCI0.BAT0._BST",
        "\\_SB.PCI0.LPCB.BAT0._BST",
        "\\BAT0._BST",
        "\\_BST",
        NULL
    };

    for (int i = 0; methods[i]; ++i) {
        acpi_value_t v = acpi_try_eval_method(methods[i]);
        if (v.kind == ACPI_VAL_PACKAGE && v.package.count >= 4) {
            acpi_value_t v0 = aml_pkg_elem_to_value(&v.package.elems[0]);
            acpi_value_t v1 = aml_pkg_elem_to_value(&v.package.elems[1]);
            acpi_value_t v2 = aml_pkg_elem_to_value(&v.package.elems[2]);
            acpi_value_t v3 = aml_pkg_elem_to_value(&v.package.elems[3]);
            uint64_t state = aml_value_as_int(&v0);
            uint64_t rem = aml_value_as_int(&v1);
            uint64_t full = aml_value_as_int(&v2);
            uint64_t rate = aml_value_as_int(&v3);
            if (full) {
                uint64_t pct = (rem * 100u) / full;
                if (pct > 100u) pct = 100u;
                g_battery_percent = pct;
            }
            g_battery_charging = (state & 0x02u) != 0u || (state & 0x08u) != 0u;
            g_have_battery = true;
            g_battery_minutes = (rate > 0 && rem > 0 && !g_battery_charging) ? ((rem * 60u) / rate) : 0;
            acpi_strncpy0(g_source_label, "AML", sizeof(g_source_label));
            return;
        }
    }

    /* Try _BIF for a static full-capacity hint. */
    const char* bif_methods[] = {
        "\\_SB.BAT0._BIF",
        "\\_SB.PCI0.BAT0._BIF",
        "\\_SB.PCI0.LPCB.BAT0._BIF",
        "\\BAT0._BIF",
        "\\_BIF",
        NULL
    };
    for (int i = 0; bif_methods[i]; ++i) {
        acpi_value_t v = acpi_try_eval_method(bif_methods[i]);
        if (v.kind == ACPI_VAL_PACKAGE && v.package.count >= 4) {
            acpi_value_t b2 = aml_pkg_elem_to_value(&v.package.elems[2]);
            acpi_value_t b3 = aml_pkg_elem_to_value(&v.package.elems[3]);
            uint64_t design = aml_value_as_int(&b2);
            uint64_t lastfull = aml_value_as_int(&b3);
            (void)design;
            if (lastfull) {
                if (g_battery_percent > 100u) g_battery_percent = 100u;
                g_have_battery = true;
                acpi_strncpy0(g_source_label, "AML", sizeof(g_source_label));
                return;
            }
        }
    }
}


static bool g_thermal_present;
static uint64_t g_thermal_temperature_celsius;
static bool g_fan_present;
static uint64_t g_fan_speed_rpm;
static uint64_t g_cpu_cstate_count;

static bool acpi_refresh_acpica_backend(void) {
    if (!acpi_power_acpica_available() || !acpi_power_acpica_is_ready()) {
        return false;
    }

    bool updated = false;
    uint64_t percent = 0;
    uint64_t minutes = 0;
    uint64_t temperature = 0;
    uint64_t fan_rpm = 0;
    uint64_t cstate_count = 0;
    bool charging = false;

    if (acpi_power_acpica_query_battery(&percent, &charging, &minutes)) {
        g_battery_percent = (percent > 100u) ? 100u : percent;
        g_battery_charging = charging;
        g_battery_minutes = minutes;
        g_have_battery = true;
        acpi_strncpy0(g_source_label, "ACPICA", sizeof(g_source_label));
        updated = true;
    }

    if (acpi_power_acpica_query_thermal(&temperature, &fan_rpm, &cstate_count)) {
        if (temperature != UINT64_MAX) {
            g_thermal_temperature_celsius = temperature;
            g_thermal_present = true;
        }
        if (fan_rpm != UINT64_MAX) {
            g_fan_speed_rpm = fan_rpm;
            g_fan_present = true;
        }
        if (cstate_count != UINT64_MAX) {
            g_cpu_cstate_count = cstate_count;
        }
        if (!updated) {
            acpi_strncpy0(g_source_label, "ACPICA", sizeof(g_source_label));
        }
        updated = true;
    }

    if (acpi_power_acpica_ec_is_available()) {
        g_ec_ready = true;
        acpi_strncpy0(g_source_label, "ACPICA", sizeof(g_source_label));
        updated = true;
    }

    return updated;
}

/* ------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------ */

void acpi_power_init(void) {
    /* Strict once-only, with a hard reentrancy guard. The previous version
     * flipped g_acpi_ready=true at the top of this function, which meant
     * a concurrent caller (heartbeat thread + GUI paint thread) could
     * both pass the check and end up re-doing the entire AML namespace
     * scan, ACPI table discovery and SMBus probe. That is the exact
     * cause of the per-frame "tables discovered / AML namespace parsed"
     * spam that made opening any app feel sluggish.
     *
     * A plain bool check-then-set is NOT enough under real preemption:
     * a timer interrupt can land right between "if (g_acpi_init_in_progress)"
     * and "g_acpi_init_in_progress = true", letting a second thread slip
     * through the same window and re-run the whole init. `volatile` only
     * stops the compiler from caching the value in a register - it gives
     * no atomicity at all. So we do the check-and-set itself with
     * interrupts disabled (a tiny, fixed-cost window), then re-enable
     * interrupts before doing the actual (potentially slow) ACPI work. */
    uint64_t acpi_init_irq_flags = sync_irq_save();
    if (g_acpi_ready || g_acpi_init_in_progress) {
        sync_irq_restore(acpi_init_irq_flags);
        return;
    }
    g_acpi_init_in_progress = true;
    sync_irq_restore(acpi_init_irq_flags);

    memset(g_objects, 0, sizeof(g_objects));
    g_object_count = 0;
    g_battery_percent = 100;
    g_battery_charging = false;
    g_battery_minutes = 0;
    g_have_battery = false;
    acpi_strncpy0(g_source_label, "fallback", sizeof(g_source_label));

    if (acpi_power_acpica_available() && acpi_power_acpica_initialize()) {
        if (acpi_refresh_acpica_backend()) {
            if (!g_acpi_log_acpica_active) {
                serial_puts("[ACPI] ACPICA backend active\n");
                g_acpi_log_acpica_active = true;
            }
            g_heavy_refresh_done = true;
            g_acpi_ready = true;
            g_acpi_init_in_progress = false;
            return;
        }
    }

    g_acpi_available = acpi_discover_tables();
    if (!g_acpi_available) {
        if (!g_acpi_log_tables_not_found) {
            serial_puts("[ACPI] tables not found; using fallback battery model\n");
            g_acpi_log_tables_not_found = true;
        }
        g_heavy_refresh_done = true;
        g_acpi_ready = true;
        g_acpi_init_in_progress = false;
        return;
    }

    if (!g_acpi_log_tables_discovered) {
        serial_puts("[ACPI] tables discovered\n");
        g_acpi_log_tables_discovered = true;
    }
    acpi_detect_smbus();
    acpi_detect_ec();
    aml_scan_namespace();
    g_aml_ready = (g_object_count > 0);
    if (g_aml_ready && !g_acpi_log_aml_parsed) {
        serial_puts("[ACPI] AML namespace parsed (subset)\n");
        g_acpi_log_aml_parsed = true;
    }

    if (acpi_smart_battery_probe()) {
        if (!g_acpi_log_smbus_active) {
            serial_puts("[ACPI] Smart battery over SMBus active\n");
            g_acpi_log_smbus_active = true;
        }
        g_heavy_refresh_done = true;
        g_acpi_ready = true;
        g_acpi_init_in_progress = false;
        return;
    }

    acpi_update_from_aml();
    if (g_have_battery) {
        if (!g_acpi_log_battery_aml) {
            serial_puts("[ACPI] Battery data acquired via AML\n");
            g_acpi_log_battery_aml = true;
        }
        g_heavy_refresh_done = true;
        g_acpi_ready = true;
        g_acpi_init_in_progress = false;
        return;
    }

    if (!g_acpi_log_no_real_battery) {
        serial_puts("[ACPI] No real battery source found; fallback model retained\n");
        g_acpi_log_no_real_battery = true;
    }
    /* Mark heavy work done so subsequent force_refresh() calls short-circuit
     * instead of pointlessly re-running the AML eval/EC poll every GUI
     * frame. */
    g_heavy_refresh_done = true;
    g_acpi_ready = true;
    g_acpi_init_in_progress = false;
}

bool acpi_power_is_available(void) {
    return g_have_battery;
}

void acpi_power_force_refresh(void) {
    if (!g_acpi_ready) acpi_power_init();

    /* Hard throttle: refresh at most once per second. draw_taskbar() and
     * cos_power_sample() each call into us on every GUI frame, so without
     * this each refresh would re-walk the ACPICA namespace, repoll SMBus
     * and re-run AML evaluation dozens of times per second when an app
     * is on screen. */
    uint64_t now = get_timer_ticks();
    if (g_last_heavy_refresh_ms != 0 &&
        (now - g_last_heavy_refresh_ms) < 1000) {
        return;
    }
    g_last_heavy_refresh_ms = now;

    /* If init() already determined that there is no real battery source
     * (no ACPICA, no SMBus, no useful AML), there is nothing meaningful
     * to refresh and the only effect of forcing a refresh here would be
     * to spam heavy work every paint frame. Bail out early so the OS
     * stays responsive when a window is open. */
    if (g_heavy_refresh_done && !g_have_battery &&
        !acpi_power_acpica_is_ready() && !g_smbus_ready && !g_ec_ready) {
        return;
    }

    if (acpi_refresh_acpica_backend()) return;
    if (acpi_smart_battery_probe()) return;
    acpi_update_from_aml();
}

uint64_t acpi_power_get_battery_percent(void) {
    if (!g_acpi_ready) acpi_power_init();

    /* Only re-probe (ACPICA / SMBus smart battery / AML) at most once
     * per second. This is called from the taskbar on every GUI frame,
     * and re-running the full battery probe that often was doing a lot
     * of unnecessary work for a value that doesn't change that fast. */
    static uint64_t s_last_refresh_tick = 0;
    uint64_t now = get_timer_ticks();
    if (now - s_last_refresh_tick >= 1000 || s_last_refresh_tick == 0) {
        acpi_power_force_refresh();
        s_last_refresh_tick = now;
    }

    if (g_have_battery) return g_battery_percent;
    return g_battery_percent;
}

bool acpi_power_is_charging(void) {
    if (!g_acpi_ready) acpi_power_init();
    acpi_power_force_refresh();
    return g_battery_charging;
}

uint64_t acpi_power_get_estimated_minutes_remaining(void) {
    if (!g_acpi_ready) acpi_power_init();
    acpi_power_force_refresh();
    return g_battery_minutes;
}

const char* acpi_power_get_source_label(void) {
    if (!g_acpi_ready) acpi_power_init();
    return g_source_label;
}


/* ------------------------------------------------------------
 * Power-off helper
 * ------------------------------------------------------------ */

static bool acpi_try_poweroff_acpica(void) {
    if (!acpi_power_acpica_available() || !acpi_power_acpica_is_ready()) {
        return false;
    }

    serial_puts("[ACPI] poweroff via ACPICA S5\n");
    return acpi_power_acpica_suspend(5u);
}

bool acpi_power_poweroff(void) {
    if (!g_acpi_ready) acpi_power_init();

    if (acpi_try_poweroff_acpica()) {
        return true;
    }

    /* Best-effort legacy fallback:
     *  - ask AML side to prepare for S5 (if available)
     *  - then rely on platform / emulator poweroff ports
     */
    if (g_aml_ready) {
        const acpi_value_t arg = aml_make_int(5u);
        const acpi_value_t args[1] = { arg };
        const char* const pts_methods[] = {
            "\\_PTS",
            "\\_SB._PTS",
            "\\_SB.PCI0._PTS",
            "\\_SB.LPCB._PTS",
            NULL
        };
        bool pts_ok = false;
        (void)acpi_try_eval_first_method(pts_methods, args, 1, NULL, &pts_ok);
        (void)pts_ok;
    }

    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    return false;
}

/* ------------------------------------------------------------
 * Phase 2 / 3 / 4 extensions
 * ------------------------------------------------------------ */

static bool g_thermal_present = false;
static uint64_t g_thermal_temperature_celsius = 0;
static bool g_fan_present = false;
static uint64_t g_fan_speed_rpm = 0;
static uint64_t g_cpu_cstate_count = 0;
static bool g_suspend_prepared = false;
static uint8_t g_last_sleep_state = 3;

#if defined(COS_USE_ACPICA) || defined(ACPI_USE_ACPICA) || defined(ACPICA_VERSION)
#define ACPI_POWER_HAS_ACPICA 0
#else
#define ACPI_POWER_HAS_ACPICA 0
#endif

static acpi_value_t acpi_try_eval_method_args(const char* method_name, const acpi_value_t* args, uint8_t arg_count, bool* success) {
    if (!g_aml_ready || !method_name) {
        if (success) *success = false;
        return aml_make_none();
    }
    acpi_object_t* obj = acpi_find_object_exact(method_name);
    if (!obj || obj->kind != ACPI_OBJ_METHOD) {
        if (success) *success = false;
        return aml_make_none();
    }
    bool ok = true;
    acpi_value_t v = aml_call_method(obj, args, arg_count, obj->path, &ok);
    if (success) *success = ok;
    return ok ? v : aml_make_none();
}

static acpi_value_t acpi_try_eval_first_method(const char* const* methods, const acpi_value_t* args, uint8_t arg_count, const char** matched_name, bool* success) {
    if (success) *success = false;
    if (!methods) return aml_make_none();
    for (uint32_t i = 0; methods[i]; ++i) {
        acpi_object_t* obj = acpi_find_object_exact(methods[i]);
        if (!obj || obj->kind != ACPI_OBJ_METHOD) continue;
        if (matched_name) *matched_name = methods[i];
        return acpi_try_eval_method_args(methods[i], args, arg_count, success);
    }
    return aml_make_none();
}

static uint64_t acpi_temperature_to_celsius(uint64_t raw) {
    /* ACPI thermal values are commonly returned in tenths of Kelvin. */
    if (raw >= 1000u) {
        if (raw <= 2731u) return 0;
        return (raw - 2732u) / 10u;
    }
    return raw;
}

static void acpi_refresh_extended_state(void) {
    if (!g_aml_ready) return;

    /* Thermal zones: common names across BIOSes. */
    const char* const thermal_methods[] = {
        "\\_TZ.TZ00._TMP",
        "\\_TZ.THRM._TMP",
        "\\_SB.TZ00._TMP",
        "\\_SB.THRM._TMP",
        "\\TZ00._TMP",
        "\\THRM._TMP",
        "\\_TMP",
        NULL
    };

    acpi_value_t thermal = acpi_try_eval_first_method(thermal_methods, NULL, 0, NULL, NULL);
    if (thermal.kind != ACPI_VAL_NONE) {
        uint64_t raw = aml_value_as_int(&thermal);
        g_thermal_temperature_celsius = acpi_temperature_to_celsius(raw);
        g_thermal_present = true;
    }

    /* Fan telemetry: _FST usually returns status + speed. */
    const char* const fan_methods[] = {
        "\\_TZ.FAN0._FST",
        "\\_SB.FAN0._FST",
        "\\_TZ.FAN0._FPS",
        "\\_SB.FAN0._FPS",
        "\\FAN0._FST",
        "\\FAN0._FPS",
        NULL
    };

    acpi_value_t fan = acpi_try_eval_first_method(fan_methods, NULL, 0, NULL, NULL);
    if (fan.kind != ACPI_VAL_NONE) {
        if (fan.kind == ACPI_VAL_PACKAGE && fan.package.count >= 2) {
            acpi_value_t speed_v = aml_pkg_elem_to_value(&fan.package.elems[1]);
            g_fan_speed_rpm = aml_value_as_int(&speed_v);
        } else {
            g_fan_speed_rpm = aml_value_as_int(&fan);
        }
        g_fan_present = true;
    }

    /* CPU C-state support: the package size is a good minimal proxy. */
    const char* const cstate_methods[] = {
        "\\_PR.CPU0._CST",
        "\\_SB.CPU0._CST",
        "\\CPU0._CST",
        "\\_CST",
        NULL
    };

    acpi_value_t cst = acpi_try_eval_first_method(cstate_methods, NULL, 0, NULL, NULL);
    if (cst.kind != ACPI_VAL_NONE) {
        if (cst.kind == ACPI_VAL_PACKAGE) {
            uint64_t count = cst.package.count;
            if (count >= 2u) {
                /* Heuristic: drop the revision/count fields when present. */
                g_cpu_cstate_count = (count > 2u) ? (count - 2u) : (count - 1u);
            } else {
                g_cpu_cstate_count = count;
            }
        } else {
            g_cpu_cstate_count = aml_value_as_int(&cst);
        }
    }
}

bool acpi_power_ec_is_available(void) {
    if (acpi_power_acpica_ec_is_available()) return true;
    return g_ec_ready;
}

bool acpi_power_ec_read8(uint8_t reg, uint8_t* out) {
    if (!out) return false;
    if (acpi_power_acpica_ec_is_available()) return acpi_power_acpica_ec_read8(reg, out);
    return g_ec_ready && acpi_ec_read(reg, out);
}

bool acpi_power_ec_write8(uint8_t reg, uint8_t value) {
    if (acpi_power_acpica_ec_is_available()) return acpi_power_acpica_ec_write8(reg, value);
    return g_ec_ready && acpi_ec_write(reg, value);
}

uint64_t acpi_power_get_thermal_temperature_celsius(void) {
    if (!g_acpi_ready) acpi_power_init();
    acpi_refresh_extended_state();
    return g_thermal_present ? g_thermal_temperature_celsius : 0u;
}

uint64_t acpi_power_get_fan_speed_rpm(void) {
    if (!g_acpi_ready) acpi_power_init();
    acpi_refresh_extended_state();
    return g_fan_present ? g_fan_speed_rpm : 0u;
}

uint64_t acpi_power_get_cpu_cstate_count(void) {
    if (!g_acpi_ready) acpi_power_init();
    acpi_refresh_extended_state();
    return g_cpu_cstate_count;
}

bool acpi_power_suspend_to_state(uint8_t sleep_state) {
    if (!g_acpi_ready) acpi_power_init();
    serial_puts("[ACPI] suspend_to_state S");
    serial_putdec((uint64_t)sleep_state);
    serial_puts("\n");
    if (acpi_power_acpica_is_ready()) return acpi_power_acpica_suspend(sleep_state);
    if (!g_aml_ready) return false;

    const acpi_value_t arg = aml_make_int(sleep_state);
    const acpi_value_t args[1] = { arg };
    const char* const pts_methods[] = {
        "\\_PTS",
        "\\_SB._PTS",
        "\\_SB.PCI0._PTS",
        "\\_SB.LPCB._PTS",
        NULL
    };

    bool pts_ok = false;
    acpi_value_t pts = acpi_try_eval_first_method(pts_methods, args, 1, NULL, &pts_ok);
    if (!pts_ok && pts.kind == ACPI_VAL_NONE) return false;

    g_last_sleep_state = sleep_state;
    g_suspend_prepared = true;
    return true;
}

bool acpi_power_resume_from_state(uint8_t sleep_state) {
    if (!g_acpi_ready) acpi_power_init();
    serial_puts("[ACPI] resume_from_state S");
    serial_putdec((uint64_t)sleep_state);
    serial_puts("\n");
    if (acpi_power_acpica_is_ready()) return acpi_power_acpica_resume(sleep_state);
    if (!g_suspend_prepared) return false;

    if (sleep_state == 0u) sleep_state = g_last_sleep_state;
    const acpi_value_t arg = aml_make_int(sleep_state);
    const acpi_value_t args[1] = { arg };
    const char* const wak_methods[] = {
        "\\_WAK",
        "\\_SB._WAK",
        "\\_SB.PCI0._WAK",
        "\\_SB.LPCB._WAK",
        NULL
    };

    bool wak_ok = false;
    acpi_value_t wak = acpi_try_eval_first_method(wak_methods, args, 1, NULL, &wak_ok);
    g_suspend_prepared = false;
    if (!wak_ok && wak.kind == ACPI_VAL_NONE) return false;
    return true;
}

bool acpi_power_has_full_acpica(void) {
    return acpi_power_acpica_available();
}

const char* acpi_power_get_backend_name(void) {
    return acpi_power_has_full_acpica() ? "ACPICA" : "compact-aml";
}

