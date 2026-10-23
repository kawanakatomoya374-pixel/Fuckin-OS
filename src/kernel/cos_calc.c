/* cos_calc.c - the Calculator (userland/programs/calc) and the HTTP Downloader (userland/programs/httpdl) are ring-3
 * programs, one instance each: every old kernel entry point (Start menu, desktop icon, taskbar button, shell) starts
 * or re-uses them. The calculator takes an optional start mode ("hissan", "graph") as argv[1]. */
#include "serial.h"
#include "task.h"
#include "cos_calc_elf.h"
#include "cos_httpdl_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_calc_pid, s_dl_pid;

static bool alive(uint32_t pid) {
    if (!pid) return false;
    process_t *p = process_get_by_pid(pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_calc_open(const char *mode) {
    if (alive(s_calc_pid)) return;
    bool has = mode && mode[0];
    const char *argv[2] = { "Calculator", has ? mode : NULL };
    int64_t pid = cos_launch_elf_image_args("Calculator.c-os", cos_calc_elf, cos_calc_elf_len, argv, has ? 2 : 1, NULL, 0);
    s_calc_pid = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[CALC] launched\n" : "[CALC] launch FAILED\n");
}

void cos_httpdl_open(void) {
    if (alive(s_dl_pid)) return;
    const char *argv[1] = { "HttpDownloader" };
    int64_t pid = cos_launch_elf_image_args("HttpDownloader.c-os", cos_httpdl_elf, cos_httpdl_elf_len, argv, 1, NULL, 0);
    s_dl_pid = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[HTTPDL] launched\n" : "[HTTPDL] launch FAILED\n");
}
