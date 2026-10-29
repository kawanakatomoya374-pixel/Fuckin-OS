/* cos_panic.c - the screen for an error the kernel cannot go on from (a CPU exception inside the kernel itself).
 *
 * Until now such an error left a black or frozen screen and a few lines on the serial port. This stops the other CPUs, draws a full-screen message (what happened,
 * the error code 0xE0VVEEEE - VV the CPU exception number, EEEE its error code - and where), and restarts the computer on a key press (PS/2) or after 30 seconds.
 * Errors in a program are not handled here: that program is ended and a window tells the user (cos_crash.c). */
#include "serial.h"
#include "types.h"
#include "vga.h"
#include "io.h"
#include <string.h>

volatile int cos_panic_active;
extern void smp_halt_other_cpus(void);
extern void timer_vdso_snapshot(uint64_t *hz, uint64_t *base_tsc, uint64_t *base_ms, uint64_t *tick_ms);
extern void cos_crash_exception_name(uint32_t v, const char **en, const char **ja);
extern void bios_reboot(void);
extern bool cos_panic_japanese;

bool cos_panic_japanese = true;               /* kept up to date by cos_crash_gui_tick(): the panic cannot ask the settings */

static void hexs(uint64_t v, char *out) { static const char d[] = "0123456789ABCDEF"; int n = 0; char t[17]; do { t[n++] = d[v & 15]; v >>= 4; } while (v); int k = 0; out[k++] = '0'; out[k++] = 'x'; while (n) out[k++] = t[--n]; out[k] = 0; }
static uint64_t rdtsc_(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

void cos_panic_screen(uint32_t vector, uint64_t err, uint64_t rip, uint64_t addr, const char *where)
{
    if (cos_panic_active) return;                                        /* a second error while this one is on screen: nothing more to do */
    cos_panic_active = 1;
    __asm__ volatile("cli");
    smp_halt_other_cpus();
    if (!vga_has_display()) return;
    bool jp = cos_panic_japanese;
    const char *en, *ja; cos_crash_exception_name(vector, &en, &ja);
    uint32_t code = 0xE0000000u | ((vector & 0xFF) << 16) | (uint32_t)(err & 0xFFFF);
    char c[24], r[24], a[24], line[200];
    hexs(code, c); hexs(rip, r); hexs(addr, a);
    uint64_t W = SCREEN_W, H = SCREEN_H;
    vga_clear(0x000B1E3F);
    vga_fill_rect(0, 0, (int)W, 6, 0x00E5484D);
    int x = 60, y = 70;
    vga_draw_string(x, y, jp ? "C-OS で問題が発生しました" : "C-OS ran into a problem", 0x00FFFFFF, 0x000B1E3F); y += 40;
    vga_draw_string(x, y, jp ? "システムの中で解決できないエラーが起きたため、動作を止めました。" : "An error that the system cannot recover from stopped it.", 0x00D8E4FF, 0x000B1E3F); y += 26;
    vga_draw_string(x, y, jp ? "保存していない作業は失われる場合があります。" : "Work that was not saved may be lost.", 0x00D8E4FF, 0x000B1E3F); y += 50;
    line[0] = 0; strcat(line, jp ? "エラーコード:  " : "Error code:  "); strcat(line, c);
    vga_draw_string(x, y, line, 0x00FFFFFF, 0x000B1E3F); y += 28;
    line[0] = 0; strcat(line, jp ? "内容:  " : "What happened:  "); strcat(line, jp ? ja : en);
    vga_draw_string(x, y, line, 0x00D8E4FF, 0x000B1E3F); y += 28;
    line[0] = 0; strcat(line, jp ? "場所:  " : "Where:  "); strcat(line, where ? where : "kernel"); strcat(line, "   RIP "); strcat(line, r); strcat(line, "   ADDR "); strcat(line, a);
    vga_draw_string(x, y, line, 0x00D8E4FF, 0x000B1E3F); y += 60;
    vga_draw_string(x, y, jp ? "いずれかのキーを押すと再起動します。30 秒たつと自動で再起動します。" : "Press any key to restart. The computer restarts by itself after 30 seconds.", 0x00FFE08A, 0x000B1E3F); y += 36;
    vga_draw_string(x, y, jp ? "問い合わせるときは、上のエラーコードを伝えてください。(詳しい記録はシリアルログにもあります)" : "When you report this, quote the error code above. (The serial log has the details.)", 0x009AA8C8, 0x000B1E3F);
    (void)H;
    vga_flip();
    serial_puts("[PANIC] error code "); serial_puts(c); serial_puts(" shown on screen\n");
    /* 30 seconds by the real-time clock's seconds register (it runs whatever the other timers do) */
    uint32_t secs = 0; int last = -1; uint64_t spins = 0;
    for (;;) {
        if (inb(0x64) & 1) { uint8_t sc = inb(0x60); if (!(sc & 0x80)) break; }      /* a key was pressed (make code) */
        outb(0x70, 0x00); int cur = inb(0x71);
        if (last >= 0 && cur != last) { if (++secs >= 30) break; }
        last = cur;
        if (++spins > 8000000000ull) break;                                            /* the clock does not move at all: give up after a long while */
        __asm__ volatile("pause");
    }
    serial_puts("[PANIC] restarting\n");
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);                                              /* the PC reset control register (chipset reset) */
    outb(0x64, 0xFE);                                                                  /* the keyboard controller's reset line */
    { struct { uint16_t limit; uint64_t base; } __attribute__((packed)) nullidt = { 0, 0 }; __asm__ volatile("lidt %0; int3" : : "m"(nullidt)); }      /* a triple fault resets whatever is left */
    for (;;) __asm__ volatile("hlt");
}
