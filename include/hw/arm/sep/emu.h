/*
 * Apple SEP Emulation.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2025 Christian Inci (chris-pcguy).
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
#include "hw/arm/dt.h"
#include "hw/arm/patcher.h"
#include "cpu-qom.h"
#include "qom/object.h"

#define TYPE_APPLE_SEP "apple-sep"
OBJECT_DECLARE_TYPE(AppleSEP, AppleSEPClass, APPLE_SEP)

#define SEPFW_MAPPING_SIZE   (16 * MiB)
#define SEP_DMA_MAPPING_SIZE (SEPFW_MAPPING_SIZE * 2)

#define SEP_OPCODE17_INTEGRITY_TREE_SIZE (0x8000)
#define SEP_APERTURE_REGION3             (0x300000000ULL)
#define SEP_APERTURE_REGION2             (0x340000000ULL)
#define SEP_REGION2_SIZE                 ((((uint64_t)SEP_OPCODE17_INTEGRITY_TREE_SIZE) << 10) & 0xFFFFC000ULL)
#define SEP_DMA_IOVA_SIZE                (0x100000000ULL)
#define SEP_DART_PAGE_SIZE               (16 * KiB)

typedef enum AppleSEPMMIOIndex
{
    SEP_MMIO_INDEX_PMGR,
    SEP_MMIO_INDEX_TRNG_REGS,
    SEP_MMIO_INDEX_KEY,
    SEP_MMIO_INDEX_KEY_FKEY,
    SEP_MMIO_INDEX_KEY_FCFG,
    SEP_MMIO_INDEX_MONI,
    SEP_MMIO_INDEX_MONI_THRM,
    SEP_MMIO_INDEX_EISP,
    SEP_MMIO_INDEX_EISP_HMAC,
    SEP_MMIO_INDEX_AESS,
    SEP_MMIO_INDEX_AESH,
    SEP_MMIO_INDEX_AESC,
    SEP_MMIO_INDEX_PKA,
    SEP_MMIO_INDEX_PKA_TMM,
    SEP_MMIO_INDEX_MISC0,
    SEP_MMIO_INDEX_MISC1,
    SEP_MMIO_INDEX_MISC2,
    SEP_MMIO_INDEX_PROGRESS,
    SEP_MMIO_INDEX_BOOT_MONITOR,
} AppleSEPMMIOIndex;

void      ck_sep_seprom_patches(CKPatcherRange* range);
AppleSEP* apple_sep_from_node(AppleDTNode* node, MemoryRegion* ool_mr, vaddr base, uint32_t cpu_id, bool modern,
                              uint32_t chip_id);
bool      apple_sep_get_fuse_changer_bit(AppleSEP* s, uint8_t bit);
void      apple_sep_set_fw(AppleSEP* s, hwaddr sep_fw_addr, gchar* fw_data, gsize sep_fw_size);
void      apple_sep_map_mmio(AppleSEP* s, AppleSEPMMIOIndex mmio_index, hwaddr addr);
void      apple_sep_setup_tz0(AppleSEP* s, MemoryRegion* dram, hwaddr tz0_off, hwaddr tz0_size);
ARMCPU*   apple_sep_get_cpu(AppleSEP* s);    // FIXME: remove

/*
 * Keep the SEP core powered off whatever the AP writes to SEP_PS. Used when the
 * SEP ROM can only idle (apple_sep_rom_is_idle_loop()): running it would burn a
 * host thread without the guest being able to tell the difference.
 */
void apple_sep_set_cpu_parked(AppleSEP* s, bool parked);
bool apple_sep_is_cpu_parked(AppleSEP* s);

/*
 * Check that a SEP ROM or SEP firmware image could be real before loading it:
 * not empty, no larger than the region it is loaded into, and not one byte
 * repeated (a zero-filled placeholder, for example). The format is not checked,
 * so every real image passes. On failure, sets @errp and returns false.
 */
bool apple_sep_check_image(const char* what, const char* path, const void* data, size_t size, size_t max_size,
                           Error** errp);
