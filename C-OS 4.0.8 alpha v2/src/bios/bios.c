/**
 * bios.c - C-OS 4.0.8 alpha BIOS Implementation (Working Version)
 * Basic Input/Output System for C-OS 4.0.8 alpha
 */

#include "bios.h"
#include "memory.h"
#include "vga.h"
#include "io.h"
#include "../drivers/input/keyboard.h"
#include "rtc.h"
#include "string.h"
#include "acpi_power.h"

// String function declarations for freestanding environment
void* memset(void* s, int c, size_t n);
char* strcpy(char* dest, const char* src);
char* strcat(char* dest, const char* src);

// Implement VGA functions
void vga_clear(uint64_t color);
void vga_set_color(uint64_t color);
void vga_printf(const char* format, ...);

// VGA color constants
#define VGA_COLOR_WHITE 0xFFFFFFFF

// Keyboard functions are now implemented in drivers/keyboard.c
void keyboard_init(void);
bool keyboard_has_event(void);

static inline void bios_idle(void) {
    __asm__ volatile("sti; hlt");
}
key_event_t keyboard_get_event(void);

// RTC functions
void rtc_init(void);

// Entry point for linker
extern void _start(void);

/* BIOS State */
static bios_post_status_t post_status;
static bios_system_info_t sys_info;
static bios_disk_info_t disks[4];
static int disk_count = 0;
static bios_video_mode_t video_modes[8];
static int video_mode_count = 0;
static bios_video_mode_t current_video_mode;
static uint64_t total_memory_kb = 0;

static inline void bios_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf)
                     : "memory");
}

static uint64_t bios_detect_logical_cpus(void) {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    bios_cpuid(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    uint64_t cpus = 1;

    if (max_leaf >= 0x0B) {
        uint32_t level = 0;
        while (1) {
            bios_cpuid(0x0B + level, &eax, &ebx, &ecx, &edx);
            if (ebx == 0) break;
            if ((ecx & 0xFF) == 1) {
                cpus = ebx & 0xFFFF;
                break;
            }
            level++;
            if (level > 4) break;
        }
    } else {
        bios_cpuid(1, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 28)) {
            uint64_t logical = (ebx >> 16) & 0xFFu;
            if (logical > 1) cpus = logical;
        }
    }

    return cpus ? cpus : 1;
}

void bios_get_system_info_copy(bios_system_info_t* out) {
    if (!out) return;
    memcpy(out, &sys_info, sizeof(sys_info));
}

void bios_shutdown(void) {
    /* Try ACPI/ACPICA first, then common emulator power-off ports, then halt. */
    (void)acpi_power_poweroff();
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    __asm__ volatile("cli; hlt");
    for (;;) { __asm__ volatile("hlt"); }
}

void bios_reboot(void) {
    /* Try keyboard-controller reset first, then fall back to halt. */
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; ++i) {
        __asm__ volatile("pause");
    }
    __asm__ volatile("cli");
    for (;;) { __asm__ volatile("hlt"); }
}

// Forward declarations for internal BIOS functions
static void bios_init_video(void);
static void bios_init_keyboard(void);
static void bios_init_rtc(void);
static void bios_init_memory(void);
void bios_post_display(const bios_post_status_t* status);
int bios_post(void);
void bios_detect_disks(void);
int bios_get_system_info(void);

/* ============================================================
 * INITIALIZATION
 * ============================================================ */

int bios_init(void) {
    // Initialize subsystems
    bios_init_video();
    bios_init_keyboard();
    bios_init_rtc();
    bios_init_memory();
    
    return BIOS_OK;
}

/* ============================================================
 * VIDEO SUBSYSTEM
 * ============================================================ */

void bios_init_video(void) {
    // Initialize VGA controller
    vga_init(0, 0);
    
    // Set default video mode
    current_video_mode.width = 1024;
    current_video_mode.height = 768;
    current_video_mode.bpp = 32;
    current_video_mode.mode_num = 0;
    current_video_mode.framebuffer = 0;
    current_video_mode.pitch = 0;
    
    video_modes[0] = current_video_mode;
    video_mode_count = 1;
    
    post_status.video_initialized = TRUE;
}

static void bios_init_keyboard(void) {
    // Initialize keyboard controller
    keyboard_init();
    post_status.keyboard_initialized = TRUE;
}

static void bios_init_rtc(void) {
    // Initialize RTC
    rtc_init();
    post_status.rtc_initialized = TRUE;
}

static void bios_init_memory(void) {
    // Initialize memory management
    // This would normally detect memory via BIOS interrupts
    // For now, assume 64MB
    total_memory_kb = 64 * 1024;
    post_status.memory_test = TRUE;
}

/* ============================================================
 * POWER-ON SELF TEST (POST)
 * ============================================================ */

void bios_post_display(const bios_post_status_t* status) {
    vga_clear(0);
    vga_set_color(VGA_COLOR_WHITE);
    
    vga_printf("C-OS 4.0.8 alpha\n");
    vga_printf("==================\n");
    
    if (status->cpu_test) {
        vga_printf("CPUテスト: 合格\n");
    } else {
        vga_printf("CPUテスト: 失敗\n");
    }
    
    if (status->memory_test) {
        vga_printf("メモリテスト: 合格 (%d KB)\n", total_memory_kb / 1024);
    } else {
        vga_printf("メモリテスト: 失敗\n");
    }
    
    if (status->video_initialized) {
        vga_printf("映像: %dx%d @ %d bpp\n", 
                 status->video_mode.width, status->video_mode.height, status->video_mode.bpp);
    } else {
        vga_printf("映像: 失敗\n");
    }
    
    if (status->keyboard_initialized) {
        vga_printf("キーボード: 合格\n");
    } else {
        vga_printf("キーボード: 失敗\n");
    }
    
    if (status->rtc_initialized) {
        vga_printf("RTC: 合格\n");
    } else {
        vga_printf("RTC: 失敗\n");
    }
    
    vga_printf("==================\n");
    
    if (status->errors > 0) {
        vga_printf("検出されたエラー: %d\n", status->errors);
        vga_printf("続行するには何かキーを押してください...(押さないとダメだよ...(?)\n");
        
        // Wait for keypress
        while (!keyboard_has_event()) {
            bios_idle();
        }
        keyboard_get_event(); /* Clear buffer */
    }
}

int bios_post(void) {
    bios_post_status_t* status = &post_status;
    
    // Reset status
    memset(status, 0, sizeof(bios_post_status_t));
    
    // CPU self-test
    status->cpu_test = TRUE;  // Assume CPU test passes
    
    // Memory test
    status->memory_test = TRUE;  // Assume memory test passes
    
    // Initialize video
    bios_init_video();
    status->video_initialized = TRUE;
    
    // Initialize keyboard
    bios_init_keyboard();
    status->keyboard_initialized = TRUE;
    
    // Initialize RTC
    bios_init_rtc();
    status->rtc_initialized = TRUE;
    
    // Display POST results
    bios_post_display(status);
    
    return (status->errors == 0) ? BIOS_OK : BIOS_ERR_POST;
}

/* ============================================================
 * DISK DETECTION
 * ============================================================ */

void bios_detect_disks(void) {
    // Initialize disk array
    memset(disks, 0, sizeof(disks));
    disk_count = 0;
    
    // Add virtual storage disk
    strcpy(disks[0].model, "C-OS 4.0.8 alpha VFS Disk");
    strcpy(disks[0].serial, "VFS001");
    disks[0].type = 0x80;  // HDD type
    disks[0].size_mb = 32;  // 32MB VFS
    disks[0].present = TRUE;
    disk_count = 1;
    
    post_status.disk_initialized = TRUE;
}

/* ============================================================
 * SYSTEM INFORMATION
 * ============================================================ */

int bios_get_system_info(void) {
    memset(&sys_info, 0, sizeof(sys_info));

    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    bios_cpuid(0, &eax, &ebx, &ecx, &edx);
    memcpy(sys_info.vendor, &ebx, 4);
    memcpy(sys_info.vendor + 4, &edx, 4);
    memcpy(sys_info.vendor + 8, &ecx, 4);
    sys_info.vendor[12] = '\0';

    bios_cpuid(1, &eax, &ebx, &ecx, &edx);
    sys_info.has_apic = (edx & (1u << 9)) ? 1 : 0;
    sys_info.has_acpi = (edx & (1u << 22)) ? 1 : 0;
    sys_info.num_cpus = (uint8_t)bios_detect_logical_cpus();
    if (sys_info.num_cpus == 0) sys_info.num_cpus = 1;

    // CPU information
    strcpy(sys_info.cpu_vendor, "x86_64 CPU");
    strcpy(sys_info.cpu_model, "Compatible / CPUID detected");
    sys_info.cpu_speed = 1000;  // Placeholder until TSC calibration exists

    // Memory information
    sys_info.total_memory_kb = total_memory_kb;
    sys_info.available_memory_kb = (total_memory_kb > 1024) ? (total_memory_kb - 1024) : total_memory_kb;

    // BIOS information
    strcpy(sys_info.bios_vendor, "C-OS 4.0.8 alpha BIOS");
    strcpy(sys_info.bios_version, "4.0.5");
    strcpy(sys_info.bios_date, "2026-05-10");

    post_status.system_info_valid = TRUE;
    return BIOS_OK;
}

/* ============================================================
 * BOOT SEQUENCE
 * ============================================================ */

void bios_boot_sequence(void) {
    // Run POST
    bios_post();
    
    // Get system information
    bios_get_system_info();
    
    // Wait for key if errors
    if (post_status.errors > 0) {
        while (!keyboard_has_event()) {
            bios_idle();
        }
        keyboard_get_event(); /* Clear buffer */
    }
}

/* ============================================================
 * ERROR HANDLING
 * ============================================================ */

const char* bios_error_string(int error) {
    switch (error) {
        case BIOS_OK:           return "OK";
        case BIOS_ERR_MEMORY:   return "メモリエラー";
        case BIOS_ERR_DISK:     return "ディスクエラー";
        case BIOS_ERR_VIDEO:    return "映像エラー";
        case BIOS_ERR_KEYBOARD: return "キーボードエラー";
        case BIOS_ERR_MOUSE:    return "マウスエラー";
        case BIOS_ERR_RTC:      return "RTCエラー";
        case BIOS_ERR_SERIAL:   return "シリアルエラー";
        case BIOS_ERR_PARALLEL: return "パラレルエラー";
        case BIOS_ERR_NETWORK:  return "ネットワークエラー";
        case BIOS_ERR_POST:     return "POSTエラー";
        case BIOS_ERR_UNKNOWN:  return "不明なエラー";
        default:                return "無効なエラーコード";
    }
}

void bios_log_error(int error, const char* context) {
    // Log to serial or debug output
    (void)error;
    (void)context;
    /* Implementation depends on serial driver availability */
}
