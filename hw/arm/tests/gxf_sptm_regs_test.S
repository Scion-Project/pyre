/*
 * gxf_sptm_regs_test.S - self-contained check for the iOS 27 SPTM/TXM
 * register additions in hw/arm/a13_gxf.c (milestone M0 in ios-on-pixel
 * docs/ios27/GXF-GAP.md).
 *
 * Bare-metal AArch64, entered at EL1 with the MMU off on the `a13-test`
 * machine (one AppleA13 core, the CPU type t8030 uses). It:
 *   1. installs EL1 and guarded (VBAR_GL11, GXF_ABORT_EL1) vectors whose
 *      every entry reports FAIL with the ESR, so any UNDEF - a register the
 *      model does not define - fails the test closed;
 *   2. MSR/MRS round-trips every register added for SPTM, at EL1;
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

    /* --- EL1, non-guarded: every register added for SPTM --- */
    CHECK_RW 6, 15, 0, 0, 0x0123456789abcdef
    CHECK_RW 6, 15, 2, 0, 0x1122334455667788
    CHECK_RW 6, 15, 8, 7, 0x8877665544332211

    CHECK_RW 4, 15, 0, 0, 0xa5a5a5a5a5a5a5a5
    CHECK_RW 4, 15, 0, 1, 0x5a5a5a5a5a5a5a5a
    CHECK_RW 4, 15, 0, 2, 0x00000000ffffffff
    CHECK_RW 4, 15, 0, 3, 0xffffffff00000000
    CHECK_RW 4, 15, 0, 4, 0xdeadbeefcafebabe
    CHECK_RW 4, 15, 0, 5, 0x0f0f0f0f0f0f0f0f
    CHECK_RW 4, 15, 1, 0, 0x1111111122222222
    CHECK_RW 4, 15, 1, 1, 0x3333333344444444
    CHECK_RW 4, 15, 1, 2, 0x5555555566666666
    CHECK_RW 4, 15, 1, 3, 0x7777777788888888
    CHECK_RW 4, 15, 1, 4, 0x9999999900000000
    CHECK_RW 4, 15, 2, 2, 0xaaaaaaaabbbbbbbb
    CHECK_RW 4, 15, 2, 3, 0xccccccccdddddddd
    CHECK_RW 4, 15, 2, 4, 0xeeeeeeeeffffffff
    CHECK_RW 4, 15, 2, 5, 0x0123deadbeef4567

#ifdef NEGATIVE_CONTROL
    CHECK_RW 6, 15, 14, 7, 0x1                 /* undefined: must UNDEF */
#endif

    CHECK_GUARDED 0

    /* --- SPTM-style bootstrap: enable GXF, install the GL1 entry, GENTER --- */
    adr     x0, vectors
    msr     s3_6_c15_c9_2, x0          /* VBAR_GL11 */
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
    CHECK_GUARDED 1
    /* the GXF-space registers again, from guarded mode */
    CHECK_RW 6, 15, 0, 0, 0xfeedfacefeedface
    CHECK_RW 6, 15, 2, 0, 0x2468135724681357
    CHECK_RW 6, 15, 8, 7, 0x1357246813572468
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
