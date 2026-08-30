/**
 * kernel.c - Kernel Main Entry Point
 * C-OS 4.0.8 alpha - All Features Enabled
 */
#include <stdbool.h>
#include <stdint.h>

#include <types.h>
#include <serial.h>
#include <string.h>
#include <memory.h>
#include <memory_physical.h>
#include <gdt.h>
#include <idt.h>
#include <irq.h>
#include <timer.h>
#include <shell.h>
#include <../drivers/input/keyboard.h>
#include <mouse.h>
#include <vga.h>
#include <gui.h>
#include <boot_animation.h>
#include <calc_engine.h>
#include <fs.h>
#include <text_editor.h>
#include <rtc.h>
#include <password_screen.h>
#include <net.h>
#include "drivers/pci.h"
#include "drivers/ac97.h"
#include "drivers/usb.h"
#include "drivers/http.h"
#include "task.h"
#include "scheduler.h"
#include "smp.h"
#include "ipc.h"
#include "mm/paging.h"

#ifndef COS_ENABLE_FULL_DESKTOP
#define COS_ENABLE_FULL_DESKTOP 1
#endif

/* Real kernel stack for gui_main (see the thread_create_kernel_stack_size()
 * call site below for why): comfortably larger than what NetSurf's
 * layout/CSS pipeline and the QuickJS interpreter need for real-world pages,
 * while still a small, one-time, single-thread cost. Keep this in sync with
 * the JS_SetMaxStackSize() budget in cos_js_new_context() (quickjs_port.c),
 * which must stay safely below this value so QuickJS's own recursion guard
 * trips before this real stack is exhausted, not after. Deliberately placed
 * outside the ifndef above: the real build defines COS_ENABLE_FULL_DESKTOP
 * via -D, which skips that whole guarded block, and this constant must exist
 * either way. */
#define GUI_MAIN_STACK_SIZE (512u * 1024u)

#ifndef COS_HTTP_RUNTIME_SMOKE
#define COS_HTTP_RUNTIME_SMOKE 0
#endif

#ifndef COS_ENABLE_NETWORK
/* E1000 DMA descriptors and packet buffers now use page-backed, physically
 * resolvable allocations.  Keep the network stack on by default; deployments
 * can still override COS_ENABLE_NETWORK=0 at build time for isolation. */
#define COS_ENABLE_NETWORK 1
#endif

/* External declarations */
extern bool storage_init(void);
extern void text_editor_init(void);
extern void mk_advanced_filemanager_init(void);
void gdt_init(void);
void idt_init(void);
void irq_init(void);
void timer_init(void);
void keyboard_init(void);
void keyboard_poll(void);
void minimal_mouse_init(void);
void minimal_mouse_poll(void);
void vga_init(uint64_t multiboot_magic, uint64_t multiboot_info_addr);
void gui_init(void);
bool gui_is_initialized(void);
void gui_update(void);
void shell_init(void);
void shell_apply_config_snapshot(void);
void net_init(void);
void net_poll(void);
void pci_init(void);
void syscall_init(void);
void usb_init(void);
void usb_poll(void);
int storage_manager_init(void);
int storage_sync(void);
int config_manager_init(void);
void cos_power_init(void);

static inline void cli(void) { __asm__ __volatile__("cli"); }
static inline void sti(void) { __asm__ __volatile__("sti"); }
static inline void cpu_hlt(void) { __asm__ __volatile__("hlt"); }
static inline void cpu_idle(void) { __asm__ __volatile__("sti; hlt"); }

/* The GUI update loop, now running as an actual scheduled kernel
 * thread rather than directly on the boot stack. Functionally
 * identical to the loop this replaces; the only change is *where* it
 * runs.
 *
 * thread_yield() is required here, not optional: cpu_idle() (sti;hlt)
 * only pauses until the next interrupt and then resumes this same
 * loop - it never hands the CPU to the scheduler. Under the old
 * always-on preemptive scheduler that didn't matter, because
 * scheduler_tick() would eventually force a switch away once this
 * thread's time slice ran out. In cooperative mode there is no such
 * forced switch, so without an explicit yield this loop would run
 * forever and every other thread (demo_heartbeat_thread, the
 * notification GC thread, and anything the desktop init is waiting
 * on) would starve - which is exactly what produced the "stuck on
 * loading desktop" hang. */
static void gui_main_thread(void* arg) {
    (void)arg;
    bool desktop_frame_presented = false;
    /* Run input, invalidation and frame pacing on every scheduled pass.
     * gui_update() itself enforces the 16ms presentation cadence, so this
     * must not add a second modulo-based 20 FPS cap above it.  Explicitly
     * yielding still leaves CPU time for preemptive network, USB and service
     * threads between GUI passes. */
    for (;;) {
        #if COS_ENABLE_NETWORK
        net_poll();
        #endif
        usb_poll();
        gui_update();
        /* The first completed GUI update is the safe hand-off point for
         * VirtualBox: its EFI/VGA path is visibly alive before any deferred
         * INIT/SIPI traffic is issued. Other platforms have already started
         * APs in smp_init(), so this call is a no-op there. */
        if (!desktop_frame_presented) {
            desktop_frame_presented = true;
            smp_start_deferred_workers();
        }
        thread_yield();
    }
}

/* Minimal demo thread with no purpose other than proving the
 * preemptive scheduler is genuinely switching between independent
 * threads: it wakes up on its own schedule (via thread_sleep, which
 * blocks this thread specifically rather than halting the CPU) and
 * logs a heartbeat to the serial console. If multitasking is working,
 * this keeps ticking on its own cadence while the GUI thread is busy
 * doing its own thing. */
/* Enable only in a dedicated validation build. The probe starts a worker
 * that never yields for 200 timer ticks, then verifies that a second ready
 * worker observed time *before* the spin worker completed. A cooperative
 * scheduler cannot satisfy that condition: the observer runs only after the
 * spinning worker leaves the CPU. */
#ifndef COS_SCHED_PREEMPTION_PROBE
#define COS_SCHED_PREEMPTION_PROBE 0
#endif
#if COS_SCHED_PREEMPTION_PROBE
static volatile uint64_t preempt_probe_start_tick;
static volatile uint64_t preempt_probe_end_tick;
static volatile uint64_t preempt_probe_observer_tick;
static volatile bool preempt_probe_started;

static void preempt_probe_spinner(void* arg) {
    (void)arg;
    preempt_probe_start_tick = get_timer_ticks();
    preempt_probe_started = true;
    uint64_t deadline = preempt_probe_start_tick + 200u;
    while (get_timer_ticks() < deadline) {
        __asm__ volatile("pause");
    }
    preempt_probe_end_tick = get_timer_ticks();
}

static void preempt_probe_observer(void* arg) {
    (void)arg;
    while (!preempt_probe_started) {
        __asm__ volatile("pause");
    }
    preempt_probe_observer_tick = get_timer_ticks();
}

static void preempt_probe_verifier(void* arg) {
    (void)arg;
    thread_sleep(700u);
    bool passed = preempt_probe_started && preempt_probe_end_tick != 0 &&
                  preempt_probe_observer_tick >= preempt_probe_start_tick &&
                  preempt_probe_observer_tick < preempt_probe_end_tick;
    serial_puts(passed ? "[SCHED] PREEMPTION-PROBE PASS: observer ran during CPU-bound no-yield worker\\n"
                       : "[SCHED] PREEMPTION-PROBE FAIL: observer did not run before CPU-bound worker ended\\n");
    serial_puts("[SCHED] PREEMPTION-PROBE ticks start/observer/end=");
    serial_putdec(preempt_probe_start_tick); serial_puts("/");
    serial_putdec(preempt_probe_observer_tick); serial_puts("/");
    serial_putdec(preempt_probe_end_tick); serial_puts("\n");
}

static bool preempt_probe_start(void) {
    preempt_probe_start_tick = 0;
    preempt_probe_end_tick = 0;
    preempt_probe_observer_tick = 0;
    preempt_probe_started = false;
    return thread_create_kernel("preempt_spin", (void*)preempt_probe_spinner, NULL) != NULL &&
           thread_create_kernel("preempt_observe", (void*)preempt_probe_observer, NULL) != NULL &&
           thread_create_kernel("preempt_verify", (void*)preempt_probe_verifier, NULL) != NULL;
}
#endif

#if COS_HTTP_RUNTIME_SMOKE
/* This worker is intentionally compiled only into a validation image.  It
 * executes the same http_get() path used by NetSurf after GUI/scheduler setup,
 * allowing strict QEMU evidence for TLS ALPN h2, nghttp2 framing and Brotli
 * body decoding without making a network request part of normal boot. */
#define HTTP_PARALLEL_SMOKE_WORKERS 2u
static volatile uint32_t http_parallel_smoke_done;
static volatile uint32_t http_parallel_smoke_passed;

static void http_parallel_smoke_worker(void* arg) {
    uint64_t worker_id = (uint64_t)(uintptr_t)arg;
    const char *url = "https://www.cloudflare.com/cdn-cgi/trace";
    http_client_t *http = http_create();
    int passed = 0;
    if (http != NULL) {
        int rc = http_get(http, url);
        const char *encoding = rc == 0 ? http_get_header(http, "Content-Encoding") : NULL;
        passed = rc == 0 && http->used_http2 && http_status_code(http) == HTTP_OK &&
                 encoding != NULL && strncmp(encoding, "br", 2) == 0;
        serial_puts("[HTTP-TEST] parallel worker=");
        serial_putdec(worker_id);
        serial_puts(passed ? " PASS\n" : " FAIL\n");
        http_destroy(http);
    } else {
        serial_puts("[HTTP-TEST] parallel worker create failed\n");
    }
    if (passed) __atomic_fetch_add(&http_parallel_smoke_passed, 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&http_parallel_smoke_done, 1u, __ATOMIC_RELEASE);
}

static void http_runtime_smoke_thread(void* arg) {
    (void)arg;
    serial_puts("[HTTP-TEST] waiting for GUI/E1000/DHCP readiness\n");
    uint64_t ready_deadline = get_timer_ticks() + 3500u;
    while (get_timer_ticks() < ready_deadline) thread_yield();

    http_parallel_smoke_done = 0u;
    http_parallel_smoke_passed = 0u;
    serial_puts("[HTTP-TEST] starting two concurrent HTTPS requests\n");
    for (uint64_t i = 0; i < HTTP_PARALLEL_SMOKE_WORKERS; ++i) {
        thread_t *worker = thread_create_kernel_stack_size(
            i == 0u ? "http_smoke0" : "http_smoke1",
            (void*)http_parallel_smoke_worker, (void*)(uintptr_t)i,
            GUI_MAIN_STACK_SIZE);
        if (worker == NULL) {
            serial_puts("[HTTP-TEST] FAIL: parallel worker creation\n");
            return;
        }
    }

    uint64_t deadline = get_timer_ticks() + 30000u;
    while (__atomic_load_n(&http_parallel_smoke_done, __ATOMIC_ACQUIRE) <
               HTTP_PARALLEL_SMOKE_WORKERS &&
           get_timer_ticks() < deadline) {
        thread_yield();
    }
    uint32_t done = __atomic_load_n(&http_parallel_smoke_done, __ATOMIC_ACQUIRE);
    uint32_t passed = __atomic_load_n(&http_parallel_smoke_passed, __ATOMIC_ACQUIRE);
    uint32_t active = 0;
    uint32_t peak = 0;
    http_get_transport_stats(&active, &peak);
    if (done == HTTP_PARALLEL_SMOKE_WORKERS && passed == HTTP_PARALLEL_SMOKE_WORKERS &&
        peak == HTTP_PARALLEL_SMOKE_WORKERS && active == 0u) {
        serial_puts("[HTTP-TEST] PASS: two concurrent HTTP/2+Brotli requests; peak=");
        serial_putdec(peak);
        serial_puts("\n");
    } else {
        serial_puts("[HTTP-TEST] FAIL: parallel complete=");
        serial_putdec(done);
        serial_puts(" passed=");
        serial_putdec(passed);
        serial_puts(" active=");
        serial_putdec(active);
        serial_puts(" peak=");
        serial_putdec(peak);
        serial_puts("\n");
    }
}

static void http_runtime_smoke_start(void) {
    if (!thread_create_kernel_stack_size("http_h2_br_smoke",
                                         (void*)http_runtime_smoke_thread,
                                         NULL, GUI_MAIN_STACK_SIZE)) {
        serial_puts("[HTTP-TEST] FAIL: worker creation\n");
    }
}
#endif

static void demo_heartbeat_thread(void* arg) {
    (void)arg;
    /* The original version logged a 3-line serial message every second on
     * its own thread. The pre-emptive scheduler in C-OS 4.0.7 happily ran
     * this thread concurrently with the GUI paint thread, so every GUI
     * frame was racing against 3 serial_puts calls. Under QEMU/VirtualBox
     * the serial port is the dominant bottleneck, so the heartbeat was a
     * large fraction of the "heavy" feel of the OS. We keep the thread
     * around (it still proves that the scheduler is switching between
     * independent threads), but only emit the announcement message once,
     * very early, and then drop into a near-silent 30-second cadence so
     * it never contends with anything else. */
    uint64_t beat = 0;
    for (;;) {
        thread_sleep(30 * 1000);
        beat++;
        if (beat == 1) {
            serial_puts("[DEMO] heartbeat thread tick #");
            serial_putdec(beat);
            serial_puts("\n");
        }
    }
}

static void kernel_discard_thread(thread_t** thread) {
    if (!thread || !*thread) return;
    thread_destroy(*thread);
    *thread = NULL;
}

static bool kernel_graphics_ready(void) {
    return framebuffer != NULL && SCREEN_W > 0 && SCREEN_H > 0;
}

static void kernel_draw_status_screen(const char* headline, const char* detail) {
    if (!kernel_graphics_ready()) {
        return;
    }

    vga_clear(0x000A1020);
    vga_fill_rect(0, 0, (int)SCREEN_W, 48, 0x00203090);
    vga_fill_rect(0, 48, (int)SCREEN_W, 3, 0x00FFFFFF);
    vga_draw_string(20, 14, headline ? headline : "C-OS 4.0.8 alpha", 0x00FFFFFF, 0x00203090);
    vga_draw_string(20, 74, detail ? detail : "Serial debug console active", 0x00D8E8FF, 0x000A1020);
    vga_draw_string(20, 104, "Fallbacks: serial debug console, status overlay, panic screen", 0x00C0FFC0, 0x000A1020);
    vga_draw_string(20, 134, "Use the serial log if the GUI stays black.", 0x00FFFFFF, 0x000A1020);
}

static void kernel_draw_panic_overlay(const char* reason) {
    if (!kernel_graphics_ready()) {
        return;
    }

    vga_clear(0x00000000);
    vga_fill_rect(0, 0, (int)SCREEN_W, (int)SCREEN_H, 0x00200000);
    vga_fill_rect(0, 0, (int)SCREEN_W, 56, 0x00600000);
    vga_fill_rect(0, 56, (int)SCREEN_W, 2, 0x00FFFFFF);
    vga_draw_string(20, 16, "C-OS 4.0.8 alpha - KERNEL PANIC", 0x00FFFFFF, 0x00600000);
    vga_draw_string(20, 84, reason ? reason : "unknown error", 0x00FFFFFF, 0x00200000);
    vga_draw_string(20, 114, "Serial console still carries the full log.", 0x00FFD0D0, 0x00200000);
    vga_draw_string(20, 144, "This screen is the fallback overlay.", 0x00FFD0D0, 0x00200000);
}


static void kernel_fatal(const char* reason) {
    serial_puts("[KERNEL] FATAL: ");
    serial_puts(reason ? reason : "unknown");
    serial_puts("\n");
    kernel_draw_panic_overlay(reason);
    cli();
    for (;;) {
        cpu_hlt();
    }
}

/* =====================================================================
 * Multiboot memory-map parser — used to seed cos_runtime_memory_init()
 * with the RAM the host actually gave us (VirtualBox / QEMU / bare HW).
 *
 * Robustness notes:
 *  - mb1 and mb2 magics are both accepted; if neither yields a non-zero
 *    total we fall back to a build-time tunable rather than a hard-coded
 *    512 MiB so the user always sees the value the hypervisor reported.
 *  - The mb2 mmap walker validates every entry pointer, the tag length
 *    and the entry_size to avoid out-of-bounds reads when GRUB hands us
 *    a truncated info block.
 *  - "total" now means the sum of every entry's length, but we make it
 *    sane: readable RAM below 4 KiB or with non-zero reserved high bits
 *    is discarded. Available RAM is the sum of entries whose type==1.
 * ===================================================================== */

#define MULTIBOOT1_MAGIC             0x2BADB002u
#define MULTIBOOT1_LOAD_MAGIC        0x1BADB002u
#define MULTIBOOT2_MAGIC             0x36D76289u
#define MULTIBOOT2_TAG_END           0u
#define MULTIBOOT2_TAG_MMAP          6u
#define MULTIBOOT2_TAG_BASIC_MEMINFO 4u
#define MULTIBOOT2_TAG_ACPI_OLD_RSDP 14u
#define MULTIBOOT2_TAG_ACPI_NEW_RSDP 15u
#define MULTIBOOT1_MMAP_TYPE_AVAIL   1u

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_header_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed)) mb2_mmap_tag_t;

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) mb2_mmap_entry_t;

/* `mb_total_bytes` is the highest end address of RAM or firmware memory
 * that C-OS may need to access through PHYS_TO_VIRT(). It must never be
 * the sum of arbitrary reserved/MMIO descriptors: modern q35 exposes large
 * PCI windows above RAM, and treating those as a contiguous direct-map range
 * exhausts early page-table memory before the GUI can start. */
static uint64_t mb_total_bytes   = 0;
static uint64_t mb_avail_bytes   = 0;
static uint64_t mb_direct_map_extent = 0;
static uint64_t mb2_info_phys    = 0;
static uint64_t mb2_info_size    = 0;

/* Physical address of the ACPI RSDP as handed to us by the bootloader via
   a Multiboot2 tag (type 14 = old/ACPI 1.0 RSDP, type 15 = new/ACPI 2.0+
   XSDP). GRUB fills this tag in identically whether it itself was started
   by legacy BIOS or by UEFI firmware -- under UEFI, GRUB gets the pointer
   from the EFI configuration table (ACPI_20_TABLE_GUID / ACPI_TABLE_GUID)
   instead of scanning the BIOS EBDA, and simply forwards it to us the same
   way. Consuming this tag means C-OS does not need to know or care which
   firmware it was booted under to find ACPI tables; the EBDA/0xE0000-
   0xFFFFF scan in acpi_power.c remains only as a fallback for the rare
   case a loader doesn't supply this tag at all. */
static uint64_t mb2_acpi_rsdp_phys = 0;

uint64_t cos_mb2_get_acpi_rsdp(void) {
    return mb2_acpi_rsdp_phys;
}

/* Build-time default for hosts that ship an empty multiboot info block.
   Match the value the user passed to QEMU (-m NN) or VirtualBox. The
   environment variable C_OS_RAM_MB overrides this at boot if set. */
#ifndef C_OS_RAM_FALLBACK_MB
#define C_OS_RAM_FALLBACK_MB 512ULL
#endif

static void parse_multiboot1_upper(uint64_t addr) {
    if (addr == 0) return;
    /* Multiboot 1 info layout:
       offset 0  : flags
       offset 4  : mem_lower (KiB)
       offset 8  : mem_upper (KiB)
    */
    volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)addr;
    uint32_t flags    = p[0];
    uint32_t mem_low  = (flags & 0x00000001u) ? p[1] : 0u;
    uint32_t mem_up   = (flags & 0x00000001u) ? p[2] : 0u;
    uint64_t total = ((uint64_t)mem_low + mem_up) * 1024ULL;
    uint64_t avail = total;
    /* Reserve the conventional BIOS area + kernel reservation (16 MiB). */
    if (avail > 16ULL * 1024 * 1024) avail -= 16ULL * 1024 * 1024;
    mb_total_bytes = total;
    mb_avail_bytes = avail;
}

static void parse_multiboot2_mmap(uint64_t addr) {
    if (addr == 0) return;
    /* The mb2 info structure starts with total_size (u32) followed by the
       reserved u32; tags begin at +8. */
    uint32_t total_size = *(volatile uint32_t*)(uintptr_t)addr;
    if (total_size < 8u || total_size > 0x100000u) return; /* sanity */
    mb2_info_phys = addr;
    mb2_info_size = total_size;
    uint8_t* base = (uint8_t*)(uintptr_t)addr + 8;
    uint8_t* end  = (uint8_t*)(uintptr_t)addr + total_size;
    while (base + sizeof(mb2_tag_header_t) <= end) {
        mb2_tag_header_t* hdr = (mb2_tag_header_t*)base;
        if (hdr->type == MULTIBOOT2_TAG_END) break;
        if (hdr->size < sizeof(mb2_tag_header_t)) break; /* malformed */
        if (hdr->type == MULTIBOOT2_TAG_MMAP) {
            mb2_mmap_tag_t* mtag = (mb2_mmap_tag_t*)base;
            if (!mtag->entry_size || mtag->entry_size < sizeof(mb2_mmap_entry_t))
                mtag->entry_size = sizeof(mb2_mmap_entry_t);
            uint8_t* tag_end = base + mtag->size;
            if (tag_end > end) tag_end = end;
            uint8_t* entry_ptr = (uint8_t*)base + sizeof(mb2_mmap_tag_t);
            serial_puts("[MMAP] Multiboot2 memory map:\n");
            int region_idx = 0;
            while (entry_ptr + mtag->entry_size <= tag_end) {
                mb2_mmap_entry_t* e = (mb2_mmap_entry_t*)entry_ptr;

                const char* type_name;
                switch (e->type) {
                    case 1: type_name = "Available"; break;
                    case 2: type_name = "Reserved"; break;
                    case 3: type_name = "ACPI reclaimable"; break;
                    case 4: type_name = "ACPI NVS"; break;
                    case 5: type_name = "Bad RAM"; break;
                    default: type_name = "Unknown"; break;
                }
                serial_puts("[MMAP]  [");
                serial_putdec((uint64_t)region_idx++);
                serial_puts("] base=0x");
                serial_puthex(e->base_addr);
                serial_puts(" length=0x");
                serial_puthex(e->length);
                serial_puts(" (");
                serial_putdec(e->length / 1024);
                serial_puts(" KiB) type=");
                serial_putdec((uint64_t)e->type);
                serial_puts(" (");
                serial_puts(type_name);
                serial_puts(")");
                if (e->reserved != 0) {
                    serial_puts(" [SKIPPED: reserved field non-zero, treated as corrupt]");
                } else if (e->length == 0) {
                    serial_puts(" [SKIPPED: zero length]");
                } else if (e->length > (1ULL << 40)) {
                    serial_puts(" [SKIPPED: length >= 1TiB, likely corrupt]");
                } else if (e->type == MULTIBOOT1_MMAP_TYPE_AVAIL) {
                    serial_puts(" [counted in total AND available]");
                } else {
                    serial_puts(" [counted in total only]");
                }
                serial_puts("\n");

                /* Only RAM and ACPI firmware regions contribute to the
                 * PHYS_TO_VIRT direct-map high-water mark. Reserved PCI/MMIO
                 * apertures can legally live many GiB above RAM on q35; adding
                 * their lengths or endpoints here causes a pathological map
                 * attempt into non-RAM space. The direct map remains
                 * contiguous below the selected high-water mark, preserving
                 * access to holes and ACPI tables inside the actual RAM span. */
                if (e->length && e->length <= (1ULL << 40) && e->reserved == 0 &&
                    (e->type == MULTIBOOT1_MMAP_TYPE_AVAIL || e->type == 3u || e->type == 4u)) {
                    uint64_t region_end = e->base_addr + e->length;
                    if (region_end >= e->base_addr) {
                        if (region_end > mb_total_bytes) mb_total_bytes = region_end;
                        if (region_end > mb_direct_map_extent) mb_direct_map_extent = region_end;
                    }
                    if (e->type == MULTIBOOT1_MMAP_TYPE_AVAIL)
                        mb_avail_bytes += e->length;
                }
                entry_ptr += mtag->entry_size;
            }
            serial_puts("[MMAP] running total after this tag: total=0x");
            serial_puthex(mb_total_bytes);
            serial_puts(" available=0x");
            serial_puthex(mb_avail_bytes);
            serial_puts("\n");
        } else if (hdr->type == MULTIBOOT2_TAG_BASIC_MEMINFO) {
            /* Fallback path when GRUB didn't pack an mmap tag.
               mb2 basic meminfo has mem_lower at +8 and mem_upper at +12
               (both in KiB). */
            volatile uint32_t* p = (volatile uint32_t*)(uintptr_t)base;
            uint32_t lo = p[2];
            uint32_t hi = p[3];
            uint64_t total = ((uint64_t)lo + hi) * 1024ULL;
            if (total > mb_total_bytes) {
                mb_total_bytes = total;
                uint64_t avail = total;
                if (avail > 16ULL * 1024 * 1024) avail -= 16ULL * 1024 * 1024;
                if (avail > mb_avail_bytes) mb_avail_bytes = avail;
            }
        } else if (hdr->type == MULTIBOOT2_TAG_ACPI_OLD_RSDP ||
                   hdr->type == MULTIBOOT2_TAG_ACPI_NEW_RSDP) {
            /* Tag layout: header (8 bytes) followed immediately by the raw
               RSDP structure copied in by the bootloader. We only need its
               address; acpi_power.c re-validates the signature/checksum
               itself before trusting it. Prefer the "new" (ACPI 2.0+,
               type 15) RSDP if both tags are present, since it also
               carries the XSDT pointer. */
            uint8_t* rsdp_ptr = base + sizeof(mb2_tag_header_t);
            if (hdr->type == MULTIBOOT2_TAG_ACPI_NEW_RSDP || mb2_acpi_rsdp_phys == 0) {
                mb2_acpi_rsdp_phys = (uint64_t)(uintptr_t)rsdp_ptr;
            }
        }
        uint32_t step = (hdr->size + 7u) & ~7u;
        if (step < sizeof(mb2_tag_header_t)) step = sizeof(mb2_tag_header_t);
        base += step;
    }
}

static void kernel_capture_runtime_memory(uint64_t magic, uint64_t addr) {
    mb_total_bytes = 0;
    mb_avail_bytes = 0;
    mb_direct_map_extent = 0;
    mb2_info_phys = 0;
    mb2_info_size = 0;
    mb2_acpi_rsdp_phys = 0;
    if (magic == MULTIBOOT2_MAGIC) {
        parse_multiboot2_mmap(addr);
    } else if (magic == MULTIBOOT1_MAGIC || magic == MULTIBOOT1_LOAD_MAGIC) {
        parse_multiboot1_upper(addr);
    }
    if (mb_total_bytes == 0) {
        /* The bootloader did not give us a usable map. Stay consistent
           with whatever the user launches us with, instead of inventing
           a hard-coded 512 MiB that hides shortfalls. */
        uint64_t fb_mb = (uint64_t)C_OS_RAM_FALLBACK_MB;
        mb_total_bytes = fb_mb * 1024ULL * 1024ULL;
        mb_avail_bytes = mb_total_bytes;
        mb_direct_map_extent = mb_total_bytes;
    }
    /* Clip to a sane cap so a malformed map cannot exhaust the heap. */
    if (mb_total_bytes > (1ULL << 40)) mb_total_bytes = 1ULL << 40;
    if (mb_direct_map_extent == 0 || mb_direct_map_extent > (1ULL << 40))
        mb_direct_map_extent = mb_total_bytes;
    if (mb_avail_bytes > mb_total_bytes) mb_avail_bytes = mb_total_bytes;
    serial_puts("[KERNEL] multiboot parse: total=");
    serial_putdec(mb_total_bytes / (1024 * 1024));
    serial_puts(" MiB, available=");
    serial_putdec(mb_avail_bytes / (1024 * 1024));
    serial_puts(" MiB\n");
    cos_runtime_memory_init(mb_total_bytes, mb_avail_bytes);
    cos_runtime_memory_set_direct_map_extent(mb_direct_map_extent);
}

static void kernel_reserve_bootloader_regions(void) {
    if (mb2_info_phys && mb2_info_size) {
        serial_puts("[PHYS] Reserving Multiboot2 information region\n");
        phys_memory_reserve_range((phys_addr_t)mb2_info_phys, mb2_info_size);
        serial_puts("[PHYS] Multiboot2 information region reserved\n");
    }
}

/* Track what has been initialized */
static bool init_storage_done = false;
static bool init_vga_done = false;
static bool init_gui_done = false;
static bool init_shell_done = false;

void kernel_main(uint64_t magic, uint64_t addr) {
    serial_init();
    serial_puts("\n[KERNEL] ================================\n");
    serial_puts("[KERNEL] C-OS 4.0.8 alpha (64-bit)\n");
    serial_puts("[KERNEL] Boot path: VirtualBox-compatible full mode\n");
    serial_puts("[KERNEL] Multiboot magic=0x");
    serial_puthex(magic);
    serial_puts("  info=0x");
    serial_puthex(addr);
    serial_puts("\n");
    serial_puts("[KERNEL] ================================\n");

    /* Accept both Multiboot1 and Multiboot2 entry conventions. */
    if (magic != 0x2BADB002ULL &&
        magic != 0x1BADB002ULL &&
        magic != 0x36D76289ULL) {
        kernel_fatal("Invalid multiboot magic");
    }

    if (addr == 0) {
        /* No info block at all — degrade gracefully using the build-time
           fallback value instead of crashing on a toolchain that strips
           the multiboot header. */
        serial_puts("[KERNEL] WARNING: multiboot info missing, using fallback RAM\n");
        kernel_capture_runtime_memory(MULTIBOOT2_MAGIC, 0);
    } else {
        kernel_capture_runtime_memory(magic, addr);
    }

    cli();

    serial_puts("[KERNEL] Initializing GDT...\n");
    gdt_init();

    serial_puts("[KERNEL] Initializing IDT...\n");
    idt_init();
    syscall_init();

    serial_puts("[KERNEL] Initializing IRQ...\n");
    irq_init();

    serial_puts("[KERNEL] Initializing timer...\n");
    timer_init();

    serial_puts("[KERNEL] Initializing physical memory manager...\n");
    phys_memory_init();
    kernel_reserve_bootloader_regions();

    serial_puts("[KERNEL] Detecting VM/HW memory from multiboot...\n");

    serial_puts("[KERNEL] Initializing memory...\n");
    memory_init();

    /* NOTE: paging_init() was never being called anywhere in this
     * codebase before this change. Without it, kernel_pml4/
     * current_directory in paging.c stay NULL forever, which means:
     *   - paging_create_directory() (used by every process) would
     *     clone nothing (its "if (kernel_pml4) memcpy(...)" guard
     *     silently skips), producing a directory with zero mappings.
     *   - The scheduler's new per-process CR3 switch would load that
     *     empty directory and crash immediately on the very first
     *     context switch (no mappings at all for the next instruction
     *     fetch).
     * It must run after memory_init() (its page tables are allocated
     * via kmalloc_aligned, which needs the heap) and before task_init()
     * (which creates the idle process/thread and therefore the first
     * real page directory).
     */
    serial_puts("[KERNEL] Initializing paging...\n");
    paging_init();

    serial_puts("[KERNEL] Initializing VGA/Framebuffer...\n");
    vga_init(magic, addr);
    init_vga_done = true;
    vga_reserve_physical_regions();

    if (kernel_graphics_ready()) {
        serial_puts("[KERNEL] Drawing framebuffer status screen...\n");
        kernel_draw_status_screen("C-OS 4.0.8 alpha", "Framebuffer graphics ready");
        vga_flip();
        serial_puts("[KERNEL] Framebuffer status screen drawn\n");

        /* timer_wait() relies on IRQ0 advancing timer_ticks. The
         * boot splash is still part of the early init path here, so
         * enable interrupts temporarily before the splash delay.
         * scheduler_running is still false at this point, so the timer
         * ISR will not hand control to the scheduler yet. */
        // Interrupts will be enabled after scheduler and tasking are fully initialized.
        // timer_wait(75); // Moved to after scheduler init for safety
    } else {
        serial_puts("[KERNEL] WARNING: no usable framebuffer; screen will stay black."
                    " The bootloader/BIOS did not provide a 32/24bpp linear framebuffer"
                    " (see the [VGA] lines above for what it offered instead)."
                    " Under Bochs this almost always means VBE isn't enabled on the VGA"
                    " card - add 'vga: extension=vbe' to your bochsrc. Under QEMU/VirtualBox"
                    " this is rare; check your display device settings if it happens there.\n");
    }

    serial_puts("[KERNEL] Running rich boot animation...\n");
    gui_boot_animation_run();

    serial_puts("[KERNEL] Discovering SMP topology...\n");
    smp_init();
    serial_puts("[KERNEL] Initializing scheduler...\n");
    scheduler_init();

    serial_puts("[KERNEL] Initializing tasking (idle process/thread)...\n");
    task_init();

    // Enable interrupts after scheduler and tasking are ready to handle them.
    serial_puts("[KERNEL] Enabling interrupts after scheduler/tasking init...\n");
    sti();

    extern void spawn_ring3_demo_process(void);
    spawn_ring3_demo_process();

    // Now that interrupts are enabled and scheduler is running, we can safely wait.
    if (init_vga_done) {
        timer_wait(75);
    }

    serial_puts("[KERNEL] Initializing IPC...\n");
    ipc_init();

    serial_puts("[KERNEL] Initializing RTC...\n");
    rtc_init();

    serial_puts("[KERNEL] Initializing PCI bus...\n");
    pci_init();

    serial_puts("[KERNEL] Initializing AC97 audio...\n");
    ac97_init();
    if (ac97_is_available()) {
        /* Self-test tone: if this is audible, the whole AC97 output
         * path (codec reset, DMA ring, hardware) is confirmed working
         * before anything more complex (WAV, then MP3) is trusted to it. */
        ac97_beep(880, 150);
    }

    serial_puts("[KERNEL] Initializing USB stack...\n");
    usb_init();

    #if COS_ENABLE_NETWORK
    serial_puts("[KERNEL] Initializing network stack...\n");
    net_init();
    #else
    serial_puts("[KERNEL] Network stack disabled for stability.\n");
    #endif

    serial_puts("[KERNEL] Initializing mouse (PS/2)...\n");
    minimal_mouse_init();

    serial_puts("[KERNEL] Initializing persistent storage...\n");
    if (!storage_init()) {
        serial_puts("[KERNEL] WARNING: persistent storage unavailable; RAM fallback remains active\n");
    } else {
        init_storage_done = true;
    }

    /* Mounts the real FAT32 partition (see fs.c/fatfs_diskio.h) so
     * fs_list_dir()/fs_read_file_at()/etc. - already called throughout
     * the GUI (file manager, text editor, ...) - actually have a
     * filesystem to talk to. fs_init() handles a missing/unformatted
     * disk gracefully on its own (formats it, or logs and leaves
     * g_fatfs_mounted false), so this is safe to call unconditionally. */
    serial_puts("[KERNEL] Mounting FAT32 filesystem...\n");
    fs_init();

    serial_puts("[KERNEL] Initializing storage manager...\n");
    (void)storage_manager_init();

    serial_puts("[KERNEL] Initializing persistent config manager...\n");
    (void)config_manager_init();
    (void)calc_engine_init();

    /* Initialize Security & Permissions before login screen */
    extern int permission_manager_init(void);
    serial_puts("[KERNEL] Initializing security systems...\n");
    permission_manager_init();

    serial_puts("[KERNEL] Initializing keyboard (PS/2)...\n");
    keyboard_init();

// Interrupts already enabled after scheduler/tasking init.

    /* NOTE: The boot password screen has been intentionally disabled.
     * It ran a blocking event loop (see password_screen_enhanced.c) that
     * waited on keyboard input before boot could continue; any stall in
     * that loop (e.g. timer ticks not advancing, no keyboard event ever
     * arriving) made the whole system appear to hang during boot.
     * password_screen_show()/password_screen_init() are left in the tree
     * and can be re-enabled later, but boot no longer depends on them. */
    serial_puts("[KERNEL] Boot password screen disabled; skipping directly to desktop\n");

    serial_puts("[KERNEL] Entering the modern desktop...\n");
    // storage_sync is intentionally deferred until storage backend is confirmed healthy.
    // cos_power_init();

#if COS_ENABLE_FULL_DESKTOP
    serial_puts("[KERNEL] Initializing GUI...\n");
    gui_init();
    init_gui_done = gui_is_initialized();
    if (init_gui_done) {
        serial_puts("[KERNEL] Entering GUI main loop...\n");
        gui_sync_desktop_with_fs();
        /* Run the GUI update loop as a real scheduled kernel thread
         * (gui_main_thread below) instead of directly on the boot
         * stack. A second demo thread (demo_heartbeat_thread) is
         * created alongside it purely so the preemptive scheduler has
         * more than one runnable thread to actually switch between -
         * proof that multitasking is live, not just wired up and
         * sitting idle. Once both are created, this original boot path
         * has nothing further to do: the very next timer tick hands
         * control to gui_main_thread (see the scheduler_switch_task
         * fix for the prev==NULL case) and this stack is abandoned. */        bool gui_workers_ok = true;
        /* gui_main is where cos_netsurf_browser_poll() and, through it,
         * every bit of NetSurf's HTML/CSS/layout pipeline and the QuickJS
         * interpreter actually run (see gui_update() in gui_lifecycle.c) -
         * real pages put far more native C call depth on this thread than
         * the KERNEL_STACK_SIZE (512 KiB) default sized for a typical small
         * kernel worker. That mismatch let a JS-heavy page overrun the real
         * stack silently: QuickJS's own recursion guard defaults to a 1 MiB
         * budget (JS_DEFAULT_STACK_SIZE) whenever nothing configures it
         * otherwise, so on an 8 KiB stack it could never actually trip
         * before the real memory beneath the stack was already corrupted.
         * GUI_MAIN_STACK_SIZE below gives this thread real headroom, and
         * cos_js_new_context() in quickjs_port.c calls JS_SetMaxStackSize()
         * with a matching, conservative budget so QuickJS's own guard is
         * finally checking against a real number instead of a fictional
         * one 128x too large for what actually exists. */
        thread_t* gui_main_thread_handle = thread_create_kernel_stack_size(
            "gui_main", (void*)gui_main_thread, NULL, GUI_MAIN_STACK_SIZE);
        thread_t* demo_heartbeat_thread_handle = thread_create_kernel("demo_heartbeat", (void*)demo_heartbeat_thread, NULL);
        if (!gui_main_thread_handle || !demo_heartbeat_thread_handle) {
            gui_workers_ok = false;
        }

        /* Notification expiry also moves off the GUI thread's own
         * cadence and onto its own schedule - see
         * notification_center.c for why this used to be a bug when it
         * lived inside draw_notifications(). */
        extern int notification_center_start_gc_thread(void);
        if (notification_center_start_gc_thread() != 0) {
            gui_workers_ok = false;
        }

        if (!gui_workers_ok) {
            kernel_discard_thread(&demo_heartbeat_thread_handle);
            kernel_discard_thread(&gui_main_thread_handle);
        }

        if (gui_workers_ok) {
            serial_puts("[KERNEL] Enabling preemptive multitasking...\n");
            scheduler_set_preemption(1);
#if COS_HTTP_RUNTIME_SMOKE
            http_runtime_smoke_start();
#endif
#if COS_SCHED_PREEMPTION_PROBE
            if (!preempt_probe_start()) {
                serial_puts("[SCHED] PREEMPTION-PROBE setup failed\n");
            }
#endif
            serial_puts("[KERNEL] Starting scheduler and abandoning boot stack...\n");
            scheduler_start();

            /* The boot thread remains the low-priority service loop after
             * scheduling begins.  Keep network and USB progress independent
             * of GUI rendering so DHCP/TCP receive work is not starved. */
            while (1) {
#if COS_ENABLE_NETWORK
                net_poll();
#endif
                usb_poll();
                cpu_idle();
            }
        }
    }

    serial_puts("[KERNEL] Entering text-console fallback loop...\n");
    kernel_draw_status_screen("C-OS 4.0.8 alpha", "GUI unavailable - text fallback active");
    if (kernel_graphics_ready()) {
        vga_flip();
    }
    serial_puts("[KERNEL] Enabling preemptive multitasking...\n");
    scheduler_set_preemption(1);
    serial_puts("[KERNEL] Starting scheduler (fallback mode)...\n");
    scheduler_start();

    while (1) {
        #if COS_ENABLE_NETWORK
        net_poll();
        #endif
        usb_poll();
        cpu_idle();
    }
#else
    serial_puts("[KERNEL] GUI disabled - text fallback active\n");
    kernel_draw_status_screen("C-OS 4.0.8 alpha", "GUI disabled - serial/text fallback active");
    if (kernel_graphics_ready()) {
        vga_flip();
    }
    serial_puts("[KERNEL] Enabling preemptive multitasking...\n");
    scheduler_set_preemption(1);
    serial_puts("[KERNEL] Starting scheduler (minimal mode)...\n");
    scheduler_start();

    while (1) {
        /* network disabled */
        cpu_hlt();
    }
#endif

    serial_puts("[KERNEL] ERROR: fell out of main loop!\n");
    __builtin_trap();
}
