/*
 * gxf_sptm_regs_test.S - self-contained check for the iOS 27 SPTM/TXM
 * register additions in hw/arm/a13_gxf.c.
 *
 * Bare-metal AArch64, entered at EL1 on a core with Apple GXF (the t8030
 * AppleA13 CPU, which calls apple_a13_init_gxf()). It:
 *   1. installs a VBAR_EL1 vector whose synchronous handler writes a FAIL
 *      token - so any UNDEF on a missing register fails the test closed;
 *   2. MSR/MRS round-trips every register added by this change, at EL1;
 *   3. enables GXF, GENTERs into GL1, repeats the round-trip of the
 *      guarded-reachable registers from guarded mode, and GEXITs;
 *   4. writes a PASS token and parks.
 *
 * PASS/FAIL are written to RESULT_ADDR (a RAM address the harness reads).
 * The test uses only architectural + Apple-GXF encodings; it needs no
 * firmware, only a machine that wires the AppleA13 GXF register block.
 *
 * Assemble (NDK clang):
 *   llvm-mc -triple=aarch64 -filetype=obj gxf_sptm_regs_test.S -o t.o
 *   ld.lld -Ttext=0x0 --image-base=0x0 t.o -o t.elf
 *   llvm-objcopy -O binary t.elf gxf_sptm_regs_test.bin
 *
 * This file is a test input; it is not compiled into QEMU.
 */

    .set RESULT_ADDR, 0x40000000      /* harness-readable scratch RAM */
    .set PASS_TOKEN,  0x53415000      /* "PAS\0" */
    .set FAIL_TOKEN,  0x4641494c      /* "FAIL"  */

    .macro CHECK_RW name, op1, crn, crm, op2, pat
        /* write pattern, read back, compare; branch to fail on mismatch */
        ldr     x0, =\pat
        msr     s3_\op1\()_c\crn\()_c\crm\()_\op2, x0
        isb
        mrs     x1, s3_\op1\()_c\crn\()_c\crm\()_\op2
        cmp     x0, x1
        b.ne    fail
    .endm

    .text
    .globl _start
_start:
    /* install exception vectors so UNDEF -> fail */
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* --- EL1, non-guarded: exercise every new register --- */
    CHECK_RW s6c0_0, 6, 15, 0, 0, 0x0123456789abcdef
    CHECK_RW s6c2_0, 6, 15, 2, 0, 0x1122334455667788
    CHECK_RW s6c8_7, 6, 15, 8, 7, 0x8877665544332211

    CHECK_RW e2_00, 4, 15, 0, 0, 0xa5a5a5a5a5a5a5a5
    CHECK_RW e2_01, 4, 15, 0, 1, 0x5a5a5a5a5a5a5a5a
    CHECK_RW e2_02, 4, 15, 0, 2, 0x00000000ffffffff
    CHECK_RW e2_03, 4, 15, 0, 3, 0xffffffff00000000
    CHECK_RW e2_04, 4, 15, 0, 4, 0xdeadbeefcafebabe
    CHECK_RW e2_05, 4, 15, 0, 5, 0x0f0f0f0f0f0f0f0f
    CHECK_RW e2_10, 4, 15, 1, 0, 0x1111111122222222
    CHECK_RW e2_11, 4, 15, 1, 1, 0x3333333344444444
    CHECK_RW e2_12, 4, 15, 1, 2, 0x5555555566666666
    CHECK_RW e2_13, 4, 15, 1, 3, 0x7777777788888888
    CHECK_RW e2_14, 4, 15, 1, 4, 0x9999999900000000
    CHECK_RW e2_22, 4, 15, 2, 2, 0xaaaaaaaabbbbbbbb
    CHECK_RW e2_23, 4, 15, 2, 3, 0xccccccccdddddddd
    CHECK_RW e2_24, 4, 15, 2, 4, 0xeeeeeeeeffffffff
    CHECK_RW e2_25, 4, 15, 2, 5, 0x0123deadbeef4567

    /* --- enable GXF and enter GL1 --- */
    mov     x0, #1
    msr     s3_6_c15_c1_2, x0          /* GXF_CONFIG_EL1 = 1 */
    isb
    adr     x0, gl1_entry
    msr     s3_6_c15_c8_1, x0          /* GXF_ENTER_EL1 = gl1_entry */
    isb
    .inst   0x00201420                 /* GENTER -> lands at gl1_entry, GL1 */

guarded_done:
    /* back from guarded mode (GEXIT target) */
    ldr     x0, =PASS_TOKEN
    ldr     x1, =RESULT_ADDR
    str     w0, [x1]
    dsb     sy
1:  wfi
    b       1b

gl1_entry:
    /* now at GL1: re-check the GXF-space regs from guarded mode */
    CHECK_RW g6c0_0, 6, 15, 0, 0, 0xfeedfacefeedface
    CHECK_RW g6c8_7, 6, 15, 8, 7, 0x1357246813572468
    /* set GEXIT return point and leave guarded mode */
    adr     x0, guarded_done
    msr     s3_6_c15_c9_6, x0          /* ELR_GL11 = guarded_done */
    isb
    .inst   0x00201400                 /* GEXIT -> guarded_done */

fail:
    ldr     x0, =FAIL_TOKEN
    ldr     x1, =RESULT_ADDR
    str     w0, [x1]
    dsb     sy
2:  wfi
    b       2b

    /* minimal EL1 vector table: every entry -> fail */
    .balign 0x800
vectors:
    .rept 16
        b   fail
        .balign 0x80
    .endr
