/*
 * Guest floating point on the host FPU: mode tracking and flag folding.
 * See target/arm/native-fp.h and docs/NATIVE-FP.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "internals.h"
#include "fpu/softfloat.h"
#include "accel/tcg/native-fp.h"
#include "native-fp.h"

bool arm_native_fp_on;
bool arm_native_fp_fault;

#if ARM_NATIVE_FP_HOST

/* FPSR cumulative exception bits IOC, DZC, OFC, UFC, IXC (same layout on host and guest). */
#define NFP_CUM 0x1fULL

static inline uint64_t host_fpsr(void)
{
    uint64_t v;
    asm volatile("mrs %0, fpsr" : "=r"(v)::"memory");
    return v;
}

static inline void host_set_fpsr(uint64_t v) { asm volatile("msr fpsr, %0" ::"r"(v) : "memory"); }

static inline uint64_t host_fpcr(void)
{
    uint64_t v;
    asm volatile("mrs %0, fpcr" : "=r"(v)::"memory");
    return v;
}

static inline void host_set_fpcr(uint64_t v) { asm volatile("msr fpcr, %0" ::"r"(v) : "memory"); }

/* True between arm_native_fp_exec_enter() and _exit() on this thread. */
static __thread bool nfp_in_exec;

static inline bool nfp_live(CPUARMState* env) { return env->vfp.fp_status[FPST_A64].host_fpu_native; }

static inline void nfp_set_live(CPUARMState* env, bool live)
{ env->vfp.fp_status[FPST_A64].host_fpu_native = live; }

static int nfp_fpsr_to_float_flags(uint64_t f)
{
    return ((f & FPSR_IOC) ? float_flag_invalid : 0) | ((f & FPSR_DZC) ? float_flag_divbyzero : 0) |
           ((f & FPSR_OFC) ? float_flag_overflow : 0) | ((f & FPSR_UFC) ? float_flag_underflow : 0) |
           ((f & FPSR_IXC) ? float_flag_inexact : 0);
}

/* The guest's cumulative IOC..IXC as recorded outside the host FPSR. */
static uint64_t nfp_guest_cum(CPUARMState* env) { return (env->vfp.fpsr | vfp_get_fpsr_from_host(env)) & NFP_CUM; }

/*
 * Make the host FPSR safe to accumulate into: it may only hold flags the
 * guest already has. Writing FPSR is slow on some cores, so only clear it
 * when it actually holds something else.
 */
static void nfp_go_live(CPUARMState* env)
{
    uint64_t h = host_fpsr();
    if (h & NFP_CUM & ~nfp_guest_cum(env)) { host_set_fpsr(h & ~NFP_CUM); }
    nfp_set_live(env, true);
}

static inline bool nfp_own_thread(CPUARMState* env) { return nfp_in_exec && current_cpu == env_cpu(env); }

void arm_native_fp_fold(CPUARMState* env)
{
    uint64_t h;

    if (!nfp_live(env) || !nfp_own_thread(env)) { return; }
    h = host_fpsr() & NFP_CUM;
    if (h) { float_raise(nfp_fpsr_to_float_flags(h), &env->vfp.fp_status[FPST_A64]); }
}

void arm_native_fp_exec_enter(CPUARMState* env)
{
    static __thread bool fpcr_checked;

    if (!arm_native_fp_on) { return; }
    if (unlikely(!fpcr_checked)) {
        /* QEMU (softfloat's hardfloat path too) assumes the default host FP mode. */
        uint64_t fpcr = host_fpcr();
        if (fpcr != 0) {
            warn_report("native-fp: vCPU thread host FPCR was 0x%" PRIx64 ", resetting it to 0", fpcr);
            host_set_fpcr(0);
        }
        fpcr_checked = true;
    }
    nfp_in_exec = true;
    if (!(env->vfp.fpcr & ARM_NATIVE_FP_FPCR_MASK)) { nfp_go_live(env); }
}

void arm_native_fp_exec_exit(CPUARMState* env)
{
    if (!arm_native_fp_on) { return; }
    arm_native_fp_fold(env);
    nfp_set_live(env, false);
    nfp_in_exec = false;
}

void arm_native_fp_fpsr_written(CPUARMState* env)
{
    uint64_t h;

    if (!nfp_live(env) || !nfp_own_thread(env)) { return; }
    h = host_fpsr();
    if (h & NFP_CUM & ~nfp_guest_cum(env)) { host_set_fpsr(h & ~NFP_CUM); }
}

void arm_native_fp_fpcr_written(CPUARMState* env)
{
    bool want;

    /* Outside cpu_exec the next arm_native_fp_exec_enter() decides. */
    if (!arm_native_fp_on || !nfp_own_thread(env)) { return; }
    want = !(env->vfp.fpcr & ARM_NATIVE_FP_FPCR_MASK);
    if (want && !nfp_live(env)) { nfp_go_live(env); }
    else if (!want) {
        /* The caller folded the host flags under the old (native) regime already. */
        nfp_set_live(env, false);
    }
}

void arm_native_fp_init(void)
{
    static bool done;
    unsigned    ncases = 0;
    const char* bad;
    uint64_t    fpcr;

    if (done || !tcg_native_fp) { return; }
    done                = true;
    arm_native_fp_fault = tcg_native_fp_fault;
    fpcr                = host_fpcr();
    if (fpcr != 0) {
        warn_report("native-fp: host FPCR is 0x%" PRIx64 ", not the default; guest FP stays on softfloat", fpcr);
        return;
    }
    if (!tcg_native_fp_selftest) {
        arm_native_fp_on = true;
        warn_report("native-fp: on, host self-test SKIPPED (x-native-fp-selftest=off)");
        return;
    }
    bad = arm_native_fp_selftest(&ncases);
    host_set_fpsr(0);
    if (bad) {
        warn_report("native-fp: host self-test failed: %s; guest FP stays on softfloat", bad);
        return;
    }
    arm_native_fp_on = true;
    info_report("native-fp: on (host self-test passed, %u cases)", ncases);
}

#else /* !ARM_NATIVE_FP_HOST */

void arm_native_fp_init(void)
{
    static bool done;

    if (done || !tcg_native_fp) { return; }
    done = true;
    info_report("native-fp: not available on this host (needs an AArch64 host); guest FP stays on softfloat");
}

const char* arm_native_fp_selftest(unsigned* ncases) { return "not an AArch64 host"; }
void        arm_native_fp_exec_enter(CPUARMState* env) { }
void        arm_native_fp_exec_exit(CPUARMState* env) { }
void        arm_native_fp_fold(CPUARMState* env) { }
void        arm_native_fp_fpsr_written(CPUARMState* env) { }
void        arm_native_fp_fpcr_written(CPUARMState* env) { }

#endif
