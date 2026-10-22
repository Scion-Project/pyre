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
   signs), rounding ties, integer-range boundaries for the conversions and
   ordinary values, at both vector widths, every index of the by-element
   forms and every rounding mode of the float-to-int conversions. Result
   bits and the IOC/DZC/OFC/UFC/IXC flags must be identical (426768 cases,
   about 0.1 s once at startup on Apple M1).
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
`hw/arm/tests/fp_native_check.S`), single and double precision:

| guest instruction | forms | helper |
|---|---|---|
| FADD, FSUB, FMUL, FDIV (vector) | `.2s`, `.4s`, `.2d` | `gvec_f{add,sub,mul,div}_{s,d}` |
| FMLA, FMLS (vector) | `.2s`, `.4s`, `.2d` | `gvec_vf{ma,ms}_{s,d}` |
| FMIN, FMAX, FMINNM, FMAXNM (vector) | `.2s`, `.4s`, `.2d` | `gvec_f{min,max,minnum,maxnum}_{s,d}` |
| FABD, FCMEQ, FCMGE, FCMGT (vector, register) | `.2s`, `.4s`, `.2d` | `gvec_f{abd,ceq,cge,cgt}_{s,d}` |
| FMUL, FMLA, FMLS (by element) | `.2s`, `.4s`, `.2d` | `gvec_f{mul,mla,mls}_idx_{s,d}` |
| FADD, FSUB, FMUL, FDIV, FMIN, FMAX, FMINNM, FMAXNM (scalar) | `s`, `d` | `vfp_{add,sub,mul,div,min,max,minnum,maxnum}{s,d}` |
| FMADD, FMSUB, FNMADD, FNMSUB (scalar) | `s`, `d` | `vfp_muladd{s,d}` (negation is done by the translator) |
| FSQRT (scalar) | `s`, `d` | `vfp_sqrt{s,d}` |
| FCMP, FCMPE (scalar, register) | `s`, `d` | `vfp_cmp{,e}{s,d}_a64` |
| FCVT (single <-> double) | | `vfp_fcvt{ds,sd}` |
| SCVTF, UCVTF (scalar, from W/X, no fixed point) | `s`, `d` | `vfp_{s,u}qto{s,d}` |
| FCVT{Z,N,A,P,M}{S,U} (scalar, to W/X, no fixed point) | `s`, `d` | `vfp_to{s,u}{l,q}{s,d}` |

The vector helpers also serve the unpredicated SVE forms. The float-to-int
conversions take their rounding from the instruction (the translator sets
it in the status around the helper; the host instruction encodes it), every
other covered operation additionally requires the status rounding mode to be
round-to-nearest-even. Fixed-point conversions (`#fbits` != 0), half
precision, BFloat16, reductions, pairwise forms, FRINT*, FRECPE/FRSQRTE and
the reciprocal steps stay on softfloat.

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
`fp_native_check.S`, which hashes the result bits (or the integer result, or
NZCV for compares) and FPSR after every operation, under seven successive
FPCR modes (native, FZ, DN, round towards +Inf, round towards zero, FZ|DN,
native again) so the native/softfloat transitions are covered:

* all 8000 (a, b, c) triples of 20 special values per precision through
  vector FADD/FSUB/FMUL/FMLA/FMLS (`.2d`, `.4s`, `.2s`), scalar
  FMADD/FMSUB/FNMADD/FNMSUB and FMLA/FMLS by element;
* all (a, b) pairs of those values plus 12 conversion operands (ties,
  integer-range boundaries) through every other covered vector, scalar,
  compare and conversion form (FCVT{Z,N,A,P,M}{S,U} to W and X,
  SCVTF/UCVTF from W and X using the operand bits as the integer).

Variants:

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
