/*
 * fp_native_check.S - bit-exactness check for -accel tcg,native-fp=on
 * (guest FP executed on the host FPU, docs/NATIVE-FP.md).
 *
 * Runs every covered operation over all (a, b, c) triples of a table of
 * special operands (signed zeros, smallest/largest denormal, smallest normal,
 * max, infinities, quiet and signalling NaNs with payloads, ordinary values),
 * per precision, and folds the result bits and the FPSR after each operation
 * into a 64-bit FNV-style hash. The whole run is repeated under several FPCR
 * modes (native-compatible FPCR = 0, then FZ, DN, directed rounding, FZ|DN,
 * then FPCR = 0 again), so switching between the host-FPU and softfloat
 * regimes is covered too.
 *
 * The expected hash below is the softfloat (native-fp=off) result. The test
 * prints the hash on the test-control PUTC register, then reports PASS if it
 * matches and FAIL (check 1, value mismatch, DETAIL = hash) if not.
 *
 *   -DSTICKY: never clear FPSR, so the hash also covers cumulative-flag
 *             accumulation across operations and across FPCR writes.
 *
 * run-a13-tests.py runs it with native-fp off and on, and with an injected
 * native-path fault (negative control: the hash must change, and the host
 * self-test must catch the fault when it is not skipped).
 *
 *   -DHOST_HARNESS: instead of the bare-metal entry, export fpc_run() (returns
 *             the hash) so the same instruction sequence can be run natively
 *             on an AArch64 host as a third reference (FPCR/FPSR are EL0
 *             accessible).
 *
 * Architectural encodings only; no Apple code or data.
 */
#ifdef STICKY
#define CLEAR_FPSR nop
#define GOLDEN     0xff2aba1fe8b66497
#else
#define CLEAR_FPSR msr fpsr, xzr
#define GOLDEN     0xcab9271bfc0dd5ab
#endif
    .set CTL_BASE,    0x200000000
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set CTL_PUTC,    0x10
    .set PASS_TOKEN,  0x50415353
    .set FAIL_BASE,   0x46000000
    .set NDBL, 20
    .set NSGL, 20

/* 64-bit immediate without a literal pool (works for ELF and Mach-O) */
.macro MOVQ reg, imm
    movz    \reg, #((\imm) & 0xffff)
    movk    \reg, #(((\imm) >> 16) & 0xffff), lsl #16
    movk    \reg, #(((\imm) >> 32) & 0xffff), lsl #32
    movk    \reg, #(((\imm) >> 48) & 0xffff), lsl #48
.endm

    .text
#ifdef HOST_HARNESS
    .globl _fpc_run
    .globl fpc_run
    .p2align 2
_fpc_run:
fpc_run:
    stp     x29, x30, [sp, #-64]!
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    bl      fpc_body
    mov     x0, x19
    ldp     x23, x24, [sp, #48]
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #64
    ret
#else
    .globl _start
_start:
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)
    msr     cpacr_el1, x0
    isb
    bl      fpc_body
    b       report
#endif

/* the whole check; hash in x19 */
fpc_body:
    mov     x20, x30
    msr     fpsr, xzr
    MOVQ    x19, 0xcbf29ce484222325      /* hash */
    MOVQ    x24, 0x100000001b3           /* FNV prime */

    adr     x21, modes
    mov     x22, #NMODES
0:  ldr     x0, [x21], #8
    msr     fpcr, x0
    bl      run_dbl
    bl      run_sgl
    subs    x22, x22, #1
    b.ne    0b
    msr     fpcr, xzr
    ret     x20

#ifndef HOST_HARNESS
report:
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

    ldr     x0, =GOLDEN
    cmp     x19, x0
    b.ne    fail
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
10: wfi
    b       10b

fail:
    str     x19, [x1, #CTL_DETAIL]
    ldr     x0, =(FAIL_BASE | (1 << 8) | 0xff)
    str     x0, [x1, #CTL_STATUS]
11: wfi
    b       11b
#endif

/* double precision: v0 = {a, b}, v1 = {b, c}, v2 = {c, a} */
run_dbl:
    mov     x23, x30
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
    ret     x23

/* single precision: lanes mix a, b, c; .4s and .2s forms */
run_sgl:
    mov     x23, x30
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
    CLEAR_FPSR
    \op    v3.2s, v1.2s, v2.2s
    bl      mix_v3
    .endr
    .irp op, fmla, fmls
    mov     v3.16b, v2.16b
    CLEAR_FPSR
    \op    v3.4s, v0.4s, v1.4s
    bl      mix_v3
    mov     v3.16b, v0.16b
    CLEAR_FPSR
    \op    v3.2s, v1.2s, v2.2s
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
    ret     x23

/* hash ^= v3.d[0], v3.d[1], fpsr (each followed by *= prime) */
mix_v3:
    mrs     x9, fpsr
    fmov    x8, d3
    eor     x19, x19, x8
    mul     x19, x19, x24
    mov     x8, v3.d[1]
    eor     x19, x19, x8
    mul     x19, x19, x24
    eor     x19, x19, x9
    mul     x19, x19, x24
    ret

msg:
    .asciz "fp-hash="
    .balign 8
/* FPCR per pass: native (0), FZ, DN, RMode=RP, RMode=RZ, FZ|DN, native again */
modes:
    .quad 0, (1 << 24), (1 << 25), (1 << 22), (3 << 22), (3 << 24), 0
    .set NMODES, 7
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
#ifndef HOST_HARNESS
    .ltorg
#endif
