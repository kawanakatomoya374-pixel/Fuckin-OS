BITS 64
global _start
section .text
_start:
    ; --- mmap(4096, READ|WRITE) -> addr1 ---
    mov rax, 26
    mov rdi, 4096
    mov rsi, 3                  ; READ|WRITE
    int 0x80
    cmp rax, -1
    je .fail_mmap1
    mov r12, rax                ; addr1

    ; write/read at addr1 (proves demand paging works for mmap)
    mov rax, 0xDEADBEEFCAFEBABE
    mov [r12], rax
    mov rbx, [r12]
    cmp rbx, rax
    jne .fail_rw1

    ; --- mmap(8192, READ|WRITE) -> addr2, must NOT overlap addr1's 4096 ---
    mov rax, 26
    mov rdi, 8192
    mov rsi, 3
    int 0x80
    cmp rax, -1
    je .fail_mmap2
    mov r13, rax                ; addr2

    ; addr2 must be >= addr1 + 4096 (bump allocator, no overlap)
    mov rax, r12
    add rax, 4096
    cmp r13, rax
    jl .fail_overlap

    ; write at the far end of addr2's region (proves the FULL requested
    ; length is usable, not just the first page)
    mov rax, r13
    add rax, 8000
    mov qword [rax], 0x1122334455667788
    mov rbx, [rax]
    cmp rbx, 0x1122334455667788
    jne .fail_rw2

    ; --- mmap(4096, READ only) -> addr3, writing to it must crash ---
    mov rax, 26
    mov rdi, 4096
    mov rsi, 1                  ; READ only, no WRITE
    int 0x80
    cmp rax, -1
    je .fail_mmap3
    mov r14, rax                ; addr3

    ; read is fine (page not yet mapped -> demand-paged as read-only,
    ; zero-filled)
    mov rbx, [r14]
    cmp rbx, 0
    jne .fail_readonly_zero

    ; --- munmap(addr1, 4096) ---
    mov rax, 27
    mov rdi, r12
    mov rsi, 4096
    int 0x80
    cmp rax, 0
    jne .fail_munmap

    ; munmap with the WRONG length must fail (exact-match requirement)
    mov rax, 26
    mov rdi, 4096
    mov rsi, 3
    int 0x80
    mov r15, rax                 ; a fourth region, addr4
    mov rax, 27
    mov rdi, r15
    mov rsi, 8192                ; wrong length on purpose
    int 0x80
    cmp rax, 0
    je .fail_wrong_len_accepted

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_mmap1:  mov rdi, e1
              mov rsi, e1_len
              jmp .emit
.fail_rw1:    mov rdi, e2
              mov rsi, e2_len
              jmp .emit
.fail_mmap2:  mov rdi, e3
              mov rsi, e3_len
              jmp .emit
.fail_overlap: mov rdi, e4
               mov rsi, e4_len
               jmp .emit
.fail_rw2:    mov rdi, e5
              mov rsi, e5_len
              jmp .emit
.fail_mmap3:  mov rdi, e6
              mov rsi, e6_len
              jmp .emit
.fail_readonly_zero: mov rdi, e7
                     mov rsi, e7_len
                     jmp .emit
.fail_munmap: mov rdi, e8
              mov rsi, e8_len
              jmp .emit
.fail_wrong_len_accepted: mov rdi, e9
                          mov rsi, e9_len
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
m_ok: db "MMAP_PASS multi-region alloc, RW enforcement, munmap, and exact-match check all verified", 10
m_ok_len equ $ - m_ok
e1: db "MMAP_FAIL first mmap failed", 10
e1_len equ $ - e1
e2: db "MMAP_FAIL read/write at addr1 mismatch", 10
e2_len equ $ - e2
e3: db "MMAP_FAIL second mmap failed", 10
e3_len equ $ - e3
e4: db "MMAP_FAIL addr2 overlaps addr1's region", 10
e4_len equ $ - e4
e5: db "MMAP_FAIL read/write at far end of addr2 mismatch", 10
e5_len equ $ - e5
e6: db "MMAP_FAIL read-only mmap failed", 10
e6_len equ $ - e6
e7: db "MMAP_FAIL read-only region was not zero-filled", 10
e7_len equ $ - e7
e8: db "MMAP_FAIL munmap of a real region failed", 10
e8_len equ $ - e8
e9: db "MMAP_FAIL munmap with mismatched length was wrongly accepted", 10
e9_len equ $ - e9
