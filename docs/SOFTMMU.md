# Softmmu memory path: what a guest load/store costs, and what was cut

Status: #39 (fast path) and #40 (TLB floor, `tlb_reset_dirty`) merged.
Follows [SAME-ISA-FASTPATH.md](SAME-ISA-FASTPATH.md), which found 23-25% of
the host instructions of integer code in the inline TLB lookup, and TLB
refill at 18-28% of vCPU time on `chase`/`pf`.

## The fast path

Every guest access runs an inline TLB lookup before the host access. On an
AArch64 host it was 10 instructions (+1 for an unaligned access):
`sub` (address of `CPUTLBDescFast`), `ldp` (mask, table), `and` (index),
`add` (entry), `ldr` (comparator), `ldr` (addend), [`add` (last byte)],
`and` (page bits), `cmp`, `b.ne`. #39 makes it 8 (+1):

- the `sub` folds into the `ldp`: every mmu index's `CPUTLBDescFast` is
  within LDP's immediate of env (-424..-24 bytes with 26 modes);
- `CPUTLBEntry` is `{addr_read, addend, addr_write, addr_code}`, so one
  `ldp` loads the comparator and the addend (loads at +0, stores at +8).

The remaining instructions are the minimum for a direct-mapped TLB with a
dynamic size (mask and table must be loaded) and page-crossing detection.
Upstream QEMU has not changed this path since the fork base (10.2.2
already folds the `sub`; nothing to cherry-pick).

## Why the slow path is entered

Counters on every slow-path entry from generated code (measurement-only
patch), iOS 14.8 restore boot on the macOS host, 4 vCPUs, ~10 s:

| reason | before #40 | after #40 |
|---|---|---|
| TLB miss, page-table walk | 1.21-1.38M | 0.48-0.53M |
| TLB miss, victim-TLB hit | 1.9-5.7M | 0.37-0.98M |
| MMIO | 142k | 143k |
| page-crossing access | 7.1k | 7.1k |
| watchpoint / notdirty / other | ~200 | ~200 |

Alignment faults do not appear (XNU runs with SCTLR.A clear). Fills split
roughly 90% loads, 9% stores; code fetches add 0.18M (0.11M after #40).
With 64-256 entries (where the dynamic sizing kept them: XNU flushes
often, and the sizing shrinks a TLB that was <30% full at its flushes) the
refill path was ~15% of vCPU samples; a fill (page walk + `tlb_set_page_full`)
costs ~0.25 us on the emulator (`w11`).

## TLB size

| floor | fills | victim hits | restore boot, emulator |
|---|---|---|---|
| 64 (old) | 1.2-1.4M | 1.9-5.7M | 13.91 s, 12.15 s CPU |
| 1024 (#40 default) | 0.48-0.53M | 0.37-0.98M | **13.39 s, 11.59 s CPU** |
| 4096 | 0.30-0.32M | 0.04-0.32M | 13.47 s, 11.56 s CPU |

A larger TLB was first a loss (host boot 9.9 -> 11.4 s at 4096 entries):
`tlb_reset_dirty`, run whenever a RAM page gains translated code, scanned
every entry of all 26 modes of every vCPU and read each 48-byte
`CPUTLBEntryFull` (36% of vCPU samples at 4096). #40 scans only the modes
in `tlb_c.dirty` and decides from the 32-byte entry unless it has slow
flags. Beyond 1024 the per-flush `memset` grows (7% of vCPU samples at
4096) and eats the remaining gain. `-accel tcg,x-tlb-bits=N` sets the floor.

## Both together

Android emulator "iop" (Mac mini M1), CI binaries, master 2a65516 vs
52f62f4 (#39 + #40), interleaved, iOS 14.8 restore boot to `waiting_host`,
`-smp 4`, 12 runs each: **13.84 s -> 13.11 s (-5.3%), host CPU 12.04 s ->
11.15 s (-7.4%)**; `BSD root` 3.89 -> 3.69 s. Identical milestones in all 24
runs, no panic. a13-test (median of 5): w10 0.40 -> 0.38 s, w11 0.98 ->
1.01 s (refill-bound; +3-4% in every batch, at the edge of the noise),
w9/w1/w4 within noise.

Linux 6.1 guest on upstream 10.2.2 (`-M virt`, 4 KiB pages, macOS host):
the paired load alone (10.2.2 already folds the `sub`) moves `cm`, `int`,
`chase`, `mem` and `pf` by less than the run-to-run noise (±2%), and a
larger floor does not reduce their fills, which are compulsory after
flushes.

## What did not pay off, or was not tried

- **Prefilling the other 4 KiB entries of a 16 KiB guest page**: moot,
  t8030 and a13-test already use 16 KiB target pages
  (`minimum_page_bits = 14`). A Linux guest with 4 KiB pages has nothing to
  prefill either, except block mappings.
- **A floor above 1024**: see above; it would need flushes that clear only
  the used entries (a per-TLB index log, which the ASID contexts would
  have to copy and swap too).
- **A second-level / per-TB page cache** (reuse a translation for repeated
  same-page accesses within a TB): not prototyped. After #40 the victim TLB
  absorbs conflict misses for ~0.3% of vCPU time and the remaining fills
  are mostly compulsory after flushes, which no cache keeps. Within a TB it
  would save ~5 of the 8 instructions per repeated access, at the cost of
  TCG IR support for host-address values that live across guest
  instructions, and it widens a cross-vCPU race: `tlb_reset_dirty` sets
  NOTDIRTY on another vCPU's write entries without stopping it, so a store
  through a cached addend could miss a code page that became translated
  mid-TB. The cache would have to be dropped at every slow path and helper
  call and could not cover stores. Expected gain at most that of #39.

## Reproducing

- `hw/arm/tests/run-a13-bench.py --only w10,w11 OLD=... NEW=...`: w10 is
  TLB-hit loads/stores (fast path), w11 loads scattered over 4096 pages
  (refills; it fits a 4096-entry TLB, `x-tlb-bits=12`).
- Restore boot: ios-on-pixel `tools/restore_bench.py`; for the old floor
  add `x-tlb-bits=6` to its `-accel tcg,...` option.
