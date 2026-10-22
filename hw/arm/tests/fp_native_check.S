/*
 * fp_native_check.S - result/flag fingerprint of vector FADD/FSUB/FMUL/FMLA/FMLS
 * (.4s and .2d) over a table of special operands (signed zeros, denormals,
 * infinities, quiet and signalling NaNs with payloads, extremes, ordinary
 * values), for checking QEMU_TCG_NATIVE_FP=1 against softfloat.
 *
 * Every (a, b, c) triple from the table is run through each operation; the
 * result bits and the FPSR after the operation (cleared before it) are folded
 * into a 64-bit FNV-style hash, printed in hex on the test-control PUTC
 * register, then the test reports PASS. Run it with and without
 * QEMU_TCG_NATIVE_FP=1: the two hashes must be identical. Build with -DSTICKY
 * as well (FPSR never cleared) to check cumulative-flag accumulation.
 *
 * Architectural encodings only; no Apple code or data.
 */
/* -DSTICKY: never clear FPSR, so the hash also covers cumulative-flag accumulation */
#ifdef STICKY
#define CLEAR_FPSR nop
#else
#define CLEAR_FPSR msr fpsr, xzr
#endif
    .set CTL_BASE,    0x200000000
    .set CTL_STATUS,  0x08
    .set CTL_PUTC,    0x10
    .set PASS_TOKEN,  0x50415353
    .set NDBL, 20
    .set NSGL, 20

    .text
    .globl _start
_start:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    ldr     x19, =0xcbf29ce484222325     /* hash */
    ldr     x18, =0x100000001b3          /* FNV prime */

    /* double precision: lanes {a, b} x {b, c} etc. */
    adr     x10, dtab
    mov     x11, #NDBL
    mov     x12, #0                      /* i */
1:  mov     x13, #0                      /* j */
2:  mov     x14, #0                      /* k */
3:  ldr     d0, [x10, x12, lsl #3]
    ldr     d1, [x10, x13, lsl #3]
    ldr     d2, [x10, x14, lsl #3]
    mov     v0.d[1], v1.d[0]
    mov     v1.d[1], v2.d[0]
    mov     v2.d[1], v0.d[0]
    .irp op, fadd, fsub, fmul
    CLEAR_FPSR
    \op    v3.2d, v0.2d, v1.2d
    bl      mix_v3
    .endr
    .irp op, fmla, fmls
    mov     v3.16b, v2.16b
    CLEAR_FPSR
    \op    v3.2d, v0.2d, v1.2d
    bl      mix_v3
    .endr
    add     x14, x14, #1
    cmp     x14, x11
    b.ne    3b
    add     x13, x13, #1
    cmp     x13, x11
    b.ne    2b
    add     x12, x12, #1
    cmp     x12, x11
    b.ne    1b

    /* single precision */
    adr     x10, stab
    mov     x11, #NSGL
    mov     x12, #0
4:  mov     x13, #0
5:  mov     x14, #0
6:  ldr     s0, [x10, x12, lsl #2]
    ldr     s1, [x10, x13, lsl #2]
    ldr     s2, [x10, x14, lsl #2]
    mov     v0.s[1], v1.s[0]
    mov     v0.s[2], v2.s[0]
    mov     v0.s[3], v1.s[0]
    mov     v1.s[1], v2.s[0]
    mov     v1.s[2], v0.s[0]
    mov     v1.s[3], v2.s[0]
    mov     v2.s[1], v0.s[0]
    mov     v2.s[2], v1.s[0]
    mov     v2.s[3], v0.s[0]
    .irp op, fadd, fsub, fmul
    CLEAR_FPSR
    \op    v3.4s, v0.4s, v1.4s
    bl      mix_v3
    .endr
    .irp op, fmla, fmls
    mov     v3.16b, v2.16b
    CLEAR_FPSR
    \op    v3.4s, v0.4s, v1.4s
    bl      mix_v3
    .endr
    add     x14, x14, #1
    cmp     x14, x11
    b.ne    6b
    add     x13, x13, #1
    cmp     x13, x11
    b.ne    5b
    add     x12, x12, #1
    cmp     x12, x11
    b.ne    4b

    /* print "fp-hash=<16 hex digits>\n" */
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
10: wfi
    b       10b

/* hash ^= v3.d[0], v3.d[1], fpsr (each followed by *= prime) */
mix_v3:
    mrs     x9, fpsr
    fmov    x8, d3
    eor     x19, x19, x8
    mul     x19, x19, x18
    mov     x8, v3.d[1]
    eor     x19, x19, x8
    mul     x19, x19, x18
    eor     x19, x19, x9
    mul     x19, x19, x18
    ret

msg:
    .asciz "fp-hash="
    .balign 8
dtab:
    .quad 0x0000000000000000, 0x8000000000000000, 0x0000000000000001, 0x800fffffffffffff
    .quad 0x0010000000000000, 0x7fefffffffffffff, 0x7ff0000000000000, 0xfff0000000000000
    .quad 0x7ff8000000000000, 0xfff8000000000123, 0x7ff0000000000456, 0x7ff4000000000789
    .quad 0x3ff0000000000000, 0xbff8000000000000, 0x3fb999999999999a, 0x4340000000000001
    .quad 0x3cb0000000000000, 0x7fe0000000000000, 0x0008000000000000, 0xc00921fb54442d18
stab:
    .long 0x00000000, 0x80000000, 0x00000001, 0x807fffff
    .long 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000
    .long 0x7fc00000, 0xffc00123, 0x7f800456, 0x7fa00789
    .long 0x3f800000, 0xbfc00000, 0x3dcccccd, 0x4b000001
    .long 0x33800000, 0x7f000000, 0x00400000, 0xc0490fdb
    .ltorg
