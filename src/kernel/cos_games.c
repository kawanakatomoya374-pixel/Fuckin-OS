/* cos_games.c - 2D Games, Cyber Storm (the FPS) and the TinyGL viewer are ring-3 programs, one instance each
 * (userland/programs/{games2d,launcher,tinygl}). Every old kernel entry point starts or re-uses them. */
#include "serial.h"
#include "task.h"
#include "cos_games2d_elf.h"
#include "cos_launcher_elf.h"
#include "cos_tinygl_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);
static uint32_t s_pid[3];

static void launch(int slot, const char *name, const unsigned char *img, unsigned int len, const char *tag) {
    if (s_pid[slot]) {
        process_t *p = process_get_by_pid(s_pid[slot]);
        if (p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED) return;
    }
    const char *argv[1] = { name };
    int64_t pid = cos_launch_elf_image_args(name, img, len, argv, 1, NULL, 0);
    s_pid[slot] = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[GAMES] launched " : "[GAMES] launch FAILED ");
    serial_puts(tag);
    serial_puts("\n");
}

void cos_games2d_open(void)  { launch(0, "Games2D.c-os", cos_games2d_elf, cos_games2d_elf_len, "games2d"); }
void cos_fps_open(void)      { launch(1, "Launcher.c-os", cos_launcher_elf, cos_launcher_elf_len, "launcher"); }   /* the former Craft icon: tabs Craft / mc-classic-c */
void cos_tinygl_open(void)   { launch(2, "TinyGL.c-os", cos_tinygl_elf, cos_tinygl_elf_len, "tinygl"); }
