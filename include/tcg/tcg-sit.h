/*
 * Same-ISA translation (SIT): AArch64 guest instructions executed as the
 * same host instructions, for AArch64 hosts (-accel tcg,native-int=on).
 *
 * The A64 translator emits a "SIT region" for a run of covered guest
 * instructions: INDEX_op_sit_enter, one INDEX_op_sit per instruction (in
 * between the usual insn_start ops) and INDEX_op_sit_exit (or a SIT
 * conditional branch, which ends the region). TCG saves its globals to
 * the CPU state at sit_enter and does not allocate host registers inside
 * the region; the AArch64 backend maps the guest registers the region
 * uses to host registers, keeps the guest NZCV in the host NZCV, copies
 * data-processing instructions with their register fields renamed, emits
 * loads and stores as the softmmu TLB fast path (flag-free) plus the host
 * access, and writes the guest state back at the end of the region and
 * before any slow-path helper call (so guest state is precise there).
 * See docs/NATIVE-INT.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TCG_SIT_H
#define TCG_SIT_H

#include "exec/memopidx.h"

/* Guest register numbers: 0-30 are X0-X30, SIT_REG_SP is the stack pointer. */
#define SIT_REG_SP   31
#define SIT_REG_ZR   32 /* XZR/WZR: no register */
#define SIT_NUM_REGS 32

/* SitField.use */
#define SIT_USE_R 1
#define SIT_USE_W 2

/* SitInsn.nzcv */
#define SIT_NZCV_R 1 /* reads the guest NZCV */
#define SIT_NZCV_W 2 /* writes all of the guest NZCV */

typedef enum SitKind
{
    /* Data processing: copy insn, renaming the register fields in f[]. */
    SIT_COPY,
    /* ADR/ADRP: rt = guest pc + imm (pcrel) or rt = imm. */
    SIT_ADDPC,
    /* Loads and stores of general-purpose registers. */
    SIT_LD,
    SIT_ST,
    SIT_LDP,
    SIT_STP,
    /* Loads and stores of SIMD&FP registers (rt, rt2: V0-V31; size 0-4: B, H, S, D, Q). */
    SIT_LDV,
    SIT_STV,
    SIT_LDPV,
    SIT_STPV,
    /* Conditional branches to label: B.cond (cond), CBZ/CBNZ, TBZ/TBNZ. These end the region. */
    SIT_BCOND,
    SIT_CBZ,
    SIT_TBZ,
    /* rt = the 64-bit CPU state field at offset imm (a system register read with no side effects). */
    SIT_LDENV,
} SitKind;

/* SitInsn.mode: addressing mode of a load or store. */
typedef enum SitAddrMode
{
    SIT_AM_OFF,  /* [rn, #imm] */
    SIT_AM_PRE,  /* [rn, #imm]! */
    SIT_AM_POST, /* [rn], #imm */
    SIT_AM_REG,  /* [rn, rm, <ext> #shift] */
    SIT_AM_PC,   /* literal: guest pc + imm (pcrel), or imm */
} SitAddrMode;

/* SitInsn.ext for loads: what happens to the loaded value. */
typedef enum SitLoadExt
{
    SIT_LD_ZEXT,  /* zero-extend to 64 bits */
    SIT_LD_SEXT,  /* sign-extend to 64 bits */
    SIT_LD_SEXTW, /* sign-extend to 32 bits, then zero-extend */
} SitLoadExt;

typedef struct SitField
{
    uint8_t shift; /* bit position of the 5-bit register field */
    uint8_t reg;   /* guest register (0-31, SIT_REG_SP) or SIT_REG_ZR */
    uint8_t use;   /* SIT_USE_R | SIT_USE_W */
} SitField;

typedef struct SitInsn
{
    uint32_t insn; /* guest instruction word (SIT_COPY) */
    uint8_t  kind; /* SitKind */
    uint8_t  nzcv; /* SIT_NZCV_R | SIT_NZCV_W */
    uint8_t  nfield;
    SitField f[4];

    /* SIT_ADDPC, loads and stores, branches */
    uint8_t  rt, rt2, rn, rm; /* guest registers or SIT_REG_ZR */
    uint8_t  mode;            /* SitAddrMode */
    uint8_t  ext_opt;         /* SIT_AM_REG: extend option (as in the insn: 2 UXTW, 3 LSL, 6 SXTW, 7 SXTX) */
    uint8_t  ext_shift;       /* SIT_AM_REG: shift amount */
    uint8_t  size;            /* log2 bytes per register accessed */
    uint8_t  ext;             /* SitLoadExt */
    uint8_t  pcrel;           /* SIT_ADDPC: imm is relative to the guest pc in the CPU state */
    uint8_t  cond;            /* SIT_BCOND: A64 condition; SIT_CBZ/SIT_TBZ: branch if non-zero/set */
    uint8_t  bit;             /* SIT_TBZ: bit number; SIT_CBZ: 1 for 64-bit */
    int64_t  imm;             /* offset, or SIT_ADDPC value */
    MemOpIdx oi;              /* memory op and mmu index of the whole access */
} SitInsn;

/* Where the guest state the backend touches lives, as offsets from the env pointer. */
typedef struct SitLayout
{
    int xregs; /* uint64_t[32]: X0-X30, SP */
    int pc;    /* uint64_t */
    int cf;    /* uint32_t: C is 0/1 */
    int vf;    /* uint32_t: V is bit 31 */
    int nf;    /* uint32_t: N is bit 31 */
    int zf;    /* uint32_t: Z is set if zero */
    int vregs;   /* SIMD&FP register V0; low 128 bits little-endian */
    int vstride; /* bytes between V<n> and V<n+1> */
} SitLayout;

#endif
