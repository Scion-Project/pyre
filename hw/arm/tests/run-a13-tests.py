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

# Apple A14 core (t8101): a14_cpu_test.S on the Firestorm and Icestorm cores,
# and the A13 core must not have the A14 MIDR (check 2 mismatches).
A14_TESTS = [
    ("a14_cpu (firestorm)", "a14_cpu_test.S", [], "pass", {"machine": "a13-test,core=firestorm"}),
    ("a14_cpu (icestorm)", "a14_cpu_test.S", ["-DICESTORM"], "pass", {"machine": "a13-test,core=icestorm"}),
    ("a14_cpu (negative control)", "a14_cpu_test.S", ["-DNEGATIVE_CONTROL"], ("undef", 28),
     {"machine": "a13-test,core=firestorm"}),
    ("a14_cpu (negative control: A13 core)", "a14_cpu_test.S", [], ("mismatch", 2), {}),
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
# ASID TLB contexts (-accel tcg,asid-tlbs=N, tlb_switch_asid_ctx()): the
# same checks with the default 8 slots, with 1 slot (every switch evicts),
# with the kept-aside jump caches (x-asid-jc=on; off by default) and with the
# contexts off. Its last check probes that kept-aside TLBs really come back;
# the negative control runs the default build with asid-tlbs=0, where that
# probe must FAIL, so a run that silently fell back to flushing
# cannot pass the default case.
ASID_TESTS = [
    ("asid_tlb", "asid_tlb_test.S", [], "pass", {}),
    ("asid_tlb (asid-tlbs=1)", "asid_tlb_test.S", [], "pass", {"accel": "asid-tlbs=1"}),
    ("asid_tlb (x-asid-jc=on)", "asid_tlb_test.S", [], "pass", {"accel": "x-asid-jc=on"}),
    ("asid_tlb (asid-tlbs=0)", "asid_tlb_test.S", ["-DNO_ASID_TLBS"], "pass", {"accel": "asid-tlbs=0"}),
    ("asid_tlb (negative control: asid-tlbs=0)", "asid_tlb_test.S", [], ("mismatch", 73), {"accel": "asid-tlbs=0"}),
]

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


# Native integer execution (-accel tcg,native-int=on, docs/NATIVE-INT.md).
# native_int_check.S: directed EL0 tests (precise state at faults, SP/x18/
# x29/x30, flags across slow paths, a store over the running code) that must
# pass with native-int off, on, and on with loads/stores left to TCG; the
# negative control skips the slow paths' register write-back
# (x-native-int-fault) and must be caught at check 1. Then random EL0
# programs (native_int_gen.py, native_int_rand.S) whose hash must be the
# same with native-int off and on, and on AArch64 hosts the same as the host
# CPU running the program natively.
NI_MARCH = ["-march=armv8.4-a"]
NI_ON_MSG = "native-int: on" if IS_AARCH64_HOST else "native-int: not available"
NATIVE_INT_TESTS = [
    ("native_int_check, native-int=off", "native_int_check.S", NI_MARCH, "pass",
     {"accel": "native-int=off", "mem": "128M"}),
    ("native_int_check, native-int=on", "native_int_check.S", NI_MARCH, "pass",
     {"accel": "native-int=on", "mem": "128M", "need": NI_ON_MSG}),
    ("native_int_check, native-int=on, x-native-int-ldst=off", "native_int_check.S", NI_MARCH, "pass",
     {"accel": "native-int=on,x-native-int-ldst=off", "mem": "128M", "need": NI_ON_MSG}),
    ("native_int_check (negative control: slow paths skip the register write-back)", "native_int_check.S",
     NI_MARCH, ("mismatch", 1), {"accel": "native-int=on,x-native-int-fault=on", "mem": "128M",
                                  "aarch64_only": True}),
]
NI_SEEDS = int(os.environ.get("NATIVE_INT_SEEDS", "8"))
NI_INSNS = 3000


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


def run(qemu, img, accel=None, machine="a13-test", mem=None, extra=()):
    cmd = [qemu, "-M", machine, "-display", "none", "-monitor", "none", "-serial", "none", "-kernel", img]
    if mem:
        cmd += ["-m", mem]
    if accel:
        cmd += ["-accel", "tcg," + accel]
    cmd += list(extra)
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
    tests = [t + ({},) for t in TESTS] + A14_TESTS + ASID_TESTS + FP_TESTS + NATIVE_INT_TESTS
    total = skipped = 0
    with tempfile.TemporaryDirectory() as workdir:
        for i, (name, src, flags, expect, opts) in enumerate(tests):
            if opts.get("aarch64_only") and not IS_AARCH64_HOST:
                print(f"skip {name}: host is not AArch64")
                skipped += 1
                continue
            total += 1
            img = build(src, flags, workdir, f"t{i}")
            code, out = run(qemu, img, opts.get("accel"), opts.get("machine", "a13-test"), opts.get("mem"))
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
        for seed in range(1, NI_SEEDS + 1):
            total += 1
            ok, why = native_int_random(qemu, seed, workdir)
            print(f"{'ok  ' if ok else 'FAIL'} native_int_rand seed {seed} ({NI_INSNS} insns): {why}")
            failures += 0 if ok else 1
    print(f"{total - failures}/{total} passed" + (f", {skipped} skipped" if skipped else ""))
    return 1 if failures else 0


def native_int_random(qemu, seed, workdir):
    """One random EL0 program: its hash with native-int off, on, and on with
    loads/stores on TCG, and (AArch64 hosts) natively on the host CPU, must
    all agree; for seed 1 the native-int run must really emit SIT regions."""
    body = os.path.join(workdir, f"nic_body{seed}.inc")
    with open(body, "w") as f:
        subprocess.run([sys.executable, os.path.join(HERE, "native_int_gen.py"), str(seed), str(NI_INSNS)],
                       stdout=f, check=True)
    flags = NI_MARCH + [f'-DBODY="{body}"']
    img = build("native_int_rand.S", flags, workdir, f"nic{seed}")
    hashes = {}
    for accel in ("native-int=off", "native-int=on", "native-int=on,x-native-int-ldst=off"):
        extra = ()
        if seed == 1 and accel == "native-int=on" and IS_AARCH64_HOST:
            log = os.path.join(workdir, "nic_op.log")
            extra = ("-d", "op", "-D", log)
        code, out = run(qemu, img, accel, extra=extra)
        m = re.search(r"nic-hash=([0-9a-f]{16})", out)
        if code != 0 or not m:
            return False, f"{accel}: exit {code}: " + " | ".join(out.strip().splitlines()[-3:])
        hashes[accel] = m.group(1)
        if extra:
            with open(log) as f:
                n = f.read().count("sit_enter")
            if not n:
                return False, "native-int=on emitted no SIT regions"
    if IS_AARCH64_HOST:
        cc = os.environ.get("HOST_CC", "cc")
        main_c = os.path.join(workdir, "nic_host_main.c")
        obj = os.path.join(workdir, f"nic_host{seed}.o")
        exe = os.path.join(workdir, f"nic_host{seed}")
        with open(main_c, "w") as f:
            f.write("#include <stdio.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n"
                    "uint64_t nic_run(void *buf);\n"
                    "int main(void) { void *b; if (posix_memalign(&b, 16384, 65536)) return 1; memset(b, 0, 65536);\n"
                    "  printf(\"%016llx\\n\", (unsigned long long)nic_run(b)); return 0; }\n")
        try:
            subprocess.run([cc, "-march=armv8.4-a", "-c", "-x", "assembler-with-cpp", "-DHOST_HARNESS", *flags[1:],
                            os.path.join(HERE, "native_int_rand.S"), "-o", obj], check=True)
            subprocess.run([cc, main_c, obj, "-o", exe], check=True)
            hashes["host CPU"] = subprocess.run([exe], stdout=subprocess.PIPE, check=True,
                                                timeout=TIMEOUT).stdout.decode().strip()
        except (subprocess.SubprocessError, OSError) as e:
            return False, f"could not run the host harness: {e}"
    if len(set(hashes.values())) != 1:
        return False, "hashes differ: " + ", ".join(f"{k} {v}" for k, v in hashes.items())
    return True, f"{len(hashes)} runs agree on {next(iter(hashes.values()))}"


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
