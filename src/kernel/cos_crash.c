/* cos_crash.c - what the user sees when a program is ended by a CPU exception.
 *
 * The exception handlers (idt.c, page_fault.c) already end the offending ring-3 process and carry on; until now that was a line in the serial log and a window that
 * vanished. cos_crash_note() is called from them (it only queues: it runs in exception context, with locks held), and cos_crash_gui_tick() - on the GUI thread, once
 * per frame - turns each queued crash into
 *   - a line in /system/crash.log (the last 12 KiB are kept),
 *   - the error window of userland/programs/errorreport ("<program> has stopped working", the error code, the details).
 * Error code: 0xC0VVEEEE - VV the CPU exception number, EEEE its error code (for a page fault the P/W/U/R/I bits).
 * A Linux program's helper process (Firefox's content process) is only logged: the program itself shows its own message and one window per child would be noise. */
#include "serial.h"
#include "task.h"
#include "sync.h"
#include "timer.h"
#include "fs.h"
#include <string.h>
#include "cos_errorreport_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
extern void gui_notify_simple(const char *msg);

#define CQ 16
typedef struct { uint32_t pid; char name[32]; uint32_t vector; uint64_t err, rip, addr, ms; bool quiet; } crash_rec_t;
static crash_rec_t g_cq[CQ];
static volatile uint32_t g_ch, g_ct;
static uint64_t g_last_dialog_ms; static char g_last_name[32]; static uint64_t g_last_name_ms;

static void hex(uint64_t v, char *out) { static const char d[] = "0123456789abcdef"; out[0] = '0'; out[1] = 'x'; int n = 0; char t[17]; do { t[n++] = d[v & 15]; v >>= 4; } while (v); int k = 2; while (n) out[k++] = t[--n]; out[k] = 0; }
static void dec(uint64_t v, char *out) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); int k = 0; while (n) out[k++] = t[--n]; out[k] = 0; }

void cos_crash_exception_name(uint32_t v, const char **en, const char **ja) {
    switch (v) {
        case 0:  *en = "Divide by zero (#DE)"; *ja = "0 で割ろうとしました (#DE)"; break;
        case 1:  *en = "Debug trap (#DB)"; *ja = "デバッグ例外 (#DB)"; break;
        case 3:  *en = "Breakpoint (#BP)"; *ja = "ブレークポイント (#BP)"; break;
        case 4:  *en = "Overflow (#OF)"; *ja = "オーバーフロー (#OF)"; break;
        case 5:  *en = "Bounds check (#BR)"; *ja = "範囲外の参照 (#BR)"; break;
        case 6:  *en = "Invalid instruction (#UD)"; *ja = "実行できない命令 (#UD)"; break;
        case 7:  *en = "Math unit not available (#NM)"; *ja = "演算ユニットが使えません (#NM)"; break;
        case 10: *en = "Invalid task state (#TS)"; *ja = "不正な状態 (#TS)"; break;
        case 11: *en = "Segment not present (#NP)"; *ja = "セグメントがありません (#NP)"; break;
        case 12: *en = "Stack fault (#SS)"; *ja = "スタックのエラー (#SS)"; break;
        case 13: *en = "General protection fault (#GP)"; *ja = "許可されていない操作 (#GP)"; break;
        case 14: *en = "Invalid memory access (#PF)"; *ja = "メモリへの不正なアクセス (#PF)"; break;
        case 16: *en = "Floating-point error (#MF)"; *ja = "浮動小数点のエラー (#MF)"; break;
        case 17: *en = "Misaligned access (#AC)"; *ja = "アドレスのずれたアクセス (#AC)"; break;
        case 19: *en = "SIMD floating-point error (#XM)"; *ja = "SIMD 演算のエラー (#XM)"; break;
        default: *en = "CPU exception"; *ja = "CPU の例外"; break;
    }
}
uint32_t cos_crash_code(uint32_t vector, uint64_t err) { return 0xC0000000u | ((vector & 0xFF) << 16) | (uint32_t)(err & 0xFFFF); }

/* Exception context: queue only. */
void cos_crash_note(process_t *p, uint32_t vector, uint64_t err, uint64_t rip, uint64_t addr) {
    if (!p) return;
    uint64_t fl = sync_irq_save();
    uint32_t next = (g_ch + 1) % CQ;
    if (next != g_ct) {
        crash_rec_t *r = &g_cq[g_ch];
        memset(r, 0, sizeof *r);
        r->pid = (uint32_t)p->pid; r->vector = vector; r->err = err; r->rip = rip; r->addr = addr; r->ms = get_timer_ticks();
        strncpy(r->name, p->name, sizeof r->name - 1);
        if (p->linux_ctx && p->parent_pid) { process_t *pp = process_get_by_pid(p->parent_pid); if (pp && pp->linux_ctx) r->quiet = true; }
        g_ch = next;
    }
    sync_irq_restore(fl);
}

static void append_log(const char *line) {
    static char buf[12288 + 512]; uint64_t got = 0;
    buf[0] = 0;
    if (fs_read_path_into("/system/crash.log", buf, 12288, &got)) { buf[got] = 0; if (got > 8192) { size_t cut = got - 8192; const char *nl = strchr(buf + cut, '\n'); memmove(buf, nl ? nl + 1 : buf + cut, got - (size_t)(nl ? nl + 1 - buf : cut) + 1); got = strlen(buf); } }
    else got = 0;
    size_t l = strlen(line); if (got + l > sizeof buf - 1) return;
    memcpy(buf + got, line, l); got += l;
    fs_write_file_at("/system", "crash.log", buf, got);
}

/* GUI thread, once per frame. */
extern bool cos_panic_japanese;
extern bool gui_is_japanese(void);
void cos_crash_gui_tick(void) {
    { static uint64_t lang_t; uint64_t n0 = get_timer_ticks(); if (n0 - lang_t > 2000) { lang_t = n0; cos_panic_japanese = gui_is_japanese(); } }     /* the panic screen cannot ask the settings itself */
    {   /* test switch: with the file /system/cos.panictest present, the kernel provokes a fault of its own 20 s after the start (shows the panic screen; the file is removed first) */
        static bool done; if (!done && get_timer_ticks() > 20000) { done = true; uint64_t sz; bool d;
            if (fs_stat_path("/system/cos.panictest", &sz, &d)) { fs_delete_file("/system/cos.panictest"); serial_puts("[CRASH] cos.panictest: provoking a kernel fault\n"); *(volatile int *)0 = 1; } }
    }
    if (g_ct == g_ch) return;
    uint64_t now = get_timer_ticks();
    if (now - g_last_dialog_ms < 800) return;                                   /* one window at a time: several crashes in a row come out one after the other */
    uint64_t fl = sync_irq_save();
    crash_rec_t r = g_cq[g_ct]; g_ct = (g_ct + 1) % CQ;
    sync_irq_restore(fl);
    const char *en, *ja; cos_crash_exception_name(r.vector, &en, &ja);
    char code[16], pid[24], rip[24], addr[24], line[360], ms[24];
    uint32_t cv = cos_crash_code(r.vector, r.err); hex(cv, code); for (char *c = code; *c; ++c) if (*c >= 'a' && *c <= 'f') *c = (char)(*c - 32); code[1] = 'x';
    dec(r.pid, pid); hex(r.rip, rip); hex(r.addr, addr); dec(r.ms, ms);
    line[0] = 0; strcat(line, "["); strcat(line, ms); strcat(line, " ms] "); strcat(line, r.name); strcat(line, " (pid "); strcat(line, pid); strcat(line, ") "); strcat(line, code); strcat(line, " "); strcat(line, en);
    strcat(line, " rip="); strcat(line, rip); strcat(line, " addr="); strcat(line, addr); strcat(line, r.quiet ? " (helper process: not shown)\n" : "\n");
    serial_puts("[CRASH] "); serial_puts(line);
    append_log(line);
    if (r.quiet) return;
    if (!strncmp(r.name, "ErrorReport", 11)) return;                              /* never a window about the error window itself */
    if (!strcmp(r.name, g_last_name) && now - g_last_name_ms < 4000) return;        /* the same program crashing in a loop */
    strncpy(g_last_name, r.name, sizeof g_last_name - 1); g_last_name_ms = now; g_last_dialog_ms = now;
    const char *argv[9] = { "ErrorReport", r.name, code, en, ja, pid, rip, addr, "" };
    int64_t p = cos_launch_elf_image_args("ErrorReport.c-os", cos_errorreport_elf, cos_errorreport_elf_len, argv, 9, NULL, 0);
    if (p <= 0) { gui_notify_simple("A program stopped working (see /system/crash.log)"); }
}
