/**
 * userspace_demo.c - Real ring3 usermode demonstration process
 *
 * This exercises the ring3 execution path end-to-end: a genuinely
 * separate process (its own page directory, its own user stack) running
 * real machine code at CPL3, which calls back into the kernel purely
 * through the int 0x80 syscall gate (see syscall.c) - not by jumping
 * into kernel functions directly, since ring3 code cannot do that.
 *
 * The machine code below is not hand-encoded from memory: it was
 * assembled with nasm from this source (kept here for anyone who wants
 * to regenerate or modify it):
 *
 *   BITS 64
 *   ORG 0x40000000
 *   start:
 *       mov rax, 0              ; SYS_WRITE
 *       mov rdi, msg
 *       mov rsi, msg_len
 *       int 0x80
 *       mov rax, 1              ; SYS_EXIT
 *       mov rdi, 0              ; exit code 0
 *       int 0x80
 *   .hang:
 *       jmp .hang
 *   msg: db "Hello from real Ring3 userspace!", 10, 0
 *   msg_len equ $ - msg - 1
 */
#include "task.h"
#include "serial.h"
#include "types.h"
#include "mm/paging.h"

/* 0x10000000 is covered by the kernel's low identity map and cannot be
 * remapped in a copied user PML4. Use a free address in the user half. */
#define USER_DEMO_CODE_BASE 0x0000000040000000ULL

static const uint8_t g_user_demo_code[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00, 0x48, 0xbf, 0x24, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x00, 0xbe, 0x21, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0xbf, 0x00, 0x00, 0x00, 0x00, 0xcd, 0x80, 0xeb, 0xfe,
    0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x66, 0x72, 0x6f, 0x6d, 0x20, 0x72,
    0x65, 0x61, 0x6c, 0x20, 0x52, 0x69, 0x6e, 0x67, 0x33, 0x20, 0x75, 0x73,
    0x65, 0x72, 0x73, 0x70, 0x61, 0x63, 0x65, 0x21, 0x0a, 0x00,
};

void spawn_ring3_demo_process(void) {
    serial_puts("[USERSPACE] Spawning real ring3 (CPL3) demo process...\n");

    process_t* proc = process_create("ring3-demo", TASK_TYPE_USER);
    if (!proc) {
        serial_puts("[USERSPACE] FAILED: process_create() returned NULL\n");
        return;
    }

    if (!paging_setup_user_code((page_directory_t*)proc->page_dir, USER_DEMO_CODE_BASE,
                                 g_user_demo_code, sizeof(g_user_demo_code))) {
        serial_puts("[USERSPACE] FAILED: could not map user code page\n");
        return;
    }

    thread_t* th = thread_create(proc, (void*)USER_DEMO_CODE_BASE, NULL);
    if (!th) {
        serial_puts("[USERSPACE] FAILED: thread_create() returned NULL\n");
        return;
    }

    serial_puts("[USERSPACE] Ring3 demo process created (pid=");
    serial_putdec((uint64_t)proc->pid);
    serial_puts("), entry=0x");
    serial_puthex(USER_DEMO_CODE_BASE);
    serial_puts(" - watch for \"Hello from real Ring3 userspace!\" printed"
                " via the int 0x80 SYS_WRITE syscall below.\n");
}
