/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pte_stub.S - do_mem_abort inline hook stub / trampoline
 * 布局在独立 .S 中, 避免 thin-LTO 吞掉 inline asm 符号定义。
 */
#include <linux/linkage.h>

    .section .text.pte_hook, "ax"
    .align 4
    .global pte_stub_entry
    .type pte_stub_entry, %function
SYM_FUNC_START(pte_stub_entry)
    sub sp, sp, #256
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    bl pte_handle_abort
    cbz w0, 1f
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    ret
1:
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    ldr x17, 2f
    br x17
    .p2align 3
2:  .quad 0                 /* 运行时填入 trampoline 地址 */
SYM_FUNC_END(pte_stub_entry)

    .global pte_trampoline_area
    .type pte_trampoline_area, %object
pte_trampoline_area:
    .space 32                /* 原16字节 + ldr/br + .quad, 运行时写入 */
    .size pte_trampoline_area, 32

    .global pte_tramp_slot
    .type pte_tramp_slot, %object
pte_tramp_slot:
    .quad 0
    .size pte_tramp_slot, 8
