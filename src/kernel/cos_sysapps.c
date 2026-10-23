/* cos_sysapps.c - Task Manager, Memory, Storage, System Information and About are views of one ring-3 program
 * (userland/programs/sysmon). Each view is single-instance: opening it again does nothing while it is alive. */
#include "serial.h"
#include "task.h"
#include "cos_sysmon_elf.h"

extern int64_t cos_launch_elf_image_args(const char *name, const unsigned char *image, unsigned int image_len,
                                         const char *const *argv, int argc, const char *const *envp, int envc);

enum { SV_TASKMGR, SV_MEMORY, SV_STORAGE, SV_SYSINFO, SV_ABOUT, SV_COUNT };
static const char *const s_view[SV_COUNT] = { "taskmgr", "memory", "storage", "sysinfo", "about" };
static uint32_t s_pid[SV_COUNT];

static void open_view(int v) {
    if (s_pid[v]) {
        process_t *p = process_get_by_pid(s_pid[v]);
        if (p && p->state != TASK_ZOMBIE && p->state != TASK_UNUSED) return;
    }
    const char *argv[2] = { "Sysmon", s_view[v] };
    int64_t pid = cos_launch_elf_image_args("Sysmon.c-os", cos_sysmon_elf, cos_sysmon_elf_len, argv, 2, NULL, 0);
    s_pid[v] = pid > 0 ? (uint32_t)pid : 0;
    serial_puts(pid > 0 ? "[SYSMON] launched\n" : "[SYSMON] launch FAILED\n");
}

void cos_taskmgr_open(void)  { open_view(SV_TASKMGR); }
void cos_memory_open(void)   { open_view(SV_MEMORY); }
void cos_storage_open(void)  { open_view(SV_STORAGE); }
void cos_sysinfo_open(void)  { open_view(SV_SYSINFO); }
void cos_about_open(void)    { open_view(SV_ABOUT); }
