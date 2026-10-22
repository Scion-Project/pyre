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
