/* cos_shell.c - starts and watches the ring-3 shell programs: the taskbar (userland/programs/taskbar) and the start menu
 * (userland/programs/startmenu).
 *
 * The kernel taskbar (draw_taskbar) and start menu stay as the fallback: the compositor only draws a shell surface once it
 * has presented a frame, and the kernel versions take over the moment the process is gone. A program that dies is
 * restarted, at most three times a minute, so a crash loop settles on the kernel version instead of burning the CPU. */
#include "serial.h"
#include "task.h"
#include "timer.h"
#include "cos_taskbar_elf.h"
#include "cos_startmenu_elf.h"
#include "cos_desktop_elf.h"
#include "cos_notify_elf.h"
#include "cos_ctxmenu_elf.h"
#include "cos_extd_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
extern bool cos_bootsplash_done(void);

typedef struct {
    const char *name;
    const unsigned char *image;
    unsigned int len;
    uint32_t pid;
    uint64_t window_start;
    int launches;
    char fname[32];
} shell_prog_t;

static shell_prog_t s_progs[] = {
    { "taskbar",   cos_taskbar_elf,   0, 0, 0, 0, "" },
    { "startmenu", cos_startmenu_elf, 0, 0, 0, 0, "" },
    { "desktop",   cos_desktop_elf,   0, 0, 0, 0, "" },
    { "notify",    cos_notify_elf,    0, 0, 0, 0, "" },
    { "ctxmenu",   cos_ctxmenu_elf,   0, 0, 0, 0, "" },
    { "extd",      cos_extd_elf,      0, 0, 0, 0, "" },      /* runs the plugins of /system/plugins/system (userland/programs/extd) */
};
static uint64_t s_next_check;
bool            g_cos_shell_disabled;      /* set by a test to keep the kernel taskbar */

void cos_shell_supervise(void)
{
    uint64_t now = get_timer_ticks();
    if (now < s_next_check || g_cos_shell_disabled) return;
    s_next_check = now + 250;
    if (!cos_bootsplash_done()) return;
    s_progs[0].len = cos_taskbar_elf_len;
    s_progs[1].len = cos_startmenu_elf_len;
    s_progs[2].len = cos_desktop_elf_len;
    s_progs[3].len = cos_notify_elf_len;
    s_progs[4].len = cos_ctxmenu_elf_len;
    s_progs[5].len = cos_extd_elf_len;
    for (unsigned i = 0; i < sizeof s_progs / sizeof s_progs[0]; i++) {
        shell_prog_t *p = &s_progs[i];
        if (p->pid) {
            process_t *pr = process_get_by_pid(p->pid);
            if (pr && pr->state != TASK_ZOMBIE && pr->state != TASK_UNUSED) continue;
            serial_puts("[SHELL] "); serial_puts(p->name); serial_puts(" exited\n");
            p->pid = 0;
        }
        if (now - p->window_start > 60000u) { p->window_start = now; p->launches = 0; }
        if (p->launches >= 3) continue;
        ++p->launches;
        char *fname = p->fname;
        int n = 0;
        for (const char *c = p->name; *c && n < 20; c++) fname[n++] = *c;
        for (const char *c = ".c-os"; *c; c++) fname[n++] = *c;
        fname[n] = 0;
        const char *argv[1] = { p->name };
        int64_t pid = cos_launch_elf_image_args(fname, p->image, p->len, argv, 1, NULL, 0);
        p->pid = pid > 0 ? (uint32_t)pid : 0;
        serial_puts("[SHELL] "); serial_puts(p->name); serial_puts(pid > 0 ? " launched\n" : " launch FAILED - kernel version stays\n");
    }
}
