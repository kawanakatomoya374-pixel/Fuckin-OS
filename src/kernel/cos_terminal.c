/* cos_terminal.c - launch the modern ring-3 Terminal.c-os. */
#include "serial.h"
#include "task.h"
#include "cos_terminal_elf.h"
extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid;
void cos_terminal_open(void) {
    if (s_pid) {
        process_t *p = process_get_by_pid(s_pid);
        if (p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED) return;
    }
    const char *argv[1] = { "Terminal.c-os" };
    int64_t pid = cos_launch_elf_image_args("Terminal.c-os", cos_terminal_elf, cos_terminal_elf_len, argv, 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[TERMINAL] modern .c-os launched\n" : "[TERMINAL] launch FAILED\n");
}
