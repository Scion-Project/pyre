/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include "qemu/rcu.h"
#include "qemu/bitops.h"
#include "exec/cpu-common.h"

/*
 * 16384 entries: 64 page groups of 256 (see tb-hash.h). With 4096 entries
 * (64 x 64) XNU's hot kernel pages hold more indirect-branch targets than a
 * group has slots, and most misses were two targets in the same page evicting
 * each other.
 */
#define TB_JMP_CACHE_BITS 14
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache
{
    struct rcu_head rcu;
    /*
     * Bit i is clear only if array[i] was filled while every page of its TB
     * was mapped by a global (not ASID-tagged) TLB entry, so the entry can
     * survive tlb_flush_asid_tagged_by_mmuidx() just like that TLB entry does.
     * Set bits on empty entries are harmless. Only the owning CPU reads or
     * writes this bitmap.
     */
    unsigned long asid_tagged[BITS_TO_LONGS(TB_JMP_CACHE_SIZE)];
    struct
    {
        TranslationBlock* tb;
        vaddr             pc;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;
