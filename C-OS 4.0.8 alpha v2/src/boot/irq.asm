; C-OS 4.0.6 IRQ Stubs (64-bit Long Mode)
; Each IRQ stub saves all registers, calls _irq_handler, sends EOI, restores.

[BITS 64]

section .text

; IRQ macro: push dummy error code + IRQ number, jump to common stub
%macro IRQ 2
global irq%1
irq%1:
    push qword 0        ; dummy error code
    push qword %2       ; interrupt number
    jmp irq_common_stub
%endmacro

extern _irq_handler
global irq_common_stub

irq_common_stub:
    ; Save all general-purpose registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Pass pointer to stack frame as first argument (rdi = System V AMD64 ABI)
    mov rdi, rsp
    call _irq_handler

    ; Restore all general-purpose registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         ; remove int_no and err_code
    iretq               ; 64-bit interrupt return

; Define all 16 IRQ stubs
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47
