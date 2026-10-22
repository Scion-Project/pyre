/*
 * TCG accelerator options for executing guest integer code as the same host
 * instructions (same-ISA translation, docs/NATIVE-INT.md)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_NATIVE_INT_H
#define ACCEL_TCG_NATIVE_INT_H

/*
 * -accel tcg,native-int=on|off (default off): on an AArch64 host, an
 * AArch64 target may emit covered EL0 integer instructions, loads/stores
 * and conditional branches as the same host instructions (tcg/tcg-sit.h).
 * Forced off on other hosts.
 */
extern bool tcg_native_int;
/* -accel tcg,x-native-int-ldst=off: diagnostic, keep loads and stores on TCG. */
extern bool tcg_native_int_ldst;
/* -accel tcg,x-native-int-el1=on: experimental, also at EL1 (measurement only). */
extern bool tcg_native_int_el1;
/* -accel tcg,x-native-int-fault=on: test only, slow paths skip writing back the guest registers. */
extern bool tcg_native_int_fault;

#endif
