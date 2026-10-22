/*
 * TCG accelerator options for executing guest floating point on the host FPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_NATIVE_FP_H
#define ACCEL_TCG_NATIVE_FP_H

/* -accel tcg,native-fp=on: targets may run guest FP on the host FPU where bit-exact (docs/NATIVE-FP.md). */
extern bool tcg_native_fp;
/* -accel tcg,x-native-fp-fault=on: test only, the target injects a known-wrong native operation. */
extern bool tcg_native_fp_fault;
/* -accel tcg,x-native-fp-selftest=off: test only, skip the target's host self-test. */
extern bool tcg_native_fp_selftest;
/* -accel tcg,x-native-fp-inline=off: diagnostic, keep native FP inside helpers (no inline host FP code). */
extern bool tcg_native_fp_inline;

#endif
