BITS 64
global _start
section .text
_start:
    mov rax, 0
    mov rdi, msg1
    mov rsi, msg1_len
    int 0x80

    ; mmap READ-only, then WRITE to it - must be force-terminated by the
    ; kernel's page-protection fault handling (a write to a page mapped
    ; without PAGE_RW is a genuine CPU protection fault), proving
    ; PROT_READ-only is enforced at the hardware level, not just accepted
    ; and silently ignored.
    mov rax, 26
    mov rdi, 4096
    mov rsi, 1                  ; READ only
    int 0x80
    cmp rax, -1
    je .fail_mmap

    mov rbx, rax
    mov qword [rbx], 0x1234     ; must fault - this line should never
                                 ; complete successfully

    ; unreachable if protection is correctly enforced
    mov rax, 0
    mov rdi, e_survived
    mov rsi, e_survived_len
    int 0x80
    mov rax, 1
    mov rdi, 1
    int 0x80

.fail_mmap:
    mov rax, 0
    mov rdi, e_mmap
    mov rsi, e_mmap_len
    int 0x80
    mov rax, 1
    mov rdi, 1
    int 0x80

section .rodata
msg1: db "MMAPFAULT about to write to a PROT_READ-only mmap region", 10
msg1_len equ $ - msg1
e_survived: db "MMAPFAULT_FAIL write to read-only mmap region did not fault", 10
e_survived_len equ $ - e_survived
e_mmap: db "MMAPFAULT_FAIL mmap itself failed", 10
e_mmap_len equ $ - e_mmap
