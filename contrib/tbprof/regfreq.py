#!/usr/bin/env python3
"""regfreq.py LOG COUNTS [--minus LOG COUNTS]
Dynamic guest GPR reference frequency (each operand reference of x0..x30/sp/xzr,
weighted by executed count), and the share of guest instructions that read or
write NZCV. Answers: how much of the GPR traffic do the top-N registers cover?
"""
import collections, re, sys, importlib.util
import capstone
from capstone import arm64 as A
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True


def parse(logf, cntf):
    counts = {int(a): int(b) for a, b in (l.split() for l in open(cntf))}
    guest, tbs, cur = {}, [], None
    for l in open(logf, errors='replace'):
        if l.startswith('G '):
            _, pc, w = l.split(); guest[int(pc, 16)] = int(w, 16)
        elif l.startswith('OUT:'):
            m = re.search(r'TBIDX (\d+) PC ([0-9a-f]+) ICOUNT (\d+)', l)
            tbs.append((int(m.group(1)), int(m.group(2), 16), int(m.group(3))))
    return counts, guest, tbs


def key(tb, guest):
    return (tb[1], tb[2], tuple(guest.get(tb[1] + 4 * k) for k in range(tb[2])))


counts, guest, tbs = parse(sys.argv[1], sys.argv[2])
if '--minus' in sys.argv:
    k = sys.argv.index('--minus')
    bc, bg, btbs = parse(sys.argv[k + 1], sys.argv[k + 2])
    base = collections.Counter()
    for tb in btbs:
        base[key(tb, bg)] += bc.get(tb[0], 0)
    for tb in tbs:
        kk = key(tb, guest); c = counts.get(tb[0], 0); d = min(c, base[kk])
        counts[tb[0]] = c - d; base[kk] -= d

pcexec = collections.Counter()
for tb in tbs:
    c = counts.get(tb[0], 0)
    if c:
        for k in range(tb[2]):
            pcexec[tb[1] + 4 * k] += c
refs = collections.Counter(); flagr = flagw = total = 0
cache = {}
for pc, c in pcexec.items():
    w = guest.get(pc)
    if w is None:
        continue
    total += c
    if w not in cache:
        try:
            i = next(md.disasm(w.to_bytes(4, 'little'), 0))
            rr, ww = i.regs_access()
            names = [i.reg_name(r) for r in set(rr) | set(ww)]
            fr = 'nzcv' in [i.reg_name(r) for r in rr] or i.mnemonic.startswith('b.') or i.mnemonic in ('csel', 'csinc', 'csinv', 'csneg', 'cset', 'csetm', 'cinc', 'adc', 'adcs', 'sbc', 'sbcs', 'ccmp', 'ccmn')
            fw = 'nzcv' in [i.reg_name(r) for r in ww] or i.mnemonic in ('cmp', 'cmn', 'tst', 'adds', 'subs', 'ands', 'bics', 'ccmp', 'ccmn', 'adcs', 'sbcs', 'negs')
            regs = []
            for n in names:
                n = n.replace('w', 'x', 1) if re.fullmatch(r'w\d+', n) else n
                if n == 'wsp': n = 'sp'
                if re.fullmatch(r'x\d+', n) or n in ('sp', 'fp', 'lr'):
                    regs.append({'fp': 'x29', 'lr': 'x30'}.get(n, n))
            cache[w] = (regs, fr, fw)
        except StopIteration:
            cache[w] = ([], False, False)
    regs, fr, fw = cache[w]
    for r in regs:
        refs[r] += c
    flagr += c * fr; flagw += c * fw
tr = sum(refs.values())
print(f"guest insns {total:.4g}; GPR operand refs {tr:.4g} ({tr / total:.2f}/insn); NZCV read by {100 * flagr / total:.1f}% of insns, written by {100 * flagw / total:.1f}%")
cum = 0
row = []
for n, (r, v) in enumerate(refs.most_common(), 1):
    cum += v
    row.append(f"{r}:{100 * v / tr:.1f}%")
    if n in (4, 8, 9, 12, 16, 20, 28):
        print(f"  top {n:2d} regs cover {100 * cum / tr:5.1f}% of refs")
print("  by reg:", ' '.join(row))
