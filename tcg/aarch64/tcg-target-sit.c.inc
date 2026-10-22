/*
 * Same-ISA translation regions (include/tcg/tcg-sit.h): AArch64 guest
 * instructions emitted as the same host instructions.
 *
 * Inside a region TCG holds nothing in host registers (sit_enter saved its
 * globals), so the region owns every allocatable host register:
 *
 *  - guest registers live in host registers from sit_pool[], loaded from
 *    the CPU state on first use and stored back when the region ends,
 *    before a conditional branch out of it, and in every slow path before
 *    a helper call (so the CPU state is exact wherever a guest exception
 *    or a TB invalidation can happen);
 *  - the guest NZCV lives in the host NZCV once an instruction of the
 *    region has written or read it; everything this file emits around the
 *    guest instructions leaves the host flags alone (the TLB check uses
 *    EOR + CBNZ, not CMP + B.NE);
 *  - SIT_S0..SIT_S3 are scratch and never hold a guest register.
 *
 * Included from tcg-target.c.inc. Copyright the Pyre authors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "tcg/tcg-sit.h"
#include "accel/tcg/native-int.h"

#define SIT_S0 TCG_REG_X16
#define SIT_S1 TCG_REG_X17
#define SIT_S2 TCG_REG_X30
#define SIT_S3 TCG_REG_X15

/* Host registers for guest registers. Callee-saved first: they survive slow-path calls. */
static const uint8_t sit_pool[] = {
    TCG_REG_X20, TCG_REG_X21, TCG_REG_X22, TCG_REG_X23, TCG_REG_X24, TCG_REG_X25, TCG_REG_X26, TCG_REG_X27,
    TCG_REG_X28, TCG_REG_X0,  TCG_REG_X1,  TCG_REG_X2,  TCG_REG_X3,  TCG_REG_X4,  TCG_REG_X5,  TCG_REG_X6,
    TCG_REG_X7,  TCG_REG_X8,  TCG_REG_X9,  TCG_REG_X10, TCG_REG_X11, TCG_REG_X12, TCG_REG_X13, TCG_REG_X14,
};

/* Host registers a helper call may clobber, among the pool. */
#define SIT_CALL_CLOBBERED 0x0003ffffu /* x0-x17 */

enum
{
    SIT_FL_ENV,   /* guest NZCV only in the CPU state */
    SIT_FL_HOST,  /* in the host NZCV, equal to the CPU state */
    SIT_FL_DIRTY, /* in the host NZCV, newer than the CPU state */
};

typedef struct SitMap
{
    int8_t   host[SIT_NUM_REGS]; /* host register of each guest register, or -1 */
    uint32_t dirty;              /* guest registers newer than the CPU state */
    uint32_t busy;               /* host registers in use */
    uint8_t  nzcv;               /* SIT_FL_* */
} SitMap;

typedef struct SitSlow
{
    QSIMPLEQ_ENTRY(SitSlow) next;
    const SitInsn*       si;
    SitMap               map; /* before the access: what to write back, what to reload */
    TCGReg               addr;
    TCGReg               rt, rt2; /* host registers of the data registers (XZR: 31) */
    tcg_insn_unit*       label_ptr;
    const tcg_insn_unit* raddr;
} SitSlow;

struct TCGSitState
{
    const SitLayout* lay;
    SitMap           m;
    bool             open;
    QSIMPLEQ_HEAD(, SitSlow) slow;
};

static inline int sit_xofs(TCGContext* s, int g) { return s->sit->lay->xregs + 8 * g; }

static void sit_map_reset(SitMap* m)
{
    memset(m->host, -1, sizeof(m->host));
    m->dirty = 0;
    m->busy  = 0;
    m->nzcv  = SIT_FL_ENV;
}

/* A64 condition codes, for the host instructions emitted here. */
enum
{
    SIT_CC_EQ = 0,
    SIT_CC_NE = 1,
    SIT_CC_CS = 2,
    SIT_CC_MI = 4,
    SIT_CC_VS = 6,
};

/* CSET Wd, cc / CSETM Wd, cc (CSINC/CSINV Wd, WZR, WZR, !cc): no flags written. */
static void sit_cset(TCGContext* s, TCGReg rd, unsigned cc) { tcg_out32(s, 0x1a9f07e0u | (cc ^ 1) << 12 | rd); }

static void sit_csetm(TCGContext* s, TCGReg rd, unsigned cc) { tcg_out32(s, 0x5a9f03e0u | (cc ^ 1) << 12 | rd); }

/* ORR Wd, Wn, Wm, LSL #sh */
static void sit_orr_w_lsl(TCGContext* s, TCGReg rd, TCGReg rn, TCGReg rm, unsigned sh)
{ tcg_out32(s, 0x2a000000u | rm << 16 | sh << 10 | rn << 5 | rd); }

/* MOV Xd, Xm with Xm == 31 meaning XZR (ORR Xd, XZR, Xm). */
static void sit_mov(TCGContext* s, TCGReg rd, TCGReg rm)
{
    if (rd != rm) { tcg_out32(s, 0xaa0003e0u | rm << 16 | rd); }
}

/* Xd = Xn + imm, any imm; Xd may be Xn; uses SIT_S1 for large values (Xd, Xn != SIT_S1). */
static void sit_addi(TCGContext* s, TCGReg rd, TCGReg rn, int64_t imm)
{
    uint64_t a = imm < 0 ? -(uint64_t)imm : imm;
    if (a < 0x1000000 && (a & 0xfff000) && (a & 0xfff)) {
        tcg_out_insn_3401(s, imm < 0 ? I3401_SUBI : I3401_ADDI, TCG_TYPE_I64, rd, rn, a & 0xfff000);
        rn = rd;
        a &= 0xfff;
    }
    if (a == 0 && rd == rn) { return; }
    if (is_aimm(a)) {
        tcg_out_insn_3401(s, imm < 0 ? I3401_SUBI : I3401_ADDI, TCG_TYPE_I64, rd, rn, a);
        return;
    }
    tcg_debug_assert(rd != SIT_S1 && rn != SIT_S1);
    tcg_out_movi(s, TCG_TYPE_I64, SIT_S1, imm);
    tcg_out_insn(s, 3502, ADD, TCG_TYPE_I64, rd, rn, SIT_S1);
}

/* Host NZCV -> CPU state (CF, VF, NF, ZF). Leaves the host flags unchanged. */
static void sit_nzcv_store(TCGContext* s)
{
    const SitLayout* lay = s->sit->lay;

    sit_cset(s, SIT_S0, SIT_CC_CS);
    sit_csetm(s, SIT_S1, SIT_CC_VS);
    tcg_out_ldst(s, I3312_STRW, SIT_S0, TCG_AREG0, lay->cf, 2);
    tcg_out_ldst(s, I3312_STRW, SIT_S1, TCG_AREG0, lay->vf, 2);
    sit_csetm(s, SIT_S0, SIT_CC_MI);
    sit_cset(s, SIT_S1, SIT_CC_NE);
    tcg_out_ldst(s, I3312_STRW, SIT_S0, TCG_AREG0, lay->nf, 2);
    tcg_out_ldst(s, I3312_STRW, SIT_S1, TCG_AREG0, lay->zf, 2);
}

/* CPU state -> host NZCV. */
static void sit_nzcv_load(TCGContext* s)
{
    const SitLayout* lay = s->sit->lay;

    tcg_out_ldst(s, I3312_LDRW, SIT_S0, TCG_AREG0, lay->nf, 2);
    tcg_out_logicali(s, I3404_ANDI, TCG_TYPE_I32, SIT_S0, SIT_S0, 0x80000000u);
    tcg_out_ldst(s, I3312_LDRW, SIT_S1, TCG_AREG0, lay->zf, 2);
    tcg_out_insn(s, 3401, SUBSI, TCG_TYPE_I32, TCG_REG_XZR, SIT_S1, 0);
    sit_cset(s, SIT_S1, SIT_CC_EQ);
    sit_orr_w_lsl(s, SIT_S0, SIT_S0, SIT_S1, 30);
    tcg_out_ldst(s, I3312_LDRW, SIT_S1, TCG_AREG0, lay->cf, 2);
    sit_orr_w_lsl(s, SIT_S0, SIT_S0, SIT_S1, 29);
    tcg_out_ldst(s, I3312_LDRW, SIT_S1, TCG_AREG0, lay->vf, 2);
    tcg_out_insn(s, 3402, UBFM, TCG_TYPE_I32, SIT_S1, SIT_S1, 0, 31, 31); /* lsr w, w, #31 */
    sit_orr_w_lsl(s, SIT_S0, SIT_S0, SIT_S1, 28);
    tcg_out32(s, 0xd51b4200u | SIT_S0); /* msr nzcv, x16 */
}

/* Write the dirty guest registers (and the guest NZCV if dirty) of @m to the CPU state. */
static void sit_writeback(TCGContext* s, const SitMap* m)
{
    uint32_t dirty = m->dirty;

    while (dirty) {
        int g = ctz32(dirty);
        int o = sit_xofs(s, g);

        dirty &= dirty - 1;
        if (g < SIT_NUM_REGS - 1 && (dirty & (1u << (g + 1))) && o + 8 < 0x200) {
            tcg_out_insn(s, 3314, STP, m->host[g], m->host[g + 1], TCG_AREG0, o, 1, 0);
            dirty &= ~(1u << (g + 1));
        }
        else {
            tcg_out_ldst(s, I3312_STRX, m->host[g], TCG_AREG0, o, 3);
        }
    }
    if (m->nzcv == SIT_FL_DIRTY) { sit_nzcv_store(s); }
}

/* A free host register; if none, evict a guest register outside @keep. */
static TCGReg sit_alloc(TCGContext* s, uint32_t keep)
{
    SitMap* m = &s->sit->m;

    for (int i = 0; i < ARRAY_SIZE(sit_pool); i++) {
        if (!(m->busy & (1u << sit_pool[i]))) { return sit_pool[i]; }
    }
    for (int g = 0; g < SIT_NUM_REGS; g++) {
        if (m->host[g] >= 0 && !(keep & (1u << g))) {
            TCGReg r = m->host[g];

            if (m->dirty & (1u << g)) { tcg_out_ldst(s, I3312_STRX, r, TCG_AREG0, sit_xofs(s, g), 3); }
            m->dirty &= ~(1u << g);
            m->busy  &= ~(1u << r);
            m->host[g] = -1;
            return r;
        }
    }
    g_assert_not_reached();
}

static void sit_bind(TCGContext* s, int g, TCGReg r)
{
    s->sit->m.host[g]  = r;
    s->sit->m.busy    |= 1u << r;
}

/* The host register holding guest register @g, loading it if needed. */
static TCGReg sit_use(TCGContext* s, int g, uint32_t keep)
{
    TCGReg r;

    if (g == SIT_REG_ZR) { return TCG_REG_XZR; }
    if (s->sit->m.host[g] >= 0) { return s->sit->m.host[g]; }
    r = sit_alloc(s, keep);
    tcg_out_ldst(s, I3312_LDRX, r, TCG_AREG0, sit_xofs(s, g), 3);
    sit_bind(s, g, r);
    return r;
}

/* A host register for guest register @g, which is about to be written in full. */
static TCGReg sit_def(TCGContext* s, int g, uint32_t keep)
{
    TCGReg r;

    if (g == SIT_REG_ZR) { return TCG_REG_XZR; }
    if (s->sit->m.host[g] >= 0) { return s->sit->m.host[g]; }
    r = sit_alloc(s, keep);
    sit_bind(s, g, r);
    return r;
}

static void sit_dirty(TCGContext* s, int g)
{
    if (g != SIT_REG_ZR) { s->sit->m.dirty |= 1u << g; }
}

static inline uint32_t sit_bit(int g) { return g == SIT_REG_ZR ? 0 : 1u << g; }

static void sit_need_nzcv(TCGContext* s)
{
    if (s->sit->m.nzcv == SIT_FL_ENV) {
        sit_nzcv_load(s);
        s->sit->m.nzcv = SIT_FL_HOST;
    }
}

static void tcg_out_sit_enter(TCGContext* s, const SitLayout* lay)
{
    if (!s->sit) {
        s->sit = tcg_malloc(sizeof(*s->sit));
        QSIMPLEQ_INIT(&s->sit->slow);
    }
    tcg_debug_assert(lay->xregs >= 0 && lay->xregs + 8 * SIT_NUM_REGS <= 0x8000);
    s->sit->lay  = lay;
    s->sit->open = true;
    sit_map_reset(&s->sit->m);
}

static void sit_close(TCGContext* s)
{
    sit_writeback(s, &s->sit->m);
    sit_map_reset(&s->sit->m);
    s->sit->open = false;
}

static void tcg_out_sit_exit(TCGContext* s)
{
    tcg_debug_assert(s->sit && s->sit->open);
    sit_close(s);
}

static void sit_copy(TCGContext* s, const SitInsn* si)
{
    uint32_t insn = si->insn, keep = 0;
    TCGReg   h[4];

    for (int i = 0; i < si->nfield; i++) { keep |= sit_bit(si->f[i].reg); }
    if (si->nzcv & SIT_NZCV_R) { sit_need_nzcv(s); }
    /* Sources first (they may load), then the written-only destinations. */
    for (int i = 0; i < si->nfield; i++) {
        if (si->f[i].use & SIT_USE_R) { h[i] = sit_use(s, si->f[i].reg, keep); }
    }
    for (int i = 0; i < si->nfield; i++) {
        if (!(si->f[i].use & SIT_USE_R)) { h[i] = sit_def(s, si->f[i].reg, keep); }
    }
    for (int i = 0; i < si->nfield; i++) { insn = deposit32(insn, si->f[i].shift, 5, h[i]); }
    tcg_out32(s, insn);
    for (int i = 0; i < si->nfield; i++) {
        if (si->f[i].use & SIT_USE_W) { sit_dirty(s, si->f[i].reg); }
    }
    if (si->nzcv & SIT_NZCV_W) { s->sit->m.nzcv = SIT_FL_DIRTY; }
}

static void sit_ldenv(TCGContext* s, const SitInsn* si)
{
    TCGReg rd = sit_def(s, si->rt, sit_bit(si->rt));

    if (rd == TCG_REG_XZR) { return; }
    tcg_out_ldst(s, I3312_LDRX, rd, TCG_AREG0, si->imm, 3);
    sit_dirty(s, si->rt);
}

static void sit_addpc(TCGContext* s, const SitInsn* si)
{
    TCGReg rd = sit_def(s, si->rt, sit_bit(si->rt));

    if (rd == TCG_REG_XZR) { return; }
    if (si->pcrel) {
        tcg_out_ldst(s, I3312_LDRX, SIT_S0, TCG_AREG0, s->sit->lay->pc, 3);
        sit_addi(s, rd, SIT_S0, si->imm);
    }
    else {
        tcg_out_movi(s, TCG_TYPE_I64, rd, si->imm);
    }
    sit_dirty(s, si->rt);
}

/*
 * The softmmu TLB fast path for the guest address in @addr, as
 * prepare_host_addr() but flag-free: leaves the host address addend in
 * SIT_S1 and returns the CBNZ to patch with the slow path.
 */
static tcg_insn_unit* sit_tlb(TCGContext* s, TCGReg addr, MemOpIdx oi, bool is_ld)
{
    MemOp        opc    = get_memop(oi);
    MemOp        s_bits = opc & MO_SIZE;
    TCGAtomAlign aa = atom_and_align_for_opc(s, opc, have_lse2 ? MO_ATOM_WITHIN16 : MO_ATOM_IFALIGN, s_bits == MO_128);
    unsigned     a_mask = (1 << aa.align) - 1;
    unsigned     s_mask = (1u << s_bits) - 1;
    int          fast_ofs = tlb_mask_table_ofs(s, get_mmuidx(oi));
    uint64_t     compare_mask = (uint64_t)TARGET_PAGE_MASK | a_mask;
    tcg_insn_unit* label_ptr;

    if (fast_ofs >= -0x200) { tcg_out_insn(s, 3314, LDP, SIT_S0, SIT_S1, TCG_AREG0, fast_ofs, 1, 0); }
    else {
        tcg_out_insn(s, 3401, SUBI, TCG_TYPE_I64, SIT_S0, TCG_AREG0, -fast_ofs);
        tcg_out_insn(s, 3314, LDP, SIT_S0, SIT_S1, SIT_S0, 0, 1, 0);
    }
    tcg_out_insn(s, 3502S, AND_LSR, TCG_TYPE_I64, SIT_S0, SIT_S0, addr, TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS);
    tcg_out_insn(s, 3502, ADD, TCG_TYPE_I64, SIT_S1, SIT_S1, SIT_S0);
    if (is_ld) { tcg_out_insn(s, 3314, LDP, SIT_S0, SIT_S1, SIT_S1, offsetof(CPUTLBEntry, addr_read), 1, 0); }
    else {
        tcg_out_insn(s, 3314, LDP, SIT_S1, SIT_S0, SIT_S1, offsetof(CPUTLBEntry, addend), 1, 0);
    }
    if (a_mask >= s_mask) { tcg_out_logicali(s, I3404_ANDI, TCG_TYPE_I64, SIT_S2, addr, compare_mask); }
    else {
        tcg_out_insn(s, 3401, ADDI, TCG_TYPE_I64, SIT_S2, addr, s_mask - a_mask);
        tcg_out_logicali(s, I3404_ANDI, TCG_TYPE_I64, SIT_S2, SIT_S2, compare_mask);
    }
    tcg_out_insn(s, 3510, EOR, TCG_TYPE_I64, SIT_S0, SIT_S0, SIT_S2);
    label_ptr = s->code_ptr;
    tcg_out_insn(s, 3201, CBNZ, TCG_TYPE_I64, SIT_S0, 0);
    return label_ptr;
}

static AArch64Insn sit_ld_insn(const SitInsn* si)
{
    static const AArch64Insn zext[4] = {I3312_LDRB, I3312_LDRH, I3312_LDRW, I3312_LDRX};
    static const AArch64Insn sext[3] = {I3312_LDRSBX, I3312_LDRSHX, I3312_LDRSWX};
    static const AArch64Insn sextw[2] = {I3312_LDRSBW, I3312_LDRSHW};

    switch (si->ext) {
        case SIT_LD_SEXT : return sext[si->size];
        case SIT_LD_SEXTW: return sextw[si->size];
        default          : return zext[si->size];
    }
}

static void sit_ldst(TCGContext* s, const SitInsn* si)
{
    static const AArch64Insn st[4] = {I3312_STRB, I3312_STRH, I3312_STRW, I3312_STRX};
    SitMap*  m     = &s->sit->m;
    bool     is_ld = si->kind == SIT_LD || si->kind == SIT_LDP;
    bool     pair  = si->kind == SIT_LDP || si->kind == SIT_STP;
    uint32_t keep  = sit_bit(si->rn) | sit_bit(si->rt) | (pair ? sit_bit(si->rt2) : 0);
    TCGReg   hn, hm = TCG_REG_XZR, ht, ht2 = TCG_REG_XZR, addr;
    SitSlow* sp;

    if (si->mode == SIT_AM_REG) { keep |= sit_bit(si->rm); }
    hn = sit_use(s, si->rn, keep);
    if (si->mode == SIT_AM_REG) { hm = sit_use(s, si->rm, keep); }
    if (is_ld) {
        ht = sit_def(s, si->rt, keep);
        if (pair) { ht2 = sit_def(s, si->rt2, keep); }
    }
    else {
        ht = sit_use(s, si->rt, keep);
        if (pair) { ht2 = sit_use(s, si->rt2, keep); }
    }

    sp      = tcg_malloc(sizeof(*sp));
    sp->si  = si;
    sp->map = *m;
    sp->rt  = ht;
    sp->rt2 = ht2;

    switch (si->mode) {
        case SIT_AM_OFF:
        case SIT_AM_PRE:
            if (si->imm) {
                addr = SIT_S3;
                sit_addi(s, addr, hn, si->imm);
            }
            else {
                addr = hn;
            }
            break;
        case SIT_AM_POST: addr = hn; break;
        case SIT_AM_REG:
            /* ADD Xd, Xn, Rm, <extend> #shift */
            addr = SIT_S3;
            tcg_out32(s, 0x8b200000u | hm << 16 | si->ext_opt << 13 | si->ext_shift << 10 | hn << 5 | addr);
            break;
        default: g_assert_not_reached();
    }
    sp->addr      = addr;
    sp->label_ptr = sit_tlb(s, addr, si->oi, is_ld);

    switch (si->kind) {
        case SIT_LD: tcg_out_ldst_r(s, sit_ld_insn(si), ht, SIT_S1, TCG_TYPE_I64, addr); break;
        case SIT_ST: tcg_out_ldst_r(s, st[si->size], ht, SIT_S1, TCG_TYPE_I64, addr); break;
        case SIT_LDP:
        case SIT_STP: {
            /* LDP/LDPSW/STP Rt, Rt2, [x17]: opc from the size (and sign), L from the kind */
            uint32_t opc = si->size == 3 ? 0xa9000000u : si->ext == SIT_LD_SEXT ? 0x69000000u : 0x29000000u;

            tcg_out_insn(s, 3502, ADD, TCG_TYPE_I64, SIT_S1, SIT_S1, addr);
            tcg_out32(s, opc | (is_ld ? 1u << 22 : 0) | ht2 << 10 | SIT_S1 << 5 | ht);
            break;
        }
        default: g_assert_not_reached();
    }
    sp->raddr = tcg_splitwx_to_rx(s->code_ptr);
    QSIMPLEQ_INSERT_TAIL(&s->sit->slow, sp, next);

    if (si->mode == SIT_AM_PRE || si->mode == SIT_AM_POST) {
        sit_addi(s, hn, hn, si->imm);
        sit_dirty(s, si->rn);
    }
    if (is_ld) {
        sit_dirty(s, si->rt);
        if (pair) { sit_dirty(s, si->rt2); }
    }
}

/* Branch to @l: the region ends here, with the guest state written back. */
static void sit_branch(TCGContext* s, const SitInsn* si, TCGLabel* l)
{
    TCGReg rt = TCG_REG_XZR;

    if (si->kind == SIT_BCOND) { tcg_debug_assert(s->sit->m.nzcv != SIT_FL_ENV); }
    else {
        rt = sit_use(s, si->rt, sit_bit(si->rt));
    }
    sit_close(s);
    switch (si->kind) {
        case SIT_BCOND:
            tcg_out_reloc(s, s->code_ptr, R_AARCH64_CONDBR19, l, 0);
            tcg_out32(s, I3202_B_C | si->cond);
            break;
        case SIT_CBZ:
            tcg_out_reloc(s, s->code_ptr, R_AARCH64_CONDBR19, l, 0);
            tcg_out_insn_3201(s, si->cond ? I3201_CBNZ : I3201_CBZ, si->bit ? TCG_TYPE_I64 : TCG_TYPE_I32, rt, 0);
            break;
        case SIT_TBZ:
            tcg_out_reloc(s, s->code_ptr, R_AARCH64_TSTBR14, l, 0);
            tcg_out_insn_3205(s, si->cond ? I3205_TBNZ : I3205_TBZ, rt, si->bit, 0);
            break;
        default: g_assert_not_reached();
    }
}

static void tcg_out_sit(TCGContext* s, const SitInsn* si, TCGLabel* l)
{
    tcg_debug_assert(s->sit && s->sit->open);
    switch (si->kind) {
        case SIT_COPY : sit_copy(s, si); break;
        case SIT_ADDPC: sit_addpc(s, si); break;
        case SIT_LDENV: sit_ldenv(s, si); break;
        case SIT_LD   :
        case SIT_ST   :
        case SIT_LDP  :
        case SIT_STP  : sit_ldst(s, si); break;
        case SIT_BCOND:
        case SIT_CBZ  :
        case SIT_TBZ  : sit_branch(s, si, l); break;
        default       : g_assert_not_reached();
    }
}

/* The out-of-line path of a SIT load or store: the CPU state is made exact, then the TCG helper. */
static bool sit_slow_out(TCGContext* s, SitSlow* sp)
{
    const SitInsn*   si    = sp->si;
    MemOp            mop   = get_memop(si->oi);
    bool             is_ld = si->kind == SIT_LD || si->kind == SIT_LDP;
    uint32_t         reload;

    if (!reloc_pc19(sp->label_ptr, tcg_splitwx_to_rx(s->code_ptr))) { return false; }

    if (!tcg_native_int_fault) { sit_writeback(s, &sp->map); }

    /* env, addr, [data], oi, retaddr */
    if (is_ld) {
        tcg_out_mov(s, TCG_TYPE_I64, TCG_REG_X1, sp->addr);
        tcg_out_movi(s, TCG_TYPE_I32, TCG_REG_X2, si->oi);
        tcg_out_movi(s, TCG_TYPE_PTR, TCG_REG_X3, (uintptr_t)sp->raddr);
        tcg_out_mov(s, TCG_TYPE_PTR, TCG_REG_X0, TCG_AREG0);
        tcg_out_call_int(s, qemu_ld_helpers[mop & MO_SSIZE]);

        /* Results to the CPU state; the reload below brings them into the data registers. */
        if (si->kind == SIT_LD) {
            if (si->rt != SIT_REG_ZR) {
                if (si->ext == SIT_LD_SEXTW) { tcg_out_movr(s, TCG_TYPE_I32, TCG_REG_X0, TCG_REG_X0); }
                tcg_out_ldst(s, I3312_STRX, TCG_REG_X0, TCG_AREG0, sit_xofs(s, si->rt), 3);
            }
        }
        else if (si->size == 3) {
            if (si->rt != SIT_REG_ZR) { tcg_out_ldst(s, I3312_STRX, TCG_REG_X0, TCG_AREG0, sit_xofs(s, si->rt), 3); }
            if (si->rt2 != SIT_REG_ZR) {
                tcg_out_ldst(s, I3312_STRX, TCG_REG_X1, TCG_AREG0, sit_xofs(s, si->rt2), 3);
            }
        }
        else {
            bool sign = si->ext == SIT_LD_SEXT;

            /* Two words in one doubleword: Rt the low, Rt2 the high (little-endian only). */
            tcg_out_insn_3402(s, sign ? I3402_SBFM : I3402_UBFM, TCG_TYPE_I64, SIT_S0, TCG_REG_X0, 1, 0, 31);
            tcg_out_insn_3402(s, sign ? I3402_SBFM : I3402_UBFM, TCG_TYPE_I64, SIT_S1, TCG_REG_X0, 1, 32, 63);
            if (si->rt != SIT_REG_ZR) { tcg_out_ldst(s, I3312_STRX, SIT_S0, TCG_AREG0, sit_xofs(s, si->rt), 3); }
            if (si->rt2 != SIT_REG_ZR) { tcg_out_ldst(s, I3312_STRX, SIT_S1, TCG_AREG0, sit_xofs(s, si->rt2), 3); }
        }
    }
    else {
        TCGReg data = TCG_REG_X2;

        sit_mov(s, SIT_S0, sp->rt);
        if (si->kind == SIT_STP) {
            if (si->size == 3) { sit_mov(s, SIT_S2, sp->rt2); }
            else {
                /* x16 = Wt | Xt2 << 32 */
                tcg_out_movr(s, TCG_TYPE_I32, SIT_S0, SIT_S0);
                tcg_out_insn(s, 3402, BFM, TCG_TYPE_I64, SIT_S0, sp->rt2, 1, 32, 31);
            }
        }
        sit_mov(s, SIT_S1, sp->addr);
        tcg_out_mov(s, TCG_TYPE_I64, TCG_REG_X1, SIT_S1);
        tcg_out_mov(s, TCG_TYPE_I64, data, SIT_S0);
        if (si->kind == SIT_STP && si->size == 3) {
            /* Int128 in x2:x3 (an even pair on Linux, the next two registers on Darwin: the same here) */
            tcg_out_mov(s, TCG_TYPE_I64, TCG_REG_X3, SIT_S2);
            data = TCG_REG_X3;
        }
        tcg_out_movi(s, TCG_TYPE_I32, data + 1, si->oi);
        tcg_out_movi(s, TCG_TYPE_PTR, data + 2, (uintptr_t)sp->raddr);
        tcg_out_mov(s, TCG_TYPE_PTR, TCG_REG_X0, TCG_AREG0);
        tcg_out_call_int(s, qemu_st_helpers[mop & MO_SIZE]);
    }

    /* Reload what the call clobbered, and the loaded registers. */
    reload = 0;
    for (int g = 0; g < SIT_NUM_REGS; g++) {
        if (sp->map.host[g] >= 0 && (SIT_CALL_CLOBBERED & (1u << sp->map.host[g]))) { reload |= 1u << g; }
    }
    if (is_ld) {
        reload |= sit_bit(si->rt);
        if (si->kind == SIT_LDP) { reload |= sit_bit(si->rt2); }
    }
    while (reload) {
        int    g = ctz32(reload);
        TCGReg r = sp->map.host[g] >= 0 ? sp->map.host[g] : (g == si->rt ? sp->rt : sp->rt2);

        reload &= reload - 1;
        tcg_out_ldst(s, I3312_LDRX, r, TCG_AREG0, sit_xofs(s, g), 3);
    }
    if (sp->map.nzcv != SIT_FL_ENV) { sit_nzcv_load(s); }
    tcg_out_goto(s, sp->raddr);
    return true;
}

static int tcg_out_sit_finalize(TCGContext* s)
{
    SitSlow* sp;

    if (!s->sit) { return 0; }
    tcg_debug_assert(!s->sit->open);
    QSIMPLEQ_FOREACH (sp, &s->sit->slow, next) {
        if (!sit_slow_out(s, sp)) { return -2; }
        if (unlikely((void*)s->code_ptr > s->code_gen_highwater)) { return -1; }
    }
    return 0;
}
