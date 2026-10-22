/*
 * Guest floating point on the host FPU (same-ISA fast path)
 *
 * On an AArch64 host the host FPU implements exactly the Arm FP semantics
 * that softfloat emulates, so when the guest's FPCR selects the same mode as
 * the host's (round to nearest even, no FZ/FIZ/DN, FPCR.AH = 0, no trapped
 * exceptions) a covered guest FP instruction can be executed with the host
 * instruction of the same name and gives the bit-identical result and the
 * same cumulative FPSR flags. See docs/NATIVE-FP.md for the exact conditions.
 *
 * Cumulative flags: while a vCPU is "live" (inside cpu_exec with a native
 * FPCR), native operations leave their exception flags in the host FPSR and
 * nothing is written per operation. The guest's cumulative flags are then
 * the union of the softfloat state and the host FPSR; the host FPSR is folded
 * into the guest state when the guest reads FPSR, when FPCR changes and when
 * the vCPU leaves cpu_exec. The host FPSR is only cleared when it holds a
 * flag the guest does not have (guest wrote FPSR, or C code raised a flag
 * outside the native regime), so the hot path never writes FPSR.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_ARM_NATIVE_FP_H
#define TARGET_ARM_NATIVE_FP_H

#include "fpu/softfloat-types.h"

#if defined(__aarch64__) && defined(CONFIG_TCG)
#define ARM_NATIVE_FP_HOST 1
#else
#define ARM_NATIVE_FP_HOST 0
#endif

/* FPCR controls that must all be clear for the host FPU (FPCR == 0) to match. */
#define ARM_NATIVE_FP_FPCR_MASK (FPCR_FIZ | FPCR_AH | FPCR_EEXC_MASK | FPCR_RMODE_MASK | FPCR_FZ | FPCR_DN)

/* Set once at the first CPU realize: requested, host is AArch64, host self-test passed. */
extern bool arm_native_fp_on;
/* Test-only fault injection (accel property x-native-fp-fault): native FADD (.2d/d) swaps its operands. */
extern bool arm_native_fp_fault;

#ifdef CONFIG_TCG
void        arm_native_fp_init(void);
const char* arm_native_fp_selftest(unsigned* ncases);
void        arm_native_fp_exec_enter(CPUARMState* env);
void arm_native_fp_exec_exit(CPUARMState* env);
void arm_native_fp_fold(CPUARMState* env);
void arm_native_fp_fpsr_written(CPUARMState* env);
void arm_native_fp_fpcr_written(CPUARMState* env);
#endif

/*
 * True if a helper may execute an operation for status S on the host FPU.
 * Only the A64 status of a live vCPU on its own thread ever has this set, and
 * only while its FPCR is native-compatible; see arm_native_fp_exec_enter().
 */
static inline bool arm_native_fp_ok(const float_status* s) { return ARM_NATIVE_FP_HOST && s->host_fpu_native; }

#if ARM_NATIVE_FP_HOST
/*
 * Vector kernels. Each applies the host instruction of the same name to
 * OPRSZ bytes (a multiple of 8): 16-byte chunks with the .4s/.2d form, an
 * 8-byte remainder with .2s (single) or the scalar form (double). Inline asm
 * and not C operators: the compiler may commute a C "a + b", which changes
 * which NaN payload propagates, and may contract or reassociate.
 */
#define NFP_V3(NAME, Q4, D8)                                                                      \
    static inline void nfp_##NAME(void* vd, const void* vn, const void* vm, intptr_t oprsz)        \
    {                                                                                              \
        char*       d = vd;                                                                        \
        const char *n = vn, *m = vm;                                                               \
        intptr_t    i = 0;                                                                         \
        for (; i + 16 <= oprsz; i += 16) {                                                         \
            asm volatile("ldr q0, [%1]\n\t"                                                        \
                         "ldr q1, [%2]\n\t" Q4 "\n\t"                                              \
                         "str q0, [%0]" ::"r"(d + i),                                              \
                         "r"(n + i), "r"(m + i)                                                    \
                         : "v0", "v1", "memory");                                                  \
        }                                                                                          \
        if (i < oprsz) {                                                                           \
            asm volatile("ldr d0, [%1]\n\t"                                                        \
                         "ldr d1, [%2]\n\t" D8 "\n\t"                                              \
                         "str d0, [%0]" ::"r"(d + i),                                              \
                         "r"(n + i), "r"(m + i)                                                    \
                         : "v0", "v1", "memory");                                                  \
        }                                                                                          \
    }

/* Fused multiply-add forms: Vd = Vd +/- Vn * Vm, the addend is the destination. */
#define NFP_MLA(NAME, Q4, D8)                                                                     \
    static inline void nfp_##NAME(void* vd, const void* vn, const void* vm, intptr_t oprsz)        \
    {                                                                                              \
        char*       d = vd;                                                                        \
        const char *n = vn, *m = vm;                                                               \
        intptr_t    i = 0;                                                                         \
        for (; i + 16 <= oprsz; i += 16) {                                                         \
            asm volatile("ldr q0, [%0]\n\t"                                                        \
                         "ldr q1, [%1]\n\t"                                                        \
                         "ldr q2, [%2]\n\t" Q4 "\n\t"                                              \
                         "str q0, [%0]" ::"r"(d + i),                                              \
                         "r"(n + i), "r"(m + i)                                                    \
                         : "v0", "v1", "v2", "memory");                                            \
        }                                                                                          \
        if (i < oprsz) {                                                                           \
            asm volatile("ldr d0, [%0]\n\t"                                                        \
                         "ldr d1, [%1]\n\t"                                                        \
                         "ldr d2, [%2]\n\t" D8 "\n\t"                                              \
                         "str d0, [%0]" ::"r"(d + i),                                              \
                         "r"(n + i), "r"(m + i)                                                    \
                         : "v0", "v1", "v2", "memory");                                            \
        }                                                                                          \
    }

NFP_V3(fadd_s, "fadd v0.4s, v0.4s, v1.4s", "fadd v0.2s, v0.2s, v1.2s")
NFP_V3(fsub_s, "fsub v0.4s, v0.4s, v1.4s", "fsub v0.2s, v0.2s, v1.2s")
NFP_V3(fmul_s, "fmul v0.4s, v0.4s, v1.4s", "fmul v0.2s, v0.2s, v1.2s")
NFP_V3(fadd_d_ok, "fadd v0.2d, v0.2d, v1.2d", "fadd d0, d0, d1")
NFP_V3(fadd_d_swapped, "fadd v0.2d, v1.2d, v0.2d", "fadd d0, d1, d0")
NFP_V3(fsub_d, "fsub v0.2d, v0.2d, v1.2d", "fsub d0, d0, d1")
NFP_V3(fmul_d, "fmul v0.2d, v0.2d, v1.2d", "fmul d0, d0, d1")
NFP_MLA(fmla_s, "fmla v0.4s, v1.4s, v2.4s", "fmla v0.2s, v1.2s, v2.2s")
NFP_MLA(fmls_s, "fmls v0.4s, v1.4s, v2.4s", "fmls v0.2s, v1.2s, v2.2s")
NFP_MLA(fmla_d, "fmla v0.2d, v1.2d, v2.2d", "fmadd d0, d1, d2, d0")
NFP_MLA(fmls_d, "fmls v0.2d, v1.2d, v2.2d", "fmsub d0, d1, d2, d0")

static inline void nfp_fadd_d(void* vd, const void* vn, const void* vm, intptr_t oprsz)
{
    if (unlikely(arm_native_fp_fault)) {
        nfp_fadd_d_swapped(vd, vn, vm, oprsz);
    }
    else {
        nfp_fadd_d_ok(vd, vn, vm, oprsz);
    }
}
#endif /* ARM_NATIVE_FP_HOST */

#endif /* TARGET_ARM_NATIVE_FP_H */
