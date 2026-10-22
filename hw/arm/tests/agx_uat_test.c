/*
 * Host-side test for the AGX UAT walker (hw/gpu/apple-agx/agx-uat.c).
 * Built and run by hw/arm/tests/run-host-tests.sh; no guest firmware needed.
 *
 * Builds synthetic 16 KiB-granule page tables in a fake physical memory and
 * checks translations, the user/kernel halves, blocks and every fault kind.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "qemu/osdep.h"
#include "../../gpu/apple-agx/agx-uat.h"

#define MEM_BASE 0x800000000ULL
#define MEM_SIZE (64 * AGX_UAT_PGSZ)
#define IAS      39

static uint8_t  mem[MEM_SIZE];
static unsigned next_page = 1;
static int      failures;
static int      reads;

static bool rd(void* opaque, uint64_t pa, uint64_t* val)
{
    (void)opaque;
    reads++;
    if (pa < MEM_BASE || pa + 8 > MEM_BASE + MEM_SIZE || (pa & 7) != 0) { return false; }
    memcpy(val, mem + (pa - MEM_BASE), 8);
    return true;
}

static uint64_t new_table(void) { return MEM_BASE + (uint64_t)(next_page++) * AGX_UAT_PGSZ; }

static void put(uint64_t pa, uint64_t v) { memcpy(mem + (pa - MEM_BASE), &v, 8); }

static uint64_t get(uint64_t pa)
{
    uint64_t v;
    memcpy(&v, mem + (pa - MEM_BASE), 8);
    return v;
}

static unsigned idx(uint64_t va, int level)
{
    unsigned shift = AGX_UAT_PGBIT + (AGX_UAT_LEVELS - level) * AGX_UAT_LVBIT;
    return ((va & ((1ULL << IAS) - 1)) >> shift) & ((1u << AGX_UAT_LVBIT) - 1);
}

/* Map one 16 KiB page, allocating tables as needed. High bits mimic the
 * permission/attribute bits a real driver sets; the walker must ignore them. */
static void map_page(uint64_t ttb, uint64_t va, uint64_t pa)
{
    uint64_t table = ttb;
    for (int level = 1; level < AGX_UAT_LEVELS; level++) {
        uint64_t slot = table + idx(va, level) * 8;
        uint64_t pte  = get(slot);
        if ((pte & AGX_UAT_PTE_VALID) == 0) {
            pte = new_table() | 3;
            put(slot, pte);
        }
        table = pte & AGX_UAT_PTE_ADDR_MASK;
    }
    put(table + idx(va, AGX_UAT_LEVELS) * 8, (pa & AGX_UAT_PTE_ADDR_MASK) | (0x6ULL << 52) | AGX_UAT_PTE_AF | (2 << 6) | 3);
}

static void expect_ok(const char* name, uint64_t ttb, uint64_t va, uint64_t want_pa, uint64_t want_size)
{
    AgxUatWalk   w = {0};
    AgxUatResult r = agx_uat_walk(rd, NULL, ttb, va, IAS, &w);
    bool         ok = r == AGX_UAT_OK && w.pa == want_pa && w.map_size == want_size;
    printf("%-4s %s: va 0x%016llx -> r=%d pa 0x%llx size 0x%llx (want 0x%llx/0x%llx)\n", ok ? "ok" : "FAIL", name,
           (unsigned long long)va, r, (unsigned long long)w.pa, (unsigned long long)w.map_size,
           (unsigned long long)want_pa, (unsigned long long)want_size);
    if (!ok) { failures++; }
}

static void expect_fault(const char* name, uint64_t ttb, uint64_t va, AgxUatResult want, int want_level)
{
    AgxUatWalk   w = {0};
    AgxUatResult r = agx_uat_walk(rd, NULL, ttb, va, IAS, &w);
    bool         ok = r == want && (want_level == 0 || w.level == want_level);
    printf("%-4s %s: va 0x%016llx -> r=%d level %d (want r=%d level %d)\n", ok ? "ok" : "FAIL", name,
           (unsigned long long)va, r, w.level, want, want_level);
    if (!ok) { failures++; }
}

int main(void)
{
    uint64_t ttb0 = new_table(); /* user half */
    uint64_t ttb1 = new_table(); /* kernel/firmware half */
    uint64_t kva  = 0xffffffa000000000ULL;
    uint64_t l2;

    map_page(ttb0, 0x4000, 0x900000000ULL);
    map_page(ttb0, 0x4000 + AGX_UAT_PGSZ, 0x900123000ULL & ~(AGX_UAT_PGSZ - 1));
    map_page(ttb0, (1ULL << IAS) - AGX_UAT_PGSZ, 0x9abcd0000ULL & ~(AGX_UAT_PGSZ - 1));
    map_page(ttb1, kva, 0x880000000ULL);
    map_page(ttb1, kva + 0x1234000, 0x880004000ULL);

    expect_ok("user page", ttb0, 0x4000, 0x900000000ULL, AGX_UAT_PGSZ);
    expect_ok("user page offset", ttb0, 0x4000 + 0x123, 0x900000123ULL, AGX_UAT_PGSZ);
    expect_ok("user next page", ttb0, 0x8000 + 0x10, 0x900120010ULL, AGX_UAT_PGSZ);
    expect_ok("user top of IAS", ttb0, (1ULL << IAS) - 8, (0x9abcd0000ULL & ~(AGX_UAT_PGSZ - 1)) + AGX_UAT_PGSZ - 8,
              AGX_UAT_PGSZ);
    expect_ok("kernel page", ttb1, kva + 0x40, 0x880000040ULL, AGX_UAT_PGSZ);
    expect_ok("kernel page 2", ttb1, kva + 0x1234000 + 0x3ff8, 0x880007ff8ULL, AGX_UAT_PGSZ);
    /* TTBR low bits (valid bit, ASID in [63:48]) must not affect the walk. */
    expect_ok("ttbr with valid+asid", ttb1 | 1 | (5ULL << 48), kva, 0x880000000ULL, AGX_UAT_PGSZ);

    /* A 32 MiB block at L2. */
    l2 = get(ttb0 + idx(0x4000, 1) * 8) & AGX_UAT_PTE_ADDR_MASK;
    put(l2 + 5 * 8, 0x940000000ULL | AGX_UAT_PTE_AF | 1);
    expect_ok("L2 block", ttb0, 5ULL * (1ULL << 25) + 0x12345, 0x940012345ULL, 1ULL << 25);

    expect_fault("unmapped L1", ttb0, 1ULL << 37, AGX_UAT_FAULT_INVALID, 1);
    expect_fault("unmapped L3", ttb0, 0xc000, AGX_UAT_FAULT_INVALID, 3);
    expect_fault("non-canonical VA", ttb0, 1ULL << 45, AGX_UAT_FAULT_RANGE, 0);
    expect_fault("bad table PA", MEM_BASE + MEM_SIZE, 0x4000, AGX_UAT_FAULT_BUS, 0);

    /* An L3 descriptor with only the valid bit is reserved. */
    {
        AgxUatWalk w = {0};
        agx_uat_walk(rd, NULL, ttb1, kva, IAS, &w);
        uint64_t l3 = (get((get(ttb1 + idx(kva, 1) * 8) & AGX_UAT_PTE_ADDR_MASK) + idx(kva, 2) * 8) &
                       AGX_UAT_PTE_ADDR_MASK) + idx(kva, 3) * 8;
        put(l3, get(l3) & ~AGX_UAT_PTE_TABLE);
        expect_fault("L3 reserved encoding", ttb1, kva, AGX_UAT_FAULT_INVALID, 3);
    }

    printf("%s (%d reads)\n", failures ? "FAILED" : "PASS", reads);
    return failures ? 1 : 0;
}
