; =============================================================================
; context_switch.asm - Real Multitasking Context Switch Implementation
; C-OS 4.0.7 - 64-bit Long Mode
; =============================================================================

[BITS 64]
section .text

extern thread_get_current
extern thread_exit

global hal_context_switch
hal_context_switch:
    ; rdi = old_context, rsi = new_context

    ; Save old context.
    ;
    ; Caller-saved registers (RAX/RCX/RDX/RSI/RDI/R8-R11) are captured
    ; from the interrupt frame by scheduler_capture_interrupt_context()
    ; on the preemptive path. We intentionally do not overwrite them
    ; here with the C call arguments (old_context/new_context), because
    ; that would smear pointer values into the thread state on every
    ; switch.
    mov [rdi + 0],   r15
    mov [rdi + 8],   r14
    mov [rdi + 16],  r13
    mov [rdi + 24],  r12
    mov [rdi + 32],  r11
    mov [rdi + 40],  r10
    mov [rdi + 48],  r9
    mov [rdi + 56],  r8
    mov [rdi + 64],  rbp
    mov [rdi + 104], rbx

    mov rax, [rsp]
    mov [rdi + 120], rax
    mov rax, cs
    mov [rdi + 128], rax
    pushfq
    pop rax
    mov [rdi + 136], rax
    ; NOTE: this routine is entered via `call`, so at this point [rsp] still
    ; holds the return address we just saved into rip above. If we resume
    ; this thread later, iretq loads rip directly (not via a `ret`), so the
    ; return-address slot is never popped through the normal call mechanism.
    ; We must therefore save rsp+8 here -- the value rsp *would* have after
    ; the caller's `call` is retired by an ordinary `ret` -- otherwise every
    ; cooperative switch (thread_yield/scheduler_switch_task) leaves the
    ; resumed thread's stack permanently misaligned by 8 bytes, corrupting
    ; every local variable access from that point on.
    lea rax, [rsp + 8]
    mov [rdi + 144], rax
    mov rax, ss
    mov [rdi + 152], rax

    ; Use RBX as the immutable base pointer to the new context.
    mov rbx, rsi

    ; Select return path from the new CS selector.
    mov ecx, dword [rbx + 128]

    ; Restore the non-control registers first.
    mov r15, [rbx + 0]
    mov r14, [rbx + 8]
    mov r13, [rbx + 16]
    mov r12, [rbx + 24]
    mov r11, [rbx + 32]
    mov r10, [rbx + 40]
    mov r9,  [rbx + 48]
    mov r8,  [rbx + 56]
    mov rbp, [rbx + 64]
    mov rdi, [rbx + 72]
    mov rsi, [rbx + 80]
    mov rdx, [rbx + 88]

    test ecx, 3
    jnz .to_user

.to_kernel:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; NOTE: in 64-bit long mode, iretq *always* pops RIP/CS/RFLAGS/RSP/SS
    ; (all 5 fields) even when there is no privilege-level change -- this
    ; is a well-known difference from legacy 32-bit iret, which only pops
    ; 3 fields (RIP/CS/RFLAGS) for a same-privilege return. All 5 fields
    ; must be pushed here, or iretq silently pops RSP/SS from whatever
    ; happens to be next on the stack (zero, on a freshly allocated
    ; stack -- which is exactly what caused an immediate RSP=0/SS=0
    ; triple fault the moment a new thread reached task_entry_wrapper).
    push qword [rbx + 152]   ; ss
    push qword [rbx + 144]   ; rsp
    push qword [rbx + 136]   ; rflags
    push qword [rbx + 128]   ; cs (kernel code selector for this thread)
    push qword [rbx + 120]   ; rip

    mov rax, [rbx + 112]
    mov rcx, [rbx + 96]
    mov rbx, [rbx + 104]
    iretq

.to_user:
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push qword [rbx + 152]
    push qword [rbx + 144]
    push qword [rbx + 136]
    push qword [rbx + 128]
    push qword [rbx + 120]

    mov rax, [rbx + 112]
    mov rcx, [rbx + 96]
    mov rbx, [rbx + 104]
    iretq


global task_entry_wrapper
task_entry_wrapper:
    call rax
    xor esi, esi
    call thread_get_current
    test rax, rax
    jz .halt
    mov rdi, rax
    xor esi, esi
    call thread_exit

.halt:
    ud2

global get_current_rsp
get_current_rsp:
    mov rax, rsp
    ret

global get_current_rip
get_current_rip:
    mov rax, [rsp]
    ret
