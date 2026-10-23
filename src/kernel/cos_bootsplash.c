/* cos_bootsplash.c - launches the ring-3 boot splash (userland/programs/bootsplash) once,
 * right after the window system is ready, and answers "is it done yet" for
 * gui_boot_animation_completed() (boot_animation.c) to gate the desktop's reveal on -
 * exactly the same gate that used to be driven by the old kernel-space animation's own
 * frame loop, just now watching a real process instead of a local flag.
 *
 * Never allowed to hang boot: if the process fails to launch, or does not exit within a
 * generous safety timeout (the program itself finishes in ~2.2s; this errs far longer),
 * "done" becomes true regardless. A missing splash is a cosmetic loss; a desktop that
 * never appears because a launch failed is not an acceptable trade for one.
 */
#include "serial.h"
#include "task.h"
#include "timer.h"
#include "cos_bootsplash_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
extern uint64_t get_timer_ticks(void);

#define BOOTSPLASH_TIMEOUT_MS 8000u

static uint32_t s_pid = 0;
static bool     s_launched = false;
static bool     s_gave_up = false;
static uint64_t s_launch_ticks = 0;

void cos_bootsplash_launch(void) {
    if (s_launched) return;
    s_launched = true;
    const char *argv[1] = { "bootsplash" };
    int64_t pid = cos_launch_elf_image_args("bootsplash.c-os", cos_bootsplash_elf, cos_bootsplash_elf_len, argv, 1, NULL, 0);
    s_pid = pid > 0 ? (uint32_t)pid : 0;
    s_launch_ticks = get_timer_ticks();
    serial_puts(pid > 0 ? "[BOOTSPLASH] launched\n" : "[BOOTSPLASH] launch FAILED - skipping straight to desktop\n");
}

bool cos_bootsplash_done(void) {
    if (!s_launched) return true;        /* cos_bootsplash_launch() hasn't run yet - nothing to wait for */
    if (!s_pid || s_gave_up) return true;
    process_t *p = process_get_by_pid(s_pid);
    if (!p || p->state == TASK_ZOMBIE || p->state == TASK_UNUSED) return true;
    /* PIT ticks are ~1ms apart on this kernel (see timer.c); a fixed tick budget is a
     * closer match to real elapsed time than assuming a rate and doing the arithmetic
     * ourselves would be worth here. */
    if (get_timer_ticks() - s_launch_ticks > BOOTSPLASH_TIMEOUT_MS) {
        serial_puts("[BOOTSPLASH] timed out waiting for exit - revealing desktop anyway\n");
        s_gave_up = true;
        return true;
    }
    return false;
}
