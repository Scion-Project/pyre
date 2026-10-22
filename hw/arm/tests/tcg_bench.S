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
 *  10  loads and stores that hit the softmmu TLB: 16 per iteration, mixed
 *      sizes, within one 16 KiB page (MMU off)
 *  11  TLB refills: loads scattered over 4096 16 KiB pages (MMU on, 64 MiB
 *      of VA aliased onto 512 KiB of RAM), so nearly every access walks the
 *      page table unless the softmmu TLB holds all 4096 pages
 *  12  compiled code: a clang -O2 C kernel (list walk with a hashing call
 *      per node, then a branchy byte state machine over a string), the
 *      kind of integer code user processes run
 *
 * -DEL0 (workloads 1, 2, 4, 7, 8, 10, 12): run the workload at EL0 with the
 * MMU on (16 KiB pages: the image read-only and executable at EL0, a data
 * block read-write at EL0), as user code runs; it ends with SVC #0. This is
 * what -accel tcg,native-int=on applies to (docs/NATIVE-INT.md).
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

#ifdef EL0
    .set DATA,    0x802000000                   /* EL0 RW, XN: data, stack, page table */
    .set W10_BUF, 0x802400000
    .set W12_DATA, 0x802800000
    .set EL0_SP,  0x803e00000
    .set EL0_L2,  0x803f00000
#else
    .set W10_BUF, 0x800400000
    .set W12_DATA, 0x800800000
#endif

    .text
    .globl _start
_start:
#ifdef EL0
    adr     x0, el1_vectors
    msr     vbar_el1, x0
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)                  /* FPEN: no FP/SIMD traps at EL0/EL1 */
    msr     cpacr_el1, x0
    ldr     x0, =EL0_L2
    mov     x1, #2048
5:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    5b
    ldr     x0, =EL0_L2
    ldr     x1, =0x800000000 | 0x7c1            /* image: AF, inner shareable, Attr0, RO at EL0 and EL1 */
    str     x1, [x0, #0x400 * 8]
    ldr     x1, =DATA | 0x0060000000000741      /* RW at EL0 and EL1, PXN, UXN */
    str     x1, [x0, #0x401 * 8]
    ldr     x1, =CTL_BASE | 0x0060000000000405  /* device, EL1 only */
    str     x1, [x0, #0x100 * 8]
    dsb     ish
    ldr     x0, =0x00ff
    msr     mair_el1, x0
    ldr     x0, =28 | (1 << 8) | (1 << 10) | (3 << 12) | (2 << 14) | (28 << 16) | (1 << 23) | (1 << 30) | (1 << 32)
    msr     tcr_el1, x0
    ldr     x0, =EL0_L2
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
    bic     x0, x0, #(1 << 1)                   /* A */
    bic     x0, x0, #(1 << 19)                  /* WXN */
    msr     sctlr_el1, x0
    isb
    ldr     x0, =EL0_SP
    msr     sp_el0, x0
    adr     x0, el0_start
    msr     elr_el1, x0
    mov     x0, #0x3c0                          /* EL0t, DAIF masked */
    msr     spsr_el1, x0
    eret
el0_start:
#endif
    ldr     x20, =ITERS
    adr     x21, funcs
    mov     x22, #0                     /* target index */
    ldr     x23, =FSTRIDE
    ldr     x24, =(NFUNC - 1)
#ifndef EL0
    msr     tpidr_el1, x21
#endif

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
#ifndef EL0
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)          /* FPEN: no FP/SIMD traps at EL1 */
    msr     cpacr_el1, x0
    isb
#endif
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
#ifndef EL0
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)          /* FPEN: no FP/SIMD traps at EL1 */
    msr     cpacr_el1, x0
    isb
#endif
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
#elif WORKLOAD == 10
    ldr     x1, =W10_BUF
1:  ldr     x2, [x1]
    ldr     w3, [x1, #0x1008]
    ldrh    w4, [x1, #0x1010]
    ldrb    w5, [x1, #0xf18]
    str     x2, [x1, #0x40]
    str     w3, [x1, #0x1048]
    strh    w4, [x1, #0x1050]
    strb    w5, [x1, #0xf58]
    ldr     x6, [x1, #0x3080]
    ldr     x7, [x1, #0x2088]
    ldr     w8, [x1, #0x1090]
    ldrsw   x9, [x1, #0x98]
    str     x6, [x1, #0xc0]
    str     x7, [x1, #0x10c8]
    str     w8, [x1, #0x20d0]
    str     x9, [x1, #0x30d8]
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 11
    .set W11_L2,  0x8001c0000               /* level-2 table, after the image */
    .set W11_L3,  0x800200000               /* two level-3 tables: 4096 16 KiB pages */
    .set W11_VA,  0x804000000               /* 64 MiB of VA ... */
    .set W11_PA,  0x802000000               /* ... onto 32 pages of RAM */
    ldr     x0, =W11_L2
    mov     x1, #2048
5:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    5b
    ldr     x0, =W11_L2
    ldr     x1, =0x800000000 | 0x701        /* 32 MiB block (code, tables), AF, inner shareable, Attr0 */
    str     x1, [x0, #0x400 * 8]
    ldr     x1, =CTL_BASE | 0x0060000000000405 /* device block */
    str     x1, [x0, #0x100 * 8]
    ldr     x1, =W11_L3 | 3
    str     x1, [x0, #0x402 * 8]
    ldr     x1, =(W11_L3 + 0x4000) | 3
    str     x1, [x0, #0x403 * 8]
    ldr     x0, =W11_L3
    mov     x2, #0
    ldr     x3, =W11_PA | 0x703             /* page, AF, inner shareable, Attr0 */
6:  and     x4, x2, #31
    add     x1, x3, x4, lsl #14
    str     x1, [x0, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, #4096
    b.ne    6b
    dsb     ish
    ldr     x0, =0x00ff                     /* Attr0 normal WB, Attr1 device */
    msr     mair_el1, x0
    /* T0SZ=28 (36-bit VA, level 2 start), TG0=16K, EPD1, TG1=16K, IPS=36 bits */
    ldr     x0, =28 | (1 << 8) | (1 << 10) | (3 << 12) | (2 << 14) | (28 << 16) | (1 << 23) | (1 << 30) | (1 << 32)
    msr     tcr_el1, x0
    ldr     x0, =W11_L2
    msr     ttbr0_el1, x0
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
    ldr     x27, =W11_VA
    mov     x22, #0
    ldr     x23, =5039                      /* odd stride over 16384 4 KiB slots: every page, 4x a cycle */
    ldr     x24, =16383
1:
    .rept 4
    add     x0, x27, x22, lsl #12
    ldr     x1, [x0]
    add     x22, x22, x23
    and     x22, x22, x24
    .endr
    subs    x20, x20, #1
    b.ne    1b
#elif WORKLOAD == 12
    /* 256 list nodes {next, key, val} linked in a scrambled order, and a 1024-byte string */
    ldr     x9, =W12_DATA
#ifndef EL0
    add     sp, x9, #0x10000
#endif
    mov     x10, #0                             /* node index */
    mov     x11, #1
    ldr     w12, =0x9e3779b9
6:  add     x13, x9, x10, lsl #4                /* node i */
    mul     w14, w11, w12
    str     w14, [x13, #8]                      /* key */
    lsr     w15, w14, #7
    str     w15, [x13, #12]                     /* val */
    add     x16, x10, #97                       /* next = node (i + 97) % 256, 0 ends the list */
    and     x16, x16, #255
    add     x17, x9, x16, lsl #4
    cmp     x16, #0
    csel    x17, xzr, x17, eq
    str     x17, [x13]
    add     w11, w11, #1
    add     x10, x10, #1
    cmp     x10, #256
    b.ne    6b
    add     x22, x9, #0x1000                    /* the string */
    adr     x13, w12_chars
    mov     x10, #0
7:  mul     w14, w11, w12
    add     w11, w11, #1
    lsr     w14, w14, #28
    ldrb    w15, [x13, x14]
    strb    w15, [x22, x10]
    add     x10, x10, #1
    cmp     x10, #1024
    b.ne    7b
    mov     x21, x9
    mov     x25, #0
1:  mov     x0, x21
    mov     x1, x22
    mov     w2, #1024
    bl      w12_kernel
    add     x25, x25, x0
    subs    x20, x20, #1
    b.ne    1b
#else
#error unknown WORKLOAD
#endif

#ifdef EL0
    svc     #0
#endif
    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
2:  wfi
    b       2b

#ifdef EL0
el1_pass:
    mrs     x0, esr_el1
    lsr     x0, x0, #26
    cmp     x0, #0x15                           /* SVC from EL0: done */
    b.ne    el1_fail
    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    b       .
el1_fail:
    mrs     x0, esr_el1
    ldr     x1, =CTL_BASE
    str     x0, [x1, #CTL_STATUS]
    b       .
    .ltorg
    .balign 2048
el1_vectors:
    .rept 16
    b       el1_pass
    .balign 0x80
    .endr
#endif

#if WORKLOAD == 12
/*
 * w12_kernel(head, str, n) and w12_mix(): clang -O2 -fno-jump-tables for
 * aarch64-none-elf, from this C:
 *
 *   struct node { struct node *next; uint32_t key; uint32_t val; };
 *   __attribute__((noinline)) uint32_t w12_mix(uint32_t h, uint32_t k)
 *   {   k *= 0xcc9e2d51u; k = (k << 15) | (k >> 17); k *= 0x1b873593u;
 *       h ^= k; h = (h << 13) | (h >> 19); return h * 5 + 0xe6546b64u; }
 *   uint64_t w12_kernel(struct node *head, const uint8_t *str, int n)
 *   {   uint64_t acc = 0; uint32_t h = 0; int state = 0;
 *       for (struct node *p = head; p; p = p->next) {
 *           h = w12_mix(h, p->key);
 *           if (p->val & 1) acc += p->val; else if (p->val & 2) acc ^= h; else acc -= p->key;
 *       }
 *       for (int i = 0; i < n; i++) {
 *           uint8_t c = str[i];
 *           switch (state) {
 *           case 0: if (c >= '0' && c <= '9') state = 1; else if (c == ' ') state = 2; break;
 *           case 1: if (c < '0' || c > '9') state = c == '.' ? 3 : 0; acc += c; break;
 *           case 2: state = c == ' ' ? 2 : 0; break;
 *           default: state = 0; acc ^= c;
 *           }
 *       }
 *       return acc + h + state;
 *   }
 */
w12_mix:
    mov     w8, #11601
    mov     w9, #32768
    movk    w8, #52382, lsl #16
    movk    w9, #5800, lsl #16
    mul     w8, w1, w8
    mul     w9, w1, w9
    bfxil   w9, w8, #17, #15
    mov     w8, #13715
    movk    w8, #7047, lsl #16
    mul     w8, w9, w8
    mov     w9, #27492
    movk    w9, #58964, lsl #16
    eor     w8, w8, w0
    ror     w8, w8, #19
    add     w8, w8, w8, lsl #2
    add     w0, w8, w9
    ret
w12_kernel:
    stp     x30, x23, [sp, #-48]!
    stp     x20, x19, [sp, #32]
    mov     w20, w2
    mov     x19, x1
    stp     x22, x21, [sp, #16]
    cbz     x0, 10f
    mov     x21, x0
    mov     x23, xzr
    mov     w0, wzr
2:  ldr     w22, [x21, #8]
    mov     w1, w22
    bl      w12_mix
    ldr     w9, [x21, #12]
    mov     w8, w0
    sub     x10, x23, x22
    eor     x11, x23, x8
    ldr     x21, [x21]
    tst     w9, #0x2
    csel    x10, x10, x11, eq
    add     x11, x23, x9
    tst     w9, #0x1
    csel    x23, x10, x11, eq
    cbnz    x21, 2b
    cmp     w20, #1
    b.lt    11f
4:  mov     w10, wzr
    mov     w9, w20
    mov     w11, #2
    mov     w12, #3
    mov     w13, #1
    b       7f
5:  cmp     w14, #32
    csel    w10, w11, wzr, eq
6:  subs    x9, x9, #1
    b.eq    9f
7:  ldrb    w14, [x19], #1
    cmp     w10, #2
    b.eq    5b
    sub     w15, w14, #58
    cmp     w14, #46
    add     x17, x23, x14
    csel    w16, w12, wzr, eq
    cmn     w15, #10
    csinc   w15, w16, wzr, lo
    sub     w16, w14, #48
    cmp     w14, #32
    csel    w18, w11, wzr, eq
    cmp     w16, #10
    eor     x14, x23, x14
    csel    w16, w13, w18, lo
    cmp     w10, #0
    csel    w16, wzr, w16, ne
    csel    x14, x14, x23, ne
    cmp     w10, #1
    csel    w10, w15, w16, eq
    csel    x23, x17, x14, eq
    b       6b
9:  mov     w9, w10
    b       12f
10: mov     x8, xzr
    mov     x23, xzr
    cmp     w20, #1
    b.ge    4b
11: mov     x9, xzr
12: ldp     x20, x19, [sp, #32]
    add     x8, x9, x8
    ldp     x22, x21, [sp, #16]
    add     x0, x8, x23
    ldp     x30, x23, [sp], #48
    ret
w12_chars:
    .ascii  "0123456789 . ab "
#endif

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
