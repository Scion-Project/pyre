/*
 * sep_iommu_ro_test.S - targeted test for IOMMU partial-permission denial
 * (ios-on-pixel#58). Runs as the SEP ROM on t8030 (iOS 14.8 test kit).
 *
 * The SEP core's address 0 is the dart-sep IOMMU (SID 0). This ROM:
 *   1. builds DART page tables in the (unused) SEP firmware carve-out:
 *        IOVA page 0x40 (0x100000) -> PHYS_RO, read-only  (PTE NO_WRITE)
 *        IOVA page 0x41 (0x104000) -> PHYS_RW, read-write
 *        IOVA page 0x42 (0x108000) -> PHYS_WO, write-only (PTE NO_READ)
 *   2. points dart-sep SID 0 at them and enables translation,
 *   3. checks that permitted accesses work and denied ones take a synchronous
 *      abort without touching memory:
 *        c1 read RO ok          c2 write RW ok        c3 write RO aborts
 *        c4 RO page unchanged   c5 write WO ok        c6 read WO aborts
 *        c7 execute from WO aborts
 *   4. prints the results and a final PASS/FAIL on uart0 ("SEPTEST" lines).
 *
 * The iOS kernel programs dart-sep SID 0 for itself around SEP power-on, which
 * can replace these tables mid-run. So the checks run silently, the DART
 * registers are read back afterwards, and the whole run is retried if they
 * changed; only a run with an undisturbed DART is reported.
 *
 * Addresses come from the 14.8 n104ap device tree and the kit's boot log
 * (SEPFW Physical Base 0x813000000). Position independent; no Apple code.
 *
 * Build (NDK clang 19):
 *   clang -c -x assembler-with-cpp sep_iommu_ro_test.S -o t.o
 *   ld.lld -Ttext=0x240000000 --image-base=0x240000000 -e _start t.o -o t.elf
 *   llvm-objcopy -O binary t.elf sep_iommu_ro_test.seprom
 * Run: the iOS 14.8 restore kit (docs/PHONE-SMOKE.md) with
 *   -M t8030,...,sep-rom=sep_iommu_ro_test.seprom and any non-uniform sep-fw;
 *   the "SEPTEST" lines appear on the serial console once the kernel powers the
 *   SEP on. -d guest_errors also logs each "IOMMU denied ..." access.
 * This file is a test input; it is not compiled into QEMU.
 */
    .set UART_UTXH,   0x235200020
    .set DART,        0x2412C0000
    .set DART_TLB_OP, 0x20
    .set DART_TLBSET, 0x34
    .set DART_SIDCFG, 0x100            /* SID_CONFIG(0) */
    .set DART_TTBR,   0x200            /* TTBR(0, 0) */
    .set L1,          0x813000000
    .set L2,          0x813004000
    .set PHYS_RO,     0x813008000
    .set PHYS_RW,     0x81300C000
    .set PHYS_WO,     0x813010000
    .set IOVA_RO,     0x100000
    .set IOVA_RW,     0x104000
    .set IOVA_WO,     0x108000
    .set PAT_RO,      0x11223344
    .set PAT_RW,      0x55667788
    .set PAT_WO,      0x99aabbcc
    .set PAT_BAD,     0xdeadbeef
    .set RET_INSN,    0xd65f03c0       /* ret */
    .set TTBR_VAL,    ((1 << 31) | (L1 >> 12))
    .set MAX_TRIES,   50

/*
 * Registers: x19 = failed-check bitmask (bit n = check n), x20 = fault
 * expectation (0 none, 1 data abort, 3 fetch abort), x22 = aborts taken in
 * the current check, x24/x25 = ESR/FAR of the last abort, x26 = tries,
 * x11/x12 x13/x14 x15/x16 = ESR/FAR saved for c3, c6, c7, x27 = an
 * unexpected abort was taken (ESR/FAR in x17/x18).
 */
    .text
    .globl _start
_start:
    adr     x0, vectors
    msr     vbar_el1, x0
    isb
    mov     x26, #0
    adr     x0, s_start
    bl      puts

try:
    add     x26, x26, #1
    mov     x19, #0
    mov     x20, #0
    mov     x27, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0

    /* page tables (16 KiB DART pages): L1[0] -> L2; L2[0x40] RO, [0x41] RW, [0x42] WO */
    ldr     x1, =L1
    ldr     x2, =(L2 | 1)
    str     x2, [x1]
    ldr     x1, =L2
    ldr     x2, =(PHYS_RO | 1 | (1 << 7))
    str     x2, [x1, #(0x40 * 8)]
    ldr     x2, =(PHYS_RW | 1)
    str     x2, [x1, #(0x41 * 8)]
    ldr     x2, =(PHYS_WO | 1 | (1 << 8))
    str     x2, [x1, #(0x42 * 8)]
    /* seed pages through their physical addresses */
    ldr     x1, =PHYS_RO
    ldr     w2, =PAT_RO
    str     w2, [x1]
    ldr     x1, =PHYS_RW
    str     wzr, [x1]
    ldr     x1, =PHYS_WO
    str     wzr, [x1]
    ldr     w2, =RET_INSN               /* executable content: only the IOMMU can stop the fetch */
    str     w2, [x1, #4]
    dsb     sy

    /* dart-sep SID 0: TTBR -> L1, enable translation, invalidate */
    ldr     x1, =DART
    ldr     w2, =TTBR_VAL
    str     w2, [x1, #DART_TTBR]
    mov     w2, #(1 << 7)
    str     w2, [x1, #DART_SIDCFG]
    mov     w2, #1
    str     w2, [x1, #DART_TLBSET]
    mov     w2, #(1 << 20)
    str     w2, [x1, #DART_TLB_OP]
    dsb     sy
    isb

    /* c1: read through the RO mapping */
    ldr     x1, =IOVA_RO
    ldr     w2, [x1]
    ldr     w3, =PAT_RO
    cmp     w2, w3
    b.eq    1f
    orr     x19, x19, #(1 << 1)
1:
    /* c2: write through the RW mapping, visible at the physical page */
    ldr     x1, =IOVA_RW
    ldr     w2, =PAT_RW
    str     w2, [x1]
    dsb     sy
    ldr     x1, =PHYS_RW
    ldr     w3, [x1]
    cmp     w2, w3
    b.eq    1f
    orr     x19, x19, #(1 << 2)
1:
    /* c3: write through the RO mapping must abort; c4: page unchanged */
    mov     x22, #0
    mov     x20, #1
    ldr     x1, =IOVA_RO
    ldr     w2, =PAT_BAD
    str     w2, [x1]
    dsb     sy
    mov     x20, #0
    mov     x11, x24
    mov     x12, x25
    cmp     x22, #1
    b.eq    1f
    orr     x19, x19, #(1 << 3)
1:  ldr     x1, =PHYS_RO
    ldr     w2, [x1]
    ldr     w3, =PAT_RO
    cmp     w2, w3
    b.eq    1f
    orr     x19, x19, #(1 << 4)
1:
    /* c5: write through the WO mapping is allowed */
    ldr     x1, =IOVA_WO
    ldr     w2, =PAT_WO
    str     w2, [x1]
    dsb     sy
    ldr     x1, =PHYS_WO
    ldr     w3, [x1]
    cmp     w2, w3
    b.eq    1f
    orr     x19, x19, #(1 << 5)
1:
    /* c6: read through the WO mapping must abort */
    mov     x22, #0
    mov     x20, #1
    ldr     x1, =IOVA_WO
    ldr     w2, [x1]
    mov     x20, #0
    mov     x13, x24
    mov     x14, x25
    cmp     x22, #1
    b.eq    1f
    orr     x19, x19, #(1 << 6)
1:
    /* c7: executing from the WO mapping must abort (handler returns to lr) */
    mov     x22, #0
    mov     x20, #3
    ldr     x1, =(IOVA_WO + 4)
    blr     x1
    mov     x20, #0
    mov     x15, x24
    mov     x16, x25
    cmp     x22, #1
    b.eq    1f
    orr     x19, x19, #(1 << 7)
1:
    /* was the DART left alone for the whole run? otherwise retry */
    ldr     x1, =DART
    ldr     w2, [x1, #DART_TTBR]
    ldr     w3, =TTBR_VAL
    cmp     w2, w3
    b.ne    disturbed
    ldr     w2, [x1, #DART_SIDCFG]
    tbz     w2, #7, disturbed
    b       report
disturbed:
    cmp     x26, #MAX_TRIES
    b.lt    try
    adr     x0, s_giveup
    bl      puts
    b       park

report:
    adr     x0, s_tries
    bl      puts
    mov     x0, x26
    bl      puthex
    adr     x0, s_nl
    bl      puts
    mov     x9, #1
    adr     x10, labels
1:  mov     x0, x10
    bl      puts
    adr     x0, s_ok
    lsr     x2, x19, x9
    tbz     x2, #0, 2f
    adr     x0, s_bad
2:  bl      puts
    /* next label */
3:  ldrb    w2, [x10], #1
    cbnz    w2, 3b
    add     x9, x9, #1
    cmp     x9, #8
    b.lt    1b
    adr     x0, s_c3
    mov     x0, x0
    bl      puts
    mov     x0, x11
    bl      puthex
    adr     x0, s_far
    bl      puts
    mov     x0, x12
    bl      puthex
    adr     x0, s_c6
    bl      puts
    mov     x0, x13
    bl      puthex
    adr     x0, s_far
    bl      puts
    mov     x0, x14
    bl      puthex
    adr     x0, s_c7
    bl      puts
    mov     x0, x15
    bl      puthex
    adr     x0, s_far
    bl      puts
    mov     x0, x16
    bl      puthex
    adr     x0, s_nl
    bl      puts
    cbz     x27, 4f
    orr     x19, x19, #1
    adr     x0, s_unexp
    bl      puts
    mov     x0, x17
    bl      puthex
    adr     x0, s_far
    bl      puts
    mov     x0, x18
    bl      puthex
    adr     x0, s_nl
    bl      puts
4:  adr     x0, s_pass
    cbz     x19, 1f
    adr     x0, s_fail
1:  bl      puts
park:
    wfi
    b       park

/* sync exception handler (current EL, SP_ELx): record, resume if expected */
sync_handler:
    mrs     x24, esr_el1
    mrs     x25, far_el1
    cbnz    x20, 2f
    mov     x27, #1                     /* unexpected: note it, skip the access */
    mov     x17, x24
    mov     x18, x25
    b       3f
2:  add     x22, x22, #1
    cmp     x20, #3
    b.eq    1f
3:  mrs     x0, elr_el1                 /* data abort: skip the access */
    add     x0, x0, #4
    msr     elr_el1, x0
    eret
1:  msr     elr_el1, x30                /* fetch abort: return to the caller */
    eret

/* puts: x0 = NUL-terminated string (leaf, clobbers x0-x2) */
puts:
    ldr     x1, =UART_UTXH
1:  ldrb    w2, [x0], #1
    cbz     w2, 2f
    str     w2, [x1]
    b       1b
2:  ret

/* puthex: x0 = value, 16 hex digits (leaf, clobbers x0-x4) */
puthex:
    ldr     x1, =UART_UTXH
    mov     x3, #60
1:  lsr     x2, x0, x3
    and     x2, x2, #0xf
    cmp     x2, #10
    add     x4, x2, #'0'
    add     x2, x2, #('a' - 10)
    csel    x2, x4, x2, lt
    str     w2, [x1]
    subs    x3, x3, #4
    b.ge    1b
    ret

    .ltorg

s_start:  .asciz "\nSEPTEST start\n"
s_tries:  .asciz "SEPTEST runs until the DART was undisturbed: 0x"
labels:
    .asciz "SEPTEST c1 read via RO mapping: "
    .asciz "SEPTEST c2 write via RW mapping: "
    .asciz "SEPTEST c3 write via RO mapping aborts: "
    .asciz "SEPTEST c4 RO page unchanged: "
    .asciz "SEPTEST c5 write via WO mapping: "
    .asciz "SEPTEST c6 read via WO mapping aborts: "
    .asciz "SEPTEST c7 execute from WO mapping aborts: "
s_ok:     .asciz "ok\n"
s_bad:    .asciz "FAILED\n"
s_c3:     .asciz "SEPTEST aborts: c3 esr=0x"
s_c6:     .asciz "\nSEPTEST aborts: c6 esr=0x"
s_c7:     .asciz "\nSEPTEST aborts: c7 esr=0x"
s_far:    .asciz " far=0x"
s_nl:     .asciz "\n"
s_unexp:  .asciz "SEPTEST FAIL: unexpected exception esr=0x"
s_giveup: .asciz "SEPTEST INCONCLUSIVE: dart-sep kept changing under the test\n"
s_pass:   .asciz "SEPTEST PASS\n"
s_fail:   .asciz "SEPTEST FAIL\n"

    .balign 0x800
vectors:
    .rept 16
        b   sync_handler
        .balign 0x80
    .endr
