/* cos_music.c - "open audio file" for the whole OS.
 *
 * The music player is a ring-3 program (userland/programs/music/music.c),
 * built by the kernel build and embedded here. Every place that used to
 * open the old in-kernel player (desktop icon, Start menu, file manager
 * double-click, "open with") calls cos_music_open(path) instead:
 *   - player not running: launch it, with the file as argv[1];
 *   - player running: send the file as a COS_EV_OPEN event to its window
 *     and raise it - one player, like a normal desktop OS;
 *   - player launched but its window not up yet: remember the file and
 *     deliver it the moment the window exists (cos_music_on_window_created).
 */
#include "types.h"
#include "string.h"
#include "serial.h"
#include "task.h"
#include "cos_app_window.h"
#include "cos_music_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);

static uint32_t s_pid = 0;
static char s_pending[256];

static bool player_alive(void) {
    if (!s_pid) return false;
    process_t *p = process_get_by_pid(s_pid);
    return p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED;
}

void cos_music_open(const char *path) {
    bool has_path = path && path[0] && !(path[0] == '/' && path[1] == '\0');
    if (player_alive()) {
        if (!has_path) {
            /* just bring it forward */
            (void)cos_app_window_deliver_path(s_pid, "", COS_EV_OPEN, 0, 0);
            return;
        }
        if (cos_app_window_deliver_path(s_pid, path, COS_EV_OPEN, 0, 0)) return;
        size_t n = 0;
        while (path[n] && n < sizeof(s_pending) - 1) { s_pending[n] = path[n]; ++n; }
        s_pending[n] = '\0';
        return;
    }
    const char *argv[2] = { "music", has_path ? path : NULL };
    int64_t pid = cos_launch_elf_image_args("music.c-os", cos_music_elf, cos_music_elf_len,
                                            argv, has_path ? 2 : 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    s_pending[0] = '\0';
    serial_puts(pid > 0 ? "[MUSIC] player launched\n" : "[MUSIC] player launch FAILED\n");
}

void cos_music_on_window_created(uint32_t pid) {
    if (pid == s_pid && s_pending[0]) {
        (void)cos_app_window_deliver_path(pid, s_pending, COS_EV_OPEN, 0, 0);
        s_pending[0] = '\0';
    }
}

const unsigned char *cos_music_image(unsigned int *len) { if (len) *len = cos_music_elf_len; return cos_music_elf; }
