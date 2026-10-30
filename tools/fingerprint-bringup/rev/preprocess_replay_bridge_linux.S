.text
.globl call_preprocess
.p2align 2
call_preprocess:
    stp x29, x30, [sp, #-32]!
    str x18, [sp, #16]
    mov x29, sp
    adrp x16, platform_x18
    str x18, [x16, :lo12:platform_x18]
    blr x4
    ldr x18, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.macro BOUNDARY name, target
.globl \name
.p2align 2
\name:
    stp x29, x30, [sp, #-32]!
    str x18, [sp, #16]
    mov x29, sp
    adrp x16, platform_x18
    ldr x18, [x16, :lo12:platform_x18]
    bl \target
    ldr x18, [sp, #16]
    ldp x29, x30, [sp], #32
    ret
.endm
BOUNDARY bridge_alloc, tracked_alloc
BOUNDARY bridge_free, tracked_free
BOUNDARY bridge_memset, boundary_memset
BOUNDARY bridge_memcpy, boundary_memcpy
BOUNDARY bridge_log, boundary_log
BOUNDARY bridge_logpath, boundary_logpath
.section .note.GNU-stack,"",@progbits
