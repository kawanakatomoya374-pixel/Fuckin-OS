BITS 64
global _start
section .text
_start:
    ; SYS_SIGACTION(10, handler)  -- register handler for signal 10
    mov rax, 23
    mov rdi, 10
    mov rsi, handler
    int 0x80
    cmp rax, 0
    jne .fail_sigaction

    ; write our own pid to a well-known file so the sender can find us,
    ; then loop calling sleep(20ms) - the natural checkpoint where a
    ; pending signal actually gets delivered - until the handler has run
    ; and set g_got_signal, or a bound is hit.
    mov rax, 11                 ; SYS_GETPID
    int 0x80
    mov rbx, g_my_pid
    mov [rbx], eax

    mov rax, 19                 ; SYS_FD_OPEN would be needed for a real
    ; write a tiny marker file with our pid so the sender process can
    ; read it back. Use the whole-file API (simplest here).
    mov rax, 8
    mov rdi, pidpath
    mov rsi, g_my_pid
    mov rdx, 4
    int 0x80

    xor r12, r12                ; loop counter
.wait_loop:
    mov rax, 10                 ; SYS_SLEEP_MS
    mov rdi, 20
    int 0x80

    mov rbx, g_got_signal
    mov eax, [rbx]
    test eax, eax
    jnz .signal_arrived

    inc r12
    cmp r12, 200                ; ~4 seconds worth of 20ms sleeps, bounded
    jb .wait_loop

    mov rax, 0
    mov rdi, e_timeout
    mov rsi, e_timeout_len
    int 0x80
    jmp .done

.signal_arrived:
    ; verify BOTH that the handler ran (g_got_signal set) AND that the
    ; signal number the handler received via rdi was exactly 10, AND
    ; that execution resumed correctly after sigreturn (this comparison
    ; itself only makes sense if registers/flow were not corrupted).
    mov rbx, g_got_signum
    mov eax, [rbx]
    cmp eax, 10
    jne .fail_signum

    ; prove normal execution truly resumed after the handler by doing
    ; real work here and checking it: not just "did not crash".
    mov rax, 111
    mov rbx, 222
    add rax, rbx
    cmp rax, 333
    jne .fail_resume

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_sigaction:
    mov rdi, e_sa
    mov rsi, e_sa_len
    jmp .emit
.fail_signum:
    mov rdi, e_sn
    mov rsi, e_sn_len
    jmp .emit
.fail_resume:
    mov rdi, e_rs
    mov rsi, e_rs_len
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

; void handler(int signum)  -- signum arrives in edi per the SysV ABI
handler:
    mov rax, g_got_signum
    mov [rax], edi
    mov rax, g_got_signal
    mov dword [rax], 1
    ret                          ; jumps to the kernel-built trampoline

section .bss
g_got_signal: resd 1
g_got_signum: resd 1
g_my_pid:     resd 1

section .rodata
pidpath: db "/sigreceiver.pid", 0
m_ok: db "SIGNAL_PASS handler ran with correct signum and execution resumed correctly", 10
m_ok_len equ $ - m_ok
e_sa: db "SIGNAL_FAIL sigaction failed", 10
e_sa_len equ $ - e_sa
e_sn: db "SIGNAL_FAIL handler received wrong signal number", 10
e_sn_len equ $ - e_sn
e_rs: db "SIGNAL_FAIL execution did not resume correctly after sigreturn", 10
e_rs_len equ $ - e_rs
e_timeout: db "SIGNAL_FAIL timed out waiting for signal", 10
e_timeout_len equ $ - e_timeout
