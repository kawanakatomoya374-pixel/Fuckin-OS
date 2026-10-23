extern cos_smap_active
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
    ; SMAP: ring 0 may only touch user memory deliberately (stac/clac around the access). Never trust the AC flag the
    ; interrupted code had - user code can set it - so clear it on every entry. Skipped on CPUs without SMAP.
    cmp byte [rel cos_smap_active], 0
    je .smap_off
    clac
.smap_off:
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

    ; The kernel itself is built with SSE2 (-msse2), so the C handler below
    ; freely clobbers XMM0-15 / MXCSR / x87 state. Without saving it here, any
    ; interrupt silently corrupts the vector registers of whatever user code
    ; was running (and the scheduler's later fxsave would capture the
    ; clobbered values). Park the interrupted FPU/SSE state in a 16-byte
    ; aligned 512-byte area on this thread's own kernel stack; rbx is
    ; callee-saved and keeps the original frame pointer across the call.
    mov rbx, rsp
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]

    ; Pass pointer to stack frame as first argument (rdi = System V AMD64 ABI)
    mov rdi, rbx
    call _irq_handler

    fxrstor64 [rsp]
    mov rsp, rbx

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

; Vectors 0x40-0x5F are for interrupts that arrive through the Local APIC (MSI, I/O APIC routes taken by kernel modules); 0xFF is the APIC's spurious vector.
; _irq_handler ends these with a Local APIC EOI instead of the PIC's (and sends none for 0xFF).
%macro APICVEC 1
apic_vec_%1:
    push qword 0
    push qword %1
    jmp irq_common_stub
section .rodata
    dq apic_vec_%1
section .text
%endmacro

section .rodata
align 8
global cos_apic_stubs
cos_apic_stubs:
section .text
%assign apic_n 0x40
%rep 32
    APICVEC %[apic_n]
%assign apic_n apic_n+1
%endrep

; The SMP scheduler's own vectors (smp.h): 0xD0 tick, 0xE0 kick, 0xF1 TLB shootdown.
section .rodata
align 8
global cos_ipi_stubs
cos_ipi_stubs:
section .text
APICVEC 0xD0
APICVEC 0xE0
APICVEC 0xF1

global cos_apic_spurious_stub
cos_apic_spurious_stub:
    push qword 0
    push qword 0xFF
    jmp irq_common_stub
