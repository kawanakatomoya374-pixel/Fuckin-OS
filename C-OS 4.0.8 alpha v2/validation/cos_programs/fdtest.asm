BITS 64
global _start
section .text
_start:
    ; --- open for write (create), write in TWO chunks, close ---
    mov rax, 16
    mov rdi, fpath
    mov rsi, 2                  ; COS_O_WRONLY_CREATE
    int 0x80
    cmp rax, 0
    jle .fail_open_w
    mov r12, rax

    mov rax, 19
    mov rdi, r12
    mov rsi, chunk1
    mov rdx, chunk1_len
    int 0x80
    cmp rax, chunk1_len
    jne .fail_write

    mov rax, 19
    mov rdi, r12
    mov rsi, chunk2
    mov rdx, chunk2_len
    int 0x80
    cmp rax, chunk2_len
    jne .fail_write

    mov rax, 22
    mov rdi, r12
    int 0x80
    cmp rax, 0
    jne .fail_close

    ; --- open for read, read in two short chunks (proves seek position
    ;     persists between calls on the SAME fd) ---
    mov rax, 16
    mov rdi, fpath
    mov rsi, 1                  ; COS_O_RDONLY
    int 0x80
    cmp rax, 0
    jle .fail_open_r
    mov r12, rax

    mov rax, 18
    mov rdi, r12
    mov rsi, rbuf
    mov rdx, 5                  ; read only "AAAAA" first
    int 0x80
    cmp rax, 5
    jne .fail_read1

    mov rax, 18
    mov rdi, r12
    mov rsi, rbuf2
    mov rdx, 5                  ; must continue from byte 5, i.e. "BBBBB"
    int 0x80
    cmp rax, 5
    jne .fail_read2

    ; verify second chunk is what chunk2 actually was
    mov rsi, rbuf2
    mov rdi, chunk2
    mov rcx, 5
    xor rbx, rbx
.cmp2:
    mov al, [rsi+rbx]
    cmp al, [rdi+rbx]
    jne .fail_seek
    inc rbx
    cmp rbx, rcx
    jb .cmp2

    ; --- SYS_FD_LSEEK back to 0, re-read first 5 bytes ---
    mov rax, 20
    mov rdi, r12
    mov rsi, 0
    int 0x80
    cmp rax, 0
    jne .fail_seek2

    mov rax, 18
    mov rdi, r12
    mov rsi, rbuf3
    mov rdx, 5
    int 0x80
    cmp rax, 5
    jne .fail_read3

    mov rsi, rbuf3
    mov rdi, chunk1
    mov rcx, 5
    xor rbx, rbx
.cmp3:
    mov al, [rsi+rbx]
    cmp al, [rdi+rbx]
    jne .fail_seek
    inc rbx
    cmp rbx, rcx
    jb .cmp3

    ; --- read past EOF: must return 0, not an error ---
    mov rax, 20
    mov rdi, r12
    mov rsi, 100
    int 0x80
    mov rax, 18
    mov rdi, r12
    mov rsi, rbuf3
    mov rdx, 10
    int 0x80
    cmp rax, 0
    jne .fail_eof

    mov rax, 22
    mov rdi, r12
    int 0x80

    ; --- directory listing: opendir("/"), readdir until end, expect at
    ;     least one entry (this very file, if nothing else) ---
    mov rax, 17
    mov rdi, droot
    int 0x80
    cmp rax, 0
    jle .fail_opendir
    mov r13, rax

    xor r14, r14                ; entry count
.readdir_loop:
    mov rax, 21
    mov rdi, r13
    mov rsi, dirent_buf
    int 0x80
    cmp rax, 0
    je .readdir_done            ; 0 = end of directory (not an error)
    cmp rax, -1
    je .fail_readdir
    inc r14
    jmp .readdir_loop
.readdir_done:
    test r14, r14
    jz .fail_readdir_empty

    mov rax, 22
    mov rdi, r13
    int 0x80

    mov rax, 0
    mov rdi, m_ok
    mov rsi, m_ok_len
    int 0x80
    jmp .done

.fail_open_w:   mov rdi, e_ow
                mov rsi, e_ow_len
                jmp .emit
.fail_write:    mov rdi, e_w
                mov rsi, e_w_len
                jmp .emit
.fail_close:    mov rdi, e_c
                mov rsi, e_c_len
                jmp .emit
.fail_open_r:   mov rdi, e_or
                mov rsi, e_or_len
                jmp .emit
.fail_read1:    mov rdi, e_r1
                mov rsi, e_r1_len
                jmp .emit
.fail_read2:    mov rdi, e_r2
                mov rsi, e_r2_len
                jmp .emit
.fail_seek:     mov rdi, e_sk
                mov rsi, e_sk_len
                jmp .emit
.fail_seek2:    mov rdi, e_sk2
                mov rsi, e_sk2_len
                jmp .emit
.fail_read3:    mov rdi, e_r3
                mov rsi, e_r3_len
                jmp .emit
.fail_eof:      mov rdi, e_eof
                mov rsi, e_eof_len
                jmp .emit
.fail_opendir:  mov rdi, e_od
                mov rsi, e_od_len
                jmp .emit
.fail_readdir:  mov rdi, e_rd
                mov rsi, e_rd_len
                jmp .emit
.fail_readdir_empty:
                mov rdi, e_rde
                mov rsi, e_rde_len
.emit:
    mov rax, 0
    int 0x80
.done:
    mov rax, 1
    mov rdi, 0
    int 0x80

section .bss
rbuf:  resb 8
rbuf2: resb 8
rbuf3: resb 8
dirent_buf: resb 264

section .rodata
fpath:  db "/fdtest.dat", 0
droot:  db "/", 0
chunk1: db "AAAAA"
chunk1_len equ $ - chunk1
chunk2: db "BBBBB"
chunk2_len equ $ - chunk2
m_ok:   db "FD_PASS open/write/close, open/read/seek/read, EOF, and directory listing all verified", 10
m_ok_len equ $ - m_ok
e_ow:   db "FD_FAIL open for write failed", 10
e_ow_len equ $ - e_ow
e_w:    db "FD_FAIL write returned wrong count", 10
e_w_len equ $ - e_w
e_c:    db "FD_FAIL close failed", 10
e_c_len equ $ - e_c
e_or:   db "FD_FAIL open for read failed", 10
e_or_len equ $ - e_or
e_r1:   db "FD_FAIL first read returned wrong count", 10
e_r1_len equ $ - e_r1
e_r2:   db "FD_FAIL second read (seek position) returned wrong count", 10
e_r2_len equ $ - e_r2
e_sk:   db "FD_FAIL seek/read content mismatch", 10
e_sk_len equ $ - e_sk
e_sk2:  db "FD_FAIL lseek(0) failed", 10
e_sk2_len equ $ - e_sk2
e_r3:   db "FD_FAIL read after seek(0) returned wrong count", 10
e_r3_len equ $ - e_r3
e_eof:  db "FD_FAIL read past EOF did not return 0", 10
e_eof_len equ $ - e_eof
e_od:   db "FD_FAIL opendir failed", 10
e_od_len equ $ - e_od
e_rd:   db "FD_FAIL readdir returned an error", 10
e_rd_len equ $ - e_rd
e_rde:  db "FD_FAIL directory listing was empty", 10
e_rde_len equ $ - e_rde
