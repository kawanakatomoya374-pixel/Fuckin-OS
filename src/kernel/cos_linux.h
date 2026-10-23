/* cos_linux.h - running Linux x86-64 programs (docs/LINUX_COMPAT.md).
 *
 * A Linux binary talks to its kernel with the `syscall` instruction and the Linux system call numbers. C-OS leaves EFER.SCE off, so the instruction raises #UD;
 * the exception handler hands it to cos_linux_try_syscall(), which runs the Linux call on top of C-OS's own services (files, memory, time) and resumes after
 * the instruction. Nothing in a C-OS program changes: only processes started by cos_linux_spawn() take this path. */
#ifndef COS_LINUX_H
#define COS_LINUX_H

#include <stdint.h>
#include <stdbool.h>
#include "idt.h"
#include "task.h"

/* Starts the ELF file `path` as a Linux process (static or dynamic, PIE: the address window below COS_ELF_PIE_BASE belongs to the kernel). Returns the pid, or -1. */
int64_t cos_linux_spawn(const char *path, const char *const *argv, int argc, const char *const *envp, int envc);

/* From the #UD handler: true if the exception was a Linux `syscall` and has been executed (the frame is updated, execution resumes after the instruction). */
bool cos_linux_try_syscall(struct regs *r);

/* From the page-fault handler: a lazily mapped page of a Linux process (mmap, brk). */
bool cos_linux_page_fault(process_t *proc, uint64_t page);

/* Process teardown. */
void cos_linux_release(process_t *proc);

#endif
