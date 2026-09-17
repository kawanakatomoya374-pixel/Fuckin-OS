BITS 64
global _start
section .text
_start:
    ; SYS_DLOPEN("/libmath.c-osll")
    mov rax, 14
    mov rdi, lpath
    int 0x80
    cmp rax, 0
    jle .fail_open
    mov r12, rax                ; handle

    ; SYS_DLSYM(handle, "cos_add")
    mov rax, 15
    mov rdi, r12
    mov rsi, s_add
    int 0x80
    test rax, rax
    jz .fail_sym
    mov r13, rax                ; &cos_add

    ; SYS_DLSYM(handle, "cos_counter_bump")
    mov rax, 15
    mov rdi, r12
    mov rsi, s_bump
    int 0x80
    test rax, rax
    jz .fail_sym
    mov r14, rax                ; &cos_counter_bump

    ; call cos_add(30, 12) -> must be 42
    mov rdi, 30
    mov rsi, 12
    call r13
    cmp rax, 42
    jne .fail_add

    ; call cos_counter_bump() twice. The counter starts at 100 and is
    ; reached through a RELOCATED pointer - if R_X86_64_RELATIVE was not
    ; applied this dereferences a wrong address and will not return 101/102.
    call r14
    cmp rax, 101
    jne .fail_reloc
    call r14
    cmp rax, 102
    jne .fail_reloc

    ; an unknown symbol must resolve to 0, not to garbage
    mov rax, 15
    mov rdi, r12
    mov rsi, s_none
    int 0x80
    test rax, rax
    jnz .fail_none

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_open:
    mov rax, 0
    mov rdi, e_open
    mov rsi, e_open_len
    int 0x80
    jmp .done
.fail_sym:
    mov rax, 0
    mov rdi, e_sym
    mov rsi, e_sym_len
    int 0x80
    jmp .done
.fail_add:
    mov rax, 0
    mov rdi, e_add
    mov rsi, e_add_len
    int 0x80
    jmp .done
.fail_reloc:
    mov rax, 0
    mov rdi, e_rel
    mov rsi, e_rel_len
    int 0x80
    jmp .done
.fail_none:
    mov rax, 0
    mov rdi, e_none
    mov rsi, e_none_len
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
lpath:  db "/libmath.c-osll", 0
s_add:  db "cos_add", 0
s_bump: db "cos_counter_bump", 0
s_none: db "definitely_not_exported", 0
m_ok:   db "DLL_PASS dlopen + dlsym + called library code + relocations verified", 10
m_ok_len equ $ - m_ok
e_open: db "DLL_FAIL dlopen failed", 10
e_open_len equ $ - e_open
e_sym:  db "DLL_FAIL dlsym returned 0 for an exported symbol", 10
e_sym_len equ $ - e_sym
e_add:  db "DLL_FAIL cos_add(30,12) did not return 42", 10
e_add_len equ $ - e_add
e_rel:  db "DLL_FAIL relocated data pointer gave wrong counter value", 10
e_rel_len equ $ - e_rel
e_none: db "DLL_FAIL unknown symbol resolved to non-zero", 10
e_none_len equ $ - e_none
