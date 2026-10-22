/*
 * Common definitions for the softmmu tlb
 *
 * Copyright (c) 2003 Fabrice Bellard
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
#ifndef EXEC_TLB_COMMON_H
    #define EXEC_TLB_COMMON_H 1

    #define CPU_TLB_ENTRY_BITS (HOST_LONG_BITS == 32 ? 4 : 5)

/* Minimalized TLB entry for use by TCG fast path. */
typedef union CPUTLBEntry
{
    struct
    {
        uintptr_t addr_read;
        /*
         * Addend to virtual address to get host address.  IO accesses
         * use the corresponding iotlb value.  It sits between addr_read
         * and addr_write so that the TCG fast path can load either
         * comparator together with the addend in one load-pair
         * instruction (tcg/aarch64).
         */
        uintptr_t addend;
        uintptr_t addr_write;
        uintptr_t addr_code;
    };
    /*
     * Padding to get a power of two size, as well as index
     * access to addr_{read,write,code}: see TLB_CMP_IDX.
     */
    uintptr_t addr_idx[(1 << CPU_TLB_ENTRY_BITS) / sizeof(uintptr_t)];
} CPUTLBEntry;

QEMU_BUILD_BUG_ON(sizeof(CPUTLBEntry) != (1 << CPU_TLB_ENTRY_BITS));

/* Index of the comparator for MMUAccessType @t in CPUTLBEntry.addr_idx. */
    #define TLB_CMP_IDX(t) ((t) + ((t) != 0))

/*
 * Data elements that are per MMU mode, accessed by the fast path.
 * The structure is aligned to aid loading the pair with one insn.
 */
typedef struct CPUTLBDescFast
{
    /* Contains (n_entries - 1) << CPU_TLB_ENTRY_BITS */
    uintptr_t mask;
    /* The array of tlb entries itself. */
    CPUTLBEntry* table;
} CPUTLBDescFast QEMU_ALIGNED(2 * sizeof(void*));

#endif /* EXEC_TLB_COMMON_H */
