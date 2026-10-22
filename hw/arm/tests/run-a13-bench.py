#!/usr/bin/env python3
#
# TCG microbenchmarks on the firmware-free `a13-test` machine.
#
#   hw/arm/tests/run-a13-bench.py [--reps N] [--only w1,w3] NAME=QEMU [NAME=QEMU ...]
#
# Builds tcg_bench.S (one image per workload, see the file header), then
# runs every workload with every QEMU binary, interleaved, and prints the
# median wall time of the QEMU process and the change against the first
# binary. Each run must exit 0 with the a13-test PASS line. The numbers are
# proxies for the hot paths of an iOS boot (indirect-branch lookup,
# system-register access, TB invalidation); see ios-on-pixel docs/PERF.md.
#
# Environment: CLANG (default `clang`).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from importlib import import_module  # noqa: E402

extract_text = import_module("run-a13-tests").extract_text
CLANG = os.environ.get("CLANG", "clang")

# name, defines, what it measures
WORKLOADS = [
    ("w1", ["-DWORKLOAD=1", "-DITERS=60000000"], "hot BL/RET (240M returns via lookup_and_goto_ptr)"),
    ("w2", ["-DWORKLOAD=2"], "BLR/RET over 4096 targets"),
    ("w2s", ["-DWORKLOAD=2", "-DNFUNC=16384", "-DFSTRIDE=144", "-DITERS=10000000"], "BLR/RET over 16384 targets"),
    ("w3", ["-DWORKLOAD=3"], "320M MRS TPIDR_EL1"),
    ("w4", ["-DWORKLOAD=4", "-DITERS=100000000"], "ALU loop (control)"),
    ("w5", ["-DWORKLOAD=5", "-DITERS=4000000"], "as w2 + 62.5k TB invalidations"),
    ("w6", ["-DWORKLOAD=6", "-DITERS=5000000"], "40M MSR TPIDR_EL1/ELR_EL1"),
    ("w7", ["-DWORKLOAD=7", "-DITERS=2000000"], "48M vector FMLA/FMUL/FADD (.4s/.2d)"),
    ("w8", ["-DWORKLOAD=8", "-DITERS=2000000"], "40M scalar FP (FMADD/FDIV/FSQRT/FCMP/FCVTZS/SCVTF)"),
    ("w9", ["-DWORKLOAD=9", "-DITERS=1000000"], "2M ASID switches, KPTI-style (16 pages, 4 calls each)"),
]


def build(name, defines, workdir):
    obj = os.path.join(workdir, name + ".o")
    img = os.path.join(workdir, name + ".bin")
    subprocess.run([CLANG, "--target=aarch64-none-elf", "-c", "-x", "assembler-with-cpp", *defines,
                    os.path.join(HERE, "tcg_bench.S"), "-o", obj], check=True)
    with open(img, "wb") as f:
        f.write(extract_text(obj))
    return img


def run(qemu, img):
    """QEMU may carry TCG options after '@', e.g. build/qemu-system-aarch64@native-fp=on."""
    qemu, _, accel = qemu.partition("@")
    cmd = [qemu, "-M", "a13-test", "-display", "none", "-monitor", "none", "-serial", "none", "-kernel", img]
    if accel:
        cmd += ["-accel", "tcg," + accel]
    t = time.monotonic()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=600)
    dt = time.monotonic() - t
    if p.returncode != 0 or b"a13-test: PASS" not in p.stdout:
        sys.exit(f"{qemu} {os.path.basename(img)}: exit {p.returncode}\n{p.stdout.decode(errors='replace')}")
    return dt


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--only", help="comma-separated workload names")
    ap.add_argument("qemu", nargs="+", metavar="NAME=QEMU[@TCGOPTS]")
    a = ap.parse_args()
    bins = [q.split("=", 1) if "=" in q else (q, q) for q in a.qemu]
    loads = [w for w in WORKLOADS if not a.only or w[0] in a.only.split(",")]
    times = {}
    with tempfile.TemporaryDirectory() as workdir:
        imgs = {w: build(w, d, workdir) for w, d, _ in loads}
        for _ in range(a.reps):
            for w, _, _ in loads:
                for name, qemu in bins:
                    times.setdefault((w, name), []).append(run(qemu, imgs[w]))
    for w, _, what in loads:
        base = statistics.median(times[(w, bins[0][0])])
        for name, _ in bins:
            v = times[(w, name)]
            m = statistics.median(v)
            print(f"{w:4s} {name:12s} {m:7.2f} s  [{min(v):.2f}-{max(v):.2f}]  {(m / base - 1) * 100:+6.1f}%  {what}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
