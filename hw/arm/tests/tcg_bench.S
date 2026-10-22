/*
 * tcg_bench.S - bare-metal TCG microbenchmarks for the a13-test machine.
 *
 * Proxy workloads for the hot paths in the iOS boot profile (ios-on-pixel
 * docs/PERF.md): indirect branches/returns (helper_lookup_tb_ptr and the
 * per-vCPU jump cache) and system-register reads (TPIDR_EL1, XNU's
 * current_thread()). Entered at EL1, MMU off. Reports PASS when done; the
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
