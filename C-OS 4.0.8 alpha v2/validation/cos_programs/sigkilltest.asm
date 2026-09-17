BITS 64
global _start
section .text
_start:
    ; register a handler for signal 9 - must be IGNORED by the kernel,
    ; which force-terminates on signal 9 regardless.
    mov rax, 23
    mov rdi, 9
    mov rsi, handler
    int 0x80

    ; SYS_KILL(self, 9)
    mov rax, 11
    int 0x80
    mov edi, eax
    mov rax, 24
    mov rsi, 9
    int 0x80

    ; SYS_SLEEP_MS is the checkpoint where delivery happens - must not
    ; return, since the process should be terminated before this resumes.
    mov rax, 10
    mov rdi, 50
    int 0x80

    ; unreachable if signal 9 correctly terminated the process
    mov rax, 0
    mov rdi, e_survived
    mov rsi, e_survived_len
    int 0x80
    mov rax, 1
    mov rdi, 1
    int 0x80

handler:
    ; must never run - signal 9 cannot be caught
    mov rax, 0
    mov rdi, e_handler_ran
    mov rsi, e_handler_ran_len
    int 0x80
    ret

section .rodata
e_survived: db "SIGKILL_FAIL process survived past a self-sent signal 9", 10
e_survived_len equ $ - e_survived
e_handler_ran: db "SIGKILL_FAIL registered handler for signal 9 actually ran", 10
e_handler_ran_len equ $ - e_handler_ran
