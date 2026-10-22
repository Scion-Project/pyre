/*
 * Apple SEP: recognise a SEP ROM that can only idle.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

/*
 * Return true if the AArch64 SEP ROM in @rom (@size bytes, loaded at the SEP's
 * reset address) can do nothing but idle. Execution is followed from the first
 * instruction: it must reach a loop made only of WFI, WFE, NOP, YIELD and
 * unconditional immediate branches (B) that stay inside the image, before any
 * other instruction. Placeholder ROMs such as `b .` or `1: wfi; b 1b` qualify;
 * a real SEPROM does not (its reset path loads, stores and writes system
 * registers straight away).
 *
 * Out of reset all of DAIF is masked, and such a loop never unmasks it, never
 * accesses memory and never changes a register other than the PC, so a core
 * running it is indistinguishable from a core that is powered off.
 *
 * Only the standard C headers are used, so the function can be built into a
 * host-side test (hw/arm/tests/sep_rom_idle_test.c).
 */
bool apple_sep_rom_is_idle_loop(const void* rom, size_t size);
