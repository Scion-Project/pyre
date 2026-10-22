/*
 * a14_cpu_test.S - checks the Apple A14 (Firestorm/Icestorm) core model
 * (hw/arm/a14.c, apple_a14_init_gxf() in hw/arm/a13_gxf.c) on the
 * `a13-test` machine with core=firestorm or core=icestorm.
 *
 * Bare-metal AArch64 at EL1, MMU off. It:
 *   1. checks MIDR_EL1: Apple, part 0x21 (Firestorm) or, with -DICESTORM,
 *      0x20 (Icestorm);
 *   2. checks the ID registers: BTI, MTE, SVE and SME absent, and the v8.5
 *      features LLVM's apple-a14 lists present (FlagM2, FRINTTS, SB, SSBS,
 *      DPB2);
 *   3. round-trips the A14-only implementation-defined registers the
 *      iPhone13,x kernel uses (HID/EHID space and the rest of a14.c's list);
 *   4. reads the counter aliases ACNTVCT_EL0/ACNTPCT_EL0 (S3_4_C15_C10_6/5)
 *      and checks they track CNTVCT_EL0/CNTPCT_EL0;
 *   5. round-trips the re-encoded SPRR registers (SPRR_PERM_EL1 at
 *      S3_6_C15_C1_6, the EL0 mask at S3_6_C15_C3_0) and ASPSR (C8_3) at
 *      non-guarded EL1, then GENTERs and checks the GL1 registers at
 *      S3_6_C15_C10_<n> are separate from the EL1 ones (TPIDR, VBAR,
 *      ASPSR), that the GL1 vectors come from VBAR_GL1, and that GEXIT
 *      returns through ELR_GL1/SPSR_GL1 as GENTER left them;
 *   6. reports PASS.
 * Any UNDEF lands in the fail vectors. FAIL status words are
 * 0x46000000 | (check << 8) | cause, as in gxf_sptm_regs_test.S.
 *
 * -DNEGATIVE_CONTROL adds an encoding nothing defines on the A14 either
 * (S3_6_C15_C13_7); that build must FAIL with an UNDEF.
 *
 * This file is a test input; it is not compiled into QEMU. It uses only
 * architectural and Apple implementation-defined encodings.
 */

    .set CTL_BASE,    0x200000000
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set PASS_TOKEN,  0x50415353
    .set FAIL_BASE,   0x46000000

    .macro CHECK_RW op1, crn, crm, op2, pat
        add     x19, x19, #1
        ldr     x0, =\pat
        msr     s3_\op1\()_c\crn\()_c\crm\()_\op2, x0
        isb
        mrs     x1, s3_\op1\()_c\crn\()_c\crm\()_\op2
        cmp     x0, x1
        b.ne    mismatch
    .endm

/* x1 = field (\reg >> \shift) & 0xf; must equal \want */
    .macro CHECK_FIELD reg, shift, want
        add     x19, x19, #1
        mrs     x1, \reg
        ubfx    x1, x1, #\shift, #4
        cmp     x1, #\want
        b.ne    mismatch
    .endm

/* field must be at least \want */
    .macro CHECK_FIELD_GE reg, shift, want
        add     x19, x19, #1
        mrs     x1, \reg
        ubfx    x1, x1, #\shift, #4
        cmp     x1, #\want
        b.lo    mismatch
    .endm

    .text
    .globl _start
_start:
    mov     x19, #0
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* 1. MIDR_EL1 */
    add     x19, x19, #1
    mrs     x1, midr_el1
    lsr     x2, x1, #24
    cmp     x2, #0x61                  /* implementer: Apple */
    b.ne    mismatch
    add     x19, x19, #1
    ubfx    x1, x1, #4, #12            /* PartNum */
#ifdef ICESTORM
    cmp     x1, #0x20
#else
    cmp     x1, #0x21
#endif
    b.ne    mismatch

    /* 2. ID registers */
    CHECK_FIELD id_aa64pfr1_el1, 0, 0      /* BT: no BTI */
    CHECK_FIELD id_aa64pfr1_el1, 8, 0      /* MTE: none */
    CHECK_FIELD id_aa64pfr1_el1, 24, 0     /* SME: none */
    CHECK_FIELD id_aa64pfr0_el1, 32, 0     /* SVE: none */
    CHECK_FIELD_GE id_aa64pfr1_el1, 4, 1   /* SSBS */
    CHECK_FIELD_GE id_aa64isar0_el1, 52, 2 /* TS: FlagM2 */
    CHECK_FIELD_GE id_aa64isar1_el1, 0, 2  /* DPB: DPB2 */
    CHECK_FIELD_GE id_aa64isar1_el1, 32, 1 /* FRINTTS */
    CHECK_FIELD_GE id_aa64isar1_el1, 36, 1 /* SB */

    /* 3. A14-only implementation-defined registers (storage) */
    CHECK_RW 0, 15, 1, 2, 0x1111000000000001   /* EHID20 */
    CHECK_RW 0, 15, 1, 3, 0x1111000000000002   /* HID21 */
    CHECK_RW 0, 15, 2, 0, 0x1111000000000003   /* HID2 */
    CHECK_RW 0, 15, 6, 0, 0x1111000000000004   /* HID6 */
    CHECK_RW 0, 15, 9, 1, 0x1111000000000005   /* EHID9 */
    CHECK_RW 0, 15, 11, 2, 0x1111000000000006  /* HID18 */
    CHECK_RW 0, 15, 15, 5, 0x1111000000000007  /* HID17 */
    CHECK_RW 1, 15, 0, 3, 0x2222000000000001
    CHECK_RW 4, 15, 4, 7, 0x2222000000000002
    CHECK_RW 4, 15, 9, 5, 0x2222000000000003
    CHECK_RW 5, 15, 1, 3, 0x2222000000000004
    CHECK_RW 6, 15, 0, 2, 0x2222000000000005
    CHECK_RW 6, 15, 2, 5, 0x2222000000000006
    CHECK_RW 6, 15, 11, 1, 0x2222000000000007
    CHECK_RW 6, 15, 14, 6, 0x2222000000000008
    CHECK_RW 6, 15, 15, 4, 0x2222000000000009

#ifdef NEGATIVE_CONTROL
    CHECK_RW 6, 15, 13, 7, 0x1                 /* undefined: must UNDEF */
#endif

    /* APSTS_EL1: PAC master key valid */
    add     x19, x19, #1
    mrs     x1, s3_6_c15_c12_4
    tbz     x1, #0, mismatch

    /* 4. counter aliases: alias read lies between two architectural reads */
    add     x19, x19, #1
    mrs     x4, cntvct_el0
    mrs     x5, s3_4_c15_c10_6                 /* ACNTVCT_EL0 */
    isb
    mrs     x6, cntvct_el0
    mov     x1, x5
    cmp     x5, x4
    b.lo    mismatch
    cmp     x5, x6
    b.hi    mismatch
    add     x19, x19, #1
    mrs     x4, cntpct_el0
    mrs     x5, s3_4_c15_c10_5                 /* ACNTPCT_EL0 */
    isb
    mrs     x6, cntpct_el0
    mov     x1, x5
    cmp     x5, x4
    b.lo    mismatch
    cmp     x5, x6
    b.hi    mismatch

    /* 5. re-encoded SPRR registers and ASPSR, non-guarded. SPRR stays
     * disabled (SPRR_CONFIG_EL1 = 0), so the permission values have no
     * effect on translation here. */
    CHECK_RW 6, 15, 1, 6, 0x2020a5063020f0e0   /* SPRR_PERM_EL1 */
    CHECK_RW 6, 15, 3, 0, 0x00000000ffff0000   /* EL0 permission mask */
    CHECK_RW 6, 15, 8, 3, 0x0000000000000002   /* ASPSR, outside GL */
    ldr     x0, =0x1e1e1e1e1e1e1e1e
    msr     tpidr_el1, x0

    /* GENTER into GL1 */
    adr     x0, guarded_abort
    msr     s3_6_c15_c8_2, x0          /* GXF_ABORT_EL1 */
    mov     x0, #1
    msr     s3_6_c15_c1_2, x0          /* GXF_CONFIG_EL1 = 1 */
    isb
    adr     x0, gl1_entry
    msr     s3_6_c15_c8_1, x0          /* GXF_ENTER_EL1 */
    isb
    mov     x20, #0
    .inst   0x00201420                 /* GENTER */
genter_ret:
    cbz     x20, not_entered
    /* back at EL1: the EL1 copies keep what EL1 code wrote, not what GL1
     * wrote to its own registers */
    add     x19, x19, #1
    mrs     x1, tpidr_el1
    ldr     x0, =0x1e1e1e1e1e1e1e1e
    cmp     x0, x1
    b.ne    mismatch
    add     x19, x19, #1
    mrs     x1, s3_6_c15_c8_3          /* ASPSR, outside GL */
    cmp     x1, #2
    b.ne    mismatch

    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    dsb     sy
1:  wfi
    b       1b

gl1_entry:
    adr     x0, vectors
    msr     s3_6_c15_c10_2, x0         /* VBAR_GL1 */
    isb
    /* GL1 registers (A14 encodings) are their own state */
    CHECK_RW 6, 15, 10, 1, 0x5151515151515151  /* TPIDR_GL1 */
    CHECK_RW 6, 15, 10, 4, 0x0000000000000005  /* ASPSR_GL1 */
    CHECK_RW 6, 15, 10, 7, 0x0000000012345000  /* FAR_GL1 */
    /* the EL1 names still reach the EL1 state */
    add     x19, x19, #1
    mrs     x1, tpidr_el1
    ldr     x0, =0x1e1e1e1e1e1e1e1e
    cmp     x0, x1
    b.ne    mismatch
    /* VBAR_GL1 reads back what was written */
    add     x19, x19, #1
    adr     x0, vectors
    mrs     x1, s3_6_c15_c10_2
    cmp     x0, x1
    b.ne    mismatch
    /* ELR_GL1 holds the GENTER return address (GEXIT uses it) */
    add     x19, x19, #1
    mrs     x1, s3_6_c15_c10_6
    adr     x0, genter_ret
    cmp     x0, x1
    b.ne    mismatch
    mrs     x0, s3_6_c15_c10_5                 /* ESR_GL1: readable */
    mrs     x0, s3_6_c15_c10_3                 /* SPSR_GL1 */
    mov     x20, #1
    .inst   0x00201400                 /* GEXIT */
    b       not_entered

mismatch:
    mov     x3, x1
    mov     x2, #0xff
    b       report_fail

not_entered:
    mov     x3, #0
    mov     x2, #0xfe
    b       report_fail

guarded_abort:
    mov     x2, #0xfd
    b       exc_fail

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
2:  wfi
    b       2b

    .ltorg

    .balign 0x800
vectors:
    .irp idx, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        mov     x2, #\idx
        b       exc_fail
        .balign 0x80
    .endr
