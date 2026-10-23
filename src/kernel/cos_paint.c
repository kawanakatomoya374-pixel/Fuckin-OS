/* cos_paint.c - the Paint app is a ring-3 program (userland/programs/paint).
 * Single instance, like the File Manager and the music player. */
#include "serial.h"
#include "task.h"
#include "cos_paint_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid = 0;

static bool alive(void) {
    if (!s_pid) return false;
    process_t *p = process_get_by_pid(s_pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_paint_open(void) {
    if (alive()) return;
    const char *argv[1] = { "Paint" };
    int64_t pid = cos_launch_elf_image_args("Paint.c-os", cos_paint_elf, cos_paint_elf_len, argv, 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[PAINT] launched\n" : "[PAINT] launch FAILED\n");
}
