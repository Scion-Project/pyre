import sys, capstone
md=capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
idx=sys.argv[2]; on=False
for l in open(sys.argv[1], errors='replace'):
    if l.startswith('OUT:'): on = (' TBIDX %s ' % idx) in l
    if not on: continue
    if l.startswith('W '):
        w=int(l[2:10],16); d=list(md.disasm(w.to_bytes(4,'little'),0)); print('    ', (d[0].mnemonic+' '+d[0].op_str) if d else '.word %08x'%w)
    elif l.strip()=='' : on=False
    else: print(l.rstrip())
