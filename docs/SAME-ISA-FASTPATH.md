# Same-ISA fast path: how close to native can TCG get on an AArch64 host?

Status: research note and design proposal. Nothing here changes the emulator;
the proof of concept lives on the `poc/native-fp` branch behind an
environment variable, and the measurement tooling on `analysis/tbprof`.

## TL;DR

Inferno runs an AArch64 guest on an AArch64 host (Apple M-series Macs, Pixel
phones), but TCG treats it like any cross-ISA pair: every guest instruction
is lifted to TCG IR, re-lowered to host code, and its architectural state
lives in `CPUARMState` between instructions that need it. We measured where
the host instructions and the host time actually go, for a plain arm64 Linux
guest and for the `a13-test` microbenchmarks.

* **Integer code runs about 4.4x slower than native (CoreMark) and the
  generated code is the bottleneck: 90% of vCPU time is in the code cache.**
  Each guest instruction becomes **6.2-8.6 host instructions** (boot,
  CoreMark, a branchy integer loop), where a same-ISA translation needs
  about one per data-processing instruction. Of those host instructions,
  35-48% implement the operations themselves (2.3-4.2 per guest instruction:
  NZCV computed into four separate fields, 32-bit results zero-extended,
  shifted operands split), 23-25% are the softmmu TLB fast path (where the
  code does loads/stores), 11-15% are `CPUARMState` GPR spill/reload, 6-13%
  NZCV stores, 3-8% PC updates and 9-20% TB entry/exit glue.
* **Floating-point/SIMD code is a different story: 62-79% of vCPU time is in
  softfloat**, because every vector FADD/FMUL/FMLA is a helper call that
  loops over lanes in software. On a same-ISA host this is pure waste: the
  host FPU implements exactly the guest's semantics.
* **Syscall-heavy code with a KPTI kernel (the default for a KASLR arm64
  Linux) is dominated by softmmu TLB refills and jump-cache flushes (61% of
  vCPU time)**, because QEMU's TLB is not ASID-tagged and KPTI switches ASID
  twice per syscall: a null syscall costs ~21.5 us under TCG, 2.6 us with
  `nokaslr` (no KPTI), ~0.2 us native. No code-generation change touches
  this.

Ranked recommendation (details in "Design" and "Expected speedup"):

1. **Native FP/SIMD execution (do first).** Execute guest FP arithmetic on
   the host FPU when the guest FPCR mode equals the host's. The PoC
   (helper-level, ten gvec helpers, behind `QEMU_TCG_NATIVE_FP=1`) gives
   **2.9x on the vector-FP microbenchmark on the Android emulator**
   (3.68 s -> 1.27 s, control workload unchanged), **1.8x on the Linux
   double-precision triad** and only 1.1x on float32 SIMD code, where
   softfloat's own hardfloat shortcut already applies, with bit-identical
   results and FPSR flags over 8000 special-value operand triples. The
   remaining cost is the helper call and per-lane loop, so the real
   version must emit host FP instructions inline (new TCG FP vector ops).
2. **A same-ISA translation tier for integer code** ("SIT"): copy
   data-processing instructions verbatim with an identity register map and
   guest NZCV in host NZCV, rewrite only loads/stores (keep the softmmu fast
   path, flag-free), branches (TB chaining), system-register and
   exception-generating instructions; fall back to TCG per TB for anything
   else. The model below gives **~2.7x (CoreMark) to ~4.3x (branchy integer
   loop) fewer executed host instructions, bounding the speedup at ≤2.2-2.3x**
   (realistically 1.5-2x, since TCG's extra instructions are cheap ones).
   Register pinning inside TCG alone is worth much less (≤10%: GPR
   spill/reload is 11-15% of host instructions and any fixed set of 9
   pinnable registers covers only 71-75% of GPR references).
3. **ASID-tagged softmmu TLB** (independent of ISA; high value for KPTI
   guests, and for any guest that switches address spaces often).

The two alternative near-native designs are costed at the end: running guest
EL0 natively in the host process (large win for user code, very large
engineering and correctness cost), and a persistent translation cache (only
attacks translation time, which is 0.1-11% of vCPU time in these workloads).

## 1. What we measured, and how

### Vehicles

* **Plain arm64 Linux guest**: Debian bookworm arm64 installer kernel
  (6.1, `-M virt -cpu cortex-a72 -m 1G`) with a 0.8 MB initramfs whose
  `/init` is a static program (NDK clang `-O2`) running one or more of:
  `int` (branchy integer switch loop + hashing), `mem` (STREAM-like triad on
  3x8 MiB of doubles, auto-vectorised), `chase` (random pointer chase over
  64 MiB), `sys` (`getppid` loop), `pf` (mmap/touch/munmap 16 MiB),
  `simd` (auto-vectorised float FMA + int ops), `cm` (CoreMark, `-O2`).
  The same static binaries run natively inside the Android emulator for the
  baseline. Inferno no longer has the `virt` machine or GIC/PL011, so the
  Linux guest runs on **upstream QEMU 10.2.2** (Inferno's TCG core is
  `10.2.2_10.1.3_hybrid`; `tcg/`, `accel/tcg/` and `target/arm/tcg/` are the
  same design and largely the same code; Inferno differs in reformatting,
  its jump-cache/sysreg changes #19-#26 and a cheaper exit-request check).
* **`a13-test` microbenchmarks** (`hw/arm/tests/tcg_bench.S`, PR #27) on
  Inferno master, plus a new `w7` (vector FMLA/FMUL/FADD, register-only).
* Host: Mac mini M1 (macOS host build for profiling; the arm64 Android
  emulator "iop", 8 vCPUs, for timings, under the bench lock).

### Instruments

* **tbprof** (`analysis/tbprof` branch, not for merge): with `TBPROF=<file>`
  every TB starts with a 64-bit execution counter; `-d out_asm` additionally
  dumps the guest instruction words and the raw host code words of every
  guest-instruction chunk. `contrib/tbprof/tbprof.py` disassembles the host
  code with Capstone and classifies each host instruction:

  | category | what it is |
  |---|---|
  | `work` | everything that is not one of the below: the lowered guest operation, including flag computation, zero-extension, address arithmetic |
  | `mem` | the host load/store that performs the guest access |
  | `tlb` | the inline softmmu fast path (`ldp` mask/table, `and`, `add`, 2x`ldr`, alignment/page `and`, `cmp`, `b.ne`) |
  | `st_gpr` | `ldr`/`str` of `env->xregs[]` (GPR spill/reload) |
  | `st_flags` | `ldr`/`str` of `env->CF/VF/NF/ZF` |
  | `st_pc` | `ldr`/`str` of `env->pc` |
  | `st_vec` | `ldr`/`str` of `env->vfp.zregs[]` |
  | `helper` | `bl`/`blr` to a helper plus its address/env setup |
  | `glue` | TB prologue (exit-request check, `can_do_io`), `goto_tb`, `exit_tb`, `lookup_and_goto_ptr` jumps |

  Per TB it computes the *expected executed* host instructions over the TB's
  host CFG (TLB-miss and exit-request branches not taken, slow paths not
  executed, other conditional branches 50/50, unconditional branch ends the
  path), and weights by the TB's execution count. Boot is profiled
  separately and subtracted per (pc, guest words) so the workload rows are
  workload-only (`nokaslr norandmaps` so addresses match).
  This is an instruction count, not a cycle count.
* **macOS `sample`** on the QEMU process (1 ms, 8 s per workload), with
  `contrib/tbprof/sampself.py` computing *self* time of the vCPU thread per
  bucket: JIT code (`???` frames), softfloat, softmmu refill
  (`tlb_set_page`, page-table walk, `probe_access`), softmmu slow-path
  access, TB lookup / exec loop, translation, jump-cache/TB flush,
  exception/sysreg helpers.

## 2. Results

### 2.1 Time: where the vCPU thread is

Self time of the vCPU thread, upstream 10.2.2 on the M1 host, KASLR kernel
(default; KPTI on) for all rows, so `sys` and `pf` (a page fault is also an
exception entry) include KPTI's TLB flushes; steady state of each workload:

| workload | JIT code | softfloat | softmmu refill | TB lookup / exec loop | translate | TB/jmp-cache flush | other |
|---|---|---|---|---|---|---|---|
| cm (CoreMark) | **90.2%** | - | 0.4% | 8.9% | 0.1% | - | 0.4% |
| int | **69.8%** | - | 1.8% | 15.4% | 6.7% | 0.5% | 5.8% |
| chase | 49.6% | - | 28.1% | 5.9% | 8.1% | 0.6% | 7.7% |
| pf | 42.8% | - | 18.1% | 15.6% | 11.4% | 3.1% | 9.0% |
| mem (FP triad) | 11.8% | **79.4%** | 3.5% | 0.6% | 0.4% | - | 4.3% |
| simd | 31.3% | **62.2%** | 0.2% | 0.2% | 0.2% | - | 5.9% |
| sys (KPTI) | 3.5% | - | **46.6%** | 19.7% | 6.4% | **13.8%** | 10.0% |

### 2.2 Host instructions: what the generated code spends them on

Executed host instructions per category (Linux workloads are
boot-subtracted):

| workload | host/guest | work | mem | tlb | st_gpr | st_flags | st_pc | st_vec | helper | glue |
|---|---|---|---|---|---|---|---|---|---|---|
| Linux boot | 6.21 | 37.8% | 2.8% | 24.8% | 14.8% | 6.1% | 3.2% | 0.0% | 1.3% | 9.1% |
| cm | 8.13 | 35.1% | 2.6% | 22.9% | 12.2% | 6.9% | 5.4% | 0.0% | 0.1% | 14.8% |
| int | 8.62 | 48.2% | 0.0% | 0.0% | 10.6% | 12.6% | 8.3% | 0.0% | 0.0% | 20.3% |
| chase | 7.84 | 32.2% | 3.7% | 33.7% | 10.9% | 5.8% | 1.6% | 1.6% | 2.7% | 7.6% |
| pf | 7.73 | 38.1% | 2.0% | 17.6% | 13.2% | 7.7% | 4.1% | 0.0% | 3.5% | 13.7% |
| sys | 6.80 | 26.3% | 3.5% | 31.8% | 13.6% | 2.5% | 5.1% | 0.0% | 5.6% | 10.8% |
| mem | 11.48 | 29.2% | 4.4% | 39.4% | 6.5% | 3.2% | 1.0% | 8.6% | 3.4% | 4.4% |
| simd | 11.71 | 27.9% | 4.6% | 41.8% | 4.7% | 3.1% | 0.8% | 12.4% | 0.8% | 3.9% |
| a13 w1 (BL/RET) | 8.70 | 23.0% | - | - | 11.5% | 4.6% | 16.1% | - | 13.8% | 31.0% |
| a13 w2 (BLR/RET) | 7.00 | 40.5% | - | - | 22.6% | 4.8% | 9.5% | - | 4.8% | 17.9% |
| a13 w4 (ALU) | 2.10 | 83.2% | - | - | 5.6% | 5.6% | 1.4% | - | - | 4.2% |

By guest instruction class (CoreMark / Linux boot), host instructions per
guest instruction:

| class | share of guest insns | host/guest | of which |
|---|---|---|---|
| ALU | 54.5% / 68.1% | 6.56 / 4.32 | work 3.72/2.60, st_gpr 1.17/0.94, st_flags 1.02/0.55, glue 0.64/0.22 |
| load/store | 21.0% / 17.3% | 13.14 / 13.29 | tlb 8.85/8.89, st_gpr 1.57/1.46, work 1.18/1.68, mem 1.00 |
| branch | 24.4% / 11.4% | 5.34 / 5.76 | work 2.37/2.25, st_pc 1.81/1.66, glue 1.05/1.08 |
| vector load/store | (mem, simd: 24-27%) | 27.7 | two TLB lookups per 128-bit access (18), st_vec 4 |

A typical CoreMark-style TB makes the overheads concrete. Guest:

```
eor  w11, w11, w8 ; add x8, x8, #1 ; mul w11, w11, w12 ; cmp x10, x8 ; b.eq ...
```

5 instructions, 36 host instructions per execution: four `ldr` of
`xregs[]`, three `and #0xffffffff` (32-bit results zero-extended because
TCG globals are 64-bit), the `cmp` expanded into separate N, Z, C and V
computations (`sub`, `lsr`, `orr`, `cmp`+`cset`, `eor`/`eor`/`and`/`lsr`)
and four `str` to `env->NF/ZF/CF/VF`, two `str` of `xregs[]`,
`can_do_io` stores, the exit-request check, `ldr/add/str env->pc` (PC-relative
TBs) and the `goto_tb`. A same-ISA translation is 5 instructions plus about
3 of TB glue.

### 2.3 Register usage: can a pinned subset do it?

Dynamic GPR operand references (`contrib/tbprof/regfreq.py`):

| workload | refs/insn | top 8 regs cover | top 9 | top 16 | hottest |
|---|---|---|---|---|---|
| cm | 1.45 | 70.9% | 75.3% | 92.9% | x16 15.8%, x17 15.3%, x0 9.4%, x15 8.1%, x9, x14 |
| int | 1.02 | 100% | 100% | 100% | x13, x11, x8, x9, x10, x12 |
| sys (kernel) | 1.39 | 67.7% | 71.0% | 85.5% | sp 16.1%, x0 12.5%, x1, x30, x2, x19 |

NZCV is read by 24-30% and written by 16-27% of user-code instructions
(6%/5% in the kernel syscall path).

The hot set differs per workload: leaf user code lives in x8-x17 (including
IP0/IP1, which compilers use as ordinary temporaries), the kernel in sp,
x0-x3, x19 and x30. Inside TCG only callee-saved host registers survive
helper calls, i.e. at most 9 (x20-x28) could hold guest registers, and any
fixed choice of 9 covers 71-75% of references at best and much less for
the wrong workload. Pinning is only a big win if (nearly) **all** guest
registers live in host registers, which is possible only because guest and
host have the same register file.

### 2.4 Native vs TCG

Wall time of each workload inside the guest (`WL` lines), median of 2-3
runs. Native = the same static binary run directly in the Android emulator
on the same M1 (Hypervisor.framework guest, so near-native: stage-2
translation adds some cost to TLB-miss-heavy code). TCG = upstream 10.2.2 on
the macOS host, `nokaslr norandmaps` unless noted.

| workload | native | TCG | slowdown |
|---|---|---|---|
| int (8 reps) | 0.389 s | 1.36 s | **3.5x** |
| cm (CoreMark, 8000 it.) | 0.451 s | 2.00 s | **4.4x** |
| pf | 0.452 s | 3.17 s | 7.0x |
| chase | 0.820 s | 4.29 s (1.33 s with randomised maps) | 5.2x (1.6x) |
| sys (1.6M getppid) | 0.329 s | 4.25 s | 13x |
| sys, default KASLR kernel (KPTI on) | 0.329 s | 34.5 s | **105x** |
| simd (float32 FMA + int) | 0.322 s | 6.70 s | **21x** |
| mem (double triad) | 0.222 s | 6.18 s | **28x** |

`chase` is sensitive to the guest's address-space layout (transparent huge
pages change the guest page-walk depth QEMU pays on every TLB refill); the
two numbers bracket it.

Note the gap between instruction expansion and time: CoreMark executes 8.1x
the host instructions but is only 4.4x slower, because TCG's extra
instructions (L1-resident spills/reloads, independent flag computations)
retire at high IPC on a wide core. Instruction-count savings therefore
overstate time savings; native is the hard floor.

## 3. Prior art

* **QEMU TCG** itself: guest state lives in `CPUArchState`; TCG globals are
  cached in host registers only within a TB and are synced back before any
  op that can fault (`qemu_ld/st`, helpers that may raise) and at TB end
  (docs/devel/tcg.rst, "Translator Internals"). Condition codes are
  computed eagerly into separate N/Z/C/V globals for Arm. FP is softfloat
  (with a narrow "hardfloat" shortcut for some scalar float32/64 ops on
  some hosts, used only when the inexact flag is already set and rounding
  is nearest-even; it does not cover the vector helpers' per-lane loop
  overhead).
* **Apple Rosetta 2** (Dougall Johnson, "Why is Rosetta 2 fast?", 2022,
  https://dougallj.wordpress.com/2022/11/09/why-is-rosetta-2-fast/):
  ahead-of-time translation of whole text segments, one-to-one instruction
  translation with all x86 registers kept in fixed Arm registers (possible
  because x86-64 has half as many GPRs), state canonical at every
  instruction boundary (precise exceptions for free), hardware assists for
  the semantic gaps (TSO mode, FEAT_FlagM/FlagM2 and Apple's flag extensions
  for x86 flags), BL/RET kept as BL/RET for return prediction, and an
  "unused flags" optimisation. The lesson for us: on a fast core, 1:1
  translation with fixed register mapping and no inter-instruction
  optimisation is enough; the cost is in semantic gaps, and same-ISA has
  none for data-processing instructions.
* **FEX-Emu** (x86 on Arm64, user mode): static register allocation of x86
  GPRs/XMMs onto fixed host registers ("SRA",
  https://github.com/FEX-Emu/FEX/wiki/Register-Allocator), TSO emulated with
  LRCPC/atomics or hardware TSO where available
  (https://fex-emu.com/Scourge-of-emulation/), on-disk code caching.
  **Box64** (dynarec, x86-64 on Arm64): x86 registers mapped to fixed Arm64
  registers, "native flags" (x86 flags kept in Arm NZCV when the semantics
  line up, https://box86.org/2024/07/revisiting-the-dynarec/). Both are
  cross-ISA and user-mode, but they establish that fixed register mapping +
  native flags are what removes the per-instruction state traffic we
  measure; memory-model emulation, their biggest cost, does not exist for
  us (guest and host share the Arm memory model, barriers copy 1:1).
* **HQEMU** (Hong et al., CGO 2012, https://doi.org/10.1145/2259016.2259030):
  QEMU + LLVM trace optimisation on a second thread; hot traces get
  cross-block register allocation and optimisation. Effective for long
  running code, heavyweight (LLVM in the emulator), and it keeps softmmu.
* **Captive** (Spink, Wagstaff, Franke, USENIX ATC 2019,
  https://www.usenix.org/conference/atc19/presentation/spink): a
  system-level DBT running as a hypervisor guest, mapping guest virtual
  memory with real page tables so guest loads/stores need no software TLB.
  The "host-MMU-assisted fast path" idea in its strongest form; needs
  virtualisation, which an Android app does not have.
* **Cota et al., VEE 2019 (Qelt)**
  (http://www.cs.columbia.edu/~cota/pubs/cota_vee19.pdf): quantifies
  QEMU softmmu overhead (TLB hit latency on every access, refill cost) and
  scalable MTTCG translation.
* **Same-ISA translators.** VMware's binary translator (Adams & Agesen,
  ASPLOS 2006, "A Comparison of Software and Hardware Techniques for x86
  Virtualization") translated guest kernel x86 to x86: most instructions
  "IDENT" (copied verbatim), only privileged, control-flow and
  PC-relative instructions rewritten, while guest user code ran directly.
  **DynamoRIO** on AArch64 is a same-ISA DBT that copies application code
  and steals one register (x28 by default, holding its TLS pointer,
  https://dynamorio.org/page_aarch64_port.html), spilling the app's value
  for that register. **kqemu** (QEMU's old accelerator) and **VirtualBox
  raw mode** ran guest user code natively and used recompilation as a
  fallback, the ancestors of option (b) below. No published system-level
  same-ISA DBT for AArch64 with a software MMU is known to us.
* **Persistent code caches**: Reddi et al., "Persistent Code Caching", CGO
  2007 (Pin); Wang et al., "A General Persistent Code Caching Framework for
  Dynamic Binary Translation", USENIX ATC 2016 (QEMU-based).

## 4. Design

### 4.1 Phase 1: native FP/SIMD (PoC done, behind a flag)

Same ISA means the host FPU computes exactly what the guest's FPU would,
provided the controls match: FPCR.{RMode, FZ, FZ16, DN, AH, FIZ, NEP} and
trap enables. QEMU keeps the guest FPCR in `float_status` objects (one per
FP context, see `target/arm/tcg/vfp_helper.c`); the host FPCR is all-zero
while QEMU runs.

PoC (`poc/native-fp`, `target/arm/tcg/vec_helper.c`): when
`QEMU_TCG_NATIVE_FP=1`, the host is AArch64 and its FPCR is 0, the gvec
helpers for vector FADD/FSUB/FMUL/FMLA/FMLS (`.s`, `.d`) check that the
`float_status` is the matching mode (round-to-nearest-even, no FZ/FIZ, no
default-NaN, the FPCR.AH=0 NaN-propagation and tininess-before-rounding
rules) and then execute the host instruction per element (inline asm, so
the compiler cannot commute operands and change NaN propagation), clearing
the host FPSR before and folding its cumulative flags into the guest
`float_status` after. Anything else falls back to softfloat.

Full version:

1. New TCG vector ops for FP arithmetic (`fadd_vec`, `fmul_vec`,
   `fma_vec`, ...), implemented only by `tcg/aarch64` and only advertised
   when host == guest == AArch64. `target/arm/tcg/translate-a64.c` uses them
   instead of the gvec helpers when the TB was translated for the
   native-compatible FP mode.
2. The FP mode becomes part of the TB key: one hflags bit, "FPCR is
   native-compatible", recomputed where FPCR is written (`vfp_set_fpcr`)
   and on exception entry/return. TBs translated with the bit set may emit
   native FP; others use softfloat as today. This avoids per-op checks.
3. FPSR: the host FPSR accumulates the guest's cumulative flags while in
   generated code. Clear it at code-cache entry (`tcg_qemu_tb_exec`
   prologue), and fold it into the guest `vfp.fpsr`/`float_status` on exit
   and before any helper that reads or writes FP status (MRS FPSR, scalar
   softfloat helpers). QEMU's own host FP use is in C between TBs, so the
   epilogue fold keeps it from leaking into the guest.
4. Non-default FPCR (FZ, DN, directed rounding, AH=1) keeps softfloat at
   first; later, write the guest FPCR into the host FPCR at code-cache
   entry and restore it at exit (and around helpers), if `MSR FPCR` turns
   out cheap enough on the target cores.
5. Coverage order by profile weight: vector FMLA/FMUL/FADD/FSUB (s/d), then
   scalar `fmadd`/`fadd`/`fmul`/`fdiv`/`fsqrt`, conversions (`fcvtzs`,
   `scvtf`, `fcvt`), compares (`fcmp` sets NZCV), FP16 and BF16 only if the
   host has them (FEAT_FP16 on all ARMv8.2+ phone cores).

Risks: host features must be a superset of the guest model's FP features;
FPCR.AH (FEAT_AFP) and FEAT_EBF16 change semantics and must be excluded or
mirrored; trapped FP exceptions (FPCR.IxE) are optional in the architecture
and unimplemented on Apple and most Arm cores, so a guest enabling them must
stay on softfloat; denormal/FZ behaviour is exactly FPCR-controlled, so
mirroring FPCR is sufficient.

### 4.2 Phase 2: a same-ISA translation tier for integer code ("SIT")

Not a TCG backend tweak: TCG's model (state in env, IR temps, a register
allocator that may use any host register and clobbers NZCV freely) is what
produces the overheads above. SIT is a second, much simpler code generator
that shares everything around code generation with TCG:

* **Same TB structure and management**: `tb_gen_code` tries SIT first and
  falls back to the TCG front end for the TB if SIT declines any
  instruction (unsupported, singlestep/icount/plugins/gdbstub,
  watchpoints). TB lookup, jump cache, chaining (`goto_tb` patching),
  invalidation on code writes (page `PAGE_WRITE` protection / `tb_invalidate_phys_*`),
  `cpu_exec` loop, interrupts via the exit-request check: all unchanged.
* **Register map** (identity where possible):
  * guest x0-x15, x19-x27, x29 live in the same-numbered host registers
    for the whole time the vCPU is in generated code;
  * guest x16, x17, x18, x28, x30 and SP live in `env` slots unless a TB
    "claims" them: per TB, SIT picks the scratch registers it needs among
    host x16/x17/x30 (x30 is needed for `bl` to helpers) and, if the TB
    uses guest x16/x17, saves/restores those around the rewritten
    sequences that need scratch (CoreMark shows x16/x17 are 31% of
    user-code GPR references, so they must not be permanently evicted);
  * host x18 is never used (platform register on macOS; shadow call stack
    on Android), host x28 holds `env`, host sp stays the host stack (a
    guest SP must never be in host sp: async host signals would write
    below it);
  * guest NZCV lives in host NZCV.
  Entry (`tcg_qemu_tb_exec` prologue) loads the map from `env`, exit
  (epilogue, `exit_tb`) stores it back, so C code between TBs sees a normal
  `CPUARMState`. TBs chain to each other with the registers live.
* **Verbatim copy** for a whitelist of fully specified data-processing
  encodings whose register operands are all identity-mapped: ADD/SUB/logic
  (shifted/extended register and immediate forms), MOV*, bitfield,
  extract, shifts, MADD/MSUB/SMULH/UMULH, divides, CLZ/CLS/RBIT/REV,
  CSEL family, CCMP/CCMN, ADC/SBC, flag-setting forms; and (with phase 1)
  FP/SIMD register-to-register arithmetic. Decoding uses the existing
  decodetree tables; CONSTRAINED UNPREDICTABLE encodings are never copied.
  An instruction that names a non-identity register is rewritten with a
  load/store of its `env` slot around it (cheap, and rare outside
  loads/stores where it is folded into the address computation).
* **Rewritten instructions**:
  * *Loads/stores*: address computation from the guest registers, then the
    existing softmmu fast path, **rewritten to be flag-free** (`eor` +
    `cbnz` instead of `cmp` + `b.ne`) so guest NZCV in host NZCV survives,
    then the host access with the guest's own size/sign/ordering
    (LDAR/STLR/LDAPR keep acquire/release semantics because the host
    instruction is the same; writeback forms update the guest base
    register). Pair and 128-bit accesses do one TLB lookup when they cannot
    cross a page (alignment known from the address mod 16) instead of two.
    The out-of-line slow path stores all identity-mapped registers + NZCV to
    `env` before calling the helper (this is what makes the faulting state
    precise) and reloads after. Exclusives (LDXR/STXR) and LSE atomics keep
    TCG's current cmpxchg-based semantics at first.
  * *Branches*: B/BL/B.cond/CBZ/TBZ become the host branch to a `goto_tb`
    exit (direct, patched for chaining); BL also writes the guest return
    address to guest x30's slot. BR/BLR/RET go through the jump cache
    (`lookup_and_goto_ptr`), with a small inline cache of the last target as
    Rosetta/Dolphin do for returns.
  * *System registers, exception-generating and hint instructions*
    (MRS/MSR, SYS, SVC/HVC/SMC, ERET, WFI, PAC*, BTI handling, DC/IC/TLBI,
    MRS NZCV/FPSR): exit SIT for that instruction, i.e. sync state and call
    the same helpers the TCG front end uses, or end the TB.
  * *PC-relative*: ADR/ADRP and literal loads materialise the *guest* PC.
* **State reconstruction**: per guest instruction SIT records the host
  offset (as `insn_start` does), so `cpu_restore_state` finds the guest PC;
  because every rewritten sequence leaves guest registers in their
  canonical place before it can fault, the register state is precise at
  every instruction boundary (Rosetta's invariant).

What SIT keeps from today: the softmmu TLB fast path (~7 instructions per
access), TB entry checks, and helper calls for system work. What it drops:
GPR/flag/PC state traffic, flag recomputation, 32-bit zero-extension
fix-ups, IR lowering inflation.

### 4.3 How softmmu stays correct

SIT does not change the guest memory model or the TLB: every guest access
still goes through `CPUTLB` (so MMIO, watchpoints, dirty tracking, TB
invalidation on writes to code pages and page permissions behave as today);
only the instruction sequence around it changes (flag-free compare,
registers in place). The invariants that must hold:

1. Before any call that can longjmp (`cpu_loop_exit`) the guest state is in
   `env` (slow paths and helpers spill the identity map + NZCV first).
2. Nothing between a guest access and its TLB check writes guest-visible
   registers.
3. `TLB_*` flag bits in the comparator (MMIO, watchpoint, notdirty) keep
   forcing the slow path exactly as now.
4. Self-modifying code: unchanged mechanism (write-protected code pages,
   `tb_invalidate_phys_range`, `CF_INVALID`); a store that invalidates the
   current TB still exits through the slow path.

### 4.4 Correctness risks (honest list)

* **Feature mismatch.** Verbatim copy is only valid for instructions the
  *host* implements with the guest model's semantics. Guest A13 (ARMv8.4)
  on a v9 phone core is fine for integer instructions; guest features the
  host lacks (or vice versa, features the guest model must *not* have, e.g.
  the A13 model has no BTI/MTE/SVE) must be filtered at decode. PAC: guest
  keys are not the host's (an EL0 host process cannot set keys), so PAC
  instructions are always rewritten (a `-cpu max` Linux guest signs every
  function return; that path costs as much as today).
* **Flags across rewritten code.** Every host sequence SIT emits must
  preserve NZCV or save/restore it (`mrs/msr nzcv`); helpers clobber it.
* **Host register discipline.** Code called from generated code (helpers,
  slow paths) follows the host ABI: x0-x17 and NZCV are clobbered, so every
  call site spills/reloads them (cold paths only, but `lookup_and_goto_ptr`
  is hot for BLR/RET-heavy code; give it a register-preserving assembly
  fast path).
* **Memory ordering.** Same Arm memory model: barriers and acquire/release
  forms copy 1:1; the extra TLB loads are ordinary loads to QEMU-private
  memory. MTTCG's existing rules for exclusives and atomics are kept.
* **Precise exceptions and debug.** Instruction-boundary state is canonical
  except for registers held in host registers, which slow paths spill
  first. Singlestep, icount, gdbstub breakpoints and plugins disable SIT
  (TCG fallback), so they keep today's behaviour.
* **Asynchronous host signals** must not see guest values in host
  sp/x18/x28 (never mapped) and must not run C code that reads `env` while
  the vCPU is in generated code (QEMU's signal handlers already only set
  flags).
* **Double maintenance.** SIT is a second code generator; keep it
  deliberately small (whitelist + fallback), test it differentially against
  TCG (same guest program, compare `CPUARMState` at TB exits under a debug
  option), and keep the fallback always available.

### 4.5 Phase 3: ASID-tagged TLB

Orthogonal but large for KPTI guests (and anything that switches address
spaces often): key `CPUTLBEntry` comparators with the ASID (or keep a few
TLB sets per ASID) instead of `tlb_flush` on TTBR/ASID changes. Our `sys`
workload: 22 us per syscall with KPTI vs 0.9 us without; 47% softmmu refill
+ 14% jump-cache flush.

## 5. Expected speedup (from the measurements)

Instruction-count model (time in JIT code assumed proportional to executed
host instructions, which overstates the gain: spills and TLB loads are
cheap L1 hits on a wide core):

| workload | host/guest today | SIT model | JIT share of time | bound on speedup |
|---|---|---|---|---|
| cm | 8.13 | ~3.0 (ALU 1, branch ~2, ld/st ~8, +TB glue) | 90% | ≤2.3x (native floor: 4.4x) |
| int | 8.62 | ~2.0 | 70% | ≤2.2x (native floor: 3.5x) |
| boot/kernel-ish | 6.21 | ~2.8 | (not sampled) | ≤2.2x |
| mem / simd (FP) | - | phase 1 removes softfloat (79% / 62% of time) | 12% / 31% | helper-level PoC measured 1.8x / 1.1x; inline FP ops bounded at ≤4.8x / ≤2.6x |
| sys (KPTI) | - | unchanged by SIT; phase 3 | 3.5% | ASID-tagged TLB: up to ~8x on syscalls (21.5 us -> the 2.6 us no-KPTI cost) |

The model: ALU 1 host instruction, branch ~2 (branch + chained `b`), load/
store ~8 (flag-free TLB check 7 + access), plus 2-3 instructions of TB entry
glue per TB; time outside the code cache (TB lookup, translation, refills)
unchanged. Because TCG's overhead instructions retire quickly (CoreMark: 8.1x
the instructions in 4.4x the time, i.e. ~1.8x the native code's
instructions per second), a realistic expectation for integer code is nearer 1.5-2x than the bound.

Register pinning *inside TCG* (9 callee-saved host registers, synced at
helper calls and TB exits) would remove at most the `st_gpr` share for the
pinned registers (11-15% of host instructions x 71-75% coverage) and none of
the flag, PC or lowering overhead: **≤10%**. It is not worth doing on its own.

## 6. Two other near-native designs, costed

**(b) Guest EL0 natively in the host process, trap back to an emulated
kernel** (kqemu / VirtualBox raw-mode style). Guest user code would run at
full native speed (this is the only design that also removes the softmmu
cost for user code: CoreMark 4-5x, FP code 10-20x, page-touching code
benefits most). Cost: the guest's user address space must be mapped at its
own virtual addresses inside the host process (guest VAs collide with the
host's own mappings: needs a reserved region and a guest kernel that places
user mappings there, or a separate host process per guest address space
with shared memory); every guest PTE change must become `mmap/mprotect`
(shadow paging; fork/exec/page-fault-heavy guests pay a host syscall per
PTE update); guest syscalls must be caught: Syscall User Dispatch is
x86-only upstream (arm64 patches were posted, not merged), so it would be a
seccomp filter keyed on `seccomp_data.instruction_pointer` returning
`SECCOMP_RET_TRAP` (SIGSYS per guest syscall, ~2-5 us); faults arrive as
SIGSEGV; timer interrupts as signals; EL0-visible system registers leak the
host's (TPIDRRO_EL0, CNTVCT_EL0 offset, CTR/DCZID), and TPIDR_EL0 must be
swapped at every transition because the host libc uses it. For an iOS guest
the 16K/4K page-size mismatch and XNU's use of TPIDRRO_EL0 add more. Large
engineering cost, high correctness risk, largest payoff for user code.

**(c) Persistent / AOT translation cache keyed by guest-page hash.** Saves
translation time and cold-start: translation is 0.1% (CoreMark) to 11%
(page-fault workload) of vCPU time here, more during boot and app launch.
TCG code is not position-independent (absolute helper addresses in literal
pools, `goto_tb` patch sites, `jmp_target` addresses, constant pools with
host pointers), so a cache needs relocatable code generation (helpers via
an `env`-relative table, PC-relative everything) plus validation (page
hash, TB flags/cflags, CPU model and QEMU build id). Moderate engineering,
low correctness risk, modest steady-state gain; worth it for boot/launch
latency once code generation is fast (it compounds with SIT, whose
translation is cheaper anyway).

## 7. Proof of concept: native vector FP

Branch `poc/native-fp` (3 commits on master 339be2c, not for merge):
`target/arm/tcg/vec_helper.c` as described in 4.1, `hw/arm/tests/tcg_bench.S`
workload `w7` (register-only vector FMLA/FMUL/FADD on `.4s` and `.2d`,
48M FP instructions), and `hw/arm/tests/fp_native_check.S`.

**Correctness.** `fp_native_check` runs FADD/FSUB/FMUL/FMLA/FMLS (`.2d`, `.4s`)
over all 8000 (a, b, c) triples of 20 special operands per precision (signed
zeros, smallest/largest denormal, smallest normal, max, infinities, quiet and
signalling NaNs with payloads, ordinary values) and hashes the result bits
plus FPSR after each operation. Same hash with and without
`QEMU_TCG_NATIVE_FP=1`, on the macOS host and in the Android emulator:
`59664714ecfd4d49` (FPSR cleared before each op) and `e8f02ac4a115c77f`
(`-DSTICKY`, flags accumulate). Negative control: swapping the operands of
the native FADD.2d (which changes NaN propagation) changes both hashes. The
existing `run-a13-tests.py` suite passes on the PoC build.

**Performance**, Android emulator "iop" (8 vCPU, M1), CI `qemu-android-arm64`
artifact of the branch, same binary with the variable off/on, 5 interleaved
runs each, whole-process time (includes ~0.1 s start-up):

| workload | `QEMU_TCG_NATIVE_FP=0` | `=1` | change |
|---|---|---|---|
| w4 (ALU control) | 2.80 s [2.79-2.83] | 2.79 s [2.77-2.80] | -0.4% (noise) |
| w7 (vector FP) | 3.68 s [3.62-3.73] | 1.27 s [1.27-1.29] | **-65% (2.9x)** |

Linux guest on the macOS host (upstream 10.2.2 + the same patch, 3 runs
each):

| workload | off | on | change |
|---|---|---|---|
| mem (double triad, exact values) | 6.18 s | 3.41 s | **1.8x** |
| simd (float32 FMA, inexact) | 6.78 s | 6.16 s | 1.10x |

The first PoC version cleared FPSR (`MSR FPSR`) on every helper call; that
made `simd` 1.7x *slower* (6.7 s -> 11.6 s), because writing FPSR is
expensive on M1 and softfloat's hardfloat path was already handling the
inexact float32 case. The second version reads FPSR around the operation
and writes it only when it holds a flag the guest does not already have.
Lessons for the real implementation: keep FPSR/FPCR writes out of the hot
path entirely (fold flags at code-cache exit), and remove the helper call
and per-lane loop by emitting host vector FP instructions inline; the
helper-level PoC only shows the semantics are exactly right and where the
remaining time goes.

## 8. Reproducing

```
# analysis build (branch analysis/tbprof)
TBPROF=w.cnt qemu-system-aarch64 ... -d out_asm -D w.log
python3 -I contrib/tbprof/tbprof.py w.log w.cnt [--minus boot.log boot.cnt]
python3 -I contrib/tbprof/regfreq.py w.log w.cnt [--minus ...]
sample <qemu pid> 8 -file w.sample; python3 -I contrib/tbprof/sampself.py w.sample
# PoC (branch poc/native-fp)
QEMU_TCG_NATIVE_FP=1 qemu-system-aarch64 -M a13-test -display none -kernel w7.bin
```

The guest workload source (`wl.c`) and the initramfs builder are in
`contrib/tbprof/` on the analysis branch.
