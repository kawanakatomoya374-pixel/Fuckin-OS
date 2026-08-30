; smp_trampoline.asm - x86-64 AP bootstrap for C-OS
;
; The BSP copies this position-independent image to physical 0x9000 and starts
; each AP with a Local-APIC SIPI vector of 0x09.  The low-memory mailbox at
; 0x8000 is filled by smp.c with the BSP's current CR3, GDT descriptor, a
; private AP runtime stack, and the C entry point.  APs enter a bounded
; AP-only work loop after bootstrap; this never touches the still-UP scheduler
; run queue or global GUI/VRAM state.

%define SMP_MAILBOX_PHYS       0x8000
%define SMP_MB_CR3             (SMP_MAILBOX_PHYS + 0x00)
%define SMP_MB_GDT             (SMP_MAILBOX_PHYS + 0x08)
%define SMP_MB_STACK           (SMP_MAILBOX_PHYS + 0x18)
%define SMP_MB_ENTRY           (SMP_MAILBOX_PHYS + 0x20)
%define SMP_TRAMPOLINE_PHYS    0x9000

section .boot.data align=16
bits 16

global smp_ap_trampoline_start
global smp_ap_trampoline_end
smp_ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, SMP_TRAMPOLINE_PHYS

    ; The BSP kernel GDT currently has only a 64-bit code selector at 0x08
    ; and no 32-bit code selector at 0x38.  AP bootstrap needs both modes,
    ; so load the position-independent trampoline GDT instead of assuming
    ; a particular kernel GDT layout.
    lgdt [abs (SMP_TRAMPOLINE_PHYS + smp_ap_local_gdt_desc - smp_ap_trampoline_start)]

    mov eax, cr0
    or eax, 0x00000001          ; protected mode
    mov cr0, eax
    jmp 0x08:(SMP_TRAMPOLINE_PHYS + smp_ap_protected - smp_ap_trampoline_start)

bits 32
smp_ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, dword [abs SMP_MB_CR3]
    mov cr3, eax

    mov eax, cr4
    ; PAE plus OSFXSR/OSXMMEXCPT: optimized C code copies AP work items
    ; with SSE2 (movdqa), so every AP must establish the same XMM state
    ; permissions as the BSP before entering the worker loop.
    or eax, 0x620               ; PAE | OSFXSR | OSXMMEXCPT
    mov cr4, eax

    mov ecx, 0xC0000080         ; IA32_EFER
    rdmsr
    or eax, 0x100               ; LME
    wrmsr

    mov eax, cr0
    or eax, 0x80000001          ; paging + protected mode
    mov cr0, eax
    jmp 0x18:(SMP_TRAMPOLINE_PHYS + smp_ap_long_mode - smp_ap_trampoline_start)

bits 64
smp_ap_long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [abs SMP_MB_STACK]
    and rsp, -16
    mov rax, [abs SMP_MB_ENTRY]
    call rax

smp_ap_park:
    cli
    hlt
    jmp smp_ap_park

align 8
smp_ap_local_gdt:
    dq 0x0000000000000000  ; null
    dq 0x00CF9A000000FFFF  ; 32-bit code, selector 0x08
    dq 0x00CF92000000FFFF  ; data, selector 0x10
    dq 0x00AF9A000000FFFF  ; 64-bit code, selector 0x18
smp_ap_local_gdt_end:
smp_ap_local_gdt_desc:
    dw smp_ap_local_gdt_end - smp_ap_local_gdt - 1
    dd SMP_TRAMPOLINE_PHYS + smp_ap_local_gdt - smp_ap_trampoline_start

align 16
smp_ap_trampoline_end:
