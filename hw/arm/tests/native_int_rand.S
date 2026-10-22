/*
 * native_int_rand.S - run a random A64 integer program (native_int_gen.py)
 * at EL0 and print its hash, for comparing native-int with TCG and with
 * the host CPU (docs/NATIVE-INT.md).
 *
 * Build with -DBODY=\"body.inc\" (the generated program) and
 * -march=armv8.4-a. Two builds:
 *   - bare metal for the a13-test machine: EL1 turns the MMU on (16 KiB
 *     pages; the image read-only and executable at EL0 and EL1, a 32 MiB
 *     data block read-write at EL0 and never executable), enters EL0 at
 *     nic_el0, which runs the program on a 64 KiB buffer and returns the
 *     hash with SVC; EL1 prints "nic-hash=<16 hex digits>" and reports PASS.
 *     Any other exception reports FAIL with ESR in the detail register.
 *   - -DHOST_HARNESS: exports nic_run(buffer) so the same program can run
 *     natively on an AArch64 host as a third reference.
 *
 * Architectural encodings only; no Apple code or data.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
    .set CTL_BASE,    0x200000000
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set CTL_PUTC,    0x10
    .set PASS_TOKEN,  0x50415353
    .set FAIL_BASE,   0x46000000
    .set DATA,        0x802000000         /* data block: EL0 RW, XN */
    .set BUF,         0x802000000         /* the program's 64 KiB buffer */
    .set L2,          0x803f00000         /* level-2 table, 2048 entries */
    .set EL0_SP,      0x803e00000

    .text
#ifdef HOST_HARNESS
    .globl _nic_run
    .globl nic_run
    .p2align 2
_nic_run:
nic_run:
    stp     x29, x30, [sp, #-96]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    bl      nic_body
    bl      nic_hash_buffer
    ldp     x27, x28, [sp, #80]
    ldp     x25, x26, [sp, #64]
    ldp     x23, x24, [sp, #48]
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #96
    ret
#else
    .globl _start
_start:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)                  /* FPEN: no FP/SIMD traps at EL0/EL1 */
    msr     cpacr_el1, x0
    adr     x0, vectors
    msr     vbar_el1, x0

    ldr     x0, =L2
    mov     x1, #2048
1:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    1b
    ldr     x0, =L2
    ldr     x1, =0x800000000 | 0x7c1            /* image: 32 MiB block, AF, inner shareable, Attr0, AP=RO EL0+EL1 */
    str     x1, [x0, #0x400 * 8]
    ldr     x1, =DATA | 0x0060000000000741      /* data: AP=RW EL0+EL1, PXN, UXN */
    str     x1, [x0, #0x401 * 8]
    ldr     x1, =CTL_BASE | 0x0060000000000405  /* device block, EL1 only */
    str     x1, [x0, #0x100 * 8]
    dsb     ish
    ldr     x0, =0x00ff                         /* Attr0 normal WB, Attr1 device */
    msr     mair_el1, x0
    /* T0SZ=28 (36-bit VA, level 2 start), TG0=16K, EPD1, TG1=16K, IPS=36 bits */
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
    orr     x0, x0, #(1 << 23)                  /* SPAN: exception entry leaves PAN alone */
    bic     x0, x0, #(1 << 1)                   /* A */
    bic     x0, x0, #(1 << 19)                  /* WXN */
    msr     sctlr_el1, x0
    isb

    ldr     x0, =EL0_SP
    msr     sp_el0, x0
    adr     x0, nic_el0
    msr     elr_el1, x0
    mov     x0, #0x3c0                          /* EL0t, DAIF masked */
    msr     spsr_el1, x0
    ldr     x0, =BUF
    eret

nic_el0:
    bl      nic_body
    bl      nic_hash_buffer
    svc     #0

    .balign 2048
vectors:
    .rept 16                                    /* every vector; 0x400 is synchronous from EL0 (SVC) */
    b       el1_unexpected
    .balign 0x80
    .endr

el1_unexpected:
    mrs     x1, esr_el1
    lsr     x2, x1, #26
    cmp     x2, #0x15                           /* SVC from AArch64 */
    b.ne    fail
    mov     x19, x0
    ldr     x1, =CTL_BASE
    adr     x2, msg
7:  ldrb    w0, [x2], #1
    cbz     w0, 8f
    str     x0, [x1, #CTL_PUTC]
    b       7b
8:  mov     x3, #60
9:  lsr     x0, x19, x3
    and     x0, x0, #15
    cmp     x0, #10
    add     x4, x0, #'0'
    add     x5, x0, #('a' - 10)
    csel    x0, x4, x5, lo
    str     x0, [x1, #CTL_PUTC]
    subs    x3, x3, #4
    b.pl    9b
    mov     x0, #10
    str     x0, [x1, #CTL_PUTC]
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    b       .

fail:
    ldr     x2, =CTL_BASE
    str     x1, [x2, #CTL_DETAIL]
    ldr     x0, =FAIL_BASE | (1 << 8) | 0xfe
    str     x0, [x2, #CTL_STATUS]
    b       .

msg:
    .asciz "nic-hash="
    .balign 4
    .ltorg
#endif

    /* Page aligned in every build: the ADRP results the program uses are relative to its page. */
    .balign 16384
nic_body:
#include BODY
    ret

/* Fold the 64 KiB buffer at x0 into the hash in x23; return it in x0. */
nic_hash_buffer:
    mov     x1, #0
    mov     x3, #0x10000
1:  ldr     x2, [x0, x1]
    eor     x23, x23, x2
    ror     x23, x23, #13
    add     x23, x23, x1
    add     x1, x1, #8
    cmp     x1, x3
    b.ne    1b
    mov     x0, x23
    ret
