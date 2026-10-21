# gxf_sptm_regs_test — self-contained check for the iOS 27 SPTM register block

`gxf_sptm_regs_test.S` is a tiny bare-metal AArch64 program that exercises the
Apple system registers added to `hw/arm/a13_gxf.c` for iOS 27 SPTM/TXM:

- GXF-space (op1=6, CRn=15): `S3_6_C15_C0_0`, `S3_6_C15_C2_0`, `S3_6_C15_C8_7`
- Apple EL2 impl-defined (op1=4, CRn=15): `S3_4_C15_C0_0..C0_5`,
  `S3_4_C15_C1_0..C1_4`, `S3_4_C15_C2_2..C2_5`

## What it proves
At EL1 (and, for the GXF-space regs, from inside GL1 after a GENTER) it writes a
pattern to each register, reads it back, and compares. It installs a `VBAR_EL1`
vector whose every entry writes a FAIL token, so a missing register — which would
raise UNDEF — fails the test closed. On success it writes a PASS token to
`RESULT_ADDR` (0x40000000) and parks in `wfi`. It also GENTER/GEXIT round-trips to
confirm the guarded path still works with the new registers present.

## Build (NDK llvm / clang 19)
```sh
NDK=.../ndk/<ver>/toolchains/llvm/prebuilt/<host>/bin
$NDK/aarch64-linux-android30-clang -c -x assembler-with-cpp \
    gxf_sptm_regs_test.S -o gxf_sptm_regs_test.o
$NDK/ld.lld -Ttext=0 --image-base=0 gxf_sptm_regs_test.o -o gxf_sptm_regs_test.elf
$NDK/llvm-objcopy -O binary gxf_sptm_regs_test.elf gxf_sptm_regs_test.bin
```
The assembler step alone is a useful check: it confirms every register encoding and
the GENTER (`0x00201420`) / GEXIT (`0x00201400`) opcodes are well-formed and map to
exactly the encodings the register table defines. (Verified: the `.S` assembles with
clang 19 and emits `msr/mrs S3_6_C15_C0_0`, `… C2_0`, `… C8_7`, the full
`S3_4_C15_*` block, and the canonical GENTER/GEXIT words.)

## Run
The GXF register block is registered by `apple_a13_init_gxf()`, which runs when the
**t8030 AppleA13 CPU** is realized (`-cpu apple-gxf` on the generic machines does not
install it). Running the blob to completion therefore needs the t8030 machine, which
in turn needs the SPTM/TXM/kernel boot chain the loader agents are building. Once
that boots, drop this blob in at EL1 early and check `RESULT_ADDR` for PASS, or run
with `-d unimp,guest_errors` and confirm no "GXF/SPTM: ... unmodelled" line reports an
*undefined* access (the new regs log at LOG_UNIMP but return/accept values rather
than UNDEF-ing).

## Status
Self-contained at the **assembly/encoding** level today (asm + build verified
against the register table). End-to-end execution is gated on the t8030 SPTM boot
path and is the integration step for that work. This matches the register additions
being storage-backed with LOG_UNIMP tracing: the first real boot will show which
registers the firmware actually depends on.
