"""Self-time breakdown of the busiest vCPU thread from a macOS `sample` file (JIT code = '???')."""
import re, sys, collections, importlib.util
spec = importlib.util.spec_from_file_location('ss', __file__.replace('sampself.py', 'sampsum.py'))
RULES = None
src = open(__file__.replace('sampself.py', 'sampsum.py')).read()
RULES = eval(src[src.index('RULES = ') + 8: src.index(']\ntot') + 1])
threads = []; cur = None
for l in open(sys.argv[1], errors='replace'):
    m = re.match(r'^    (\d+) Thread_', l)
    if m:
        cur = []; threads.append(cur); continue
    if l.startswith('Total number in stack') or l.startswith('Sort by top'):
        cur = None
    if cur is not None:
        m = re.match(r'^(\s*[+!:| ]*?)(\d+) (\S.*)$', l)
        if m: cur.append((len(m.group(1)), int(m.group(2)), m.group(3)))
def busy(t):
    return sum(c for d, c, n in t if 'cpu_tb_exec' in n)
t = max(threads, key=busy)
selfc = collections.Counter(); total = 0
for i, (d, c, n) in enumerate(t):
    ch = 0
    for d2, c2, n2 in t[i + 1:]:
        if d2 <= d: break
        if all(True for _ in [0]):
            pass
    # children = following lines with depth greater than d up to the first line with depth <= d, but only direct children
    kids = []; j = i + 1
    while j < len(t) and t[j][0] > d:
        if not kids or t[j][0] <= kids[0][0]:
            kids.append(t[j])
        j += 1
    s = c - sum(k[1] for k in kids if k[0] == (kids[0][0] if kids else 0))
    name = n.split('  (in')[0].strip()
    selfc[name] += s
tot = collections.Counter(); unk = collections.Counter()
for name, s in selfc.items():
    if s <= 0: continue
    if re.match(r'^(__psynch|__ulock|__sigwait|mach_msg|__select|poll|kevent)', name): cat = 'wait'
    else:
        for cat, rx in RULES:
            if re.search(rx, name): break
        else:
            cat = 'other'; unk[name] += s
    tot[cat] += s
S = sum(v for k, v in tot.items() if k != 'wait')
print(f"vCPU busy samples {S}: " + "  ".join(f"{k} {100*v/S:.1f}%" for k, v in tot.most_common() if k != 'wait'))
print("   other:", "; ".join(f"{k} {v}" for k, v in unk.most_common(6)))
