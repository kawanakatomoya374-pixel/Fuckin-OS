BITS 64
global _start

; ---- syscall numbers used ----
%define SYS_WRITE        0
%define SYS_EXIT         1
%define SYS_WIN_CREATE   2
%define SYS_WIN_FILL     3
%define SYS_WIN_CLEAR    4
%define SYS_SLEEP_MS     10
%define SYS_WRITE_FILE   8
%define SYS_WIN_DRAW_TEXT 28
%define SYS_WIN_POLL_KEY  29

%define COS_KEY_NONE      0
%define COS_KEY_ENTER     1
%define COS_KEY_BACKSPACE 2
%define COS_KEY_ESC       3

%define MAX_INPUT   48
%define MAX_OUTPUT  460
%define WRAP_CHARS  48
%define MAX_LINES   5

section .text
_start:
    ; SYS_WIN_CREATE(title, len, w, h)
    mov rax, SYS_WIN_CREATE
    mov rdi, win_title
    mov rsi, win_title_len
    mov rdx, 480
    mov r10, 320
    int 0x80
    cmp rax, 0
    jle .startup_fail
    mov rbx, win_handle
    mov [rbx], rax

    call draw_screen

.main_loop:
    ; SYS_WIN_POLL_KEY(handle, &key_event)
    mov rax, SYS_WIN_POLL_KEY
    mov rbx, win_handle
    mov rdi, [rbx]
    mov rsi, key_event
    int 0x80
    cmp rax, 1
    jne .no_key

    ; key_event: byte0 = ascii, byte1 = special
    mov rbx, key_event
    movzx ecx, byte [rbx + 1]     ; special
    movzx edx, byte [rbx]         ; ascii

    cmp ecx, COS_KEY_ENTER
    je .on_enter
    cmp ecx, COS_KEY_BACKSPACE
    je .on_backspace
    cmp ecx, COS_KEY_ESC
    je .on_esc
    cmp ecx, COS_KEY_NONE
    jne .after_key                ; unknown special, ignore

    ; printable ASCII
    cmp edx, 32
    jl .after_key
    cmp edx, 126
    jg .after_key
    mov rbx, input_len
    mov eax, [rbx]
    cmp eax, MAX_INPUT - 1
    jge .after_key
    mov rsi, input_buf
    mov byte [rsi + rax], dl
    inc eax
    mov byte [rsi + rax], 0
    mov [rbx], eax
    jmp .after_key

.on_backspace:
    mov rbx, input_len
    mov eax, [rbx]
    test eax, eax
    jz .after_key
    dec eax
    mov [rbx], eax
    mov rsi, input_buf
    mov byte [rsi + rax], 0
    jmp .after_key

.on_esc:
    mov rbx, input_len
    mov dword [rbx], 0
    mov rax, input_buf
    mov byte [rax], 0
    mov rbx, output_len
    mov dword [rbx], 0
    mov rax, output_buf
    mov byte [rax], 0
    jmp .after_key

.on_enter:
    call do_convert
    call write_output_file

.after_key:
    call draw_screen
    jmp .poll_again

.no_key:
    mov rax, SYS_SLEEP_MS
    mov rdi, 16
    int 0x80

.poll_again:
    mov rbx, loop_count
    mov eax, [rbx]
    inc eax
    mov [rbx], eax
    cmp eax, 6000                 ; ~ a few minutes of interactive polling
    jb .main_loop

    mov rax, SYS_EXIT
    mov rdi, 0
    int 0x80

.startup_fail:
    mov rax, SYS_EXIT
    mov rdi, 1
    int 0x80


; ---- do_convert: input_buf/input_len -> output_buf/output_len ----
; MSB-first 8-bit binary per character, space-separated.
do_convert:
    mov rsi, input_buf
    mov rdi, output_buf
    xor r9, r9                    ; char index
.char_loop:
    mov rbx, input_len
    mov eax, [rbx]
    cmp r9d, eax
    jae .chars_done
    movzx r10d, byte [rsi + r9]
    mov r11, 7
.bit_loop:
    mov ecx, r11d
    mov eax, r10d
    shr eax, cl
    and eax, 1
    add eax, '0'
    mov [rdi], al
    inc rdi
    dec r11
    cmp r11, -1
    jne .bit_loop
    mov byte [rdi], ' '
    inc rdi
    inc r9
    jmp .char_loop
.chars_done:
    mov byte [rdi], 0
    mov rax, rdi
    mov rbx, output_buf
    sub rax, rbx
    mov rbx, output_len
    mov [rbx], eax
    ret


; ---- write_output_file: writes output_buf (output_len bytes) to disk ----
write_output_file:
    mov rax, SYS_WRITE_FILE
    mov rdi, out_path
    mov rsi, output_buf
    mov rbx, output_len
    mov edx, [rbx]
    int 0x80
    ret


; ---- draw_screen: redraws the whole window from current state ----
draw_screen:
    mov rbx, win_handle
    mov r15, [rbx]                 ; keep handle in r15 across this routine

    mov rax, SYS_WIN_CLEAR
    mov rdi, r15
    int 0x80

    ; input panel background
    mov rax, SYS_WIN_FILL
    mov rdi, r15
    mov rsi, 10
    mov rdx, 5
    mov r10, 460
    mov r8, 55
    mov r9, 0x00F5F5F5
    int 0x80

    ; output panel background
    mov rax, SYS_WIN_FILL
    mov rdi, r15
    mov rsi, 10
    mov rdx, 65
    mov r10, 460
    mov r8, 210
    mov r9, 0x00EAF2FF
    int 0x80

    ; bottom divider line
    mov rax, SYS_WIN_FILL
    mov rdi, r15
    mov rsi, 10
    mov rdx, 285
    mov r10, 460
    mov r8, 2
    mov r9, 0x00888888
    int 0x80

    ; input area: placeholder (2 lines) if empty, else the typed text
    mov rbx, input_len
    mov eax, [rbx]
    test eax, eax
    jnz .draw_typed

    mov rax, SYS_WIN_DRAW_TEXT
    mov rdi, r15
    mov rsi, 18
    mov rdx, 14
    mov r10, 0x00999999
    mov r8, 0x00F5F5F5
    mov r9, placeholder1
    int 0x80

    mov rax, SYS_WIN_DRAW_TEXT
    mov rdi, r15
    mov rsi, 18
    mov rdx, 34
    mov r10, 0x00999999
    mov r8, 0x00F5F5F5
    mov r9, placeholder2
    int 0x80
    jmp .draw_label

.draw_typed:
    mov rax, SYS_WIN_DRAW_TEXT
    mov rdi, r15
    mov rsi, 18
    mov rdx, 22
    mov r10, 0x00202020
    mov r8, 0x00F5F5F5
    mov r9, input_buf
    int 0x80

.draw_label:
    mov rax, SYS_WIN_DRAW_TEXT
    mov rdi, r15
    mov rsi, 150
    mov rdx, 75
    mov r10, 0x00404040
    mov r8, 0x00EAF2FF
    mov r9, label_converted
    int 0x80

    ; wrapped output lines
    mov rbx, output_len
    mov eax, [rbx]
    test eax, eax
    jz .draw_done

    xor r12, r12                   ; offset into output_buf
    xor r13, r13                   ; line index
.line_loop:
    cmp r13, MAX_LINES
    jae .draw_done
    mov rbx, output_len
    mov eax, [rbx]
    cmp r12d, eax
    jae .draw_done

    ; chunk length = min(WRAP_CHARS, remaining)
    mov edx, eax
    sub edx, r12d
    cmp edx, WRAP_CHARS
    jbe .chunk_ok
    mov edx, WRAP_CHARS
.chunk_ok:
    mov r14, rdx                   ; keep chunk length safe in r14

    mov rsi, output_buf
    add rsi, r12
    movzx r8, byte [rsi + r14]     ; save byte at the cut point
    mov byte [rsi + r14], 0        ; temporarily terminate the chunk

    mov r9, rsi                    ; text pointer for the syscall
    mov eax, r13d
    imul eax, 18
    add eax, 100                   ; y = 100 + line*18
    mov rdx, rax                   ; y
    mov rax, SYS_WIN_DRAW_TEXT
    mov rdi, r15
    mov rsi, 18                    ; x
    mov r10, 0x00202020
    push r8
    mov r8, 0x00EAF2FF
    int 0x80
    pop r8

    mov rsi, output_buf
    add rsi, r12
    mov [rsi + r14], r8b           ; restore the clobbered byte

    add r12d, r14d
    inc r13
    jmp .line_loop

.draw_done:
    ret


section .bss
win_handle: resq 1
input_buf:  resb MAX_INPUT
input_len:  resd 1
output_buf: resb MAX_OUTPUT
output_len: resd 1
key_event:  resb 2
loop_count: resd 1

section .rodata
win_title: db "Test - Text to Binary"
win_title_len equ $ - win_title

placeholder1: db "Please enter the characters you want to", 0
placeholder2: db "convert here...", 0
label_converted: db "Converted code...", 0

out_path: db "/test_output.txt", 0
