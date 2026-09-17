BITS 64
global _start
section .text
_start:
    mov rax, 1
    mov rdi, 11
    int 0x80
