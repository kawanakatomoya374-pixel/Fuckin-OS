BITS 64
section .text
global cos_add
global cos_mul
global cos_counter_bump

cos_add:
    mov rax, rdi
    add rax, rsi
    ret

cos_mul:
    mov rax, rdi
    imul rax, rsi
    ret

; Uses a global in .data, which forces a real R_X86_64_RELATIVE relocation
; via the pointer table below - the whole point of testing a shared object.
cos_counter_bump:
    mov rax, [rel counter_ptr]
    mov rcx, [rax]
    inc rcx
    mov [rax], rcx
    mov rax, rcx
    ret

section .data
counter:     dq 100
counter_ptr: dq counter        ; needs relocating when base != 0
