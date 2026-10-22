/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Jump cache lookup for a target's own lookup_and_goto_ptr helper.
 */

#pragma once

#include "exec/vaddr.h"

typedef struct CPUState CPUState;

/**
 * tcg_lookup_tb_ptr:
 * @cpu: the current CPU
 * @pc, @cs_base, @flags: the TB state, exactly as the target's
 *     TCGCPUOps.get_tb_cpu_state() would return it
 *
 * The body of helper_lookup_tb_ptr() after the state is known, for a target
 * helper that computes the state inline rather than through TCGCPUOps.
 * Returns the host code of the TB to run next, or tcg_code_gen_epilogue to
 * return to the main loop. Only call it from a lookup_and_goto_ptr helper.
 */
const void* tcg_lookup_tb_ptr(CPUState* cpu, vaddr pc, uint64_t cs_base, uint32_t flags);
