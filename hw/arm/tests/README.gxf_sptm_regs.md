# gxf_sptm_regs_test: check for the iOS 27 SPTM register block (M0)

`gxf_sptm_regs_test.S` is a tiny bare-metal AArch64 program that exercises the
Apple system registers added to `hw/arm/a13_gxf.c` for iOS 27 SPTM/TXM:

- GXF-space (op1=6, CRn=15): `S3_6_C15_C0_0`, `S3_6_C15_C2_0`, `S3_6_C15_C8_7`
- Apple EL2 impl-defined (op1=4, CRn=15): `S3_4_C15_C0_0..C0_5`,
  `S3_4_C15_C1_0..C1_4`, `S3_4_C15_C2_2..C2_5`

## What it proves

At non-guarded EL1 it writes a pattern to each register and reads it back. It
then reproduces SPTM's bootstrap prologue in miniature: `GXF_CONFIG_EL1 = 1`,
`GXF_ENTER_EL1 = handler`, `GENTER`. It checks that `GXF_STATUS_EL1` reports
guarded, round-trips the GXF-space registers again from GL1, `GEXIT`s, and
checks that `GXF_STATUS_EL1` reports non-guarded.

Every EL1 and guarded vector (`VBAR_EL1`, `VBAR_GL11`, `GXF_ABORT_EL1`)
reports FAIL with the ESR, so a missing register (UNDEF) fails closed. A hang
is caught by the runner's timeout.

## Running it

It runs on the firmware-free `a13-test` machine (`hw/arm/a13-test.c`): one
AppleA13 core (the CPU type t8030 creates), RAM, and a control register whose
writes become QEMU's exit status. No Apple code, data or images are involved.

```sh
hw/arm/tests/run-a13-tests.py build/qemu-system-aarch64
```

The runner needs `clang` with the AArch64 backend (Apple's clang works) and
Python 3. It runs two cases, and both must come out as expected:

| Case | Expected |
|---|---|
| `gxf_sptm_regs` | QEMU exits 0 and prints `a13-test: PASS` |
| `gxf_sptm_regs (negative control)`, built with `-DNEGATIVE_CONTROL` | FAIL at check 19 from an exception vector with `ESR.EC == 0` (UNDEF on `S3_6_C15_C14_7`, which nothing defines) |

The negative control shows that a missing register really is detected. Without
it, a harness that never ran the checks would also "pass".

The CI `build` workflow runs this after each Linux and macOS host build.
