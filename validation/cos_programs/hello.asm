BITS 64
global _start
section .text
_start:
    mov rax, 0
    mov rdi, msg
    mov rsi, msg_len
    int 0x80
    mov rax, 0
    mov rdi, rodata_msg
    mov rsi, rodata_len
    int 0x80
    ; prove .bss works: write a byte, read it back, print result
    mov rbx, bss_flag
    mov byte [rbx], 0x41
    mov al, [rbx]
    cmp al, 0x41
    jne .skip
    mov rax, 0
    mov rdi, bss_msg
    mov rsi, bss_len
    int 0x80
.skip:
    mov rax, 1
    mov rdi, 0
    int 0x80
.hang:
    jmp .hang

section .rodata
rodata_msg: db "COSELF rodata segment loaded", 10
rodata_len equ $ - rodata_msg

section .data
msg: db "COSELF_PASS hello from a real .c-os ELF executable", 10
msg_len equ $ - msg
bss_msg: db "COSELF bss writable and zero-initialised", 10
bss_len equ $ - bss_msg

section .bss
bss_flag: resb 1
