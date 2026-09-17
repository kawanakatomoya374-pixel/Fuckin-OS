BITS 64
global _start
section .text
_start:
    ; SYS_SPAWN("/childexit.c-os")
    mov rax, 12
    mov rdi, cpath
    int 0x80
    cmp rax, 0
    jle .fail_spawn
    mov r12, rax                ; child pid

    ; SYS_WAITPID(child, &status, 5000ms)
    mov rax, 13
    mov rdi, r12
    mov rsi, status_addr
    mov rsi, [rsi]
    mov rdx, 5000
    int 0x80
    cmp rax, r12
    jne .fail_wait              ; must return exactly the pid we waited on

    ; status must be 42
    mov rbx, status_addr
    mov rbx, [rbx]
    mov eax, [rbx]
    cmp eax, 42
    jne .fail_status

    ; waiting again for the same pid must now fail (already collected)
    mov rax, 13
    mov rdi, r12
    mov rsi, 0
    mov rdx, 0
    int 0x80
    cmp rax, -1
    jne .fail_twice

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done
.fail_spawn:
    mov rax, 0
    mov rdi, e_sp
    mov rsi, e_sp_len
    int 0x80
    jmp .done
.fail_wait:
    mov rax, 0
    mov rdi, e_w
    mov rsi, e_w_len
    int 0x80
    jmp .done
.fail_status:
    mov rax, 0
    mov rdi, e_st
    mov rsi, e_st_len
    int 0x80
    jmp .done
.fail_twice:
    mov rax, 0
    mov rdi, e_tw
    mov rsi, e_tw_len
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .data
status: dd 0
status_addr: dq status

section .rodata
cpath: db "/childexit.c-os", 0
m_ok:  db "WAITPID_PASS spawned child, collected exit status 42, second wait correctly failed", 10
m_ok_len equ $ - m_ok
e_sp:  db "WAITPID_FAIL spawn failed", 10
e_sp_len equ $ - e_sp
e_w:   db "WAITPID_FAIL waitpid did not return the child pid", 10
e_w_len equ $ - e_w
e_st:  db "WAITPID_FAIL exit status was not 42", 10
e_st_len equ $ - e_st
e_tw:  db "WAITPID_FAIL second waitpid unexpectedly succeeded", 10
e_tw_len equ $ - e_tw
