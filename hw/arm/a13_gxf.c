/*
 * Apple A13 GXF.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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
#include "qemu/log.h"
#include "exec/cputlb.h"
#include "hw/arm/a13_gxf.h"
#include "target/arm/cpregs.h"
#include "target/arm/cpu.h"
#include "target/arm/internals.h"

static CPAccessResult access_gxf(CPUARMState* env, const ARMCPRegInfo* ri, bool isread)
{
    if (arm_is_guarded(env)) { return CP_ACCESS_OK; }
    return CP_ACCESS_TRAP_EL1;
}

static uint64_t vbar_el1_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    if (arm_is_guarded(env)) { return env->gxf.vbar_gl[1]; }
    return raw_read(env, ri);
}

static void vbar_el1_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    if (arm_is_guarded(env)) { env->gxf.vbar_gl[1] = value & ~0x1FULL; }
    else if (!(env->cp15.vmsa_lock_el1 & VMSA_LOCK_VBAR_EL1)) {
        raw_write(env, ri, value & ~0x1FULL);
    }
}

static void sprr_perm_el0_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    // maybe use uint64_t for mask/umask?
    uint64_t perm = raw_read(env, ri);
    uint32_t mask = env->sprr.mprr_el_br_el1[0][0];
    if (arm_current_el(env)) {
        raw_write(env, ri, value);
        return;
    }

    for (int i = 0; i < 16; i++) {
        uint32_t umask          = SPRR_MASK_EXTRACT_IDX_ATTR(mask, i);
        uint64_t requested_perm = APRR_EXTRACT_IDX_ATTR(value, i);
        uint64_t orig_perm      = APRR_EXTRACT_IDX_ATTR(perm, i);
        uint64_t changed_perm   = ((requested_perm ^ orig_perm) & umask);
        uint64_t result_perm    = orig_perm;
        /* Only change bits that are set in mask */

        result_perm &= ~changed_perm;
        result_perm |= requested_perm & changed_perm;

        perm &= ~(APRR_ATTR_MASK << APRR_SHIFT_FOR_IDX(i));
        perm |= result_perm << APRR_SHIFT_FOR_IDX(i);
    }
    // what's about bit36?
    // Visual also accidentally found it on his own "TPRO regions should not be writable on entry to dyld"

    raw_write(env, ri, perm);

    tlb_flush_by_mmuidx(env_cpu(env), ARMMMUIdxBit_E10_0);
}

static uint64_t gxf_cpreg_raw_read(CPUARMState* env, const ARMCPRegInfo* ri)
{ return *(uint64_t*)((char*)(env) + (ri)->bank_fieldoffsets[0]); }

static void gxf_cpreg_raw_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{ *(uint64_t*)((char*)(env) + (ri)->bank_fieldoffsets[0]) = value; }

static const ARMCPRegInfo apple_a13_gxf_cp_override_reginfo[] = {
    {
        .name            = "TPIDR_EL1",
        .state           = ARM_CP_STATE_AA64,
        .opc0            = 3,
        .opc1            = 0,
        .opc2            = 4,
        .crn             = 13,
        .crm             = 0,
        .access          = PL1_RW,
        .type            = ARM_CP_OVERRIDE | ARM_CP_SUPPRESS_TB_END,
        .gxf_fieldoffset = offsetof(CPUARMState, gxf.tpidr_gl[1]),
        .raw_readfn      = raw_read,
        .raw_writefn     = raw_write,
        .fieldoffset     = offsetof(CPUARMState, cp15.tpidr_el[1]),
        .resetvalue      = 0,
    },
    {
        .name              = "VBAR",
        .state             = ARM_CP_STATE_BOTH,
        .opc0              = 3,
        .crn               = 12,
        .crm               = 0,
        .opc1              = 0,
        .opc2              = 0,
        .access            = PL1_RW,
        .type              = ARM_CP_OVERRIDE,
        .readfn            = vbar_el1_read,
        .writefn           = vbar_el1_write,
        .raw_readfn        = raw_read,
        .raw_writefn       = raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, cp15.vbar_s), offsetof(CPUARMState, cp15.vbar_ns)},
        .resetvalue        = 0,
    },
    {
        .name            = "SPSR_EL1",
        .state           = ARM_CP_STATE_AA64,
        .type            = ARM_CP_ALIAS | ARM_CP_OVERRIDE | ARM_CP_SUPPRESS_TB_END,
        .opc0            = 3,
        .opc1            = 0,
        .crn             = 4,
        .crm             = 0,
        .opc2            = 0,
        .access          = PL1_RW,
        .raw_readfn      = raw_read,
        .raw_writefn     = raw_write,
        .fieldoffset     = offsetof(CPUARMState, banked_spsr[BANK_SVC]),
        .gxf_fieldoffset = offsetof(CPUARMState, gxf.spsr_gl[1]),
        .resetvalue      = 0,
    },
    {
        .name            = "ELR_EL1",
        .state           = ARM_CP_STATE_AA64,
        .type            = ARM_CP_ALIAS | ARM_CP_OVERRIDE | ARM_CP_SUPPRESS_TB_END,
        .opc0            = 3,
        .opc1            = 0,
        .crn             = 4,
        .crm             = 0,
        .opc2            = 1,
        .access          = PL1_RW,
        .raw_readfn      = raw_read,
        .raw_writefn     = raw_write,
        .fieldoffset     = offsetof(CPUARMState, elr_el[1]),
        .gxf_fieldoffset = offsetof(CPUARMState, gxf.elr_gl[1]),
        .resetvalue      = 0,
    },
    {
        .name            = "ESR_EL1",
        .state           = ARM_CP_STATE_AA64,
        .opc0            = 3,
        .crn             = 5,
        .crm             = 2,
        .opc1            = 0,
        .opc2            = 0,
        .access          = PL1_RW,
        .type            = ARM_CP_OVERRIDE | ARM_CP_SUPPRESS_TB_END,
        .accessfn        = access_tvm_trvm,
        .raw_readfn      = raw_read,
        .raw_writefn     = raw_write,
        .fieldoffset     = offsetof(CPUARMState, cp15.esr_el[1]),
        .gxf_fieldoffset = offsetof(CPUARMState, gxf.esr_gl[1]),
        .resetvalue      = 0,
    },
    {
        .name            = "FAR_EL1",
        .state           = ARM_CP_STATE_AA64,
        .opc0            = 3,
        .crn             = 6,
        .crm             = 0,
        .opc1            = 0,
        .opc2            = 0,
        .type            = ARM_CP_OVERRIDE | ARM_CP_SUPPRESS_TB_END,
        .access          = PL1_RW,
        .accessfn        = access_tvm_trvm,
        .raw_readfn      = raw_read,
        .raw_writefn     = raw_write,
        .fieldoffset     = offsetof(CPUARMState, cp15.far_el[1]),
        .gxf_fieldoffset = offsetof(CPUARMState, gxf.far_gl[1]),
        .resetvalue      = 0,
    },
};

static const ARMCPRegInfo apple_a13_gxf_cp_reginfo[] = {
    {
        .name        = "GXF_CONFIG_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 2,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_config_el[1]),
    },
    {
        .name        = "GXF_STATUS_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 0,
        .access      = PL1_R,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_status_el[1]),
    },
    {
        .name        = "GXF_ENTER_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 1,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_enter_el[1]),
    },
    {
        .name        = "GXF_ABORT_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 2,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.gxf_abort_el[1]),
    },
    {
        .name        = "ASPSR_GL11",
        .type        = ARM_CP_SUPPRESS_TB_END,
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 8,
        .opc2        = 3,
        .access      = PL1_RW,
        .accessfn    = access_gxf,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, gxf.aspsr_gl[1]),
    },
    {
        .name              = "SP_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 0,
        .access            = PL2_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.sp_gl[1]), offsetof(CPUARMState, sp_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "TPIDR_GL11",
        .type              = ARM_CP_SUPPRESS_TB_END,
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 1,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.tpidr_gl[1]), offsetof(CPUARMState, cp15.tpidr_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "VBAR_GL11",
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 2,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.vbar_gl[1]), offsetof(CPUARMState, cp15.vbar_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "SPSR_GL11",
        .type              = ARM_CP_SUPPRESS_TB_END,
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 3,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.spsr_gl[1]), offsetof(CPUARMState, banked_spsr[BANK_SVC])},
        .resetvalue        = 0,
    },
    {
        .name              = "ESR_GL11",
        .type              = ARM_CP_SUPPRESS_TB_END,
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 5,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.esr_gl[1]), offsetof(CPUARMState, cp15.esr_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "ELR_GL11",
        .type              = ARM_CP_SUPPRESS_TB_END,
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 6,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.elr_gl[1]), offsetof(CPUARMState, elr_el[1])},
        .resetvalue        = 0,
    },
    {
        .name              = "FAR_GL11",
        .type              = ARM_CP_SUPPRESS_TB_END,
        .cp                = CP_REG_ARM64_SYSREG_CP,
        .state             = ARM_CP_STATE_AA64,
        .opc0              = 3,
        .opc1              = 6,
        .crn               = 15,
        .crm               = 9,
        .opc2              = 7,
        .access            = PL1_RW,
        .accessfn          = access_gxf,
        .raw_readfn        = gxf_cpreg_raw_read,
        .raw_writefn       = gxf_cpreg_raw_write,
        .bank_fieldoffsets = {offsetof(CPUARMState, gxf.far_gl[1]), offsetof(CPUARMState, cp15.far_el[1])},
        .resetvalue        = 0,
    },
    // TODO: Implement lockdown for these registers to prevent unexpected
    // changes
    {
        .name        = "SPRR_CONFIG_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 0,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .readfn      = raw_read,
        .writefn     = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_config_el[1]),
    },
    {
        .name        = "SPRR_CONFIG_EL0",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 1,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .readfn      = raw_read,
        .writefn     = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_config_el[0]),
    },
    {
        .name       = "SPRR_EL0BR0_EL1",
        .cp         = CP_REG_ARM64_SYSREG_CP,
        .state      = ARM_CP_STATE_AA64,
        .opc0       = 3,
        .opc1       = 6,
        .crn        = 15,
        .crm        = 1,
        .opc2       = 5,
        .access     = PL0_RW,
        .resetvalue = 0,
        .readfn     = raw_read,
        .writefn    = sprr_perm_el0_write,
        // is that actually not needed?
        // .raw_readfn = raw_read,
        .raw_writefn = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_el_br_el1[0][0]),
    },
    {
        .name        = "SPRR_EL0BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 6,
        .access      = PL1_RW | PL0_R,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_el_br_el1[0][1]),
    },
    {
        .name        = "SPRR_EL1BR0_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 1,
        .opc2        = 7,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_el_br_el1[1][0]),
    },
    {
        .name        = "SPRR_EL1BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 0,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .readfn      = raw_read,
        .writefn     = raw_write,
        .fieldoffset = offsetof(CPUARMState, sprr.sprr_el_br_el1[1][1]),
    },
    {
        .name        = "MPRR_EL0BR0_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 1,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[0][0]),
    },
    {
        .name        = "MPRR_EL0BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 2,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[0][1]),
    },
    {
        .name        = "MPRR_EL1BR0_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 3,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[1][0]),
    },
    {
        .name        = "MPRR_EL1BR1_EL1",
        .cp          = CP_REG_ARM64_SYSREG_CP,
        .state       = ARM_CP_STATE_AA64,
        .opc0        = 3,
        .opc1        = 6,
        .crn         = 15,
        .crm         = 3,
        .opc2        = 4,
        .access      = PL1_RW,
        .resetvalue  = 0,
        .fieldoffset = offsetof(CPUARMState, sprr.mprr_el_br_el1[1][1]),
    },
};

/*
 * iOS 27 SPTM / TXM / kernel touch a handful of Apple system registers that
 * the GL1 GXF model above does not define. An EL1 access to an undefined
 * Apple sysreg raises UNDEF, which crashes the monitors on first touch, so
 * these are defined as plain RW storage (reset 0, read returns last written
 * value). Their exact meaning is not modelled yet (see docs/ios27/GXF-GAP.md),
 * so every access is logged with LOG_UNIMP to reveal, on the first real boot,
 * which ones the firmware depends on and in what order. No poll-and-wait
 * dependency on any of these was observed in the 27.0.1 images; if one turns
 * out to be polled it should graduate to a register with real behaviour.
 */
static uint64_t sptm_impdef_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    uint64_t value = raw_read(env, ri);
    qemu_log_mask(LOG_UNIMP, "GXF/SPTM: read unmodelled Apple reg %s => 0x%" PRIx64 "\n", ri->name, value);
    return value;
}

static void sptm_impdef_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP, "GXF/SPTM: write unmodelled Apple reg %s <= 0x%" PRIx64 "\n", ri->name, value);
    raw_write(env, ri, value);
}

#define SPTM_IMPDEF_REG(nm, op1_, crm_, op2_, field)                                                 \
    {                                                                                                \
        .name        = nm,                                                                           \
        .cp          = CP_REG_ARM64_SYSREG_CP,                                                        \
        .state       = ARM_CP_STATE_AA64,                                                            \
        .opc0        = 3,                                                                            \
        .opc1        = (op1_),                                                                       \
        .crn         = 15,                                                                           \
        .crm         = (crm_),                                                                       \
        .opc2        = (op2_),                                                                       \
        .access      = PL1_RW,                                                                       \
        .type        = ARM_CP_OVERRIDE,                                                              \
        .resetvalue  = 0,                                                                            \
        .readfn      = sptm_impdef_read,                                                             \
        .writefn     = sptm_impdef_write,                                                            \
        .raw_readfn  = raw_read,                                                                     \
        .raw_writefn = raw_write,                                                                    \
        .fieldoffset = offsetof(CPUARMState, field),                                                 \
    }

static const ARMCPRegInfo apple_a13_sptm_cp_reginfo[] = {
    /* GXF-space (op0=3, op1=6, CRn=15) regs the kernel/SPTM use beyond the
     * GL1 set above. Named by encoding; semantics OPEN. */
    SPTM_IMPDEF_REG("S3_6_C15_C0_0", 6, 0, 0, gxf.gxf_ctl_misc[0]),
    SPTM_IMPDEF_REG("S3_6_C15_C2_0", 6, 2, 0, gxf.gxf_ctl_misc[1]),
    SPTM_IMPDEF_REG("S3_6_C15_C8_7", 6, 8, 7, gxf.gxf_ctl_misc[2]),
    /* Apple EL2 impl-defined block (op0=3, op1=4, CRn=15). Reached from EL1
     * on Apple cores (seen in both the non-guarded kernel and guarded SPTM),
     * so PL1_RW rather than PL2_RW. Named by encoding; semantics OPEN. */
    SPTM_IMPDEF_REG("S3_4_C15_C0_0", 4, 0, 0, gxf.apple_el2_impdef[0]),
    SPTM_IMPDEF_REG("S3_4_C15_C0_1", 4, 0, 1, gxf.apple_el2_impdef[1]),
    SPTM_IMPDEF_REG("S3_4_C15_C0_2", 4, 0, 2, gxf.apple_el2_impdef[2]),
    SPTM_IMPDEF_REG("S3_4_C15_C0_3", 4, 0, 3, gxf.apple_el2_impdef[3]),
    SPTM_IMPDEF_REG("S3_4_C15_C0_4", 4, 0, 4, gxf.apple_el2_impdef[4]),
    SPTM_IMPDEF_REG("S3_4_C15_C0_5", 4, 0, 5, gxf.apple_el2_impdef[5]),
    SPTM_IMPDEF_REG("S3_4_C15_C1_0", 4, 1, 0, gxf.apple_el2_impdef[6]),
    SPTM_IMPDEF_REG("S3_4_C15_C1_1", 4, 1, 1, gxf.apple_el2_impdef[7]),
    SPTM_IMPDEF_REG("S3_4_C15_C1_2", 4, 1, 2, gxf.apple_el2_impdef[8]),
    SPTM_IMPDEF_REG("S3_4_C15_C1_3", 4, 1, 3, gxf.apple_el2_impdef[9]),
    SPTM_IMPDEF_REG("S3_4_C15_C1_4", 4, 1, 4, gxf.apple_el2_impdef[10]),
    SPTM_IMPDEF_REG("S3_4_C15_C2_2", 4, 2, 2, gxf.apple_el2_impdef[11]),
    SPTM_IMPDEF_REG("S3_4_C15_C2_3", 4, 2, 3, gxf.apple_el2_impdef[12]),
    SPTM_IMPDEF_REG("S3_4_C15_C2_4", 4, 2, 4, gxf.apple_el2_impdef[13]),
    SPTM_IMPDEF_REG("S3_4_C15_C2_5", 4, 2, 5, gxf.apple_el2_impdef[14]),
};

#undef SPTM_IMPDEF_REG

void apple_a13_init_gxf_override(AppleA13State* cpu)
{
    if (arm_feature(&cpu->parent_obj.env, ARM_FEATURE_GXF)) {
        define_arm_cp_regs(&cpu->parent_obj, apple_a13_gxf_cp_override_reginfo);
    }
}

/*
 * Several encodings in apple_a13_sptm_cp_reginfo already have a real model:
 * - in the A13 core table, defined just before this:
 *   S3_6_C15_C2_0 ARM64_REG_MMU_ERR_STS, S3_4_C15_C0_2 ARM64_REG_FED_ERR_STS,
 *   S3_4_C15_C0_5, S3_4_C15_C1_3 AMX_STATUS_EL1 (read-only),
 *   S3_4_C15_C1_4 AMX_CTL_EL1, and the per-cluster CTRR registers
 *   S3_4_C15_C2_2..C2_5 (CTRR_LOCK, CTRR_A_LWR, CTRR_A_UPR, CTRR_CTL);
 * - in target/arm/helper.c, defined later during the ARM CPU realize:
 *   S3_4_C15_C0_4 APCTL_EL1, S3_4_C15_C1_0/C1_1 KERNELKEYLO/HI_EL1,
 *   S3_4_C15_C1_2 VMSA_LOCK_EL1.
 * The placeholders are marked ARM_CP_OVERRIDE so the later real definitions
 * replace them (redefining a non-override register asserts in
 * add_cpreg_to_hashtable() on assert-enabled builds, which aborted every
 * AppleA13 realize there), and they are skipped where an earlier definition
 * exists, so the A13 registers keep exactly their previous semantics.
 */
static void apple_a13_define_sptm_regs(ARMCPU* cpu)
{
    for (size_t i = 0; i < ARRAY_SIZE(apple_a13_sptm_cp_reginfo); ++i) {
        const ARMCPRegInfo* r   = &apple_a13_sptm_cp_reginfo[i];
        uint32_t            key = ENCODE_AA64_CP_REG(r->cp, r->crn, r->crm, r->opc0, r->opc1, r->opc2);

        if (ARMCPRegTable_cget(cpu->cp_regs, key) == NULL) { define_one_arm_cp_reg(cpu, r); }
    }
}

void apple_a13_init_gxf(AppleA13State* cpu)
{
    if (arm_feature(&cpu->parent_obj.env, ARM_FEATURE_GXF)) {
        define_arm_cp_regs(&cpu->parent_obj, apple_a13_gxf_cp_reginfo);
        apple_a13_define_sptm_regs(&cpu->parent_obj);
    }
}
