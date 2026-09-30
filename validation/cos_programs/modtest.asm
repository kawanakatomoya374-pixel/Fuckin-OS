BITS 64
global _start
section .text
_start:
    mov rax, 2
    mov rdi, title
    mov rsi, title_len
    mov rdx, 300
    mov r10, 150
    int 0x80
    cmp rax, 0
    jle .fail
    mov rbx, handle
    mov [rbx], rax

    xor r12, r12
    xor r13, r13
.loop:
    mov rax, 29
    mov rbx, handle
    mov rdi, [rbx]
    mov rsi, ev
    int 0x80
    cmp rax, 1
    jne .no_key

    mov rbx, ev
    movzx ecx, byte [rbx]         ; ascii
    movzx edx, byte [rbx+2]       ; modifiers

    mov rbx, report
    mov [rbx+9], cl
    add edx, '0'
    mov [rbx+13], dl
    mov rax, 0
    mov rdi, report
    mov rsi, report_len
    int 0x80

    inc r12
    cmp r12, 4
    jae .done
    jmp .loop

.no_key:
    mov rax, 10
    mov rdi, 16
    int 0x80
    inc r13
    cmp r13, 6000
    jb .loop

.done:
    mov rax, 0
    mov rdi, m_done
    mov rsi, m_done_len
    int 0x80
    mov rax, 1
    mov rdi, 0
    int 0x80
.fail:
    mov rax, 1
    mov rdi, 1
    int 0x80

section .bss
handle: resq 1
ev: resb 3

section .data
; report must be WRITABLE (this program modifies it in place to build
; each line) - .rodata is mapped read-only by the ELF loader's W^X
; enforcement, and writing to it correctly faults and terminates the
; process. An earlier version of this file declared it in .rodata by
; mistake, which silently killed the program on its first received key
; and was the actual cause of a long, otherwise-inexplicable "modifier
; keys never get reported" investigation - the modifier-key feature
; itself, and every kernel-side piece of the keyboard/window pipeline,
; was correct the entire time.
report: db "MODKEY c=X m=0", 10
report_len equ $ - report

section .rodata
title: db "ModTest"
title_len equ $ - title
m_done: db "MODKEY_DONE", 10
m_done_len equ $ - m_done
