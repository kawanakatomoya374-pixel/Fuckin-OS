
/**
 * acpica_backend.c - ACPICA integration bridge / OS Services Layer
 *
 * This file provides the ACPICA OS Services Layer for the kernel and a small
 * bootstrap wrapper used by acpi_power.c.  It is intentionally conservative:
 * the bridge is designed to compile even when the ACPICA source tree is not
 * present, in which case every entry point degrades to a safe no-op.
 */

#include "types.h"
#include "string.h"
#include "serial.h"
#include "io.h"
#include "memory.h"
#include "timer.h"
#include "irq.h"
#include "idt.h"
#include "pci.h"
#include "mm/paging.h"
#include <stdarg.h>

/* ACPICA is vendored in src/third_party/acpica and its include path is part
 * of the kernel build.  The prior permanently-disabled probe made every
 * ACPICA entry point a no-op even though libacpica.a was linked. */
#ifndef COS_ACPICA_PRESENT
#define COS_ACPICA_PRESENT 1
#endif

static bool g_acpica_initialized = false;
static bool g_acpica_bootstrapped = false;
static void* g_ec_device = NULL;
static bool g_ec_handler_installed = false;
static bool g_ec_ports_ready = false;
static volatile bool g_battery_notify_dirty = true;
static volatile bool g_thermal_notify_dirty = true;
static volatile bool g_gpe_notify_dirty = true;
static volatile uint64_t g_gpe_event_count = 0;
static volatile uint64_t g_fixed_event_count = 0;
static volatile uint64_t g_suspend_request_count = 0;
static volatile uint64_t g_resume_request_count = 0;

static void acpi_local_log_prefix(const char* tag) {
    serial_puts("[ACPICA] ");
    serial_puts(tag);
    serial_puts(": ");
}

static void acpi_local_log_sleep_state(const char* tag, uint8_t sleep_state) {
    acpi_local_log_prefix(tag);
    serial_puts("S");
    serial_putdec((uint64_t)sleep_state);
    serial_puts("\n");
}

bool acpi_power_acpica_available(void) {
    return COS_ACPICA_PRESENT ? true : false;
}

bool acpi_power_acpica_is_ready(void) {
    return g_acpica_initialized;
}

#if COS_ACPICA_PRESENT

#include "acpi.h"

extern void timer_wait_ms(uint64_t ms);

typedef struct {
    volatile uint32_t locked;
} acpi_os_lock_t;

typedef struct {
    uint32_t max_units;
    uint32_t units;
} acpi_os_sem_t;

typedef struct {
    uint16_t object_size;
} acpi_os_cache_t;

typedef struct {
    ACPI_OSD_HANDLER handler;
    void* context;
} acpi_os_irq_binding_t;

static acpi_os_irq_binding_t g_acpi_irq_bindings[256];

static uint32_t acpi_local_irq_from_intno(uint64_t int_no) {
    if (int_no >= 32u && int_no < 48u) {
        return (uint32_t)(int_no - 32u);
    }
    return (uint32_t)int_no;
}

static void acpi_os_irq_dispatch(struct regs* r) {
    if (!r) return;
    uint32_t irq = acpi_local_irq_from_intno(r->int_no);
    if (irq < 256u && g_acpi_irq_bindings[irq].handler) {
        g_acpi_irq_bindings[irq].handler(g_acpi_irq_bindings[irq].context);
    }
}

static void acpi_local_strcpy(char* dst, const char* src, size_t cap) {
    if (!dst || !cap) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

static uint8_t acpi_local_checksum(const uint8_t* p, uint64_t len) {
    uint8_t s = 0;
    for (uint64_t i = 0; i < len; ++i) s = (uint8_t)(s + p[i]);
    return s;
}

typedef struct PACKED {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_phys;
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} acpi_rsdp_t;

/* Provided by kernel.c: RSDP physical address forwarded from the
   bootloader's Multiboot2 tag (14/15), if present. See the longer
   explanation next to the equivalent check in acpi_power.c -- using this
   is what makes RSDP discovery work under UEFI too, since GRUB resolves
   it from the EFI configuration table there instead of the BIOS EBDA. */
extern uint64_t cos_mb2_get_acpi_rsdp(void);

static const acpi_rsdp_t* acpi_local_scan_rsdp(void) {
    /* IMPORTANT: every address handled in this function is a *physical*
     * address (from the Multiboot2 tag, the BDA, or the legacy BIOS scan
     * range). None of it is identity-mapped once paging_init() installs
     * its narrower kernel map, so it must go through PHYS_TO_VIRT() before
     * being dereferenced -- see the matching fix/explanation in
     * acpi_power.c (acpi_phys_ptr()). This used to cast the raw physical
     * value straight to a pointer, which page-faulted the first time any
     * caller touched ACPI (e.g. the taskbar's battery gauge on the first
     * GUI frame after boot). */
    uint64_t mb2_rsdp_phys = cos_mb2_get_acpi_rsdp();
    if (mb2_rsdp_phys) {
        const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)(uintptr_t)PHYS_TO_VIRT(mb2_rsdp_phys);
        if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' && rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
            rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' && rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ' &&
            acpi_local_checksum((const uint8_t*)rsdp, 20u) == 0u) {
            return rsdp;
        }
        /* Tag present but invalid -- fall through to the legacy scan. */
    }

    /* Reading the BIOS Data Area at a fixed physical address; GCC's
     * -Warray-bounds misfires on this low-address raw pointer cast, so the
     * warning is silenced locally instead of globally. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    uint16_t ebda_seg = *(const volatile uint16_t*)(uintptr_t)PHYS_TO_VIRT(0x40EULL);
#pragma GCC diagnostic pop
    uint64_t ebda_phys = ((uint64_t)ebda_seg) << 4u;
    if (ebda_phys) {
        for (uint64_t phys = ebda_phys; phys < ebda_phys + 1024u; phys += 16u) {
            const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)(uintptr_t)PHYS_TO_VIRT(phys);
            if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' && rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
                rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' && rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ') {
                if (acpi_local_checksum((const uint8_t*)rsdp, 20u) == 0u) return rsdp;
            }
        }
    }
    for (uint64_t phys = 0x000E0000ULL; phys < 0x00100000ULL; phys += 16u) {
        const acpi_rsdp_t* rsdp = (const acpi_rsdp_t*)(uintptr_t)PHYS_TO_VIRT(phys);
        if (rsdp->signature[0] == 'R' && rsdp->signature[1] == 'S' && rsdp->signature[2] == 'D' && rsdp->signature[3] == ' ' &&
            rsdp->signature[4] == 'P' && rsdp->signature[5] == 'T' && rsdp->signature[6] == 'R' && rsdp->signature[7] == ' ') {
            if (acpi_local_checksum((const uint8_t*)rsdp, 20u) == 0u) return rsdp;
        }
    }
    return NULL;
}

static ACPI_PHYSICAL_ADDRESS acpi_local_find_rsdp(void) {
    const acpi_rsdp_t* rsdp = acpi_local_scan_rsdp();
    if (!rsdp) return 0;
    /* acpi_local_scan_rsdp() returns a translated *virtual* pointer (see
     * above); ACPICA expects a physical address back from this function,
     * so convert back with VIRT_TO_PHYS() instead of just stripping the
     * pointer down to an integer as before. */
    return (ACPI_PHYSICAL_ADDRESS)VIRT_TO_PHYS((uint64_t)(uintptr_t)rsdp);
}

static bool acpi_local_ec_wait_input_clear(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((inb(0x66u) & 0x02u) == 0u) return true;
    }
    return false;
}

static bool acpi_local_ec_wait_output_full(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((inb(0x66u) & 0x01u) != 0u) return true;
    }
    return false;
}

static bool acpi_local_ec_port_read8(uint8_t reg, uint8_t* out) {
    if (!out) return false;
    if (!acpi_local_ec_wait_input_clear()) return false;
    outb(0x66u, 0x80u);
    if (!acpi_local_ec_wait_input_clear()) return false;
    outb(0x62u, reg);
    if (!acpi_local_ec_wait_output_full()) return false;
    *out = inb(0x62u);
    return true;
}

static bool acpi_local_ec_port_write8(uint8_t reg, uint8_t value) {
    if (!acpi_local_ec_wait_input_clear()) return false;
    outb(0x66u, 0x81u);
    if (!acpi_local_ec_wait_input_clear()) return false;
    outb(0x62u, reg);
    if (!acpi_local_ec_wait_input_clear()) return false;
    outb(0x62u, value);
    return true;
}

static ACPI_STATUS acpi_local_ec_address_space_handler(
    UINT32 Function,
    ACPI_PHYSICAL_ADDRESS Address,
    UINT32 BitWidth,
    UINT64 *Value,
    void *HandlerContext,
    void *RegionContext)
{
    (void)HandlerContext;
    (void)RegionContext;
    if (!Value || BitWidth != 8u) return AE_BAD_PARAMETER;
    uint8_t byte = 0;
    if (Function == ACPI_READ) {
        if (!acpi_local_ec_port_read8((uint8_t)Address, &byte)) return AE_ERROR;
        *Value = byte;
        return AE_OK;
    }
    if (Function == ACPI_WRITE) {
        return acpi_local_ec_port_write8((uint8_t)Address, (uint8_t)(*Value & 0xFFu)) ? AE_OK : AE_ERROR;
    }
    return AE_BAD_PARAMETER;
}

static ACPI_STATUS acpi_local_find_ec_callback(ACPI_HANDLE Object, UINT32 NestingLevel, void *Context, void **ReturnValue) {
    (void)NestingLevel;
    (void)Context;
    (void)ReturnValue;
    if (!g_ec_device) {
        g_ec_device = Object;
    }
    return AE_OK;
}

static void acpi_local_mark_notify_dirty(const char* kind) {
    g_gpe_notify_dirty = true;
    if (!kind) {
        g_battery_notify_dirty = true;
        g_thermal_notify_dirty = true;
        return;
    }
    if (strcmp(kind, "battery") == 0 || strcmp(kind, "power") == 0) {
        g_battery_notify_dirty = true;
    }
    if (strcmp(kind, "thermal") == 0 || strcmp(kind, "fan") == 0) {
        g_thermal_notify_dirty = true;
    }
    if (strcmp(kind, "all") == 0) {
        g_battery_notify_dirty = true;
        g_thermal_notify_dirty = true;
    }
}

static void acpi_local_device_notify_handler(ACPI_HANDLE Device, UINT32 Value, void *Context) {
    (void)Device;
    (void)Value;
    acpi_local_mark_notify_dirty((const char*)Context);
}

static ACPI_STATUS acpi_local_install_notify_callback(ACPI_HANDLE Object, UINT32 NestingLevel, void *Context, void **ReturnValue) {
    (void)NestingLevel;
    (void)ReturnValue;
    if (!Object) return AE_OK;
    const char* kind = (const char*)Context;
    ACPI_STATUS st = AcpiInstallNotifyHandler(Object, ACPI_DEVICE_NOTIFY, acpi_local_device_notify_handler, (void*)kind);
    if (ACPI_SUCCESS(st)) {
        acpi_local_mark_notify_dirty(kind);
    }
    return AE_OK;
}

static void acpi_local_install_notify_support(void) {
    (void)AcpiGetDevices("PNP0C0A", acpi_local_install_notify_callback, (void*)"battery", NULL);
    (void)AcpiGetDevices("PNP0C0B", acpi_local_install_notify_callback, (void*)"thermal", NULL);
    (void)AcpiGetDevices("PNP0C0F", acpi_local_install_notify_callback, (void*)"fan", NULL);
}
static UINT32 acpi_local_fixed_event_handler(void *Context) {
    const char* kind = (const char*)Context;
    ++g_fixed_event_count;
    g_gpe_notify_dirty = true;
    if (kind && kind[0]) {
        acpi_local_log_prefix("fixed-event");
        serial_puts(kind);
        serial_puts("\n");
    }
    return ACPI_INTERRUPT_HANDLED;
}

static void acpi_local_global_event_handler(UINT32 EventType, ACPI_HANDLE Device, UINT32 EventNumber, void *Context) {
    (void)Device;
    (void)Context;
    if (EventType == ACPI_EVENT_TYPE_GPE) {
        ++g_gpe_event_count;
        g_gpe_notify_dirty = true;
        acpi_local_log_prefix("gpe");
        serial_puts("#");
        serial_putdec((uint64_t)EventNumber);
        serial_puts("\n");
        return;
    }
    ++g_fixed_event_count;
    g_gpe_notify_dirty = true;
    acpi_local_log_prefix("fixed");
    serial_puts("#");
    serial_putdec((uint64_t)EventNumber);
    serial_puts("\n");
}

static void acpi_local_install_event_support(void) {
    if (ACPI_SUCCESS(AcpiInstallGlobalEventHandler(acpi_local_global_event_handler, NULL))) {
        (void)AcpiEnableEvent(ACPI_EVENT_GLOBAL, 0u);
    }
    if (ACPI_SUCCESS(AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, acpi_local_fixed_event_handler, (void*)"power-button"))) {
        (void)AcpiEnableEvent(ACPI_EVENT_POWER_BUTTON, 0u);
    }
    if (ACPI_SUCCESS(AcpiInstallFixedEventHandler(ACPI_EVENT_SLEEP_BUTTON, acpi_local_fixed_event_handler, (void*)"sleep-button"))) {
        (void)AcpiEnableEvent(ACPI_EVENT_SLEEP_BUTTON, 0u);
    }
    if (ACPI_SUCCESS(AcpiInstallFixedEventHandler(ACPI_EVENT_RTC, acpi_local_fixed_event_handler, (void*)"rtc"))) {
        (void)AcpiEnableEvent(ACPI_EVENT_RTC, 0u);
    }
}

static void acpi_local_install_ec_support(void) {
    if (g_ec_handler_installed) return;
    uint8_t st = inb(0x66u);
    g_ec_ports_ready = (st != 0xFFu);
    g_ec_device = NULL;
    if (ACPI_SUCCESS(AcpiGetDevices("PNP0C09", acpi_local_find_ec_callback, NULL, NULL)) && g_ec_device) {
        ACPI_STATUS st = AcpiInstallAddressSpaceHandler(
            g_ec_device,
            ACPI_ADR_SPACE_EC,
            acpi_local_ec_address_space_handler,
            NULL,
            NULL);
        if (ACPI_SUCCESS(st)) {
            g_ec_handler_installed = true;
        }
    }
}

/* ---- OSL primitives ---- */

ACPI_STATUS AcpiOsInitialize(void) { return AE_OK; }
ACPI_STATUS AcpiOsTerminate(void) { return AE_OK; }

void *AcpiOsAllocate(ACPI_SIZE Size) {
    return kmalloc((size_t)Size);
}

void AcpiOsFree(void *Memory) {
    if (Memory) kfree(Memory);
}

static void acpi_local_vprintf(const char* format, va_list args) {
    char buffer[1024];
    if (!format) format = "";
    vsnprintf(buffer, sizeof(buffer), format, args);
    serial_puts(buffer);
}

void AcpiOsPrintf(const char* Format, ...) {
    va_list args;
    va_start(args, Format);
    acpi_local_vprintf(Format, args);
    va_end(args);
}

void AcpiOsVprintf(const char* Format, va_list Args) {
    acpi_local_vprintf(Format, Args);
}

ACPI_STATUS AcpiOsCreateCache(char *CacheName, UINT16 ObjectSize, UINT16 MaxDepth, ACPI_CACHE_T **ReturnCache) {
    (void)CacheName; (void)MaxDepth;
    if (!ReturnCache) return AE_BAD_PARAMETER;
    acpi_os_cache_t* cache = (acpi_os_cache_t*)kmalloc(sizeof(acpi_os_cache_t));
    if (!cache) return AE_NO_MEMORY;
    cache->object_size = ObjectSize;
    *ReturnCache = (ACPI_CACHE_T*)cache;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteCache(ACPI_CACHE_T *Cache) {
    if (Cache) kfree(Cache);
    return AE_OK;
}

ACPI_STATUS AcpiOsPurgeCache(ACPI_CACHE_T *Cache) {
    (void)Cache;
    return AE_OK;
}

void *AcpiOsAcquireObject(ACPI_CACHE_T *Cache) {
    if (!Cache) return NULL;
    acpi_os_cache_t* cache = (acpi_os_cache_t*)Cache;
    void* obj = kmalloc(cache->object_size ? cache->object_size : 1u);
    if (obj) memset(obj, 0, cache->object_size ? cache->object_size : 1u);
    return obj;
}

ACPI_STATUS AcpiOsReleaseObject(ACPI_CACHE_T *Cache, void *Object) {
    (void)Cache;
    if (Object) kfree(Object);
    return AE_OK;
}

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle) {
    if (!OutHandle) return AE_BAD_PARAMETER;
    acpi_os_lock_t* lock = (acpi_os_lock_t*)kmalloc(sizeof(acpi_os_lock_t));
    if (!lock) return AE_NO_MEMORY;
    lock->locked = 0;
    *OutHandle = (ACPI_SPINLOCK)lock;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle) {
    if (Handle) kfree((void*)Handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle) {
    (void)Handle;
    return 0;
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags) {
    (void)Handle;
    (void)Flags;
}

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE *OutHandle) {
    if (!OutHandle) return AE_BAD_PARAMETER;
    acpi_os_sem_t* sem = (acpi_os_sem_t*)kmalloc(sizeof(acpi_os_sem_t));
    if (!sem) return AE_NO_MEMORY;
    sem->max_units = MaxUnits ? MaxUnits : 1u;
    sem->units = (InitialUnits <= sem->max_units) ? InitialUnits : sem->max_units;
    *OutHandle = (ACPI_SEMAPHORE)sem;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle) {
    if (Handle) kfree((void*)Handle);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout) {
    (void)Timeout;
    if (!Handle || Units == 0u) return AE_BAD_PARAMETER;
    acpi_os_sem_t* sem = (acpi_os_sem_t*)Handle;
    if (sem->units >= Units) {
        sem->units -= Units;
    } else {
        sem->units = 0u;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units) {
    if (!Handle || Units == 0u) return AE_BAD_PARAMETER;
    acpi_os_sem_t* sem = (acpi_os_sem_t*)Handle;
    uint64_t next = (uint64_t)sem->units + (uint64_t)Units;
    sem->units = (next > sem->max_units) ? sem->max_units : (uint32_t)next;
    return AE_OK;
}

#if (ACPI_MUTEX_TYPE != ACPI_BINARY_SEMAPHORE)
ACPI_STATUS AcpiOsCreateMutex(ACPI_MUTEX *OutHandle) {
    return AcpiOsCreateSemaphore(1u, 1u, (ACPI_SEMAPHORE*)OutHandle);
}

void AcpiOsDeleteMutex(ACPI_MUTEX Handle) {
    (void)AcpiOsDeleteSemaphore((ACPI_SEMAPHORE)Handle);
}

ACPI_STATUS AcpiOsAcquireMutex(ACPI_MUTEX Handle, UINT16 Timeout) {
    return AcpiOsWaitSemaphore((ACPI_SEMAPHORE)Handle, 1u, Timeout);
}

void AcpiOsReleaseMutex(ACPI_MUTEX Handle) {
    (void)AcpiOsSignalSemaphore((ACPI_SEMAPHORE)Handle, 1u);
}
#endif

void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length) {
    (void)Length;
    /* Where is a *physical* address; translate it into the kernel's
     * higher-half direct map of RAM before handing back a usable pointer.
     * See the note on acpi_local_scan_rsdp() above -- this was previously
     * returned as-is, which faulted the first time ACPICA (or any code
     * downstream of it) actually dereferenced the "pointer" it got back. */
    return (void*)(uintptr_t)PHYS_TO_VIRT((uint64_t)Where);
}

void AcpiOsUnmapMemory(void *Where, ACPI_SIZE Length) {
    (void)Where;
    (void)Length;
}

ACPI_STATUS AcpiOsGetPhysicalAddress(void *LogicalAddress, ACPI_PHYSICAL_ADDRESS *PhysicalAddress) {
    if (!PhysicalAddress) return AE_BAD_PARAMETER;
    *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)VIRT_TO_PHYS((uint64_t)(uintptr_t)LogicalAddress);
    return AE_OK;
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width) {
    if (!Value) return AE_BAD_PARAMETER;
    uintptr_t virt = (uintptr_t)PHYS_TO_VIRT((uint64_t)Address);
    switch (Width) {
        case 8:  *Value = *(volatile const uint8_t *)virt; break;
        case 16: *Value = *(volatile const uint16_t*)virt; break;
        case 32: *Value = *(volatile const uint32_t*)virt; break;
        case 64: *Value = *(volatile const uint64_t*)virt; break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width) {
    uintptr_t virt = (uintptr_t)PHYS_TO_VIRT((uint64_t)Address);
    switch (Width) {
        case 8:  *(volatile uint8_t *)virt = (uint8_t)Value; break;
        case 16: *(volatile uint16_t*)virt = (uint16_t)Value; break;
        case 32: *(volatile uint32_t*)virt = (uint32_t)Value; break;
        case 64: *(volatile uint64_t*)virt = (uint64_t)Value; break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width) {
    if (!Value) return AE_BAD_PARAMETER;
    switch (Width) {
        case 8:  *Value = inb((uint16_t)Address); break;
        case 16: *Value = inw((uint16_t)Address); break;
        case 32: *Value = inl((uint16_t)Address); break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width) {
    switch (Width) {
        case 8:  outb((uint16_t)Address, (uint8_t)Value); break;
        case 16: outw((uint16_t)Address, (uint16_t)Value); break;
        case 32: outl((uint16_t)Address, Value); break;
        default: return AE_BAD_PARAMETER;
    }
    return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void) {
    return acpi_local_find_rsdp();
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_TABLE_HEADER **NewTable) {
    (void)ExistingTable;
    if (NewTable) *NewTable = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *ExistingTable, ACPI_PHYSICAL_ADDRESS *NewAddress, UINT32 *NewTableLength) {
    (void)ExistingTable;
    if (NewAddress) *NewAddress = 0;
    if (NewTableLength) *NewTableLength = 0;
    return AE_OK;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *InitVal, ACPI_STRING *NewVal) {
    (void)InitVal;
    if (NewVal) *NewVal = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context) {
    (void)Type;
    if (Function) Function(Context);
    return AE_OK;
}

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context) {
    if (!ServiceRoutine) return AE_BAD_PARAMETER;
    if (InterruptNumber >= 256u) return AE_BAD_PARAMETER;
    g_acpi_irq_bindings[InterruptNumber].handler = ServiceRoutine;
    g_acpi_irq_bindings[InterruptNumber].context = Context;
    if (InterruptNumber < 16u) {
        irq_install_handler((int)InterruptNumber, acpi_os_irq_dispatch);
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine) {
    (void)ServiceRoutine;
    if (InterruptNumber < 256u) {
        g_acpi_irq_bindings[InterruptNumber].handler = NULL;
        g_acpi_irq_bindings[InterruptNumber].context = NULL;
    }
    return AE_OK;
}

ACPI_THREAD_ID AcpiOsGetThreadId(void) {
    return (ACPI_THREAD_ID)1u;
}

UINT64 AcpiOsGetTimer(void) {
    return (UINT64)get_timer_ticks() * 100000ULL;
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info) {
    (void)Info;
    if (Function == ACPI_SIGNAL_BREAKPOINT) {
        serial_puts("[ACPICA] breakpoint signal\n");
    } else if (Function == ACPI_SIGNAL_FATAL) {
        serial_puts("[ACPICA] fatal signal\n");
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue) {
    (void)SleepState;
    (void)RegaValue;
    (void)RegbValue;
    return AE_OK;
}

BOOLEAN AcpiOsReadable(void *Pointer, ACPI_SIZE Length) {
    return (Pointer != NULL && Length > 0u) ? TRUE : FALSE;
}

BOOLEAN AcpiOsWritable(void *Pointer, ACPI_SIZE Length) {
    return (Pointer != NULL && Length > 0u) ? TRUE : FALSE;
}

ACPI_STATUS AcpiOsGetTableByName(char *Signature, UINT32 Instance, ACPI_TABLE_HEADER **Table, ACPI_PHYSICAL_ADDRESS *Address) {
    (void)Signature;
    (void)Instance;
    if (Table) *Table = NULL;
    if (Address) *Address = 0;
    return AE_NOT_FOUND;
}

ACPI_STATUS AcpiOsGetTableByIndex(UINT32 Index, ACPI_TABLE_HEADER **Table, UINT32 *Instance, ACPI_PHYSICAL_ADDRESS *Address) {
    (void)Index;
    if (Table) *Table = NULL;
    if (Instance) *Instance = 0;
    if (Address) *Address = 0;
    return AE_NOT_FOUND;
}

ACPI_STATUS AcpiOsGetTableByAddress(ACPI_PHYSICAL_ADDRESS Address, ACPI_TABLE_HEADER **Table) {
    (void)Address;
    if (Table) *Table = NULL;
    return AE_NOT_FOUND;
}

void *AcpiOsOpenDirectory(char *Pathname, char *WildcardSpec, char RequestedFileType) {
    (void)Pathname;
    (void)WildcardSpec;
    (void)RequestedFileType;
    return NULL;
}

char *AcpiOsGetNextFilename(void *DirHandle) {
    (void)DirHandle;
    return NULL;
}

void AcpiOsCloseDirectory(void *DirHandle) {
    (void)DirHandle;
}

ACPI_STATUS AcpiOsInitializeDebugger(void) { return AE_OK; }
void AcpiOsTerminateDebugger(void) {}

ACPI_STATUS AcpiOsWaitCommandReady(void) { return AE_OK; }
ACPI_STATUS AcpiOsNotifyCommandComplete(void) { return AE_OK; }

void AcpiOsTracePoint(ACPI_TRACE_EVENT_TYPE Type, BOOLEAN Begin, UINT8 *Aml, char *Pathname) {
    (void)Type;
    (void)Begin;
    (void)Aml;
    (void)Pathname;
}

void AcpiOsRedirectOutput(void *Destination) {
    (void)Destination;
}

ACPI_STATUS AcpiOsGetLine(char *Buffer, UINT32 BufferLength, UINT32 *BytesRead) {
    if (Buffer && BufferLength) Buffer[0] = '\0';
    if (BytesRead) *BytesRead = 0;
    return AE_OK;
}

void AcpiOsWaitEventsComplete(void) {}

void AcpiOsSleep(UINT64 Milliseconds) {
    if (Milliseconds == 0u) return;
    timer_wait_ms(Milliseconds);
}

void AcpiOsStall(UINT32 Microseconds) {
    if (Microseconds == 0u) return;
    uint64_t ms = (Microseconds + 999u) / 1000u;
    if (ms == 0u) ms = 1u;
    timer_wait_ms(ms);
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 *Value, UINT32 Width) {
    if (!PciId || !Value) return AE_BAD_PARAMETER;
    uint64_t bus = PciId->Bus;
    uint64_t device = PciId->Device;
    uint64_t function = PciId->Function;

    switch (Width) {
        case 8: {
            uint64_t dword = pci_read_dword(bus, device, function, Reg & ~3u);
            *Value = (dword >> ((Reg & 3u) * 8u)) & 0xFFu;
            return AE_OK;
        }
        case 16:
            *Value = pci_read_word(bus, device, function, Reg);
            return AE_OK;
        case 32:
            *Value = pci_read_dword(bus, device, function, Reg);
            return AE_OK;
        case 64: {
            uint64_t lo = pci_read_dword(bus, device, function, Reg);
            uint64_t hi = pci_read_dword(bus, device, function, Reg + 4u);
            *Value = lo | (hi << 32u);
            return AE_OK;
        }
        default:
            return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 Value, UINT32 Width) {
    if (!PciId) return AE_BAD_PARAMETER;
    uint64_t bus = PciId->Bus;
    uint64_t device = PciId->Device;
    uint64_t function = PciId->Function;

    switch (Width) {
        case 8: {
            uint64_t existing = pci_read_dword(bus, device, function, Reg & ~3u);
            uint64_t shift = (Reg & 3u) * 8u;
            existing &= ~(0xFFULL << shift);
            existing |= ((Value & 0xFFu) << shift);
            pci_write_dword(bus, device, function, Reg & ~3u, existing);
            return AE_OK;
        }
        case 16:
            pci_write_word(bus, device, function, Reg, Value);
            return AE_OK;
        case 32:
            pci_write_dword(bus, device, function, Reg, Value);
            return AE_OK;
        case 64:
            pci_write_dword(bus, device, function, Reg, (UINT32)(Value & 0xFFFFFFFFu));
            pci_write_dword(bus, device, function, Reg + 4u, (UINT32)(Value >> 32u));
            return AE_OK;
        default:
            return AE_BAD_PARAMETER;
    }
}

static void acpi_local_log_bootstrap_failure(const char* stage, ACPI_STATUS status) {
    acpi_local_log_prefix(stage);
    serial_puts("failed (status=0x");
    serial_puthex((uint64_t)(uint32_t)status);
    serial_puts(")\n");
}

static ACPI_STATUS acpi_local_bootstrap(void) {
    if (g_acpica_bootstrapped) return AE_OK;

    /* ACPICA's diagnostic defaults can emit thousands of per-method AML
     * trace lines on ordinary firmware. Serial output is synchronous on this
     * kernel and would starve desktop startup and USB enumeration; retain our
     * concise error/status logging while disabling verbose subsystem tracing. */
    AcpiDbgLevel = 0;
    AcpiDbgLayer = 0;

    ACPI_STATUS status = AcpiInitializeSubsystem();
    if (ACPI_FAILURE(status)) {
        acpi_local_log_bootstrap_failure("InitializeSubsystem", status);
        return status;
    }

    status = AcpiInitializeTables(NULL, 16u, FALSE);
    if (ACPI_FAILURE(status)) {
        acpi_local_log_bootstrap_failure("InitializeTables", status);
        return status;
    }

    /* AcpiInitializeTables() already owns the initial root table list.
     * Reallocation is an optional growth operation and ACPICA returns
     * AE_NOT_EXIST on firmware/configurations where there is no detached
     * list to grow.  It must not prevent the standard LoadTables path. */
    status = AcpiReallocateRootTable();
    if (ACPI_FAILURE(status)) {
        acpi_local_log_prefix("ReallocateRootTable");
        serial_puts("not required; continuing (status=0x");
        serial_puthex((uint64_t)(uint32_t)status);
        serial_puts(")\n");
    }

    status = AcpiLoadTables();
    if (ACPI_FAILURE(status)) {
        acpi_local_log_bootstrap_failure("LoadTables", status);
        return status;
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        acpi_local_log_bootstrap_failure("EnableSubsystem", status);
        return status;
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (ACPI_FAILURE(status)) {
        acpi_local_log_bootstrap_failure("InitializeObjects", status);
        return status;
    }

    acpi_local_install_ec_support();
    acpi_local_install_notify_support();
    acpi_local_install_event_support();
    g_acpica_bootstrapped = true;
    return AE_OK;
}

static uint64_t acpi_local_integer_from_obj(const ACPI_OBJECT *obj) {
    if (!obj) return 0;
    switch (obj->Type) {
        case ACPI_TYPE_INTEGER: return obj->Integer.Value;
        case ACPI_TYPE_BUFFER:
            if (obj->Buffer.Length >= 1) return obj->Buffer.Pointer[0];
            return 0;
        case ACPI_TYPE_STRING:
            return (uint64_t)(uintptr_t)obj->String.Pointer;
        default:
            return 0;
    }
}

static ACPI_STATUS acpi_local_eval(const char* path, ACPI_OBJECT_LIST* args, ACPI_BUFFER* out) {
    if (!path || !out) return AE_BAD_PARAMETER;
    return AcpiEvaluateObject(NULL, (char*)path, args, out);
}

static bool acpi_local_eval_int_method(const char* path, const ACPI_OBJECT* args_in, UINT32 arg_count, uint64_t* out_val) {
    if (!path || !out_val) return false;
    ACPI_OBJECT args_storage[7];
    ACPI_OBJECT_LIST arg_list;
    ACPI_BUFFER out = { ACPI_ALLOCATE_BUFFER, NULL };

    if (arg_count > 7u) arg_count = 7u;
    for (UINT32 i = 0; i < arg_count; ++i) args_storage[i] = args_in[i];
    arg_list.Count = arg_count;
    arg_list.Pointer = args_storage;

    ACPI_STATUS st = acpi_local_eval(path, arg_count ? &arg_list : NULL, &out);
    if (ACPI_FAILURE(st) || !out.Pointer) return false;

    const ACPI_OBJECT* obj = (const ACPI_OBJECT*)out.Pointer;
    *out_val = acpi_local_integer_from_obj(obj);
    AcpiOsFree(out.Pointer);
    return true;
}

static bool acpi_local_eval_package_method(const char* path, const ACPI_OBJECT* args_in, UINT32 arg_count, uint64_t* first, uint64_t* second, uint64_t* third, uint64_t* fourth) {
    if (!path) return false;
    ACPI_OBJECT args_storage[7];
    ACPI_OBJECT_LIST arg_list;
    ACPI_BUFFER out = { ACPI_ALLOCATE_BUFFER, NULL };

    if (arg_count > 7u) arg_count = 7u;
    for (UINT32 i = 0; i < arg_count; ++i) args_storage[i] = args_in[i];
    arg_list.Count = arg_count;
    arg_list.Pointer = args_storage;

    ACPI_STATUS st = acpi_local_eval(path, arg_count ? &arg_list : NULL, &out);
    if (ACPI_FAILURE(st) || !out.Pointer) return false;

    const ACPI_OBJECT* root = (const ACPI_OBJECT*)out.Pointer;
    bool ok = false;
    if (root->Type == ACPI_TYPE_PACKAGE && root->Package.Count > 0) {
        const ACPI_OBJECT* elems = root->Package.Elements;
        if (first)  *first  = acpi_local_integer_from_obj(&elems[0]);
        if (second && root->Package.Count > 1) *second = acpi_local_integer_from_obj(&elems[1]);
        if (third  && root->Package.Count > 2) *third  = acpi_local_integer_from_obj(&elems[2]);
        if (fourth && root->Package.Count > 3) *fourth = acpi_local_integer_from_obj(&elems[3]);
        ok = true;
    }
    AcpiOsFree(out.Pointer);
    return ok;
}

bool acpi_power_acpica_initialize(void) {
    if (!acpi_power_acpica_available()) return false;
    ACPI_STATUS status = acpi_local_bootstrap();
    g_acpica_initialized = ACPI_SUCCESS(status);
    if (g_acpica_initialized) {
        acpi_local_log_prefix("initialized");
        serial_puts("event hooks active\n");
    }
    return g_acpica_initialized;
}

bool acpi_power_acpica_query_battery(uint64_t* percent, bool* charging, uint64_t* minutes) {
    if (!g_acpica_initialized) return false;

    bool notified = g_battery_notify_dirty || g_gpe_notify_dirty;
    g_battery_notify_dirty = false;
    g_gpe_notify_dirty = false;
    (void)notified;
    const char* const bst_methods[] = {
        "\\_SB.BAT0._BST",
        "\\_SB.PCI0.BAT0._BST",
        "\\_SB.PCI0.LPCB.BAT0._BST",
        "\\BAT0._BST",
        "\\_BST",
        NULL
    };
    const char* const bif_methods[] = {
        "\\_SB.BAT0._BIF",
        "\\_SB.PCI0.BAT0._BIF",
        "\\_SB.PCI0.LPCB.BAT0._BIF",
        "\\BAT0._BIF",
        "\\_BIF",
        NULL
    };

    for (uint32_t i = 0; bst_methods[i]; ++i) {
        uint64_t st = 0, rem = 0, rate = 0, volt = 0;
        if (acpi_local_eval_package_method(bst_methods[i], NULL, 0, &st, &rem, &rate, &volt)) {
            (void)volt;
            if (percent) *percent = rem > 100u ? 100u : rem;
            if (charging) *charging = (st & 0x1u) != 0u || (rate < 0x80000000ULL && rate != 0u);
            if (minutes) *minutes = rate ? (rem * 60u) / (rate ? rate : 1u) : 0u;
            return true;
        }
    }

    for (uint32_t i = 0; bif_methods[i]; ++i) {
        uint64_t design = 0, lastfull = 0, warn = 0, low = 0;
        if (acpi_local_eval_package_method(bif_methods[i], NULL, 0, &design, &lastfull, &warn, &low)) {
            (void)design; (void)warn; (void)low;
            if (percent) *percent = lastfull > 100u ? 100u : lastfull;
            if (charging) *charging = false;
            if (minutes) *minutes = 0u;
            return true;
        }
    }

    return false;
}

bool acpi_power_acpica_query_thermal(uint64_t* celsius, uint64_t* fan_rpm, uint64_t* cstate_count) {
    if (!g_acpica_initialized) return false;

    bool notified = g_thermal_notify_dirty || g_gpe_notify_dirty;
    g_thermal_notify_dirty = false;
    g_gpe_notify_dirty = false;
    (void)notified;

    if (celsius) *celsius = UINT64_MAX;
    if (fan_rpm) *fan_rpm = UINT64_MAX;
    if (cstate_count) *cstate_count = UINT64_MAX;

    const char* const thermal_methods[] = {
        "\\_TZ.TZ00._TMP", "\\_TZ.THRM._TMP", "\\_SB.TZ00._TMP", "\\_SB.THRM._TMP", "\\TZ00._TMP", "\\THRM._TMP", "\\_TMP", NULL
    };
    const char* const fan_methods[] = {
        "\\_TZ.FAN0._FST", "\\_SB.FAN0._FST", "\\_TZ.FAN0._FPS", "\\_SB.FAN0._FPS", "\\FAN0._FST", "\\FAN0._FPS", NULL
    };
    const char* const cstate_methods[] = {
        "\\_PR.CPU0._CST", "\\_SB.CPU0._CST", "\\CPU0._CST", "\\_CST", NULL
    };

    bool ok = false;
    for (uint32_t i = 0; thermal_methods[i]; ++i) {
        uint64_t raw = 0;
        if (acpi_local_eval_int_method(thermal_methods[i], NULL, 0, &raw)) {
            if (celsius) {
                if (raw >= 1000u) {
                    *celsius = (raw <= 2731u) ? 0u : (raw - 2732u) / 10u;
                } else {
                    *celsius = raw;
                }
            }
            ok = true;
            break;
        }
    }

    for (uint32_t i = 0; fan_methods[i]; ++i) {
        uint64_t a = 0, b = 0, c = 0, d = 0;
        if (acpi_local_eval_package_method(fan_methods[i], NULL, 0, &a, &b, &c, &d)) {
            if (fan_rpm) *fan_rpm = b ? b : a;
            ok = true;
            break;
        }
    }

    for (uint32_t i = 0; cstate_methods[i]; ++i) {
        uint64_t v = 0;
        if (acpi_local_eval_int_method(cstate_methods[i], NULL, 0, &v)) {
            if (cstate_count) *cstate_count = v;
            ok = true;
            break;
        }
    }

    return ok;
}

bool acpi_power_acpica_ec_is_available(void) {
    return g_ec_handler_installed || g_ec_ports_ready;
}

bool acpi_power_acpica_ec_read8(uint8_t reg, uint8_t* out) {
    if (!out) return false;
    return acpi_local_ec_port_read8(reg, out);
}

bool acpi_power_acpica_ec_write8(uint8_t reg, uint8_t value) {
    return acpi_local_ec_port_write8(reg, value);
}

bool acpi_power_acpica_suspend(uint8_t sleep_state) {
    if (!g_acpica_initialized) return false;
    ++g_suspend_request_count;
    acpi_local_log_sleep_state("suspend-request", sleep_state);
    ACPI_STATUS st = AcpiEnterSleepStatePrep(sleep_state);
    if (ACPI_FAILURE(st)) {
        acpi_local_log_prefix("suspend-prep-failed");
        serial_puthex((uint64_t)st);
        serial_puts("\n");
        return false;
    }
    st = AcpiEnterSleepState(sleep_state);
    if (ACPI_FAILURE(st)) {
        acpi_local_log_prefix("suspend-failed");
        serial_puthex((uint64_t)st);
        serial_puts("\n");
        return false;
    }
    return true;
}

bool acpi_power_acpica_resume(uint8_t sleep_state) {
    if (!g_acpica_initialized) return false;
    ++g_resume_request_count;
    acpi_local_log_sleep_state("resume-request", sleep_state);
    ACPI_STATUS st = AcpiLeaveSleepState(sleep_state);
    if (ACPI_FAILURE(st)) {
        acpi_local_log_prefix("resume-failed");
        serial_puthex((uint64_t)st);
        serial_puts("\n");
        return false;
    }
    acpi_local_log_prefix("resume-ok");
    serial_puts("suspend="); serial_putdec(g_suspend_request_count);
    serial_puts(" resume="); serial_putdec(g_resume_request_count);
    serial_puts(" gpe="); serial_putdec(g_gpe_event_count);
    serial_puts(" fixed="); serial_putdec(g_fixed_event_count);
    serial_puts("\n");
    return true;
}

#else  /* COS_ACPICA_PRESENT */

bool acpi_power_acpica_initialize(void) { return false; }
bool acpi_power_acpica_query_battery(uint64_t* percent, bool* charging, uint64_t* minutes) {
    (void)percent; (void)charging; (void)minutes;
    return false;
}
bool acpi_power_acpica_query_thermal(uint64_t* celsius, uint64_t* fan_rpm, uint64_t* cstate_count) {
    (void)celsius; (void)fan_rpm; (void)cstate_count;
    return false;
}
bool acpi_power_acpica_ec_is_available(void) { return false; }
bool acpi_power_acpica_ec_read8(uint8_t reg, uint8_t* out) { (void)reg; if (out) *out = 0; return false; }
bool acpi_power_acpica_ec_write8(uint8_t reg, uint8_t value) { (void)reg; (void)value; return false; }
bool acpi_power_acpica_suspend(uint8_t sleep_state) { (void)sleep_state; return false; }
bool acpi_power_acpica_resume(uint8_t sleep_state) { (void)sleep_state; return false; }

#endif /* COS_ACPICA_PRESENT */
