#ifndef STUB_TASK_H
#define STUB_TASK_H
#include <stdint.h>
#include <stdbool.h>
/* Minimal stand-in for the kernel's process_t: only the fields
 * cos_elf_link.c reads. Kept deliberately small so the harness cannot
 * quietly grow a dependency on scheduler state it has no business
 * touching. */
typedef struct process {
    uint64_t pid;
    void    *page_dir;
    uint64_t stack_start;
    uint64_t stack_end;
} process_t;
#endif
