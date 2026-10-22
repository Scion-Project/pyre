/*
 * Apple AGX GPU: UAT (GPU MMU) page-table walker.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "qemu/osdep.h"
#include "agx-uat.h"

AgxUatResult agx_uat_walk(AgxUatRead64 read64, void* opaque, uint64_t ttb, uint64_t va, unsigned ias,
                          AgxUatWalk* out)
{
    uint64_t table = ttb & AGX_UAT_TTBR_ADDR_MASK;
    uint64_t lva;
    int      level;

    if (!agx_uat_va_in_range(va, ias)) { return AGX_UAT_FAULT_RANGE; }
    lva = va & ((1ULL << ias) - 1);

    for (level = 1; level <= AGX_UAT_LEVELS; level++) {
        unsigned shift = AGX_UAT_PGBIT + (AGX_UAT_LEVELS - level) * AGX_UAT_LVBIT;
        uint64_t idx   = (lva >> shift) & ((1ULL << AGX_UAT_LVBIT) - 1);
        uint64_t pte;

        if (!read64(opaque, table + idx * 8, &pte)) { return AGX_UAT_FAULT_BUS; }
        if (out != NULL) {
            out->level = level;
            out->pte   = pte;
        }
        if ((pte & AGX_UAT_PTE_VALID) == 0) { return AGX_UAT_FAULT_INVALID; }
        if (level == AGX_UAT_LEVELS || (pte & AGX_UAT_PTE_TABLE) == 0) {
            /* Leaf: a page at L3; a block at L1/L2. An L3 entry without the
             * table/page bit is reserved in ARM64 format. */
            uint64_t size = 1ULL << shift;
            if (level == AGX_UAT_LEVELS && (pte & AGX_UAT_PTE_TABLE) == 0) { return AGX_UAT_FAULT_INVALID; }
            if (out != NULL) {
                out->map_size = size;
                out->pa       = (pte & AGX_UAT_PTE_ADDR_MASK & ~(size - 1)) | (lva & (size - 1));
            }
            return AGX_UAT_OK;
        }
        table = pte & AGX_UAT_PTE_ADDR_MASK;
    }
    return AGX_UAT_FAULT_INVALID;
}
