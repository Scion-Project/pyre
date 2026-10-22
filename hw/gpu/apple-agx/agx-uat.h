/*
 * Apple AGX GPU: UAT (GPU MMU) page-table walker.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/*
 * The UAT is an ARM64-format stage-1 page table with a 16 KiB granule:
 * three levels of 2048 eight-byte entries, each level resolving 11 VA bits
 * (L1 [IAS-1:36], L2 [35:25], L3 [24:14]). Table and page descriptors both
 * have type bits [1:0] == 3; bit 0 alone is a valid block (L1/L2 only).
 * Bits [51:14] hold the output address, [63:52] are software/permission
 * bits we ignore. The kernel (TTBR1) half is the top of the 64-bit space;
 * only the low IAS bits of a VA index the tables.
 *
 * This file uses only standard C so the host unit tests
 * (hw/arm/tests/run-host-tests.sh) can build it without QEMU.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define AGX_UAT_PGBIT  (14)
#define AGX_UAT_PGSZ   (1ULL << AGX_UAT_PGBIT)
#define AGX_UAT_LVBIT  (11)
#define AGX_UAT_LEVELS (3)

#define AGX_UAT_PTE_VALID     (1ULL << 0)
#define AGX_UAT_PTE_TABLE     (1ULL << 1)
#define AGX_UAT_PTE_AF        (1ULL << 10)
#define AGX_UAT_PTE_ADDR_MASK (0x000FFFFFFFFFC000ULL)
/* TTBR: table base in [47:14], valid in bit 0, ASID from bit 48 up. */
#define AGX_UAT_TTBR_ADDR_MASK (0x0000FFFFFFFFC000ULL)
#define AGX_UAT_TTBR_VALID     (1ULL << 0)
#define AGX_UAT_TTBR_ASID(_t)  ((_t) >> 48)

/* Reads one 64-bit little-endian word of guest physical memory. */
typedef bool (*AgxUatRead64)(void* opaque, uint64_t pa, uint64_t* val);

typedef enum
{
    AGX_UAT_OK = 0,
    AGX_UAT_FAULT_RANGE,   /* VA outside the IAS window of either half */
    AGX_UAT_FAULT_INVALID, /* descriptor not valid at `level` */
    AGX_UAT_FAULT_BUS,     /* reading a descriptor failed */
} AgxUatResult;

typedef struct
{
    uint64_t pa;       /* translated physical address */
    uint64_t pte;      /* leaf descriptor */
    uint64_t map_size; /* size of the leaf mapping (page or block) */
    int      level;    /* level the walk stopped at (1..3) */
} AgxUatWalk;

/* Index bits for a VA. `ias` is the input address size (e.g. 39). */
static inline bool agx_uat_va_in_range(uint64_t va, unsigned ias)
{
    uint64_t hi = va >> ias;
    return hi == 0 || hi == (UINT64_MAX >> ias);
}

/*
 * Walk the table rooted at `ttb` (a TTBR value: base, valid bit, ASID)
 * for `va`. Lower-half VAs belong to TTBR0, upper-half ones to TTBR1: the
 * caller picks which root to pass.
 */
AgxUatResult agx_uat_walk(AgxUatRead64 read64, void* opaque, uint64_t ttb, uint64_t va, unsigned ias,
                          AgxUatWalk* out);
