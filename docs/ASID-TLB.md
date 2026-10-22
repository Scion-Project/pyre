# ASID TLB contexts: no TLB refill on every address-space switch

Status: implemented (`-accel tcg,asid-tlbs=N`, default 8; `x-asid-jc`,
default on). This is item 3 of [SAME-ISA-FASTPATH.md](SAME-ISA-FASTPATH.md).

## The cost

QEMU's softmmu TLB is not tagged with an address space id. When the guest
writes TTBR0_EL1/TTBR1_EL1 with a different ASID, Inferno dropped every
non-global (nG) entry of the EL1&0 regime and the jump-cache slots that
were filled through such entries (upstream QEMU flushes the whole TLB and
the whole jump cache). When the guest switches back, every page is walked
again and every indirect branch target is looked up in the global TB hash
table.

Measured with flush counters per caller (a measurement-only patch, not
merged):

| workload | ASID changes | TLB fills | jump-cache misses | other flushes |
|---|---|---|---|---|
| Linux 6.1 KASLR (KPTI) `sys`: 1.6M `getppid` (upstream 10.2.2, 1 vCPU) | 3.2M (82k/s), reuse distance always 1 | 87M | 66M | 1.5k range TLBIs/s |
| iOS 14.8 restore boot to `waiting_host` (Inferno, 4 vCPUs, ~10 s) | 553 (46/s); reuse depth 1: 84%, 2: 10%, 3-5: 3%, >16: 3% | 1.46M | 1.45M | 10142 batched TLBI drains, 3148 range TLBIs, 877 full flushes from memory-map changes (`tcg_commit`), 3 TCR writes, 15 resets |

KPTI switches ASID twice per system call and maps the kernel nG, so every
system call refilled the TLB and the jump cache from scratch: 23 µs per
`getppid` under TCG against 2-3 µs without KPTI (`nokaslr`). XNU maps its
kernel global and switched ASID only 46 times a second during the restore
boot, so it was already cheap there.

Which guest operations flush what:

| guest operation | before | now |
|---|---|---|
| TTBRx_EL1 write that changes an ASID field (AArch64, EL1&0) | drop nG entries of the EL1&0 indexes, clear the matching jump-cache slots | swap with the kept-aside TLBs and jump cache of the incoming address space (miss: as before) |
| TLBI VAE1/VALE1/VAAE1/RVAE1 (IS) | flush the pages from the EL1&0 indexes, all ASIDs (QEMU ignores the ASID) | same, also applied to the kept-aside TLBs and jump caches |
| TLBI ASIDE1, VMALLE1 | flush the EL1&0 indexes | same, kept-aside copies of those indexes dropped |
| TCR_EL1, SCTLR_EL1, HCR_EL2.{VM,PTW,DC,...}, SCR_EL3.NS, memory-map change | full flush | same, kept-aside copies dropped |
| SPRR_CONFIG_EL0/EL1, SPRR_EL0BR0/EL1BR1 change from EL1 | nothing (entries kept stale permissions) | flush (#34) |
| exception level change, PAN, GXF entry | nothing (separate mmu indexes) | nothing |

TLBI by ASID could in principle invalidate only that ASID's entries, but
QEMU does not record the ASID per entry, it is rare in both guests, and
over-invalidation is always correct, so it still drops everything.

## Design

`tlb_switch_asid_ctx()` (accel/tcg/cputlb.c) keeps, per vCPU, up to N
slots, each holding the TLBs (`CPUTLBDesc` + `CPUTLBDescFast`, by pointer)
of the EL1&0 mmu indexes and the jump cache of an address space the guest
left. The key is every register the EL1&0 page-table walk reads whose
writes do not flush: TTBR0_EL1, TTBR1_EL1 (ASIDs and table bases),
VTTBR/VTCR (and the secure ones), MAIR_EL1, SCR_EL3 and Apple's SPRR
configuration and permission registers. On a switch the live TLBs move into
a slot under the outgoing key; if a slot holds the incoming key, its TLBs
and jump cache become live (pointer swaps), otherwise the switch behaves
exactly as before (copy the outgoing TLBs aside, drop the nG entries).

Invariant: a slot holds exactly what the live TLB would hold had the guest
stayed in that address space and run nothing. Fills only go to the live
TLB; every other change to TLB contents is applied to the slots too, at the
same time and under `tlb_c.lock`: full flushes drop the index from every
slot, page and range flushes (including the batched TLBI drain) are applied
to each slot's copy, dirty-tracking resets scan the slots (so a write
through a restored entry still traps for self-modifying code and
migration). Because QEMU's TLBIs ignore the ASID, every entry a slot brings
back was filled under that address space and not invalidated since: what
an ASID-tagged hardware TLB may return. Indexes held by a slot keep their
`tlb_c.dirty` bit, so remote broadcast flushes are never skipped for a vCPU
whose slots need them.

Kept-aside jump caches get the same treatment: page and range flushes clear
their entries for the flushed pages; full TLB flushes, `tb_flush` (TB
memory is reused), reset and dropping an index from the slot mark them
stale. TB invalidation needs nothing (a lookup never accepts a CF_INVALID
TB, and TBs are only reused after `tb_flush`).

TLBs are only resized at flushes; a switch that keeps them is not a flush,
so a switch grows the live TLB when a flush would (shrinking is left to
real flushes). This also needed `n_used_entries` to be exact (#37): it used
to wrap below zero and make TLBs double for no reason.

Only the EL1&0 regime uses it (not HCR_EL2.{E2H,TGE} = 11), and only for
AArch64 TTBR writes. Memory: one jump cache (516 KiB) per used slot, and
the slot's TLB arrays.

## Knobs

- `-accel tcg,asid-tlbs=N`: slots per vCPU, 0-16, default 8; 0 = the old
  behaviour.
- `-accel tcg,x-asid-jc=off`: keep TLBs but not jump caches.
- `info jit` prints `ASID TLB switches  <n> (<m> restored)`.

## Results

a13-test microbenchmark w9 (`hw/arm/tests/tcg_bench.S`, KPTI-style: two
ASIDs, all mappings nG, 2M switches), Android emulator "iop" on the Mac
mini M1 (the target platform), median of 5:

| | asid-tlbs=0 | TLBs | TLBs + jump caches |
|---|---|---|---|
| w9 ASID switches | 7.55 s | 2.13 s (3.5x) | 0.98 s (7.7x) |
| w1 BL/RET (control) | 2.19 s | 2.14 s | 2.15 s |
| w2 BLR/RET 4096 targets (control) | 3.96 s | 4.01 s | 4.00 s |
| w4 ALU (control) | 2.88 s | 2.89 s | 2.90 s |

Linux 6.1 KASLR guest (KPTI) on upstream QEMU 10.2.2 with a port of this
code (Inferno has no `virt` machine), macOS host, median of 3, seconds:

| workload | off | TLBs | TLBs + jump caches |
|---|---|---|---|
| sys (1.6M getppid) | 37.4 | 16.3 | 6.1 (6.1x; `nokaslr` reference 4.9) |
| pf (mmap/touch/munmap) | 5.90 | 4.94 | 4.40 |
| mem (triad) | 6.22 | 5.59 | 5.64 |
| chase | 1.29 | 1.00 | 1.01 |
| int / simd / cm | 1.37 / 6.82 / 2.07 | 1.36 / 6.79 / 2.05 | 1.41 / 6.76 / 2.09 |

With both, the `sys` run does 0.11M TLB fills instead of 87M and 0.78M
jump-cache misses instead of 66M (3.2M switches, 2 misses).

iOS 14.8 restore boot (emulator, `-smp 4`, to `waiting_host`, 10
interleaved runs each, CI Android binaries): master 13.82 s [13.55-14.32],
host CPU 11.96 s; TLBs 13.83 s, 12.03 s; TLBs + jump caches 13.68 s,
11.96 s. No measurable change, as expected from 46 switches a second; all
milestones reached in every run, no panic in the 50 boots with contexts on
(the `Kernel data abort at pc 0xfffffff0099126d4` panic seen about once in
40-50 boots also happens on unmodified master).

## Tests

`hw/arm/tests/asid_tlb_test.S` (run by `run-a13-tests.py` with 8 slots, 1
slot, no jump caches, and 0): 12 ASIDs mapping one VA to different pages;
TLBI VAE1IS/VALE1IS/VAAE1IS/RVAE1IS/VMALLE1IS/ASIDE1IS of a kept-aside
ASID; self-modifying code through a kept-aside writable alias; one code VA
holding different code in two ASIDs, called through the jump cache and
remapped while kept aside; and a probe (with its negative control) showing
that the contexts are really used. Disabling, one at a time, the kept-aside
range-flush, dirty-reset, drop and jump-cache page-clear hooks makes it fail
(checks 55, 68, 64, 83). Not covered by the test: the page-flush hook (only
reached through watchpoints and AArch32 TLBIs) and the stale-marking from
`tb_flush`/reset (the drop path marks jump caches stale first).

## Residual risk

- A guest that changes a page-table entry without a TLBI and relies on an
  ASID switch to make QEMU drop the old translation now sees the old
  translation after switching back (the probe in the test shows exactly
  that). The architecture allows it (TLB entries are tagged by ASID; the
  break-before-make sequence needs a TLBI), and hardware ASID-tagged TLBs
  behave the same way. `asid-tlbs=0` restores the old behaviour.
- Correctness depends on every TLB change outside fills going through
  `cputlb.c`'s flush and dirty-tracking functions. New code that edits TLB
  entries directly would have to handle the slots too.
- Translation state that is folded into TLB entries at fill time and can
  change without a flush must be in the key; the key covers what ptw.c
  reads today. Registers added to the walk later must be added there.
