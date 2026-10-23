/* cos_clock.c - the Clock app is a ring-3 program (userland/programs/clock).
 * Single instance, like the File Manager and the music player: every path
 * that used to open the old WIN_CLOCK window (Start menu, desktop icon)
 * starts - or raises - this instead. */
#include "serial.h"
#include "task.h"
#include "cos_clock_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid = 0;

static bool alive(void) {
    if (!s_pid) return false;
    process_t *p = process_get_by_pid(s_pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_clock_open(void) {
    if (alive()) return;
    const char *argv[1] = { "Clock" };
    int64_t pid = cos_launch_elf_image_args("Clock.c-os", cos_clock_elf, cos_clock_elf_len, argv, 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[CLOCK] launched\n" : "[CLOCK] launch FAILED\n");
}
