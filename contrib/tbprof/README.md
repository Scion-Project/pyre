# tbprof (analysis only, not for merge)

Tooling behind docs/SAME-ISA-FASTPATH.md.

* `TBPROF=<file>` (this branch's QEMU): per-TB execution counters, written at exit.
* `-d out_asm -D <log>`: guest insn words (`G`), CPUARMState offsets (`OFFS`), raw host code words per guest-insn chunk (`W`).
* `tbprof.py LOG COUNTS [--minus LOG COUNTS] [--top N]`: executed host instructions by category (work, mem, tlb, st_gpr, st_flags, st_pc, st_vec, helper, glue) and per guest instruction class. Needs `pip install capstone`; run with `python3 -I`.
* `regfreq.py LOG COUNTS [--minus ...]`: dynamic guest GPR reference frequency, NZCV read/write share.
* `tbdis.py LOG TBIDX`: disassemble one TB from the log.
* `sampsum.py` / `sampself.py SAMPLE`: bucket a macOS `sample` profile (self time of the vCPU thread).
* `wl.c`, `mkcpio.py`: the Linux guest workloads (static, NDK clang -O2; `/init` runs `wl=name:reps,...` from the kernel command line, then powers off) and a newc initramfs writer (`mkcpio.py out.cpio wl cm` with CoreMark built as `cm`).
* `runlinux.sh QEMU WLSPEC`: boot the Debian arm64 kernel with that initramfs on `-M virt -cpu cortex-a72` (upstream QEMU; Inferno has no virt machine).
