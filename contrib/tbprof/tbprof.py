#!/usr/bin/env python3
"""
tbprof.py LOG COUNTS [--top N]

Classify the host code TCG generated for an AArch64 guest on an AArch64 host,
weighted by how often each TB executed.

LOG     -d out_asm log from the analysis/tbprof build (G/OUT/W lines)
COUNTS  per-TB execution counts written by TBPROF=<file>

For each TB the expected number of executed host instructions per category is
computed over the TB's host CFG: TLB-miss branches and the exit-request branch
are assumed not taken, slow paths are not executed, any other conditional
branch is taken with p=0.5, and an unconditional branch/BR/RET ends the path.
"""
import collections, re, sys
import capstone
from capstone import arm64 as A

md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True
gmd = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

logf, cntf = sys.argv[1], sys.argv[2]
TOP = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 15

def parse(logf, cntf):
    counts = {}
    for l in open(cntf):
        i, c = l.split()
        counts[int(i)] = int(c)
    guest = {}
    tbs = []
    cur = None
    for l in open(logf, errors='replace'):
        if l.startswith('G '):
            _, pc, w = l.split()
            guest[int(pc, 16)] = int(w, 16)
        elif l.startswith('W '):
            cur[3][-1][1].append(int(l[2:10], 16))
        elif l.startswith('OUT:'):
            m = re.search(r'TBIDX (\d+) PC ([0-9a-f]+) ICOUNT (\d+)', l)
            cur = [int(m.group(1)), int(m.group(2), 16), int(m.group(3)), []]
            tbs.append(cur)
        elif l.startswith('  -- guest addr'):
            if 'tb prologue' in l:
                cur[3].append(('pro', []))
            else:
                off = int(l.split()[3], 16)
                tbpc = cur[1]
                cur[3].append((tbpc + ((off - (tbpc & 0xfff)) & 0xfff), []))
        elif l.startswith('  -- tb slow paths'):
            cur[3].append(('slow', []))
        elif l.startswith('OFFS'):
            for kv in l.split()[1:]:
                k, v = kv.split('=')
                OFFS[k] = int(v)
    return counts, guest, tbs


def tbkey(tb, guest):
    return (tb[1], tb[2], tuple(guest.get(tb[1] + 4 * k) for k in range(tb[2])))


OFFS = {}
counts, guest, tbs = parse(logf, cntf)
if '--minus' in sys.argv:
    k = sys.argv.index('--minus')
    bc, bg, btbs = parse(sys.argv[k + 1], sys.argv[k + 2])
    base = collections.Counter()
    for tb in btbs:
        base[tbkey(tb, bg)] += bc.get(tb[0], 0)
    removed = 0
    for tb in tbs:
        key = tbkey(tb, guest)
        c = counts.get(tb[0], 0)
        d = min(c, base[key])
        if d:
            counts[tb[0]] = c - d
            base[key] -= d
            removed += d
    print(f"(subtracted {removed} baseline TB executions)")

X19, X16, X17, X30 = A.ARM64_REG_X19, A.ARM64_REG_X16, A.ARM64_REG_X17, A.ARM64_REG_X30
TMPS = {X16, X17, X30, A.ARM64_REG_W16, A.ARM64_REG_W17, A.ARM64_REG_W30}


def env_field(off):
    if off < 0:
        return 'neg'
    if OFFS['xregs'] <= off < OFFS['xregs'] + 256:
        return 'gpr'
    if off == OFFS['pc']:
        return 'pc'
    if off in (OFFS['CF'], OFFS['VF'], OFFS['NF'], OFFS['ZF']):
        return 'flags'
    if OFFS['zregs'] <= off < OFFS['zregs_end']:
        return 'vec'
    return 'other'


def mem_base_disp(ins):
    for op in ins.operands:
        if op.type == A.ARM64_OP_MEM:
            return op.mem.base, op.mem.index, op.mem.disp
    return None


def gclass(w):
    if w is None:
        return 'unknown'
    try:
        i = next(gmd.disasm(w.to_bytes(4, 'little'), 0))
    except StopIteration:
        return 'undef'
    m = i.mnemonic
    if m in ('svc', 'hvc', 'smc', 'eret', 'brk', 'hlt', 'wfi', 'wfe', 'yield', 'isb', 'dsb', 'dmb', 'sev', 'sevl', 'clrex') \
            or m in ('mrs', 'msr', 'sys', 'sysl', 'tlbi', 'dc', 'ic', 'at') or m.startswith('pac') or m.startswith('aut') \
            or m in ('xpaclri', 'hint', 'nop', 'bti'):
        return 'sys'
    if m in ('b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.') \
            or m in ('retaa', 'retab', 'braa', 'blraa'):
        return 'branch'
    ops = i.op_str
    if m.startswith(('ld', 'st', 'cas', 'swp', 'prfm')):
        if re.search(r'\b[vqdsbh]\d+\b', ops.split('[')[0]) or ' {v' in ' ' + ops:
            return 'ldst-simd'
        return 'ldst'
    if re.search(r'\bv\d+\.', ops) or re.match(r'^[sdhqb]\d+', ops) or m.startswith('f'):
        return 'simd-fp'
    return 'alu'


CATS = ['work', 'mem', 'tlb', 'st_gpr', 'st_pc', 'st_flags', 'st_vec', 'st_other', 'helper', 'glue', 'prof']


def analyse_tb(tb):
    """Return (expected executed host insns per category, per-guest-class breakdown)."""
    idx, pc, icount, chunks = tb
    flat = []   # (ins, cat, gpc, region)
    base = 0
    for gpc, words in chunks:
        code = b''.join(w.to_bytes(4, 'little') for w in words)
        dis = list(md.disasm(code, base))
        if len(dis) != len(words):  # undecodable word: fall back word by word
            dis = []
            for k, w in enumerate(words):
                d = list(md.disasm(w.to_bytes(4, 'little'), base + 4 * k))
                dis.append(d[0] if d else None)
        for ins in dis:
            flat.append([ins, None, gpc])
        base += 4 * len(words)
    n = len(flat)
    # profiling counter in the prologue: ldr xA,[xB] ; add xA,xA,#1 ; str xA,[xB] and xB's materialisation
    for i in range(n):
        ins = flat[i][0]
        if ins is None or flat[i][2] != 'pro' or ins.mnemonic != 'str':
            continue
        mb = mem_base_disp(ins)
        if not mb or mb[0] == X19:
            continue
        b = mb[0]
        flat[i][1] = 'prof'
        for k in range(i - 1, -1, -1):
            pk = flat[k][0]
            if pk is None or flat[k][2] != 'pro':
                break
            if (pk.mnemonic in ('ldr', 'add') and pk.operands and pk.operands[0].reg == ins.operands[0].reg) or \
                    (pk.operands and pk.operands[0].reg == b and pk.mnemonic in ('mov', 'movz', 'movk', 'adr', 'adrp', 'ldr', 'add')):
                flat[k][1] = 'prof'
        break
    # the first chunk is "TB prologue + first guest insn": split at the exit-request branch
    cut = None
    for i in range(n):
        ins = flat[i][0]
        if flat[i][2] != 'pro':
            break
        if ins is not None and (ins.mnemonic in ('cbnz', 'cbz', 'tbnz', 'tbz') or ins.mnemonic.startswith('b.')):
            cut = i
            break
    for i in range(n):
        if flat[i][2] == 'pro' and (cut is None or i > cut) and flat[i][1] != 'prof':
            flat[i][2] = pc
    # --- categorise ---------------------------------------------------------
    i = 0
    first_cond_in_pro = None
    tlb_bne = set()
    while i < n:
        ins, cat, gpc = flat[i]
        if ins is None:
            flat[i][1] = 'work'; i += 1; continue
        if cat == 'prof':
            i += 1; continue
        m = ins.mnemonic
        if gpc == 'slow':
            flat[i][1] = 'tlb'; i += 1; continue
        # TLB fast path: sub x16, x19, #ofs ; ldp x16, x17, [x16] ; ... ; b.ne ; access
        mb0 = mem_base_disp(ins)
        if (m == 'sub' and len(ins.operands) >= 2 and ins.operands[0].reg == X16 and ins.operands[1].reg == X19) or \
                (m == 'ldp' and ins.operands[0].reg == X16 and ins.operands[1].reg == X17 and mb0 and mb0[0] == X19):
            j = i
            while j < n and flat[j][0] is not None and not flat[j][0].mnemonic.startswith('b.'):
                flat[j][1] = 'tlb'; j += 1
            if j < n:
                flat[j][1] = 'tlb'; tlb_bne.add(j); j += 1
            if j < n:
                flat[j][1] = 'mem'
                # 128-bit / pair accesses may use several insns; mark following ones touching x17 base
                k = j + 1
                while k < n and flat[k][0] is not None and (mem_base_disp(flat[k][0]) or (0,))[0] == X17:
                    flat[k][1] = 'mem'; k += 1
                i = k
            else:
                i = j
            continue
        mb = mem_base_disp(ins)
        if mb and mb[0] == X19:
            f = env_field(mb[2])
            flat[i][1] = {'gpr': 'st_gpr', 'pc': 'st_pc', 'flags': 'st_flags', 'vec': 'st_vec',
                          'other': 'st_other', 'neg': 'glue'}[f]
            i += 1; continue
        if m in ('bl', 'blr'):
            flat[i][1] = 'helper'
            # address materialisation into x16 and env argument just before the call
            k = i - 1
            while k >= 0 and flat[k][1] is None and flat[k][0] is not None and (
                    (flat[k][0].mnemonic in ('mov', 'movz', 'movk', 'adr', 'adrp', 'ldr')
                     and flat[k][0].operands and flat[k][0].operands[0].reg == X16)
                    or (flat[k][0].mnemonic == 'mov' and len(flat[k][0].operands) == 2
                        and flat[k][0].operands[1].reg == X19)):
                flat[k][1] = 'helper'; k -= 1
            i += 1; continue
        if cat == 'prof':
            i += 1; continue
        if gpc == 'pro':
            flat[i][1] = 'glue'
            if first_cond_in_pro is None and (m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz')):
                first_cond_in_pro = i
            i += 1; continue
        flat[i][1] = None
        i += 1
    # remaining uncategorised: TB-end control (unconditional b/br after the last guest insn) = glue; else work
    for i in range(n):
        if flat[i][1] is None:
            ins = flat[i][0]
            if ins.mnemonic in ('b', 'br', 'ret') or (ins.mnemonic in ('mov', 'movz', 'movk') and ins.operands
                                                       and ins.operands[0].reg in (A.ARM64_REG_X0, A.ARM64_REG_W0)
                                                       and i + 1 < n and flat[i + 1][0] is not None
                                                       and flat[i + 1][0].mnemonic == 'b'):
                flat[i][1] = 'glue'
            else:
                flat[i][1] = 'work'
    # --- expected executed count over the host CFG --------------------------
    addr2i = {flat[i][0].address: i for i in range(n) if flat[i][0] is not None}
    memo = {}
    sys.setrecursionlimit(20000)

    def E(i):
        if i >= n:
            return {}
        if i in memo:
            return memo[i]
        ins, cat, gpc = flat[i]
        here = {(cat, gpc): 1.0}
        res = None
        if ins is not None:
            m = ins.mnemonic
            tgt = None
            if ins.operands and ins.operands[-1].type == A.ARM64_OP_IMM and (
                    m == 'b' or m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz')):
                tgt = addr2i.get(ins.operands[-1].imm)
            if m in ('b',):
                # unpatched goto_tb is 'b .' (and nothing branches backwards): leaving the TB
                res = E(tgt) if tgt is not None and tgt > i else {}
            elif m in ('br', 'ret'):
                res = {}
            elif m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
                if i in tlb_bne or i == first_cond_in_pro or tgt is None or tgt <= i:
                    res = E(i + 1)
                else:
                    a, b = E(i + 1), E(tgt)
                    res = {k: 0.5 * a.get(k, 0) + 0.5 * b.get(k, 0) for k in set(a) | set(b)}
        if res is None:
            res = E(i + 1)
        out = dict(res)
        for k, v in here.items():
            out[k] = out.get(k, 0) + v
        memo[i] = out
        return out

    # evaluate from the end backwards to keep recursion shallow
    for i in range(n - 1, -1, -1):
        E(i)
    return E(0)


tot = collections.Counter()
by_gclass = collections.defaultdict(collections.Counter)
gcount = collections.Counter()
guest_total = 0
per_tb = []
seen_tb = 0
for tb in tbs:
    c = counts.get(tb[0], 0)
    if not c:
        continue
    seen_tb += 1
    e = analyse_tb(tb)
    guest_total += c * tb[2]
    tbh = 0
    for (cat, gpc), v in e.items():
        tot[cat] += c * v
        if cat != 'prof':
            tbh += c * v
        if isinstance(gpc, int):
            by_gclass[gclass(guest.get(gpc))][cat] += c * v
    # guest instruction classes, executed counts
    for k in range(tb[2]):
        gcount[gclass(guest.get(tb[1] + 4 * k))] += c
    per_tb.append((tbh, tb[1], tb[2], c))

host_total = sum(v for k, v in tot.items() if k != 'prof')
print(f"TBs executed: {seen_tb} of {len(tbs)} translated; guest insns executed {guest_total:.4g}; "
      f"host insns executed (model) {host_total:.4g}; expansion {host_total / guest_total:.2f} host/guest")
print(f"{'category':10s} {'host insns':>12s} {'share':>7s} {'per guest insn':>15s}")
for k in CATS:
    if k == 'prof':
        continue
    print(f"{k:10s} {tot[k]:12.4g} {100 * tot[k] / host_total:6.1f}% {tot[k] / guest_total:15.3f}")
print("\nguest insn classes (executed share; host insns per guest insn of that class, by category):")
gsum = sum(gcount.values())
for gc, cnt in gcount.most_common():
    row = by_gclass[gc]
    h = sum(v for k, v in row.items() if k != 'prof')
    parts = ' '.join(f"{k}={row[k] / cnt:.2f}" for k in CATS if k != 'prof' and row[k] / cnt >= 0.05)
    print(f"  {gc:10s} {100 * cnt / gsum:5.1f}%  host/guest {h / cnt:5.2f}  [{parts}]")
print(f"\nTop {TOP} TBs by host insns executed:")
per_tb.sort(reverse=True)
for h, pc, ic, c in per_tb[:TOP]:
    print(f"  pc {pc:016x} icount {ic:3d} execs {c:10d} host/exec {h / c:6.1f} share {100 * h / host_total:5.1f}%")
