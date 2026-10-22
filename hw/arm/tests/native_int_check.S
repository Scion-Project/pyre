/*
 * native_int_check.S - directed EL0 tests for native-int (same-ISA
 * translation, docs/NATIVE-INT.md) on the a13-test machine. They must pass
 * with native-int off and on; native_int_rand.S covers the instruction
 * semantics, this file the state a native-int region must keep exact:
 *
 *   1-2  a load that faults after ALU instructions and a flag-setting
 *        instruction in the same region: the data abort sees every
 *        register and NZCV as of the faulting instruction, the destination
 *        unchanged, FAR = the address; same for a post-index load (the
 *        base register is not updated)
 *   3    SP as a base register with pre/post-index pairs
 *   4    x18, x29 and x30 as ordinary operands and load destinations
 *   5    NZCV set before a page-crossing (slow-path) load and store and
 *        consumed after them
 *   6    a store over code later in the same block (the store's slow
 *        path invalidates the TB being executed), then the architectural
 *        DC CVAU/IC IVAU/ISB sequence: registers and NZCV written before
 *        the store survive, and the new instruction runs
 *
 * EL1 sets up the MMU (16 KiB pages): the image read-only (executable at
 * EL0 and EL1), DATA read-write at EL0, never executable, CODE read-write-
 * execute at EL0 (for test 6), and nothing at HOLE. EL0 data aborts are
 * recorded in SAVE (registers, FAR, ELR, SPSR, ESR) and skipped; EL0 ends
 * with SVC #0 (PASS) or SVC #n (check n failed).
 *
 * Architectural encodings only; no Apple code or data.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
    .set CTL_BASE,    0x200000000
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set PASS_TOKEN,  0x50415353
    .set FAIL_BASE,   0x46000000
    .set DATA,        0x802000000         /* EL0 RW, XN */
    .set SAVE,        0x802000000         /* x0-x30, FAR, ELR, SPSR, ESR at the last EL0 data abort */
    .set BUF,         0x802010000
    .set L2,          0x803f00000
    .set EL1_SP,      0x803e00000
    .set CODE,        0x804000000         /* EL0 RWX */
    .set HOLE,        0x806000000         /* unmapped */

/* x28 = 64-bit immediate */
.macro LI reg, val
    movz    \reg, #((\val) & 0xffff)
    movk    \reg, #(((\val) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\val) >> 32) & 0xffff), lsl #32
    movk    \reg, #(((\val) >> 48) & 0xffff), lsl #48
.endm

/* check: \reg == \val, else SVC #\n (uses x28 and the flags) */
.macro CHK reg, val, n
    LI      x28, \val
    cmp     \reg, x28
    b.eq    1f
    svc     #\n
1:
.endm

/* check: SAVE slot \slot == \val (uses x27, x28) */
.macro CHKSAVE slot, val, n
    LI      x27, SAVE
    ldr     x27, [x27, #(\slot) * 8]
    CHK     x27, \val, \n
.endm

    .text
    .globl _start
_start:
    adr     x0, vectors
    msr     vbar_el1, x0
    ldr     x0, =EL1_SP
    mov     sp, x0

    ldr     x0, =L2
    mov     x1, #2048
1:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    1b
    ldr     x0, =L2
    ldr     x1, =0x800000000 | 0x7c1            /* image: AF, inner shareable, Attr0, RO at EL0 and EL1 */
    str     x1, [x0, #0x400 * 8]
    ldr     x1, =DATA | 0x0060000000000741      /* RW at EL0 and EL1, PXN, UXN */
    str     x1, [x0, #0x401 * 8]
    ldr     x1, =CODE | 0x0020000000000741      /* RW at EL0 and EL1, PXN */
    str     x1, [x0, #0x402 * 8]
    ldr     x1, =CTL_BASE | 0x0060000000000405  /* device, EL1 only */
    str     x1, [x0, #0x100 * 8]
    dsb     ish
    ldr     x0, =0x00ff
    msr     mair_el1, x0
    ldr     x0, =28 | (1 << 8) | (1 << 10) | (3 << 12) | (2 << 14) | (28 << 16) | (1 << 23) | (1 << 30) | (1 << 32)
    msr     tcr_el1, x0
    ldr     x0, =L2
    msr     ttbr0_el1, x0
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)                   /* M */
    orr     x0, x0, #(1 << 2)                   /* C */
    orr     x0, x0, #(1 << 12)                  /* I */
    orr     x0, x0, #(1 << 23)                  /* SPAN */
    orr     x0, x0, #(1 << 26)                  /* UCI: DC CVAU and IC IVAU at EL0 */
    bic     x0, x0, #(1 << 1)                   /* A */
    bic     x0, x0, #(1 << 19)                  /* WXN */
    msr     sctlr_el1, x0
    isb

    /* test 6's code goes to the EL0-writable CODE block */
    adr     x0, smc_snip
    adr     x1, smc_snip_end
    ldr     x2, =CODE
2:  ldr     w3, [x0], #4
    str     w3, [x2], #4
    cmp     x0, x1
    b.ne    2b
    ldr     x2, =CODE
    dc      cvau, x2
    dsb     ish
    ic      ialluis
    dsb     ish
    isb

    ldr     x0, =BUF
    msr     sp_el0, x0
    adr     x0, el0_main
    msr     elr_el1, x0
    mov     x0, #0x3c0                          /* EL0t, DAIF masked */
    msr     spsr_el1, x0
    eret

    .balign 2048
vectors:
    .rept 16
    b       el1_sync
    .balign 0x80
    .endr

el1_sync:
    stp     x0, x1, [sp, #-16]!
    mrs     x0, esr_el1
    lsr     x1, x0, #26
    cmp     x1, #0x15                           /* SVC */
    b.eq    el1_svc
    cmp     x1, #0x24                           /* data abort from EL0 */
    b.ne    el1_fail
    ldr     x0, =SAVE
    stp     x2, x3, [x0, #2 * 8]
    stp     x4, x5, [x0, #4 * 8]
    stp     x6, x7, [x0, #6 * 8]
    stp     x8, x9, [x0, #8 * 8]
    stp     x10, x11, [x0, #10 * 8]
    stp     x12, x13, [x0, #12 * 8]
    stp     x14, x15, [x0, #14 * 8]
    stp     x16, x17, [x0, #16 * 8]
    stp     x18, x19, [x0, #18 * 8]
    stp     x20, x21, [x0, #20 * 8]
    stp     x22, x23, [x0, #22 * 8]
    stp     x24, x25, [x0, #24 * 8]
    stp     x26, x27, [x0, #26 * 8]
    stp     x28, x29, [x0, #28 * 8]
    str     x30, [x0, #30 * 8]
    ldp     x2, x3, [sp], #16
    stp     x2, x3, [x0, #0]
    mrs     x2, far_el1
    mrs     x3, elr_el1
    stp     x2, x3, [x0, #31 * 8]
    mrs     x2, spsr_el1
    mrs     x3, esr_el1
    stp     x2, x3, [x0, #33 * 8]
    mrs     x2, elr_el1
    add     x2, x2, #4                          /* skip the faulting instruction */
    msr     elr_el1, x2
    ldp     x2, x3, [x0, #2 * 8]
    ldr     x0, [x0, #0]
    eret

el1_svc:
    ldr     x1, =CTL_BASE
    and     x0, x0, #0xffff
    cbnz    x0, 3f
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    b       .
3:  str     x0, [x1, #CTL_DETAIL]
    lsl     x0, x0, #8
    ldr     x2, =FAIL_BASE | 0xff
    orr     x0, x0, x2
    str     x0, [x1, #CTL_STATUS]
    b       .

el1_fail:
    ldr     x1, =CTL_BASE
    str     x0, [x1, #CTL_DETAIL]
    ldr     x0, =FAIL_BASE | 0xfe
    str     x0, [x1, #CTL_STATUS]
    b       .

    .ltorg

/* ---------------------------------------------------------------- EL0 */
    .balign 16
el0_main:
    /* 1: a faulting load after dirty registers and flags in the same block */
    LI      x7, HOLE + 0x48
    LI      x6, 0x6666666666666666
    movz    x1, #0x1111
    add     x2, x1, #0x200
    eor     x3, x2, x1, lsl #4
    mov     x4, #-5
    subs    x5, x4, #3                          /* N=1, C=1 */
fault1:
    ldr     x6, [x7]
    CHKSAVE 1, 0x1111, 1
    CHKSAVE 2, 0x1311, 1
    CHKSAVE 3, 0x1311 ^ 0x11110, 1
    CHKSAVE 4, 0xfffffffffffffffb, 1
    CHKSAVE 5, 0xfffffffffffffff8, 1
    CHKSAVE 6, 0x6666666666666666, 1           /* destination unchanged */
    CHKSAVE 31, HOLE + 0x48, 1                 /* FAR */
    adr     x26, fault1
    LI      x27, SAVE
    ldr     x27, [x27, #32 * 8]                 /* ELR */
    cmp     x27, x26
    b.eq    2f
    svc     #1
2:  LI      x27, SAVE
    ldr     x27, [x27, #33 * 8]                 /* SPSR: NZCV = 1010 */
    lsr     x27, x27, #28
    CHK     x27, 0xa, 1

    /* 2: a faulting post-index load: the base is not written back */
    LI      x9, HOLE + 0x100
    LI      x8, 0x8888
    adds    x10, x9, #1                         /* flags: N=0 Z=0 C=0 V=0 */
fault2:
    ldr     x8, [x9], #16
    CHKSAVE 9, HOLE + 0x100, 2
    CHKSAVE 8, 0x8888, 2
    CHKSAVE 10, HOLE + 0x101, 2
    CHK     x9, HOLE + 0x100, 2
    CHK     x8, 0x8888, 2

    /* 3: SP as a base register */
    mov     x20, sp
    LI      x0, 0x0123456789abcdef
    LI      x1, 0xfedcba9876543210
    stp     x0, x1, [sp, #-32]!
    str     x0, [sp, #16]
    ldp     x2, x3, [sp], #16
    ldr     x4, [sp], #16
    mov     x21, sp
    CHK     x21, BUF, 3
    CHK     x2, 0x0123456789abcdef, 3
    CHK     x3, 0xfedcba9876543210, 3
    CHK     x4, 0x0123456789abcdef, 3
    add     sp, sp, #-64
    sub     sp, sp, #-64
    mov     x21, sp
    CHK     x21, BUF, 3

    /* 4: x18, x29, x30 as ordinary registers */
    LI      x18, 0x1818
    LI      x29, 0x2929
    add     x30, x18, x29, lsl #8
    LI      x0, BUF + 0x100
    stp     x29, x30, [x0]
    str     x18, [x0, #16]
    ldp     x18, x29, [x0, #8]
    ldr     x30, [x0]
    CHK     x18, 0x1818 + 0x292900, 4
    CHK     x29, 0x1818, 4
    CHK     x30, 0x2929, 4

    /* 5: flags across slow-path (page-crossing) accesses */
    LI      x0, BUF + 0x4000 - 4                /* an 8-byte access crosses a 16 KiB page */
    LI      x1, 0x5555aaaa5555aaaa
    mov     x2, #3
    cmp     x2, #3                              /* Z=1 C=1 */
    str     x1, [x0]
    ldr     x3, [x0]
    ccmp    x3, x1, #0, eq                      /* still Z=1 if the flags survived */
    b.ne    9f
    cset    x4, cs
    CHK     x4, 1, 5
    b       10f
9:  svc     #5
10:

    /* 6: a store over code ahead in the same block */
    adr     x12, smc_patch
    adr     x13, smc_snip
    sub     x12, x12, x13
    LI      x16, CODE
    add     x12, x16, x12
    LI      x11, 0xd2800040                     /* movz x0, #2 */
    blr     x16
    CHK     x0, 2, 6
    CHK     x10, 0x1111, 6
    CHK     x15, 0x1111 - 0x2222, 6
    CHK     x17, 1, 6

    svc     #0

/* copied to CODE; x11 = the new instruction, x12 = where it goes */
smc_snip:
    movz    x10, #0x1111
    LI      x14, -0x2222
    adds    x15, x10, x14                       /* N=1 */
    str     w11, [x12]
    dc      cvau, x12
    dsb     ish
    ic      ivau, x12
    dsb     ish
    isb
smc_patch:
    movz    x0, #1
    cset    x17, mi
    ret
smc_snip_end:
