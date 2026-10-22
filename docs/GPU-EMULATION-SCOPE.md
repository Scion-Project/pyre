# Apple A13 GPU (AGX) emulation in `t8030`: scoping study

Status: **research only**. Nothing here is implemented yet. This document scopes
how the `t8030` machine could emulate the iPhone 11's GPU well enough that the
guest's **own, unmodified** GPU kernel driver and Metal userspace run against
it, with the rendering done on the host GPU: the Pixel 10's Tensor G5 GPU
through Vulkan, and the Mac mini through Metal (via MoltenVK) for development.

This is the way console emulators handle GPUs (RPCS3's RSX, yuzu/Ryujinx's
Maxwell, Dolphin's Flipper, Xenia's Xenos, Vita3K's SGX543). The guest isn't
touched. The emulator implements the hardware interface that the guest driver
programs, and translates the work it submits into host GPU work.

Ethics (see `ios-on-pixel/docs/ETHICS.md`): no Apple binaries, firmware,
kernelcaches, device trees, shader binaries or captures go into any repository.
This document contains only structural facts and pointers to public
reverse-engineering. Captures made from a user's own guest stay on that user's
machine.

> **Naming caveat.** The workstream was framed as "G11P". Public kext names
> (`AGXAcceleratorG11P_B0`, `AGXFirmwareKextG11PRTBuddy` were already seen on
> devices in mid-2019, before the A13 shipped, and an `AGXG12P` kext exists)
> suggest A12 = G11P and **A13 = G12P**. Confirm this from the kext list of your
> own kernelcache (`ipsw kernel kexts …`) before anything is named in code. This
> document says "G12P (A13)" from here on. Everything below applies to either
> name.

---

## 1. What `t8030` does for the GPU today

**Nothing.** There's no GPU model of any kind:

* `hw/arm/boot.c` has `gfx-asc` in `REM_NAMES`, so the GPU's coprocessor node
  is **deleted from the device tree** before boot. With no `gfx-asc`, the AGX
  kexts never match or start. The `sgx` node (the GPU proper) isn't created as
  a device. Its MMIO ranges aren't mapped (reads go to the unimplemented-device
  or PMGR catch-alls).
* `hw/arm/t8030.c` has no `sgx`/`gfx` creation function. The AMCC model
  programs the `AMCC_NON_PLANE_GFX` hash registers only as part of memory
  controller setup.
* The guest runs with **software rendering** turned on by filesystem patches
  (ios-on-pixel `docs/ROADMAP.md` §3). CoreAnimation/backboardd composite on
  the emulated CPU into IOSurfaces. `disp0` (`hw/display/apple_displaypipe_v4.c`)
  scans those out. The display pipe accepts only linear ARGB/BGRA and **drops
  compressed frames** (`GP_PIXEL_FORMAT_COMPRESSED` → "dropping frame as it's
  compressed"). That matters once a GPU produces scanout surfaces (§5, risk R6).

### Building blocks that already exist

| Need | Existing Inferno code | Fit |
|---|---|---|
| ASC/IOP mailbox + RTKit protocol | `hw/misc/a7iop/` (`core.c`, `regs-v2.c`/`regs-v4.c`, `rtkit.c`, `mailbox/`) | Direct. `apple_rtkit_init()` + `apple_rtkit_register_user_ep()` is how AOP, SMC, SIO and ANS are modelled. **Every** RTKit IOP except SEP is high-level emulated (HLE): QEMU plays the firmware's role and no firmware runs. `gfx-asc` should follow the same pattern. |
| IOP carveouts / segment props | `t8030_rtkit_mem_setup()` / `t8030_rtkit_seg_prop_setup()` | Direct, for any `segment-ranges` the GPU kexts expect. |
| DMA translation | `hw/arm/dart.c`, `hw/arm/sart.c` | **Not used by AGX.** The GPU has its own MMU (UAT). See below. |
| Scanout | `apple_displaypipe_v4.c` + `dart-disp0` | Reused unchanged for linear surfaces. Needs a decompressor for compressed ones. |
| Bare-metal CI machine | `hw/arm/a13-test.c` (`-M a13-test`) | Good home for synthetic GPU tests that need no Apple code (§3.5). |

### What the guest GPU stack expects at boot

These are structural facts from public RE: the Asahi Linux docs/m1n1 for G13
on Macs, and general iOS kext structure. Check each against your own device
tree and kernelcache.

1. **Device tree.** `arm-io/sgx` (GPU: MMIO `reg`, interrupts, DVFS/perf-state
   tables, core/cluster counts, power-domain properties) and `arm-io/gfx-asc`
   (ASC wrapper, with the RTKit `iop-gfx-nub`). Removing `gfx-asc` from
   `REM_NAMES` and leaving `sgx` intact is step zero.
2. **Kexts.** `AGXFirmwareKextG12PRTBuddy` (RTBuddy: starts the ASC and runs
   the RTKit management handshake), `AGXG12P` / `AGXAcceleratorG12P*` (the
   accelerator, an `IOGPU`-family driver), and `IOGPUFamily`/`IOSurface`
   underneath. On iOS the GPU firmware image travels in the kernelcache (the
   RTBuddy kext loads it), not via iBoot as on Apple-silicon Macs. **Under HLE
   the image is never executed.** The model accepts the load and answers as the
   firmware would.
3. **RTKit handshake.** HELLO / EPMAP / rollcall / power state on the
   management endpoint, crashlog/syslog/ioreport endpoints, then
   GPU-specific endpoints. On G13 Asahi saw very little mailbox traffic: one
   init message carrying a pointer to the **initdata** structure, then
   "kick"/doorbell messages and a "pong"/firmware-ctl endpoint. Everything else
   goes through shared memory.
4. **UAT (GPU MMU).** ARM64-format page tables with a 16 KiB granule and 40-bit
   GPU VAs, kernel/user split. A fixed page holds up to 16 context TTBR pairs.
   The ASC uses the kernel half as its own CPU page table. The driver also
   expects the firmware to map MMIO regions it lists in initdata. The emulator
   needs a **UAT walker** (stage-1 ARM64 walk over guest PA, with a TLB and
   invalidation driven by the driver's TLBI handshake).
5. **initdata.** A large nested structure with: channel ring control/data
   pointers, DVFS/power tables, colour-space coefficients, the firmware MMIO
   mapping list, UAT layout, and many opaque buffers. Its layout is
   **versioned per firmware build** (drm/asahi keeps separate layouts for each
   macOS firmware version it supports).
6. **Channels.** CPU→GPU: per-priority work channels for TA (vertex/tiling),
   3D (fragment) and CP (compute), plus DeviceControl. GPU→CPU: Event
   (128-bit fired-stamp bitmaps), FWLog, Fault, Statistics and a few unknown
   ones. Then come contexts → work queues → work items (each with a
   **microsequence** of firmware ops: start/finish TA, 3D, CP, timestamps,
   wait-for-stamp barriers) → stamps/events back to the driver. Tiler heap and
   buffer management is shared between driver and firmware.
7. **Direct MMIO from the driver.** The accelerator also reads GPU ID/config
   registers and dumps fault registers itself. These need plausible values.
8. **Watchdogs.** The driver times out work and triggers GPU recovery/panic if
   stamps don't advance. Slow emulation must not trip these (R3).

---

## 2. Public documentation and how far it transfers to G12P

| Source | What it covers | Transfer to A13 (G12P) |
|---|---|---|
| **Asahi Linux docs**, "Apple GPU (AGX)" (Lina's notes) | Kernel-side model: UAT, initdata, channels, contexts, work queues, microsequences, tiler heaps, stamps, TA/3D walkthrough | **Concepts: high.** AGX has kept this split since its PowerVR-derived origins. **Byte layouts: low.** They're per firmware version and per generation. |
| **m1n1** `proxyclient/m1n1/fw/agx/*.py` (initdata, channels, cmdqueue, microsequence) + `trace/agx.py` | Executable struct definitions and a **hypervisor tracer** that decodes live driver↔firmware traffic | Method: very high. Inferno is effectively a hypervisor too, and the same trace-and-decode loop applies. Struct layouts: they're a starting skeleton for G12P, not a spec. |
| **Linux `drm/asahi`** (Rust; Asahi kernel tree, being upstreamed) | A complete working firmware-interface client for G13/G14 (incl. G14X), with versioned struct macros | Gives the shape of a versioned layout table and the init ordering. We implement the **other side** of the same interface. |
| **Mesa `src/asahi`**: `compiler/` (NIR→AGX), `genxml/cmdbuf.xml` (VDM/CDM/PPP/USC control-word packing), `lib/decode.c` (agxdecode), `layout/` (twiddled + compressed image layouts), `libagx`/`clc`, `vulkan/` (Honeykrisp) | Userspace: shader ISA encoding, control streams, image layouts, fixed-function state | **G13+ only.** No device in Mesa is older than G13. Expect the A13 control streams and ISA to be **related but not identical**. agxdecode and cmdbuf.xml are the best starting point for a G12P decoder. |
| **dougallj/applegpu** | G13 ISA docs, disassembler, assembler and a **Python ISA simulator** checked against hardware | Same caveat (G13). The simulator is the model for our M1 interpreter. The G12P encoding has to be confirmed by experiment. |
| Alyssa Rosenzweig's and Asahi Lina's blog series | Tile-based deferred rendering details: tile buffers, background/end-of-tile programs, blending done in shader, vertex fetch done in shader, tiler-heap overflow ("partial renders") | High on architecture. These features predate G13. |

**Bottom line.** For G12P, the *architecture* is publicly documented through
its G13 descendant: firmware-mediated submission, TBDR, programmable
blending/fetch, UAT. The *encodings* are not: initdata/microsequence layouts,
control-stream words and the shader ISA. Those must be recovered for G12P.
That can be done **entirely inside the emulator**, without hardware, once M0
works. The guest's own Metal compiler then produces shader binaries and
command streams for any MSL we feed it, which allows differential RE:
compile known shaders, diff the output, record the facts. Only the facts go
into the repo, never the captures.

---

## 3. Architecture and staged plan

### 3.1 Approach: HLE of the firmware, not LLE

* **HLE (recommended).** QEMU implements `gfx-asc` as an RTKit IOP (as AOP,
  SMC, SIO and ANS already are) and consumes initdata, channels and work
  queues directly. The GPU firmware never runs. This is the same boundary
  drm/asahi programs against, so it's the **best-documented** interface on
  the GPU path.
* **LLE (rejected as the main path).** This would run the real GPU firmware on
  an emulated ASC core, the way SEP runs. The handshake would come for free,
  but the firmware drives the GPU's *internal* MMIO and command processors.
  That interface is completely undocumented, and we'd still have to emulate
  everything below it. It might be useful later as a one-off **oracle** for
  layouts that are hard to infer. It is not the product.

### 3.2 Proposed code layout (Inferno fork)

```
hw/gpu/apple-agx/            (new; AGPL-3.0 like other Inferno-authored code)
  agx.c            sgx MMIO stubs (ID/config/power/fault regs), IRQs, DT glue
  agx-asc.c        gfx-asc RTKit HLE: endpoints, doorbells, initdata parse
  agx-uat.c        UAT walker + TLB + invalidation
  agx-fw-<build>.c versioned layout tables (one per supported guest build)
  agx-queue.c      channels, contexts, work queues, microsequence executor
                   (dedicated thread, not under the BQL)
  agx-cs.c         VDM / PPP / CDM control-stream decoder (agxdecode-style)
  agx-isa.c        USC ISA decoder + interpreter            (M1)
  agx-raster.c     reference software tiler/rasteriser/ROP  (M1)
  agx-spirv.c      ISA → IR → SPIR-V                        (M2)
  agx-vk/          Vulkan backend, optional meson feature   (M3)
  agx-trace.c      trace-events + capture/replay format (captures never committed)
```

Hook-up in `t8030.c`: `t8030_create_gpu()`, plus dropping `gfx-asc` from
`REM_NAMES` behind a machine property (`-M t8030,gpu=on`; default off until
M1). Software rendering stays the default path until the GPU path matches it.

### 3.3 Milestones

Each milestone has a measurable exit criterion. Target **one pinned guest
build** first: the iOS 14 build the project already boots to SpringBoard. The
firmware interface changes between builds, so later builds become added
layout tables, not a rewrite.

**M0. The driver loads and the firmware handshake completes.**
- Device: `sgx` MMIO stubs with plausible ID/config values, the `gfx-asc`
  RTKit HLE, the UAT walker, and initdata decoding for the pinned build.
- Ack enough of DeviceControl that the driver considers the GPU up. Return
  power/DVFS acks and empty statistics/log channels.
- Work submissions are **acknowledged and completed without rendering**: fire
  the stamps/events so nothing times out. This alone shows what the
  submission path looks like.
- Exit criteria: with the software-rendering patches reverted *in the user's
  own image*, (a) the AGX accelerator and IOGPU device appear in the guest
  IORegistry; (b) no GPU timeout, recovery or panic for 30 min of idle plus a
  scripted UI session; (c) the agx-trace log decodes ≥ 95% of channel
  messages and work items to known types for the pinned build; (d) backboardd
  takes its Metal path, so the screen stays black or stale but the system is
  stable.

**M1. Command streams are parsed and a software rasteriser shows GPU-rendered output.**
- Decoders for G12P VDM (vertex/draw), PPP (fixed-function state), CDM
  (compute) and the USC shader words.
- An ISA interpreter (applegpu-style) covering the instructions that appear
  in the SpringBoard corpus.
- TBDR on the CPU: tiler → per-tile raster → fragment program writing the
  emulated **tile buffer** → end-of-tile program → memory, plus the
  background/load programs.
- Texture sampling, including twiddled and compressed layouts (port the ideas
  in Mesa's `ail`).
- A compressed-surface decoder for `disp0`, or a demonstration that the
  pinned build doesn't scan out compressed surfaces.
- Exit criteria: (a) SpringBoard home screen, app switcher and one stock app
  render via the GPU path; (b) **PSNR ≥ 40 dB against the software-rendering
  reference** for a fixed scripted sequence, compared locally; (c) a
  shader-corpus report: n distinct shaders seen, % interpreted without
  unknown opcodes (target 100% of the boot-to-SpringBoard corpus);
  (d) synthetic command-stream tests on `a13-test` in CI (§3.5).

**M2. Shader ISA → SPIR-V translation.**
- Decode AGX USC → a small SSA IR → SPIR-V. One option is AGX → NIR, then
  Mesa's MIT-licensed `nir_to_spirv` from zink. The alternative is a direct
  emitter like yuzu/Ryujinx's.
- Handle AGX-specific semantics: uniform/texture state registers, varyings
  via coefficient registers, in-shader vertex fetch and blending, tile-buffer
  loads and stores (lowered to attachments/local reads or storage images),
  the half/full register file, and helper/lane masks.
- Exit criteria: (a) 100% of the M1 corpus translates and passes
  `spirv-val`; (b) differential tests: every corpus shader run through the
  interpreter and through SPIR-V (on lavapipe/SwiftShader in CI) gives
  bit-identical integer results and FP results within ≤ 1 ULP (except
  documented transcendental approximations); (c) translation cost is under
  the frame budget once the pipeline cache is warm.

**M3. Vulkan backend on Tensor G5 (MoltenVK on the Mac mini for development).**
- Map PPP state to Vulkan pipelines, with dynamic state where possible.
- Emulate tile memory: a Xenia-EDRAM-style choice between real attachments +
  `VK_KHR_dynamic_rendering_local_read` /
  `VK_EXT_rasterization_order_attachment_access`, and a fragment-interlock or
  storage-image fallback.
- Buffer and texture caches over guest RAM with write-tracking invalidation
  through QEMU's dirty-memory/TLB-notdirty machinery (yuzu/RPCS3-style),
  zero-copy buffer import (`VK_EXT_external_memory_host`) where the driver
  supports it, and a GPU-side detile/compress.
- Present scanout surfaces that live on the host GPU straight to the Android
  surface (`AHardwareBuffer`) or the Cocoa view, skipping readback.
- Async pipeline compilation and an on-disk pipeline cache.
- Exit criteria: (a) the M1 PSNR test also passes on MoltenVK and on the
  Pixel's PowerVR Vulkan driver; (b) validation-layer clean on the scripted
  sequence; (c) on the Pixel, SpringBoard home ↔ app-switcher animation runs
  at a **median frame time at least 3× better than software rendering**
  (baseline measured first), with the emulated CPU no longer dominated by
  CoreAnimation rasterisation in a `perf` profile; (d) no guest-RAM readback
  on the steady-state scanout path.

### 3.4 Effort estimate

Person-months (PM) assume one engineer experienced in emulator GPU work and
QEMU, using the public material above. The ranges are wide because G12P
encodings are unknown until M0 traces exist.

| Milestone | PM (low–high) | Driver of the range |
|---|---|---|
| M0 handshake + stable no-op submission | **2–4** | How far the pinned build's initdata/microsequence layouts differ from m1n1's G13 definitions |
| M1 decoders + ISA interpreter + software TBDR + compressed scanout | **6–12** | G12P control-stream and ISA encoding RE; tile-buffer and compression semantics |
| M2 ISA → SPIR-V | **4–7** | AGX-specific semantics (tile-buffer access, coefficient varyings, register-file aliasing) |
| M3 Vulkan backend (PowerVR + MoltenVK) | **6–12** | Tile-memory emulation choice, cache coherency with TCG-written guest RAM, PowerVR driver quirks and extension gaps |
| **Total** | **18–35 PM** (~1.5–3 person-years) | For comparison: drm/asahi took about a year of one expert to reach a usable G13 driver, *with* real hardware and m1n1 |

M0 is the cheapest, most decisive checkpoint. It also gives the trace
corpus that makes the M1–M3 estimates firm. Re-estimate there.

### 3.5 Testing without Apple code in the repo

* **CI (public):** synthetic UAT tables, initdata and command streams, plus
  hand-assembled shaders. Write the assembler for the known subset as RE
  progresses, the way applegpu did. Run them on `-M a13-test` with the AGX
  device attached, checking interpreter output against golden images that the
  tests themselves produce. SPIR-V checks run on lavapipe.
* **Local (private):** a capture/replay format records work items and
  referenced memory from a user's own guest. Replays drive regression and
  performance work offline. These captures contain Apple-compiled shaders, so
  they **never** get committed, uploaded or attached to issues.

---

## 4. Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| R1 | **G12P encodings are undocumented** (all public ISA and control-stream work is G13+) | Schedule (M1/M2) | Differential RE inside the emulator using the guest's own compiler; start from agxdecode/cmdbuf.xml/applegpu; keep the decoder table-driven |
| R2 | **Firmware-interface churn** between guest builds | Each new iOS build needs a new layout table | Pin one build; generate layout tables from a description file (drm/asahi-style versioned structs) |
| R3 | **Guest GPU watchdogs** versus slow emulation (TCG + software raster) | Recovery loops and panics | Complete work asynchronously but keep firmware timestamps/heartbeats plausible; stamp-on-ack in M0 |
| R4 | **Tile-memory semantics** (programmable blending, tile-buffer reads, end-of-tile programs) map badly onto immediate-mode Vulkan | Correctness and performance in M3 | PowerVR is TBDR too, so prefer local-read/raster-order extensions; fall back to interlock or storage images (Xenia ROV path). Check the Pixel driver's extension list early |
| R5 | **Coherency with guest RAM**: the CPU (TCG) and the emulated GPU share memory. Readbacks are expensive on the phone | Performance | Page-granular write tracking; host-resident surfaces for render targets and scanout; external-memory-host import |
| R6 | **Compressed and twiddled surfaces** reach scanout (`disp0` drops compressed frames today) | Black screen at M1 | Decompressor in `disp0` (CPU in M1, GPU in M3), or show the pinned build avoids it |
| R7 | **CPU-bound guest.** If the TCG CPU is the bottleneck, GPU offload gains less | Value | Measure software-rendering cost first (perf profile share of CoreAnimation/libRIP-style rasterisation). M3 exit criterion (c) checks this |
| R8 | **PowerVR Vulkan driver quirks** (shader compiler bugs, missing features) and **MoltenVK gaps** (geometry/transform-feedback-style paths, some sampler/format combos) | M3 schedule | Keep the backend conservative (core Vulkan 1.1 + optional extensions); dual-target from day one so bugs can be bisected by driver |
| R9 | **Legal/ethics drift**: captures, firmware or dumped tables slipping into the repo | Project integrity | Structural facts only; `.gitignore` the capture directories; review checklist item in PRs |
| R10 | **Upstream divergence**: a large AI-assisted subsystem in a fork | Maintenance | Keep it self-contained under `hw/gpu/apple-agx/` with one hook in `t8030.c`; never propose it upstream without a human rewrite and upstream agreement |

## 5. Prior art

* **RPCS3 (RSX):** HLE of a command FIFO; vertex/fragment program
  decompilers to GLSL/SPIR-V; a *surface cache* reconciling GPU render
  targets with CPU-visible memory. This is the template for R5.
* **yuzu / Ryujinx / Citron (Maxwell):** engine-level HLE (3D, compute, DMA);
  a shader recompiler from Maxwell to IR to SPIR-V/GLSL/MSL; buffer and
  texture caches with memory tracking; async shader compilation and pipeline
  caches. These are the closest template for M2/M3.
* **Xenia (Xenos):** command processor HLE; microcode → SPIR-V/DXBC; **EDRAM
  emulation via rasterizer-ordered views / fragment interlock**, the best
  analogue for AGX tile memory (R4).
* **Vita3K (PowerVR SGX543):** a *tile-based PowerVR* GPU emulated on Vulkan
  (incl. MoltenVK), with the USSE ISA translated to SPIR-V. It has the same
  ancestry as AGX and targets the same hosts.
* **Dolphin (Flipper/Hollywood):** TEV → generated shaders, *ubershaders* to
  hide compile stutter, and long-running pixel-accuracy regression testing
  (FifoCI with replayed FIFO logs). This is the model for §3.5's
  capture/replay CI.
* **Cemu (Latte), PCSX2 (GS hardware renderer):** shader recompilers, and
  local-memory coherency between emulated GPU memory and host textures.
* **Asahi (m1n1 tracer, drm/asahi, Mesa asahi, applegpu):** the
  domain-specific prior art for AGX itself. Our device implements the
  firmware side of the interface drm/asahi drives, and Mesa/applegpu give the
  userspace encodings for G13+.
* **Paravirtual alternatives** (virtio-gpu Venus, Apple's
  ParavirtualizedGraphics for macOS guests): these give better performance
  per engineering month but **require guest-side drivers**. That conflicts
  with the "unmodified guest" goal, so they're out of scope here. ios-on-pixel
  ROADMAP §3 keeps a paravirtual Metal path as a separate research item.

## 6. Recommendation

Fund **M0 only** first (2–4 PM), as `-M t8030,gpu=on`, default off. It's
cheap and low-risk because it reuses the RTKit HLE pattern Inferno already
uses for four other IOPs. It answers the two unknowns that dominate the rest
of the estimate: how far the pinned build's firmware layouts are from m1n1's
G13 definitions, and what the G12P command and shader corpus looks like. Make
the go/no-go call on M1–M3 with that trace corpus in hand.
