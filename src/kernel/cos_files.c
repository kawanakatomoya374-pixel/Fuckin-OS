/* cos_files.c - the File Manager is a ring-3 program (userland/programs/files),
 * built by the kernel build and embedded here. Every path that used to open the
 * in-kernel window (desktop icon, Start menu, "open folder") starts it - or raises
 * the running one - through cos_files_open(). Single instance, like the music player
 * and C-OS Studio: a folder opened while it runs is sent as COS_EV_OPEN. */
#include "cos_fm_support.h"
#include "serial.h"
#include "string.h"
#include "task.h"
#include "cos_app_window.h"
#include "cos_files_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid = 0;
static char s_pending[256];

static bool alive(void) {
    if (!s_pid) return false;
    process_t *p = process_get_by_pid(s_pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_files_open(const char *path) {
    bool has = path && path[0];
    if (alive()) {
        if (!has) { (void)cos_app_window_deliver_path(s_pid, "", COS_EV_OPEN, 0, 0); return; }
        if (cos_app_window_deliver_path(s_pid, path, COS_EV_OPEN, 0, 0)) return;
        size_t n = 0;
        while (path[n] && n < sizeof(s_pending) - 1) { s_pending[n] = path[n]; ++n; }
        s_pending[n] = 0;
        return;
    }
    const char *argv[2] = { "Files", has ? path : NULL };
    int64_t pid = cos_launch_elf_image_args("Files.c-os", cos_files_elf, cos_files_elf_len, argv, has ? 2 : 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    s_pending[0] = 0;
    serial_puts(pid > 0 ? "[FILES] launched\n" : "[FILES] launch FAILED\n");
}
const unsigned char *cos_files_image(unsigned int *len) { if (len) *len = cos_files_elf_len; return cos_files_elf; }

void cos_files_on_window_created(uint32_t pid) {
    if (pid == s_pid && s_pending[0]) {
        (void)cos_app_window_deliver_path(pid, s_pending, COS_EV_OPEN, 0, 0);
        s_pending[0] = 0;
    }
}
