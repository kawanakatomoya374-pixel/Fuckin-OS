BITS 64
global _start
section .text
_start:
    ; Loads ONLY libdepend.c-osll - never libbase - so
    ; cos_depend_wrapper's JUMP_SLOT relocation for cos_base_value
    ; cannot be resolved by anything. This must be REJECTED at load
    ; time, not silently mis-loaded with a dangling/zero call target.
    mov rax, 14
    mov rdi, ldepend
    int 0x80
    cmp rax, 0
    jg .fail_should_have_rejected

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done
.fail_should_have_rejected:
    mov rdi, e_bad
    mov rsi, e_bad_len
    jmp .emit
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
ldepend: db "/libdepend.c-osll", 0
m_ok: db "EXTSYM_MISSING_PASS dlopen correctly rejected a library with an unresolvable external symbol", 10
m_ok_len equ $ - m_ok
e_bad: db "EXTSYM_MISSING_FAIL dlopen succeeded despite libbase never being loaded - dangling call target", 10
e_bad_len equ $ - e_bad
