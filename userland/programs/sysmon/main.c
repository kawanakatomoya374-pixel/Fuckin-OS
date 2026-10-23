/* sysmon.c-os - Task Manager, Memory, Storage, System Information and About as one ring-3 program.
 *
 * The kernel used to draw these five apps itself with VGA primitives. They only ever displayed numbers, so they
 * are now one binary that reads SYS_SYSTEM_INFO / SYS_PROCLIST and picks its view from argv[1]
 * ("taskmgr", "memory", "storage", "sysinfo", "about"). Unlike the old Task Manager, which listed GUI windows,
 * this one lists real processes.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "cos.h"
#include "cos_ui.h"

#define BG        0x141826
#define PANEL     0x1C2236
#define PANEL2    0x232B44
#define LINE      0x2F3A58
#define TEXT      0xE8ECF5
#define MUTED     0x8891A5
#define ACCENT    0x4C9AFF
#define GOOD      0x3DD68C
#define WARN      0xF2B84B
#define BAD       0xFF5C5C

enum { V_TASKMGR, V_MEMORY, V_STORAGE, V_SYSINFO, V_ABOUT };
enum { TAB_PROC, TAB_PERF, TAB_HW, TAB_COUNT };

#define HIST 120
static cui_canvas c;
static cui_font *f_s, *f_m, *f_b, *f_h, *f_big;
static cos_sysinfo_t si;
static cos_procinfo_t procs[128];
static int nprocs;
static uint8_t hist[HIST];
static int hist_n;
static int tab, scroll;

static void fmt_size(uint64_t b, char *out, size_t n) {
    if (b >= (1ULL << 30)) snprintf(out, n, "%llu.%02llu GB", (unsigned long long)(b >> 30), (unsigned long long)((b & ((1ULL << 30) - 1)) * 100 >> 30));
    else if (b >= (1ULL << 20)) snprintf(out, n, "%llu.%02llu MB", (unsigned long long)(b >> 20), (unsigned long long)((b & ((1ULL << 20) - 1)) * 100 >> 20));
    else if (b >= 1024) snprintf(out, n, "%llu KB", (unsigned long long)(b >> 10));
    else snprintf(out, n, "%llu B", (unsigned long long)b);
}

static void fmt_uptime(uint64_t ms, char *out, size_t n) {
    uint64_t s = ms / 1000;
    snprintf(out, n, "%llu:%02llu:%02llu", (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60), (unsigned long long)(s % 60));
}

static void bar(int x, int y, int w, int h, uint64_t used, uint64_t total, uint32_t col) {
    cui_round_rect(&c, x, y, w, h, h / 2, PANEL2);
    if (total == 0) return;
    int fw = (int)((uint64_t)w * (used > total ? total : used) / total);
    if (fw > 0) cui_round_rect(&c, x, y, fw < h ? h : fw, h, h / 2, col);
}

static uint32_t load_col(uint64_t p) { return p < 50 ? GOOD : p < 80 ? WARN : BAD; }

static void header(const char *title, const char *sub) {
    cui_vgradient(&c, 0, 0, c.w, 64, 0x1F3A6E, BG);
    cui_text(&c, f_h, 20, 10, title, TEXT);
    if (sub) cui_text(&c, f_s, 20, 38, sub, MUTED);
}

static void kv(int x, int y, int vx, const char *k, const char *v) {
    cui_text(&c, f_m, x, y, k, MUTED);
    cui_text(&c, f_m, vx, y, v, TEXT);
}

static void card(int x, int y, int w, int h, const char *title) {
    cui_round_rect(&c, x, y, w, h, 10, PANEL);
    cui_round_rect_outline(&c, x, y, w, h, 10, 1.0f, LINE);
    if (title) cui_text(&c, f_b, x + 16, y + 12, title, ACCENT);
}

static void sample(void) {
    cos_sysinfo(&si);
    nprocs = cos_proclist(procs, 128);
    if (nprocs < 0) nprocs = 0;
    uint64_t l = si.load_now > 100 ? 100 : si.load_now;
    if (hist_n < HIST) hist[hist_n++] = (uint8_t)l;
    else { memmove(hist, hist + 1, HIST - 1); hist[HIST - 1] = (uint8_t)l; }
}

static const char *state_name(uint32_t s) {
    switch (s) {
    case COS_PROC_STATE_CREATED: return "Created";
    case COS_PROC_STATE_READY: return "Ready";
    case COS_PROC_STATE_RUNNING: return "Running";
    case COS_PROC_STATE_BLOCKED: return "Blocked";
    case COS_PROC_STATE_SLEEPING: return "Sleeping";
    case COS_PROC_STATE_ZOMBIE: return "Zombie";
    default: return "?";
    }
}

static void draw_graph(int x, int y, int w, int h) {
    cui_round_rect(&c, x, y, w, h, 8, 0x0D1020);
    for (int i = 1; i < 4; i++) cui_fill(&c, x + 4, y + h * i / 4, w - 8, 1, LINE);
    int step = (w - 8) / (HIST - 1);
    if (step < 1) step = 1;
    for (int i = 1; i < hist_n; i++) {
        float x0 = (float)(x + 4 + (i - 1) * step), x1 = (float)(x + 4 + i * step);
        float y0 = (float)(y + h - 4 - (h - 8) * hist[i - 1] / 100);
        float y1 = (float)(y + h - 4 - (h - 8) * hist[i] / 100);
        cui_line(&c, x0, y0, x1, y1, 2.0f, load_col(hist[i]));
    }
}

static void tabbar(int y) {
    static const char *const names[TAB_COUNT] = { "Processes", "Performance", "Hardware" };
    int x = 16;
    for (int i = 0; i < TAB_COUNT; i++) {
        int w = cui_text_width(f_m, names[i]) + 28;
        if (i == tab) {
            cui_round_rect(&c, x, y, w, 30, 8, PANEL2);
            cui_fill(&c, x + 10, y + 27, w - 20, 3, ACCENT);
        }
        cui_text_center(&c, f_m, x, y, w, 30, names[i], i == tab ? TEXT : MUTED);
        x += w + 6;
    }
}

static int tab_hit(int mx, int my, int y) {
    static const char *const names[TAB_COUNT] = { "Processes", "Performance", "Hardware" };
    if (my < y || my >= y + 30) return -1;
    int x = 16;
    for (int i = 0; i < TAB_COUNT; i++) {
        int w = cui_text_width(f_m, names[i]) + 28;
        if (mx >= x && mx < x + w) return i;
        x += w + 6;
    }
    return -1;
}

static void draw_taskmgr(void) {
    char a[64], b[64], line[160];
    cui_fill(&c, 0, 0, c.w, c.h, BG);
    fmt_uptime(cos_time_ms(), a, sizeof a);
    snprintf(line, sizeof line, "%d processes   CPU %llu%%   up %s", nprocs, (unsigned long long)si.load_now, a);
    header("Task Manager", line);
    tabbar(72);
    int top = 112, w = c.w - 32;

    if (tab == TAB_PROC) {
        cui_round_rect(&c, 16, top, w, 28, 6, PANEL2);
        int cx[7] = { 28, 70, 250, 340, 440, 540, 640 };
        const char *hd[7] = { "PID", "Name", "State", "Threads", "Memory", "CPU time", "Type" };
        for (int i = 0; i < 7; i++) if (cx[i] < c.w) cui_text(&c, f_s, cx[i], top + 6, hd[i], MUTED);
        int rows = (c.h - top - 40) / 26;
        int max_scroll = nprocs > rows ? nprocs - rows : 0;
        if (scroll > max_scroll) scroll = max_scroll;
        for (int r = 0; r < rows && r + scroll < nprocs; r++) {
            const cos_procinfo_t *p = &procs[r + scroll];
            int y = top + 32 + r * 26;
            if (r & 1) cui_fill(&c, 16, y, w, 26, PANEL);
            snprintf(a, sizeof a, "%llu", (unsigned long long)p->pid);
            cui_text(&c, f_s, cx[0], y + 5, a, MUTED);
            cui_text_fit(&c, f_s, cx[1], y + 5, 170, p->name[0] ? p->name : "(unnamed)", TEXT);
            uint32_t sc = p->state == COS_PROC_STATE_RUNNING ? GOOD : p->state == COS_PROC_STATE_ZOMBIE ? BAD : TEXT;
            cui_text(&c, f_s, cx[2], y + 5, state_name(p->state), sc);
            snprintf(a, sizeof a, "%u", p->threads);
            cui_text(&c, f_s, cx[3], y + 5, a, TEXT);
            fmt_size(p->heap_bytes, b, sizeof b);
            if (cx[4] < c.w) cui_text(&c, f_s, cx[4], y + 5, b, TEXT);
            snprintf(a, sizeof a, "%llu ms", (unsigned long long)p->cpu_time);
            if (cx[5] < c.w) cui_text(&c, f_s, cx[5], y + 5, a, TEXT);
            if (cx[6] < c.w) cui_text(&c, f_s, cx[6], y + 5, p->is_user ? "User" : "Kernel", p->is_user ? ACCENT : MUTED);
        }
    } else if (tab == TAB_PERF) {
        int gh = (c.h - top - 40) / 3 - 40;
        if (gh < 70) gh = 70;
        int y = top;
        snprintf(line, sizeof line, "CPU  %llu%%   (1m %llu%%   5m %llu%%   15m %llu%%)", (unsigned long long)si.load_now,
                 (unsigned long long)si.load_1m, (unsigned long long)si.load_5m, (unsigned long long)si.load_15m);
        cui_text(&c, f_b, 20, y, line, TEXT);
        draw_graph(16, y + 26, w, gh);
        y += gh + 44;
        {   /* one bar per CPU */
            uint8_t cpu_pct[16];
            int ncpu = cos_cpu_load(cpu_pct, 16);
            if (ncpu > 1) {
                int cols = ncpu > 4 ? 4 : ncpu, rows = (ncpu + cols - 1) / cols, bw = (w - 16 * (cols - 1)) / cols;
                snprintf(line, sizeof line, "Cores  (%d online)", ncpu);
                cui_text(&c, f_b, 20, y, line, TEXT);
                for (int i = 0; i < ncpu; i++) {
                    int bx = 16 + (i % cols) * (bw + 16), by = y + 28 + (i / cols) * 34;
                    snprintf(line, sizeof line, "CPU%d  %u%%", i, (unsigned)cpu_pct[i]);
                    cui_text(&c, f_s, bx, by, line, MUTED);
                    bar(bx, by + 16, bw, 10, cpu_pct[i], 100, load_col(cpu_pct[i]));
                }
                y += 28 + rows * 34 + 10;
            }
        }
        fmt_size(si.kheap_used, a, sizeof a);
        fmt_size(si.kheap_used + si.kheap_free, b, sizeof b);
        snprintf(line, sizeof line, "Kernel heap  %s of %s", a, b);
        cui_text(&c, f_b, 20, y, line, TEXT);
        bar(16, y + 28, w, 22, si.kheap_used, si.kheap_used + si.kheap_free, ACCENT);
        y += 66;
        fmt_size(si.stor_used, a, sizeof a);
        fmt_size(si.stor_total, b, sizeof b);
        snprintf(line, sizeof line, "Storage  %s of %s", a, b);
        cui_text(&c, f_b, 20, y, line, TEXT);
        bar(16, y + 28, w, 22, si.stor_used, si.stor_total, GOOD);
    } else {
        int hw = w;
        card(16, top, hw, 214, "Hardware & firmware");
        int y = top + 44;
        snprintf(line, sizeof line, "%u (BIOS)  /  %u online of %u possible", si.cpus_bios ? si.cpus_bios : 1, si.cpus_online, si.cpus_possible);
        kv(32, y, 190, "CPUs", line); y += 28;
        kv(32, y, 190, "CPU vendor", si.cpu_vendor[0] ? si.cpu_vendor : "unknown"); y += 28;
        snprintf(line, sizeof line, "APIC %s   ACPI %s", si.has_apic ? "yes" : "no", si.has_acpi ? "yes" : "no");
        kv(32, y, 190, "Platform", line); y += 28;
        snprintf(line, sizeof line, "%s %s", si.bios_vendor, si.bios_version);
        kv(32, y, 190, "BIOS", line); y += 28;
        kv(32, y, 190, "BIOS date", si.bios_date[0] ? si.bios_date : "unknown");
        cos_device_info_t *dev = (cos_device_info_t *)(void *)procs;   /* procs[] is idle on this tab */
        int nd = cos_devices_list(dev, (int)(sizeof procs / sizeof *dev));
        card(16, top + 230, hw, 100, "Input & USB devices");
        snprintf(line, sizeof line, "%d device%s detected (see Settings > Devices for details)", nd < 0 ? 0 : nd, nd == 1 ? "" : "s");
        cui_text(&c, f_m, 32, top + 274, line, MUTED);
    }
    char st[160];
    fmt_size(si.kheap_used, a, sizeof a);
    fmt_size(si.ram_total, b, sizeof b);
    snprintf(st, sizeof st, "%d processes    CPU %llu%%    Kernel heap %s    RAM %s", nprocs, (unsigned long long)si.load_now, a, b);
    cui_fill(&c, 0, c.h - 26, c.w, 26, PANEL);
    cui_text(&c, f_s, 16, c.h - 21, st, MUTED);
}

static void draw_memory(void) {
    char a[64], b[64], d[64], line[160];
    cui_fill(&c, 0, 0, c.w, c.h, BG);
    header("Memory", "Physical memory and the kernel heap");
    int w = c.w - 32, y = 80;
    uint64_t pg_used = si.pages_total > si.pages_free ? si.pages_total - si.pages_free : 0;
    card(16, y, w, 130, "Physical memory");
    fmt_size(si.ram_total, a, sizeof a);
    fmt_size(pg_used * 4096, b, sizeof b);
    fmt_size(si.pages_free * 4096, d, sizeof d);
    snprintf(line, sizeof line, "%s installed    %s in use    %s free", a, b, d);
    cui_text(&c, f_m, 32, y + 44, line, TEXT);
    bar(32, y + 76, w - 32, 22, pg_used, si.pages_total, load_col(si.pages_total ? pg_used * 100 / si.pages_total : 0));
    snprintf(line, sizeof line, "%llu of %llu pages (4 KB)", (unsigned long long)pg_used, (unsigned long long)si.pages_total);
    cui_text(&c, f_s, 32, y + 104, line, MUTED);
    y += 146;
    card(16, y, w, 130, "Kernel heap");
    fmt_size(si.kheap_used, a, sizeof a);
    fmt_size(si.kheap_free, b, sizeof b);
    snprintf(line, sizeof line, "%s used    %s free", a, b);
    cui_text(&c, f_m, 32, y + 44, line, TEXT);
    bar(32, y + 76, w - 32, 22, si.kheap_used, si.kheap_used + si.kheap_free, ACCENT);
    y += 146;
    card(16, y, w, 100, "Processes");
    uint64_t total_heap = 0;
    for (int i = 0; i < nprocs; i++) total_heap += procs[i].heap_bytes;
    fmt_size(total_heap, a, sizeof a);
    snprintf(line, sizeof line, "%d running processes hold %s of heap", nprocs, a);
    cui_text(&c, f_m, 32, y + 44, line, TEXT);
}

static void draw_storage(void) {
    char a[64], b[64], d[64], line[160];
    cui_fill(&c, 0, 0, c.w, c.h, BG);
    snprintf(line, sizeof line, "%u device%s attached", si.ustor_devices, si.ustor_devices == 1 ? "" : "s");
    header("Storage", line);
    int w = c.w - 32, y = 80;
    card(16, y, w, 150, "Persistent storage (FAT32)");
    fmt_size(si.stor_total, a, sizeof a);
    fmt_size(si.stor_used, b, sizeof b);
    fmt_size(si.stor_free, d, sizeof d);
    snprintf(line, sizeof line, "%s total    %s used    %s free", a, b, d);
    cui_text(&c, f_m, 32, y + 44, line, TEXT);
    bar(32, y + 76, w - 32, 22, si.stor_used, si.stor_total, load_col(si.stor_total ? si.stor_used * 100 / si.stor_total : 0));
    cui_text(&c, f_s, 32, y + 108, si.has_password ? "Password protection: enabled" : "Password protection: off", MUTED);
    y += 166;
    card(16, y, w, 130, "All attached devices");
    fmt_size(si.ustor_total, a, sizeof a);
    fmt_size(si.ustor_free, b, sizeof b);
    snprintf(line, sizeof line, "%s total    %s free", a, b);
    cui_text(&c, f_m, 32, y + 44, line, TEXT);
    bar(32, y + 76, w - 32, 22, si.ustor_total > si.ustor_free ? si.ustor_total - si.ustor_free : 0, si.ustor_total, ACCENT);
}

static void draw_sysinfo(void) {
    char a[64], line[160];
    cui_fill(&c, 0, 0, c.w, c.h, BG);
    header("System Information", si.os_name);
    int y = 84, w = c.w - 32;
    struct { const char *k; char v[96]; } rows[10];
    int n = 0;
#define ROW(K, ...) do { rows[n].k = K; snprintf(rows[n].v, sizeof rows[n].v, __VA_ARGS__); n++; } while (0)
    ROW("OS name", "%s", si.os_name);
    ROW("Version", "%s", si.os_version);
    ROW("Architecture", "x86-64");
    ROW("Kernel", "%s", si.boot_msg);
    ROW("CPU", "%s, %u online", si.cpu_vendor[0] ? si.cpu_vendor : "x86 compatible", si.cpus_online);
    fmt_size(si.ram_total, a, sizeof a);
    ROW("Total memory", "%s", a);
    fmt_size(si.kheap_used, a, sizeof a);
    ROW("Kernel heap used", "%s", a);
    fmt_size(si.stor_total, a, sizeof a);
    ROW("Storage", "%s", a);
    fmt_uptime(cos_time_ms(), a, sizeof a);
    ROW("Uptime", "%s", a);
    ROW("Processes", "%d", nprocs);
    for (int i = 0; i < n; i++) {
        int ry = y + i * 34;
        if (ry + 34 > c.h - 40) break;
        if (i % 2 == 0) cui_round_rect(&c, 16, ry, w, 32, 6, PANEL);
        cui_text(&c, f_m, 32, ry + 6, rows[i].k, MUTED);
        cui_text_fit(&c, f_m, 230, ry + 6, c.w - 250, rows[i].v, TEXT);
    }
    cui_text(&c, f_s, 16, c.h - 28, "Files are browsed and edited from the File Manager.", MUTED);
    (void)line;
}

static void draw_about(void) {
    char a[64], b[64], line[160];
    cui_vgradient(&c, 0, 0, c.w, c.h, 0x1B4FA0, 0x0B0E16);
    int cx = c.w / 2;
    cui_round_rect(&c, cx - 56, 36, 112, 112, 28, 0x2A6FD6);
    cui_round_rect_outline(&c, cx - 56, 36, 112, 112, 28, 2.0f, 0x7FB2FF);
    cui_text_center(&c, f_big, cx - 56, 36, 112, 112, "C", TEXT);
    cui_text_center(&c, f_h, 0, 164, c.w, 32, si.os_name, TEXT);
    snprintf(line, sizeof line, "Version %s", si.os_version);
    cui_text_center(&c, f_m, 0, 198, c.w, 24, line, 0xC8DEFF);
    fmt_size(si.ram_total, a, sizeof a);
    fmt_size(si.stor_total, b, sizeof b);
    snprintf(line, sizeof line, "%s RAM  |  %s storage  |  %u CPU%s", a, b, si.cpus_online ? si.cpus_online : 1, si.cpus_online > 1 ? "s" : "");
    cui_text_center(&c, f_m, 0, 232, c.w, 24, line, 0xB4DCFF);
    cui_text_center(&c, f_s, 0, 262, c.w, 22, "x86-64 bare-metal kernel with ring-3 applications", 0xB4DCFF);
    int bx = cx - 50, by = c.h - 70;
    cui_round_rect(&c, bx, by, 100, 34, 8, ACCENT);
    cui_text_center(&c, f_m, bx, by, 100, 34, "OK", TEXT);
}

int main(int argc, char **argv) {
    int view = V_TASKMGR;
    const char *title = "Task Manager";
    int32_t ww = 760, wh = 560;
    if (argc > 1 && argv[1]) {
        if (!strcmp(argv[1], "memory")) { view = V_MEMORY; title = "Memory"; ww = 560; wh = 520; }
        else if (!strcmp(argv[1], "storage")) { view = V_STORAGE; title = "Storage"; ww = 560; wh = 460; }
        else if (!strcmp(argv[1], "sysinfo")) { view = V_SYSINFO; title = "System Information"; ww = 560; wh = 520; }
        else if (!strcmp(argv[1], "about")) { view = V_ABOUT; title = "About"; ww = 480; wh = 400; }
    }
    cos_win_info_t wi;
    int64_t h = cos_win2_create(title, ww, wh, &wi);
    if (h <= 0) return 1;
    cui_canvas_init(&c, wi.pixels, wi.width, wi.height, wi.stride);
    f_s = cui_font_default(13);
    f_m = cui_font_default(15);
    f_b = cui_font_bold(15);
    f_h = cui_font_bold(24);
    f_big = cui_font_bold(64);

    for (;;) {
        sample();
        switch (view) {
        case V_MEMORY: draw_memory(); break;
        case V_STORAGE: draw_storage(); break;
        case V_SYSINFO: draw_sysinfo(); break;
        case V_ABOUT: draw_about(); break;
        default: draw_taskmgr(); break;
        }
        cos_win2_present(h);

        uint64_t until = cos_time_ms() + 500;
        bool redraw = false, quit = false;
        while (!redraw && !quit) {
            uint64_t now = cos_time_ms();
            if (now >= until) break;
            cos_win_event_t ev;
            int r = cos_win2_wait(h, &ev, (uint32_t)(until - now));
            if (r <= 0) continue;
            if (ev.type == COS_EV_CLOSE) quit = true;
            else if (ev.type == COS_EV_KEY && ev.ascii == 27 && view == V_ABOUT) quit = true;
            else if (ev.type == COS_EV_MOUSE_DOWN) {
                if (view == V_ABOUT && ev.x >= c.w / 2 - 50 && ev.x < c.w / 2 + 50 && ev.y >= c.h - 70 && ev.y < c.h - 36) quit = true;
                if (view == V_TASKMGR) {
                    int t = tab_hit(ev.x, ev.y, 72);
                    if (t >= 0 && t != tab) { tab = t; scroll = 0; redraw = true; }
                }
            } else if (ev.type == COS_EV_WHEEL && view == V_TASKMGR && tab == TAB_PROC) {
                scroll -= ev.wheel * 3;
                if (scroll < 0) scroll = 0;
                redraw = true;
            }
        }
        if (quit) break;
    }
    cos_win2_close(h);
    return 0;
}
