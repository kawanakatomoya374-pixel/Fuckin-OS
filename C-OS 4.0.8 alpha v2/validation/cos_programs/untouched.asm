BITS 64
global _start
section .text
_start:
    ; sbrk(4096) - grow the heap but NEVER touch the returned memory
    ; before handing it to a syscall. This is the completely ordinary
    ; "malloc a buffer, then read() into it" pattern.
    mov rax, 6
    mov rdi, 4096
    int 0x80
    cmp rax, -1
    je .fail_brk
    mov r12, rax                 ; fresh, UNTOUCHED heap buffer

    ; write a file first so there is something to read back
    mov rax, 8
    mov rdi, fpath
    mov rsi, fdata
    mov rdx, fdata_len
    int 0x80
    cmp rax, fdata_len
    jne .fail_write

    ; SYS_READ_FILE straight into the untouched buffer - no prior access
    ; to r12 has happened, so the page backing it is NOT YET PRESENT in
    ; the page tables. This is the case a naive paging_user_range_ok()
    ; check gets wrong.
    mov rax, 7
    mov rdi, fpath
    mov rsi, r12
    mov rdx, 64
    int 0x80
    cmp rax, fdata_len
    jne .fail_read

    mov rax, 0
    mov rdi, ok
    mov rsi, ok_len
    int 0x80
    jmp .done
.fail_brk:
    mov rax, 0
    mov rdi, e1
    mov rsi, e1_len
    int 0x80
    jmp .done
.fail_write:
    mov rax, 0
    mov rdi, e2
    mov rsi, e2_len
    int 0x80
    jmp .done
.fail_read:
    mov rax, 0
    mov rdi, e3
    mov rsi, e3_len
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
fpath: db "/untouched.dat", 0
fdata: db "untouched heap buffer test data"
fdata_len equ $ - fdata
ok:  db "UNTOUCHED_PASS read into never-before-touched heap memory worked", 10
ok_len equ $ - ok
e1:  db "UNTOUCHED_FAIL sbrk failed", 10
e1_len equ $ - e1
e2:  db "UNTOUCHED_FAIL file write failed", 10
e2_len equ $ - e2
e3:  db "UNTOUCHED_FAIL read into untouched heap buffer failed", 10
e3_len equ $ - e3
