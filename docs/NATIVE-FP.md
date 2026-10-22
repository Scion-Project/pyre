# Guest floating point on the host FPU (`-accel tcg,native-fp=on`)

On an AArch64 host, TCG can execute some AArch64 guest floating-point
instructions with the host instruction of the same name instead of
softfloat. Guest and host implement the same architecture, so with the same
FPCR controls the host gives the bit-identical result and the same FPSR
cumulative flags (NaN propagation, tininess before rounding, fused rounding
are all Arm rules on both sides). Background, profile and the PoC are in
[SAME-ISA-FASTPATH.md](SAME-ISA-FASTPATH.md); FP/SIMD-heavy guest code spends
62-79% of its time in softfloat.

Default: **off**. Turn it on with `-accel tcg,native-fp=on`.

## When the host FPU is used

All of the following must hold; otherwise the instruction runs on softfloat,
exactly as with `native-fp=off`.

1. **Host.** The QEMU binary is built for AArch64 (macOS on Apple silicon,
   Linux/Android arm64). On any other host the option only prints
   `native-fp: not available on this host` and has no effect.
2. **Host FP mode.** The host FPCR is 0 (round to nearest even, no FZ, FZ16,
   DN, AH, FIZ, no trap enables) when QEMU starts; QEMU never changes it. A
   vCPU thread that finds it non-zero resets it to 0 (with a warning).
3. **Host self-test.** Once, before any vCPU runs, every native kernel is
   compared with the softfloat path of the helper it replaces over all pairs
   (binary operations) or triples (fused multiply-add) of 24 special operands
   per precision: signed zeros, smallest/largest denormals, smallest normal,
   largest finite, infinities, quiet and signalling NaNs with payloads (both
   signs), rounding ties and ordinary values, at both vector widths. Result
   bits and the IOC/DZC/OFC/UFC/IXC flags must be identical (88128 cases).
   Any difference prints `native-fp: host self-test failed: <case>` and the
   whole run stays on softfloat. On success QEMU prints
   `native-fp: on (host self-test passed, N cases)`.
4. **Guest FP mode.** The guest FPCR selects the same mode as the host:
   RMode = 00 (round to nearest even), FZ = 0, DN = 0 (IEEE NaN propagation,
   not default-NaN), FPCR.AH = 0 (the Arm NaN-propagation, tininess and
   0 * Inf + NaN rules, not the FEAT_AFP alternate ones), FIZ = 0, and no
   exception trap enables (QEMU implements them as RAZ/WI anyway). FZ16, AHP
   and EBF only affect half precision and BFloat16, which are never executed
   natively; NEP is handled by the translator and does not change the
   arithmetic. A guest that sets any of the checked bits runs on softfloat
   until it clears them again.
5. **Instruction.** Only the AArch64 FP status (`FPST_A64`) is ever native:
   AArch32 code, the FPCR.AH=1 status, the "standard FPSCR" status and the
   SME ZA status always use softfloat.

Covered instructions (helper level; each one is in the self-test and in
`hw/arm/tests/fp_native_check.S`):

| guest instruction | forms | helper |
|---|---|---|
| FADD, FSUB, FMUL (vector) | `.2s`, `.4s`, `.2d` | `gvec_f{add,sub,mul}_{s,d}` |
| FMLA, FMLS (vector) | `.2s`, `.4s`, `.2d` | `gvec_vf{ma,ms}_{s,d}` |

The same helpers serve the unpredicated SVE forms, which therefore also run
natively under the same conditions.

## Cumulative exception flags

The hot path never writes FPSR (writing it costs more than the operation on
Apple M1). While a vCPU is live (inside `cpu_exec` on its own thread with a
native-compatible FPCR), native operations leave their flags in the host
FPSR, and the guest's cumulative flags are the union of the softfloat state
and the host FPSR. The host FPSR is folded into the guest state

* when the guest reads FPSR (`MRS FPSR`, also gdbstub/migration paths on the
  vCPU thread),
* before the guest FPCR changes (the flags are folded under the old mode),
* when the vCPU leaves `cpu_exec`.

It is cleared only when it holds a flag the guest does not have: when the
guest writes FPSR (e.g. clears it), or when the vCPU (re)enters the native
regime after C code raised a flag outside it. Flags are only ever added to
the guest, never lost.

Caveat: host C code that runs on the vCPU thread while it is live and does
host floating-point arithmetic would have its flags folded into the guest's.
In QEMU that is softfloat's own hardfloat fast path, which only executes the
same guest operation on the same host semantics (so it raises exactly the
flags the guest operation raises), and device models reached through MMIO
helpers; none of the Apple SoC models or the `a13-test` machine use host
floating point.

## Testing

`hw/arm/tests/run-a13-tests.py` (CI, every host) runs
`fp_native_check.S`: FADD/FSUB/FMUL/FMLA/FMLS over all 8000 operand triples
of 20 special values per precision, `.2d`, `.4s` and `.2s`, hashing result
bits and FPSR after every operation, under seven successive FPCR modes
(native, FZ, DN, round towards +Inf, round towards zero, FZ|DN, native again)
so the native/softfloat transitions are covered. Variants:

* FPSR cleared before every operation, and `-DSTICKY` (never cleared, covers
  accumulation across operations and FPCR writes);
* `native-fp=off` and `=on`: both must give the embedded softfloat hash;
* negative controls (AArch64 hosts): `x-native-fp-fault=on` makes the native
  FADD (double) swap its operands, which changes NaN propagation. With the
  self-test the fault must be caught (`host self-test failed`) and the run
  must still pass on softfloat; with `x-native-fp-selftest=off` the guest
  hash must change (FAIL at check 1);
* the same instruction sequence run natively on the host CPU
  (`-DHOST_HARNESS`, AArch64 hosts) must give the embedded hash too, so
  softfloat, native-fp and the host hardware agree three ways.

`x-native-fp-fault` and `x-native-fp-selftest` are test-only.
