BITS 64
global _start
section .text
_start:
    mov rax, 10                 ; SYS_SLEEP_MS
    mov rdi, 150
    int 0x80
    mov rax, 1
    mov rdi, 22
    int 0x80
