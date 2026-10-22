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
import platform
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
# vector at that check with ESR.EC == 0 (unknown instruction / UNDEF), or
# ("mismatch", check_number): FAIL with a value mismatch (cause 0xff) there.
TESTS = [
    ("gxf_sptm_regs", "gxf_sptm_regs_test.S", [], "pass"),
    ("gxf_sptm_regs (negative control)", "gxf_sptm_regs_test.S", ["-DNEGATIVE_CONTROL"], ("undef", 19)),
    ("gxf_banked_el1", "gxf_banked_el1_test.S", [], "pass"),
    ("gxf_banked_el1 (negative control)", "gxf_banked_el1_test.S", ["-DNEGATIVE_CONTROL"], ("mismatch", 31)),
]

# Guest FP on the host FPU (-accel tcg,native-fp=on, docs/NATIVE-FP.md).
# fp_native_check.S embeds the softfloat hash; it must come out the same
# with native-fp off and on. The host self-test must catch an injected
# native fault (the run then falls back to softfloat and still PASSes), and
# with the self-test skipped the injected fault must change the hash (FAIL at
# check 1), both with inline host FP and with native FP in helpers only:
# together they show both guards detect a wrong native result.
# Where the host is not AArch64 native-fp is unavailable and the fault
# controls are skipped. A further check (AArch64 hosts) runs the same code
# natively and compares with the embedded hash.
IS_AARCH64_HOST = platform.machine().lower() in ("arm64", "aarch64")
NATIVE_ON_MSG = "native-fp: on (host self-test passed" if IS_AARCH64_HOST else "native-fp: not available"
FP_TESTS = []
for _variant, _flags in (("", []), (", sticky FPSR", ["-DSTICKY"])):
    FP_TESTS += [
        (f"fp_native_check{_variant}, native-fp=off", "fp_native_check.S", _flags, "pass",
         {"accel": "native-fp=off"}),
        (f"fp_native_check{_variant}, default (native-fp on where available)", "fp_native_check.S", _flags,
         "pass", {}),
        (f"fp_native_check{_variant}, native-fp=on", "fp_native_check.S", _flags, "pass",
         {"accel": "native-fp=on", "need": NATIVE_ON_MSG}),
        (f"fp_native_check{_variant}, native-fp=on, helpers only (x-native-fp-inline=off)", "fp_native_check.S",
         _flags, "pass", {"accel": "native-fp=on,x-native-fp-inline=off", "need": NATIVE_ON_MSG}),
        (f"fp_native_check{_variant} (negative control: injected fault, self-test catches it)",
         "fp_native_check.S", _flags, "pass",
         {"accel": "native-fp=on,x-native-fp-fault=on", "need": "native-fp: host self-test failed",
          "aarch64_only": True}),
        (f"fp_native_check{_variant} (negative control: injected fault, self-test skipped)",
         "fp_native_check.S", _flags, ("mismatch", 1),
         {"accel": "native-fp=on,x-native-fp-fault=on,x-native-fp-selftest=off", "aarch64_only": True}),
        (f"fp_native_check{_variant} (negative control: injected fault, self-test skipped, helpers only)",
         "fp_native_check.S", _flags, ("mismatch", 1),
         {"accel": "native-fp=on,x-native-fp-fault=on,x-native-fp-selftest=off,x-native-fp-inline=off",
          "aarch64_only": True}),
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


def run(qemu, img, accel=None):
    cmd = [qemu, "-M", "a13-test", "-display", "none", "-monitor", "none", "-serial", "none", "-kernel", img]
    if accel:
        cmd += ["-accel", "tcg," + accel]
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
    kind, check = expect
    for line in out.splitlines():
        m = re.search(r"a13-test: FAIL status=0x([0-9a-f]+) detail=0x([0-9a-f]+)", line)
        if not m:
            continue
        status = int(m.group(1), 16)
        detail = int(m.group(2), 16)
        cause = status & 0xff
        got_check = (status >> 8) & 0xffff
        ec = (detail >> 26) & 0x3f
        if kind == "mismatch":
            if code == 1 and status & ~0xffffff == FAIL_BASE and cause == 0xff and got_check == check:
                return True, f"failed as expected (check {got_check}, value mismatch, read 0x{detail:x})"
            return False, f"wrong failure: check {got_check}, cause 0x{cause:x}"
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
    tests = [t + ({},) for t in TESTS] + FP_TESTS
    total = skipped = 0
    with tempfile.TemporaryDirectory() as workdir:
        for i, (name, src, flags, expect, opts) in enumerate(tests):
            if opts.get("aarch64_only") and not IS_AARCH64_HOST:
                print(f"skip {name}: host is not AArch64")
                skipped += 1
                continue
            total += 1
            img = build(src, flags, workdir, f"t{i}")
            code, out = run(qemu, img, opts.get("accel"))
            ok, why = judge(expect, code, out)
            if ok and opts.get("need") and opts["need"] not in out:
                ok, why = False, f"{why}, but QEMU did not report '{opts['need']}'"
            print(f"{'ok  ' if ok else 'FAIL'} {name}: {why}")
            if not ok:
                failures += 1
                print("  qemu output:\n    " + "\n    ".join(out.strip().splitlines() or ["(none)"]))
        if IS_AARCH64_HOST:
            for variant, flags in (("", []), (", sticky FPSR", ["-DSTICKY"])):
                total += 1
                ok, why = host_reference(flags, workdir)
                print(f"{'ok  ' if ok else 'FAIL'} fp_native_check{variant} on the host CPU (no QEMU): {why}")
                failures += 0 if ok else 1
        else:
            print("skip fp_native_check on the host CPU: host is not AArch64")
            skipped += 1
    print(f"{total - failures}/{total} passed" + (f", {skipped} skipped" if skipped else ""))
    return 1 if failures else 0


def host_reference(flags, workdir):
    """Run fp_native_check's instruction sequence natively (HOST_HARNESS) and
    compare its hash with the one embedded for QEMU: a third reference
    besides softfloat and native-fp."""
    src = os.path.join(HERE, "fp_native_check.S")
    with open(src) as f:
        text = f.read()
    m = re.search(r"#ifdef STICKY.*?GOLDEN\s+(0x[0-9a-f]+).*?#else.*?GOLDEN\s+(0x[0-9a-f]+)", text, re.S)
    golden = int(m.group(1 if flags else 2), 16)
    cc = os.environ.get("HOST_CC", "cc")
    obj = os.path.join(workdir, "fpc_host.o")
    exe = os.path.join(workdir, "fpc_host")
    main_c = os.path.join(workdir, "fpc_host_main.c")
    with open(main_c, "w") as f:
        f.write("#include <stdio.h>\n#include <stdint.h>\nuint64_t fpc_run(void);\n"
                "int main(void) { printf(\"%016llx\\n\", (unsigned long long)fpc_run()); return 0; }\n")
    try:
        subprocess.run([cc, "-c", "-x", "assembler-with-cpp", "-DHOST_HARNESS", *flags, src, "-o", obj], check=True)
        subprocess.run([cc, main_c, obj, "-o", exe], check=True)
        got = int(subprocess.run([exe], stdout=subprocess.PIPE, check=True, timeout=TIMEOUT).stdout, 16)
    except (subprocess.SubprocessError, ValueError, OSError) as e:
        return False, f"could not run the host harness: {e}"
    if got != golden:
        return False, f"host CPU hash {got:016x} != embedded softfloat hash {golden:016x}"
    return True, f"host CPU hash {got:016x} matches"


if __name__ == "__main__":
    sys.exit(main())
