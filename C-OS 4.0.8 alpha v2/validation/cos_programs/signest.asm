BITS 64
global _start
section .text
_start:
    ; register handlers for signals 10 and 11
    mov rax, 23
    mov rdi, 10
    mov rsi, handler_a
    int 0x80

    mov rax, 23
    mov rdi, 11
    mov rsi, handler_b
    int 0x80

    ; send ourselves signal 10
    mov rax, 11                 ; SYS_GETPID
    int 0x80
    mov edi, eax
    mov rsi, 10
    mov rax, 24                 ; SYS_KILL
    int 0x80

    xor r12, r12
.wait_loop:
    mov rax, 10                 ; SYS_SLEEP_MS - the checkpoint where
    mov rdi, 20                 ; delivery happens
    int 0x80

    mov rbx, g_b_ran
    mov eax, [rbx]
    test eax, eax
    jnz .both_done

    inc r12
    cmp r12, 200
    jb .wait_loop

    mov rdi, e_timeout
    mov rsi, e_timeout_len
    jmp .emit

.both_done:
    ; handler_a must have run before handler_b (strict ordering, not
    ; nested/simultaneous execution)
    mov rbx, g_a_seq
    mov eax, [rbx]
    cmp eax, 1
    jne .fail_order

    mov rbx, g_b_seq
    mov eax, [rbx]
    cmp eax, 2
    jne .fail_order

    ; prove normal execution resumed CORRECTLY after BOTH signals -
    ; real computation, not just "did not crash". This is the check that
    ; actually catches last_sigframe_addr corruption: if handler_b's
    ; SIGRETURN read a garbled/reused frame, this would land on the wrong
    ; value or crash outright.
    mov rax, 777
    mov rbx, 111
    sub rax, rbx
    cmp rax, 666
    jne .fail_resume

    mov rdi, m_ok
    mov rsi, m_ok_len
    jmp .emit

.fail_order:
    mov rdi, e_order
    mov rsi, e_order_len
    jmp .emit
.fail_resume:
    mov rdi, e_resume
    mov rsi, e_resume_len
.emit:
    mov rax, 0
    int 0x80
    mov rax, 1
    mov rdi, 0
    int 0x80

; handler for signal 10: sends itself signal 11 WHILE ITS OWN HANDLER IS
; STILL EXECUTING (before returning) - this is exactly the nesting
; scenario the fix targets. Without the fix, this SYS_KILL's own
; end-of-syscall delivery check would immediately try to deliver signal
; 11 NESTED inside handler_a, clobbering last_sigframe_addr and losing
; handler_a's own way back to the original context.
handler_a:
    mov rbx, g_a_seq
    mov dword [rbx], 1

    mov rax, 11                 ; SYS_GETPID
    int 0x80
    mov edi, eax
    mov rsi, 11
    mov rax, 24                 ; SYS_KILL(self, 11)
    int 0x80

    ; do real work here too, so a corrupted return INTO this handler's
    ; own remaining code (not just the outer resume) would also be caught
    mov rax, 50
    add rax, 50
    cmp rax, 100
    je .a_ok
    mov rbx, g_a_seq
    mov dword [rbx], 999        ; poison: prove this path was reached wrong
.a_ok:
    ret

; handler for signal 11: must run AFTER handler_a fully finishes (once
; blocked_signals is restored and this deferred signal finally becomes
; deliverable), not nested inside it.
handler_b:
    mov rbx, g_b_seq
    mov dword [rbx], 2
    mov rbx, g_b_ran
    mov dword [rbx], 1
    ret

section .bss
g_a_seq: resd 1
g_b_seq: resd 1
g_b_ran: resd 1

section .rodata
m_ok: db "SIGNEST_PASS nested signal correctly deferred, ran in order, and resumed cleanly", 10
m_ok_len equ $ - m_ok
e_order: db "SIGNEST_FAIL handlers did not run in strict A-then-B order", 10
e_order_len equ $ - e_order
e_resume: db "SIGNEST_FAIL execution did not resume correctly after both signals", 10
e_resume_len equ $ - e_resume
e_timeout: db "SIGNEST_FAIL timed out waiting for both handlers", 10
e_timeout_len equ $ - e_timeout
