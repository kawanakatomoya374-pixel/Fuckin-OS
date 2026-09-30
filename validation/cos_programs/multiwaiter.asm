BITS 64
global _start
section .text
_start:
    ; spawn the SLOW child first
    mov rax, 12
    mov rdi, pslow
    int 0x80
    cmp rax, 0
    jle .fail_spawn_slow
    mov r12, rax                 ; slow child pid

    ; spawn the FAST child second (it will race to exit before the slow one)
    mov rax, 12
    mov rdi, pfast
    int 0x80
    cmp rax, 0
    jle .fail_spawn_fast
    mov r13, rax                 ; fast child pid

    ; wait SPECIFICALLY for the slow child while the fast one exits in
    ; the background almost immediately. wait_queue_wake_all() wakes
    ; every blocked waiter unconditionally, so this parent WILL be
    ; spuriously woken when the fast child exits (not the one it is
    ; targeting) - it must re-check, find nothing yet, and go back to
    ; sleep rather than mis-returning the fast child's pid/status.
    mov rax, 13                  ; SYS_WAITPID
    mov rdi, r12                 ; specifically the SLOW child
    mov rsi, status
    mov rdx, 1                   ; block
    int 0x80
    cmp rax, r12
    jne .fail_wrong_pid

    mov rbx, status
    mov eax, [rbx]
    cmp eax, 22                  ; the SLOW child's status, not the fast one's (11)
    jne .fail_wrong_status

    ; now collect the fast child too, non-blocking (it exited a while ago)
    mov rax, 13
    mov rdi, r13
    mov rsi, status2
    mov rdx, 0                   ; WNOHANG
    int 0x80
    cmp rax, r13
    jne .fail_fast_not_collected

    mov rbx, status2
    mov eax, [rbx]
    cmp eax, 11
    jne .fail_fast_status

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_spawn_slow: mov rdi, e1
                   mov rsi, e1_len
                   jmp .emit
.fail_spawn_fast: mov rdi, e2
                   mov rsi, e2_len
                   jmp .emit
.fail_wrong_pid:   mov rdi, e3
                   mov rsi, e3_len
                   jmp .emit
.fail_wrong_status: mov rdi, e4
                     mov rsi, e4_len
                     jmp .emit
.fail_fast_not_collected: mov rdi, e5
                          mov rsi, e5_len
                          jmp .emit
.fail_fast_status: mov rdi, e6
                    mov rsi, e6_len
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .bss
status:  resd 1
status2: resd 1

section .rodata
pslow: db "/childslow.c-os", 0
pfast: db "/childfast.c-os", 0
m_ok: db "MULTIWAIT_PASS targeted wait correctly ignored a different sibling's spurious wake", 10
m_ok_len equ $ - m_ok
e1: db "MULTIWAIT_FAIL spawn of slow child failed", 10
e1_len equ $ - e1
e2: db "MULTIWAIT_FAIL spawn of fast child failed", 10
e2_len equ $ - e2
e3: db "MULTIWAIT_FAIL waitpid returned the wrong pid (likely picked up the wrong sibling)", 10
e3_len equ $ - e3
e4: db "MULTIWAIT_FAIL status did not match the targeted slow child", 10
e4_len equ $ - e4
e5: db "MULTIWAIT_FAIL WNOHANG collection of the already-exited fast child failed", 10
e5_len equ $ - e5
e6: db "MULTIWAIT_FAIL fast child status mismatch", 10
e6_len equ $ - e6
