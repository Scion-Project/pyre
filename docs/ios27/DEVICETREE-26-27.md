# t8030 DeviceTree diff: 26.6.2 vs 27.0.1 (n104ap)

Input for modelling the new hardware/boot properties needed to boot iOS 27
(issue #11). Parsed from the two decrypted `DeviceTree.n104ap` blobs (Apple DT
format: `{u32 nprops, u32 nchildren, props{char name[32], u32 len, data}}`),
local research copies, read-only.

## Topology: unchanged

Both trees have **323 nodes**; **no node was added or removed**. So booting 27
needs **no new device-tree nodes** — only a handful of new/changed *properties*
on existing nodes. (New hardware blocks, if any, would show up when the kernel
probes; nothing in the static DT demands a new node.)

## Property changes on shared nodes

| Node | Added on 27 | Removed on 27 |
|---|---|---|
| `/chosen` | `txm-secure-channel`, `secureboot-certificate-format`, `secureboot-key-scheme`, `cryptex1-certificate-format`, `cryptex1-key-scheme` | — |
| `/arm-io/dart-sep` | `txm-secure-channel-base`, `txm-secure-channel-size`, `vm-base`, `vm-reserve`, `allow-vm-reserve-mapping` | — |
| `/arm-io/ans` | `nvme-queue-entries` | — |
| `/arm-io/sart-ans` | `sart-throttle-disable` | — |
| `/arm-io/dockchannel-uart` | `dock-wstat-mask` | — |
| `/arm-io/pmgr/clpc` | `pkg-lpm-power-target` | — |
| `/arm-io/pmgr/ppm` | `cpms-min-update-interval` | `min-update-interval-overrides` |
| `/arm-io/cpu-debug-interface` | — | `panic-trace-mode` |
| `/buttons` | `aot-power` | — |

## The ones that matter for the TXM/SPTM boot chain

- **`/chosen/txm-secure-channel = 1`** (u32): flag that a TXM↔kernel secure
  channel exists on this boot.
- **`/arm-io/dart-sep`** gains a DART (SEP IOMMU) window for that channel:
  - `txm-secure-channel-base = 0x4000` (IOVA, 64-bit: `{0x4000, 0}`)
  - `txm-secure-channel-size = 0x4000` (16 KiB, 64-bit: `{0x4000, 0}`)
  - `vm-base = 0x4000`, `vm-reserve = {0x4000, 0xc000}`,
    `allow-vm-reserve-mapping` (empty/flag).
  So the SEP DART must tolerate a reserved 16 KiB VM window at IOVA `0x4000`
  for the TXM secure channel. This is the kernel↔TXM shared-memory path and is
  a later milestone (modelling the SEP↔TXM channel); it is not needed just to
  *map* the TXM image.
- **`/chosen` secureboot/cryptex1 key-scheme + certificate-format**
  (`secureboot-key-scheme`, `secureboot-certificate-format = x509`,
  `cryptex1-*`): Image4 trust-evaluation parameters that TXM/Image4 consume
  (TXM carries the matching "secureboot certificate format" / "cryptex1
  certificate format" strings). Static DT props; Inferno loads the real DT so
  they are already present — no emulator modelling needed to pass them through.

## Non-TXM changes (ordinary HW/power tuning)

`ans`/`sart-ans` (NVMe queue depth, SART throttle), `dockchannel-uart`
(`dock-wstat-mask`), `pmgr/clpc` + `pmgr/ppm` (CLPC/PPM power-management
tuning), `cpu-debug-interface` (dropped `panic-trace-mode`), `buttons`
(`aot-power`). These are parameter tweaks on already-modelled or
benignly-ignored nodes; none is expected to block boot. `nvme-queue-entries`
on `ans` is the one worth a glance if the NVMe/ANS model reads queue geometry
from the DT.

## Bottom line for issue #11

No new DT nodes to model. The only boot-critical additions are the
`dart-sep` TXM secure-channel window and the `/chosen` TXM/secureboot flags,
both of which are already in the loaded DT; the SEP-DART TXM channel only needs
emulator work when the kernel↔TXM channel itself is modelled (post image-load
milestone). Everything else is power/peripheral tuning.
