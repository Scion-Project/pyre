# An Apple A14 (`t8101`, iPhone 12) machine: scoping study

Status: **T0 and T1 implemented** (`-M t8101`, see §9 for what was done,
what was verified and how, and what T2 needs). Sections 1–8 are the original
scoping study. This document scopes what it would take to add a `t8101`
machine next to `t8030`, and whether it's worth doing for the GPU work in
[`GPU-EMULATION-SCOPE.md`](GPU-EMULATION-SCOPE.md).

Why ask: the A14's GPU is a **G13** part, the same generation as the M1
(`t8103`) GPU that Asahi Linux has documented in depth (m1n1, drm/asahi,
Mesa `src/asahi`, dougallj/applegpu). Most other A14 blocks are shared with
the M1 too, and Asahi has documented those as well. The A13's GPU (G12P) has
no public encoding work at all (GPU doc §2, risk R1).

Ethics are as in the GPU doc: no Apple binaries, firmware, kernelcaches,
device trees or keys in any repository, and no instructions for getting them.
This document contains only structural facts and pointers to public sources.
Items marked **verify** are inferences from the M1 or from XNU. Check them
against the device tree and kernelcache from your own iPhone 12 IPSW before
anything goes into code.

**Target devices.** `t8101` ships in iPhone 12 mini / 12 / 12 Pro / 12 Pro
Max (`iPhone13,1`–`13,4`, boards `d52g`, `d53g`, `d53p`, `d54p`) and the iPad
Air 4 (`j307`/`j308`). Suggested first target: **`d53g` (iPhone 12)**. It has
4 GiB of RAM, which fits `t8030_fixup_ram_size()`'s 4 GiB cap. The Pros have
6 GiB.

---

## 0. Summary

* **Cost, CPU path to SpringBoard with simulated SEP: about 6–13
  person-months (PM).** Real SEP with a user-supplied key adds 2–4 PM. About
  half of the core estimate is one item, the **display coprocessor (DCP)**.
* **The A14 is the first SoC with a DCP.** On iPhone 12 the framebuffer driver
  (IOMobileFramebuffer) runs as firmware on an RTKit coprocessor, as on the
  M1. So the A13 display model (`apple_displaypipe_v4.c`, programmed by the
  in-kernel IOMFB) **does not carry over**. A `t8101` needs a DCP RTKit
  high-level emulation (HLE) that speaks the IOMFB RPC protocol. Asahi
  documented that protocol for macOS DCP firmware, and the iOS 14 version
  will differ in detail. This is the biggest new device and the biggest risk.
* **Everything else is mostly configuration.** Inferno already models the
  t8020/t8030 generation of almost every IP block: AIC, DART, PMGR pwrstate,
  ANS2 NVMe, SMC/AOP/SIO RTKit, SPI/I2C/GPIO/SPMI, WDT, MCA, S5L UART. The M1
  uses the same generations. Linux even drives the M1's DART with its
  "T8020" register layout. Most of the work is lifting hardcoded `t8030`
  constants into a per-SoC table and new CPU IDs/sysregs.
* **GPU payoff.** It removes the largest GPU risk (R1: undocumented G12P
  encodings) and gives working oracles: applegpu's simulator and Mesa's
  compiler, both for G13. Estimated GPU cost drops from 18–35 PM to about
  13–27 PM. The firmware interface (initdata/channels) is still per-build,
  and still differs from what drm/asahi supports (macOS builds, not iOS 14).
* **Recommendation.** First run a cheap check: look for the `dcp` and
  `dart-dcp` nodes in your own `d53g` device tree. Then do T0–T1 (§6),
  2–4 PM, which boots to serial and settles the CPU/sysreg unknowns. Decide on
  T2+ and the GPU platform after that.

---

## 1. Device inventory: `t8030` today → `t8101`

How `t8030` is built (from `hw/arm/t8030.c`, `t8030_init()`):

* The **device tree is user-supplied** (`-dtb`, loaded by
  `apple_boot_load_dt_file()`). Most MMIO bases, IRQ numbers and IOP
  carveouts are read from it, and `hw/arm/boot.c` deletes nodes listed in
  `REM_NAMES`/`REM_DEV_TYPES` and properties in `REM_PROPS`.
* **iBoot doesn't run** in the normal path. QEMU loads the kernelcache itself
  (`t8030_load_kernelcache()`), patches it (`hw/arm/kernel_patches.c`), fills
  in `/chosen` and the memory maps (`apple_boot_populate_dt()`/`finalise_dt()`),
  builds boot args (rev2/rev3 by build), and starts CPU0. SecureROM boot
  (`securerom=`) and SPTM/TXM boot (iOS 27, `sptm-boot.c`) are alternative
  paths.
* So for `t8101`, everything the DT describes comes **for free** from the
  `d53g` DT. The work is in what `t8030.c` **hardcodes**, and in devices whose
  *programming model* changed.

### 1.1 Hardcoded `t8030` facts that must become per-SoC

| Where | What | `t8101` action |
|---|---|---|
| `t8030.c` top | `SROM_BASE 0x1_0000_0000`, `SRAM_BASE 0x1_9C00_0000`, `DRAM_BASE 0x8_0000_0000`, arm-io at `0x2_0000_0000` (from DT `ranges`) | The M1 uses the same layout: SROM at 4 GiB, SRAM at `0x19C000000`, DRAM at `0x800000000`, devices at `0x2xxxxxxxx`. Expect it to match. **Verify** against the `d53g` DT/`memory-map`. |
| `t8030.c` top | `SEPROM_BASE 0x240000000`, 8 MiB; `SEP_UNKN*`/`SEP_dram_*` RAM windows | Re-derive from the A14 SEPROM (real-SEP path only). |
| `t8030_create_sep()` | 18 SEP sub-block MMIO bases at `armio_base + 0x40800000…0x42140000` (PMGR, TRNG, KEY, FCFG, MONI, EISP, AESS/AESH, PKA, MISC0–2, PROGRESS, BOOT_MONITOR) | Re-derive for A14 (real-SEP path only). |
| `t8030_pmgr_setup()`, `pmgr_unk_reg_*` | PMGR catch-all at `armio+0x3BC00000`; fuse/security block `0x3D2BC000…` (production/secure mode, board-id/epoch, chip revision, ECID lo/hi at `0x3D2BC300/304`); `PMGR_AON 0x3D280088`; `0x3D2E4xxx`/`0x3D2E8000` SEP-related scratch | On the M1, m1n1 finds the same kind of AON/fuse block in the same `0x23D2xxxxx` region, so the offsets may well match. **Verify** each read the A14 kernel/SEP makes (trace unknown accesses at T1). |
| `t8030_amcc_setup()` | AMCC base `0x200000000`, 4 planes × `0x40000`, plane/non-plane register offsets, GFX hash registers | The A14 memory controller is a different configuration (plane/channel count). Take the counts from the DT `mcc`/`amcc` node properties. The register layout is probably the same family. |
| `t8030-config.c.inc` | `clock-frequencies`, voltage-state tables, `bridge-settings*` | New tables for `t8101`. These are DT-shaped fake values the kernel only sanity-checks. Synthesize them, don't copy them. |
| `hw/intc/apple_aic.c` | `AIC_INT_COUNT 576`, `AIC_CPU_COUNT 6` ("hardcoded for T8030") | Make it a property. Take the IRQ count from the DT/`AIC_INFO`. The CPU count stays 6. |
| `t8030.c` defines | `SPI0_IRQ 444`, `GPIO_SPI0_CS 204`, `GPIO_FORCE_DFU 161`, `DWC2_IRQ 495`, `ANS_SIZE`, `SIO_SIZE`, `DISPLAY_SIZE` | Read them from the DT where possible, otherwise put them in the per-SoC table. |
| `t8030_init()` | `chip-id 0x8030`, `board-id 0x4`, `platform-name "t8030"`, `target-type "n104sim"`, `chip-revision 0x20`, `display-scale 2`, `oled-display 1`, `graphics-featureset-class "GLES2,0"` | `0x8101`, the board-id from the model's BuildManifest, `d53g…`, display scale 3, 1170×2532 panel (`disp-width`/`disp-height`). |
| `t8030_init()` | `sio_protocol` switch on iOS major version (9 for 13–16, 10 for 17+) | Probably the same for A14. **Verify** once SIO comes up. |
| `boot.c` `REM_NAMES` | Nodes deleted before boot | Same list plus A14 additions found at T1, e.g. ANE/AVD/AVE/ISP DARTs, `dcp`/`dart-dcp` until the DCP model exists (§1.2). |

Suggested refactor (part of T0): keep **one** machine implementation and add
a `AppleSoCConfig` table (`t8030_config`, `t8101_config`) holding the items
above. Register `t8101` as a second `TYPE_MACHINE` sharing `t8030_init()`.
A fork of `t8030.c` would double the maintenance on every fix.

### 1.2 Per-block assessment

"Same family" means Inferno's existing model should work after a DT and
config change. Linux compatibles are from the upstream/Asahi kernel trees.

| Block | `t8030` model | Public `t8103` evidence | `t8101` work |
|---|---|---|---|
| **CPU clusters** | `hw/arm/a13.c`: 2 clusters (P = Lightning, E = Thunder), 6 cores, IPI sysregs, CTRR per cluster | M1 = 4+4 Firestorm/Icestorm. **A14 = 2 Firestorm + 4 Icestorm**, the same topology as A13 | New MIDR part numbers and sysregs (§2). Topology is unchanged (`A13_MAX_CPU 6`, 2 clusters). |
| **AIC** | `apple_aic.c`, register layout `REV/CAP0/CAP1/RST…`, IPIs, timebase | Linux `apple,t8103-aic` = **"AICv1"** (A7–M1). AIC2 starts with t6000 | Same family. Parameterise the IRQ count. |
| **Timers / FIQ** | ARM generic timer at 24 MHz, virtual timer + fast-IPI → FIQ OR gate | Same on the M1 (Asahi: timers and fast IPIs arrive as FIQs) | None. |
| **DART** | `hw/arm/dart.c` (+ `dart-stub.c`), 2-level tables, 16 KiB pages | Linux `apple-dart.c` drives `t8103` with `type = DART_T8020`, the **A12-era register layout**, `oas = 36` | Same family. |
| **SART** | `sart.c`, versions via `sart-version` | Linux `apple,t8103-sart` = SART v2 | Should be covered. The version comes from the DT. |
| **PMGR** | Generic pwrstate registers + catch-alls | Linux `apple,t8103-pmgr-pwrstate`, the same pwrstate format since A7-era | Same family. The catch-all offsets need checking (§1.1). |
| **AMCC** | Custom model + DT props | m1n1 configures the t8103 MCC; no Linux driver | Recount planes/channels (§1.1). |
| **ANS / NVMe** | `ans.c` "ANS2": RTKit HLE + Apple NVMe with **linear SQ** (`0x24908`) | Linux `apple,t8103-nvme-ans2`, the same linear-submission/NVMMU scheme | Same family. **Verify** whether the A14's ANS has the NVMMU/TCB path Linux uses on the M1. If it does, add it. |
| **SMC** | `smc.c`, A7IOP v4, RTKit, key/value | Linux `macsmc` (RTKit, same key protocol) | Same family. The key set comes from the guest. |
| **AOP** | `aop.c`, A7IOP v4, RTKit HLE (most sensors deleted via `REM_NAMES`) | Asahi has RE'd AOP on M-series for mic/ALS | Same approach. |
| **SIO** | `apple_sio.c`, protocol 9/10 | Asahi `apple,sio` (DMA for DP audio on M1/M2) | Same family. |
| **SEP / A7IOP** | `hw/arm/sep/*`: real SEP core runs the user-supplied SEPROM + decrypted SEPFW (`t8030_create_sep`), or `sep_sim` | None (SEP is out of scope for Asahi) | §3. |
| **Display** | `apple_displaypipe_v4.c` (`disp0`) + `dart-disp0`; IOMFB runs **in the kernel** | **A14/M1 add the DCP**: IOMFB runs on an RTKit ASC; the kernel talks RPC to it. Asahi `apple,t8103-dcp` (drm/apple driver; m1n1 DCP tracer) | **New device: DCP RTKit HLE** (§1.3). The A13 `disp0` model won't be used. |
| **Scaler (M2R/MSR)** | `apple_scaler.c` + `dart-scaler` | None | Probably the same family. **Verify** the node is still present. |
| **MIPI DSIM / display PMU / backlight** | `synopsys_mipi_dsim`, Chestnut, LM3539-style backlight over I2C | None | On an OLED panel the DCP owns more of this. Re-check which nodes the guest still drives. |
| **Multitouch (MT-SPI)** | `mt-spi.c` | Asahi `apple,z2-multitouch` (Touch Bar), the same Z2 family | The protocol is probably the same. The firmware blob and report sizes differ per panel. |
| **USB** | `hcd-dwc3.c` + `apple_typec` (ATC), `hcd-dwc2.c`, `apple_otg.c`, VirtualHere uplink | Linux DWC3 + Asahi ATC PHY (`apple,t8103-atcphy`) | Same family. iPhone 12 is still Lightning; the DT decides which controllers exist. |
| **GPIO** | `apple_gpio.c` (×4 controllers) | Linux `apple,t8103-pinctrl` | Same family. |
| **I2C / SPI** | `apple_i2c.c` (PASemi-derived), `apple_spi.c` | Linux `apple,t8103-i2c`, `apple,t8103-spi` | Same family. |
| **SPMI / PMU** | `apple_spmi.c`, `spmi-pmu.c` (D2255-style), baseband SPMI | Linux `apple,t8103-spmi` | Same family. The PMU part may differ; the DT `compatible` says which. |
| **WDT** | `apple_wdt.c` | Linux `apple,t8103-wdt` | Same family. |
| **UART** | S5L UART ×9 | Linux `apple,s5l-uart` | None. |
| **AES** | `apple_aes` (hardware AES, GID/UID) | None | Same family is likely. GID-key operations remain unavailable/simulated as today. |
| **MCA / audio** | `mca.c`, CS35L27 amps, CS42L77 codec | Linux `apple,t8103-mca` | Same family. The codec/amp parts follow the board. |
| **PCIe** | `apcie` + DARTs; WLAN/BT/baseband removed | Linux `apple,pcie` (t8103) | Same family. The iPhone 12 baseband is a different modem, but it stays removed. |
| **Temp sensors** | 14 nodes | None | Follow the DT names. |
| **GPU** | None (`gfx-asc` removed) | G13: see §5 | §5. |

### 1.3 The DCP

Public facts: Asahi's progress reports and Project Zero's 2022 "fake
Carrier.app" write-up describe the DCP as introduced with the A14 and M1.
The bootloader starts the DCP firmware, which contains most of the old IOMFB
driver. The kernel-side `IOMobileFramebuffer` becomes a thin shim that makes
RPC calls (method names and argument structs) over RTKit shared-memory
endpoints. Asahi's DCP driver and m1n1 tracer give the transport, the call
dispatch, and the struct layouts for the macOS firmware versions they support.

For `t8101` that means:

* A DCP device on `a7iop`/`rtkit` (the pattern already used for AOP/SMC/SIO/ANS),
  with the iBoot-side state the shim expects to already exist, since there is no
  iBoot (the "firmware already running" handshake).
* An IOMFB RPC server for the subset the iOS 14 shim calls: swap
  submission, mode/timing queries, power, brightness, hot-plug-less
  internal panel. Scanout reads the swapped surface through `dart-dcp` and
  presents it with the existing QEMU console code from `apple_displaypipe_v4`.
* **The iOS 14 RPC tables are not the macOS 12/13 ones** Asahi pins. The
  method set and struct layouts will need tracing in the emulator. Asahi's
  tracer method transfers directly. This is the same per-build versioning
  problem the GPU has (GPU doc R2).
* The existing kernel patch for the iOS 14.8 IOMFB "bandwidth values not
  found" panic is A13-specific and won't apply.

Estimate: **3–6 PM** to a stable home screen through DCP HLE. A possible
shortcut: remove the `dcp` node (`REM_NAMES`) and bring the system up
headless, checking progress over serial and USB. If iOS tolerates having no
display, T1–T3 can proceed before the DCP model exists. **Verify.**

---

## 2. CPU: Firestorm/Icestorm vs Lightning/Thunder

### What Inferno models for the A13

* `target/arm/cpu64.c` `apple-gxf`: `max` with PAuth (APA = PAuth2, `pauth-noop`
  under TCG), SVE/SME/BTI/MTE turned off. Its comment already cites LLVM's
  `apple-a14` as "v8.5 excluding BTI".
* `hw/arm/a13.c`: MIDR implementer `0x61`, variant/revision 1/1, part `0x12`
  (Lightning) / `0x13` (Thunder) / `0x15` (SEP). It defines about 40
  implementation-defined sysregs as plain storage: `HID0/1/3/4/5/7/8/9/11/13/14/16`,
  `EHID3/4/10`, the `*_ERR_STS` family, `LLC_*`, `PMC0/1`, `PMCR0/1`, `PMSR`,
  `AMX_CTL/STATUS`, `CPU_OVRD`, `ACC_OVRD/CFG`, `SYS_ACC_PWR_DN_SAVE`, uncore
  `UPMCR0/UPMSR/UPMPCM`, plus functional fast-IPI (`IPI_RR_LOCAL/GLOBAL`,
  `IPI_SR`, `IPI_CR`) and per-cluster `CTRR_*`.
* `hw/arm/a13_gxf.c`: GXF (`GXF_CONFIG/STATUS/ENTER/ABORT_EL1`, the banked
  `*_GL11` registers, the VBAR/SPSR/ELR/ESR/FAR redirection when guarded) and
  SPRR (`SPRR_CONFIG_EL0/EL1`, the per-EL `SPRR_*BR*` permission registers
  and `MPRR_*` masks), with TLB flushes folded into the PTW. That's
  everything PPL needs on iOS 14.

### What changes for the A14

| Item | A13 | A14 | Work |
|---|---|---|---|
| MIDR part | `0x12` P / `0x13` E | **`0x21` Firestorm / `0x20` Icestorm** (XNU `osfmk/arm/cpuid.h`; the M1 "Tonga" variants are `0x23`/`0x22`) | Per-SoC table in `apple_a13_create()`. XNU derives `cpufamily` (Firestorm/Icestorm family) from this. With the wrong part number the kernel gets `CPUFAMILY_UNKNOWN` and takes wrong code paths. |
| ISA level | v8.4 + PAuth | v8.5 minus BTI/MTE (LLVM `apple-a14`). M1 Linux hwcaps add `flagm2`, `frint` and `sb`/`ssbs`/`dcpodp` over A13-class cores. | Expose the extra `ID_AA64ISAR*`/`PFR*` fields TCG already implements (from `max`), keep BTI/MTE/SVE/SME off. Under KVM the host decides. |
| HID/EHID chicken bits | ~16 HIDs | The M1 set is in m1n1 `chickens_firestorm.c`/`chickens_icestorm.c` and `cpu_regs.h`; XNU's public `pexpert/pexpert/arm64` board headers and `HAS_*` feature macros differ between T8030 and T8101 | Diff the HAS_* sets and the HID writes in XNU's public `start.s`/`cpu.c`. Add the new encodings as storage. Expect a handful; days of work, found by UNDEF traps at T1. |
| GXF / SPRR | Modelled | Same mechanism as on the M1. Sven Peter documented it on the M1 at the same `S3_6_C15_C1_*`/`C8_*` encodings Inferno uses | Expect it to work as is. Any extra SPRR fields show up as traps during PPL bring-up. |
| CTRR / KTRR | Per-cluster CTRR | Same (CTRR since A12) | None. |
| AMX | Present; the kernel patch "disable AMX" | AMX present (corsix/amx documents the M1 version) | Patches are pattern-based. Check they still match the iPhone13 kernelcache. |
| TSO bit | No | The M1 has a TSO mode bit (Asahi). It's present on the A14 per die sharing, but iOS doesn't use it | None. |
| Performance counters / uncore | Storage only | Same encodings on the M1 (Asahi PMU driver) | None. |
| SPTM | Used from iOS 27 on `t8030` (`t8030_kernel_uses_sptm()` detects `__DATA_SPTM`) | Detection is generic. **Verify** which iOS release moves t8101 to SPTM | None for iOS 14; carries over for 27. |

Net: **0.5–1.5 PM** of CPU work, mostly in T1.

---

## 3. SEP (constraint)

The A14 SEP is a newer core with a different SEPROM and a different SEP
firmware. The real-SEP path (`sep-rom=` + `sep-fw=`) needs **the user's own
A14 SEPROM and a SEPFW decrypted with a key the user supplies**, exactly as
for `t8030`. This document does not cover where those come from. Under the
ethics rules, the repository must never contain them or say how to obtain
them.

Engineering consequences:

* The SEP sub-block MMIO bases, `SEP_UNKN*` RAM windows, AESS command-flag
  encodings (`hw/arm/sep/aes.c` already distinguishes T8015/T8020),
  `SEPOS_PHYS_BASE` constants in `debug-trace.c`, PKA/TRNG/key register
  layouts and the boot monitor all need A14 values. That's **2–4 PM**, and
  only with a usable key.
* The **simulated SEP** (`t8030_create_sep_sim`) talks the kernel-facing SEP
  mailbox protocol and doesn't depend on SEPROM. It's the default plan for
  `t8101` T1–T4, with the same limitations as today (no data protection or
  `ENABLE_DATA_ENCRYPTION`, no biometrics).

---

## 4. iOS versions and restore

* The iPhone 12 and 12 Pro shipped with iOS 14.1, and the 12 mini and Pro Max
  with 14.2. **The project's iOS 14 target (14.8) applies unchanged.** Pinning
  the same 14.8 build as `t8030` means the same XNU source, so the
  pattern-based kernel patches, the simulated SEP protocol and the SIO
  protocol (9) all carry over.
* The iPhone 12 is also supported by every later release the repo targets,
  up to the iOS 26/27 work in `docs/ios27/`.
* Restore needs the same artefacts as `t8030`, taken from the user's own
  `iPhone13,2` IPSW: kernelcache, the `d53g` device tree, restore ramdisk,
  trustcache, and the SEP firmware if real SEP is used. It also needs the
  machine identity (`chip-id 0x8101`, board-id, ECID, model/serial properties)
  to match that model's build identity in the BuildManifest. Whatever the
  `t8030` restore guide does for personalization, it does the same with the
  `t8101` build identity.
* `boot-mode` (`enter_recovery`/`exit_recovery`), the USB uplink (DWC/ATC +
  VirtualHere) and NVMe namespace layout are SoC-independent.

---

## 5. GPU: what Asahi/Mesa gives a G13 (A14) target

Naming: Asahi uses G13G (M1), G13S/C/D (M1 Pro/Max/Ultra) and G14x (M2
family). By the same scheme the A14 is **G13P**. **Verify** from the kext list
(`AGXG13P`, `AGXFirmwareKextG13PRTBuddy`).

| Artefact | Applies to A14 G13P? | Notes |
|---|---|---|
| **dougallj/applegpu** ISA docs, disassembler, assembler, simulator | **Yes, directly** | Same ISA generation. The simulator becomes a reference oracle for our interpreter (GPU doc M1). Spot-check opcode coverage on the phone variant. |
| **Mesa `src/asahi/compiler`** (NIR → AGX) | **Yes, as oracle and tooling** | Compile test shaders on the host, then compare against guest-Metal output for the same shader. Not used in the emulator datapath. |
| **Mesa `genxml/cmdbuf.xml`, `lib/decode.c` (agxdecode)** | **Yes, mostly** | VDM/CDM/PPP word packing is per generation. Expect small phone/firmware differences. This turns M1's "decoder RE" into "decoder port". |
| **Mesa `layout/` (ail)**: twiddled/compressed image layouts | **Yes** | Also needed for compressed scanout (GPU doc R6), now via DCP. |
| **UAT format** (m1n1 `hw/uat.py`, drm/asahi `mmu.rs`) | **Yes** | Same page-table format and TTBR page. The VA split may differ on iOS. |
| **drm/asahi + m1n1 `fw/agx/*.py`** (initdata, channels, microsequences) | **Partially** | The concepts and most field meanings transfer. **The layouts don't**: drm/asahi pins macOS 12.3/13.x firmware, iOS 14.8 is a different (older) build, and the phone variant has fewer cores and different DVFS tables. Early m1n1 history (macOS 11.x/12.x-era traces) is the closest starting skeleton. Layouts still need tracing. |
| **m1n1 hypervisor tracer method** (`trace/agx.py`) | **Yes** | The same trace-and-decode loop, run inside Inferno. |
| Mesa Gallium/Honeykrisp drivers, drm/asahi UAPI | **No** (reference only) | The guest uses Apple's IOGPU/Metal stack, not Linux UAPI. |
| m1n1 GPU power-up / SoC glue (PMGR domains, `sgx` MMIO init values for t8103) | **No** | SoC-specific. Use the `d53g` DT. |
| G13S/C/D multi-die features, G14 features in drm/asahi | **No** | |

Effect on the GPU estimate (GPU doc §3.4, 18–35 PM):

| Milestone | `t8030` (G12P) | `t8101` (G13P) | Why |
|---|---|---|---|
| M0 handshake | 2–4 | 1.5–3 | m1n1/drm/asahi structs are much closer, but iOS layouts still need tracing |
| M1 decoders + ISA interpreter + soft TBDR | 6–12 | 3–7 | Encodings are documented, applegpu simulator is an oracle |
| M2 ISA → SPIR-V | 4–7 | 3–5 | Semantics documented; Mesa NIR passes are a reference for tile-buffer/varying lowering |
| M3 Vulkan backend | 6–12 | 6–12 | Host side, unchanged |
| **Total** | **18–35** | **~13–27** | Saves about 5–8 PM and removes risk R1 |

The machine costs about 6–13 PM and the GPU saves about 5–8 PM. On GPU
effort alone it roughly breaks even. The real gain is **lower risk and
verifiable correctness**: R1 is gone, and public tools exist to check the
emulator against. A `t8101` machine is also a better base for every other
DCP-era device (M1-documented IP) than the A13 is.

---

## 6. Milestone plan

PM = person-months for one engineer who knows QEMU and Inferno, with simulated
SEP unless stated. Each milestone has an exit criterion checked on the
pinned iOS 14.8 `d53g` build.

| # | Milestone | Content | Exit criteria | PM |
|---|---|---|---|---|
| **T0** | Machine skeleton | `AppleSoCConfig` refactor of `t8030.c` (§1.1); `-M t8101`; AIC IRQ count property; A14 MIDR/sysreg table; DT fix-ups for `t8101` identity | `t8030` boots to SpringBoard unchanged (regression); `-M t8101` loads the `d53g` DT and kernelcache, starts CPU0, and reaches the first kprintf | 1–1.5 |
| **T1** | Kernel to serial | New HID/EHID sysregs, PMGR/fuse/AMCC values, CTRR/PPL (GXF/SPRR) checks, `REM_NAMES` additions incl. `dcp` | Serial log through IOKit matching to "waiting for root device" with no panic; PPL initialised; all 6 CPUs online; unknown-MMIO trace triaged | 1–2.5 |
| **T2** | Root mount | ANS2/NVMe (+ NVMMU if present), SMC, SIO, AOP, SPMI-PMU, GPIO, I2C, WDT, sim SEP | Boots the restore ramdisk to `restored` *and* an installed system to `launchd`, with the root filesystem mounted from NVMe | 1–2 |
| **T3** | Restore completes | USB (DWC3/ATC/uplink), recovery/DFU modes, NVMe partitioning, sim SEP restore steps | `idevicerestore`-style restore of the pinned build completes; the device reboots into setup with no panic | 0.5–1.5 |
| **T4** | SpringBoard | DCP HLE (§1.3), MT-SPI, buttons, backlight/panel, software-rendering patches for the iPhone13 build | SpringBoard home screen shown, touch input works, 30 min idle without panic or watchdog | 3–6 (DCP-dominated) |
| *T5 (opt.)* | Real SEP | §3, needs the user's own key | SEPOS boots; passcode set/unlock works | 2–4 |
| *G0…* | GPU | GPU doc M0–M3 retargeted to G13P | As in GPU doc | ~13–27 |

**To SpringBoard: about 6.5–13.5 PM** (call it 6–13). With real SEP: about
9–17 PM.

---

## 7. Risks

| # | Risk | Impact | Mitigation |
|---|---|---|---|
| T-R1 | **DCP iOS 14 RPC protocol differs from Asahi's macOS tables** | T4 schedule, which is half the estimate | Trace in the emulator with Asahi's method; implement only the shim's call set. Bring T1–T3 up headless (DCP removed) first. |
| T-R2 | **Hidden SoC-specific MMIO** (PMGR/AON/fuse/AMCC reads with no DT entry) | T1 slips | The unknown-access logging already in `pmgr_unk_reg_*`. M1 m1n1 maps as hints. |
| T-R3 | **New implementation-defined sysregs or SPRR fields** | T1 panics in early start/PPL | Diff XNU public HAS_* macros and m1n1 chickens/cpu_regs first. Treat UNDEF traps as a checklist. |
| T-R4 | **SEP**: the A14 SEPROM/SEPFW need a user-supplied key; sim-SEP gaps (data protection, biometrics) | No data protection; restore steps that need SEP | Same as `t8030`: sim SEP by default, real SEP optional. |
| T-R5 | **Maintenance cost of two machines** | Every `t8030` fix has to be done twice | Shared implementation + config table (T0). Never fork `t8030.c`. |
| T-R6 | **Memory**: Pro models need 6 GiB, the cap is 4 GiB | Pro boards unusable | Target `d53g`. Lift the cap per SoC later. |
| T-R7 | **GPU firmware layouts are still per-build** even on G13 | GPU M0 cost stays above zero | Generate layout tables from a description file (GPU doc R2). Start from early m1n1. |
| T-R8 | **Upstream divergence** (ChefKiss upstream has no `t8101`) | Merge conflicts in `t8030.c` | Keep `t8101` additions in the config table and new files. |
| T-R9 | **Ethics drift**: DTs, firmware or traces from a real iPhone 12 landing in the repo | Project integrity | Structural facts only. Values are synthesized, not copied. Captures stay local. |

## 8. Public sources

* Asahi Linux docs (HW: AIC, DART, SART, PMGR, ANS/NVMe, SMC, DCP, AGX, SPRR/GXF);
  progress reports (DCP introduced with A14/M1).
* Linux upstream + Asahi trees: `irq-apple-aic.c`, `iommu/apple-dart.c`
  (`DART_T8020` layout for t8103), `soc/apple/sart.c`, `rtkit`,
  `nvme/host/apple.c`, `pmdomain/apple`, `spi-apple.c`, `i2c-pasemi`,
  `pinctrl-apple-gpio.c`, `apple_wdt.c`, `spmi-apple-controller.c`,
  `sound/soc/apple/mca.c`, `drm/apple` (DCP), `drm/asahi`.
* m1n1: `proxyclient/m1n1/fw/agx`, `fw/dcp`, `trace/*`, `hw/uat.py`,
  `src/chickens_*.c`, `src/cpu_regs.h`.
* Mesa `src/asahi`; dougallj/applegpu; corsix/amx.
* XNU open source: `osfmk/arm/cpuid.h` (Firestorm/Icestorm part numbers and
  family), `pexpert/pexpert/arm64/*` board headers, `osfmk/arm64` start/CPU code.
* LLVM `AArch64Processors.td` (`apple-a13`/`apple-a14` feature sets).
* Sven Peter, "Apple's SPRR and GXF" (M1). Project Zero, "The curious tale of a
  fake Carrier.app" (2022, DCP background).

---

## 9. T0/T1 status (October 2026)

**Result.** `-M t8101` boots the iOS 14.8 (18H17) `iPhone13,2` kernel,
headless, through IOKit matching to `BSD root: md0`, mounts the restore
ramdisk (the same restore ramdisk and trustcache as `iPhone12,1` 14.8) and
starts `launchd` and `restored_external` ("Restore environment starting").
All six application cores start (`-smp 7`: 6 + the parked SEP core). That is
past the T1 exit; T1's "PPL initialised" and "unknown-MMIO triaged" are
covered by the items below. `t8030` is unchanged (regression runs in the PR).

Run it like `t8030`, with the d53g device tree and the iPhone13,x
kernelcache from your own IPSW, the 14.8 restore ramdisk/trustcache, and the
idle placeholder SEP ROM path (`sep-rom=` an idle loop, `sep-fw=` anything):
the real SEP needs A14 values (§3), and this build enables
`ENABLE_DATA_ENCRYPTION`, which rules out the simulated SEP.

### 9.1 How the "verify" items were resolved

Evidence types: **DT** = read from the d53g device tree; **KC** = from the
iPhone13,x kernelcache (strings, or the same code sequence as in the
iPhone12,x kernel with different encodings/offsets); **BOOT** = the kernel
gets past the point that needed it; **M1** = public Asahi/m1n1 documentation,
not checked beyond BOOT. Nothing from these files is in the repository.

| Item | Answer | Evidence |
|---|---|---|
| Memory map (SROM/SRAM/DRAM, arm-io) | Unchanged: arm-io `ranges` `0x0 → 0x2_0000_0000`, size 4 GiB, PCIe at `0x6_0000_0000`; DRAM at `0x8_0000_0000` | DT, BOOT |
| AIC | Same AICv1 (`aic,1`, `aic-version 2`), MMIO 0xC000; 704 IRQs (22 `ipid-mask` words). `aic-timebase` reg is absolute on d53g | DT, BOOT (`_aicNumExtInts = 0x2c0`) |
| DART | `dart,t8020` throughout. New: an `instance` list shorter than `reg`; a `DAPF`-typed entry is a DART with an address filter whose filter range is appended after the listed ones. The kernel wants APF slices (`PARAMS2[15:8]`) and lock support in `PARAMS2` | DT, KC (panic strings), BOOT |
| CPU MIDR | `apple,icestorm` ×4 (cluster 0, E), `apple,firestorm` ×2 (cluster 1, P); part 0x20/0x21 | DT, XNU cpuid.h, BOOT |
| GXF/SPRR | **Not as on A13.** GL1 has its own SP/TPIDR/VBAR/SPSR/ASPSR/ESR/ELR/FAR at `S3_6_C15_C10_0..7`, and the EL1 names are not redirected in GL; SPRR EL1 permissions moved from `S3_6_C15_C3_0` to `C1_6`, the EL0 mask from `C3_1` to `C3_0` | KC (same PPL code, swapped encodings), M1 names, BOOT |
| New sysregs | `ACNTVCT_EL0` `S3_4_C15_C10_6` (the kernel's `mach_absolute_time`), `APSTS_EL1` `S3_6_C15_C12_4` (spun on for MKEYVld); ~70 other encodings used only by the A14 kernel are storage (HID2/6/17/18/21, EHID9/20 among them) | KC (encoding scan), BOOT |
| PMGR | Main block moved to `0x23B70_0000` (size 0x80000) with power-state registers from offset 0 and SEP_PS at `+0xC00` (`ps-regs`); always-on power states at `0x23D28_0000` (catch-all range, mirrored now); fuse block at `0x23D2B_C000` as on t8030 | DT, BOOT |
| PMGR properties | The kernel needs t8030's voltage-state tables (`voltage-states1` etc.), `bridge-settings-0..17` (d53g has no optional bridges), `tunableh-board-id`/`-major-rev`/`-minor-rev` on the pmgr node, and per-cluster DVMR weight maps (`0x210E206A8`, `+0x38`, `+0x40`; `0x211E2...`) | KC (panic strings / `AppleT8101PMGR` code), BOOT; values synthesized |
| CPU start | `pmgr +0x54004` (M1 layout) | M1, BOOT (all six cores take interrupts) |
| AMCC | `mcc` node identical in shape (4 channels, same reg layout); t8030 AMCC model used as is | DT, BOOT |
| Board identity | chip-id 0x8101, board-id 0xC, security domain 1 | BuildManifest (d53gap identity), BOOT |
| IOPs | `iop,ascwrap-v4` (ANS, SMC, AOP, SIO, PMP; SEP `iop-sep,ascwrap-v4`). Same message registers, queue control at `+0x10/+0x14` of the mailbox block (Linux `asc-mailbox-v4`) | BOOT (RTKit handshakes complete) |
| ANS / NVMe | `nvme-linear-sq` makes the kernel pick ANS3 + CoastGuard, which fails in `pmap_iommu_map` (as t8030's `nvme-coastguard`); removed, ANS2 + NVMe work | BOOT |
| PCIe | Same register layout as t8030 with 3 ports (bridges 1, 2) | DT, BOOT |
| SPI, SPMI | New controllers: `spi-1,spimc` and `spmi,t8101` (M1-style). Not created yet | DT |
| DCP | Present (`dcp`, `dart-dcp`, `dcp0-expert`); removed, the system runs headless to restored | DT, BOOT |
| SIO protocol | 9, as on t8030 | BOOT |
| SPTM | Not used by iOS 14 | KC |

Not verified: the A14 MIDR variant/revision (1/1 is the A13 value; the kernel
reports chip revision B1 from the fuse readback, which is consistent), the
meaning of the 70 storage registers, the real DVMR weight maps and tunables
(made-up values the kernel accepts), whether `restored` needs anything else
(the next stop is USB), the A14 SEP (§3).

### 9.2 What T2 needs

* **USB** (DWC3/ATC on d53g; there is no `mapper-usb-device`, so no DWC2
  OTG): for restore and the uplink. Then the restore flow can be tried.
* **SPI** (`spimc`, M1-style; Linux `spi-apple.c`) for multi-touch and the
  audio codec, and **SPMI** (`spmi,t8101`; Linux `spmi-apple-controller.c`)
  for the PMU (RTC: "RTC did not show up").
* **Temperature sensors** (DT-listed, removed for now) and the remaining
  t8101 nodes the KEEP_COMP list drops (`mca`, `nco`, `pwm`, `dwi`, ...).
* **ANS3 / CoastGuard** if the installed system needs it (NVMMU question
  from §1.2).
* An installed-system boot to `launchd` with the root on NVMe (T2 exit).
