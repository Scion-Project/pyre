#!/usr/bin/env python3
#
# Random A64 integer programs for testing native-int (same-ISA translation,
# docs/NATIVE-INT.md) against TCG and against the host CPU.
#
#   native_int_gen.py SEED [NINSN] > body.inc
#
# The output is the body of nic_body() for native_int_rand.S: a straight
# sequence of random data-processing instructions, loads and stores into a
# 64 KiB buffer, and forward conditional branches, interleaved with
# instructions that native-int leaves to TCG (MRS/MSR NZCV, FMOV, LDAR,
# LDADD, ...) so that its regions start and end everywhere. Registers are
# folded into a hash in x23. The program only depends on the buffer's
# contents, never on its address, so the same body gives the same hash
# under QEMU at EL0 and run natively on an AArch64 host.
#
# Register use: x0-x17, x19-x21 random; x22 scratch for the hash; x23 hash;
# x24 code anchor (ADR/ADRP results are made relative to it); x25 index
# scratch; x26-x28 buffer pointers (moved by pre/post-index forms within
# their windows); x18, x29, x30 and sp unused (platform register, frame).
#
# SPDX-License-Identifier: AGPL-3.0-or-later

import random
import sys

POOL = list(range(0, 18)) + [19, 20, 21]
# SIMD&FP registers the program may use: V8-V15 are callee-saved on the host (the harness).
VPOOL = list(range(0, 8)) + list(range(16, 32))
VSZ = {0: "b", 1: "h", 2: "s", 3: "d", 4: "q"}
CONDS = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"]
# buffer windows: base register, window start (offset in the 64 KiB buffer), initial offset in the window.
# x27 sits 64 bytes below a 16 KiB page boundary, so its accesses cross pages (the TLB slow path).
BASES = {26: (0x0000, 0x800), 27: (0x4000 - 0x1000, 0x1000 - 64), 28: (0x8000, 0x1000)}
WINDOW = 0x2000  # bytes each base may move around in


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.out = []
        self.label = 0
        self.off = {b: v[1] for b, v in BASES.items()}  # current offset of each base within its window

    def emit(self, s):
        self.out.append("    " + s)

    def reg(self):
        return self.r.choice(POOL)

    def x(self, n=None):
        return f"x{self.reg() if n is None else n}"

    def w(self, n=None):
        return f"w{self.reg() if n is None else n}"

    def rr(self, sf):
        return self.x() if sf else self.w()

    def imm64(self):
        r = self.r.random()
        if r < 0.2:
            return self.r.choice([0, 1, -1 & (2**64 - 1), 0x8000000000000000, 0x7fffffffffffffff, 0x80000000,
                                  0xffffffff, 0x7fffffff])
        if r < 0.4:
            return self.r.getrandbits(8)
        return self.r.getrandbits(64)

    def movimm(self, rd, v):
        self.emit(f"movz x{rd}, #{v & 0xffff}")
        for i in (1, 2, 3):
            h = (v >> (16 * i)) & 0xffff
            if h:
                self.emit(f"movk x{rd}, #{h}, lsl #{16 * i}")

    def bitmask(self, sf):
        esize = self.r.choice([2, 4, 8, 16, 32, 64] if sf else [2, 4, 8, 16, 32])
        ones = self.r.randint(1, esize - 1)
        rot = self.r.randint(0, esize - 1)
        e = (1 << ones) - 1
        e = ((e >> rot) | (e << (esize - rot))) & ((1 << esize) - 1)
        v = 0
        for i in range(0, 64 if sf else 32, esize):
            v |= e << i
        return v

    # --- data processing -------------------------------------------------
    def alu(self):
        r = self.r
        sf = r.random() < 0.7
        R = lambda: self.rr(sf)  # noqa: E731
        bits = 64 if sf else 32
        k = r.randrange(24)
        if k == 0:
            op = r.choice(["add", "adds", "sub", "subs"])
            sh = r.choice(["", ", lsl #12"])
            self.emit(f"{op} {R()}, {R()}, #{r.randrange(4096)}{sh}")
        elif k == 1:
            op = r.choice(["add", "adds", "sub", "subs"])
            self.emit(f"{op} {R()}, {R()}, {R()}, {r.choice(['lsl', 'lsr', 'asr'])} #{r.randrange(bits)}")
        elif k == 2:
            op = r.choice(["add", "adds", "sub", "subs"])
            ext = r.choice(["uxtb", "uxth", "uxtw", "sxtb", "sxth", "sxtw"] + (["uxtx", "sxtx"] if sf else []))
            m = self.x() if ext.endswith("x") else self.w()
            self.emit(f"{op} {R()}, {R()}, {m}, {ext} #{r.randrange(5)}")
        elif k == 3:
            op = r.choice(["and", "orr", "eor", "ands"])
            self.emit(f"{op} {R()}, {R()}, #{self.bitmask(sf):#x}")
        elif k == 4:
            op = r.choice(["and", "bic", "orr", "orn", "eor", "eon", "ands", "bics"])
            self.emit(f"{op} {R()}, {R()}, {R()}, {r.choice(['lsl', 'lsr', 'asr', 'ror'])} #{r.randrange(bits)}")
        elif k == 5:
            op = r.choice(["movz", "movn", "movk"])
            self.emit(f"{op} {R()}, #{r.getrandbits(16)}, lsl #{16 * r.randrange(4 if sf else 2)}")
        elif k == 6:
            op = r.choice(["sbfm", "ubfm", "bfm"])
            self.emit(f"{op} {R()}, {R()}, #{r.randrange(bits)}, #{r.randrange(bits)}")
        elif k == 7:
            self.emit(f"extr {R()}, {R()}, {R()}, #{r.randrange(bits)}")
        elif k == 8:
            op = r.choice(["madd", "msub"])
            self.emit(f"{op} {R()}, {R()}, {R()}, {R()}")
        elif k == 9:
            op = r.choice(["smaddl", "smsubl", "umaddl", "umsubl"])
            self.emit(f"{op} {self.x()}, {self.w()}, {self.w()}, {self.x()}")
        elif k == 10:
            self.emit(f"{r.choice(['smulh', 'umulh'])} {self.x()}, {self.x()}, {self.x()}")
        elif k == 11:
            self.emit(f"{r.choice(['udiv', 'sdiv'])} {R()}, {R()}, {R()}")
        elif k == 12:
            self.emit(f"{r.choice(['lslv', 'lsrv', 'asrv', 'rorv'])} {R()}, {R()}, {R()}")
        elif k == 13:
            ops = ["rbit", "rev16", "rev", "clz", "cls"] + (["rev32"] if sf else [])
            self.emit(f"{r.choice(ops)} {R()}, {R()}")
        elif k == 14:
            self.emit(f"{r.choice(['adc', 'adcs', 'sbc', 'sbcs'])} {R()}, {R()}, {R()}")
        elif k == 15:
            op = r.choice(["ccmp", "ccmn"])
            y = R() if r.random() < 0.5 else f"#{r.randrange(32)}"
            self.emit(f"{op} {R()}, {y}, #{r.randrange(16)}, {r.choice(CONDS)}")
        elif k == 16:
            op = r.choice(["csel", "csinc", "csinv", "csneg"])
            self.emit(f"{op} {R()}, {R()}, {R()}, {r.choice(CONDS)}")
        elif k == 17:
            op = r.choice(["cmp", "cmn", "tst"])
            if op == "tst":
                self.emit(f"tst {R()}, #{self.bitmask(sf):#x}")
            else:
                self.emit(f"{op} {R()}, {R()}")
        elif k == 18:
            # PC-relative, made position independent
            rd = self.reg()
            if r.random() < 0.5:
                self.emit(f"adr x{rd}, . + {r.randrange(-4096, 4096) & ~3}")
                self.emit(f"sub x{rd}, x{rd}, x24")
            else:
                # ADRP as raw words (an assembled ADRP needs a relocation): Xd = page(pc) + d pages,
                # minus page(pc) of the next insn. nic_body is page aligned in every build.
                d = r.randrange(-64, 64) & 0x1fffff
                self.emit(f".inst {0x90000000 | rd | (d & 3) << 29 | (d >> 2) << 5:#010x}")
                self.emit(f".inst {0x90000000 | 22:#010x}")
                self.emit(f"sub x{rd}, x{rd}, x22")
        elif k == 19:
            self.emit(f"mov {R()}, {R()}")
        elif k == 20:
            self.emit(f"{r.choice(['neg', 'negs', 'mvn'])} {R()}, {R()}")
        elif k == 21:
            self.emit(f"{r.choice(['sxtb', 'sxth', 'uxtb', 'uxth'])} {self.w()}, {self.w()}")
        elif k == 22:
            self.emit(f"{r.choice(['cset', 'csetm'])} {R()}, {r.choice(CONDS[:-1])}")
        else:
            self.emit(f"{r.choice(['lsl', 'lsr', 'asr', 'ror'])} {R()}, {R()}, #{r.randrange(bits)}")

    # --- loads and stores ----------------------------------------------------
    def window(self, b):
        start = BASES[b][0]
        return start, start + WINDOW

    def vldst(self, wb=True):
        """SIMD&FP loads and stores (and literal loads), like ldst()."""
        r = self.r
        b = r.choice(list(BASES))
        cur = self.off[b]
        k = r.randrange(8)
        v = lambda: r.choice(VPOOL)  # noqa: E731
        if k <= 2:
            size = r.randrange(5)
            op = r.choice(["ldr", "str"])
            mode = r.choice(["uoff", "unscaled", "pre", "post"] if wb else ["uoff", "unscaled"])
            reg = f"{VSZ[size]}{v()}"
            if mode == "uoff":
                off = r.randrange(0, 128) << size
                if cur + off + (1 << size) > WINDOW:
                    off = 0
                self.emit(f"{op} {reg}, [x{b}, #{off}]")
            elif mode == "unscaled":
                off = r.randrange(-256, 256)
                if not (0 <= cur + off and cur + off + 16 <= WINDOW):
                    off = 0
                self.emit(f"{op.replace('ldr', 'ldur').replace('str', 'stur')} {reg}, [x{b}, #{off}]")
            else:
                imm = r.randrange(-256, 256)
                lo, hi = 64, WINDOW - 64
                if not (lo <= cur + imm <= hi):
                    imm = -imm if lo <= cur - imm <= hi and -imm != 256 else 0
                self.emit(f"{op} {reg}, [x{b}, #{imm}]!" if mode == "pre" else f"{op} {reg}, [x{b}], #{imm}")
                self.off[b] = cur + imm
        elif k <= 3:
            size = r.randrange(5)
            op = r.choice(["ldr", "str"])
            self.emit(f"and x25, {self.x()}, #0xf8")
            ext = r.choice(["lsl", "uxtw", "sxtw"])
            idx = "x25" if ext == "lsl" else "w25"
            sh = r.choice([0, size]) if size else 0
            if ext == "lsl" and not sh:
                self.emit(f"{op} {VSZ[size]}{v()}, [x{b}, {idx}]")
            else:
                self.emit(f"{op} {VSZ[size]}{v()}, [x{b}, {idx}, {ext} #{sh}]")
        elif k <= 5:
            size = r.randrange(2, 5)
            op = r.choice(["ldp", "stp"])
            a_, c_ = v(), v()
            while op == "ldp" and c_ == a_:
                c_ = v()
            scale = 1 << size
            mode = r.choice(["off", "pre", "post"] if wb else ["off"])
            imm = r.randrange(-64, 64) * scale
            lo, hi = 128, WINDOW - 128
            if not (lo <= cur + imm <= hi):
                imm = -imm if lo <= cur - imm <= hi and -imm != 64 * scale else 0
            regs = f"{VSZ[size]}{a_}, {VSZ[size]}{c_}"
            if mode == "off":
                self.emit(f"{op} {regs}, [x{b}, #{imm}]")
            elif mode == "pre":
                self.emit(f"{op} {regs}, [x{b}, #{imm}]!")
                self.off[b] = cur + imm
            else:
                self.emit(f"{op} {regs}, [x{b}], #{imm}")
                self.off[b] = cur + imm
        else:
            # literal loads from a pool jumped over
            self.label += 1
            n = self.label
            if r.random() < 0.5:
                self.emit(f"ldr {VSZ[r.randrange(2, 5)]}{v()}, 8{n}f")
            else:
                op, dst = r.choice([("ldr", self.x()), ("ldr", self.w()), ("ldrsw", self.x())])
                self.emit(f"{op} {dst}, 8{n}f")
            self.emit(f"b 9{n}f")
            self.out.append(f"8{n}:")
            self.emit(f".quad {r.getrandbits(64):#x}, {r.getrandbits(64):#x}")
            self.out.append(f"9{n}:")

    def ldst(self, wb=True):
        """wb=False: no pre/post-index forms (inside a branch shadow the tracked base offsets must not move)."""
        r = self.r
        if r.random() < 0.3:
            return self.vldst(wb)
        b = r.choice(list(BASES))
        cur = self.off[b]
        k = r.randrange(10)
        if k <= 3:
            # single register, immediate offsets
            kind = r.choice(["ldr", "str"])
            size = r.choice([0, 1, 2, 3])
            if kind == "ldr" and size < 3 and r.random() < 0.4:
                op = {0: "ldrsb", 1: "ldrsh", 2: "ldrsw"}[size]
                dst = self.x() if size == 2 or r.random() < 0.5 else self.w()
            else:
                op = kind + {0: "b", 1: "h", 2: "", 3: ""}[size]
                dst = self.x() if size == 3 else self.w()
            if kind == "str" and r.random() < 0.15:
                dst = "xzr" if size == 3 else "wzr"
            mode = r.choice(["uoff", "unscaled", "pre", "post"] if wb else ["uoff", "unscaled"])
            if mode == "uoff":
                off = r.randrange(0, 256) << size
                if cur + off + (1 << size) > WINDOW:
                    off = 0
                self.emit(f"{op} {dst}, [x{b}, #{off}]")
            elif mode == "unscaled":
                off = r.randrange(-256, 256)
                if not (0 <= cur + off and cur + off + 8 <= WINDOW):
                    off = 0
                u = {"ldr": "ldur", "str": "stur", "ldrb": "ldurb", "strb": "sturb", "ldrh": "ldurh",
                     "strh": "sturh", "ldrsb": "ldursb", "ldrsh": "ldursh", "ldrsw": "ldursw"}[op]
                self.emit(f"{u} {dst}, [x{b}, #{off}]")
            else:
                imm = r.randrange(-256, 256)
                lo, hi = 64, WINDOW - 64
                if not (lo <= cur + imm <= hi):
                    imm = -imm if lo <= cur - imm <= hi and -imm != 256 else 0
                self.emit(f"{op} {dst}, [x{b}, #{imm}]!" if mode == "pre" else f"{op} {dst}, [x{b}], #{imm}")
                self.off[b] = cur + imm
        elif k <= 5:
            # register offset
            kind = r.choice(["ldr", "str"])
            size = r.choice([0, 1, 2, 3])
            op = kind + {0: "b", 1: "h", 2: "", 3: ""}[size]
            if kind == "ldr" and size < 3 and r.random() < 0.3:
                op = {0: "ldrsb", 1: "ldrsh", 2: "ldrsw"}[size]
                dst = self.x()
            else:
                dst = self.x() if size == 3 else self.w()
            self.emit(f"and x25, {self.x()}, #0xf8")
            ext = r.choice(["lsl", "uxtw", "sxtw", "sxtx"])
            sh = r.choice([0, size]) if size else 0
            idx = "x25" if ext in ("lsl", "sxtx") else "w25"
            amt = f" #{sh}" if (sh or ext != "lsl") else ""
            if ext == "lsl" and not sh:
                self.emit(f"{op} {dst}, [x{b}, {idx}]")
            else:
                self.emit(f"{op} {dst}, [x{b}, {idx}, {ext}{amt}]")
        else:
            # pairs
            kind = r.choice(["ldp", "stp", "ldpsw"])
            sf = kind == "ldpsw" or r.random() < 0.6
            if kind == "ldpsw":
                sf, size = True, 2
            else:
                size = 3 if sf else 2
            a, c = self.reg(), self.reg()
            while kind != "stp" and c == a:
                c = self.reg()
            ra, rc = (f"x{a}", f"x{c}") if sf else (f"w{a}", f"w{c}")
            if kind == "stp" and r.random() < 0.15:
                ra = "xzr" if sf else "wzr"
            scale = 1 << size
            mode = r.choice(["off", "pre", "post"] if wb else ["off"])
            imm = r.randrange(-64, 64) * scale
            lo, hi = 128, WINDOW - 128
            if not (lo <= cur + imm <= hi):
                imm = -imm if lo <= cur - imm <= hi and -imm != 64 * scale else 0
            if mode == "off":
                self.emit(f"{kind} {ra}, {rc}, [x{b}, #{imm}]")
            elif mode == "pre":
                self.emit(f"{kind} {ra}, {rc}, [x{b}, #{imm}]!")
                self.off[b] = cur + imm
            else:
                self.emit(f"{kind} {ra}, {rc}, [x{b}], #{imm}")
                self.off[b] = cur + imm

    # --- instructions native-int leaves to TCG: region boundaries --------------
    def other(self):
        r = self.r
        k = r.randrange(7)
        if k == 0:
            self.emit("mrs x22, nzcv")
            self.emit("eor x23, x23, x22")
        elif k == 1:
            self.emit(f"msr nzcv, {self.x()}")
        elif k == 2:
            self.emit("nop")
        elif k == 3:
            self.emit(f"fmov d0, {self.x()}")
            self.emit(f"fmov {self.x()}, d0")
        else:
            # ordered and atomic accesses need an aligned address
            self.emit(f"and x25, x{r.choice(list(BASES))}, #~15")
            if k == 4:
                self.emit(f"ldar {self.x()}, [x25]")
            elif k == 5:
                self.emit(f"ldadd {self.x()}, {self.x()}, [x25]")
            else:
                self.emit(f"stlr {self.w()}, [x25]")

    def branch(self):
        r = self.r
        self.label += 1
        n = self.label
        k = r.randrange(3)
        if k == 0:
            self.emit(f"b.{r.choice(CONDS[:-1])} 9{n}f")
        elif k == 1:
            self.emit(f"{r.choice(['cbz', 'cbnz'])} {self.rr(r.random() < 0.5)}, 9{n}f")
        else:
            self.emit(f"{r.choice(['tbz', 'tbnz'])} {self.x()}, #{r.randrange(64)}, 9{n}f")
        for _ in range(r.randint(0, 3)):
            if r.random() < 0.7:
                self.alu()
            else:
                self.ldst(wb=False)
        self.out.append(f"9{n}:")

    def fold(self):
        for n in self.r.sample(POOL, 6):
            self.emit(f"eor x23, x23, x{n}")
            self.emit("ror x23, x23, #11")

    def program(self, n):
        r = self.r
        self.out.append("    adr x24, nic_body")
        self.movimm(23, r.getrandbits(64))
        for b in (26, 27, 28):
            self.movimm(25, BASES[b][0] + BASES[b][1])
            self.emit(f"add x{b}, x0, x25")
        for n_ in POOL:
            self.movimm(n_, self.imm64())
        for n_ in VPOOL:
            self.emit(f"fmov d{n_}, x{self.reg()}")
            self.emit(f"mov v{n_}.d[1], x{self.reg()}")
        self.emit("msr nzcv, x1")
        i = 0
        while i < n:
            p = r.random()
            if p < 0.55:
                self.alu()
            elif p < 0.85:
                self.ldst()
            elif p < 0.93:
                self.branch()
            else:
                self.other()
            i += 1
            if i % 12 == 0:
                self.fold()
        # final: every pool register, then the flags
        for n_ in POOL:
            self.emit(f"eor x23, x23, x{n_}")
            self.emit("ror x23, x23, #7")
        self.emit("mrs x22, nzcv")
        self.emit("eor x23, x23, x22")
        for n_ in VPOOL:
            self.emit(f"fmov x22, d{n_}")
            self.emit("eor x23, x23, x22")
            self.emit(f"mov x22, v{n_}.d[1]")
            self.emit("eor x23, x23, x22, ror #17")
        # buffer base back from x26's final offset, for nic_hash_buffer
        self.movimm(25, BASES[26][0] + self.off[26])
        self.emit("sub x0, x26, x25")
        return "\n".join(self.out) + "\n"


def main():
    seed = int(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
    sys.stdout.write(f"/* generated by native_int_gen.py {seed} {n} */\n")
    sys.stdout.write(Gen(seed).program(n))


if __name__ == "__main__":
    main()
