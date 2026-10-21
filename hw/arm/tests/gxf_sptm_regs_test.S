/*
 * gxf_sptm_regs_test.S - self-contained check for the iOS 27 SPTM/TXM
 * register additions in hw/arm/a13_gxf.c (milestone M0 in ios-on-pixel
 * docs/ios27/GXF-GAP.md).
 *
 * Bare-metal AArch64, entered at EL1 with the MMU off on the `a13-test`
 * machine (one AppleA13 core, the CPU type t8030 uses). It:
 *   1. installs EL1 and guarded (GXF_ABORT_EL1, and VBAR_GL11 once in GL1)
 *      vectors whose every entry reports FAIL with the ESR, so any UNDEF - a
 *      register the model does not define - fails the test closed;
 *   2. at EL1, MSR/MRS round-trips every placeholder register added for
 *      SPTM, and checks that the encodings SPTM uses which already have a
 *      real model (PAC keys, APCTL, VMSA lock, AMX, CTRR, error status) are
 *      accessible, writing back the value read so their state is unchanged;
 *   3. reproduces SPTM's bootstrap prologue in miniature: GXF_CONFIG_EL1 = 1,
 *      GXF_ENTER_EL1 = handler, GENTER; checks GXF_STATUS_EL1 says guarded,
 *      round-trips the GXF-space registers from GL1, then GEXITs and checks
 *      GXF_STATUS_EL1 says not guarded;
 *   4. reports PASS.
 *
 * Results go to the a13-test control block (hw/arm/a13-test.c), which turns
 * them into QEMU's exit status: 0 = PASS, 1 = FAIL. A hang (e.g. a fault
 * loop) is caught by the runner's timeout. FAIL status words are
 * 0x46000000 | (check << 8) | cause, where cause 0xff = value mismatch and
 * 0x00-0x0f = exception vector index; DETAIL holds the ESR (or the value
 * read back).
 *
 * Build with -DNEGATIVE_CONTROL to add an access to an encoding nothing
 * defines (S3_6_C15_C14_7); that build must FAIL, which shows the harness
 * really detects a missing register.
 *
 * Run: hw/arm/tests/run-a13-tests.sh <path to qemu-system-aarch64>
 *
 * This file is a test input; it is not compiled into QEMU. It uses only
 * architectural and Apple GXF encodings - no Apple code or data.
 */

    .set CTL_BASE,    0x200000000     /* a13-test control block */
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set PASS_TOKEN,  0x50415353      /* "PASS" */
    .set FAIL_BASE,   0x46000000

/* x19 = number of the check being run (reported on failure) */
    .macro CHECK_RW op1, crn, crm, op2, pat
        add     x19, x19, #1
        ldr     x0, =\pat
        msr     s3_\op1\()_c\crn\()_c\crm\()_\op2, x0
        isb
        mrs     x1, s3_\op1\()_c\crn\()_c\crm\()_\op2
        cmp     x0, x1
        b.ne    mismatch
    .endm

/* read, then write the same value back: the register must exist and be
 * writable, without the test changing its state */
    .macro CHECK_WB op1, crn, crm, op2
        add     x19, x19, #1
        mrs     x0, s3_\op1\()_c\crn\()_c\crm\()_\op2
        msr     s3_\op1\()_c\crn\()_c\crm\()_\op2, x0
        isb
    .endm

/* read only: the register must exist */
    .macro CHECK_RD op1, crn, crm, op2
        add     x19, x19, #1
        mrs     x0, s3_\op1\()_c\crn\()_c\crm\()_\op2
    .endm

/* expect (GXF_STATUS_EL1 & 1) == \want */
    .macro CHECK_GUARDED want
        add     x19, x19, #1
        mrs     x1, s3_6_c15_c8_0          /* GXF_STATUS_EL1 */
        and     x1, x1, #1
        cmp     x1, #\want
        b.ne    mismatch
    .endm

    .text
    .globl _start
_start:
    mov     x19, #0
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* --- EL1, non-guarded: every encoding SPTM / the iOS 27 kernel use --- */
    /* placeholders (storage only, added for SPTM): full round trip */
    CHECK_RW 6, 15, 0, 0, 0x0123456789abcdef
    CHECK_RW 6, 15, 8, 7, 0x8877665544332211
    CHECK_RW 4, 15, 0, 0, 0xa5a5a5a5a5a5a5a5
    CHECK_RW 4, 15, 0, 1, 0x5a5a5a5a5a5a5a5a
    CHECK_RW 4, 15, 0, 3, 0xffffffff00000000
    /* already modelled elsewhere: must be accessible, state left as is */
    CHECK_WB 6, 15, 2, 0                       /* ARM64_REG_MMU_ERR_STS */
    CHECK_WB 4, 15, 0, 2                       /* ARM64_REG_FED_ERR_STS */
    CHECK_WB 4, 15, 0, 4                       /* APCTL_EL1 */
    CHECK_WB 4, 15, 0, 5                       /* A13 S3_4_c15_c0_5 */
    CHECK_WB 4, 15, 1, 0                       /* KERNELKEYLO_EL1 */
    CHECK_WB 4, 15, 1, 1                       /* KERNELKEYHI_EL1 */
    CHECK_WB 4, 15, 1, 2                       /* VMSA_LOCK_EL1 */
    CHECK_RD 4, 15, 1, 3                       /* AMX_STATUS_EL1 (read-only) */
    CHECK_WB 4, 15, 1, 4                       /* AMX_CTL_EL1 */
    CHECK_WB 4, 15, 2, 2                       /* CTRR_LOCK_EL1 (per cluster) */
    CHECK_WB 4, 15, 2, 3                       /* CTRR_A_LWR_EL1 (per cluster) */
    CHECK_WB 4, 15, 2, 4                       /* CTRR_A_UPR_EL1 (per cluster) */
    CHECK_WB 4, 15, 2, 5                       /* CTRR_CTL_EL1 (per cluster) */

#ifdef NEGATIVE_CONTROL
    CHECK_RW 6, 15, 14, 7, 0x1                 /* undefined: must UNDEF */
#endif

    CHECK_GUARDED 0

    /* --- SPTM-style bootstrap: enable GXF, install the GL1 entry, GENTER --- */
    adr     x0, guarded_abort
    msr     s3_6_c15_c8_2, x0          /* GXF_ABORT_EL1 */
    mov     x0, #1
    msr     s3_6_c15_c1_2, x0          /* GXF_CONFIG_EL1 = 1 */
    isb
    adr     x0, gl1_entry
    msr     s3_6_c15_c8_1, x0          /* GXF_ENTER_EL1 = gl1_entry */
    isb
    .inst   0x00201420                 /* GENTER -> gl1_entry, guarded */
    b       not_entered                /* GENTER must not fall through */

guarded_done:
    /* back at non-guarded EL1 (GEXIT returns to ELR_GL11) */
    CHECK_GUARDED 0
    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    dsb     sy
1:  wfi
    b       1b

gl1_entry:
    /* VBAR_GL11 is only accessible while guarded (as in SPTM, which sets it
     * right after its first GENTER); a fault before this line hangs and is
     * caught by the runner's timeout */
    adr     x0, vectors
    msr     s3_6_c15_c9_2, x0          /* VBAR_GL11 */
    isb
    CHECK_GUARDED 1
    /* the GXF-space registers again, from guarded mode */
    CHECK_RW 6, 15, 0, 0, 0xfeedfacefeedface
    CHECK_RW 6, 15, 8, 7, 0x1357246813572468
    CHECK_WB 6, 15, 2, 0
    adr     x0, guarded_done
    msr     s3_6_c15_c9_6, x0          /* ELR_GL11 = guarded_done */
    isb
    .inst   0x00201400                 /* GEXIT -> guarded_done */
    b       not_entered                /* GEXIT must not fall through */

/* value mismatch: DETAIL = value read back */
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

/* exception: x2 = vector index, DETAIL = ESR (banked when guarded) */
exc_fail:
    mrs     x3, esr_el1

/* status = FAIL_BASE | (check << 8) | cause(x2) */
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

    /* EL1 / GL1 vector table: every entry reports its index */
    .balign 0x800
vectors:
    .irp idx, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        mov     x2, #\idx
        b       exc_fail
        .balign 0x80
    .endr
