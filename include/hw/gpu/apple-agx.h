/*
 * Apple AGX GPU (A13 "G12P"): sgx register stubs + gfx-asc firmware HLE.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#include "qemu/osdep.h"
#include "hw/arm/dt.h"
#include "hw/sysbus.h"

#define TYPE_APPLE_AGX "apple-agx"

/*
 * Create the GPU from the device tree's `sgx` and `gfx-asc` nodes.
 *
 * MMIO regions, in order: gfx-asc reg[0] (ASC + mailbox), gfx-asc reg[1]
 * (ASC wrapper), sgx reg[0], sgx reg[1]. IRQs: the gfx-asc mailbox lines.
 */
SysBusDevice* apple_agx_create(AppleDTNode* sgx, AppleDTNode* gfx_asc);

/* pmap-io-ranges signature the kernel expects on the UAT regions ('GUAT'). */
#define AGX_PMAP_SIG_UAT (0x47554154)

/* Each UAT region the boot loader carves out is one 16 KiB page. */
#define AGX_UAT_REGION_SIZE (16 * KiB)

/*
 * Tell the GPU where the boot loader put the UAT regions: the kernel-half
 * L1 table, the TTBAT (context TTBR table) and the handoff area.
 */
void apple_agx_set_uat_regions(SysBusDevice* agx, hwaddr shared, hwaddr ttbat, hwaddr handoff);
