BITS 64
global _start
section .text
_start:
    mov rax, 0
    mov rdi, msg
    mov rsi, msg_len
    int 0x80
    ; exit with a distinctive status the parent will check for
    mov rax, 1
    mov rdi, 42
    int 0x80
section .rodata
msg: db "CHILD running, will exit with status 42", 10
msg_len equ $ - msg
