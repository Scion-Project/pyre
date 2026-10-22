# GPU M0 notes: AGX bring-up as seen from QEMU

Working notes for milestone M0 of [`GPU-EMULATION-SCOPE.md`](GPU-EMULATION-SCOPE.md)
(`-M t8030,gpu=on`). Everything here was learned **black-box**: from QEMU's side
of the interface (MMIO/mailbox trace points, UAT walks of the structures the
driver builds, `-d exec` PC traces) and from the public G13 documentation
(Asahi Linux docs, m1n1 `fw/agx`, drm/asahi). No Apple code or data is in
this repository; captures stay on the machine that made them.

Guest: iOS 14.8 (18H17) restore ramdisk, research kernelcache, N104AP device
tree. Kext personality: `AGXAcceleratorG12P_B0`.

## How to look

* Trace points: `-trace 'apple_agx_*' -trace 'apple_rtkit_*'`. `apple_agx_dump`
  (on MSG_INIT) walks the UAT and logs the TTBAT, initdata, regionA/B/C and
  fw_status, one non-zero 64-bit word per line.
* Where a driver waits or bails out with no MMIO/mailbox trace: run with
  `-d exec,nochain -dfilter <kext text range> -D file` and look for long gaps
  between consecutive blocks of one function (a timed wait) and the branch
  taken after it. That is how step 1 below was found: the function that
  released the half-built firmware object had a ~2.5 s gap (a sleep with
  timeout) right before its error path.

## Bring-up sequence (what the driver needs)

1. **Accelerator probe.** Reads the sgx ID registers (`agx.c`), then writes
   sgx `+0xd08000..0xd08024` (`+0xd08024` gets a value that changes from run
   to run; `+0xd08000` gets 1 last). No polling.
2. **Wait for the GPU coprocessor.** The accelerator then waits, with a timeout
   of about 2.5 s of guest time, for RTBuddy(GFX). If that never appears it
   releases a half-built object whose destructor destroys an uninitialised
   mutex: `panic: "Destroying invalid mutex"`. (This was the M0 blocker in #46.)
   RTBuddy(GFX) never started because `gfx-asc`'s `iommu-parent` is
   `mapper-gfx-asc` (compatible `iommu-mapper,gfx`, the AGXArmFirmwareMapper),
   which `hw/arm/boot.c` deleted: the generic `iommu-mapper` entry in
   `KEEP_COMP` only matches that exact string. **Fix:** keep it when `gpu=on`.
3. **RTBuddy(GFX) start.** With the nub marked `pre-loaded`/`running` (as for
   every HLE'd IOP), RTBuddy still panics in `_attemptFirmwareLoad`
   ("REQUIRE failed: result == kIOReturnSuccess") unless the nub also has a
   firmware region (`region-base`/`region-size`, which iBoot does not put in
   the GFX nub). **Fix:** carve one out (4 MiB, unused) as for SIO/ANS. It then
   prints "Resuming...", reads the 8-byte ASC wrapper register (`gfx-asc`
   reg[1]) and writes `region-base | 1` to it.
4. **RTKit.** HELLO (v11), EPMAP, rollcall, power state 0x20; the driver starts
   EP 0x20 (firmware) and EP 0x21 (doorbell).
5. **MSG_INIT.** One message on EP 0x20: `0x0081_0fa0000c0000`, type 0x81 with
   a 44-bit initdata pointer that sign-extends to `0xffffffa0_000c0000`, the
   same kernel IOVA base drm/asahi uses on G13. The driver does not wait for
   a reply. After it, in a restore boot, there is **no further GPU traffic**:
   no doorbells, no FWCtl, every channel's read/write pointers stay zero for
   minutes, and no GPU watchdog fires. Boot reaches "waiting for host".

## UAT

* TTBAT entry 0 = `(ttbr0, ttbr1)` for context 0; ttbr1 is the
  `gfx-shared-region-base` page | 1. Kernel-half VAs walk from that page.
* 16 KiB granule, 3 levels with index shifts 36/25/14, 8/2048/2048 entries,
  so a 39-bit VA per half; leaf PTEs at the last level, e.g.
  `0x0080000827e5c44b` (PA in [39:14], AF, bit 55 set). The walker in
  `agx-uat.c` resolves everything the driver mapped.

## initdata (G12P / iOS 14.8)

Same top-level layout as m1n1's G13 `InitData` (see the comment above
`agx_dump()` in `agx.c` for offsets). regionB holds 17 `(state, ring)` channel
pairs in m1n1's order (TA/3D/CL x4, DevCtrl, Event, FWLog, KTrace, Stats) and
the stats/hwdata pointers from `+0x170` (with hwdata_b repeated, as on G13).
fw_status starts with the FWCtl channel pair, pointing into fw_status itself.
regionC has a few small non-zero words at `+0x28..0x60`; regionA's first
0x100 bytes are zero.

## Open questions for M1

* What makes the driver ring a doorbell? Nothing in the restore ramdisk opens
  the GPU; the first DevCtrl/work submissions need a boot that starts a GPU
  client (backboardd/Metal). `agx_ep_doorbell` logs DevCtrl ring pointers
  for when that happens.
* Which doorbell number maps to which work channel, the channel message
  formats and ring sizes (regionB ring spacing is 0xc000 for the first 12),
  and whether the firmware must publish anything in fw_status/regionA/C
  before the driver will submit work. Diff each against m1n1's G13 structs.
* Whether the driver checks the firmware's progress on first open (a
  timeout there would be the next "GPU watchdog" to answer).
