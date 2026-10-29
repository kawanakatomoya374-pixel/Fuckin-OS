/* cos_dlwin.c - the downloads window (userland/programs/dlprogress) is a ring-3 program; one instance, started when a download begins (cos_gui_table.c) */
#include "serial.h"
#include "task.h"
#include "cos_dlprogress_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid = 0;

void cos_dlwin_open(void) {
    if (s_pid) { process_t *p = process_get_by_pid(s_pid); if (p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED) return; }
    const char *argv[1] = { "Downloads" };
    int64_t pid = cos_launch_elf_image_args("Downloads.c-os", cos_dlprogress_elf, cos_dlprogress_elf_len, argv, 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
}
