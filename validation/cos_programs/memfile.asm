BITS 64
global _start
section .text
_start:
    ; --- SYS_SBRK(0): query current break ---
    mov rax, 6
    mov rdi, 0
    int 0x80
    cmp rax, -1
    je .fail_brk
    mov r12, rax                ; initial break

    ; --- SYS_SBRK(8192): grow heap by 2 pages, returns OLD break ---
    mov rax, 6
    mov rdi, 8192
    int 0x80
    cmp rax, -1
    je .fail_brk
    mov r13, rax                ; old break = start of new region
    cmp r13, r12
    jne .fail_brk               ; must equal the break we just queried

    ; --- actually USE the memory (proves demand paging works) ---
    mov rbx, r13
    mov rax, 0xCAFEBABE12345678
    mov [rbx], rax
    mov rcx, [rbx]
    mov rdx, 0xCAFEBABE12345678
    cmp rcx, rdx
    jne .fail_mem

    ; write at the far end of the newly granted region too
    mov rbx, r13
    add rbx, 8000
    mov rax, 0x1122334455667788
    mov [rbx], rax
    mov rcx, [rbx]
    mov rdx, 0x1122334455667788
    cmp rcx, rdx
    jne .fail_mem

    mov rax, 0
    mov rdi, ok_mem
    mov rsi, ok_mem_len
    int 0x80

    ; --- SYS_WRITE_FILE(path, data, len) ---
    mov rax, 8
    mov rdi, fpath
    mov rsi, fdata
    mov rdx, fdata_len
    int 0x80
    cmp rax, fdata_len
    jne .fail_write

    ; --- SYS_READ_FILE(path, heap_buffer, 64) : read back into the heap ---
    mov rax, 7
    mov rdi, fpath
    mov rsi, r13                ; destination = our heap allocation
    mov rdx, 64
    int 0x80
    cmp rax, fdata_len
    jne .fail_read

    ; verify contents byte by byte
    mov rsi, r13
    mov rdi, fdata
    mov rcx, fdata_len
    xor rbx, rbx
.cmp:
    mov al, [rsi+rbx]
    cmp al, [rdi+rbx]
    jne .fail_cmp
    inc rbx
    cmp rbx, rcx
    jb .cmp

    mov rax, 0
    mov rdi, ok_all
    mov rsi, ok_all_len
    int 0x80
    jmp .done

.fail_brk:
    mov rax, 0
    mov rdi, e_brk
    mov rsi, e_brk_len
    int 0x80
    jmp .done
.fail_mem:
    mov rax, 0
    mov rdi, e_mem
    mov rsi, e_mem_len
    int 0x80
    jmp .done
.fail_write:
    mov rax, 0
    mov rdi, e_wr
    mov rsi, e_wr_len
    int 0x80
    jmp .done
.fail_read:
    mov rax, 0
    mov rdi, e_rd
    mov rsi, e_rd_len
    int 0x80
    jmp .done
.fail_cmp:
    mov rax, 0
    mov rdi, e_cmp
    mov rsi, e_cmp_len
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .rodata
fpath:  db "/memfile.dat", 0
fdata:  db "COS heap and file IO round trip"
fdata_len equ $ - fdata
ok_mem: db "MEMFILE heap grown and written/read back OK", 10
ok_mem_len equ $ - ok_mem
ok_all: db "MEMFILE_PASS sbrk + file write + file read into heap all verified", 10
ok_all_len equ $ - ok_all
e_brk:  db "MEMFILE_FAIL sbrk failed", 10
e_brk_len equ $ - e_brk
e_mem:  db "MEMFILE_FAIL heap memory did not read back", 10
e_mem_len equ $ - e_mem
e_wr:   db "MEMFILE_FAIL file write failed", 10
e_wr_len equ $ - e_wr
e_rd:   db "MEMFILE_FAIL file read failed", 10
e_rd_len equ $ - e_rd
e_cmp:  db "MEMFILE_FAIL file contents did not match", 10
e_cmp_len equ $ - e_cmp
