/* cos_messenger.c - C-OS Messenger is a ring-3 program (userland/programs/messenger). Single instance: it owns UDP port 5150. */
#include "serial.h"
#include "task.h"
#include "cos_messenger_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid = 0;

static bool alive(void) {
    if (!s_pid) return false;
    process_t *p = process_get_by_pid(s_pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_messenger_open(void) {
    if (alive()) return;
    const char *argv[1] = { "C-OS Message" };
    int64_t pid = cos_launch_elf_image_args("CosMessage.c-os", cos_messenger_elf, cos_messenger_elf_len, argv, 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[MESSENGER] launched\n" : "[MESSENGER] launch FAILED\n");
}
