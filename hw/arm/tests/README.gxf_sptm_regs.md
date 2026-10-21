# gxf_sptm_regs_test: check for the iOS 27 SPTM register block (M0)

`gxf_sptm_regs_test.S` is a tiny bare-metal AArch64 program that checks every
Apple system-register encoding iOS 27's SPTM/TXM/kernel were seen to use (see
ios-on-pixel `docs/ios27/GXF-GAP.md` §3) is reachable on the AppleA13 CPU.

| Encoding | Model | Test |
|---|---|---|
| `S3_6_C15_C0_0`, `S3_6_C15_C8_7`, `S3_4_C15_C0_0`, `C0_1`, `C0_3` | SPTM placeholder (storage, `hw/arm/a13_gxf.c`) | write pattern, read back |
| `S3_6_C15_C2_0` MMU_ERR_STS, `S3_4_C15_C0_2` FED_ERR_STS, `C0_5`, `C1_4` AMX_CTL | A13 core table | read, write same value back |
| `S3_4_C15_C2_2..C2_5` CTRR_LOCK / A_LWR / A_UPR / CTL | A13 cluster table | read, write same value back |
| `S3_4_C15_C0_4` APCTL, `C1_0`/`C1_1` KERNELKEYLO/HI, `C1_2` VMSA_LOCK | `target/arm/helper.c` | read, write same value back |
| `S3_4_C15_C1_3` AMX_STATUS | A13 core table (read-only) | read |

## What it proves

At non-guarded EL1 it runs the checks above. It then reproduces SPTM's
bootstrap prologue in miniature: `GXF_CONFIG_EL1 = 1`,
`GXF_ENTER_EL1 = handler`, `GENTER`. It checks that `GXF_STATUS_EL1` reports
guarded, re-checks the GXF-space registers from GL1, then `GEXIT`s. It checks
that GEXIT returns after the GENTER (as a kernel-to-SPTM call does) and that
`GXF_STATUS_EL1` then reports non-guarded.

While guarded, the EL1 register names (`VBAR_EL1`, `ELR_EL1`, ...) address the
GL1 bank, and the `*_GL11` names address the non-guarded EL1 copies (and are
only accessible from GL1). So the test points the GL1 vectors at its fail
table with `VBAR_EL1` from inside GL1.

Every EL1 and GL1 vector, and `GXF_ABORT_EL1`, reports FAIL with the ESR, so a
missing register (UNDEF) fails closed. A hang is caught by the runner's
timeout.

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
