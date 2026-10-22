/*
 * internal execution defines for qemu
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "exec/vaddr.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "qemu/xxhash.h"
#include "tb-jmp-cache.h"

#ifdef CONFIG_SOFTMMU

    /* Only the bottom TB_JMP_PAGE_BITS of the jump cache hash bits vary for
       addresses on the same page.  The top bits are the same.  This allows
       TLB invalidation to quickly clear a subset of the hash table.  */
    #define TB_JMP_PAGE_BITS 8
    #define TB_JMP_PAGE_SIZE (1 << TB_JMP_PAGE_BITS)
    #define TB_JMP_ADDR_MASK (TB_JMP_PAGE_SIZE - 1)
    #define TB_JMP_PAGE_MASK (TB_JMP_CACHE_SIZE - TB_JMP_PAGE_SIZE)

static inline unsigned int tb_jmp_cache_hash_page(vaddr pc)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK;
}

/*
 * The in-page index starts at bit 2: A64 instructions are 4-byte aligned, and
 * folding from bit 0 (as for byte-aligned ISAs) leaves the index's low bits
 * correlated with its high bits, so a 16 KiB page reaches only part of its group.
 */
static inline unsigned int tb_jmp_cache_hash_func(vaddr pc)
{
    vaddr tmp;
    tmp = pc ^ (pc >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS));
    return (((tmp >> (TARGET_PAGE_BITS - TB_JMP_PAGE_BITS)) & TB_JMP_PAGE_MASK)
            | (((pc >> 2) ^ (pc >> 6)) & TB_JMP_ADDR_MASK));
}

/*
 * True if the in-group part of tb_jmp_cache_hash_func() (bits 2 to
 * TB_JMP_PAGE_BITS + 5 of the PC) lies within the page offset, so a TB at a
 * given page offset can only be in one slot per group (true for 16 KiB
 * target pages, as on t8030).
 */
static inline bool tb_jmp_cache_slot_in_page(void) { return TARGET_PAGE_BITS >= TB_JMP_PAGE_BITS + 6; }

#else

/* In user-mode we can get better hashing because we do not have a TLB */
static inline unsigned int tb_jmp_cache_hash_func(vaddr pc)
{ return (pc ^ (pc >> TB_JMP_CACHE_BITS)) & (TB_JMP_CACHE_SIZE - 1); }

static inline bool tb_jmp_cache_slot_in_page(void) { return false; }
    #define TB_JMP_PAGE_SIZE 1
    #define TB_JMP_ADDR_MASK 0

#endif /* CONFIG_SOFTMMU */

static inline uint32_t tb_hash_func(tb_page_addr_t phys_pc, vaddr pc, uint32_t flags, uint64_t flags2, uint32_t cf_mask)
{ return qemu_xxhash8(phys_pc, pc, flags2, flags, cf_mask); }
