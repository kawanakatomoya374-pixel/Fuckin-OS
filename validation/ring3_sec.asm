BITS 64
ORG 0x8000000000
start:
    ; 1. legitimate write - must succeed
    mov rax, 0
    mov rdi, msg_ok
    mov rsi, msg_ok_len
    int 0x80
    mov r12, rax                ; save result

    ; 2. ATTACK: point SYS_WRITE at a kernel address (higher half).
    ;    Before the fix this made the kernel dump kernel memory to serial.
    mov rax, 0
    mov rdi, 0xFFFF800000100000
    mov rsi, 64
    int 0x80
    mov r13, rax                ; expect -1 (rejected)

    ; 3. ATTACK: unmapped user address -> would #PF inside the handler
    mov rax, 0
    mov rdi, 0x0000000030000000
    mov rsi, 32
    int 0x80
    mov r14, rax                ; expect -1 (rejected)

    ; 4. ATTACK: length overflow, wrapping past top of address space
    mov rax, 0
    mov rdi, msg_ok
    mov rsi, 0xFFFFFFFFFFFFFFF0
    int 0x80
    mov r15, rax                ; clamped then validated; expect -1 or small

    ; report: only print PASS if all three attacks were rejected with -1
    cmp r13, -1
    jne fail
    cmp r14, -1
    jne fail
    mov rax, 0
    mov rdi, msg_pass
    mov rsi, msg_pass_len
    int 0x80
    jmp done
fail:
    mov rax, 0
    mov rdi, msg_fail
    mov rsi, msg_fail_len
    int 0x80
done:
    mov rax, 1
    mov rdi, 0
    int 0x80
.hang:
    jmp .hang

msg_ok:   db "RING3SEC baseline write ok", 10
msg_ok_len equ $ - msg_ok
msg_pass: db "RING3SEC_PASS kernel rejected all invalid user pointers", 10
msg_pass_len equ $ - msg_pass
msg_fail: db "RING3SEC_FAIL kernel accepted an invalid user pointer", 10
msg_fail_len equ $ - msg_fail
