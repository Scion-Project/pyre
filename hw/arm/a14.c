/*
 * Apple A14 (Firestorm/Icestorm) system registers.
 *
 * The A14 core is the A13 model (a13.c, a13_gxf.c) with its own MIDR part
 * numbers (set in a13.c), a few re-encoded GXF/SPRR registers
 * (apple_a14_init_gxf()), the non-speculative counter aliases, and a set of
 * implementation-defined registers that the iOS 14.8 iPhone13,x kernel
 * touches and the A13 model does not define. Those are plain storage until a
 * boot shows one needs behaviour; see docs/T8101-SCOPE.md section 9.
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
#include "hw/arm/a13.h"
#include "hw/arm/a13_gxf.h"
#include "target/arm/cpregs.h"
#include "target/arm/cpu.h"

/*
 * Implementation-defined encodings (op0 = 3, CRn = 15) found in the
 * iPhone13,x kernel and not in the A13 model, as {op1, CRm, op2}. Names
 * known from m1n1's cpu_regs.h for the M1: S3_0_C15_C1_2 EHID20,
 * S3_0_C15_C1_3 HID21, S3_0_C15_C2_0 HID2, S3_0_C15_C6_0 HID6,
 * S3_0_C15_C9_1 EHID9, S3_0_C15_C11_2 HID18, S3_0_C15_C15_5 HID17.
 * Most of the others appear once per kernel, in register save/dump paths.
 */
static const uint8_t apple_a14_impdef_regs[][3] = {
    {0, 1, 2},  {0, 1, 3},  {0, 2, 0},  {0, 2, 1},  {0, 6, 0},  {0, 9, 1},  {0, 11, 2}, {0, 12, 1},
    {0, 15, 5}, {1, 0, 3},  {1, 1, 3},  {1, 8, 2},  {1, 8, 3},  {1, 9, 3},  {1, 10, 3}, {1, 11, 3},
    {4, 2, 6},  {4, 4, 6},  {4, 4, 7},  {4, 5, 0},  {4, 5, 2},  {4, 8, 0},  {4, 8, 1},  {4, 8, 2},
    {4, 8, 3},  {4, 9, 0},  {4, 9, 1},  {4, 9, 2},  {4, 9, 3},  {4, 9, 4},  {4, 9, 5},  {4, 11, 0},
    {4, 11, 1}, {4, 11, 4}, {4, 11, 5}, {4, 12, 5}, {5, 1, 3},  {6, 0, 2},  {6, 0, 3},  {6, 2, 1},
    {6, 2, 2},  {6, 2, 3},  {6, 2, 4},  {6, 2, 5},  {6, 6, 0},  {6, 6, 1},  {6, 6, 2},  {6, 6, 3},
    {6, 7, 0},  {6, 7, 1},  {6, 7, 2},  {6, 7, 3},  {6, 7, 4},  {6, 7, 5},  {6, 7, 6},  {6, 7, 7},
    {6, 9, 4},  {6, 11, 1}, {6, 14, 4}, {6, 14, 5}, {6, 14, 6}, {6, 14, 7}, {6, 15, 0},
    {6, 15, 1}, {6, 15, 2}, {6, 15, 3}, {6, 15, 4}, {6, 15, 5}, {6, 15, 7},
};

QEMU_BUILD_BUG_ON(ARRAY_SIZE(apple_a14_impdef_regs) > APPLE_A14_IMPDEF_MAX);

static uint64_t a14_impdef_read(CPUARMState* env, const ARMCPRegInfo* ri)
{
    uint64_t value = raw_read(env, ri);
    qemu_log_mask(LOG_UNIMP, "A14: read unmodelled Apple reg %s => 0x%" PRIx64 "\n", ri->name, value);
    return value;
}

static void a14_impdef_write(CPUARMState* env, const ARMCPRegInfo* ri, uint64_t value)
{
    qemu_log_mask(LOG_UNIMP, "A14: write unmodelled Apple reg %s <= 0x%" PRIx64 "\n", ri->name, value);
    raw_write(env, ri, value);
}

void apple_a14_init_cpregs(AppleA13State* acpu) { apple_a14_init_gxf(acpu); }

/*
 * S3_4_C15_C10_5/6 are the A14's non-speculative aliases of CNTPCT_EL0 and
 * CNTVCT_EL0 (m1n1 CNTPCT_ALIAS_EL0/CNTVCT_ALIAS_EL0; XNU HAS_ACNTVCT): the
 * iPhone13,x kernel's mach_absolute_time() reads S3_4_C15_C10_6. They take
 * the generic timer's own read and access functions, so EL0 access follows
 * CNTKCTL_EL1 exactly as for the architectural registers.
 */
static void apple_a14_define_counter_alias(ARMCPU* cpu, uint8_t src_op2, uint8_t alias_op2, const char* name)
{
    const ARMCPRegInfo* src =
        ARMCPRegTable_cget(cpu->cp_regs, ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, 14, 0, 3, 3, src_op2));
    ARMCPRegInfo r;

    assert_nonnull(src);
    r       = *src;
    r.name  = name;
    r.cp    = CP_REG_ARM64_SYSREG_CP;
    r.state = ARM_CP_STATE_AA64;
    r.opc0  = 3;
    r.opc1  = 4;
    r.crn   = 15;
    r.crm   = 10;
    r.opc2  = alias_op2;
    r.type |= ARM_CP_NO_RAW;
    define_one_arm_cp_reg(cpu, &r);
}

/*
 * APSTS_EL1 (m1n1 SYS_IMP_APL_APSTS_EL1, XNU ARM64_REG_APSTS_EL1): after
 * loading the kernel PAC keys XNU's start.s spins until MKEYVld (bit 0) is
 * set. Keys are always valid here.
 */
static const ARMCPRegInfo apple_a14_apsts_reginfo = {
    .name       = "APSTS_EL1",
    .cp         = CP_REG_ARM64_SYSREG_CP,
    .state      = ARM_CP_STATE_AA64,
    .opc0       = 3,
    .opc1       = 6,
    .crn        = 15,
    .crm        = 12,
    .opc2       = 4,
    .access     = PL1_R,
    .type       = ARM_CP_CONST,
    .resetvalue = 1,
};

/* Called after the ARM CPU realize, so every architectural register exists. */
void apple_a14_init_late_cpregs(AppleA13State* acpu)
{
    ARMCPU* cpu = &acpu->parent_obj;

    apple_a14_define_counter_alias(cpu, 1, 5, "ACNTPCT_EL0");
    apple_a14_define_counter_alias(cpu, 2, 6, "ACNTVCT_EL0");
    define_one_arm_cp_reg(cpu, &apple_a14_apsts_reginfo);

    for (size_t i = 0; i < ARRAY_SIZE(apple_a14_impdef_regs); ++i) {
        const uint8_t* e   = apple_a14_impdef_regs[i];
        uint32_t       key = ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, 15, e[1], 3, e[0], e[2]);
        ARMCPRegInfo   r   = {
                .name        = g_strdup_printf("S3_%u_C15_C%u_%u", e[0], e[1], e[2]),
                .cp          = CP_REG_ARM64_SYSREG_CP,
                .state       = ARM_CP_STATE_AA64,
                .opc0        = 3,
                .opc1        = e[0],
                .crn         = 15,
                .crm         = e[1],
                .opc2        = e[2],
                .access      = PL1_RW,
                .type        = ARM_CP_NO_RAW,
                .resetvalue  = 0,
                .readfn      = a14_impdef_read,
                .writefn     = a14_impdef_write,
                .fieldoffset = offsetof(AppleA13State, a14_impdef[i]) - offsetof(ARMCPU, env),
        };

        /* Never shadow a register something else already models. */
        if (ARMCPRegTable_cget(cpu->cp_regs, key) == NULL) { define_one_arm_cp_reg(cpu, &r); }
    }
}
