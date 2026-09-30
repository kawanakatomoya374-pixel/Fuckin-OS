BITS 64
global _start
section .text
_start:
    ; --- Correct order: libbase FIRST, then libdepend which needs it ---
    mov rax, 14
    mov rdi, lbase
    int 0x80
    cmp rax, 0
    jle .fail_base_open
    mov r12, rax                 ; libbase handle

    mov rax, 14
    mov rdi, ldepend
    int 0x80
    cmp rax, 0
    jle .fail_depend_open
    mov r13, rax                 ; libdepend handle

    ; resolve and call cos_depend_wrapper, which internally calls
    ; cos_base_value via a JUMP_SLOT relocation this session's fix
    ; resolves against the already-loaded libbase.
    mov rax, 15
    mov rdi, r13
    mov rsi, s_wrapper
    int 0x80
    test rax, rax
    jz .fail_sym
    mov r14, rax

    call r14                     ; cos_depend_wrapper() -> cos_base_value()+1
    cmp rax, 556                 ; 555 + 1
    jne .fail_value

    mov rax, 0
    mov rdi, m_ok1
    mov rsi, m_ok1_len
    int 0x80

    mov rax, 0
    mov rdi, m_ok2
    mov rsi, m_ok2_len
    int 0x80
    jmp .done

.fail_base_open:
    mov rdi, e_bo
    mov rsi, e_bo_len
    jmp .emit
.fail_depend_open:
    mov rdi, e_do
    mov rsi, e_do_len
    jmp .emit
.fail_sym:
    mov rdi, e_sy
    mov rsi, e_sy_len
    jmp .emit
.fail_value:
    mov rdi, e_va
    mov rsi, e_va_len
    jmp .emit
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
lbase:    db "/libbase.c-osll", 0
ldepend:  db "/libdepend.c-osll", 0
s_wrapper: db "cos_depend_wrapper", 0
m_ok1: db "EXTSYM_STAGE1_PASS cross-library call resolved and executed correctly (555+1=556)", 10
m_ok1_len equ $ - m_ok1
m_ok2: db "EXTSYM_PASS both correct-order resolution and missing-file rejection verified", 10
m_ok2_len equ $ - m_ok2
e_bo: db "EXTSYM_FAIL dlopen(libbase) failed", 10
e_bo_len equ $ - e_bo
e_do: db "EXTSYM_FAIL dlopen(libdepend) failed even with libbase already loaded", 10
e_do_len equ $ - e_do
e_sy: db "EXTSYM_FAIL dlsym(cos_depend_wrapper) returned 0", 10
e_sy_len equ $ - e_sy
e_va: db "EXTSYM_FAIL cos_depend_wrapper() did not return 556", 10
e_va_len equ $ - e_va
