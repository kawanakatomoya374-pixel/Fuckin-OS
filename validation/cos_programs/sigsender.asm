BITS 64
global _start
section .text
_start:
    ; Wait a little to let the receiver register its handler and write
    ; its pid marker file first.
    mov rax, 10
    mov rdi, 100
    int 0x80

    ; SYS_READ_FILE(pidpath, buf, 4)
    mov rax, 7
    mov rdi, pidpath
    mov rsi, pidbuf
    mov rdx, 4
    int 0x80
    cmp rax, 4
    jne .fail_read

    mov rbx, pidbuf
    mov eax, [rbx]

    ; SYS_KILL(target_pid, 10)
    mov rax, 24
    mov rdi, rax                ; placeholder overwritten below
    mov edi, [rbx]              ; target pid from the marker file
    mov rsi, 10
    mov rax, 24
    int 0x80
    cmp rax, 0
    jne .fail_kill

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_read:
    mov rdi, e_r
    mov rsi, e_r_len
    jmp .emit
.fail_kill:
    mov rdi, e_k
    mov rsi, e_k_len
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .bss
pidbuf: resd 1

section .rodata
pidpath: db "/sigreceiver.pid", 0
m_ok: db "SIGSENDER sent signal 10 to the receiver process", 10
m_ok_len equ $ - m_ok
e_r: db "SIGSENDER_FAIL could not read receiver pid file", 10
e_r_len equ $ - e_r
e_k: db "SIGSENDER_FAIL SYS_KILL failed", 10
e_k_len equ $ - e_k
