# Guest floating point on the host FPU (`-accel tcg,native-fp=on`)

On an AArch64 host, TCG can execute some AArch64 guest floating-point
instructions with the host instruction of the same name instead of
softfloat. Guest and host implement the same architecture, so with the same
FPCR controls the host gives the bit-identical result and the same FPSR
cumulative flags (NaN propagation, tininess before rounding, fused rounding
are all Arm rules on both sides). Background, profile and the PoC are in
[SAME-ISA-FASTPATH.md](SAME-ISA-FASTPATH.md); FP/SIMD-heavy guest code spends
62-79% of its time in softfloat.

Default: **on** wherever it applies (AArch64 hosts); `-accel tcg,native-fp=off`
turns it off. With the default QEMU only reports problems (a failed host
self-test, an unexpected host FPCR); with an explicit `native-fp=on` it also
reports the outcome (`native-fp: on (...)` or `not available on this host`).
See "Validation" at the end for what the default rests on.

## When the host FPU is used

All of the following must hold; otherwise the instruction runs on softfloat,
exactly as with `native-fp=off`.

1. **Host.** The QEMU binary is built for AArch64 (macOS on Apple silicon,
   Linux/Android arm64). On any other host the option has no effect (an
   explicit `native-fp=on` prints `native-fp: not available on this host`).
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
   whole run stays on softfloat. On success an explicit `native-fp=on`
   prints `native-fp: on (host self-test passed, N cases)`.
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
`hw/arm/tests/fp_native_check.S`), single and double precision (the
hottest are also emitted inline, see the next section):

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

## Inline host FP (TCG AArch64 backend)

The helper-level path still pays a helper call per instruction. On top of
it, the translator emits the most common forms as host FP instructions in
the generated code, with no call at all:

| guest instruction | inline as |
|---|---|
| FADD, FSUB, FMUL, FDIV, FMIN, FMAX, FMINNM, FMAXNM, FABD, FCMEQ, FCMGE, FCMGT (vector, `.2s/.4s/.2d`) | the same AdvSIMD instruction |
| FMLA, FMLS (vector) | FMLA, FMLS |
| FMUL, FMLA, FMLS (by element) | load-and-broadcast of the element (LD1R) + the vector form |
| FADD, FSUB, FMUL, FDIV, FMIN, FMAX, FMINNM, FMAXNM, FSQRT (scalar `s/d`) | the same scalar instruction |
| FMADD, FMSUB, FNMADD, FNMSUB (scalar `s/d`) | the same instruction |

Everything else covered above (compares setting NZCV, conversions) stays in
the native helpers.

How it is guarded:

* A TB flag, `NATIVE_FP` (A64 hflags), is set when native-fp is on and the
  guest FPCR is native-compatible (condition 4 above). The translator only
  emits inline host FP in TBs that carry it, so the FP mode is part of the
  TB key and there is no per-instruction check. Guest `MSR FPCR` already
  ends the TB and rebuilds hflags; FPCR changes from outside guest code
  (gdbstub, reset, migration) are caught on the next `cpu_exec` entry, which
  rebuilds hflags if the flag disagrees with FPCR.
* New TCG vector opcodes `fp1_vec`, `fp2_vec`, `fp3_vec` (constant args:
  the operation and "scalar"), implemented only by the AArch64 backend
  (`TCG_TARGET_HAS_fp_vec`); every other backend reports them unsupported
  and never sees them, since the flag is only ever set on AArch64 hosts.
* With FPCR.NEP = 1 the scalar forms keep the helper path (merging).
* Flags are handled exactly as for the helpers (next section): the inline
  instructions accumulate into the host FPSR while the vCPU is live.

`-accel tcg,native-fp=on,x-native-fp-inline=off` keeps native FP in the
helpers only (diagnostic, and the CI tests run both ways).

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
* `native-fp=off`, `=on` (inline host FP) and `=on` with
  `x-native-fp-inline=off` (helpers only): all must give the embedded
  softfloat hash;
* negative controls (AArch64 hosts): `x-native-fp-fault=on` makes the native
  FADD (double) swap its operands, which changes NaN propagation. With the
  self-test the fault must be caught (`host self-test failed`) and the run
  must still pass on softfloat; with `x-native-fp-selftest=off` the guest
  hash must change (FAIL at check 1), both inline (the translator swaps the
  operands of the inline FADD.2d) and with helpers only;
* the same instruction sequence run natively on the host CPU
  (`-DHOST_HARNESS`, AArch64 hosts) must give the embedded hash too, so
  softfloat, native-fp and the host hardware agree three ways.

`x-native-fp-fault` and `x-native-fp-selftest` are test-only.

## Validation

Everything below is in CI or reproducible with the scripts named.

* Bit-exactness: the host self-test (426768 cases) and `fp_native_check`
  on every CI host (Linux x86-64/arm64, macOS arm64/x86-64) and in the
  Android emulator; on the AArch64 hosts softfloat, native-fp (inline and
  helpers-only) and the host CPU itself give the same hashes
  (`e421108006d5ca6e`, sticky FPSR `c6159aad366011df`).
* iOS 14 restore boot (t8030, research kernel, Android emulator "iop", 4
  vCPUs, CI Android build of #33, 10 interleaved rounds of native-fp off
  and on, `restore_bench.py`): with native-fp on, 10/10 boots reached
  "waiting for host" with the same milestones and no panic (median
  waiting_host 13.7 s, off 13.8 s). One "off" boot hit the intermittent
  `Kernel data abort` at pc 0xfffffff0099126d4 that also appears in
  baseline builds without native-fp (it predates this work).
* Engagement during that boot (debug counters, not in the tree): every
  `cpu_exec` entry was in the native regime (FPCR was 0 throughout, 36 FPCR
  writes, none non-native), 163738 of 163741 A64 TBs were translated with
  `NATIVE_FP`, and 1002 FP operations were emitted inline. The restore boot
  is barely FP, so it validates the mode and flag machinery under iOS rather
  than FP throughput; the FP-heavy numbers are below.
* Performance (same binary, off / helpers only / inline, median of 5): in
  the Android emulator, `w7` (register-only vector FMLA/FMUL/FADD, 48M ops)
  3.72 s / 0.99 s / 0.08 s, `w8` (scalar FP mix) 0.71 / 0.62 / 0.31 s, `w4`
  (ALU control) 2.86 / 2.93 / 2.92 s. Linux guest on the M1 host (upstream
  10.2.2 with this code ported): `mem` 6.26 / 1.72 / 1.70 s, `simd` 7.06 /
  3.45 / 3.37 s (native: 0.22 and 0.32 s); a profile of the inline runs
  shows no softfloat or FP helper left, the rest is the TCG memory path and
  generated code. Startup cost of the self-test: about 0.05-0.1 s, once.
