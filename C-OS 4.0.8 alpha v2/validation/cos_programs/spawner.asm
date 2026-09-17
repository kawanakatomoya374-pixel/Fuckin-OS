BITS 64
global _start
section .text
_start:
    ; --- SYS_GETPID ---
    mov rax, 11
    int 0x80
    cmp rax, 0
    jle .fail_pid
    mov r12, rax                ; my pid

    mov rax, 0
    mov rdi, m_pid
    mov rsi, m_pid_len
    int 0x80

    ; --- SYS_YIELD: must return without hanging ---
    mov rax, 9
    int 0x80

    ; --- SYS_SLEEP_MS(50): must return, and not busy-spin ---
    mov rax, 10
    mov rdi, 50
    int 0x80

    mov rax, 0
    mov rdi, m_sleep
    mov rsi, m_sleep_len
    int 0x80

    ; --- SYS_SPAWN("/diskhello.c-os") ---
    mov rax, 12
    mov rdi, spath
    int 0x80
    cmp rax, 0
    jle .fail_spawn
    mov r13, rax                ; child pid

    ; child pid must differ from ours
    cmp r13, r12
    je .fail_same

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_pid:
    mov rax, 0
    mov rdi, e_pid
    mov rsi, e_pid_len
    int 0x80
    jmp .done
.fail_spawn:
    mov rax, 0
    mov rdi, e_spawn
    mov rsi, e_spawn_len
    int 0x80
    jmp .done
.fail_same:
    mov rax, 0
    mov rdi, e_same
    mov rsi, e_same_len
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
spath:   db "/diskhello.c-os", 0
m_pid:   db "SPAWNER got a valid pid from SYS_GETPID", 10
m_pid_len equ $ - m_pid
m_sleep: db "SPAWNER yield and sleep(50ms) both returned", 10
m_sleep_len equ $ - m_sleep
m_ok:    db "SPAWNER_PASS getpid + yield + sleep + spawned a child process", 10
m_ok_len equ $ - m_ok
e_pid:   db "SPAWNER_FAIL SYS_GETPID returned an invalid pid", 10
e_pid_len equ $ - e_pid
e_spawn: db "SPAWNER_FAIL SYS_SPAWN failed", 10
e_spawn_len equ $ - e_spawn
e_same:  db "SPAWNER_FAIL child pid equals parent pid", 10
e_same_len equ $ - e_same
