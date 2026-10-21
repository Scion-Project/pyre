/*
 * iOS 27+ SPTM boot layout: physical placement of the SPTM, TXM and boot
 * kernelcache images, and the /chosen/memory-map image regions SPTM looks up.
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

#pragma once

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "exec/vaddr.h"
#include "hw/arm/boot.h"
#include "hw/arm/dt.h"
#include "system/memory.h"

/// Size SPTM requires of /chosen/cl4-entropy (copied, then cleared).
#define SPTM_CL4_ENTROPY_SIZE (0xc0)

/*
 * Segment classes, named by the suffix iBoot gives the matching
 * /chosen/memory-map property ("<image>-<suffix>", e.g. "BootKC-rx").
 */
typedef enum
{
    APPLE_SPTM_RO, /* read-only: __TEXT, __PRELINK_TEXT, __DATA_CONST, __LATE_CONST */
    APPLE_SPTM_RX, /* executable: __TEXT_EXEC */
    APPLE_SPTM_BX, /* boot-time executable: __TEXT_BOOT_EXEC */
    APPLE_SPTM_RS, /* SPTM-owned kernel data: __DATA_SPTM */
    APPLE_SPTM_RW, /* writable: __DATA, __LAST, __PRELINK_INFO */
    APPLE_SPTM_RM, /* boot-only data SPTM reclaims: __BOOTDATA */
    APPLE_SPTM_LE, /* __LINKEDIT */
    APPLE_SPTM_CLASS_COUNT,
} AppleSptmClass;

typedef struct
{
    hwaddr   pa;   /* 0 if the image has no segment of this class */
    uint64_t size; /* 16 KiB multiple */
    vaddr    va;   /* lowest link address of the class's segments */
} AppleSptmRegion;

typedef struct
{
    const char*     prefix; /* "SPTM", "TXM" or "BootKC" */
    MachoHeader64*  macho;
    AppleSptmRegion region[APPLE_SPTM_CLASS_COUNT];
    vaddr           entry_va; /* LC_UNIXTHREAD pc (link address) */
} AppleSptmImage;

typedef struct
{
    AppleSptmImage  sptm;
    AppleSptmImage  txm;
    AppleSptmImage  kc;
    AppleSptmRegion trustcache;
    AppleSptmRegion cl4_rx; /* placeholder page, see sptm-boot.c */
    AppleSptmRegion device_tree;
    hwaddr          chain_start; /* TXM-ro ... SPTM-rw, contiguous */
    hwaddr          chain_end;
    hwaddr          end; /* first free byte after everything placed here */
} AppleSptmLayout;

/// Classify the segments of the three images. Fails (error_fatal) if a
/// class is not contiguous in virtual memory or a required class is missing.
void apple_sptm_layout_init(AppleSptmLayout* layout, MachoHeader64* sptm, MachoHeader64* txm, MachoHeader64* kc);

/// Create the memory-map properties this layout publishes (16-byte zeroed
/// entries). Must run before the device tree is finalised.
void apple_sptm_allocate_memory_map(AppleDTNode* memory_map, const AppleSptmLayout* layout);

/// Choose physical addresses: the region-order chain starts at or above
/// `min_pa`, positioned so SPTM's physical address is congruent to its link
/// address modulo `sptm_align`; the remaining regions follow the chain.
void apple_sptm_layout_place(AppleSptmLayout* layout, hwaddr min_pa, hwaddr sptm_align, uint64_t trustcache_size,
                             uint64_t device_tree_size);

/// Copy every segment (zero-filling vmsize beyond filesize), the trust cache
/// and the CL4 placeholder page into guest memory.
void apple_sptm_layout_load(const AppleSptmLayout* layout, AddressSpace* as, const uint8_t* trustcache,
                            uint64_t trustcache_size);

/// Fill the memory-map properties allocated by apple_sptm_allocate_memory_map().
/// `kernel_slide` is the kernel's virtual slide (published as "slide").
void apple_sptm_publish_memory_map(AppleDTNode* memory_map, const AppleSptmLayout* layout, vaddr kernel_slide);

/// Re-apply the checks SPTM makes on the image regions (presence, 16-byte
/// size, page alignment, and the contiguous region order it validates in
/// validate_region_order()). Returns false and sets `errp` on the first
/// violation.
bool apple_sptm_check_memory_map(AppleDTNode* memory_map, Error** errp);

/// Physical address of an image's entry point, from its region layout.
hwaddr apple_sptm_entry_pa(const AppleSptmImage* image);
