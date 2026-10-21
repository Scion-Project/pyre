/*
 * iOS 27+ SPTM boot layout.
 *
 * On SPTM boots iBoot does not hand the kernel a linear kernelcache. It
 * places SPTM, TXM, the trust cache, the boot kernelcache and the device tree
 * as classed "image regions", and describes each one in /chosen/memory-map as
 * "<image>-<class>" = { paddr, length } (the same 16-byte shape as
 * "Kernel-<segment>"). SPTM looks these up during sptm_init()
 * (init_get_image_region) and panics if a required one is missing. It then
 * builds the kernel's and TXM's page tables from them, launches TXM with
 * arguments it constructs itself, and enters the kernel.
 *
 * What this file reproduces, all read from the 27.0.1 t8030 SPTM image:
 *
 * - SPTM's validate_region_order() walks a fixed table and requires each
 *   present region to start exactly where the previous one ended:
 *     TXM-ro TXM-rx TXM-bx TrustCache [AuxKC-ro] [AuxKC-rx]
 *     BootKC-rx BootKC-bx BootKC-ro BootKC-rs [CL4-rx] [CL4-ro]
 *     DeviceTree SPTM-ro SPTM-rx SPTM-rw
 *   ([..] = optional; an optional region may be absent or have paddr -1).
 *   So the kernel's executable segments sit physically *below* its
 *   read-only ones, the reverse of their link order.
 * - TXM-rw/-le, BootKC-rw/-le, SPTM-rm/-le are also required, outside the
 *   chain. "-entry" and "-virt" hold link addresses in their first word:
 *   SPTM computes an image's entry PA as region_pa + (entry - virt), using
 *   the ro region (TXM, BootKC) or rx region (CL4).
 * - "slide" holds the kernel's virtual slide, which SPTM passes on to XNU.
 * - CL4 (the exclaves kernel) lookups of CL4-entry/-rx/-virt are
 *   unconditional, and a zero-sized CL4-rx panics ("Required PAPT range has
 *   no mappings"). SPTM only starts it ("Starting SK...") when the device
 *   tree has exclaves-enabled, which t8030 does not. So CL4-rx is a single
 *   zeroed placeholder page inside the chain, and CL4-ro is left out.
 * - SPTM runs with the MMU off first and addresses its own data
 *   PC-relatively, so SPTM itself stays linear (ro, rx, rw, rm, le in link
 *   order), and its physical address must be congruent to its link address
 *   modulo 32 MiB (the start.s-style rounding it applies to phys_base).
 *
 * Only structural facts (region names, order and size rules) are encoded
 * here; no Apple code or data.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/sptm-boot.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "system/address-spaces.h"

#define SPTM_PAGE (0x4000ULL)

static const char* const apple_sptm_class_suffix[APPLE_SPTM_CLASS_COUNT] = {
    [APPLE_SPTM_RO] = "ro", [APPLE_SPTM_RX] = "rx", [APPLE_SPTM_BX] = "bx", [APPLE_SPTM_RS] = "rs",
    [APPLE_SPTM_RW] = "rw", [APPLE_SPTM_RM] = "rm", [APPLE_SPTM_LE] = "le",
};

static AppleSptmClass apple_sptm_classify(const MachoSegmentCommand64* seg)
{
    static const struct
    {
        const char*    name;
        AppleSptmClass cls;
    } by_name[] = {
        {"__TEXT", APPLE_SPTM_RO},        {"__PRELINK_TEXT", APPLE_SPTM_RO},  {"__DATA_CONST", APPLE_SPTM_RO},
        {"__LATE_CONST", APPLE_SPTM_RO},  {"__TEXT_EXEC", APPLE_SPTM_RX},     {"__TEXT_BOOT_EXEC", APPLE_SPTM_BX},
        {"__DATA_SPTM", APPLE_SPTM_RS},   {"__BOOTDATA", APPLE_SPTM_RM},      {"__LINKEDIT", APPLE_SPTM_LE},
    };

    for (size_t i = 0; i < ARRAY_SIZE(by_name); ++i) {
        if (strncmp(seg->segname, by_name[i].name, sizeof(seg->segname)) == 0) { return by_name[i].cls; }
    }
    return (seg->initprot & VM_PROT_EXECUTE) != 0 ? APPLE_SPTM_RX : APPLE_SPTM_RW;
}

typedef struct
{
    MachoHeader64*    hdr;
    MachoLoadCommand* cmd;
    uint32_t          index;
} AppleSptmSegIter;

static void apple_sptm_seg_iter_init(AppleSptmSegIter* it, MachoHeader64* hdr)
{
    it->hdr   = hdr;
    it->cmd   = (MachoLoadCommand*)(hdr + 1);
    it->index = 0;
}

// Next loaded (non-empty, non-__PAGEZERO) top-level segment, or NULL.
static MachoSegmentCommand64* apple_sptm_seg_next(AppleSptmSegIter* it)
{
    while (it->index < it->hdr->n_cmds) {
        MachoLoadCommand* cmd = it->cmd;

        it->cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size);
        it->index++;
        if (cmd->cmd == LC_SEGMENT_64) {
            MachoSegmentCommand64* seg = (MachoSegmentCommand64*)cmd;
            if (seg->vmsize != 0 && strncmp(seg->segname, "__PAGEZERO", 10) != 0) { return seg; }
        }
    }
    return NULL;
}

static void apple_sptm_image_init(AppleSptmImage* img, const char* prefix, MachoHeader64* macho,
                                  uint32_t required_mask)
{
    MachoSegmentCommand64* seg;
    AppleSptmSegIter       it;
    uint64_t               member_size[APPLE_SPTM_CLASS_COUNT] = {0};
    vaddr                  end[APPLE_SPTM_CLASS_COUNT]         = {0};
    MachoLoadCommand*      cmd;

    memset(img, 0, sizeof(*img));
    img->prefix = prefix;
    img->macho  = macho;

    apple_sptm_seg_iter_init(&it, macho);
    while ((seg = apple_sptm_seg_next(&it)) != NULL) {
        AppleSptmClass   cls = apple_sptm_classify(seg);
        AppleSptmRegion* r   = &img->region[cls];

        if (member_size[cls] == 0 || seg->vmaddr < r->va) { r->va = seg->vmaddr; }
        if (seg->vmaddr + seg->vmsize > end[cls]) { end[cls] = seg->vmaddr + seg->vmsize; }
        member_size[cls] += seg->vmsize;
    }

    for (int cls = 0; cls < APPLE_SPTM_CLASS_COUNT; ++cls) {
        AppleSptmRegion* r = &img->region[cls];

        if (member_size[cls] == 0) {
            if (required_mask & BIT(cls)) {
                error_setg(&error_fatal, "%s image has no %s segments, which SPTM boots require", prefix,
                           apple_sptm_class_suffix[cls]);
            }
            continue;
        }
        // A region is one {paddr, length} range: its segments must be
        // contiguous in the link layout.
        if (end[cls] - r->va != member_size[cls] || (r->va & (SPTM_PAGE - 1)) != 0) {
            error_setg(&error_fatal, "%s image: %s segments are not page-aligned and contiguous", prefix,
                       apple_sptm_class_suffix[cls]);
        }
        r->size = ROUND_UP(member_size[cls], SPTM_PAGE);
    }

    cmd = (MachoLoadCommand*)(macho + 1);
    for (uint32_t i = 0; i < macho->n_cmds; ++i, cmd = (MachoLoadCommand*)((char*)cmd + cmd->cmd_size)) {
        if (cmd->cmd == LC_UNIXTHREAD) {
            // arm64 thread state: pc slot (as in apple_boot_load_macho()).
            img->entry_va = ldq_le_p((char*)cmd + 0x110);
        }
    }
    if (img->entry_va == 0) { error_setg(&error_fatal, "%s image has no LC_UNIXTHREAD entry point", prefix); }
}

void apple_sptm_layout_init(AppleSptmLayout* layout, MachoHeader64* sptm, MachoHeader64* txm, MachoHeader64* kc)
{
    memset(layout, 0, sizeof(*layout));
    apple_sptm_image_init(&layout->sptm, "SPTM", sptm,
                          BIT(APPLE_SPTM_RO) | BIT(APPLE_SPTM_RX) | BIT(APPLE_SPTM_RW) | BIT(APPLE_SPTM_RM)
                              | BIT(APPLE_SPTM_LE));
    apple_sptm_image_init(&layout->txm, "TXM", txm,
                          BIT(APPLE_SPTM_RO) | BIT(APPLE_SPTM_RX) | BIT(APPLE_SPTM_BX) | BIT(APPLE_SPTM_RW)
                              | BIT(APPLE_SPTM_LE));
    apple_sptm_image_init(&layout->kc, "BootKC", kc,
                          BIT(APPLE_SPTM_RO) | BIT(APPLE_SPTM_RX) | BIT(APPLE_SPTM_BX) | BIT(APPLE_SPTM_RS)
                              | BIT(APPLE_SPTM_RW) | BIT(APPLE_SPTM_LE));

    // SPTM is entered with the MMU off and must be physically linear, and
    // its ro, rx and rw regions are consecutive in the chain: they have to be
    // consecutive in its link layout too (so do rm and le after rw).
    for (int cls = APPLE_SPTM_RX; cls < APPLE_SPTM_CLASS_COUNT; ++cls) {
        const AppleSptmRegion* r = &layout->sptm.region[cls];
        if (r->size != 0 && r->va < layout->sptm.region[APPLE_SPTM_RO].va) {
            error_setg(&error_fatal, "SPTM image: %s segments lie below its read-only segments",
                       apple_sptm_class_suffix[cls]);
        }
    }
}

static void apple_sptm_set_range(AppleDTNode* memory_map, const char* name, uint64_t first, uint64_t second)
{
    uint64_t value[2];

    stq_le_p(&value[0], first);
    stq_le_p(&value[1], second);
    apple_dt_set_prop(memory_map, name, sizeof(value), value);
}

static void apple_sptm_image_props(AppleDTNode* memory_map, const AppleSptmImage* img, bool publish)
{
    char name[32];

    for (int cls = 0; cls < APPLE_SPTM_CLASS_COUNT; ++cls) {
        const AppleSptmRegion* r = &img->region[cls];
        if (r->size == 0) { continue; }
        snprintf(name, sizeof(name), "%s-%s", img->prefix, apple_sptm_class_suffix[cls]);
        apple_sptm_set_range(memory_map, name, publish ? r->pa : 0, publish ? r->size : 0);
    }
}

void apple_sptm_allocate_memory_map(AppleDTNode* memory_map, const AppleSptmLayout* layout)
{
    static const char* const extra[] = {
        "TXM-entry", "TXM-virt", "BootKC-entry", "BootKC-virt", "CL4-rx", "CL4-entry", "CL4-virt", "slide",
    };

    apple_sptm_image_props(memory_map, &layout->sptm, false);
    apple_sptm_image_props(memory_map, &layout->txm, false);
    apple_sptm_image_props(memory_map, &layout->kc, false);
    for (size_t i = 0; i < ARRAY_SIZE(extra); ++i) { apple_sptm_set_range(memory_map, extra[i], 0, 0); }
}

static hwaddr apple_sptm_congruent_pa(hwaddr min_pa, vaddr va, hwaddr modulus)
{
    hwaddr off = va & (modulus - 1);

    return ROUND_UP(min_pa - off, modulus) + off;
}

void apple_sptm_layout_place(AppleSptmLayout* layout, hwaddr min_pa, hwaddr sptm_align, uint64_t trustcache_size,
                             uint64_t device_tree_size)
{
    AppleSptmImage*  sptm = &layout->sptm;
    AppleSptmImage*  txm  = &layout->txm;
    AppleSptmImage*  kc   = &layout->kc;
    AppleSptmRegion* chain[] = {
        &txm->region[APPLE_SPTM_RO], &txm->region[APPLE_SPTM_RX], &txm->region[APPLE_SPTM_BX],
        &layout->trustcache,         &kc->region[APPLE_SPTM_RX],  &kc->region[APPLE_SPTM_BX],
        &kc->region[APPLE_SPTM_RO],  &kc->region[APPLE_SPTM_RS],  &layout->cl4_rx,
        &layout->device_tree,        &sptm->region[APPLE_SPTM_RO],
    };
    uint64_t off_sptm = 0;
    hwaddr   pa;

    layout->trustcache.size  = ROUND_UP(trustcache_size, SPTM_PAGE);
    layout->cl4_rx.size      = SPTM_PAGE;
    layout->device_tree.size = ROUND_UP(device_tree_size, SPTM_PAGE);

    for (size_t i = 0; i + 1 < ARRAY_SIZE(chain); ++i) { off_sptm += chain[i]->size; }

    layout->chain_start = apple_sptm_congruent_pa(min_pa + off_sptm, sptm->region[APPLE_SPTM_RO].va, sptm_align)
                        - off_sptm;

    pa = layout->chain_start;
    for (size_t i = 0; i < ARRAY_SIZE(chain); ++i) {
        chain[i]->pa  = pa;
        pa           += chain[i]->size;
    }

    // SPTM is linear from SPTM-ro: SPTM-rx, -rw (the end of the chain), then
    // -rm and -le, all at their link offsets.
    for (int cls = APPLE_SPTM_RX; cls < APPLE_SPTM_CLASS_COUNT; ++cls) {
        AppleSptmRegion* r = &sptm->region[cls];
        if (r->size == 0) { continue; }
        r->pa = sptm->region[APPLE_SPTM_RO].pa + (r->va - sptm->region[APPLE_SPTM_RO].va);
        pa    = MAX(pa, r->pa + r->size);
    }
    layout->chain_end = sptm->region[APPLE_SPTM_RW].pa + sptm->region[APPLE_SPTM_RW].size;

    // Outside the chain: TXM's and the kernel's writable data and linkedit.
    AppleSptmRegion* rest[] = {
        &txm->region[APPLE_SPTM_RW],
        &txm->region[APPLE_SPTM_LE],
        &kc->region[APPLE_SPTM_RW],
        &kc->region[APPLE_SPTM_LE],
    };
    for (size_t i = 0; i < ARRAY_SIZE(rest); ++i) {
        rest[i]->pa  = pa;
        pa          += rest[i]->size;
    }
    layout->end = pa;
}

static void apple_sptm_image_load(const AppleSptmImage* img, AddressSpace* as)
{
    uint8_t*               buf = apple_boot_get_macho_buffer(img->macho);
    vaddr                  kc_base;
    MachoSegmentCommand64* seg;
    AppleSptmSegIter       it;

    // The host copy is indexed from the 32 MiB-rounded image base.
    apple_boot_get_kc_bounds(img->macho, NULL, &kc_base, NULL, NULL, NULL);

    apple_sptm_seg_iter_init(&it, img->macho);
    while ((seg = apple_sptm_seg_next(&it)) != NULL) {
        const AppleSptmRegion* r  = &img->region[apple_sptm_classify(seg)];
        hwaddr                 pa = r->pa + (seg->vmaddr - r->va);
        uint64_t               n  = MIN(seg->filesize, seg->vmsize);

        address_space_write(as, pa, MEMTXATTRS_UNSPECIFIED, buf + (seg->vmaddr - kc_base), n);
        if (seg->vmsize > n) { address_space_set(as, pa + n, 0, seg->vmsize - n, MEMTXATTRS_UNSPECIFIED); }
    }
}

void apple_sptm_layout_load(const AppleSptmLayout* layout, AddressSpace* as, const uint8_t* trustcache,
                            uint64_t trustcache_size)
{
    apple_sptm_image_load(&layout->txm, as);
    apple_sptm_image_load(&layout->kc, as);
    apple_sptm_image_load(&layout->sptm, as);

    address_space_set(as, layout->trustcache.pa, 0, layout->trustcache.size, MEMTXATTRS_UNSPECIFIED);
    address_space_write(as, layout->trustcache.pa, MEMTXATTRS_UNSPECIFIED, trustcache, trustcache_size);
    address_space_set(as, layout->cl4_rx.pa, 0, layout->cl4_rx.size, MEMTXATTRS_UNSPECIFIED);
}

hwaddr apple_sptm_entry_pa(const AppleSptmImage* img)
{
    for (int cls = 0; cls < APPLE_SPTM_CLASS_COUNT; ++cls) {
        const AppleSptmRegion* r = &img->region[cls];
        if (r->size != 0 && img->entry_va >= r->va && img->entry_va < r->va + r->size) {
            return r->pa + (img->entry_va - r->va);
        }
    }
    error_setg(&error_fatal, "%s entry point 0x%016" VADDR_PRIx " is outside its segments", img->prefix,
               img->entry_va);
    return 0;
}

// SPTM computes an image's entry PA as <image>-ro.paddr + (entry - virt), so
// "virt" is the link address that corresponds to the ro region under the
// image's actual (possibly permuted) placement.
static vaddr apple_sptm_virt(const AppleSptmImage* img)
{
    return img->entry_va - (apple_sptm_entry_pa(img) - img->region[APPLE_SPTM_RO].pa);
}

void apple_sptm_publish_memory_map(AppleDTNode* memory_map, const AppleSptmLayout* layout, vaddr kernel_slide)
{
    const AppleSptmImage*  kc = &layout->kc;
    MachoSegmentCommand64* seg;
    AppleSptmSegIter       it;
    char                   name[32];

    apple_sptm_image_props(memory_map, &layout->sptm, true);
    apple_sptm_image_props(memory_map, &layout->txm, true);
    apple_sptm_image_props(memory_map, kc, true);

    apple_sptm_set_range(memory_map, "TXM-entry", layout->txm.entry_va, 0);
    apple_sptm_set_range(memory_map, "TXM-virt", apple_sptm_virt(&layout->txm), 0);
    apple_sptm_set_range(memory_map, "BootKC-entry", kc->entry_va + kernel_slide, 0);
    apple_sptm_set_range(memory_map, "BootKC-virt", apple_sptm_virt(kc) + kernel_slide, 0);
    apple_sptm_set_range(memory_map, "slide", kernel_slide, 0);

    // Placeholder CL4: entry == virt makes SPTM's computed CL4 entry the page
    // itself; nothing jumps to it without exclaves-enabled.
    apple_sptm_set_range(memory_map, "CL4-rx", layout->cl4_rx.pa, layout->cl4_rx.size);
    apple_sptm_set_range(memory_map, "CL4-entry", 0, 0);
    apple_sptm_set_range(memory_map, "CL4-virt", 0, 0);

    // Keep the older per-segment entries (allocated for every kernel)
    // accurate for the permuted placement.
    apple_sptm_seg_iter_init(&it, kc->macho);
    while ((seg = apple_sptm_seg_next(&it)) != NULL) {
        const AppleSptmRegion* r = &kc->region[apple_sptm_classify(seg)];
        snprintf(name, sizeof(name), "Kernel-%s", seg->segname);
        if (apple_dt_get_prop(memory_map, name) != NULL) {
            apple_sptm_set_range(memory_map, name, r->pa + (seg->vmaddr - r->va), seg->vmsize);
        }
    }
}

static bool apple_sptm_get_range(AppleDTNode* memory_map, const char* name, uint64_t* paddr, uint64_t* length,
                                 Error** errp)
{
    AppleDTProp* prop = apple_dt_get_prop(memory_map, name);

    if (prop == NULL) { return false; }
    if (prop->len != 16) {
        error_setg(errp, "memory-map %s has size %u, SPTM requires 16", name, prop->len);
        return false;
    }
    *paddr  = ldq_le_p(prop->data);
    *length = ldq_le_p((uint8_t*)prop->data + 8);
    return true;
}

bool apple_sptm_check_memory_map(AppleDTNode* memory_map, Error** errp)
{
    // The table SPTM's validate_region_order() walks, in order.
    static const struct
    {
        const char* name;
        bool        optional;
    } order[] = {
        {"TXM-ro", false},    {"TXM-rx", false},    {"TXM-bx", false},     {"TrustCache", false},
        {"AuxKC-ro", true},   {"AuxKC-rx", true},   {"BootKC-rx", false},  {"BootKC-bx", false},
        {"BootKC-ro", false}, {"BootKC-rs", false}, {"CL4-rx", true},      {"CL4-ro", true},
        {"DeviceTree", false}, {"SPTM-ro", false},  {"SPTM-rx", false},    {"SPTM-rw", false},
    };
    // Looked up with init_get_image_region(..., required) elsewhere in sptm_init().
    static const char* const required[] = {
        "SPTM-rm",  "SPTM-le",      "TXM-rw",      "TXM-le",       "BootKC-rw", "BootKC-le", "BootArgs",
        "slide",    "TXM-entry",    "TXM-virt",    "BootKC-entry", "BootKC-virt", "CL4-entry", "CL4-rx",
        "CL4-virt",
    };
    const char* prev     = NULL;
    uint64_t    prev_end = 0;
    uint64_t    pa;
    uint64_t    len;
    Error*      local    = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(order); ++i) {
        if (!apple_sptm_get_range(memory_map, order[i].name, &pa, &len, &local)) {
            if (local != NULL) {
                error_propagate(errp, local);
                return false;
            }
            if (order[i].optional) { continue; }
            error_setg(errp, "memory-map has no %s, which SPTM requires", order[i].name);
            return false;
        }
        if (order[i].optional && pa == UINT64_MAX) { continue; }
        if (pa == 0 || len == 0 || pa + len < pa || ((pa | len) & (SPTM_PAGE - 1)) != 0) {
            error_setg(errp, "memory-map %s [0x%" PRIx64 "+0x%" PRIx64 "] is not a valid page range", order[i].name,
                       pa, len);
            return false;
        }
        if (prev != NULL && pa != prev_end) {
            error_setg(errp, "memory-map %s at 0x%" PRIx64 " does not start where %s ends (0x%" PRIx64 ")",
                       order[i].name, pa, prev, prev_end);
            return false;
        }
        prev     = order[i].name;
        prev_end = pa + len;
    }

    for (size_t i = 0; i < ARRAY_SIZE(required); ++i) {
        if (!apple_sptm_get_range(memory_map, required[i], &pa, &len, &local)) {
            if (local == NULL) { error_setg(&local, "memory-map has no %s, which SPTM requires", required[i]); }
            error_propagate(errp, local);
            return false;
        }
    }
    return true;
}
