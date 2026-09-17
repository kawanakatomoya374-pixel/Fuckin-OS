BITS 64
global _start
section .text
_start:
    mov rax, 0
    mov rdi, msg1
    mov rsi, msg1_len
    int 0x80

    ; Deliberately crash with an invalid opcode (#UD). A "complete" loader
    ; must survive this: only this process should die, not the kernel.
    ud2

    ; Should never reach here.
    mov rax, 0
    mov rdi, msg_bad
    mov rsi, msg_bad_len
    int 0x80
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
msg1: db "CRASHTEST about to execute ud2 (invalid opcode)", 10
msg1_len equ $ - msg1
msg_bad: db "CRASHTEST_FAIL survived past ud2 - should be impossible", 10
msg_bad_len equ $ - msg_bad
