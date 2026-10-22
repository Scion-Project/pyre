/*
 * tcg_bench.S - bare-metal TCG microbenchmarks for the a13-test machine.
 *
 * Proxy workloads for the hot paths in the iOS boot profile (ios-on-pixel
 * docs/PERF.md): indirect branches/returns (helper_lookup_tb_ptr and the
 * per-vCPU jump cache) and system-register reads (TPIDR_EL1, XNU's
 * current_thread()). Entered at EL1, MMU off (workload 9 turns it on).
 * Reports PASS when done; the
 * runner times the QEMU process. No Apple code or data.
 *
 * Build with -DWORKLOAD=<n> (see below), optionally -DITERS=, -DNFUNC=,
 * -DFSTRIDE=. hw/arm/tests/run-a13-bench.py builds and times them.
 *
 * Workloads:
 *   1  hot BL/RET: RET through lookup_and_goto_ptr, always a jump-cache hit
 *   2  BLR/RET over NFUNC targets FSTRIDE bytes apart (jump-cache pressure)
 *   3  MRS TPIDR_EL1 (a GXF-banked register; XNU's current_thread())
 *   4  plain ALU loop (control: no lookups, no helpers)
 *   5  as 2, plus a store over just-executed code every 64 iterations
 *      (one TB invalidation each)
 *   6  MSR TPIDR_EL1 / MSR ELR_EL1 (system-register writes)
 *   7  vector FP: FMLA/FMUL/FADD on .4s and .2d (softfloat helpers today)
 *   8  scalar FP: FMADD/FMUL/FADD/FDIV/FSQRT, FCMP, FCVTZS/SCVTF (double and
 *      single), register-only
 *   9  KPTI-style system calls: two ASID switches per iteration, loads from
 *      16 pages and 4 spread calls in between (turns the MMU on, 16 KiB
 *      pages, all mappings non-global)
 *
 * This file is a test input; it is not compiled into QEMU. It uses only
 * architectural encodings - no Apple code or data.
 */
    .set CTL_BASE,    0x200000000
    .set CTL_STATUS,  0x08
    .set PASS_TOKEN,  0x50415353

#ifndef WORKLOAD
#define WORKLOAD 1
#endif
#ifndef ITERS
#define ITERS 20000000
#endif
#ifndef NFUNC
#define NFUNC 4096          /* call targets for the spread workloads (power of 2) */
#endif
#ifndef FSTRIDE
#define FSTRIDE 272         /* bytes between call targets */
#endif

    .text
    .globl _start
_start:
    ldr     x20, =ITERS
    adr     x21, funcs
    mov     x22, #0                     /* target index */
    ldr     x23, =FSTRIDE
    ldr     x24, =(NFUNC - 1)
    msr     tpidr_el1, x21

#if WORKLOAD == 1
    /* hot call/return: one BL target, RET through the jump cache */
1:  bl      leaf
    bl      leaf
    bl      leaf
    bl      leaf
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 2
    /* spread indirect calls: BLR to NFUNC targets FSTRIDE bytes apart, in a
     * scrambled order, each returning with RET */
1:  madd    x0, x22, x23, x21
    blr     x0
    add     x22, x22, #709
    and     x22, x22, x24
    madd    x0, x22, x23, x21
    blr     x0
    add     x22, x22, #709
    and     x22, x22, x24
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 3
    /* system-register reads: 16 x MRS TPIDR_EL1 per iteration, each value
     * used (so TCG cannot drop dead loads) */
    mov     x1, #0
1:  .rept 16
    mrs     x0, tpidr_el1
    add     x1, x1, x0
    .endr
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 4
    /* plain guest code, no lookups or helpers: a dependent ALU chain */
    mov     x0, #1
1:  .rept 16
    add     x0, x0, x0, lsl #1
    eor     x0, x0, x20
    .endr
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 5
    /* spread indirect calls (as 2) plus, every 64 iterations, a call into a
     * code page followed by a store over the code just run: each store
     * invalidates that TB (guest code-page writes: page reuse, JITs, loaders) */
1:  madd    x0, x22, x23, x21
    blr     x0
    add     x22, x22, #709
    and     x22, x22, x24
    madd    x0, x22, x23, x21
    blr     x0
    add     x22, x22, #709
    and     x22, x22, x24
    tst     x20, #63
    b.ne    2f
    adr     x0, smc_page
    blr     x0
    ldr     w1, [x0]
    str     w1, [x0]                    /* rewrite the RET in place */
2:  subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 6
    /* system-register writes: 4 x (MSR TPIDR_EL1 / MSR ELR_EL1) per
     * iteration, as in XNU's context-switch and exception-return paths;
     * each write used to end the TB */
1:  .rept 4
    msr     tpidr_el1, x20
    msr     elr_el1, x20
    .endr
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 7
    /* vector FP, register-only: 4 x (FMLA, FMUL, FADD) on .4s and on .2d per
     * iteration; every one is a gvec helper call into softfloat today */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)          /* FPEN: no FP/SIMD traps at EL1 */
    msr     cpacr_el1, x0
    isb
    fmov    v0.4s, #1.0
    fmov    v1.4s, #0.5
    fmov    v2.2d, #1.0
    fmov    v3.2d, #0.5
    movi    v4.4s, #0
    movi    v7.2d, #0
1:  .rept 4
    fmla    v4.4s, v0.4s, v1.4s
    fmul    v5.4s, v4.4s, v1.4s
    fadd    v6.4s, v5.4s, v0.4s
    fmla    v7.2d, v2.2d, v3.2d
    fmul    v16.2d, v7.2d, v3.2d
    fadd    v17.2d, v16.2d, v2.2d
    .endr
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 8
    /* scalar FP, register-only: 2 x 10 FP instructions per iteration, each
     * a softfloat helper call today */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)          /* FPEN: no FP/SIMD traps at EL1 */
    msr     cpacr_el1, x0
    isb
    fmov    d0, #1.5
    fmov    d1, #0.5
    fmov    d2, #3.0
    fmov    s16, #1.5
    fmov    s17, #0.5
    fmov    s18, #3.0
    movi    d4, #0
    movi    d20, #0
1:  .rept 2
    fmadd   d4, d0, d1, d4
    fmul    d5, d4, d1
    fadd    d6, d5, d2
    fdiv    d7, d6, d2
    fsqrt   d8, d7
    fcmp    d8, d2
    fcvtzs  x9, d8
    scvtf   d9, x9
    fmadd   s20, s16, s17, s20
    fdiv    s21, s20, s18
    .endr
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 9
    /*
     * KPTI-style system calls: two address spaces with the same (non-global)
     * mappings under ASIDs 1 ("user") and 2 ("kernel"). Each iteration loads
     * from 8 pages, switches TTBR0_EL1 to the other ASID (the system call
     * entry), makes 4 spread calls and loads from 8 other pages, then
     * switches back. Every switch is an ASID change.
     */
    .set W9_L2,   0x8001c0000               /* level-2 table, after the image */
    .set W9_DATA, 0x800200000               /* 16 data pages */
    ldr     x0, =W9_L2
    mov     x1, #2048
5:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    5b
    ldr     x0, =W9_L2
    ldr     x1, =0x800000000 | 0xf01        /* 32 MiB block, AF, inner shareable, Attr0, nG */
    str     x1, [x0, #0x400 * 8]
    ldr     x2, =0x2000000
    add     x1, x1, x2
    str     x1, [x0, #0x401 * 8]
    ldr     x1, =CTL_BASE | 0x0060000000000405 /* device block, global */
    str     x1, [x0, #0x100 * 8]
    dsb     ish
    ldr     x0, =0x00ff                     /* Attr0 normal WB, Attr1 device */
    msr     mair_el1, x0
    /* T0SZ=28 (36-bit VA, level 2 start), TG0=16K, EPD1, TG1=16K, IPS=36 bits, ASID from TTBR0 */
    ldr     x0, =28 | (1 << 8) | (1 << 10) | (3 << 12) | (2 << 14) | (28 << 16) | (1 << 23) | (1 << 30) | (1 << 32)
    msr     tcr_el1, x0
    ldr     x25, =W9_L2 | (1 << 48)
    ldr     x26, =W9_L2 | (2 << 48)
    msr     ttbr0_el1, x25
    isb
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)               /* M */
    orr     x0, x0, #(1 << 2)               /* C */
    orr     x0, x0, #(1 << 12)              /* I */
    bic     x0, x0, #(1 << 1)               /* A */
    bic     x0, x0, #(1 << 19)              /* WXN */
    msr     sctlr_el1, x0
    isb
    ldr     x27, =W9_DATA
1:  mov     x1, x27
    .rept 8
    ldr     x0, [x1]
    add     x1, x1, #4, lsl #12
    .endr
    msr     ttbr0_el1, x26
    isb
    .rept 4
    madd    x0, x22, x23, x21
    blr     x0
    add     x22, x22, #709
    and     x22, x22, x24
    .endr
    add     x1, x27, #32, lsl #12
    .rept 8
    ldr     x0, [x1]
    add     x1, x1, #4, lsl #12
    .endr
    msr     ttbr0_el1, x25
    isb
    subs    x20, x20, #1
    b.ne    1b
#else
#error unknown WORKLOAD
#endif

    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
2:  wfi
    b       2b

leaf:
    ret

    .ltorg
    .balign 16384
smc_page:
    ret
    .balign 16384
funcs:
    .rept NFUNC
    ret
    .skip FSTRIDE - 4
    .endr
