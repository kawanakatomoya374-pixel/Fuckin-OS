extern cos_xsave_on
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
    sub rsp, 1088           ; COS_FPU_STACK (task.h): XSAVE area (1024) + 64 for the alignment
    and rsp, -64
    ; which format this entry saved is recorded IN the area (offset 1016, past everything XSAVE/FXSAVE write): a register would have to survive the C handler and any task switch inside it
    cmp byte [rel cos_xsave_on], 0
    je .fx_save
    mov qword [rsp+1016], 1
    ; XSAVE only sets the XSTATE_BV bits of the components it saves and leaves the other bits of the 64-byte header as they were: all 64 bytes are cleared first, or stale stack bytes make the XRSTOR later fault (#GP)
    mov qword [rsp+512], 0
    mov qword [rsp+520], 0
    mov qword [rsp+528], 0
    mov qword [rsp+536], 0
    mov qword [rsp+544], 0
    mov qword [rsp+552], 0
    mov qword [rsp+560], 0
    mov qword [rsp+568], 0
    mov eax, -1
    mov edx, -1
    xsave64 [rsp]
    jmp .fx_saved
.fx_save:
    mov qword [rsp+1016], 0
    fxsave64 [rsp]
.fx_saved:

    mov rdi, rbx
    call _isr_handler

    cmp qword [rsp+1016], 0
    je .fx_rest
    mov eax, -1
    mov edx, -1
    xrstor64 [rsp]
    jmp .fx_restd
.fx_rest:
    fxrstor64 [rsp]
.fx_restd:
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

; The SYSCALL instruction (EFER.SCE, MSRs set by cos_syscall_init_cpu in gdt.c). On entry: rcx = the return RIP, r11 = the user RFLAGS, rsp still the user's, interrupts off
; (SFMASK), CS/SS are the kernel's. The thread's kernel stack top is in this CPU's cos_percpu_t (GS base, never swapped). The frame built here is the one the #UD gate
; (vector 6) pushes for the `syscall` bytes: RIP points at them, so cos_linux_try_syscall() runs unchanged and the frame returns through isr_common_stub's iretq.
global cos_syscall_entry
cos_syscall_entry:
    mov [gs:8], rsp
    mov rsp, [gs:0]
    push qword 0x23         ; user SS
    push qword [gs:8]       ; user RSP
    push r11                ; user RFLAGS
    push qword 0x1B         ; user CS
    sub rcx, 2
    push rcx                ; RIP of the syscall instruction (the handler adds 2 again)
    push qword 0            ; error code
    push qword 6            ; vector 6: the Linux layer's entry
    jmp isr_common_stub

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
