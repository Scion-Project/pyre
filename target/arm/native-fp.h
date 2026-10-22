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
static inline bool arm_native_fp_live(const float_status* s) { return ARM_NATIVE_FP_HOST && s->host_fpu_native; }

/*
 * For operations that round per the status (arithmetic, int->fp): also
 * require round-to-nearest-even, so a helper that ever ran inside a
 * temporarily changed rounding mode (gen_set_rmode) stays on softfloat.
 * Conversions to integer encode their rounding in the host instruction and
 * use arm_native_fp_live() plus the status rounding mode instead.
 */
static inline bool arm_native_fp_ok(const float_status* s)
{ return arm_native_fp_live(s) && s->float_rounding_mode == float_round_nearest_even; }

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

NFP_V3(fdiv_s, "fdiv v0.4s, v0.4s, v1.4s", "fdiv v0.2s, v0.2s, v1.2s")
NFP_V3(fdiv_d, "fdiv v0.2d, v0.2d, v1.2d", "fdiv d0, d0, d1")
NFP_V3(fmin_s, "fmin v0.4s, v0.4s, v1.4s", "fmin v0.2s, v0.2s, v1.2s")
NFP_V3(fmin_d, "fmin v0.2d, v0.2d, v1.2d", "fmin d0, d0, d1")
NFP_V3(fmax_s, "fmax v0.4s, v0.4s, v1.4s", "fmax v0.2s, v0.2s, v1.2s")
NFP_V3(fmax_d, "fmax v0.2d, v0.2d, v1.2d", "fmax d0, d0, d1")
NFP_V3(fminnum_s, "fminnm v0.4s, v0.4s, v1.4s", "fminnm v0.2s, v0.2s, v1.2s")
NFP_V3(fminnum_d, "fminnm v0.2d, v0.2d, v1.2d", "fminnm d0, d0, d1")
NFP_V3(fmaxnum_s, "fmaxnm v0.4s, v0.4s, v1.4s", "fmaxnm v0.2s, v0.2s, v1.2s")
NFP_V3(fmaxnum_d, "fmaxnm v0.2d, v0.2d, v1.2d", "fmaxnm d0, d0, d1")
NFP_V3(fabd_s, "fabd v0.4s, v0.4s, v1.4s", "fabd v0.2s, v0.2s, v1.2s")
NFP_V3(fabd_d, "fabd v0.2d, v0.2d, v1.2d", "fabd d0, d0, d1")
NFP_V3(fceq_s, "fcmeq v0.4s, v0.4s, v1.4s", "fcmeq v0.2s, v0.2s, v1.2s")
NFP_V3(fceq_d, "fcmeq v0.2d, v0.2d, v1.2d", "fcmeq d0, d0, d1")
NFP_V3(fcge_s, "fcmge v0.4s, v0.4s, v1.4s", "fcmge v0.2s, v0.2s, v1.2s")
NFP_V3(fcge_d, "fcmge v0.2d, v0.2d, v1.2d", "fcmge d0, d0, d1")
NFP_V3(fcgt_s, "fcmgt v0.4s, v0.4s, v1.4s", "fcmgt v0.2s, v0.2s, v1.2s")
NFP_V3(fcgt_d, "fcmgt v0.2d, v0.2d, v1.2d", "fcmgt d0, d0, d1")

/*
 * By-element forms. The index selects an element of Vm within each 128-bit
 * segment (as in QEMU's DO_FMUL_IDX/DO_FMLA_IDX and the architecture), and
 * it has to be an immediate, hence one asm per index. FMLA/FMLS by element
 * add into a separate addend vector A (which may alias D).
 */
#define NFP_IDX_LOOP(LD_A, INSN_Q, INSN_D, CLOB)                                                  \
    for (; i + 16 <= oprsz; i += 16) {                                                             \
        asm volatile(LD_A("q") "ldr q1, [%1]\n\tldr q2, [%2]\n\t" INSN_Q "\n\tstr q0, [%0]" ::"r"(d + i), \
                     "r"(n + i), "r"(m + i), "r"(a + i)                                            \
                     : "v0", "v1", "v2", "memory");                                                \
    }                                                                                              \
    if (i < oprsz) {                                                                               \
        asm volatile(LD_A("d") "ldr d1, [%1]\n\tldr q2, [%2]\n\t" INSN_D "\n\tstr d0, [%0]" ::"r"(d + i), \
                     "r"(n + i), "r"(m + i), "r"(a + i)                                            \
                     : "v0", "v1", "v2", "memory");                                                \
    }
#define NFP_NO_A(W)   ""
#define NFP_LD_A(W)   "ldr " W "0, [%3]\n\t"

#define NFP_IDX_FN(NAME, LD_A, OP, QREGS, DREGS, EL, NIDX)                                         \
    static inline void nfp_##NAME(void* vd, const void* vn, const void* vm, const void* va, intptr_t oprsz, \
                                  intptr_t idx)                                                    \
    {                                                                                              \
        char*       d = vd;                                                                        \
        const char *n = vn, *m = vm, *a = va;                                                      \
        intptr_t    i = 0;                                                                         \
        switch (idx & (NIDX - 1)) {                                                                \
            NFP_IDX_CASE(0, LD_A, OP, QREGS, DREGS, EL)                                            \
            NFP_IDX_CASE(1, LD_A, OP, QREGS, DREGS, EL)                                            \
            NFP_IDX_CASE_HI_##NIDX(2, LD_A, OP, QREGS, DREGS, EL)                                  \
            NFP_IDX_CASE_HI_##NIDX(3, LD_A, OP, QREGS, DREGS, EL)                                  \
        }                                                                                          \
    }
#define NFP_IDX_CASE(K, LD_A, OP, QREGS, DREGS, EL)                                               \
    case K:                                                                                        \
        NFP_IDX_LOOP(LD_A, OP " " QREGS ", v2." EL "[" #K "]", OP " " DREGS ", v2." EL "[" #K "]", ) \
        break;
#define NFP_IDX_CASE_HI_4(K, LD_A, OP, QREGS, DREGS, EL) NFP_IDX_CASE(K, LD_A, OP, QREGS, DREGS, EL)
#define NFP_IDX_CASE_HI_2(K, LD_A, OP, QREGS, DREGS, EL)

NFP_IDX_FN(fmul_idx_s, NFP_NO_A, "fmul", "v0.4s, v1.4s", "v0.2s, v1.2s", "s", 4)
NFP_IDX_FN(fmla_idx_s, NFP_LD_A, "fmla", "v0.4s, v1.4s", "v0.2s, v1.2s", "s", 4)
NFP_IDX_FN(fmls_idx_s, NFP_LD_A, "fmls", "v0.4s, v1.4s", "v0.2s, v1.2s", "s", 4)
NFP_IDX_FN(fmul_idx_d, NFP_NO_A, "fmul", "v0.2d, v1.2d", "d0, d1", "d", 2)
NFP_IDX_FN(fmla_idx_d, NFP_LD_A, "fmla", "v0.2d, v1.2d", "d0, d1", "d", 2)
NFP_IDX_FN(fmls_idx_d, NFP_LD_A, "fmls", "v0.2d, v1.2d", "d0, d1", "d", 2)

/*
 * Scalar kernels on the raw bit patterns QEMU passes around (float32 and
 * float64 are uint32_t/uint64_t).
 */
static inline float    nfp_f(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static inline double   nfp_d(uint64_t x) { double f; memcpy(&f, &x, 8); return f; }
static inline uint32_t nfp_fbits(float f) { uint32_t x; memcpy(&x, &f, 4); return x; }
static inline uint64_t nfp_dbits(double f) { uint64_t x; memcpy(&x, &f, 8); return x; }

#define NFP_S2(NAME, INSN)                                                                        \
    static inline uint32_t nfp_##NAME##_s(uint32_t a, uint32_t b)                                  \
    {                                                                                              \
        float r;                                                                                   \
        asm volatile(INSN " %s0, %s1, %s2" : "=w"(r) : "w"(nfp_f(a)), "w"(nfp_f(b)));              \
        return nfp_fbits(r);                                                                       \
    }                                                                                              \
    static inline uint64_t nfp_##NAME##_d(uint64_t a, uint64_t b)                                  \
    {                                                                                              \
        double r;                                                                                  \
        asm volatile(INSN " %d0, %d1, %d2" : "=w"(r) : "w"(nfp_d(a)), "w"(nfp_d(b)));              \
        return nfp_dbits(r);                                                                       \
    }
NFP_S2(add, "fadd")
NFP_S2(sub, "fsub")
NFP_S2(mul, "fmul")
NFP_S2(div, "fdiv")
NFP_S2(min, "fmin")
NFP_S2(max, "fmax")
NFP_S2(minnum, "fminnm")
NFP_S2(maxnum, "fmaxnm")

static inline uint32_t nfp_sqrt_s(uint32_t a)
{
    float r;
    asm volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(nfp_f(a)));
    return nfp_fbits(r);
}
static inline uint64_t nfp_sqrt_d(uint64_t a)
{
    double r;
    asm volatile("fsqrt %d0, %d1" : "=w"(r) : "w"(nfp_d(a)));
    return nfp_dbits(r);
}

/* QEMU muladd(a, b, c) = a * b + c; FMADD Rd, Rn, Rm, Ra = Ra + Rn * Rm (FPMulAdd(addend, op1, op2)). */
static inline uint32_t nfp_muladd_s(uint32_t a, uint32_t b, uint32_t c)
{
    float r;
    asm volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(nfp_f(a)), "w"(nfp_f(b)), "w"(nfp_f(c)));
    return nfp_fbits(r);
}
static inline uint64_t nfp_muladd_d(uint64_t a, uint64_t b, uint64_t c)
{
    double r;
    asm volatile("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(nfp_d(a)), "w"(nfp_d(b)), "w"(nfp_d(c)));
    return nfp_dbits(r);
}

/* FCMP/FCMPE: the NZCV result in PSTATE layout (bits 31..28), as the *_a64 compare helpers return. */
#define NFP_CMP(NAME, INSN, R, CONV)                                                              \
    static inline uint64_t nfp_##NAME(uint64_t a, uint64_t b)                                      \
    {                                                                                              \
        uint64_t f;                                                                                \
        asm volatile(INSN " %" R "1, %" R "2\n\tmrs %0, nzcv" : "=r"(f) : "w"(CONV(a)), "w"(CONV(b)) : "cc"); \
        return f;                                                                                  \
    }
NFP_CMP(cmp_s, "fcmp", "s", nfp_f)
NFP_CMP(cmpe_s, "fcmpe", "s", nfp_f)
NFP_CMP(cmp_d, "fcmp", "d", nfp_d)
NFP_CMP(cmpe_d, "fcmpe", "d", nfp_d)

static inline uint64_t nfp_fcvt_ds(uint32_t a)
{
    double r;
    asm volatile("fcvt %d0, %s1" : "=w"(r) : "w"(nfp_f(a)));
    return nfp_dbits(r);
}
static inline uint32_t nfp_fcvt_sd(uint64_t a)
{
    float r;
    asm volatile("fcvt %s0, %d1" : "=w"(r) : "w"(nfp_d(a)));
    return nfp_fbits(r);
}

/* SCVTF/UCVTF from a 64-bit integer (A64 widens W sources before the helper). */
#define NFP_ITOF(NAME, INSN, FR, FT, BITS)                                                        \
    static inline uint64_t nfp_##NAME(uint64_t x)                                                  \
    {                                                                                              \
        FT r;                                                                                      \
        asm volatile(INSN " %" FR "0, %x1" : "=w"(r) : "r"(x));                                    \
        return BITS(r);                                                                            \
    }
NFP_ITOF(sqtod, "scvtf", "d", double, nfp_dbits)
NFP_ITOF(uqtod, "ucvtf", "d", double, nfp_dbits)
NFP_ITOF(sqtos, "scvtf", "s", float, nfp_fbits)
NFP_ITOF(uqtos, "ucvtf", "s", float, nfp_fbits)

/*
 * FCVT{Z,N,A,P,M}{S,U}: the rounding is part of the instruction; pick it
 * from the status rounding mode (set by the translator around the helper).
 * Returns false for a mode with no instruction (round to odd). OT is the C
 * type of the helper's result, so widening matches the helper exactly.
 */
#define NFP_TOI_1(INSN, OR, FR, CONV) asm volatile(INSN " %" OR "0, %" FR "1" : "=r"(r) : "w"(CONV(x)))
#define NFP_TOI(NAME, SU, OR, RT, OT, FR, IT, CONV)                                               \
    static inline bool nfp_##NAME(IT x, FloatRoundMode rm, OT* out)                                \
    {                                                                                              \
        RT r;                                                                                      \
        switch (rm) {                                                                              \
            case float_round_to_zero    : NFP_TOI_1("fcvtz" SU, OR, FR, CONV); break;              \
            case float_round_nearest_even: NFP_TOI_1("fcvtn" SU, OR, FR, CONV); break;             \
            case float_round_ties_away  : NFP_TOI_1("fcvta" SU, OR, FR, CONV); break;              \
            case float_round_up         : NFP_TOI_1("fcvtp" SU, OR, FR, CONV); break;              \
            case float_round_down       : NFP_TOI_1("fcvtm" SU, OR, FR, CONV); break;              \
            default                     : return false;                                            \
        }                                                                                          \
        *out = r;                                                                                  \
        return true;                                                                               \
    }
NFP_TOI(tosqd, "s", "x", int64_t, uint64_t, "d", uint64_t, nfp_d)
NFP_TOI(touqd, "u", "x", uint64_t, uint64_t, "d", uint64_t, nfp_d)
NFP_TOI(tosld, "s", "w", int32_t, uint64_t, "d", uint64_t, nfp_d)
NFP_TOI(tould, "u", "w", uint32_t, uint64_t, "d", uint64_t, nfp_d)
NFP_TOI(tosqs, "s", "x", int64_t, uint64_t, "s", uint32_t, nfp_f)
NFP_TOI(touqs, "u", "x", uint64_t, uint64_t, "s", uint32_t, nfp_f)
NFP_TOI(tosls, "s", "w", int32_t, uint32_t, "s", uint32_t, nfp_f)
NFP_TOI(touls, "u", "w", uint32_t, uint32_t, "s", uint32_t, nfp_f)

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

/*
 * Helper-side glue: try the host FPU first, else fall through to softfloat.
 * Expand to nothing on hosts without native FP.
 */
#if ARM_NATIVE_FP_HOST
#define NFP_RETURN_IF(OK, EXPR)                                                                   \
    do {                                                                                           \
        if (OK) { return EXPR; }                                                                   \
    } while (0)
/* fp -> integer (shift 0 only; the rounding comes from the status) */
#define NFP_TOI_TRY(K, X, SHIFT, FPST, T)                                                         \
    do {                                                                                           \
        T nfp_r_;                                                                                  \
        if ((SHIFT) == 0 && arm_native_fp_live(FPST) && nfp_to##K(X, (FPST)->float_rounding_mode, &nfp_r_)) { \
            return nfp_r_;                                                                         \
        }                                                                                          \
    } while (0)
/* integer -> fp (shift 0 only) */
#define NFP_ITOF_TRY(K, X, SHIFT, FPST)                                                           \
    do {                                                                                           \
        if ((SHIFT) == 0 && arm_native_fp_ok(FPST)) { return nfp_##K(X); }                        \
    } while (0)
#else
#define NFP_RETURN_IF(OK, EXPR)            do { } while (0)
#define NFP_TOI_TRY(K, X, SHIFT, FPST, T)  do { } while (0)
#define NFP_ITOF_TRY(K, X, SHIFT, FPST)    do { } while (0)
#endif
#define NFP_TOI_SKIP(K, X, SHIFT, FPST, T) do { } while (0)
#define NFP_ITOF_SKIP(K, X, SHIFT, FPST)   do { } while (0)

#endif /* TARGET_ARM_NATIVE_FP_H */
