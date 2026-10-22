import sys, os
out = open(sys.argv[1], 'wb'); ino = [100]
def ent(name, mode, data=b'', rdev=(0, 0)):
    ino[0] += 1
    nb = name.encode() + b'\0'
    hdr = '070701' + ''.join('%08x' % v for v in [ino[0], mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(nb), 0])
    out.write(hdr.encode() + nb); out.write(b'\0' * ((4 - (110 + len(nb)) % 4) % 4))
    out.write(data); out.write(b'\0' * ((4 - len(data) % 4) % 4))
ent('dev', 0o40755); ent('dev/console', 0o20600, rdev=(5, 1)); ent('proc', 0o40755); ent('tmp', 0o41777)
ent('init', 0o100755, open(sys.argv[2], 'rb').read()); ent('cm', 0o100755, open(sys.argv[3], 'rb').read())
ent('TRAILER!!!', 0)
