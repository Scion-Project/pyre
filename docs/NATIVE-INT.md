# Guest integer code as host instructions (`-accel tcg,native-int=on`)

On an AArch64 host, TCG can execute covered AArch64 guest EL0 integer
instructions as the same host instructions, with the guest registers in host
registers and the guest NZCV in the host flags. This is step 1 of the
same-ISA translation tier ("SIT") in [SAME-ISA-FASTPATH.md](SAME-ISA-FASTPATH.md).
Default: **off**. On other hosts the option has no effect (an explicit
`native-int=on` prints `native-int: not available on this host`).

## How it works

The A64 translator emits a *SIT region* for a run of covered instructions
(`include/tcg/tcg-sit.h`): `sit_enter`, one `sit` op per instruction
(between the usual `insn_start` ops), and `sit_exit` or a SIT conditional
branch. TCG saves its globals to the CPU state at `sit_enter`, like at a
label, and does not allocate host registers inside the region. The AArch64
backend (`tcg/aarch64/tcg-target-sit.c.inc`):

* maps each guest register the region uses to a host register (x20-x28,
  x0-x14), loaded from the CPU state on first use;
* copies data-processing instructions with their register fields renamed;
* emits loads and stores as the softmmu TLB fast path, rewritten to leave
  the host flags alone (EOR + CBNZ), then the native host access, with an
  out-of-line slow path calling the usual `helper_*_mmu`;
* keeps the guest NZCV in the host NZCV from the first instruction that
  writes or reads it;
* writes the guest registers (and NZCV) back to the CPU state at the end of
  the region, before a conditional branch out of it, and **in every slow
  path before the helper call**. The CPU state is therefore exact wherever a
  guest exception, a watchpoint or a TB invalidation can happen; the guest
  PC comes from the `insn_start` data as for any TCG code.

Any instruction translated with TCG ops ends the region first (the
translator inserts `sit_exit` after its `insn_start`), so coverage is per
instruction and the TCG path is always available.

## What is covered

EL0, AArch64, little-endian data, no single step, no BTI, no MTE:

| class | instructions |
|---|---|
| data processing | ADD/SUB(S) immediate, shifted and extended register; AND/ORR/EOR/ANDS (immediate) and the shifted-register logical forms; MOVZ/MOVN/MOVK; SBFM/UBFM/BFM; EXTR; MADD/MSUB/SMADDL/SMSUBL/UMADDL/UMSUBL/SMULH/UMULH; UDIV/SDIV; LSLV/LSRV/ASRV/RORV; RBIT/REV16/REV32/REV/CLZ/CLS; ADC/SBC(S); CCMP/CCMN; CSEL/CSINC/CSINV/CSNEG |
| PC-relative | ADR, ADRP |
| loads/stores | LDR/STR of general-purpose registers: unsigned and unscaled offsets, pre/post-index, register offset, sign-extending loads; LDP/STP/LDPSW; LDR (literal) |
| SIMD&FP loads/stores | LDR/STR of B/H/S/D/Q registers (same addressing modes), LDP/STP of S/D/Q registers, LDR (literal); no SVE/SME. The data goes through the CPU state (vector registers are not mapped) |
| system registers | MRS of plain 64-bit fields with no access check (TPIDR_EL0, TPIDRRO_EL0) |
| branches | B.cond (when the guest flags are already in the host flags), CBZ/CBNZ, TBZ/TBNZ (inside a region) |

Everything else stays on the existing paths, in particular system registers,
exception-generating instructions, PAC, exclusives, LSE atomics,
acquire/release, FP/SIMD, unprivileged loads/stores, and writeback forms
with Rt == Rn (CONSTRAINED UNPREDICTABLE).

Options:

* `native-int=on|off` (default off);
* `x-native-int-ldst=off`: diagnostic, loads and stores stay on TCG;
* `x-native-int-el1=on`: experimental, also at EL1 (measurement only);
* `x-native-int-fault=on`: test only, slow paths skip the register
  write-back (the directed test must catch it).

## Testing

`hw/arm/tests/run-a13-tests.py` (CI):

* `native_int_check.S`, directed EL0 tests with native-int off, on, and on
  with loads/stores on TCG: registers, NZCV and FAR exact at a faulting
  load and a faulting post-index load; SP as a base register with
  pre/post-index pairs; x18/x29/x30 as ordinary registers; NZCV across
  page-crossing (slow-path) accesses; a store over code later in the same
  block (TB invalidation from inside a region). Negative control:
  `x-native-int-fault=on` must fail at check 1.
* `native_int_rand.S` + `native_int_gen.py`: random EL0 programs (3000
  instructions per seed: the covered classes with random registers,
  immediates, shifts and extends, loads/stores through three moving base
  registers, one window crossing a 16 KiB page, forward conditional
  branches, and TCG-only instructions in between so regions start and end
  everywhere). The hash of all registers, flags and the 64 KiB buffer must
  be the same with native-int off, on, and on with loads/stores on TCG,
  and on AArch64 hosts the same as the host CPU running the program
  natively. Seed 1 also checks, through `-d op`, that native-int really
  emitted SIT regions. `NATIVE_INT_SEEDS` (default 8) sets the number of
  seeds; 300 seeds pass on an M1 host.

`hw/arm/tests/run-a13-bench.py` has EL0 variants of the microbenchmarks
(`u1`, `u2`, `u4`, `u10`, `u12`: `tcg_bench.S -DEL0`), which run at EL0 with
the MMU on, where native-int applies.
