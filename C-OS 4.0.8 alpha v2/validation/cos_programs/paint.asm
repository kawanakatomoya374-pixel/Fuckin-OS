BITS 64
global _start
section .text
_start:
    ; SYS_WIN_CREATE(title, len, w, h) -> handle in rax
    mov rax, 2
    mov rdi, title
    mov rsi, title_len
    mov rdx, 400              ; width
    mov r10, 300              ; height
    int 0x80
    cmp rax, 0
    jle .fail
    mov rbx, rax              ; keep handle

    ; left half WHITE  : fill(handle, 0, 0, 200, 300-titlebar, 0xFFFFFF)
    mov rax, 3
    mov rdi, rbx
    mov rsi, 0
    mov rdx, 0
    mov r10, 200
    mov r8, 300
    mov r9, 0x00FFFFFF
    int 0x80

    ; right half BLACK : fill(handle, 200, 0, 200, 300, 0x000000)
    mov rax, 3
    mov rdi, rbx
    mov rsi, 200
    mov rdx, 0
    mov r10, 200
    mov r8, 300
    mov r9, 0x00000000
    int 0x80

    ; a grey band across the middle, to prove multiple queued rects and
    ; that later rects paint over earlier ones
    mov rax, 3
    mov rdi, rbx
    mov rsi, 0
    mov rdx, 130
    mov r10, 400
    mov r8, 30
    mov r9, 0x00808080
    int 0x80

    mov rax, 0
    mov rdi, ok_msg
    mov rsi, ok_len
    int 0x80
    jmp .done
.fail:
    mov rax, 0
    mov rdi, fail_msg
    mov rsi, fail_len
    int 0x80
.done:
    ; Exit rather than spin. An earlier version sat in `jmp $` to "keep the
    ; window alive", but a ring3 busy-loop is preemptively scheduled and
    ; therefore burns a full share of CPU forever - measurably starving the
    ; rest of the system (the browser self-test stopped completing in
    ; time). The window does not need this process alive: its contents are
    ; a command queue owned by the kernel, so the surface keeps rendering
    ; after the program exits.
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
title:    db "Black and White (.c-os)"
title_len equ $ - title
ok_msg:   db "COSWIN_PASS ring3 program created a window and drew into it", 10
ok_len    equ $ - ok_msg
fail_msg: db "COSWIN_FAIL SYS_WIN_CREATE returned an error", 10
fail_len  equ $ - fail_msg
