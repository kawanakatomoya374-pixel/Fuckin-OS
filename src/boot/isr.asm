extern cos_smap_active
; C-OS 4.0.6 ISR Stubs (64-bit Long Mode)
; Exception handlers (ISR 0-31)

[BITS 64]

section .text

extern _isr_handler
global isr_common_stub

; ISR without error code: push dummy 0 + interrupt number
%macro ISR_NOERRCODE 1
global isr%1
isr%1:
    push qword 0        ; dummy error code
    push qword %1       ; interrupt number
    jmp isr_common_stub
%endmacro

; ISR with error code: CPU already pushed error code, just push interrupt number
%macro ISR_ERRCODE 1
global isr%1
isr%1:
    push qword %1       ; interrupt number (error code already on stack)
    jmp isr_common_stub
%endmacro

isr_common_stub:
    ; SMAP: ring 0 may only touch user memory deliberately (stac/clac around the access). Never trust the AC flag the
    ; interrupted code had - user code can set it - so clear it on every entry. Skipped on CPUs without SMAP.
    cmp byte [rel cos_smap_active], 0
    je .smap_off
    clac
.smap_off:
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

    ; Same reasoning as irq_common_stub: the kernel is built with SSE2, so
    ; the C handler (notably the page-fault path that demand-pages user
    ; heap/stack) clobbers XMM0-15/MXCSR. A fault in the middle of a SIMD
    ; loop in user code would otherwise resume with corrupted vector
    ; registers. Park the FPU/SSE state on this stack, 16-byte aligned;
    ; rbx (callee-saved) keeps the original frame pointer across the call.
    mov rbx, rsp
    sub rsp, 512
    and rsp, -16
    fxsave64 [rsp]

    mov rdi, rbx
    call _isr_handler

    fxrstor64 [rsp]
    mov rsp, rbx

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

    add rsp, 16
    iretq

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_ERRCODE   21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

; Syscall entry point (int 0x80), invoked by ring3 user code. No CPU-pushed
; error code, same as the other exception vectors above.
ISR_NOERRCODE 128
