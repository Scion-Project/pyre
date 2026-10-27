# Pyre ![CI](https://img.shields.io/github/actions/workflow/status/Scion-Project/pyre/build.yaml?style=for-the-badge)

Apple-silicon device emulation for Android phones. Pyre is the engine behind
[Scion](https://github.com/Scion-Project/scion): an emulated iPhone 11
(t8030, A13) running on a Google Pixel 10, with an A14 (t8101) machine in progress.

It is a fork of [ChefKiss Inferno](https://github.com/ChefKissInc/Inferno), itself a
heavily modified QEMU. Upstream's machine models, boot flow and kernel patch set are
the foundation; everything below is what Pyre adds on top, developed in the open with
one PR per change and numbers attached.

## What Pyre adds

**Faster CPU emulation (TCG), measured on the iOS 14.8 restore boot**

| Change | Where | Effect |
|---|---|---|
| Jump-cache fast path, storage-only sysreg writes no longer end a TB, PC-relative TB invalidation clears 64 slots not 16k | `accel/tcg`, `hw/arm/a13_gxf.c` | hot returns −29%, ~⅔ of 496k TB exits per boot gone |
| GXF-banked EL1 registers as plain fields | `hw/arm/a13_gxf.c` | TPIDR_EL1 read 24 ns → <1 ns |
| Guest floating point on the host FPU, inline from the arm64 backend, bit-exact with a 426 768-case startup self-test and softfloat fallback | `target/arm/tcg`, `tcg/aarch64`, `docs/NATIVE-FP.md` | vector FP microbench 46×, Linux `mem` 3.7×, `simd` 2.1× |
| ASID-tagged TLB contexts (TLBs and jump caches kept across address-space switches) | `accel/tcg/cputlb.c`, `docs/ASID-TLB.md` | KPTI syscall loop 6.1×; `-accel tcg,asid-tlbs=0` restores old behaviour |
| Softmmu fast path 10 → 8 host instructions; TLB floor 64 → 1024 entries; cheaper dirty reset | `tcg/aarch64`, `docs/SOFTMMU.md` | boot −5%, host CPU −7% |
| Placeholder SEP ROMs park the SEP core instead of spinning | `hw/arm/sep/rom-idle.c` | idle host CPU 0.7 → 0.07 cores |

Together: restore boot 19.75 s → 13.1 s and host CPU 21 → 11 s per boot on the
reference emulator, with iOS 14 behaviour unchanged. Method and tables:
[scion/docs/PERF.md](https://github.com/Scion-Project/scion/blob/main/docs/PERF.md).

**Reliability.** A kernel pattern patch fixes a ~3% boot-time panic caused by a race
in the display driver's simulator check (`hw/arm/kernel_patches.c`).

**Testing without Apple software.** `-M a13-test` is a firmware-free A13 bench:
bare-metal register tests, GXF/SPTM register round-trips, TCG microbenchmarks
(`hw/arm/tests/`, `run-a13-tests.py`, `run-a13-bench.py`), plus host unit tests
(`run-host-tests.sh`). CI runs them on Linux and macOS, and cross-builds the Android
library on every PR.

**Work in progress, each behind an opt-in switch**

- **iOS 27 / SPTM+TXM**: the SPTM/GXF register set, SPTM and TXM loading and the
  iBoot-style image layout SPTM checks (`-M t8030,sptm=…,txm=…`). Status and gaps:
  [scion/docs/IOS27.md](https://github.com/Scion-Project/scion/blob/main/docs/IOS27.md).
- **GPU (A13 "G12P")**: milestone 0 scaffold, `-M t8030,gpu=on` — the guest's GPU
  driver matches and starts against an emulated coprocessor; firmware handshake in
  progress. Plan: `docs/GPU-EMULATION-SCOPE.md`.
- **A14 / t8101 (iPhone 12)**: per-SoC constant tables and the new machine, kernel to
  serial first. Plan: `docs/T8101-SCOPE.md`.
- **Near-native direction**: `docs/SAME-ISA-FASTPATH.md` quantifies where a same-ISA
  (arm64-on-arm64) translation tier would land and what it costs.

## Building for Android

Pyre is consumed as the `third_party/inferno` submodule of Scion, whose
`native/build-android.sh` cross-compiles it and its static dependencies with the NDK.
Host builds use the usual `meson setup build && ninja -C build` (see upstream's
documentation for machine options).

## Ground rules

Same as Scion's [ETHICS.md](https://github.com/Scion-Project/scion/blob/main/docs/ETHICS.md):
no Apple firmware, keys, images or instructions for obtaining them live here; only
structural facts needed to emulate hardware. Guest security monitors keep running and
enforcing; the emulator implements the hardware, it does not change the guest.

## Licence and attribution

Pyre is a derivative of ChefKiss Inferno and keeps its licensing: GPL-3.0 for the
QEMU-derived parts, AGPL-3.0 for Inferno's own code, and Pyre's additions are
AGPL-3.0 too. See `LICENSE`. The “ChefKiss Inferno” name and artwork are theirs;
their branding notice is honoured (`ui/icons/CKBrandingNotice.md`) and the name
is used here only to credit upstream. Code brought in from other GPL projects keeps
its original copyright headers. QEMU's own licensing is unchanged.
