/*
 * gxf_banked_el1_test.S - the GXF-banked EL1 registers (TPIDR_EL1,
 * ELR_EL1, SPSR_EL1, ESR_EL1, FAR_EL1) must address the GL1 copy while
 * guarded and the EL1 copy otherwise.
 *
 * The A64 translator picks the copy when it translates the MRS/MSR (from
 * the GXF bit of the TB's MMU index), so the same instruction has to give
 * a different answer depending on the state it runs in. This test runs one
 * shared subroutine, at one address, from both states:
 *
 *   1. at EL1: write a distinct pattern to each register through the shared
 *      writer, read it back through the shared reader;
 *   2. GENTER; in GL1: check the *_GL11 names still see the EL1 patterns,
 *      write GL1 patterns through the same writer and read them back
 *      through the same reader (ELR/SPSR are restored before GEXIT, which
 *      returns through them);
 *   3. GEXIT; at EL1: the reader must return the EL1 patterns again.
 *
 * Bare-metal AArch64, `a13-test` machine, EL1, MMU off. Uses only
 * architectural and Apple GXF encodings - no Apple code or data.
 * Build with -DNEGATIVE_CONTROL to make step 3 expect the GL1 pattern for
 * TPIDR_EL1, which must FAIL (value mismatch at check 31): it shows a
 * missing bank switch would be detected.
 */

    .set CTL_BASE,    0x200000000
    .set CTL_DETAIL,  0x00
    .set CTL_STATUS,  0x08
    .set PASS_TOKEN,  0x50415353
    .set FAIL_BASE,   0x46000000

/* x19 = check number; compare x1 against the 64-bit constant \val */
    .macro EXPECT reg, val
        add     x19, x19, #1
        mov     x1, \reg
        ldr     x0, =\val
        cmp     x0, x1
        b.ne    mismatch
    .endm

/* EL1 patterns (SPSR: EL1h with DAIF masked, a valid SPSR) */
    .set E_TPIDR, 0x1111000011110001
    .set E_ELR,   0x1111000011110002
    .set E_SPSR,  0x3c5
    .set E_ESR,   0x0000000096000045
    .set E_FAR,   0x1111000011110005
/* GL1 patterns */
    .set G_TPIDR, 0x2222000022220001
    .set G_ELR,   0x2222000022220002
    .set G_SPSR,  0x3c4
    .set G_ESR,   0x0000000092000046
    .set G_FAR,   0x2222000022220005

    .text
    .globl _start
_start:
    mov     x19, #0
    adr     x0, vectors
    msr     vbar_el1, x0
    isb

    /* --- 1. EL1 --- */
    ldr     x10, =E_TPIDR
    ldr     x11, =E_ELR
    ldr     x12, =E_SPSR
    ldr     x13, =E_ESR
    ldr     x14, =E_FAR
    bl      write_all
    bl      read_all
    EXPECT  x10, E_TPIDR                   /* 1 */
    EXPECT  x11, E_ELR
    EXPECT  x12, E_SPSR
    EXPECT  x13, E_ESR
    EXPECT  x14, E_FAR                     /* 5 */

    /* --- 2. GENTER into GL1 --- */
    adr     x0, guarded_abort
    msr     s3_6_c15_c8_2, x0              /* GXF_ABORT_EL1 */
    mov     x0, #1
    msr     s3_6_c15_c1_2, x0              /* GXF_CONFIG_EL1 = 1 */
    isb
    adr     x0, gl1_entry
    msr     s3_6_c15_c8_1, x0              /* GXF_ENTER_EL1 */
    isb
    mov     x20, #0
    .inst   0x00201420                     /* GENTER */

    /* --- 3. back at EL1 after GEXIT --- */
    cmp     x20, #1
    b.ne    not_entered
    mov     x19, #30
    bl      read_all
#ifdef NEGATIVE_CONTROL
    EXPECT  x10, G_TPIDR                   /* 31: must fail */
#else
    EXPECT  x10, E_TPIDR                   /* 31 */
#endif
    EXPECT  x11, E_ELR
    EXPECT  x12, E_SPSR
    EXPECT  x13, E_ESR
    EXPECT  x14, E_FAR                     /* 35 */

    ldr     x1, =CTL_BASE
    ldr     x0, =PASS_TOKEN
    str     x0, [x1, #CTL_STATUS]
    dsb     sy
1:  wfi
    b       1b

gl1_entry:
    adr     x0, vectors
    msr     vbar_el1, x0                   /* VBAR_GL1 */
    isb
    mov     x19, #10
    /* the GL11 names reach the EL1 copies */
    mrs     x2, s3_6_c15_c9_1              /* TPIDR_GL11 */
    EXPECT  x2, E_TPIDR                    /* 11 */
    mrs     x2, s3_6_c15_c9_6              /* ELR_GL11 */
    EXPECT  x2, E_ELR
    mrs     x2, s3_6_c15_c9_3              /* SPSR_GL11 */
    EXPECT  x2, E_SPSR
    mrs     x2, s3_6_c15_c9_5              /* ESR_GL11 */
    EXPECT  x2, E_ESR
    mrs     x2, s3_6_c15_c9_7              /* FAR_GL11 */
    EXPECT  x2, E_FAR                      /* 15 */

    /* GEXIT returns through ELR_GL1/SPSR_GL1: keep them */
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    ldr     x10, =G_TPIDR
    ldr     x11, =G_ELR
    ldr     x12, =G_SPSR
    ldr     x13, =G_ESR
    ldr     x14, =G_FAR
    bl      write_all
    bl      read_all
    EXPECT  x10, G_TPIDR                   /* 16 */
    EXPECT  x11, G_ELR
    EXPECT  x12, G_SPSR
    EXPECT  x13, G_ESR
    EXPECT  x14, G_FAR                     /* 20 */
    /* and the EL1 copies are untouched */
    mrs     x2, s3_6_c15_c9_1
    EXPECT  x2, E_TPIDR                    /* 21 */
    mrs     x2, s3_6_c15_c9_6
    EXPECT  x2, E_ELR
    mrs     x2, s3_6_c15_c9_3
    EXPECT  x2, E_SPSR
    mrs     x2, s3_6_c15_c9_5
    EXPECT  x2, E_ESR
    mrs     x2, s3_6_c15_c9_7
    EXPECT  x2, E_FAR                      /* 25 */

    msr     elr_el1, x21
    msr     spsr_el1, x22
    isb
    mov     x20, #1
    .inst   0x00201400                     /* GEXIT */
    b       not_entered

/* shared by both states: one address, so one MRS/MSR per register */
write_all:
    msr     tpidr_el1, x10
    msr     elr_el1, x11
    msr     spsr_el1, x12
    msr     esr_el1, x13
    msr     far_el1, x14
    isb
    ret

read_all:
    mrs     x10, tpidr_el1
    mrs     x11, elr_el1
    mrs     x12, spsr_el1
    mrs     x13, esr_el1
    mrs     x14, far_el1
    ret

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
