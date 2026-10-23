BITS 64
global _start
section .text
_start:
    ; System V: at entry, [rsp] = argc, [rsp+8] = argv[0], ...
    mov rbx, [rsp]              ; argc
    cmp rbx, 1
    jne .badargc

    mov rsi, [rsp+8]            ; argv[0] pointer
    test rsi, rsi
    jz .badptr

    ; strlen(argv[0])
    xor rcx, rcx
.len:
    cmp byte [rsi+rcx], 0
    je .gotlen
    inc rcx
    cmp rcx, 128
    jb .len
.gotlen:
    test rcx, rcx
    jz .badptr

    ; print prefix then argv[0] then newline
    mov rax, 0
    mov rdi, pre
    mov rsi, pre_len
    int 0x80

    mov rax, 0
    mov rdi, [rsp+8]
    mov rsi, rcx
    int 0x80

    mov rax, 0
    mov rdi, nl
    mov rsi, 1
    int 0x80

    ; verify argv[1] == NULL (argv array must be NULL-terminated)
    mov rax, [rsp+16]
    test rax, rax
    jnz .badterm

    mov rax, 0
    mov rdi, okmsg
    mov rsi, ok_len
    int 0x80
    jmp .done
.badargc:
    mov rax, 0
    mov rdi, e1
    mov rsi, e1_len
    int 0x80
    jmp .done
.badptr:
    mov rax, 0
    mov rdi, e2
    mov rsi, e2_len
    int 0x80
    jmp .done
.badterm:
    mov rax, 0
    mov rdi, e3
    mov rsi, e3_len
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
pre:   db "ARGV argv[0]="
pre_len equ $ - pre
nl:    db 10
okmsg: db "ARGV_PASS argc=1, argv[0] readable, argv[1]==NULL", 10
ok_len equ $ - okmsg
e1:    db "ARGV_FAIL argc was not 1", 10
e1_len equ $ - e1
e2:    db "ARGV_FAIL argv[0] was NULL or empty", 10
e2_len equ $ - e2
e3:    db "ARGV_FAIL argv[1] was not NULL", 10
e3_len equ $ - e3
