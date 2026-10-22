/*
 * Host self-test for native-fp (docs/NATIVE-FP.md), run once before it is
 * enabled: every native kernel against the softfloat path of the helper it
 * replaces, over special operands (signed zeros, denormals, extremes,
 * infinities, quiet and signalling NaNs with payloads, rounding ties,
 * integer-conversion boundaries, ordinary values), comparing result bits and
 * the IOC..IXC flags. Any difference keeps the guest on softfloat.
 *
 * Vector cases broadcast the operands so the flags belong to one operand
 * tuple (the by-element cases vary the other Vm lanes to check the index);
 * hw/arm/tests/fp_native_check.S covers mixed lanes in the guest.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/helper-proto.h"
#include "tcg/tcg-gvec-desc.h"
#include "fpu/softfloat.h"
#include "native-fp.h"

#if ARM_NATIVE_FP_HOST

static const uint64_t tab_d[] = {
    0x0000000000000000, 0x8000000000000000, 0x0000000000000001, 0x800fffffffffffff, 0x0010000000000000,
    0x7fefffffffffffff, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000123,
    0x7ff0000000000456, 0x7ff4000000000789, 0x3ff0000000000000, 0xbff8000000000000, 0x3fb999999999999a,
    0x4340000000000001, 0x3cb0000000000000, 0x7fe0000000000000, 0x0008000000000000, 0xc00921fb54442d18,
    0x3ff0000000000001, 0x3ca0000000000000, 0x8010000000000001, 0xffefffffffffffff,
};
static const uint32_t tab_s[] = {
    0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x00800000, 0x7f7fffff, 0x7f800000, 0xff800000,
    0x7fc00000, 0xffc00123, 0x7f800456, 0x7fa00789, 0x3f800000, 0xbfc00000, 0x3dcccccd, 0x4b000001,
    0x33800000, 0x7f000000, 0x00400000, 0xc0490fdb, 0x3f800001, 0x33000000, 0x80800001, 0xff7fffff,
};
/* extra operands for float -> integer: ties, integer-range boundaries */
static const uint64_t cvt_d[] = {
    0x4004000000000000, 0xc004000000000000, 0x3fe0000000000000, 0xbfe0000000000000, 0x3ff8000000000000,
    0x41e0000000000000, 0x41dfffffffe00000, 0xc1e0000000200000, 0x41f0000000000000, 0x43e0000000000000,
    0xc3e0000000000000, 0x43f0000000000000, 0x41effffffff00000, 0xbff8000000000000,
};
static const uint32_t cvt_s[] = {
    0x40200000, 0xc0200000, 0x3f000000, 0xbf000000, 0x3fc00000, 0x4f000000, 0xcf000000,
    0x4f800000, 0x5f000000, 0xdf000000, 0x5f800000, 0x4b7fffff, 0xbfc00000, 0x4effffff,
};
static const uint64_t tab_i[] = {
    0, 1, (uint64_t)-1, 2, 0x7fffffff, 0xffffffff80000000ULL, 0xffffffff, 0x7fffffffffffffffULL,
    0x8000000000000000ULL, 0x0000000001000001ULL, 0x0020000000000001ULL, 0x8000000000000401ULL,
    0x7fffffff80000000ULL, 0x1234567890abcdefULL, 0xffdfffffffffffffULL, 0x00000000ffffff7fULL,
};

static char     st_msg[200];
static unsigned st_count;

#define FAIL_IF(COND, ...)                                                                         \
    do {                                                                                           \
        if (COND) {                                                                                \
            snprintf(st_msg, sizeof(st_msg), __VA_ARGS__);                                         \
            return st_msg;                                                                         \
        }                                                                                          \
    } while (0)

static inline void     host_clear_fpsr(void) { asm volatile("msr fpsr, xzr" ::: "memory"); }
static inline uint32_t host_flags(void)
{
    uint64_t v;
    asm volatile("mrs %0, fpsr" : "=r"(v)::"memory");
    return v & 0x1f;
}

static uint32_t ref_flags(float_status* s)
{
    int f = get_float_exception_flags(s);
    return ((f & float_flag_invalid) ? FPSR_IOC : 0) | ((f & float_flag_divbyzero) ? FPSR_DZC : 0) |
           ((f & float_flag_overflow) ? FPSR_OFC : 0) | ((f & float_flag_underflow) ? FPSR_UFC : 0) |
           ((f & float_flag_inexact) ? FPSR_IXC : 0);
}

static void ref_reset(float_status* s, FloatRoundMode rm)
{
    memset(s, 0, sizeof(*s));
    arm_set_default_fp_behaviours(s);
    set_float_rounding_mode(rm, s);
}

static void fill(uint64_t* v, int esize, uint64_t x)
{
    if (esize == 8) { v[0] = v[1] = x; }
    else { v[0] = v[1] = (x & 0xffffffff) | (x << 32); }
}

static uint64_t val(int esize, unsigned i) { return esize == 8 ? tab_d[i] : tab_s[i]; }

/* ---- vector: binary and fused multiply-add ---- */

typedef void VecKernel(void*, const void*, const void*, intptr_t);
typedef void VecHelper(void*, void*, void*, float_status*, uint32_t);

static const struct
{
    const char* name;
    VecKernel*  kernel;
    VecHelper*  helper;
    int         esize;
    bool        addend; /* reads Vd */
} vec_tests[] = {
#define V(N, K, H, E, A) {N, nfp_##K, helper_gvec_##H, E, A}
    V("fadd.s", fadd_s, fadd_s, 4, false),      V("fsub.s", fsub_s, fsub_s, 4, false),
    V("fmul.s", fmul_s, fmul_s, 4, false),      V("fdiv.s", fdiv_s, fdiv_s, 4, false),
    V("fmin.s", fmin_s, fmin_s, 4, false),      V("fmax.s", fmax_s, fmax_s, 4, false),
    V("fminnm.s", fminnum_s, fminnum_s, 4, false), V("fmaxnm.s", fmaxnum_s, fmaxnum_s, 4, false),
    V("fabd.s", fabd_s, fabd_s, 4, false),      V("fcmeq.s", fceq_s, fceq_s, 4, false),
    V("fcmge.s", fcge_s, fcge_s, 4, false),     V("fcmgt.s", fcgt_s, fcgt_s, 4, false),
    V("fmla.s", fmla_s, vfma_s, 4, true),       V("fmls.s", fmls_s, vfms_s, 4, true),
    V("fadd.d", fadd_d, fadd_d, 8, false),      V("fsub.d", fsub_d, fsub_d, 8, false),
    V("fmul.d", fmul_d, fmul_d, 8, false),      V("fdiv.d", fdiv_d, fdiv_d, 8, false),
    V("fmin.d", fmin_d, fmin_d, 8, false),      V("fmax.d", fmax_d, fmax_d, 8, false),
    V("fminnm.d", fminnum_d, fminnum_d, 8, false), V("fmaxnm.d", fmaxnum_d, fmaxnum_d, 8, false),
    V("fabd.d", fabd_d, fabd_d, 8, false),      V("fcmeq.d", fceq_d, fceq_d, 8, false),
    V("fcmge.d", fcge_d, fcge_d, 8, false),     V("fcmgt.d", fcgt_d, fcgt_d, 8, false),
    V("fmla.d", fmla_d, vfma_d, 8, true),       V("fmls.d", fmls_d, vfms_d, 8, true),
#undef V
};

static const char* test_vec(void)
{
    float_status ref;

    for (unsigned t = 0; t < ARRAY_SIZE(vec_tests); t++) {
        int      es   = vec_tests[t].esize;
        unsigned nval = es == 8 ? ARRAY_SIZE(tab_d) : ARRAY_SIZE(tab_s);
        unsigned nk   = vec_tests[t].addend ? nval : 1;

        for (int sz = 16; sz >= (es == 4 ? 8 : 16); sz -= 8) {
            for (unsigned i = 0; i < nval; i++) {
                for (unsigned j = 0; j < nval; j++) {
                    for (unsigned k = 0; k < nk; k++) {
                        uint64_t n[2], m[2], dref[2], dnat[2];
                        uint32_t want, got;

                        fill(n, es, val(es, i));
                        fill(m, es, val(es, j));
                        fill(dref, es, val(es, k));
                        fill(dnat, es, val(es, k));
                        ref_reset(&ref, float_round_nearest_even);
                        vec_tests[t].helper(dref, n, m, &ref, simd_desc(sz, sz, 0));
                        want = ref_flags(&ref);
                        host_clear_fpsr();
                        vec_tests[t].kernel(dnat, n, m, sz);
                        got = host_flags();
                        st_count++;
                        FAIL_IF(memcmp(dref, dnat, sz) || want != got,
                                "%s/%d(n=0x%" PRIx64 ", m=0x%" PRIx64 ", d=0x%" PRIx64 "): host 0x%" PRIx64
                                " fpsr 0x%x, softfloat 0x%" PRIx64 " fpsr 0x%x",
                                vec_tests[t].name, sz, val(es, i), val(es, j), val(es, k), dnat[0], got, dref[0],
                                want);
                    }
                }
            }
        }
    }
    return NULL;
}

/* ---- vector by element ---- */

typedef void IdxKernel(void*, const void*, const void*, const void*, intptr_t, intptr_t);
typedef void IdxHelper3(void*, void*, void*, float_status*, uint32_t);
typedef void IdxHelper4(void*, void*, void*, void*, float_status*, uint32_t);

static const struct
{
    const char* name;
    IdxKernel*  kernel;
    IdxHelper3* h3; /* FMUL: d = n * m[idx] */
    IdxHelper4* h4; /* FMLA/FMLS: d = a +/- n * m[idx] */
    int         esize;
} idx_tests[] = {
    {"fmul.s[]", nfp_fmul_idx_s, helper_gvec_fmul_idx_s, NULL, 4},
    {"fmla.s[]", nfp_fmla_idx_s, NULL, helper_gvec_fmla_idx_s, 4},
    {"fmls.s[]", nfp_fmls_idx_s, NULL, helper_gvec_fmls_idx_s, 4},
    {"fmul.d[]", nfp_fmul_idx_d, helper_gvec_fmul_idx_d, NULL, 8},
    {"fmla.d[]", nfp_fmla_idx_d, NULL, helper_gvec_fmla_idx_d, 8},
    {"fmls.d[]", nfp_fmls_idx_d, NULL, helper_gvec_fmls_idx_d, 8},
};

static const char* test_idx(void)
{
    float_status ref;

    for (unsigned t = 0; t < ARRAY_SIZE(idx_tests); t++) {
        int      es   = idx_tests[t].esize;
        unsigned nval = es == 8 ? ARRAY_SIZE(tab_d) : ARRAY_SIZE(tab_s);
        unsigned nk   = idx_tests[t].h4 ? nval : 1;
        int      nidx = 16 / es;

        for (int sz = 16; sz >= (es == 4 ? 8 : 16); sz -= 8) {
            for (int idx = 0; idx < nidx; idx++) {
                for (unsigned i = 0; i < nval; i++) {
                    for (unsigned j = 0; j < nval; j++) {
                        for (unsigned k = 0; k < nk; k++) {
                            uint64_t n[2], m[2], a[2], dref[2] = {0}, dnat[2] = {0};
                            uint32_t want, got;

                            fill(n, es, val(es, i));
                            fill(a, es, val(es, k));
                            /* Vm: the operand in lane idx, other lanes something else */
                            if (es == 8) {
                                m[0] = m[1] = val(es, (j + 7) % nval);
                                m[idx]      = val(es, j);
                            }
                            else {
                                uint32_t* m32 = (uint32_t*)m;
                                for (int l = 0; l < 4; l++) { m32[l] = val(es, (j + 5 + l) % nval); }
                                m32[idx] = val(es, j);
                            }
                            ref_reset(&ref, float_round_nearest_even);
                            if (idx_tests[t].h3) { idx_tests[t].h3(dref, n, m, &ref, simd_desc(sz, sz, idx)); }
                            else { idx_tests[t].h4(dref, n, m, a, &ref, simd_desc(sz, sz, idx)); }
                            want = ref_flags(&ref);
                            host_clear_fpsr();
                            idx_tests[t].kernel(dnat, n, m, a, sz, idx);
                            got = host_flags();
                            st_count++;
                            FAIL_IF(memcmp(dref, dnat, sz) || want != got,
                                    "%s%d/%d(n=0x%" PRIx64 ", m=0x%" PRIx64 ", a=0x%" PRIx64 "): host 0x%" PRIx64
                                    " fpsr 0x%x, softfloat 0x%" PRIx64 " fpsr 0x%x",
                                    idx_tests[t].name, idx, sz, val(es, i), val(es, j), val(es, k), dnat[0], got,
                                    dref[0], want);
                        }
                    }
                }
            }
        }
    }
    return NULL;
}

/* ---- scalar ---- */

/* Adapters so every scalar case has the same shape: r = f(a, b, c). */
#define S2(OP, P, T)                                                                               \
    static uint64_t ref_##OP##P(uint64_t a, uint64_t b, uint64_t c, float_status* s)               \
    { return helper_vfp_##OP##P((T)a, (T)b, s); }                                                  \
    static uint64_t nat_##OP##P(uint64_t a, uint64_t b, uint64_t c) { return nfp_##OP##_##P((T)a, (T)b); }
#define S2SD(OP) S2(OP, s, uint32_t) S2(OP, d, uint64_t)
S2SD(add)
S2SD(sub)
S2SD(mul)
S2SD(div)
S2SD(min)
S2SD(max)
S2SD(minnum)
S2SD(maxnum)

static uint64_t ref_sqrts(uint64_t a, uint64_t b, uint64_t c, float_status* s) { return helper_vfp_sqrts(a, s); }
static uint64_t nat_sqrts(uint64_t a, uint64_t b, uint64_t c) { return nfp_sqrt_s(a); }
static uint64_t ref_sqrtd(uint64_t a, uint64_t b, uint64_t c, float_status* s) { return helper_vfp_sqrtd(a, s); }
static uint64_t nat_sqrtd(uint64_t a, uint64_t b, uint64_t c) { return nfp_sqrt_d(a); }
static uint64_t ref_fcvtds(uint64_t a, uint64_t b, uint64_t c, float_status* s) { return helper_vfp_fcvtds(a, s); }
static uint64_t nat_fcvtds(uint64_t a, uint64_t b, uint64_t c) { return nfp_fcvt_ds(a); }
static uint64_t ref_fcvtsd(uint64_t a, uint64_t b, uint64_t c, float_status* s) { return helper_vfp_fcvtsd(a, s); }
static uint64_t nat_fcvtsd(uint64_t a, uint64_t b, uint64_t c) { return nfp_fcvt_sd(a); }
static uint64_t ref_muladds(uint64_t a, uint64_t b, uint64_t c, float_status* s)
{ return helper_vfp_muladds(a, b, c, s); }
static uint64_t nat_muladds(uint64_t a, uint64_t b, uint64_t c) { return nfp_muladd_s(a, b, c); }
static uint64_t ref_muladdd(uint64_t a, uint64_t b, uint64_t c, float_status* s)
{ return helper_vfp_muladdd(a, b, c, s); }
static uint64_t nat_muladdd(uint64_t a, uint64_t b, uint64_t c) { return nfp_muladd_d(a, b, c); }
#define CMP(E, P, T)                                                                               \
    static uint64_t ref_cmp##E##P(uint64_t a, uint64_t b, uint64_t c, float_status* s)             \
    { return helper_vfp_cmp##E##P##_a64((T)a, (T)b, s); }                                          \
    static uint64_t nat_cmp##E##P(uint64_t a, uint64_t b, uint64_t c) { return nfp_cmp##E##_##P((T)a, (T)b); }
CMP(, s, uint32_t)
CMP(e, s, uint32_t)
CMP(, d, uint64_t)
CMP(e, d, uint64_t)
#define ITOF(K)                                                                                    \
    static uint64_t ref_##K(uint64_t a, uint64_t b, uint64_t c, float_status* s) { return helper_vfp_##K(a, 0, s); } \
    static uint64_t nat_##K(uint64_t a, uint64_t b, uint64_t c) { return nfp_##K(a); }
ITOF(sqtod)
ITOF(uqtod)
ITOF(sqtos)
ITOF(uqtos)

typedef uint64_t RefFn(uint64_t, uint64_t, uint64_t, float_status*);
typedef uint64_t NatFn(uint64_t, uint64_t, uint64_t);

/* operand kinds: F = float of ESIZE from the special table, I = integer table */
static const struct
{
    const char* name;
    RefFn*      ref;
    NatFn*      nat;
    int         esize; /* of the floating-point inputs */
    int         nops;  /* 1, 2 or 3 */
    bool        int_in;
} sc_tests[] = {
#define T2(N, F, E) {N, ref_##F, nat_##F, E, 2, false}
    T2("fadd s", adds, 4),     T2("fsub s", subs, 4),     T2("fmul s", muls, 4),     T2("fdiv s", divs, 4),
    T2("fmin s", mins, 4),     T2("fmax s", maxs, 4),     T2("fminnm s", minnums, 4), T2("fmaxnm s", maxnums, 4),
    T2("fadd d", addd, 8),     T2("fsub d", subd, 8),     T2("fmul d", muld, 8),     T2("fdiv d", divd, 8),
    T2("fmin d", mind, 8),     T2("fmax d", maxd, 8),     T2("fminnm d", minnumd, 8), T2("fmaxnm d", maxnumd, 8),
    T2("fcmp s", cmps, 4),     T2("fcmpe s", cmpes, 4),   T2("fcmp d", cmpd, 8),     T2("fcmpe d", cmped, 8),
#undef T2
    {"fsqrt s", ref_sqrts, nat_sqrts, 4, 1, false},
    {"fsqrt d", ref_sqrtd, nat_sqrtd, 8, 1, false},
    {"fcvt d<-s", ref_fcvtds, nat_fcvtds, 4, 1, false},
    {"fcvt s<-d", ref_fcvtsd, nat_fcvtsd, 8, 1, false},
    {"fmadd s", ref_muladds, nat_muladds, 4, 3, false},
    {"fmadd d", ref_muladdd, nat_muladdd, 8, 3, false},
    {"scvtf d<-x", ref_sqtod, nat_sqtod, 8, 1, true},
    {"ucvtf d<-x", ref_uqtod, nat_uqtod, 8, 1, true},
    {"scvtf s<-x", ref_sqtos, nat_sqtos, 4, 1, true},
    {"ucvtf s<-x", ref_uqtos, nat_uqtos, 4, 1, true},
};

static const char* test_scalar(void)
{
    float_status ref;

    for (unsigned t = 0; t < ARRAY_SIZE(sc_tests); t++) {
        int      es   = sc_tests[t].esize;
        unsigned nval = sc_tests[t].int_in ? ARRAY_SIZE(tab_i) : es == 8 ? ARRAY_SIZE(tab_d) : ARRAY_SIZE(tab_s);
        unsigned nj   = sc_tests[t].nops >= 2 ? nval : 1;
        unsigned nk   = sc_tests[t].nops >= 3 ? nval : 1;

        for (unsigned i = 0; i < nval; i++) {
            for (unsigned j = 0; j < nj; j++) {
                for (unsigned k = 0; k < nk; k++) {
                    uint64_t a = sc_tests[t].int_in ? tab_i[i] : val(es, i);
                    uint64_t b = val(es, j), c = val(es, k);
                    uint64_t want, got;
                    uint32_t wf, gf;

                    ref_reset(&ref, float_round_nearest_even);
                    want = sc_tests[t].ref(a, b, c, &ref);
                    wf   = ref_flags(&ref);
                    host_clear_fpsr();
                    got = sc_tests[t].nat(a, b, c);
                    gf  = host_flags();
                    st_count++;
                    FAIL_IF(want != got || wf != gf,
                            "%s(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 "): host 0x%" PRIx64 " fpsr 0x%x, "
                            "softfloat 0x%" PRIx64 " fpsr 0x%x",
                            sc_tests[t].name, a, b, c, got, gf, want, wf);
                }
            }
        }
    }
    return NULL;
}

/* ---- float -> integer, every rounding mode the translator sets ---- */

typedef uint64_t ToiRef(uint64_t, float_status*);
typedef bool     ToiNat(uint64_t, FloatRoundMode, uint64_t*);

#define TOI(K, IT, OT)                                                                             \
    static uint64_t toi_ref_##K(uint64_t x, float_status* s) { return helper_vfp_to##K((IT)x, 0, s); } \
    static bool     toi_nat_##K(uint64_t x, FloatRoundMode rm, uint64_t* r)                        \
    {                                                                                              \
        OT   o;                                                                                    \
        bool ok = nfp_to##K((IT)x, rm, &o);                                                        \
        *r      = o;                                                                               \
        return ok;                                                                                 \
    }
TOI(sqd, uint64_t, uint64_t)
TOI(uqd, uint64_t, uint64_t)
TOI(sld, uint64_t, uint64_t)
TOI(uld, uint64_t, uint64_t)
TOI(sqs, uint32_t, uint64_t)
TOI(uqs, uint32_t, uint64_t)
TOI(sls, uint32_t, uint32_t)
TOI(uls, uint32_t, uint32_t)

static const struct
{
    const char* name;
    ToiRef*     ref;
    ToiNat*     nat;
    int         esize;
} toi_tests[] = {
    {"fcvt*s x<-d", toi_ref_sqd, toi_nat_sqd, 8}, {"fcvt*u x<-d", toi_ref_uqd, toi_nat_uqd, 8},
    {"fcvt*s w<-d", toi_ref_sld, toi_nat_sld, 8}, {"fcvt*u w<-d", toi_ref_uld, toi_nat_uld, 8},
    {"fcvt*s x<-s", toi_ref_sqs, toi_nat_sqs, 4}, {"fcvt*u x<-s", toi_ref_uqs, toi_nat_uqs, 4},
    {"fcvt*s w<-s", toi_ref_sls, toi_nat_sls, 4}, {"fcvt*u w<-s", toi_ref_uls, toi_nat_uls, 4},
};

static const FloatRoundMode toi_modes[] = {
    float_round_to_zero, float_round_nearest_even, float_round_ties_away, float_round_up, float_round_down,
};

static const char* test_toi(void)
{
    float_status ref;

    for (unsigned t = 0; t < ARRAY_SIZE(toi_tests); t++) {
        int      es   = toi_tests[t].esize;
        unsigned nval = es == 8 ? ARRAY_SIZE(tab_d) : ARRAY_SIZE(tab_s);
        unsigned next = es == 8 ? ARRAY_SIZE(cvt_d) : ARRAY_SIZE(cvt_s);

        for (unsigned r = 0; r < ARRAY_SIZE(toi_modes); r++) {
            for (unsigned i = 0; i < nval + next; i++) {
                uint64_t x = i < nval ? val(es, i) : es == 8 ? cvt_d[i - nval] : cvt_s[i - nval];
                uint64_t want, got = 0;
                uint32_t wf, gf;
                bool     ok;

                ref_reset(&ref, toi_modes[r]);
                want = toi_tests[t].ref(x, &ref);
                wf   = ref_flags(&ref);
                host_clear_fpsr();
                ok = toi_tests[t].nat(x, toi_modes[r], &got);
                gf = host_flags();
                st_count++;
                FAIL_IF(!ok || want != got || wf != gf,
                        "%s rmode %d (0x%" PRIx64 "): host 0x%" PRIx64 " fpsr 0x%x, softfloat 0x%" PRIx64
                        " fpsr 0x%x",
                        toi_tests[t].name, toi_modes[r], x, got, gf, want, wf);
            }
        }
    }
    return NULL;
}

const char* arm_native_fp_selftest(unsigned* ncases)
{
    const char* bad;

    st_count = 0;
    if (!(bad = test_vec()) && !(bad = test_idx()) && !(bad = test_scalar())) { bad = test_toi(); }
    host_clear_fpsr();
    *ncases = st_count;
    return bad;
}

#endif /* ARM_NATIVE_FP_HOST */
