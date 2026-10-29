# Copy-and-patch stencils for the Sprint JIT (x86-64, System V). Each stencil is a piece of machine code that
# jit.c copies into a region in the order of the bytecode and then patches:
#
#   H0..H5   32-bit holes: slot byte offsets, immediates, structure offsets (displacements and immediates)
#   HQ       64-bit immediate hole
#   JIT_T    rel32 hole: the jump target of a branch stencil
#   JIT_X    rel32 hole: the exit stub a failed guard jumps to
#
# A stencil falls through into whatever stencil follows it, so there is no jump between straight-line stencils.
#
# Registers while JIT code runs (all callee-saved, so calls into C keep them):
#   rbx = VM *      r12 = frame base (locals at +0, the operand stack right above them)      r13 = constant pool
#   r14 = vm->slots
# Scratch: rax rcx rdx r8-r11 xmm0-xmm3.  Slots are 16 bytes: type word at +0, payload at +8.
#
# Regenerate src/runtime/jit/stencils.h with `make jit-regen` after editing this file.

#define T_BOOL 1
#define T_INT 2
#define T_FLOAT 3
#define T_ARRAY 5

.macro ST name
    .section .text.\name,"ax",@progbits
    .globl \name
    .type \name,@function
\name:
.endm

# ---- frame: entry, exit -----------------------------------------------------------------------------
# Compiled code never returns to C by `ret` from inside a nested call: every exit restores the stack pointer saved by
# the entry stencil (vm->jit_rsp), which also unwinds any native calls compiled code made on the way.
#include "layout.h"

#define T_FUNCTION 7

ST entry                        # loop region: rdi = VM, rsi = frame base, rdx = constants, rcx = current Frame
    endbr64
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rbp                   # six pushes + 8: the stack is 16-byte aligned for the libm calls below
    sub $8,%rsp
    mov %rdi,%rbx
    mov %rsi,%r12
    mov %rdx,%r13
    mov %rcx,%r15
    mov %rsp,L_VM_JRSP(%rbx)
    mov L_VM_SLOTS(%rbx),%r14

ST exit                         # H0 = exit id, HQ = the region that owns the exit table
    movabs $HQ,%rax
    mov %rax,L_VM_JCUR(%rbx)
    mov L_VM_JRSP(%rbx),%rsp
    add $8,%rsp
    pop %rbp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    mov $H0,%eax
    ret

ST utramp                       # function unit, entered from the interpreter: same arguments as entry; JIT_T = ufn
    endbr64
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    push %rbp
    sub $8,%rsp
    mov %rdi,%rbx
    mov %rsi,%r12
    mov %rdx,%r13
    mov %rcx,%r15
    mov %rsp,L_VM_JRSP(%rbx)
    mov L_VM_SLOTS(%rbx),%r14
    call JIT_T
    add $8,%rsp
    pop %rbp
    pop %r15                    # the unit returned normally: its frame is popped, the result is below the base
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    mov $-1,%eax
    ret

ST ufn                          # function unit body entry, called by utramp and by ucall
    endbr64
    sub $8,%rsp                 # call pushed 8 bytes: realign for libm

ST ufn_p                        # same, for a unit that keeps a local in rbp: the callee saves the caller's rbp
    endbr64
    push %rbp

# Returns: the result replaces the callee slot below the base. A frame the construction fast path pushed (kind != 0)
# returns its new instance (frame->uev) instead of the value.
.macro RET_TAIL                 # rax:rcx = the value
    cmpl $0,L_FR_KIND(%r15)
    je 1f
    mov L_FR_UEV(%r15),%rax
    mov L_FR_UEV+8(%r15),%rcx
1:  mov %rax,-16(%r12)
    mov %rcx,-8(%r12)
    sub $L_FRAME,%r15
    decl L_VM_NFRAMES(%rbx)
    add $8,%rsp
    ret
.endm

.macro RET_TAIL_P
    cmpl $0,L_FR_KIND(%r15)
    je 1f
    mov L_FR_UEV(%r15),%rax
    mov L_FR_UEV+8(%r15),%rcx
1:  mov %rax,-16(%r12)
    mov %rcx,-8(%r12)
    sub $L_FRAME,%r15
    decl L_VM_NFRAMES(%rbx)
    pop %rbp
    ret
.endm

ST ret_r_p
    mov H0(%r12),%rax
    mov H0+8(%r12),%rcx
    RET_TAIL_P

ST ret_k_p
    mov H0(%r13),%rax
    mov H0+8(%r13),%rcx
    RET_TAIL_P

ST ret_nil_p
    xor %eax,%eax
    xor %ecx,%ecx
    RET_TAIL_P

# rbp mirrors the payload of one local (memory stays authoritative, so exits need no spill)
ST ldrax_p
    mov %rbp,%rax
ST strax_p
    mov %rax,%rbp
ST ldp                          # H0 = slot offset
    mov H0+8(%r12),%rbp

ST ret_r                        # return slot H0
    mov H0(%r12),%rax
    mov H0+8(%r12),%rcx
    RET_TAIL

ST ret_k                        # return constant H0
    mov H0(%r13),%rax
    mov H0+8(%r13),%rcx
    RET_TAIL

ST ret_nil
    xor %eax,%eax
    xor %ecx,%ecx
    RET_TAIL

# Calls of a Sprint function. The checks all happen first (a failed one leaves through JIT_X before anything has
# happened); the effect then pushes the Frame the interpreter would push and calls the callee's compiled entry.
# On entry: rax = SFunc *, rdx = JitUnit *.
.macro UC_CHECKS argc
    cmpl $\argc,L_JU_NPARAMS(%rdx)
    jne JIT_X
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    mov L_VM_NFRAMES(%rbx),%ecx
    cmp L_VM_JFMAX(%rbx),%ecx
    jge JIT_X
    lea H0+16(%r12),%r8                     # new base
    mov L_JU_NEED(%rdx),%r9
    add %r8,%r9
    cmp L_VM_STACK_END(%rbx),%r9
    ja JIT_X
.endm

.macro UC_EFFECT argc, kind, uevt, uevp, retq   # retq (HQ or HQ2) = pc to resume the caller at (frame->pc); frame->uev = uevt:uevp
    decq L_VM_ILEFT(%rbx)
    movslq L_JU_NLOCALS(%rdx),%r9           # locals beyond the arguments start out nil
    mov $\argc,%r10d
1:  cmp %r9,%r10
    jge 2f
    mov %r10,%r11
    shl $4,%r11
    movq $0,(%r8,%r11)
    movq $0,8(%r8,%r11)
    inc %r10
    jmp 1b
2:  movabs $\retq,%r9
    mov %r9,L_FR_PC(%r15)
    add $L_FRAME,%r15
    inc %ecx
    mov %ecx,L_VM_NFRAMES(%rbx)
    mov %rax,L_FR_FN(%r15)
    mov %r8,L_FR_BASE(%r15)
    movl $\kind,L_FR_KIND(%r15)
    mov L_VM_NDR(%rbx),%r9d
    mov %r9d,L_FR_DBASE(%r15)
    movq \uevt,L_FR_UEV(%r15)
    movq \uevp,L_FR_UEV+8(%r15)
    mov L_JU_CODE(%rdx),%r9
    mov %r9,L_FR_PC(%r15)
    push %r12
    push %r13
    mov %r8,%r12
    mov L_JU_KT(%rdx),%r13
    call *L_JU_ENTRY(%rdx)
    pop %r13
    pop %r12
.endm

# Calls whose callee proto the compiler knew when it compiled (the guard compares frame->fn->proto): everything about the
# callee is an immediate. H0 = callee slot, H1 = arguments, H2 = callee locals, H3 = bytes of stack the callee needs, HQ =
# pc to resume the caller at, HQ2 = the callee's JitUnit (ucall_k) or Proto (ucall_s, a function calling itself).
.macro UCK_BODY self
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    mov L_VM_NFRAMES(%rbx),%ecx
    cmp L_VM_JFMAX(%rbx),%ecx
    jge JIT_X
    lea H0+16(%r12),%r8                     # new base
    lea H3(%r8),%r9
    cmp L_VM_STACK_END(%rbx),%r9
    ja JIT_X
    decq L_VM_ILEFT(%rbx)
    mov $H1,%r10d                           # locals beyond the arguments start out nil
    cmp $H2,%r10d
    jge 2f
1:  mov %r10,%r11
    shl $4,%r11
    movq $0,(%r8,%r11)
    movq $0,8(%r8,%r11)
    inc %r10d
    cmp $H2,%r10d
    jl 1b
2:  movabs $HQ,%r9
    mov %r9,L_FR_PC(%r15)
    add $L_FRAME,%r15
    inc %ecx
    mov %ecx,L_VM_NFRAMES(%rbx)
    mov %rax,L_FR_FN(%r15)
    mov %r8,L_FR_BASE(%r15)
    movl $0,L_FR_KIND(%r15)
    mov L_VM_NDR(%rbx),%r9d
    mov %r9d,L_FR_DBASE(%r15)
    movq $0,L_FR_UEV(%r15)
    movq $0,L_FR_UEV+8(%r15)
.if \self
    mov L_PROTO_CODE(%rdx),%r9
.else
    mov L_JU_CODE(%rdx),%r9
.endif
    mov %r9,L_FR_PC(%r15)
    push %r12
    push %r13
    mov %r8,%r12
.if \self
    call JIT_T
.else
    mov L_JU_KT(%rdx),%r13
    call *L_JU_ENTRY(%rdx)
.endif
    pop %r13
    pop %r12
.endm

ST ucall_k
    cmpq $T_FUNCTION,H0(%r12)
    jne JIT_X
    mov H0+8(%r12),%rax
    mov L_SFUNC_PROTO(%rax),%rcx
    movabs $HQ2,%rdx
    cmp L_JU_PROTO(%rdx),%rcx
    jne JIT_X
    UCK_BODY 0

ST ucall_s
    cmpq $T_FUNCTION,H0(%r12)
    jne JIT_X
    mov H0+8(%r12),%rax
    mov L_SFUNC_PROTO(%rax),%rcx
    movabs $HQ2,%rdx
    cmp %rdx,%rcx
    jne JIT_X
    UCK_BODY 1

# CALL with H1 arguments; the callee value is slot H0, the arguments follow.
ST ucall
    cmpq $T_FUNCTION,H0(%r12)
    jne JIT_X
    mov H0+8(%r12),%rax                     # SFunc *
    mov L_SFUNC_PROTO(%rax),%rcx
    test %rcx,%rcx
    jz JIT_X
    mov L_PROTO_JUNIT(%rcx),%rdx            # JitUnit *
    test %rdx,%rdx
    jz JIT_X
    UC_CHECKS H1
    UC_EFFECT H1, 0, $0, $0, HQ

# method call on an instance: slot H0 = receiver, H1 more arguments, the method value is constant H2 (kt). Moves the
# receiver and arguments up one slot and puts the method below them, exactly like the interpreter does.
ST uinvoke
    mov H2+8(%r13),%rax
    mov L_SFUNC_PROTO(%rax),%rcx
    test %rcx,%rcx
    jz JIT_X
    mov L_PROTO_JUNIT(%rcx),%rdx
    test %rdx,%rdx
    jz JIT_X
    UC_CHECKS H1+1
    mov $H1,%r10d                           # shift slots H1 .. 0 up by one
3:  mov %r10,%r11
    shl $4,%r11
    mov H0(%r12,%r11),%r9
    mov %r9,H0+16(%r12,%r11)
    mov H0+8(%r12,%r11),%r9
    mov %r9,H0+24(%r12,%r11)
    dec %r10
    jns 3b
    mov H2(%r13),%r9
    mov %r9,H0(%r12)
    mov %rax,H0+8(%r12)
    UC_EFFECT H1+1, 0, $0, $0, HQ

# new instance of class %rsi -> %rax. The fast path is jit_new_inst / new_obj / pool_alloc spelled out (no collection due, the
# size class has a free block, the heap limit is not reached, 14 fields at most); otherwise the C helper runs with the
# stack top \spo (a frame offset) published to the GC. Clobbers rcx rdx rdi r8 r9 r10.
.macro NEWINST spo
    mov L_CLS_NF(%rsi),%ecx
    cmp $14,%ecx
    ja 8f
    mov L_VM_BYTES(%rbx),%rax
    cmp L_VM_GCNEXT(%rbx),%rax
    jae 8f
    mov %rcx,%rdx
    shl $4,%rdx
    add $L_INST_SLOTS,%rdx
    add %rdx,%rax
    cmp L_VM_MAXHEAP(%rbx),%rax
    ja 8f
    lea 2(%rcx),%r10                        # size class (L_INST_SLOTS = 32 = 2 * 16)
    mov L_VM_POOLFREE(%rbx,%r10,8),%r8
    test %r8,%r8
    jz 8f
    mov (%r8),%r9
    mov %r9,L_VM_POOLFREE(%rbx,%r10,8)
    mov %rax,L_VM_BYTES(%rbx)
    mov L_VM_NEXTID(%rbx),%eax
    inc %eax
    mov %eax,L_VM_NEXTID(%rbx)
    shl $32,%rax
    or $9,%rax                              # T_INSTANCE
    mov %rax,(%r8)                          # type, marked, id
    mov L_VM_OBJS(%rbx),%rax
    mov %rax,8(%r8)                         # next
    mov %r8,L_VM_OBJS(%rbx)
    mov %rsi,L_INST_CLS(%r8)
    mov %rcx,L_INST_NSLOTS(%r8)             # nslots (the padding word is zeroed with it)
    mov L_CLS_DFLT(%rsi),%r9
    xor %r10d,%r10d
    test %r9,%r9
    jz 6f
5:  cmp %rcx,%r10                           # copy the default slot values
    jge 7f
    mov %r10,%rdx
    shl $4,%rdx
    mov (%r9,%rdx),%rax
    mov %rax,L_INST_SLOTS(%r8,%rdx)
    mov 8(%r9,%rdx),%rax
    mov %rax,L_INST_SLOTS+8(%r8,%rdx)
    inc %r10
    jmp 5b
6:  cmp %rcx,%r10                           # all nil
    jge 7f
    mov %r10,%rdx
    shl $4,%rdx
    movq $0,L_INST_SLOTS(%r8,%rdx)
    movq $0,L_INST_SLOTS+8(%r8,%rdx)
    inc %r10
    jmp 6b
7:  mov %r8,%rax
    jmp 9f
8:  lea \spo(%r12),%rax                     # slow path: the helper may collect
    mov %rax,L_VM_SP(%rbx)
    mov %rbx,%rdi
    call *L_VM_JNEW(%rbx)
9:
.endm

# construction through the fast path (class in slot H0 with H1 arguments after it, init method = constant H2, H3 = byte
# offset of the stack top, HQ = the class, HQ2 = pc to resume the caller at): checks everything first, then allocates (the one step that cannot fail),
# then calls init with the new instance as `self`; the frame is a constructor frame (kind 4) that returns the instance.
ST uctor
    cmpq $8,H0(%r12)            # T_CLASS
    jne JIT_X
    movabs $HQ,%rcx
    cmp %rcx,H0+8(%r12)
    jne JIT_X
    mov H2+8(%r13),%rax
    mov L_SFUNC_PROTO(%rax),%rcx
    test %rcx,%rcx
    jz JIT_X
    mov L_PROTO_JUNIT(%rcx),%rdx
    test %rdx,%rdx
    jz JIT_X
    UC_CHECKS H1+1
    mov H0+8(%r12),%rsi
    NEWINST H3                  # rax = new instance (the GC scans the stack up to H3 if the helper runs)
    mov %rax,%rdi
    mov $H1,%r10d               # arguments move up one slot: slots H1 .. 1
4:  test %r10,%r10
    jz 5f
    mov %r10,%r11
    shl $4,%r11
    mov H0(%r12,%r11),%r9
    mov %r9,H0+16(%r12,%r11)
    mov H0+8(%r12,%r11),%r9
    mov %r9,H0+24(%r12,%r11)
    dec %r10
    jmp 4b
5:  movq $9,H0+16(%r12)         # self
    mov %rdi,H0+24(%r12)
    mov H2(%r13),%r9            # init
    mov %r9,H0(%r12)
    mov H2+8(%r13),%rax
    mov %rax,H0+8(%r12)
    mov L_SFUNC_PROTO(%rax),%rcx
    mov L_PROTO_JUNIT(%rcx),%rdx
    lea H0+16(%r12),%r8
    mov L_VM_NFRAMES(%rbx),%ecx
    UC_EFFECT H1+1, 4, $9, %rdi, HQ2

ST unew0                        # class (no init, no arguments) in slot H0 -> new instance; H3 = stack top, HQ = class
    cmpq $8,H0(%r12)
    jne JIT_X
    movabs $HQ,%rcx
    cmp %rcx,H0+8(%r12)
    jne JIT_X
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    decq L_VM_ILEFT(%rbx)
    mov H0+8(%r12),%rsi
    NEWINST H3
    movq $9,H0(%r12)
    mov %rax,H0+8(%r12)

# inlined construction: slot H0 must hold the class HQ
ST ucls
    cmpq $8,H0(%r12)
    jne JIT_X
    movabs $HQ,%rcx
    cmp %rcx,H0+8(%r12)
    jne JIT_X

# inlined construction: a new instance of the class HQ in %rax, header only (the compiler stores the default slot values);
# H1 = number of slots (14 at most), H2 = its size in bytes, H0 = stack top (a frame offset) published to the GC when the helper runs
ST unewc
    movabs $HQ,%rsi
    mov $H1,%ecx
    mov L_VM_BYTES(%rbx),%rax
    cmp L_VM_GCNEXT(%rbx),%rax
    jae 8f
    add $H2,%rax
    cmp L_VM_MAXHEAP(%rbx),%rax
    ja 8f
    lea 2(%rcx),%r10
    mov L_VM_POOLFREE(%rbx,%r10,8),%r8
    test %r8,%r8
    jz 8f
    mov (%r8),%r9
    mov %r9,L_VM_POOLFREE(%rbx,%r10,8)
    mov %rax,L_VM_BYTES(%rbx)
    mov L_VM_NEXTID(%rbx),%eax
    inc %eax
    mov %eax,L_VM_NEXTID(%rbx)
    shl $32,%rax
    or $9,%rax
    mov %rax,(%r8)
    mov L_VM_OBJS(%rbx),%rax
    mov %rax,8(%r8)
    mov %r8,L_VM_OBJS(%rbx)
    mov %rsi,L_INST_CLS(%r8)
    mov %rcx,L_INST_NSLOTS(%r8)
    mov %r8,%rax
    jmp 9f
8:  lea H0(%r12),%rax
    mov %rax,L_VM_SP(%rbx)
    mov %rbx,%rdi
    call *L_VM_JNEW(%rbx)
9:

# ---- instances ----------------------------------------------------------------------------------------------
ST gcls                         # slot H0 must hold an instance of the class HQ
    cmpq $9,H0(%r12)            # T_INSTANCE
    jne JIT_X
    mov H0+8(%r12),%rax
    movabs $HQ,%rcx
    cmp %rcx,L_INST_CLS(%rax)
    jne JIT_X

ST ldfld                        # slot H0 <- field (byte offset H2 into the instance) of the instance in slot H1
    mov H1+8(%r12),%rax
    mov H2(%rax),%rcx
    mov H2+8(%rax),%rdx
    mov %rcx,H0(%r12)
    mov %rdx,H0+8(%r12)

ST ldfld_i                      # as ldfld, the field must be an int: rax <- its payload
    mov H1+8(%r12),%rax
    cmpq $2,H2(%rax)
    jne JIT_X
    mov H2+8(%rax),%rax
    movq $2,H0(%r12)
    mov %rax,H0+8(%r12)

ST ldfld_f                      # as ldfld, the field must be a float: xmm0 <- its payload
    mov H1+8(%r12),%rax
    cmpq $3,H2(%rax)
    jne JIT_X
    movsd H2+8(%rax),%xmm0
    movq $3,H0(%r12)
    movsd %xmm0,H0+8(%r12)

ST stfld                        # field H2 of the instance in slot H0 <- slot H1
    mov H0+8(%r12),%rax
    mov H1(%r12),%rcx
    mov H1+8(%r12),%rdx
    mov %rcx,H2(%rax)
    mov %rdx,H2+8(%rax)

ST stfldk                       # field H2 of the instance in slot H0 <- constant H1
    mov H0+8(%r12),%rax
    mov H1(%r13),%rcx
    mov H1+8(%r13),%rdx
    mov %rcx,H2(%rax)
    mov %rdx,H2+8(%rax)

# ---- upvalues: the current closure is frame->fn ---------------------------------------------------------
ST ldup                         # slot H0 <- upvalue (byte offset H1 into up[])
    mov L_FR_FN(%r15),%rax
    mov L_SFUNC_UP(%rax),%rax
    mov H1(%rax),%rax
    mov L_CELL_V(%rax),%rcx
    mov L_CELL_V+8(%rax),%rdx
    mov %rcx,H0(%r12)
    mov %rdx,H0+8(%r12)

ST stup                         # upvalue (byte offset H1) <- slot H0
    mov L_FR_FN(%r15),%rax
    mov L_SFUNC_UP(%rax),%rax
    mov H1(%rax),%rax
    mov H0(%r12),%rcx
    mov H0+8(%r12),%rdx
    mov %rcx,L_CELL_V(%rax)
    mov %rdx,L_CELL_V+8(%rax)

ST stupk                        # upvalue (byte offset H1) <- constant H0
    mov L_FR_FN(%r15),%rax
    mov L_SFUNC_UP(%rax),%rax
    mov H1(%rax),%rax
    mov H0(%r13),%rcx
    mov H0+8(%r13),%rdx
    mov %rcx,L_CELL_V(%rax)
    mov %rdx,L_CELL_V+8(%rax)


# ---- inlined calls ------------------------------------------------------------------------------------------
# guard for a callee the compiler inlines: slot H0 holds a function whose proto is HQ, the budget / frame count / stack
# have room for the frame the interpreter would push (H1 = bytes the callee needs). Nothing has happened if one fails.
ST ginl
    cmpq $T_FUNCTION,H0(%r12)
    jne JIT_X
    mov H0+8(%r12),%rax
    movabs $HQ,%rcx
    cmp L_SFUNC_PROTO(%rax),%rcx
    jne JIT_X
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    mov L_VM_NFRAMES(%rbx),%ecx
    cmp L_VM_JFMAX(%rbx),%ecx
    jge JIT_X
    lea H0+16(%r12),%r8
    add $H1,%r8
    cmp L_VM_STACK_END(%rbx),%r8
    ja JIT_X
    decq L_VM_ILEFT(%rbx)


# resources for an inlined method call (the callee is a constant, nothing to compare): same checks as ginl, slot H0 is
# where the method value will go
ST ginlr
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    mov L_VM_NFRAMES(%rbx),%ecx
    cmp L_VM_JFMAX(%rbx),%ecx
    jge JIT_X
    lea H0+16(%r12),%r8
    add $H1,%r8
    cmp L_VM_STACK_END(%rbx),%r8
    ja JIT_X
    decq L_VM_ILEFT(%rbx)

# the same two checks for a loop region: the frame and stack room an inlined call needs is checked once when the region
# is entered (jit_site, from JRegion.inl_extent), so only the instruction budget is left per call
ST ginln                        # slot H0 holds the function with prototype HQ
    cmpq $T_FUNCTION,H0(%r12)
    jne JIT_X
    mov H0+8(%r12),%rax
    movabs $HQ,%rcx
    cmp L_SFUNC_PROTO(%rax),%rcx
    jne JIT_X
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    decq L_VM_ILEFT(%rbx)

ST ginlrn
    cmpq $0,L_VM_ILEFT(%rbx)
    je JIT_X
    decq L_VM_ILEFT(%rbx)

# upvalue access in an inlined callee: the closure is the function value in slot H2
ST ldupi                        # slot H0 <- upvalue (byte offset H1 into up[]) of the closure in slot H2
    mov H2+8(%r12),%rax
    mov L_SFUNC_UP(%rax),%rax
    mov H1(%rax),%rax
    mov L_CELL_V(%rax),%rcx
    mov L_CELL_V+8(%rax),%rdx
    mov %rcx,H0(%r12)
    mov %rdx,H0+8(%r12)

ST stupi                        # upvalue (byte offset H1) of the closure in slot H2 <- slot H0
    mov H2+8(%r12),%rax
    mov L_SFUNC_UP(%rax),%rax
    mov H1(%rax),%rax
    mov H0(%r12),%rcx
    mov H0+8(%r12),%rdx
    mov %rcx,L_CELL_V(%rax)
    mov %rdx,L_CELL_V+8(%rax)

ST stupki                       # upvalue (byte offset H1) of the closure in slot H2 <- constant H0
    mov H2+8(%r12),%rax
    mov L_SFUNC_UP(%rax),%rax
    mov H1(%rax),%rax
    mov H0(%r13),%rcx
    mov H0+8(%r13),%rdx
    mov %rcx,L_CELL_V(%rax)
    mov %rdx,L_CELL_V+8(%rax)

ST jmp
    jmp JIT_T

ST tick                         # back edge: H0 = offsetof(VM, ileft); out of ticks -> exit, else count one
    cmpq $0,H0(%rbx)
    je JIT_X
    decq H0(%rbx)
    jmp JIT_T

# ---- guards ----------------------------------------------------------------------------------------------
ST gtag                         # frame slot H0 must have type word H1
    cmpq $H1,H0(%r12)
    jne JIT_X

ST gcls_mod                     # module variable at byte offset H0 must hold an instance of the class HQ
    cmpq $9,H0(%r14)            # T_INSTANCE
    jne JIT_X
    mov H0+8(%r14),%rax
    movabs $HQ,%rcx
    cmp %rcx,L_INST_CLS(%rax)
    jne JIT_X

ST gimm                         # int payload of frame slot H0 must be H1 (a region entry guard: the step of a range loop)
    cmpq $H1,H0+8(%r12)
    jne JIT_X

ST gtag_mod                     # module variable at byte offset H0 must have type word H1
    cmpq $H1,H0(%r14)
    jne JIT_X

ST gnz                          # int slot H0 must be non-zero
    cmpq $0,H0+8(%r12)
    je JIT_X

# ---- moves -----------------------------------------------------------------------------------------------
ST mov                          # slot H0 <- slot H1
    mov H1(%r12),%rax
    mov H1+8(%r12),%rcx
    mov %rax,H0(%r12)
    mov %rcx,H0+8(%r12)

ST movk                         # slot H0 <- constant H1
    mov H1(%r13),%rax
    mov H1+8(%r13),%rcx
    mov %rax,H0(%r12)
    mov %rcx,H0+8(%r12)

ST ldmod                        # slot H0 <- module variable at byte offset H2
    mov H2(%r14),%rcx
    mov H2+8(%r14),%rdx
    mov %rcx,H0(%r12)
    mov %rdx,H0+8(%r12)

ST stmod                        # module variable at byte offset H0 <- slot H2
    mov H2(%r12),%rcx
    mov H2+8(%r12),%rdx
    mov %rcx,H0(%r14)
    mov %rdx,H0+8(%r14)

# ---- integer arithmetic: dst H0, a H1, b H2.  forms: rr slot/slot, rk slot/const, kr const/slot -----------
.macro IARITH nm, insn
    ST i\nm\()_rr
    mov H1+8(%r12),%rax
    \insn H2+8(%r12),%rax
    jo JIT_X
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
    ST i\nm\()_rk
    mov H1+8(%r12),%rax
    \insn H2+8(%r13),%rax
    jo JIT_X
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
    ST i\nm\()_kr
    mov H1+8(%r13),%rax
    \insn H2+8(%r12),%rax
    jo JIT_X
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
    # variants for a destination already known to hold an int tag: no tag store
    ST i\nm\()_rr_n
    mov H1+8(%r12),%rax
    \insn H2+8(%r12),%rax
    jo JIT_X
    mov %rax,H0+8(%r12)
    ST i\nm\()_rk_n
    mov H1+8(%r12),%rax
    \insn H2+8(%r13),%rax
    jo JIT_X
    mov %rax,H0+8(%r12)
    ST i\nm\()_kr_n
    mov H1+8(%r13),%rax
    \insn H2+8(%r12),%rax
    jo JIT_X
    mov %rax,H0+8(%r12)
.endm
IARITH add, add
IARITH sub, sub
IARITH mul, imul

# floor division and modulo: divisor 0 and -1 leave to the interpreter (error / overflow cases)
.macro IDIVMOD nm, ld_a, ra, ld_b, rb, fa, fb
    ST i\nm\()_\fa\fb
    mov H1+8(\ra),%rax
    mov H2+8(\rb),%rcx
    test %rcx,%rcx
    jz JIT_X
    cmp $-1,%rcx
    je JIT_X
    cqo
    idiv %rcx
.endm
.macro IFDIV_TAIL
    test %rdx,%rdx
    jz 1f
    xor %rcx,%rdx
    jns 1f
    dec %rax
1:  movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
.endm
.macro IMOD_TAIL
    test %rdx,%rdx
    jz 1f
    mov %rdx,%r8
    xor %rcx,%r8
    jns 1f
    add %rcx,%rdx
1:  movq $T_INT,H0(%r12)
    mov %rdx,H0+8(%r12)
.endm
IDIVMOD fdiv, 0, %r12, 0, %r12, r, r
IFDIV_TAIL
IDIVMOD fdiv, 0, %r12, 0, %r13, r, k
IFDIV_TAIL
IDIVMOD fdiv, 0, %r13, 0, %r12, k, r
IFDIV_TAIL
IDIVMOD mod, 0, %r12, 0, %r12, r, r
IMOD_TAIL
IDIVMOD mod, 0, %r12, 0, %r13, r, k
IMOD_TAIL
IDIVMOD mod, 0, %r13, 0, %r12, k, r
IMOD_TAIL

# x % c and x // c for a constant divisor c > 0 that is not a power of two: multiply by the reciprocal (Hacker's
# Delight, unsigned round-up method). H1 = x slot, H2 = c, H3 = shift, HQ = magic. Negative x takes an idiv path.
ST imod_m
    mov H1+8(%r12),%rax
    mov %rax,%r8
    test %r8,%r8
    js 2f
    movabs $HQ,%rax
    mul %r8
    mov %r8,%rax
    sub %rdx,%rax
    shr $1,%rax
    add %rdx,%rax
    mov $H3,%ecx
    shr %cl,%rax
    imul $H2,%rax,%rax
    sub %rax,%r8
    jmp 3f
2:  mov %r8,%rax
    mov $H2,%ecx
    cqo
    idiv %rcx
    mov %rdx,%r8
    test %rdx,%rdx
    jz 3f
    add %rcx,%r8
3:  movq $T_INT,H0(%r12)
    mov %r8,H0+8(%r12)

ST ifdiv_m
    mov H1+8(%r12),%rax
    mov %rax,%r8
    test %r8,%r8
    js 2f
    movabs $HQ,%rax
    mul %r8
    mov %r8,%rax
    sub %rdx,%rax
    shr $1,%rax
    add %rdx,%rax
    mov $H3,%ecx
    shr %cl,%rax
    jmp 3f
2:  mov %r8,%rax
    mov $H2,%ecx
    cqo
    idiv %rcx
    test %rdx,%rdx
    jz 3f
    dec %rax
3:  movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)

# x % 2^k (H2 = 2^k - 1, an immediate), x // 2^k (H2 = k): an arithmetic mask / shift is the floor semantics
ST imod_p2
    mov H1+8(%r12),%rax
    and $H2,%rax
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
ST ifdiv_p2
    mov H1+8(%r12),%rax
    mov $H2,%ecx
    sar %cl,%rax
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)

# a / b on ints gives a float; b == 0 is an error left to the interpreter
ST idiv_rr
    cmpq $0,H2+8(%r12)
    je JIT_X
    cvtsi2sdq H1+8(%r12),%xmm0
    cvtsi2sdq H2+8(%r12),%xmm1
    divsd %xmm1,%xmm0
    movq $T_FLOAT,H0(%r12)
    movsd %xmm0,H0+8(%r12)

# ---- float arithmetic: operand types f/i (an int operand is converted), forms rr rk kr ----------------------
.macro LDF x, t, hole, r
.ifc \t,f
    movsd \hole+8(\r),\x
.else
    cvtsi2sdq \hole+8(\r),\x
.endif
.endm
.macro FARITH nm, insn, ta, fa, ra, tb, fb, rb
    ST f\nm\()_\ta\tb\()_\fa\fb
    LDF %xmm0, \ta, H1, \ra
    LDF %xmm1, \tb, H2, \rb
    \insn %xmm1,%xmm0
    movq $T_FLOAT,H0(%r12)
    movsd %xmm0,H0+8(%r12)
    ST f\nm\()_\ta\tb\()_\fa\fb\()_n          # destination already holds a float tag
    LDF %xmm0, \ta, H1, \ra
    LDF %xmm1, \tb, H2, \rb
    \insn %xmm1,%xmm0
    movsd %xmm0,H0+8(%r12)
.endm
.macro FARITH_ALL nm, insn
    .irp tt,ff,fi,if
    .ifc \tt,ff
    FARITH \nm,\insn,f,r,%r12,f,r,%r12
    FARITH \nm,\insn,f,r,%r12,f,k,%r13
    FARITH \nm,\insn,f,k,%r13,f,r,%r12
    .endif
    .ifc \tt,fi
    FARITH \nm,\insn,f,r,%r12,i,r,%r12
    FARITH \nm,\insn,f,r,%r12,i,k,%r13
    FARITH \nm,\insn,f,k,%r13,i,r,%r12
    .endif
    .ifc \tt,if
    FARITH \nm,\insn,i,r,%r12,f,r,%r12
    FARITH \nm,\insn,i,r,%r12,f,k,%r13
    FARITH \nm,\insn,i,k,%r13,f,r,%r12
    .endif
    .endr
.endm
FARITH_ALL add, addsd
FARITH_ALL sub, subsd
FARITH_ALL mul, mulsd

# division: a zero divisor is left to the interpreter (it raises the error)
.macro FDIV ta, fa, ra, tb, fb, rb
    ST fdiv_\ta\tb\()_\fa\fb
    LDF %xmm0, \ta, H1, \ra
    LDF %xmm1, \tb, H2, \rb
    xorpd %xmm2,%xmm2
    ucomisd %xmm2,%xmm1
    je JIT_X
    divsd %xmm1,%xmm0
    movq $T_FLOAT,H0(%r12)
    movsd %xmm0,H0+8(%r12)
    ST fdiv_\ta\tb\()_\fa\fb\()_n
    LDF %xmm0, \ta, H1, \ra
    LDF %xmm1, \tb, H2, \rb
    xorpd %xmm2,%xmm2
    ucomisd %xmm2,%xmm1
    je JIT_X
    divsd %xmm1,%xmm0
    movsd %xmm0,H0+8(%r12)
.endm
FDIV f,r,%r12,f,r,%r12
FDIV f,r,%r12,f,k,%r13
FDIV f,k,%r13,f,r,%r12
FDIV f,r,%r12,i,r,%r12
FDIV f,r,%r12,i,k,%r13
FDIV f,k,%r13,i,r,%r12
FDIV i,r,%r12,f,r,%r12
FDIV i,r,%r12,f,k,%r13
FDIV i,k,%r13,f,r,%r12

# negation
ST ineg
    mov H1+8(%r12),%rax
    neg %rax
    jo JIT_X
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
ST fneg
    mov H1+8(%r12),%rax
    btc $63,%rax
    movq $T_FLOAT,H0(%r12)
    mov %rax,H0+8(%r12)

# math.sqrt(x) for a float x (a negative x is an error left to the interpreter)
ST fsqrt
    movsd H1+8(%r12),%xmm0
    xorpd %xmm1,%xmm1
    ucomisd %xmm1,%xmm0
    jb JIT_X
    sqrtsd %xmm0,%xmm0
    movq $T_FLOAT,H0(%r12)
    movsd %xmm0,H0+8(%r12)

ST i2f                          # slot H0 <- float(int slot H1)
    cvtsi2sdq H1+8(%r12),%xmm0
    movq $T_FLOAT,H0(%r12)
    movsd %xmm0,H0+8(%r12)

# int slot H0 must be within +-2^53 (exactly representable as a double, so a float comparison is exact)
ST gi53
    mov H0+8(%r12),%rax
    sar $53,%rax
    inc %rax
    cmp $1,%rax
    ja JIT_X

# ---- conditional branches (taken to JIT_T): ints ------------------------------------------------------------
.macro IBR nm, cc, ncc
    ST ibr_\nm\()_t_rr
    mov H1+8(%r12),%rax
    cmp H2+8(%r12),%rax
    j\cc JIT_T
    ST ibr_\nm\()_t_rk
    mov H1+8(%r12),%rax
    cmp H2+8(%r13),%rax
    j\cc JIT_T
    ST ibr_\nm\()_f_rr
    mov H1+8(%r12),%rax
    cmp H2+8(%r12),%rax
    j\ncc JIT_T
    ST ibr_\nm\()_f_rk
    mov H1+8(%r12),%rax
    cmp H2+8(%r13),%rax
    j\ncc JIT_T
.endm
IBR lt, l, ge
IBR le, le, g
IBR gt, g, le
IBR ge, ge, l
IBR eq, e, ne
IBR ne, ne, e

# ---- conditional branches: floats (NaN compares false except !=) ------------------------------------------
.macro FBRANCH cond, pol, ta, fa, ra, tb, fb, rb
    ST fbr_\cond\()_\pol\()_\ta\tb\()_\fa\fb
.ifc \cond,lt
    LDF %xmm0, \tb, H2, \rb
    LDF %xmm1, \ta, H1, \ra
.else
.ifc \cond,le
    LDF %xmm0, \tb, H2, \rb
    LDF %xmm1, \ta, H1, \ra
.else
    LDF %xmm0, \ta, H1, \ra
    LDF %xmm1, \tb, H2, \rb
.endif
.endif
    ucomisd %xmm1,%xmm0
.ifc \pol,t
.ifc \cond,lt
    ja JIT_T
.endif
.ifc \cond,le
    jae JIT_T
.endif
.ifc \cond,gt
    ja JIT_T
.endif
.ifc \cond,ge
    jae JIT_T
.endif
.ifc \cond,eq
    jp 1f
    je JIT_T
1:
.endif
.ifc \cond,ne
    jne JIT_T
    jp JIT_T
.endif
.else
.ifc \cond,lt
    jbe JIT_T
.endif
.ifc \cond,le
    jb JIT_T
.endif
.ifc \cond,gt
    jbe JIT_T
.endif
.ifc \cond,ge
    jb JIT_T
.endif
.ifc \cond,eq
    jne JIT_T
    jp JIT_T
.endif
.ifc \cond,ne
    jp 1f
    je JIT_T
1:
.endif
.endif
.endm
.macro FBRANCH_COND cond
    .irp pol,t,f
    FBRANCH \cond,\pol,f,r,%r12,f,r,%r12
    FBRANCH \cond,\pol,f,r,%r12,f,k,%r13
    FBRANCH \cond,\pol,f,r,%r12,i,r,%r12
    FBRANCH \cond,\pol,f,r,%r12,i,k,%r13
    FBRANCH \cond,\pol,i,r,%r12,f,r,%r12
    FBRANCH \cond,\pol,i,r,%r12,f,k,%r13
    .endr
.endm
FBRANCH_COND lt
FBRANCH_COND le
FBRANCH_COND gt
FBRANCH_COND ge
FBRANCH_COND eq
FBRANCH_COND ne

# a bool slot H1: branch when it is true (t) / false (f)
ST bbr_t
    cmpl $0,H1+8(%r12)
    jne JIT_T
ST bbr_f
    cmpl $0,H1+8(%r12)
    je JIT_T

# ---- ranges ---------------------------------------------------------------------------------------------
# for-range step: state slots H1 (current), H1+16 (stop), H1+32 (step), all ints. Done -> JIT_T, else the current
# value goes to slot H0 and the state advances.
ST range_next
    mov H1+8(%r12),%rax
    mov H1+24(%r12),%rcx
    mov H1+40(%r12),%r8
    test %r8,%r8
    jle 2f
    cmp %rcx,%rax
    jge JIT_T
    jmp 3f
2:  cmp %rcx,%rax
    jle JIT_T
3:  movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
    add %r8,%rax
    jno 4f
    mov %rcx,%rax
4:  mov %rax,H1+8(%r12)

# for-range step with a constant step known at compile time (H2 = the step as a 32-bit immediate): no sign test, no
# reload of the step. Same state layout and overflow rule as range_next (an overflowing advance clamps to stop).
ST range_up                     # step > 0
    mov H1+8(%r12),%rax
    mov H1+24(%r12),%rcx
    cmp %rcx,%rax
    jge JIT_T
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
    add $H2,%rax
    jno 4f
    mov %rcx,%rax
4:  mov %rax,H1+8(%r12)

ST range_dn                     # step < 0
    mov H1+8(%r12),%rax
    mov H1+24(%r12),%rcx
    cmp %rcx,%rax
    jle JIT_T
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)
    add $H2,%rax
    jno 4f
    mov %rcx,%rax
4:  mov %rax,H1+8(%r12)

# ---- arrays: H3 = offsetof(SArray, len), H4 = offsetof(SArray, items) --------------------------------------
ST getidx_rr                    # slot H0 <- array(H1)[int(H2)]; out of range -> interpreter (raises)
    mov H1+8(%r12),%rax
    mov H2+8(%r12),%rcx
    cmp H3(%rax),%rcx
    jae JIT_X
    mov H4(%rax),%rax
    shl $4,%rcx
    mov (%rax,%rcx),%rdx
    mov 8(%rax,%rcx),%rsi
    mov %rdx,H0(%r12)
    mov %rsi,H0+8(%r12)

ST getidx_rk                    # index is the constant H2
    mov H1+8(%r12),%rax
    mov H2+8(%r13),%rcx
    cmp H3(%rax),%rcx
    jae JIT_X
    mov H4(%rax),%rax
    shl $4,%rcx
    mov (%rax,%rcx),%rdx
    mov 8(%rax,%rcx),%rsi
    mov %rdx,H0(%r12)
    mov %rsi,H0+8(%r12)


ST setidx_rr                    # array(H0)[int(H1)] <- slot H2
    mov H0+8(%r12),%rax
    mov H1+8(%r12),%rcx
    cmp H3(%rax),%rcx
    jae JIT_X
    mov H4(%rax),%rax
    shl $4,%rcx
    mov H2(%r12),%rdx
    mov H2+8(%r12),%rsi
    mov %rdx,(%rax,%rcx)
    mov %rsi,8(%rax,%rcx)

ST setidx_rk                    # value is the constant H2
    mov H0+8(%r12),%rax
    mov H1+8(%r12),%rcx
    cmp H3(%rax),%rcx
    jae JIT_X
    mov H4(%rax),%rax
    shl $4,%rcx
    mov H2(%r13),%rdx
    mov H2+8(%r13),%rsi
    mov %rdx,(%rax,%rcx)
    mov %rsi,8(%rax,%rcx)

ST alen                         # slot H0 <- len(array(H1))
    mov H1+8(%r12),%rax
    mov H3(%rax),%rax
    movq $T_INT,H0(%r12)
    mov %rax,H0+8(%r12)

# one step of budget for an inlined call (the interpreter ticks once per call)
ST tick1                        # H0 = offsetof(VM, ileft)
    cmpq $0,H0(%rbx)
    je JIT_X
    decq H0(%rbx)

# slot H0 must NOT have type word H1 (e.g. an uninitialised module variable)
ST gntag
    cmpq $H1,H0(%r12)
    je JIT_X

# module variable at byte offset H0 must be an object whose pointer is HQ
ST gobj_mod
    movabs $HQ,%rcx
    cmp %rcx,H0+8(%r14)
    jne JIT_X

# generic truthiness of slot H1: nil and false are false, everything else is true
ST tbr_t                        # jump when true
    mov H1(%r12),%rax
    test %rax,%rax
    jz 1f
    cmp $T_BOOL,%rax
    jne JIT_T
    cmpl $0,H1+8(%r12)
    jne JIT_T
1:
ST tbr_f                        # jump when false
    mov H1(%r12),%rax
    test %rax,%rax
    jz JIT_T
    cmp $T_BOOL,%rax
    jne 1f
    cmpl $0,H1+8(%r12)
    je JIT_T
1:

# nil test of slot H1 (the tag is 0 for nil): branch when it is nil (t) / not nil (f)
ST nilbr_t
    cmpq $0,H1(%r12)
    je JIT_T
ST nilbr_f
    cmpq $0,H1(%r12)
    jne JIT_T

# ---- arrays, continued --------------------------------------------------------------------------------------
# for x in array: state slots H1 (the array), H1+16 (next index, an int). Done -> JIT_T, else the element goes to slot
# H0 (and, for the two-variable form arr_next2, the index to H0 and the element to H0+16).
ST arr_next
    mov H1+8(%r12),%rax
    mov H1+24(%r12),%rcx
    cmp H3(%rax),%rcx
    jge JIT_T
    lea 1(%rcx),%rdx
    mov %rdx,H1+24(%r12)
    mov H4(%rax),%rax
    shl $4,%rcx
    mov (%rax,%rcx),%rdx
    mov 8(%rax,%rcx),%rsi
    mov %rdx,H0(%r12)
    mov %rsi,H0+8(%r12)

ST arr_next2
    mov H1+8(%r12),%rax
    mov H1+24(%r12),%rcx
    cmp H3(%rax),%rcx
    jge JIT_T
    lea 1(%rcx),%rdx
    mov %rdx,H1+24(%r12)
    movq $T_INT,H0(%r12)
    mov %rcx,H0+8(%r12)
    mov H4(%rax),%rax
    shl $4,%rcx
    mov (%rax,%rcx),%rdx
    mov 8(%rax,%rcx),%rsi
    mov %rdx,H0+16(%r12)
    mov %rsi,H0+24(%r12)

# array.append(v) without growth (H3 = len, H4 = items, H5 = cap offsets); the result is nil, written to slot H0
ST aappend_r                    # value in slot H2
    mov H1+8(%r12),%rax
    mov H3(%rax),%rcx
    cmp H5(%rax),%rcx
    jge JIT_X
    lea 1(%rcx),%rdx
    mov %rdx,H3(%rax)
    mov H4(%rax),%rax
    shl $4,%rcx
    mov H2(%r12),%rdx
    mov H2+8(%r12),%rsi
    mov %rdx,(%rax,%rcx)
    mov %rsi,8(%rax,%rcx)
    movq $0,H0(%r12)
    movq $0,H0+8(%r12)

ST aappend_k                    # value is the constant H2
    mov H1+8(%r12),%rax
    mov H3(%rax),%rcx
    cmp H5(%rax),%rcx
    jge JIT_X
    lea 1(%rcx),%rdx
    mov %rdx,H3(%rax)
    mov H4(%rax),%rax
    shl $4,%rcx
    mov H2(%r13),%rdx
    mov H2+8(%r13),%rsi
    mov %rdx,(%rax,%rcx)
    mov %rsi,8(%rax,%rcx)
    movq $0,H0(%r12)
    movq $0,H0+8(%r12)

# a libm function of one float: slot H0 <- HQ(float slot H1)
ST fcall1
    movsd H1+8(%r12),%xmm0
    movabs $HQ,%rax
    call *%rax
    movq $T_FLOAT,H0(%r12)
    movsd %xmm0,H0+8(%r12)

# ---- generic containers and strings: C helpers (the interpreter's own fast paths) ---------------------------------------
# Each returns 0 when it does not handle the case (nothing has been changed): the instruction then runs in the interpreter.
ST hget                         # slot H0 <- HQ(vm, slot H1, slot H2): table / string / array element
    mov %rbx,%rdi
    lea H1(%r12),%rsi
    lea H2(%r12),%rdx
    lea H0(%r12),%rcx
    movabs $HQ,%rax
    call *%rax
    test %eax,%eax
    jz JIT_X

ST hset                         # HQ(vm, slot H1, slot H2, slot H3) stores; H0 = the stack top published to the collector
    lea H0(%r12),%rax
    mov %rax,L_VM_SP(%rbx)
    mov %rbx,%rdi
    lea H1(%r12),%rsi
    lea H2(%r12),%rdx
    lea H3(%r12),%rcx
    movabs $HQ,%rax
    call *%rax
    test %eax,%eax
    jz JIT_X

ST hconcat                      # slot H0 <- HQ(vm, the H2 slots from slot H1): string concatenation; H3 = stack top for the collector
    lea H3(%r12),%rax
    mov %rax,L_VM_SP(%rbx)
    mov %rbx,%rdi
    lea H1(%r12),%rsi
    mov $H2,%edx
    lea H0(%r12),%rcx
    movabs $HQ,%rax
    call *%rax
    test %eax,%eax
    jz JIT_X

ST hnat                         # slot H0 <- HQ2(vm, the H2 args from slot H1) through the helper HQ: a pure native; H3 = stack top for the collector, H4 = expected result tag (0xff: any)
    lea H3(%r12),%rax
    mov %rax,L_VM_SP(%rbx)
    mov %rbx,%rdi
    lea H1(%r12),%rsi
    mov $H2,%edx
    lea H0(%r12),%rcx
    movabs $HQ2,%r8
    mov $H4,%r9d
    movabs $HQ,%rax
    call *%rax
    test %eax,%eax
    jz JIT_X

# for k, v in table / for k in table: H1 = the loop state (table, cursor, version), H0 = where the key (and, with H2 == 2, the value)
# goes. HQ = helper(vm, state, dst, n): 0 = leave for the interpreter (it raises the errors), 1 = next entry, 2 = done -> JIT_T.
ST tnext
    mov %rbx,%rdi
    lea H1(%r12),%rsi
    lea H0(%r12),%rdx
    mov $H2,%ecx
    movabs $HQ,%rax
    call *%rax
    cmp $1,%eax
    je 1f
    test %eax,%eax
    jz JIT_X
    jmp JIT_T
1:

.section .note.GNU-stack,"",@progbits
