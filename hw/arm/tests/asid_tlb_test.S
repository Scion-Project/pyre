/*
 * asid_tlb_test.S - TTBR0_EL1 ASID switches with QEMU's ASID TLB contexts.
 *
 * When the guest changes the ASID in TTBR0_EL1, QEMU keeps the outgoing
 * address space's TLBs aside and brings them back when the guest returns to
 * it ("-accel tcg,asid-tlbs=N", see tlb_switch_asid_ctx()). A kept-aside TLB
 * must receive every invalidation the live TLB receives. This test maps one
 * VA (T) to a different page under each of 12 ASIDs (non-global, nG=1; RAM,
 * code and the control registers are mapped global) and checks:
 *
 *   1-     4 ASIDs round robin: each sees its own page (TLBs restored)
 *          12 ASIDs round robin: more than the 8 default slots (eviction)
 *          TLBI VAE1IS / VALE1IS / VAAE1IS / RVAE1IS (if implemented) of a
 *          kept-aside ASID's page while another ASID is current
 *          TLBI VMALLE1 and TLBI ASIDE1 while kept aside
 *          self-modifying code through a kept-aside writable alias: the
 *          page becomes code while its writer's TLB is kept aside, then
 *          a write through the restored entry must still invalidate the
 *          translated code (dirty tracking reaches kept-aside TLBs)
 *          (after the probe) code at one VA, different in two address
 *          spaces, called alternately through BLR (the jump cache), then
 *          remapped with TLBI VAE1IS and TLBI VMALLE1IS while kept aside:
 *          the new code must run (TLBs and jump caches, kept aside or not)
 *   73     a probe that the contexts are really in use: a PTE changed
 *          without TLBI while its ASID is kept aside. With contexts the old
 *          translation comes back (an ASID-tagged TLB may legitimately do
 *          that); built with -DNO_ASID_TLBS (run with asid-tlbs=0) the switch
 *          dropped it and the new one is seen. Built without the define and
 *          run with asid-tlbs=0, the probe FAILs (value mismatch): the
 *          negative control showing the other checks really ran with
 *          contexts active.
 *
 * Bare-metal AArch64 on the `a13-test` machine, EL1, 16 KiB granule.
 * Architectural encodings only - no Apple code or data.
 */
    .set CTL_BASE,    0x200000000
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set PASS_TOKEN,  0x50415353
    .set FAIL_BASE,   0x46000000

    .set RAM_BASE,    0x800000000
    .set PG,          0x4000                 /* 16 KiB pages */
    .set NA,          12                     /* address spaces (ASIDs 1..12) */
    .set L2_BASE,     RAM_BASE + 0x100000    /* NA level-2 tables */
    .set L3_BASE,     L2_BASE + NA * PG      /* NA level-3 tables, right after */
    .set DATA_BASE,   RAM_BASE + 0x200000    /* 16 data pages */
    .set CODE_PG,     RAM_BASE + 0x300000    /* page that becomes code */
    .set TVA,         0xA00000000            /* the test VA: L2 index 0x500 */
    .set WVA,         TVA + PG               /* nG writable alias of CODE_PG (ASID 5 only) */
    .set XVA,         TVA + 2 * PG           /* nG code: CODE_A in ASID 9, CODE_B in ASID 10 */
    .set CODE_A,      CODE_PG + PG           /* "movz x0, #0xa1; ret" */
    .set CODE_B,      CODE_PG + 2 * PG       /* "movz x0, #0xb1; ret" */
    .set CODE_C,      CODE_PG + 3 * PG       /* "movz x0, #0xc1; ret" */
    .set MAGIC,       0x5a5a000000000000

    /* descriptors */
    .set BLK_RAM,     0x701                  /* block, AF, inner shareable, Attr0 (normal), global */
    .set BLK_DEV,     0x0060000000000405     /* block, AF, Attr1 (device), UXN|PXN, global */
    .set TBL,         0x3
    .set PAGE_NG,     0xf03                  /* page, AF, inner shareable, Attr0, nG */

    /* TCR: T0SZ=28 (36-bit VA, start at level 2), WBWA, inner shareable, TG0=16K, EPD1, TG1=16K, IPS=36 bits, 8-bit ASID from TTBR0 */
    .set TCR_VAL, 28 | (1 << 8) | (1 << 10) | (3 << 12) | (2 << 14) | (28 << 16) | (1 << 23) | (1 << 30) | (1 << 32)

/* x19 = check number; fail unless \reg == \want (registers) */
    .macro EXPECT_EQ reg, want
        add     x19, x19, #1
        mov     x1, \reg
        cmp     x1, \want
        b.ne    mismatch
    .endm

/* \dst = MAGIC + \idx (data page \idx's contents) */
    .macro MAGIC_OF dst, idx
        ldr     \dst, =MAGIC
        add     \dst, \dst, \idx
    .endm

    .text
    .globl _start
_start:
    mov     x19, #0
    adr     x0, vectors
    msr     vbar_el1, x0

    /* ---- page tables ---- */
    ldr     x0, =L2_BASE
    ldr     x1, =(NA * 2 * PG) / 8           /* L2 and L3 tables are contiguous */
1:  str     xzr, [x0], #8
    subs    x1, x1, #1
    b.ne    1b

    mov     x20, #0                          /* i */
2:  ldr     x0, =L2_BASE
    add     x0, x0, x20, lsl #14             /* L2[i] */
    ldr     x1, =RAM_BASE | BLK_RAM
    str     x1, [x0, #0x400 * 8]
    ldr     x2, =0x2000000
    add     x1, x1, x2
    str     x1, [x0, #0x401 * 8]
    ldr     x1, =CTL_BASE | BLK_DEV
    str     x1, [x0, #0x100 * 8]
    ldr     x1, =L3_BASE
    add     x1, x1, x20, lsl #14             /* L3[i] */
    orr     x2, x1, #TBL
    str     x2, [x0, #0x500 * 8]
    mov     x0, x20
    bl      data_desc                        /* x0 = page descriptor of DATA[i] */
    str     x0, [x1]                         /* L3[i][0]: TVA -> DATA[i] */
    add     x20, x20, #1
    cmp     x20, #NA
    b.ne    2b

    /* XVA: CODE_A in ASID 9's tables, CODE_B in ASID 10's */
    ldr     x1, =L3_BASE + 8 * PG
    ldr     x0, =CODE_A | PAGE_NG
    str     x0, [x1, #16]
    ldr     x1, =L3_BASE + 9 * PG
    ldr     x0, =CODE_B | PAGE_NG
    str     x0, [x1, #16]
    ldr     x0, =CODE_A
    mov     x1, #0xa1
    bl      emit_ret_imm
    ldr     x0, =CODE_B
    mov     x1, #0xb1
    bl      emit_ret_imm
    ldr     x0, =CODE_C
    mov     x1, #0xc1
    bl      emit_ret_imm

    /* writable nG alias of the code page, in ASID 5's tables only */
    ldr     x1, =L3_BASE + 4 * PG
    ldr     x0, =CODE_PG | PAGE_NG
    str     x0, [x1, #8]

    /* data pages: DATA[k] holds MAGIC + k */
    mov     x20, #0
3:  ldr     x0, =DATA_BASE
    add     x0, x0, x20, lsl #14
    MAGIC_OF x1, x20
    str     x1, [x0]
    add     x20, x20, #1
    cmp     x20, #16
    b.ne    3b
    dsb     ish

    /* ---- MMU on, ASID 1 ---- */
    ldr     x0, =0x00ff                      /* Attr0 normal WB, Attr1 device-nGnRnE */
    msr     mair_el1, x0
    ldr     x0, =TCR_VAL
    msr     tcr_el1, x0
    mov     x0, #0
    bl      set_as
    tlbi    vmalle1
    dsb     ish
    isb
    mrs     x0, sctlr_el1
    orr     x0, x0, #(1 << 0)                /* M */
    orr     x0, x0, #(1 << 2)                /* C */
    orr     x0, x0, #(1 << 12)               /* I */
    bic     x0, x0, #(1 << 1)                /* A */
    bic     x0, x0, #(1 << 19)               /* WXN */
    msr     sctlr_el1, x0
    isb

    /* ---- 1. four address spaces round robin, 4 rounds ---- */
    mov     x21, #4
10: mov     x20, #0
11: mov     x0, x20
    bl      check_as                         /* checks 1-16 */
    add     x20, x20, #1
    cmp     x20, #4
    b.ne    11b
    subs    x21, x21, #1
    b.ne    10b

    /* ---- 2. all twelve, 3 rounds (more than the slots) ---- */
    mov     x21, #3
12: mov     x20, #0
13: mov     x0, x20
    bl      check_as                         /* checks 17-52 */
    add     x20, x20, #1
    cmp     x20, #NA
    b.ne    13b
    subs    x21, x21, #1
    b.ne    12b

    /* ---- 3. page invalidations of a kept-aside address space ---- */
    /* TLBI VAE1IS (VA + ASID): AS0 -> DATA[13] */
    mov     x0, #0
    bl      check_as                         /* 53: AS0 cached */
    mov     x0, #1
    bl      check_as                         /* 54: AS0 kept aside */
    mov     x0, #0
    mov     x1, #13
    mov     x2, #0                           /* VAE1IS */
    bl      remap
    mov     x0, #0
    mov     x1, #13
    bl      check_as_is                      /* 55 */
    mov     x0, #1
    bl      check_as                         /* 56 */

    /* TLBI VALE1IS (last level): AS0 -> DATA[14] */
    mov     x0, #0
    mov     x1, #14
    mov     x2, #1
    bl      remap
    mov     x0, #0
    mov     x1, #14
    bl      check_as_is                      /* 57 */

    /* TLBI VAAE1IS (VA, all ASIDs): AS0 -> DATA[15] */
    mov     x0, #1
    bl      check_as                         /* 58 */
    mov     x0, #0
    mov     x1, #15
    mov     x2, #2
    bl      remap
    mov     x0, #0
    mov     x1, #15
    bl      check_as_is                      /* 59 */

    /* TLBI RVAE1IS (range), if FEAT_TLBIRANGE: AS0 -> DATA[0] */
    mov     x0, #1
    bl      check_as                         /* 60 */
    mrs     x0, id_aa64isar0_el1
    ubfx    x0, x0, #56, #4
    cmp     x0, #2
    mov     x2, #3                           /* RVAE1IS */
    mov     x3, #0                           /* VAE1IS */
    csel    x2, x2, x3, eq
    mov     x0, #0
    mov     x1, #0
    bl      remap
    mov     x0, #0
    bl      check_as                         /* 61 */

    /* ---- 4. whole-TLB invalidations of a kept-aside address space ---- */
    /* TLBI VMALLE1IS: AS2 -> DATA[12] */
    mov     x0, #2
    bl      check_as                         /* 62 */
    mov     x0, #3
    bl      check_as                         /* 63 */
    mov     x0, #2
    mov     x1, #12
    mov     x2, #4
    bl      remap
    mov     x0, #2
    mov     x1, #12
    bl      check_as_is                      /* 64 */
    /* TLBI ASIDE1IS: AS2 -> DATA[2] */
    mov     x0, #3
    bl      check_as                         /* 65 */
    mov     x0, #2
    mov     x1, #2
    mov     x2, #5
    bl      remap
    mov     x0, #2
    bl      check_as                         /* 66 */

    /* ---- 5. self-modifying code through a kept-aside alias ---- */
    mov     x22, #1
20: mov     x0, #4                           /* ASID 5: the writer, WVA -> CODE_PG */
    bl      set_as
    ldr     x0, =WVA
    mov     x1, x22
    bl      emit_ret_imm
    mov     x0, #5                           /* ASID 6: runs CODE_PG through its global mapping */
    bl      set_as
    ldr     x0, =CODE_PG
    blr     x0
    EXPECT_EQ x0, x22                        /* 67-70 */
    add     x22, x22, #1
    cmp     x22, #5
    b.ne    20b

    /*
     * ---- 6. probe: are the contexts in use? ----
     * A PTE of AS6 changes without TLBI while AS6 is kept aside. A switch
     * may also grow (and so flush) a TLB that is filling up, so try a few
     * times: with contexts at least one try must see the old translation,
     * without them none may.
     */
    mov     x19, #70
    mov     x23, #8                          /* tries */
    mov     x24, #0                          /* tries that saw the old page */
40: mov     x0, #6
    mov     x1, #6
    mov     x2, #4                           /* AS6 -> DATA[6], TLBI VMALLE1IS */
    bl      remap
    mov     x0, #6
    bl      set_as
    ldr     x0, =TVA
    ldr     x0, [x0]                         /* fill AS6's entry */
    mov     x0, #7
    bl      set_as                           /* keep AS6 aside */
    mov     x0, #6
    mov     x1, #12
    mov     x2, #6                           /* AS6 -> DATA[12], no TLBI */
    bl      remap
    mov     x0, #6
    bl      set_as
    ldr     x0, =TVA
    ldr     x2, [x0]
    MAGIC_OF x3, #6
    cmp     x2, x3
    cinc    x24, x24, eq
    b.eq    41f
    MAGIC_OF x3, #12
    EXPECT_EQ x2, x3                         /* 71: neither page: wrong */
    sub     x19, x19, #1
41: subs    x23, x23, #1
    b.ne    40b
    mov     x19, #72
    cmp     x24, #0
    cset    x2, ne
#ifdef NO_ASID_TLBS
    mov     x3, #0                           /* the switches dropped AS6's entries */
#else
    mov     x3, #1                           /* AS6's entry was kept and restored */
#endif
    EXPECT_EQ x2, x3                         /* 73 */
    tlbi    vmalle1is
    dsb     ish
    isb
    mov     x0, #6
    mov     x1, #12
    bl      check_as_is                      /* 74 */

    /* ---- 7. one code VA, two address spaces, through the jump cache ---- */
    mov     x22, #4
30: mov     x0, #8
    mov     x1, #0xa1
    bl      call_xva                         /* 75, 77, 79, 81 */
    mov     x0, #9
    mov     x1, #0xb1
    bl      call_xva                         /* 76, 78, 80, 82 */
    subs    x22, x22, #1
    b.ne    30b
    /* AS8's XVA -> CODE_C with TLBI VAE1IS while AS9 is current */
    ldr     x0, =CODE_C
    mov     x1, #0
    bl      remap_xva8
    mov     x0, #8
    mov     x1, #0xc1
    bl      call_xva                         /* 83 */
    mov     x0, #9
    mov     x1, #0xb1
    bl      call_xva                         /* 84 */
    /* back to CODE_A with TLBI VMALLE1IS */
    ldr     x0, =CODE_A
    mov     x1, #1
    bl      remap_xva8
    mov     x0, #8
    mov     x1, #0xa1
    bl      call_xva                         /* 85 */

    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    dsb     sy
4:  wfi
    b       4b

/* x0 = page descriptor (nG) of DATA[x0]; clobbers x2 */
data_desc:
    ldr     x2, =DATA_BASE
    add     x0, x2, x0, lsl #14
    ldr     x2, =PAGE_NG
    orr     x0, x0, x2
    ret

/* switch to address space x0 (ASID x0 + 1); clobbers x0-x2 */
set_as:
    ldr     x1, =L2_BASE
    add     x1, x1, x0, lsl #14
    add     x2, x0, #1
    orr     x1, x1, x2, lsl #48
    msr     ttbr0_el1, x1
    isb
    ret

/* switch to address space x0 and check TVA reads MAGIC + x0 */
check_as:
    mov     x1, x0
/* switch to address space x0 and check TVA reads MAGIC + x1 */
check_as_is:
    mov     x9, x30
    mov     x10, x1
    bl      set_as
    ldr     x0, =TVA
    ldr     x2, [x0]
    MAGIC_OF x3, x10
    EXPECT_EQ x2, x3
    mov     x30, x9
    ret

/*
 * Point address space x0's TVA at DATA[x1], break-before-make, invalidating
 * with method x2: 0 TLBI VAE1IS, 1 VALE1IS, 2 VAAE1IS, 3 RVAE1IS, 4 VMALLE1IS,
 * 5 ASIDE1IS, 6 none (the PTE changes without an invalidation).
 * Clobbers x0-x8.
 */
remap:
    mov     x8, x30
    ldr     x4, =L3_BASE
    add     x4, x4, x0, lsl #14              /* &L3[x0][0] */
    add     x5, x0, #1                       /* ASID */
    ldr     x6, =TVA
    mov     x7, x2
    mov     x0, x1
    bl      data_desc
    mov     x1, x0                           /* new descriptor */
    cmp     x7, #6
    b.eq    6f
    str     xzr, [x4]                        /* break */
    dsb     ishst
    lsr     x2, x6, #12                      /* VA[55:12] for the by-VA forms */
    orr     x3, x2, x5, lsl #48
    cmp     x7, #0
    b.ne    1f
    tlbi    vae1is, x3
    b       5f
1:  cmp     x7, #1
    b.ne    2f
    tlbi    vale1is, x3
    b       5f
2:  cmp     x7, #2
    b.ne    3f
    tlbi    vaae1is, x2
    b       5f
3:  cmp     x7, #3
    b.ne    4f
    /* RVAE1IS: ASID, TG=16K (0b10), SCALE=0, NUM=0 (2 pages), BaseADDR = VA >> 14 */
    lsr     x2, x6, #14
    orr     x2, x2, x5, lsl #48
    mov     x3, #2
    orr     x2, x2, x3, lsl #46
    sys     #0, c8, c2, #1, x2
    b       5f
4:  cmp     x7, #4
    b.ne    7f
    tlbi    vmalle1is
    b       5f
7:  lsl     x2, x5, #48
    tlbi    aside1is, x2
5:  dsb     ish
6:  str     x1, [x4]                         /* make */
    dsb     ish
    isb
    mov     x30, x8
    ret

/* switch to address space x0, BLR to XVA, check it returned x1 */
call_xva:
    mov     x11, x30
    mov     x12, x1
    bl      set_as
    ldr     x0, =XVA
    blr     x0
    EXPECT_EQ x0, x12
    mov     x30, x11
    ret

/* point AS8's XVA at the code page x0 (current: AS9), x1 = 0 TLBI VAE1IS, 1 TLBI VMALLE1IS */
remap_xva8:
    ldr     x4, =L3_BASE + 8 * PG + 16
    ldr     x2, =PAGE_NG
    orr     x5, x0, x2
    str     xzr, [x4]
    dsb     ishst
    cbnz    x1, 1f
    ldr     x2, =XVA
    lsr     x2, x2, #12
    mov     x3, #9                           /* ASID of AS8 */
    orr     x2, x2, x3, lsl #48
    tlbi    vae1is, x2
    b       2f
1:  tlbi    vmalle1is
2:  dsb     ish
    str     x5, [x4]
    dsb     ish
    isb
    ret

/* write "movz x0, #x1; ret" at VA x0 and make it visible to instruction fetch */
emit_ret_imm:
    ldr     w2, =0xd2800000                  /* movz x0, #0 */
    orr     w2, w2, w1, lsl #5
    str     w2, [x0]
    ldr     w2, =0xd65f03c0                  /* ret */
    str     w2, [x0, #4]
    dc      cvau, x0
    dsb     ish
    ic      ivau, x0
    dsb     ish
    isb
    ret

mismatch:
    mov     x3, x1
    mov     x2, #0xff
    b       report_fail

exc_fail:
    mrs     x3, esr_el1

report_fail:
    ldr     x1, =CTL_BASE
    str     x3, [x1, #CTL_DETAIL]
    lsl     x0, x19, #8
    orr     x0, x0, x2
    ldr     x4, =FAIL_BASE
    orr     x0, x0, x4
    str     x0, [x1, #CTL_STATUS]
    dsb     sy
8:  wfi
    b       8b

    .ltorg

    .balign 0x800
vectors:
    .irp idx, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        mov     x2, #\idx
        b       exc_fail
        .balign 0x80
    .endr
