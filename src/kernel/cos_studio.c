/* cos_studio.c - "open in C-OS Studio" for the whole OS.
 *
 * C-OS Studio is a ring-3 program (userland/programs/studio), built by the kernel build with
 * tools/build_studio.sh and embedded here. It is installed on disk as
 *   /C-OS Studio/C-OS Studio.c-os
 * and everything it makes goes to /C-OS Studio/deliverables. Like the music player it is a
 * single instance: opening a file while it runs sends the path to its window
 * (COS_EV_OPEN) instead of starting a second copy.
 */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "task.h"
#include "cos_app_window.h"
#include "cos_studio_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);

static uint32_t s_pid = 0;
static char s_pending[256];

static bool studio_alive(void) {
    if (!s_pid) return false;
    process_t *p = process_get_by_pid(s_pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_studio_open(const char *path) {
    bool has_path = path && path[0] && !(path[0] == '/' && path[1] == '\0');
    if (studio_alive()) {
        if (!has_path) { (void)cos_app_window_deliver_path(s_pid, "", COS_EV_OPEN, 0, 0); return; }
        if (cos_app_window_deliver_path(s_pid, path, COS_EV_OPEN, 0, 0)) return;
        size_t n = 0;
        while (path[n] && n < sizeof(s_pending) - 1) { s_pending[n] = path[n]; ++n; }
        s_pending[n] = '\0';
        return;
    }
    const char *argv[2] = { "C-OS Studio", has_path ? path : NULL };
    int64_t pid = cos_launch_elf_image_args("C-OS Studio.c-os", cos_studio_elf, cos_studio_elf_len,
                                            argv, has_path ? 2 : 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    s_pending[0] = '\0';
    serial_puts(pid > 0 ? "[STUDIO] launched\n" : "[STUDIO] launch FAILED\n");
}

void cos_studio_on_window_created(uint32_t pid) {
    if (pid == s_pid && s_pending[0]) {
        (void)cos_app_window_deliver_path(pid, s_pending, COS_EV_OPEN, 0, 0);
        s_pending[0] = '\0';
    }
}

const unsigned char *cos_studio_image(unsigned int *len) { if (len) *len = cos_studio_elf_len; return cos_studio_elf; }
