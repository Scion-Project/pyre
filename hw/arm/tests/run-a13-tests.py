#!/usr/bin/env python3
#
# Run the bare-metal Apple A13 CPU-model tests on the firmware-free
# `a13-test` machine and fail closed.
#
#   hw/arm/tests/run-a13-tests.py build/qemu-system-aarch64
#
# Each test is assembled with clang for a bare ELF target (any clang with the
# AArch64 backend works, including Apple's), its .text is extracted into a
# raw image, and QEMU's exit status decides the result (see hw/arm/a13-test.c).
# A test passes only if QEMU exits 0 *and* prints the PASS line; a timeout,
# crash or any other output counts as failure. Negative controls must FAIL
# with the expected cause, which proves a missing register is really caught.
#
# Environment: CLANG (default `clang`), QEMU_TIMEOUT (seconds, default 30).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import os
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CLANG = os.environ.get("CLANG", "clang")
TIMEOUT = int(os.environ.get("QEMU_TIMEOUT", "30"))

FAIL_BASE = 0x46000000

# (name, source, extra assembler flags, expectation)
# expectation "pass", or ("undef", check_number): FAIL raised by an exception
# vector at that check with ESR.EC == 0 (unknown instruction / UNDEF).
TESTS = [
    ("gxf_sptm_regs", "gxf_sptm_regs_test.S", [], "pass"),
    ("gxf_sptm_regs (negative control)", "gxf_sptm_regs_test.S", ["-DNEGATIVE_CONTROL"], ("undef", 19)),
]


def extract_text(obj_path):
    """Return the .text bytes of an ELF64 little-endian relocatable, refusing
    objects with relocations (a raw image cannot carry them)."""
    with open(obj_path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise RuntimeError(f"{obj_path}: not an ELF64 little-endian object")
    e_shoff, = struct.unpack_from("<Q", data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name, stype, _flags, _addr, soff, size = struct.unpack_from("<IIQQQQ", data, off)
        sections.append((name, stype, soff, size))
    strtab = sections[e_shstrndx]
    names = data[strtab[2]:strtab[2] + strtab[3]]

    def sname(idx):
        return names[idx:names.index(b"\0", idx)].decode()

    text = None
    for name, stype, soff, size in sections:
        n = sname(name)
        if stype in (4, 9) and n.endswith(".text"):  # SHT_RELA / SHT_REL
            raise RuntimeError(f"{obj_path}: has relocations ({n}); tests must be position independent")
        if n == ".text":
            text = data[soff:soff + size]
    if not text:
        raise RuntimeError(f"{obj_path}: no .text section")
    return text


def build(src, flags, workdir, tag):
    obj = os.path.join(workdir, tag + ".o")
    img = os.path.join(workdir, tag + ".bin")
    subprocess.run([CLANG, "--target=aarch64-none-elf", "-c", "-x", "assembler-with-cpp", *flags,
                    os.path.join(HERE, src), "-o", obj], check=True)
    with open(img, "wb") as f:
        f.write(extract_text(obj))
    return img


def run(qemu, img):
    cmd = [qemu, "-M", "a13-test", "-nodefaults", "-display", "none", "-kernel", img]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=TIMEOUT)
    except subprocess.TimeoutExpired as e:
        return None, (e.output or b"").decode(errors="replace")
    return p.returncode, p.stdout.decode(errors="replace")


def judge(expect, code, out):
    if code is None:
        return False, f"timed out after {TIMEOUT}s (no result reported)"
    if expect == "pass":
        if code == 0 and "a13-test: PASS" in out:
            return True, "PASS"
        return False, f"exit {code}"
    _, check = expect
    for line in out.splitlines():
        m = re.search(r"a13-test: FAIL status=0x([0-9a-f]+) detail=0x([0-9a-f]+)", line)
        if not m:
            continue
        status = int(m.group(1), 16)
        detail = int(m.group(2), 16)
        cause = status & 0xff
        got_check = (status >> 8) & 0xffff
        ec = (detail >> 26) & 0x3f
        if code == 1 and status & ~0xffffff == FAIL_BASE and cause <= 0xf and got_check == check and ec == 0:
            return True, f"failed as expected (check {got_check}, vector {cause}, ESR 0x{detail:x})"
        return False, f"wrong failure: check {got_check}, cause 0x{cause:x}, ESR.EC 0x{ec:x}"
    return False, f"exit {code}, no FAIL line (a missing register went undetected)"


def main():
    if len(sys.argv) != 2:
        print(__doc__ or "usage: run-a13-tests.py <qemu-system-aarch64>", file=sys.stderr)
        return 2
    qemu = sys.argv[1]
    failures = 0
    with tempfile.TemporaryDirectory() as workdir:
        for i, (name, src, flags, expect) in enumerate(TESTS):
            img = build(src, flags, workdir, f"t{i}")
            code, out = run(qemu, img)
            ok, why = judge(expect, code, out)
            print(f"{'ok  ' if ok else 'FAIL'} {name}: {why}")
            if not ok:
                failures += 1
                print("  qemu output:\n    " + "\n    ".join(out.strip().splitlines() or ["(none)"]))
    print(f"{len(TESTS) - failures}/{len(TESTS)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
