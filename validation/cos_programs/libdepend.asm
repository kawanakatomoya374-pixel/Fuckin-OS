BITS 64
section .text
extern cos_base_value
global cos_depend_wrapper
cos_depend_wrapper:
    call cos_base_value wrt ..plt
    add rax, 1
    ret
